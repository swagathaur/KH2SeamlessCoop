#include "puppets.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

#include "game_state.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace kh2puppets
{
namespace
{
constexpr size_t kMaxPuppets = 2;

struct Slot
{
    bool active = false;
    uint8_t playerId = 0;
    uint32_t object = 0;
    char name[33] = {};
    float pos[3] = {0, 0, 0};
    float yaw = 0;
};

std::mutex g_mutex;
Slot g_slots[kMaxPuppets];
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_any{false};
PS2Runtime::RecompiledFunction g_buildMatrix = nullptr;
std::atomic<uint32_t> g_spawnEntry{0};
std::atomic<float> g_spawnDistance{150.0f};

constexpr uint32_t kObjFactory = 0x0016F000; // OBJ* (entry id, const FVector* pos, float yaw in $f12)

// Calls a guest function from inside an override, on the EE thread: the
// context is saved and restored around it, and the callee gets stack below
// the scratch area used for pointer arguments.
uint32_t callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address,
                   std::initializer_list<uint32_t> args, std::initializer_list<float> fargs)
{
    PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
    if (!fn)
        return 0;
    const R5900Context saved = *ctx;
    int i = 4;
    for (uint32_t a : args)
        SET_GPR_U32(ctx, i++, a);
    int f = 12;
    for (float v : fargs)
        ctx->f[f++] = v;
    SET_GPR_U32(ctx, 31, 0u);
    fn(rdram, ctx, runtime);
    const uint32_t result = getRegU32(ctx, 2);
    *ctx = saved;
    return result;
}

void spawnNow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t player, uint32_t entry)
{
    float pos[4], dir[4];
    std::memcpy(pos, rdram + ((player + kh2state::addr::ObjPos) & 0x01FFFFFFu), 16);
    std::memcpy(dir, rdram + ((player + kh2state::addr::ObjDir) & 0x01FFFFFFu), 16);
    const float d = g_spawnDistance.load();
    pos[0] += dir[0] * d;
    pos[2] += dir[2] * d;
    pos[3] = 1.0f;
    // Scratch for the FVector well below the caller's frame; the callee's
    // frames go below that.
    const uint32_t sp = getRegU32(ctx, 29);
    const uint32_t vec = (sp - 0x100u) & ~15u;
    std::memcpy(rdram + (vec & 0x01FFFFFFu), pos, 16);
    const R5900Context saved = *ctx;
    SET_GPR_U32(ctx, 29, vec - 0x40u);
    const uint32_t obj = callGuest(rdram, ctx, runtime, kObjFactory, {entry, vec}, {dir[3]});
    *ctx = saved;
    std::printf("[puppets] spawned entry %04X at %.0f,%.0f,%.0f -> object %08X\n", entry, pos[0], pos[1], pos[2], obj);
    std::fflush(stdout);
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
    if (const uint32_t entry = g_spawnEntry.load(std::memory_order_relaxed))
    {
        uint32_t player;
        std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
        if (player && getRegU32(ctx, 4) == player && g_spawnEntry.exchange(0) == entry)
            spawnNow(rdram, ctx, runtime, player, entry);
    }
    if (g_any.load(std::memory_order_relaxed))
    {
        const uint32_t obj = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Slot &s : g_slots)
            if (s.active && s.object == obj)
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
    g_spawnDistance = distance;
    g_spawnEntry = entryId;
}
bool enabled() { return g_enabled; }

void update(const uint8_t *rdram, const std::vector<Target> &targets)
{
    if (!g_buildMatrix)
        return;
    const kh2state::State st = kh2state::read(rdram);
    std::vector<kh2state::ActorInfo> actors;
    std::lock_guard<std::mutex> lock(g_mutex);
    bool any = false;
    for (Slot &s : g_slots)
    {
        // Still wanted? Same player, in our room, and the object still exists.
        const Target *t = nullptr;
        for (const Target &tg : targets)
            if (s.active && tg.playerId == s.playerId)
                t = &tg;
        bool keep = g_enabled && t && t->world == st.now.world && t->room == st.now.room;
        if (keep)
        {
            if (actors.empty())
                actors = kh2state::actors(rdram);
            keep = false;
            for (const kh2state::ActorInfo &a : actors)
                if (a.address == s.object)
                    keep = true;
        }
        if (!keep)
        {
            if (s.active)
                std::printf("[puppets] player %u releases %s\n", s.playerId + 1, s.name);
            s = Slot{};
            continue;
        }
        std::memcpy(s.pos, t->pos, sizeof(s.pos));
        s.yaw = t->yaw;
        any = true;
    }
    // New players in our room borrow the first free usable NPC.
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
            if (actors.empty())
                actors = kh2state::actors(rdram);
            for (const kh2state::ActorInfo &a : actors)
            {
                if (!usable(a))
                    continue;
                bool taken = false;
                for (const Slot &s : g_slots)
                    taken = taken || (s.active && s.object == a.address);
                if (taken)
                    continue;
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
