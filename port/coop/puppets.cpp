#include "puppets.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>

#include "game_state.h"
#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_pad_host.h"

namespace kh2puppets
{
void requestPreload(uint32_t entryId);
namespace
{
void preload(uint32_t entryId, std::function<void()> requested);
constexpr size_t kMaxPuppets = 2;

struct Slot
{
    bool active = false;
    bool spawned = false;  // we created the object (and must remove it)
    bool pending = false;  // spawn job queued, object not known yet
    bool spawnPosted = false;
    uint32_t entry = 0;
    double loadAt = 0;     // spawn mode: when the model load was requested (the job ran)
    bool loadRequested = false;
    uint8_t playerId = 0;
    uint16_t motion = 0xFFFF; // last motion we started on it
    int sinceMotion = 0;      // updates since then
    uint32_t object = 0;
    char name[33] = {};
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    uint32_t gen = 0;            // which spawn its jobs belong to
    uint8_t world = 0, room = 0; // where it was asked for
};
uint32_t g_gen = 0;

std::mutex g_mutex;
Slot g_slots[kMaxPuppets];

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_any{false};
std::atomic<bool> g_anyOverride{false};
std::vector<Override> g_overrides[2]; // per channel, under g_mutex
PS2Runtime::RecompiledFunction g_buildMatrix = nullptr;
constexpr uint32_t kObjFactory = 0x0016F000; // OBJ* (entry id, const FVector* pos, float yaw in $f12)
constexpr uint32_t kMotionChange = 0x0017F540; // YS::MOTION::change(MOTION* = obj+0x140, int id, float blend, float start)
constexpr uint32_t kVtableLeave = 0x20; // obj->vtable[+0x20](obj): leave (what trap_obj_leave_force calls)
std::atomic<Mode> g_mode{Mode::Spawn};

bool ensurePrivateTable(uint8_t *rdram);

// Spawning, as a guest job (one call per frame): allocate a spawn record and
// an empty unit group from the room's heap (AREA::Alloc; what a room's spawn
// list would give YS::UNIT_DATA_OBJ::appear), call the object factory, and
// finish the object with its setup method (vtable +0x38). The factory
// stores the newest object where GetPlayer() reads (the player is simply
// created last when a room loads), so that pointer is put back.
//
// `wanted` is asked before each step that changes the game: a job queued in
// one room can run in the next (jobs wait while the player is away, as in a
// cutscene or a load), and a spawn nobody wants any more stops there (done
// gets 0) instead of making an object nobody follows.
void postSpawn(uint32_t entry, const float pos[3], float yaw, std::function<void(uint32_t obj)> done,
               std::function<bool(const uint8_t *)> wanted = {}, int32_t serial = -1)
{
    constexpr uint32_t kAreaAlloc = 0x001050D8;
    constexpr uint32_t kVtableSetup = 0x38;
    struct State
    {
        int step = 0;
        uint32_t unit = 0, group = 0, obj = 0, player = 0;
        float pos[3];
        float yaw;
    };
    auto st = std::make_shared<State>();
    std::memcpy(st->pos, pos, sizeof(st->pos));
    st->yaw = yaw;
    kh2guest::post([st, entry, done, wanted, serial](const kh2guest::Env &env, uint32_t result, bool) -> std::optional<kh2guest::Call> {
        if (st->step == 0)
            ensurePrivateTable(env.rdram); // the game looks the entry up by id
        if (st->step <= 2 && wanted && !wanted(env.rdram))
        {
            std::printf("[puppets] spawn of entry %04X dropped (not wanted any more)\n", entry);
            std::fflush(stdout);
            if (done)
                done(0);
            return std::nullopt;
        }
        switch (st->step++)
        {
        case 0:
            return kh2guest::Call{kAreaAlloc, {0x40u}, {}};
        case 1:
            st->unit = result;
            return kh2guest::Call{kAreaAlloc, {0x40u}, {}};
        case 2:
        {
            st->group = result;
            if (!st->unit || !st->group)
                break;
            std::memset(env.host(st->unit), 0, 0x40);
            std::memset(env.host(st->group), 0, 0x40);
            std::memcpy(env.host(st->unit), &entry, 4);
            std::memcpy(env.host(st->unit + 4), st->pos, 12);
            std::memcpy(env.host(st->unit + 0x14), &st->yaw, 4);
            if (serial >= 0)
            {
                // The spawn entry's serial (UNIT_DATA_OBJ +0x1E), by which
                // the room's objects are told apart (enemies.cpp).
                const int16_t ser = int16_t(serial);
                std::memcpy(env.host(st->unit + 0x1E), &ser, 2);
            }
            const float p[4] = {st->pos[0], st->pos[1], st->pos[2], 1.0f};
            std::memcpy(env.host(env.scratch), p, 16);
            std::memcpy(&st->player, env.host(kh2state::addr::PlayerPtr), 4);
            return kh2guest::Call{kObjFactory, {entry, env.scratch}, {st->yaw}};
        }
        case 3:
        {
            st->obj = result;
            // The factory made the new object GetPlayer()'s: give it back to
            // the player, if it was ours and the player is still there.
            uint32_t cur = 0;
            std::memcpy(&cur, env.host(kh2state::addr::PlayerPtr), 4);
            if (st->obj && cur == st->obj && st->player != st->obj)
            {
                bool live = false;
                for (const kh2state::ActorInfo &a : kh2state::actors(env.rdram))
                    live = live || a.address == st->player;
                if (live)
                    std::memcpy(env.host(kh2state::addr::PlayerPtr), &st->player, 4);
                else
                    std::fprintf(stderr, "[puppets] the player %08X went while spawning\n", st->player);
            }
            uint32_t vt = 0, setup = 0;
            if (st->obj)
                std::memcpy(&vt, env.host(st->obj), 4);
            if (vt >= 0x00100000u && vt < 0x02000000u)
                std::memcpy(&setup, env.host(vt + kVtableSetup), 4);
            if (setup)
                return kh2guest::Call{setup, {st->obj, st->group, st->unit}, {}};
            break;
        }
        default:
            break;
        }
        if (done)
            done(st->obj);
        return std::nullopt;
    });
}

void postLeave(uint32_t obj)
{
    kh2guest::post([obj](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        // Only while it is still in the object list: a room change may have
        // removed it since this was queued, and leaving twice frees it twice.
        bool live = false;
        for (const kh2state::ActorInfo &a : kh2state::actors(env.rdram))
            live = live || a.address == obj;
        if (!live)
            return std::nullopt;
        uint32_t vt = 0, fn = 0;
        std::memcpy(&vt, env.host(obj), 4);
        if (vt >= 0x00100000u && vt < 0x02000000u)
            std::memcpy(&fn, env.host(vt + kVtableLeave), 4);
        if (!fn)
            return std::nullopt;
        return kh2guest::Call{fn, {obj}, {}};
    });
}

// Puppets are NPC-class objects ("npc, no collision", objentry type 15) whose
// objentry record is dressed in the remote player's model and motion set
// names (the record's +0x08 and +0x28, 32 bytes each) before its files are
// requested and the object is made: the NPC class's code with the player's
// look and motions.
//
// The records are our own: the game looks entries up (Tz::JmInfo::
// GetCharaData, 0x0016E920 -> 0x001D3408) in up to three tables whose
// pointers are at 0x01D5BA10 (only the first, 00objentry.bin, is used), each
// "u32 3, u32 count, then sorted 0x60-byte records" searched by id. A third
// table at 0x01E4D000, in the gap the game never touches below the runtime's
// heap, holds two records copied from the townsfolk class's B_EX110_RTN
// (0x084C) with private ids, so no entry the game itself uses is changed.
constexpr uint32_t kPuppetEntries[kMaxPuppets] = {0x7F00, 0x7F01};
constexpr uint32_t kTemplateEntry = 0x084C; // B_EX110_RTN: type 15, "npc, no collision"
constexpr uint32_t kPrivateTable = 0x01E4D000u;
constexpr uint32_t kTableSlot = kh2state::addr::Obj0Ptr + 8; // the third table
constexpr uint32_t kEntrySize = 0x60, kEntryLook = 0x08, kEntryLookSize = 0x40;

uint32_t primaryRecord(const uint8_t *rdram, uint32_t id)
{
    uint32_t table, count;
    std::memcpy(&table, rdram + (kh2state::addr::Obj0Ptr & 0x01FFFFFFu), 4);
    if (!table || table >= 0x02000000u)
        return 0;
    std::memcpy(&count, rdram + ((table + 4) & 0x01FFFFFFu), 4);
    for (uint32_t i = 0; i < count && i < 4096; ++i)
    {
        const uint32_t rec = table + 8 + i * kEntrySize;
        uint32_t eid;
        std::memcpy(&eid, rdram + (rec & 0x01FFFFFFu), 4);
        if ((eid & 0xFFFF) == (id & 0xFFFF))
            return rec;
    }
    return 0;
}

// Our table in place and registered (any thread; idempotent). False until
// 00objentry.bin is loaded.
bool ensurePrivateTable(uint8_t *rdram)
{
    uint32_t slot;
    std::memcpy(&slot, rdram + kTableSlot, 4);
    if (slot && slot != kPrivateTable)
        return false; // someone else's (never seen)
    uint32_t head[2];
    std::memcpy(head, rdram + kPrivateTable, 8);
    if (slot == kPrivateTable && head[0] == 3 && head[1] == kMaxPuppets)
        return true;
    const uint32_t tmpl = primaryRecord(rdram, kTemplateEntry);
    if (!tmpl)
        return false;
    const uint32_t newHead[2] = {3, kMaxPuppets};
    std::memcpy(rdram + kPrivateTable, newHead, 8);
    for (uint32_t i = 0; i < kMaxPuppets; ++i)
    {
        uint8_t *rec = rdram + kPrivateTable + 8 + i * kEntrySize;
        std::memcpy(rec, rdram + (tmpl & 0x01FFFFFFu), kEntrySize);
        std::memcpy(rec, &kPuppetEntries[i], 4);
    }
    const uint32_t table = kPrivateTable;
    std::memcpy(rdram + kTableSlot, &table, 4);
    return true;
}

uint32_t entryRecord(const uint8_t *rdram, uint32_t id)
{
    for (uint32_t i = 0; i < kMaxPuppets; ++i)
        if ((id & 0xFFFF) == kPuppetEntries[i])
            return ensurePrivateTable(const_cast<uint8_t *>(rdram)) ? kPrivateTable + 8 + i * kEntrySize : 0;
    return primaryRecord(rdram, id);
}

struct Dress
{
    bool saved = false;
    uint8_t original[kEntryLookSize];
};
Dress g_dress[kMaxPuppets]; // EE thread only (guest jobs)

int dressSlot(uint32_t puppetEntry)
{
    for (size_t i = 0; i < kMaxPuppets; ++i)
        if (kPuppetEntries[i] == puppetEntry)
            return int(i);
    return -1;
}

// Guest job: give `puppetEntry` the look of `lookEntry` (0: its own again).
void postDress(uint32_t puppetEntry, uint32_t lookEntry)
{
    kh2guest::post([puppetEntry, lookEntry](const kh2guest::Env &env, uint32_t, bool) -> std::optional<kh2guest::Call> {
        const int slot = dressSlot(puppetEntry);
        const uint32_t rec = entryRecord(env.rdram, puppetEntry);
        if (slot < 0 || !rec)
            return std::nullopt;
        Dress &d = g_dress[slot];
        uint8_t *look = env.host(rec + kEntryLook);
        if (!lookEntry)
        {
            if (d.saved)
                std::memcpy(look, d.original, kEntryLookSize);
            d.saved = false;
            return std::nullopt;
        }
        const uint32_t from = entryRecord(env.rdram, lookEntry);
        if (!from)
        {
            std::printf("[puppets] no objentry %04X to dress %04X as\n", lookEntry, puppetEntry);
            return std::nullopt;
        }
        if (!d.saved)
        {
            std::memcpy(d.original, look, kEntryLookSize);
            d.saved = true;
        }
        std::memcpy(look, env.host(from + kEntryLook), kEntryLookSize);
        std::printf("[puppets] entry %04X dressed as %.32s\n", puppetEntry, reinterpret_cast<const char *>(look));
        std::fflush(stdout);
        return std::nullopt;
    });
}

void writeTransform(uint8_t *rdram, uint32_t obj, const float pos[3], float yaw)
{
    float p[4] = {pos[0], pos[1], pos[2], 1.0f};
    float d[4] = {std::sin(yaw), 0.0f, std::cos(yaw), yaw};
    std::memcpy(rdram + ((obj + kh2state::addr::ObjPos) & 0x01FFFFFFu), p, sizeof(p));
    std::memcpy(rdram + ((obj + kh2state::addr::ObjDir) & 0x01FFFFFFu), d, sizeof(d));
}

// Our puppets' motions are ours: the NPC class's own code (its stand-still
// handler while its action is off, its routine otherwise) starts its idle
// motion every frame, so the puppet glided along standing. While a puppet
// follows a player, YS::MOTION::change / start_num on its MOTION start only
// the motion the player is playing.
struct Guard
{
    std::atomic<uint32_t> motion{0}; // puppet object + 0x140; 0: none
    std::atomic<int32_t> wanted{-1};
};
Guard g_guards[kMaxPuppets + 2]; // our puppets, then the party members players stand in for
float g_lastMotionTime[kMaxPuppets + 2] = {};
uint64_t g_lastStepFrame[kMaxPuppets + 2] = {}; // guest frame of the last look at it
PS2Runtime::RecompiledFunction g_motionUpdate = nullptr;
constexpr uint32_t kMotionUpdate = 0x0017F620; // YS::MOTION's per-frame step (MOTION*, float dt)
constexpr uint32_t kFrameDelta = 0x00349E10;   // the dt YS::OBJ's update passes it

// An object we drive has its action off (0x200000 at +0x10C), and for the
// classes we use that also stops its motion's per-frame step: the player's
// run was started on it but stood still (they glided along). If its
// motion's time did not move since the last frame, step it here, as the
// object's own update would. Once a frame: the matrix is built more than
// once a frame for some (party members), and the second look saw no change
// since the first and stepped it again after the game's own step, so
// stand-ins played every motion at twice the speed.
void stepFrozenMotion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t obj)
{
    if (!g_motionUpdate)
        return;
    const uint32_t m = obj + kh2state::addr::ObjMotion;
    for (size_t i = 0; i < kMaxPuppets + 2; ++i)
    {
        if (g_guards[i].motion.load(std::memory_order_relaxed) != m)
            continue;
        const uint64_t frame = ps2PadCurrentGuestFrame();
        if (frame == g_lastStepFrame[i])
            return;
        g_lastStepFrame[i] = frame;
        uint32_t flags = 0;
        std::memcpy(&flags, rdram + ((obj + 0x10C) & 0x01FFFFFFu), 4);
        float t = 0;
        std::memcpy(&t, rdram + ((m + 0x30) & 0x01FFFFFFu), 4);
        const bool frozen = (flags & 0x200000u) && t == g_lastMotionTime[i];
        if (frozen)
        {
            float dt = 1.0f;
            std::memcpy(&dt, rdram + kFrameDelta, 4);
            if (!(dt > 0.0f && dt < 10.0f))
                dt = 1.0f;
            // Only the clock: the whole step (0x17F620) also works through
            // a queue of motions these objects never filled and started
            // stale entries every frame (run, idle, run, ...).
            float speed = 1.0f;
            std::memcpy(&speed, rdram + ((m + 0xF4) & 0x01FFFFFFu), 4);
            if (!(speed > 0.0f && speed < 10.0f))
                speed = 1.0f;
            const float next = t + dt * speed;
            std::memcpy(rdram + ((m + 0x30) & 0x01FFFFFFu), &next, 4);
            (void)runtime;
            (void)ctx;
            float after = 0;
            std::memcpy(&after, rdram + ((m + 0x30) & 0x01FFFFFFu), 4);
            static int logged = 0;
            if (logged < 40 && std::getenv("KH2_MOTION_LOG"))
            {
                ++logged;
                std::printf("[motion] stepped %08X: %.2f -> %.2f (dt %.2f)\n", obj, t, after, dt);
                std::fflush(stdout);
            }
            t = after;
        }
        g_lastMotionTime[i] = t;
        return;
    }
}
PS2Runtime::RecompiledFunction g_change = nullptr, g_startNum = nullptr, g_push = nullptr, g_start = nullptr,
                               g_startQueued = nullptr, g_locomotion = nullptr;

bool blocked(const uint8_t *rdram, R5900Context *ctx, const char *what)
{
    const uint32_t m = getRegU32(ctx, 4);
    const int32_t id = static_cast<int32_t>(getRegU32(ctx, 5));
    for (const Guard &g : g_guards)
        if (m && g.motion.load(std::memory_order_relaxed) == m)
        {
            // Only an object we drive (its action off): a new object made at
            // the address of one we drove has to start its motions itself.
            uint32_t flags = 0;
            std::memcpy(&flags, rdram + ((m - kh2state::addr::ObjMotion + 0x10C) & 0x01FFFFFFu), 4);
            if (!(flags & 0x200000u))
                return false;
            const int32_t want = g.wanted.load(std::memory_order_relaxed);
            static int traced = 0;
            if (std::getenv("KH2_MOTION_LOG") && traced < 3000)
            {
                ++traced;
                std::printf("[motion] %s(%d) on %08X from %08X (want %d)\n", what, id, m - kh2state::addr::ObjMotion,
                            getRegU32(ctx, 31), want);
                std::fflush(stdout);
            }
            if (want < 0 || id == want)
                return false;
            static int logged = 0;
            if (std::getenv("KH2_PUPPET_LOG") && logged < 20)
            {
                ++logged;
                std::printf("[puppets] kept %s(%d) off puppet motion %08X (want %d, from %08X)\n", what, id, m, want,
                            getRegU32(ctx, 31));
                std::fflush(stdout);
            }
            setReturnU32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return true;
        }
    return false;
}
void changeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!blocked(rdram, ctx, "change"))
        kh2::callOriginal(g_change, rdram, ctx, runtime);
}
// YS::MOTION::push(id, blend, start, callback): queues a motion the
// per-frame step starts later (a party member's own routine queues its idle).
void pushHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!blocked(rdram, ctx, "push"))
        kh2::callOriginal(g_push, rdram, ctx, runtime);
}
// FUN_0017F488(MOTION*, id, f12, f13): the direct motion start the player
// and party classes' own code use (run when moving, idle when not).
void startHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!blocked(rdram, ctx, "start"))
        kh2::callOriginal(g_start, rdram, ctx, runtime);
}
// FUN_0017F268(MOTION*, id, f12, f13): the start the per-frame step (0x17F620)
// calls for each queued motion. change() and start() reach it with a direct
// jump, so this only sees the queue: a driven object's own routine queues
// its idle there, and it undid the run we set (index 2 -> 0 every frame).
void startQueuedHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!blocked(rdram, ctx, "queued"))
        kh2::callOriginal(g_startQueued, rdram, ctx, runtime);
}
// FUN_0017C160(controller, OBJ*): the walk controller's per-frame step (its
// vtable +0x18, from the object update at 0x16AB54). It picks idle, walk or
// run from the object's own velocity (+0x110/+0x114) and changes the motion
// with a direct jump that no hook above sees. We move driven objects by
// position, so their velocity stays 0 and it put them back to idle every
// frame (index 2 -> 0: the stand-ins glided). Skip it for those.
void locomotionHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 5);
    const uint32_t m = obj + kh2state::addr::ObjMotion;
    if (obj)
        for (const Guard &g : g_guards)
            if (g.motion.load(std::memory_order_relaxed) == m)
            {
                uint32_t flags = 0;
                std::memcpy(&flags, rdram + ((obj + 0x10C) & 0x01FFFFFFu), 4);
                if ((flags & 0x200000u) && g.wanted.load(std::memory_order_relaxed) >= 0)
                {
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
                break;
            }
    kh2::callOriginal(g_locomotion, rdram, ctx, runtime);
}
void startNumHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!blocked(rdram, ctx, "start_num"))
        kh2::callOriginal(g_startNum, rdram, ctx, runtime);
}

