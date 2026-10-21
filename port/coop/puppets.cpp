#include "puppets.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

#include "game_state.h"
#include "guest_call.h"
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
PS2Runtime::RecompiledFunction g_buildMatrix = nullptr;
constexpr uint32_t kObjFactory = 0x0016F000; // OBJ* (entry id, const FVector* pos, float yaw in $f12)
constexpr uint32_t kEntryLoad = 0x0016EB48;    // (entry id, priority, 4, 7): queue loading its files
constexpr uint32_t kMotionChange = 0x0017F540; // YS::MOTION::change(MOTION* = obj+0x140, int id, float blend, float start)
constexpr uint32_t kObjMotion = 0x140;
constexpr uint32_t kVtableLeave = 0x20; // obj->vtable[+0x20](obj): leave (what trap_obj_leave_force calls)
std::atomic<Mode> g_mode{Mode::Borrow};

// The object factory, keeping GetPlayer()'s static: object construction
// stores the newest object there (the player is simply created last when a
// room loads), so a spawned object would otherwise become "the player".
uint32_t spawnObject(const kh2guest::Env &env, uint32_t entry, const float pos[3], float yaw)
{
    constexpr uint32_t kAreaAlloc = 0x001050D8; // AREA::Alloc(size): the room's heap, freed when it unloads
    constexpr uint32_t kVtableSetup = 0x38;     // obj->vtable[+0x38](obj, UNIT_GROUP*, UNIT_DATA_OBJ*)
    uint32_t player;
    std::memcpy(&player, env.host(kh2state::addr::PlayerPtr), 4);
    // A spawn record and an empty unit group, as a room's spawn list would
    // give (YS::UNIT_DATA_OBJ::appear): entry id, position, yaw at +0x14.
    const uint32_t unit = kh2guest::call(env, kAreaAlloc, {0x40u});
    const uint32_t group = kh2guest::call(env, kAreaAlloc, {0x40u});
    if (!unit || !group)
        return 0;
    std::memset(env.host(unit), 0, 0x40);
    std::memset(env.host(group), 0, 0x40);
    std::memcpy(env.host(unit), &entry, 4);
    std::memcpy(env.host(unit + 4), pos, 12);
    std::memcpy(env.host(unit + 0x14), &yaw, 4);
    const float p[4] = {pos[0], pos[1], pos[2], 1.0f};
    std::memcpy(env.host(env.scratch), p, 16);
    const uint32_t obj = kh2guest::call(env, kObjFactory, {entry, env.scratch}, {yaw});
    if (obj)
    {
        uint32_t vt = 0, setup = 0;
        std::memcpy(&vt, env.host(obj), 4);
        if (vt >= 0x00100000u && vt < 0x02000000u)
            std::memcpy(&setup, env.host(vt + kVtableSetup), 4);
        if (setup)
            kh2guest::call(env, setup, {obj, group, unit});
    }
    std::memcpy(env.host(kh2state::addr::PlayerPtr), &player, 4);
    return obj;
}

// The NPC-class entry that shows a player character (docs/findings.md).
uint32_t puppetEntryFor(uint16_t model)
{
    switch (model)
    {
    case 0x005A: case 0x0323: case 0x088E: // Roxas (P_EX110 and forms)
        return 0x084C;                      // B_EX110_RTN
    case 0x0054: case 0x0055: case 0x0056: case 0x0057: case 0x0058: case 0x0059: case 0x0236: case 0x02B5:
        return 0x08D2; // Sora: P_EX100_LAST
    default:
        return 0;
    }
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
            kh2guest::runPending(rdram, ctx, runtime);
    }
    if (g_any.load(std::memory_order_relaxed))
    {
        const uint32_t obj = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Slot &s : g_slots)
            if (s.active && !s.pending && s.object == obj)
            {
                writeTransform(rdram, obj, s.pos, s.yaw);
                break;
            }
    }
    g_buildMatrix(rdram, ctx, runtime);
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
    kh2guest::post([entryId, distance](const kh2guest::Env &env) {
        uint32_t player;
        std::memcpy(&player, env.host(kh2state::addr::PlayerPtr), 4);
        if (!player)
            return;
        float pos[4], dir[4];
        std::memcpy(pos, env.host(player + kh2state::addr::ObjPos), 16);
        std::memcpy(dir, env.host(player + kh2state::addr::ObjDir), 16);
        pos[0] += dir[0] * distance;
        pos[2] += dir[2] * distance;
        const uint32_t obj = spawnObject(env, entryId, pos, dir[3]);
        std::printf("[puppets] spawned entry %04X at %.0f,%.0f,%.0f -> object %08X\n", entryId, pos[0], pos[1],
                    pos[2], obj);
        std::fflush(stdout);
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
                const uint32_t obj = s.object;
                kh2guest::post([obj](const kh2guest::Env &env) {
                    uint32_t vt = 0, fn = 0;
                    std::memcpy(&vt, env.host(obj), 4);
                    if (vt >= 0x00100000u && vt < 0x02000000u)
                        std::memcpy(&fn, env.host(vt + kVtableLeave), 4);
                    if (fn)
                        kh2guest::call(env, fn, {obj});
                });
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
            if (!s.spawnPosted && t - s.loadAt > 1.5)
            {
                s.spawnPosted = true;
                const uint8_t id = s.playerId;
                const uint32_t entry = s.entry;
                const Target where = *tg;
                kh2guest::post([id, entry, where](const kh2guest::Env &env) {
                    const uint32_t obj = spawnObject(env, entry, where.pos, where.yaw);
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
            kh2guest::post([obj, motion](const kh2guest::Env &env) {
                kh2guest::call(env, kMotionChange, {obj + kh2state::addr::ObjMotion, motion}, {4.0f, 0.0f});
            });
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
            const uint32_t entry = g_mode == Mode::Spawn ? puppetEntryFor(tg.model) : 0;
            if (entry)
            {
                // Spawn mode: load the character's files first.
                *free = Slot{};
                free->active = free->spawned = free->pending = true;
                free->playerId = tg.playerId;
                free->entry = entry;
                free->loadAt = t;
                std::snprintf(free->name, sizeof(free->name), "puppet %04X", entry);
                std::memcpy(free->pos, tg.pos, sizeof(free->pos));
                free->yaw = tg.yaw;
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
    kh2guest::post([entryId](const kh2guest::Env &env) {
        kh2guest::call(env, kEntryLoad, {entryId, 0x1388u, 4u, 7u});
        std::printf("[puppets] requested loading entry %04X\n", entryId);
        std::fflush(stdout);
    });
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
