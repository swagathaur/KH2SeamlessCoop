#include "puppets.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>

#include "game_state.h"
#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace kh2puppets
{
void requestPreload(uint32_t entryId);
namespace
{
constexpr size_t kMaxPuppets = 2;

struct Slot
{
    bool active = false;
    bool spawned = false;  // we created the object (and must remove it)
    bool pending = false;  // spawn job queued, object not known yet
    bool spawnPosted = false;
    uint32_t entry = 0;
    double loadAt = 0;     // spawn mode: when the model load was requested
    uint8_t playerId = 0;
    uint16_t motion = 0xFFFF; // last motion we started on it
    uint32_t object = 0;
    char name[33] = {};
    float pos[3] = {0, 0, 0};
    float yaw = 0;
};

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
std::vector<Override> g_overrides; // under g_mutex
PS2Runtime::RecompiledFunction g_buildMatrix = nullptr;
constexpr uint32_t kObjFactory = 0x0016F000; // OBJ* (entry id, const FVector* pos, float yaw in $f12)
constexpr uint32_t kMotionChange = 0x0017F540; // YS::MOTION::change(MOTION* = obj+0x140, int id, float blend, float start)
constexpr uint32_t kVtableLeave = 0x20; // obj->vtable[+0x20](obj): leave (what trap_obj_leave_force calls)
std::atomic<Mode> g_mode{Mode::Spawn};

// Spawning, as a guest job (one call per frame): allocate a spawn record and
// an empty unit group from the room's heap (AREA::Alloc; what a room's spawn
// list would give YS::UNIT_DATA_OBJ::appear), call the object factory, and
// finish the object with its setup method (vtable +0x38). The factory
// stores the newest object where GetPlayer() reads (the player is simply
// created last when a room loads), so that pointer is put back.
void postSpawn(uint32_t entry, const float pos[3], float yaw, std::function<void(uint32_t obj)> done)
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
    kh2guest::post([st, entry, done](const kh2guest::Env &env, uint32_t result, bool) -> std::optional<kh2guest::Call> {
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
            const float p[4] = {st->pos[0], st->pos[1], st->pos[2], 1.0f};
            std::memcpy(env.host(env.scratch), p, 16);
            std::memcpy(&st->player, env.host(kh2state::addr::PlayerPtr), 4);
            return kh2guest::Call{kObjFactory, {entry, env.scratch}, {st->yaw}};
        }
        case 3:
        {
            st->obj = result;
            std::memcpy(env.host(kh2state::addr::PlayerPtr), &st->player, 4);
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
// look and motions. Each puppet slot has its own record (entries the game
// uses only in a few cutscenes), put back when the puppet goes.
constexpr uint32_t kPuppetEntries[kMaxPuppets] = {0x084C, 0x084D}; // B_EX110_RTN, B_EX170_RTN
constexpr uint32_t kEntrySize = 0x60, kEntryLook = 0x08, kEntryLookSize = 0x40;

uint32_t entryRecord(const uint8_t *rdram, uint32_t id)
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
        if (!done)
            for (const Override &o : g_overrides)
                if (o.object == obj)
                {
                    writeTransform(rdram, obj, o.pos, o.yaw);
                    break;
                }
    }
    kh2::callOriginal(g_buildMatrix, rdram, ctx, runtime);
}

bool usable(const kh2state::ActorInfo &a)
{
    // Townsfolk and named NPCs; not the player, weapons, prizes or enemies.
    return a.type == 2 || a.type == 15 || a.type == 8;
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_buildMatrix = runtime.lookupFunction(kh2state::addr::ObjBuildMatrix);
    if (!g_buildMatrix || !runtime.replaceFunction(kh2state::addr::ObjBuildMatrix, &buildMatrixHook))
    {
        std::fprintf(stderr, "[puppets] could not hook 0x%08X\n", kh2state::addr::ObjBuildMatrix);
        g_buildMatrix = nullptr;
        return false;
    }
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
            if (s.spawned && s.object && (sameRoom || !tg))
            {
                // Ours and still in this room: make it leave.
                postLeave(s.object);
            }
            if (s.spawned)
                postDress(s.entry, 0);
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
            if (!s.spawnPosted && t - s.loadAt > 1.5)
            {
                s.spawnPosted = true;
                const uint8_t id = s.playerId;
                const uint32_t entry = s.entry;
                const Target where = *tg;
                postSpawn(entry, where.pos, where.yaw, [id, entry](uint32_t obj) {
                    std::printf("[puppets] player %u spawned as entry %04X -> %08X\n", id + 1, entry, obj);
                    std::fflush(stdout);
                    std::lock_guard<std::mutex> lock(g_mutex);
                    for (Slot &x : g_slots)
                        if (x.active && x.pending && x.playerId == id)
                        {
                            x.pending = false;
                            x.object = obj;
                            if (!obj)
                                x = Slot{}; // failed; tried again next update
                        }
                });
            }
            continue;
        }
        // Our own puppets also play the player's motions.
        if (s.spawned && tg->motion != s.motion)
        {
            s.motion = tg->motion;
            const uint32_t obj = s.object;
            const uint32_t motion = tg->motion;
            kh2guest::postCall({kMotionChange, {obj + kh2state::addr::ObjMotion, motion}, {4.0f, 0.0f}});
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
                free->entry = entry;
                free->loadAt = t;
                std::snprintf(free->name, sizeof(free->name), "puppet %04X", tg.model);
                std::memcpy(free->pos, tg.pos, sizeof(free->pos));
                free->yaw = tg.yaw;
                postDress(entry, tg.model);
                requestPreload(entry);
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
}

void requestPreload(uint32_t entryId)
{
    // As a room's spawn list does (FUN_00199D38): the objentry's read request
    // (model, textures, motions into the cache buffer), then
    // CACHE_BUFF::Flush(nullptr), as the script's trap_file_flush does, which
    // starts the reads; a request alone just waits for the next room load.
    constexpr uint32_t kEntryRequest = 0x0016ED50; // (entry id, -1, 0x1E)
    constexpr uint32_t kCacheFlush = 0x00170888;   // YS::CACHE_BUFF::Flush(TASK*)
    auto step = std::make_shared<int>(0);
    kh2guest::post([entryId, step](const kh2guest::Env &, uint32_t, bool) -> std::optional<kh2guest::Call> {
        switch ((*step)++)
        {
        case 0:
            return kh2guest::Call{kEntryRequest, {entryId, 0xFFFFFFFFu, 0x1Eu}, {}};
        case 1:
            return kh2guest::Call{kCacheFlush, {0u}, {}};
        default:
            std::printf("[puppets] requested loading entry %04X\n", entryId);
            std::fflush(stdout);
            return std::nullopt;
        }
    });
}

void requestDress(uint32_t puppetEntry, uint32_t lookEntry) { postDress(puppetEntry, lookEntry); }

void setOverrides(std::vector<Override> overrides)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_overrides = std::move(overrides);
    g_anyOverride = !g_overrides.empty();
}

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