// FUN_00193608(obj) override; runs on the EE thread.
void buildMatrixHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    {
        // Once a frame, at the player: run queued guest-side jobs.
        uint32_t player;
        std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
        if (player && getRegU32(ctx, 4) == player)
            kh2guest::runPending(rdram, ctx, runtime, kh2state::addr::ObjBuildMatrix); // may not return
    }
    stepFrozenMotion(rdram, ctx, runtime, getRegU32(ctx, 4));
    if (g_any.load(std::memory_order_relaxed) || g_anyOverride.load(std::memory_order_relaxed))
    {
        const uint32_t obj = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mutex);
        bool done = false;
        for (const Slot &s : g_slots)
            if (s.active && !s.pending && s.object == obj)
            {
                writeTransform(rdram, obj, s.pos, s.yaw);
                done = true;
                break;
            }
        for (const auto &list : g_overrides)
        {
            if (done)
                break;
            for (const Override &o : list)
                if (o.object == obj)
                {
                    writeTransform(rdram, obj, o.pos, o.yaw);
                    done = true;
                    break;
                }
        }
    }
    kh2::callOriginal(g_buildMatrix, rdram, ctx, runtime);
}

bool usable(const kh2state::ActorInfo &a)
{
    // Townsfolk and named NPCs; not the player, weapons, prizes or enemies.
    return a.type == 2 || a.type == 15 || a.type == 8;
}
// FUN_0011D6C0(model, bone, out): a model's bone matrix. A weapon asks its
// holder's model for the hand bone every frame; a stand-in made in a drive
// form's look has no model until the look has loaded (+0x670 is null for a
// few frames), and the game then calls through a null table. Give it a
// plain matrix until the model is there: the weapon sits at the holder.
// FUN_0017EFA0(MOTION*, motion file, f12, f13): starts a motion from its
// file, whose first animation part (BAR type 9) it plays without looking:
// a file that has none (a drive form's motion on a stand-in made in that
// form, whose motions are not all loaded) left it calling through 0. Such a
// motion is not started; the current one goes on.
// FUN_00185A68(obj, kind, arg): a motion's cue, handed to the object at
// obj+0x9F4 through its vtable +0x50. A stand-in (a party member) playing
// Sora's victory motion met a cue meant for the player: its +0x9F4 is the
// player, whose class has nothing there, and the game called 0. Such a cue
// is dropped.
constexpr uint32_t kMotionCue = 0x00185A68;
PS2Runtime::RecompiledFunction g_motionCue = nullptr;

void motionCueHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    uint32_t to = 0, vt = 0, fn = 0;
    auto rd32 = [&](uint32_t a) {
        uint32_t v = 0;
        if (a >= 0x00100000u && a < 0x02000000u - 4)
            std::memcpy(&v, rdram + a, 4);
        return v;
    };
    to = rd32(obj + 0x9F4);
    vt = to ? rd32(to) : 0;
    fn = vt ? rd32(vt + 0x50) : 0;
    if (ctx->pc == kMotionCue && !fn)
    {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 8)
        {
            std::printf("[puppets] motion cue %u on %08X has nobody to take it (%08X): dropped\n", getRegU32(ctx, 5),
                        obj, to);
            std::fflush(stdout);
        }
        setReturnU32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_motionCue, rdram, ctx, runtime);
}

constexpr uint32_t kMotionSet = 0x0017EFA0;
PS2Runtime::RecompiledFunction g_motionSet = nullptr;

void motionSetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t file = getRegU32(ctx, 5);
    if (ctx->pc == kMotionSet && file >= 0x00100000u && file < 0x02000000u - 0x10)
    {
        uint32_t count = 0;
        std::memcpy(&count, rdram + file + 4, 4);
        bool anim = false;
        for (uint32_t i = 0; i < count && i < 256 && !anim; ++i)
        {
            uint16_t type = 0;
            const uint32_t at = file + 0x10 + i * 0x10;
            if (at + 0x10 > 0x02000000u)
                break;
            std::memcpy(&type, rdram + at, 2);
            anim = type == 9;
        }
        if (!anim)
        {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 8)
            {
                std::printf("[puppets] motion file %08X on %08X has no animation (not loaded): not started\n", file,
                            getRegU32(ctx, 4));
                std::fflush(stdout);
            }
            setReturnU32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    kh2::callOriginal(g_motionSet, rdram, ctx, runtime);
}

constexpr uint32_t kBoneMatrix = 0x0011D6C0;
PS2Runtime::RecompiledFunction g_boneMatrix = nullptr;
std::atomic<uint32_t> g_boneMisses{0};

void boneMatrixHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t model = getRegU32(ctx, 4);
    uint32_t inner = 0;
    if (model && model < 0x02000000u)
        std::memcpy(&inner, rdram + model + 0x14, 4);
    if (inner)
    {
        kh2::callOriginal(g_boneMatrix, rdram, ctx, runtime);
        return;
    }
    const uint32_t out = getRegU32(ctx, 6);
    if (out && out < 0x02000000u - 64)
    {
        const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(rdram + out, id, sizeof(id));
    }
    ctx->f[0] = 0.0f;
    setReturnU32(ctx, out);
    if (g_boneMisses.fetch_add(1) < 4)
    {
        std::printf("[puppets] bone %u asked of a model that is not loaded yet (0x%08X): a plain matrix given\n",
                    getRegU32(ctx, 5), model);
        std::fflush(stdout);
    }
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_motionCue = runtime.lookupFunction(kMotionCue);
    if (!g_motionCue || !runtime.replaceFunction(kMotionCue, &motionCueHook))
        g_motionCue = nullptr;
    g_motionSet = runtime.lookupFunction(kMotionSet);
    if (!g_motionSet || !runtime.replaceFunction(kMotionSet, &motionSetHook))
        g_motionSet = nullptr;
    g_boneMatrix = runtime.lookupFunction(kBoneMatrix);
    if (!g_boneMatrix || !runtime.replaceFunction(kBoneMatrix, &boneMatrixHook))
        g_boneMatrix = nullptr;
    g_buildMatrix = runtime.lookupFunction(kh2state::addr::ObjBuildMatrix);
    if (!g_buildMatrix || !runtime.replaceFunction(kh2state::addr::ObjBuildMatrix, &buildMatrixHook))
    {
        std::fprintf(stderr, "[puppets] could not hook 0x%08X\n", kh2state::addr::ObjBuildMatrix);
        g_buildMatrix = nullptr;
        return false;
    }
    constexpr uint32_t kStartNum = 0x0017EDA8; // YS::MOTION::start_num(int, float, float)
    g_motionUpdate = runtime.lookupFunction(kMotionUpdate);
    g_change = runtime.lookupFunction(kMotionChange);
    g_startNum = runtime.lookupFunction(kStartNum);
    if (!g_change || !runtime.replaceFunction(kMotionChange, &changeHook))
        g_change = nullptr;
    // Not start_num: it takes a motion set's entry number, which change()
    // itself passes on (run, motion 2, is entry 8 for Roxas): guarding it by
    // motion id stopped our own changes half way (the stand-ins glided).
    (void)kStartNum;
    constexpr uint32_t kMotionStart = 0x0017F488;
    g_start = runtime.lookupFunction(kMotionStart);
    if (!g_start || !runtime.replaceFunction(kMotionStart, &startHook))
        g_start = nullptr;
    constexpr uint32_t kMotionPush = 0x0017F5E0;
    g_push = runtime.lookupFunction(kMotionPush);
    if (!g_push || !runtime.replaceFunction(kMotionPush, &pushHook))
        g_push = nullptr;
    constexpr uint32_t kMotionStartQueued = 0x0017F268;
    g_startQueued = runtime.lookupFunction(kMotionStartQueued);
    if (!g_startQueued || !runtime.replaceFunction(kMotionStartQueued, &startQueuedHook))
        g_startQueued = nullptr;
    constexpr uint32_t kLocomotion = 0x0017C160;
    g_locomotion = runtime.lookupFunction(kLocomotion);
    if (!g_locomotion || !runtime.replaceFunction(kLocomotion, &locomotionHook))
        g_locomotion = nullptr;
    (void)&startNumHook;
    g_startNum = nullptr;
    return true;
}

void setEnabled(bool on) { g_enabled = on; }
void requestSpawn(uint32_t entryId, float distance)
{
    // Read where the player is when the job starts, then spawn there.
    kh2guest::post([entryId, distance](const kh2guest::Env &env, uint32_t, bool) -> std::optional<kh2guest::Call> {
        uint32_t player;
        std::memcpy(&player, env.host(kh2state::addr::PlayerPtr), 4);
        if (player)
        {
            float pos[4], dir[4];
            std::memcpy(pos, env.host(player + kh2state::addr::ObjPos), 16);
            std::memcpy(dir, env.host(player + kh2state::addr::ObjDir), 16);
            pos[0] += dir[0] * distance;
            pos[2] += dir[2] * distance;
            const float where[3] = {pos[0], pos[1], pos[2]};
            postSpawn(entryId, where, dir[3], [entryId, where](uint32_t obj) {
                std::printf("[puppets] spawned entry %04X at %.0f,%.0f,%.0f -> object %08X\n", entryId, where[0],
                            where[1], where[2], obj);
                std::fflush(stdout);
            });
        }
        return std::nullopt;
    });
}
bool enabled() { return g_enabled; }

void update(const uint8_t *rdram, const std::vector<Target> &targets)
{
    if (!g_buildMatrix)
        return;
    const kh2state::State st = kh2state::read(rdram);
    std::vector<kh2state::ActorInfo> actors;
    auto haveActors = [&]() -> const std::vector<kh2state::ActorInfo> & {
        if (actors.empty())
            actors = kh2state::actors(rdram);
        return actors;
    };
    const double t = now();
    std::lock_guard<std::mutex> lock(g_mutex);
    bool any = false;
    for (Slot &s : g_slots)
    {
        if (!s.active)
            continue;
        // Still wanted? Same player, in our room, and the object still exists.
        const Target *tg = nullptr;
        for (const Target &x : targets)
            if (x.playerId == s.playerId)
                tg = &x;
        const bool sameRoom = tg && tg->world == st.now.world && tg->room == st.now.room;
        bool keep = g_enabled && sameRoom;
        if (keep && !s.pending)
        {
            keep = false;
            for (const kh2state::ActorInfo &a : haveActors())
                keep = keep || a.address == s.object;
        }
        if (!keep)
        {
            std::printf("[puppets] player %u releases %s\n", s.playerId + 1, s.name);
            std::fflush(stdout);
            bool there = false;
            if (s.spawned && s.object && (sameRoom || !tg))
                for (const kh2state::ActorInfo &a : haveActors())
                    there = there || a.address == s.object;
            if (there)
            {
                // Ours and still in the object list: make it leave. (The
                // record keeps their look; the entry is ours alone.)
                postLeave(s.object);
            }
            s = Slot{};
            continue;
        }
        any = true;
        std::memcpy(s.pos, tg->pos, sizeof(s.pos));
        s.yaw = tg->yaw;
        if (s.pending)
        {
            // Spawn once its files have had time to load (the request is
            // asynchronous; 1.5 s is plenty for a model and its motions).
            // One spawn at a time, and only in free play (not during a
            // cutscene, when jobs wait and the room may change under them).
            bool another = false;
            for (const Slot &o : g_slots)
                another = another || (&o != &s && o.active && o.pending && o.spawnPosted);
            if (!s.spawnPosted && s.loadRequested && t - s.loadAt > 1.5 && !another && st.control == 0)
            {
                s.spawnPosted = true;
                const uint8_t id = s.playerId;
                const uint32_t entry = s.entry;
                const uint32_t gen = s.gen;
                const Target where = *tg;
                postSpawn(
                    entry, where.pos, where.yaw,
                    [id, entry, gen](uint32_t obj) {
                        if (obj)
                        {
                            std::printf("[puppets] player %u spawned as entry %04X -> %08X\n", id + 1, entry, obj);
                            std::fflush(stdout);
                        }
                        std::lock_guard<std::mutex> lock(g_mutex);
                        bool mine = false;
                        for (Slot &x : g_slots)
                            if (x.active && x.pending && x.gen == gen)
                            {
                                mine = true;
                                x.pending = false;
                                x.object = obj;
                                if (!obj)
                                    x = Slot{}; // failed; tried again next update
                            }
                        if (!mine && obj)
                        {
                            // Its slot went while it was being made: nobody
                            // follows this one, so it goes too.
                            std::printf("[puppets] entry %04X object %08X is nobody's: removing it\n", entry, obj);
                            std::fflush(stdout);
                            postLeave(obj);
                        }
                    },
                    [gen](const uint8_t *ram) {
                        const kh2state::State now = kh2state::read(ram);
                        std::lock_guard<std::mutex> lock(g_mutex);
                        for (const Slot &x : g_slots)
                            if (x.active && x.pending && x.gen == gen)
                                return x.world == now.now.world && x.room == now.now.room && now.control == 0;
                        return false;
                    });
            }
            continue;
        }
        static const bool kLog = std::getenv("KH2_PUPPET_LOG") != nullptr;
        static double lastLog = 0;
        if (kLog && s.spawned && t - lastLog > 0.5)
        {
            lastLog = t;
            auto dump = [&](const char *who, uint32_t obj) {
                std::printf("[puppets] %s %08X motion:", who, obj);
                for (uint32_t i = 0; i < 0x60; i += 4)
                {
                    uint32_t v;
                    std::memcpy(&v, rdram + ((obj + kh2state::addr::ObjMotion + i) & 0x01FFFFFFu), 4);
                    std::printf(" %08X", v);
                }
                std::printf("\n");
            };
            std::printf("[puppets] target motion %u\n", tg->motion);
            dump("puppet", s.object);
            uint32_t me;
            std::memcpy(&me, rdram + kh2state::addr::PlayerPtr, 4);
            if (me)
                dump("me", me);
            std::fflush(stdout);
        }
        // Our own puppets also play the player's motions. Their NPC
        // routine is off (it would start its own idle every frame: the
        // puppet glided along in it), and the motion is started again if
        // something else changed it.
        if (s.spawned)
        {
            constexpr uint32_t kObjFlags = 0x10C, kNoAct = 0x200000u;
            uint32_t flags;
            std::memcpy(&flags, rdram + ((s.object + kObjFlags) & 0x01FFFFFFu), 4);
            if (!(flags & kNoAct))
            {
                flags |= kNoAct;
                std::memcpy(const_cast<uint8_t *>(rdram) + ((s.object + kObjFlags) & 0x01FFFFFFu), &flags, 4);
            }
        }
        uint32_t playing = 0;
        if (s.spawned)
            std::memcpy(&playing, rdram + ((s.object + kh2state::addr::ObjMotionIndex) & 0x01FFFFFFu), 4);
        ++s.sinceMotion;
        if (s.spawned && (tg->motion != s.motion || (playing != tg->motion && s.sinceMotion > 4)))
        {
            s.sinceMotion = 0;
            s.motion = tg->motion;
            const uint32_t obj = s.object;
            const uint32_t motion = tg->motion;
            postMotion(obj, motion, 4.0f);
        }
    }
    // New players in our room get a puppet.
    if (g_enabled)
        for (const Target &tg : targets)
        {
            if (tg.world != st.now.world || tg.room != st.now.room)
                continue;
            bool have = false;
            for (const Slot &s : g_slots)
                have = have || (s.active && s.playerId == tg.playerId);
            if (have)
                continue;
            Slot *free = nullptr;
            for (Slot &s : g_slots)
                if (!s.active && !free)
                    free = &s;
            if (!free)
                break;
            const uint32_t entry = g_mode == Mode::Spawn && tg.model ? kPuppetEntries[free - g_slots] : 0;
            if (entry)
            {
                // Spawn mode: dress the slot's entry as their character and
                // load its files first.
                *free = Slot{};
                free->active = free->spawned = free->pending = true;
                free->playerId = tg.playerId;
                free->gen = ++g_gen;
                free->world = tg.world;
                free->room = tg.room;
                free->entry = entry;
                free->loadAt = t;
                std::snprintf(free->name, sizeof(free->name), "puppet %04X", tg.model);
                std::memcpy(free->pos, tg.pos, sizeof(free->pos));
                free->yaw = tg.yaw;
                postDress(entry, tg.model);
                // The files load from when the request job runs (it waits
                // with every other job while the player is away), not from now.
                const uint32_t gen = free->gen;
                preload(entry, [gen]() {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    for (Slot &x : g_slots)
                        if (x.active && x.gen == gen)
                        {
                            x.loadRequested = true;
                            x.loadAt = now();
                        }
                });
                any = true;
                continue;
            }
            // Borrow mode: the first NPC in the room nobody uses.
            for (const kh2state::ActorInfo &a : haveActors())
            {
                if (!usable(a))
                    continue;
                bool taken = false;
                for (const Slot &s : g_slots)
                    taken = taken || (s.active && s.object == a.address);
                if (taken)
                    continue;
                *free = Slot{};
                free->active = true;
                free->playerId = tg.playerId;
                free->object = a.address;
                std::memcpy(free->name, a.name, sizeof(free->name));
                std::memcpy(free->pos, tg.pos, sizeof(free->pos));
                free->yaw = tg.yaw;
                std::printf("[puppets] player %u borrows %s (%08X)\n", tg.playerId + 1, a.name, a.address);
                any = true;
                break;
            }
        }
    g_any = any;
    // What each puppet's motion may be changed to (see changeHook).
    for (size_t i = 0; i < kMaxPuppets; ++i)
    {
        const Slot &s = g_slots[i];
        const bool on = s.active && s.spawned && !s.pending && s.object;
        int32_t want = -1;
        if (on)
            for (const Target &x : targets)
                if (x.playerId == s.playerId)
                    want = x.motion;
        g_guards[i].wanted.store(want, std::memory_order_relaxed);
        g_guards[i].motion.store(on ? s.object + kh2state::addr::ObjMotion : 0, std::memory_order_relaxed);
    }
}

void requestPreload(uint32_t entryId) { preload(entryId, {}); }

void requestFormLoad(uint32_t entryId)
{
    // As the drive itself does for Sora (FUN_001B65E0): the form's entry is
    // read into a cache group of its own (0x1388 for Sora; 0x1389 here),
    // FUN_0016EB48(entry, group, 4, 7), then CACHE_BUFF::Flush. Read into the
    // room's general group (-1, as requestPreload does) a form's files took
    // the place of things the room's objects still used, and they crashed.
    constexpr uint32_t kReadRequest = 0x0016EB48;
    constexpr uint32_t kCacheFlush = 0x00170888;
    constexpr uint32_t kGroup = 0x1389;
    auto step = std::make_shared<int>(0);
    kh2guest::post([entryId, step](const kh2guest::Env &env, uint32_t, bool) -> std::optional<kh2guest::Call> {
        if (*step == 0)
            ensurePrivateTable(env.rdram);
        switch ((*step)++)
        {
        case 0:
            return kh2guest::Call{kReadRequest, {entryId, kGroup, 4u, 7u}, {}};
        case 1:
            return kh2guest::Call{kCacheFlush, {0u}, {}};
        default:
            std::printf("[puppets] requested loading form look %04X (cache group %04X)\n", entryId, kGroup);
            std::fflush(stdout);
            return std::nullopt;
        }
    });
}

namespace
{
void preload(uint32_t entryId, std::function<void()> requested)
{
    // As a room's spawn list does (FUN_00199D38): the objentry's read request
    // (model, textures, motions into the cache buffer), then
    // CACHE_BUFF::Flush(nullptr), as the script's trap_file_flush does, which
    // starts the reads; a request alone just waits for the next room load.
    constexpr uint32_t kEntryRequest = 0x0016ED50; // (entry id, -1, 0x1E)
    constexpr uint32_t kCacheFlush = 0x00170888;   // YS::CACHE_BUFF::Flush(TASK*)
    auto step = std::make_shared<int>(0);
    kh2guest::post([entryId, step, requested](const kh2guest::Env &env, uint32_t, bool) -> std::optional<kh2guest::Call> {
        if (*step == 0)
            ensurePrivateTable(env.rdram);
        switch ((*step)++)
        {
        case 0:
            return kh2guest::Call{kEntryRequest, {entryId, 0xFFFFFFFFu, 0x1Eu}, {}};
        case 1:
            return kh2guest::Call{kCacheFlush, {0u}, {}};
        default:
            std::printf("[puppets] requested loading entry %04X\n", entryId);
            std::fflush(stdout);
            if (requested)
                requested();
            return std::nullopt;
        }
    });
}
} // namespace

void requestDress(uint32_t puppetEntry, uint32_t lookEntry) { postDress(puppetEntry, lookEntry); }

void postMotion(uint32_t obj, uint32_t id, float blend)
{
    // YS::MOTION::is_exist first: a motion the object's set does not have
    // (a player's reaction-command motions on a stand-in) made change()
    // jump through garbage.
    constexpr uint32_t kMotionIsExist = 0x001805A8;
    auto step = std::make_shared<int>(0);
    kh2guest::post([obj, id, blend, step](const kh2guest::Env &env, uint32_t result, bool) -> std::optional<kh2guest::Call> {
        auto live = [&] {
            for (const kh2state::ActorInfo &a : kh2state::actors(env.rdram))
                if (a.address == obj)
                    return true;
            return false;
        };
        switch ((*step)++)
        {
        case 0:
            if (!live())
                return std::nullopt;
            return kh2guest::Call{kMotionIsExist, {obj + kh2state::addr::ObjMotion, id}, {}};
        case 1:
            if (std::getenv("KH2_MOTION_LOG"))
            {
                std::printf("[motion] %08X motion %u exists: %u\n", obj, id, result);
                std::fflush(stdout);
            }
            if (!(result & 1u) || !live())
                return std::nullopt;
            return kh2guest::Call{kMotionChange, {obj + kh2state::addr::ObjMotion, id}, {blend, 0.0f}};
        default:
            return std::nullopt;
        }
    });
}

void spawnAt(uint32_t entryId, const float pos[3], float yaw, std::function<void(uint32_t)> done, int32_t serial,
             std::function<bool(const uint8_t *)> wanted)
{
    postSpawn(entryId, pos, yaw, std::move(done), std::move(wanted), serial);
}

void setOverrides(std::vector<Override> overrides, int channel)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_overrides[channel & 1] = std::move(overrides);
    g_anyOverride = !g_overrides[0].empty() || !g_overrides[1].empty();
}

void setMotionGuards(const std::vector<std::pair<uint32_t, int32_t>> &objects)
{
    for (size_t i = 0; i < 2; ++i)
    {
        Guard &g = g_guards[kMaxPuppets + i];
        const bool on = i < objects.size() && objects[i].first;
        g.wanted.store(on ? objects[i].second : -1, std::memory_order_relaxed);
        g.motion.store(on ? objects[i].first + kh2state::addr::ObjMotion : 0, std::memory_order_relaxed);
    }
}

void remove(uint32_t obj) { postLeave(obj); }

void setMode(Mode mode) { g_mode = mode; }
Mode mode() { return g_mode; }

std::vector<Assignment> assignments()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Assignment> out;
    for (const Slot &s : g_slots)
        if (s.active)
        {
            Assignment a{s.playerId, s.object, {}};
            std::memcpy(a.name, s.name, sizeof(a.name));
            out.push_back(a);
        }
    return out;
}
} // namespace kh2puppets
