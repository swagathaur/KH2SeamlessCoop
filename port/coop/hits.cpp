#include "hits.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "game_state.h"
#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2hits
{
namespace
{
constexpr uint32_t kExecDamage = 0x00186148; // YS::BTLOBJ::_OVR_exec_damage(DAMAGE*)
constexpr uint32_t kTakeHit = 0x001B3D00;    // (obj, DAMAGE*): HP, attacker, reaction, script, exec_damage

// DAMAGE fields.
constexpr uint32_t kDmgAttacker = 0x10, kDmgTarget = 0x14, kDmgFlags = 0x18, kDmgAttack = 0x1C, kDmgParam = 0x20,
                   kDmgAttack2 = 0x58, kDmgAttack3 = 0x5C, kDmgAmount = 0x28;
constexpr uint32_t kFlagHandled = 1, kFlagHpTaken = 2;
// The stand-in ATTACK: +0x08 attacker, +0x0C target (YS::trap_attack_new
// allocates 0x140 bytes for one).
constexpr uint32_t kAttackSize = 0x140, kAttackAttacker = 0x08, kAttackTarget = 0x0C;

// Replay buffers, in guest memory nothing else uses (0x01E4C868-0x01E50000;
// the puppets' objentry table is at 0x01E4D000). A few, used in turn, in case
// the game still looks at the last one for a frame.
constexpr uint32_t kReplayBase = 0x01E4D200u;
constexpr uint32_t kReplayStride = 0x240; // DAMAGE 0x60 + parameters 0x80 + ATTACK 0x140
constexpr uint32_t kReplaySlots = 4;

PS2Runtime::RecompiledFunction g_execDamage = nullptr;
PS2Runtime::RecompiledFunction g_takeHit = nullptr;
std::mutex g_blockedMutex;
std::vector<uint32_t> g_blocked;
std::vector<uint32_t> g_protected;
std::atomic<int> g_blockedLogs{0};
bool g_log = false;
int g_logged = 0;
std::atomic<bool> g_capture{false};
std::atomic<bool> g_captureKills{false};
std::vector<Hit> g_kills; // captured, not taken yet (under g_mutex)
std::mutex g_mutex;
std::vector<Hit> g_hits; // captured, not taken yet
uint32_t g_replaySlot = 0; // EE thread only

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }
void put32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01FFFFFFu), &v, 4); }

// FUN_001B3D00(obj, DAMAGE*): a hit (HP, attacker, reaction, script).
void takeHitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4), dmg = getRegU32(ctx, 5);
    const bool replayed = dmg >= kReplayBase && dmg < kReplayBase + kReplaySlots * kReplayStride;
    if (ctx->pc == kTakeHit && !replayed && inRam(dmg))
    {
        uint32_t by = rd<uint32_t>(rdram, dmg + kDmgAttacker), byAttack = 0;
        const uint32_t attack = rd<uint32_t>(rdram, dmg + kDmgAttack);
        if (inRam(attack))
            byAttack = rd<uint32_t>(rdram, attack + kAttackAttacker);
        bool blocked = false;
        {
            std::lock_guard<std::mutex> lock(g_blockedMutex);
            for (uint32_t b : g_blocked)
                blocked = blocked || (b && (b == by || b == byAttack));
            for (uint32_t b : g_protected)
                blocked = blocked || b == obj;
        }
        if (blocked)
        {
            if (g_blockedLogs.fetch_add(1) < 20)
            {
                std::printf("[hits] a stand-in's hit on %08X does nothing here (its player's game deals it)\n", obj);
                std::fflush(stdout);
            }
            setReturnU32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    kh2::callOriginal(g_takeHit, rdram, ctx, runtime);
}

// Enemy or boss (objentry type 3 or 4)?
bool isEnemy(const uint8_t *rdram, uint32_t obj)
{
    const uint32_t rec = rd<uint32_t>(rdram, obj + kh2state::addr::ObjEntry);
    if (!inRam(rec))
        return false;
    const uint8_t type = rd<uint8_t>(rdram, rec + 4);
    return type == 3 || type == 4 || type == 12; // boss, enemy, giant boss
}

void execDamageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4), dmg = getRegU32(ctx, 5);
    if (inRam(obj) && inRam(dmg))
    {
        if (g_log && g_logged < 400)
        {
            ++g_logged;
            uint32_t entry = 0;
            const uint32_t rec = rd<uint32_t>(rdram, obj + 8);
            if (inRam(rec))
                entry = rd<uint32_t>(rdram, rec);
            std::printf("[hits] obj %08X (%04X) damage %08X:", obj, entry & 0xFFFF, dmg);
            for (uint32_t i = 0; i < kDamageSize; i += 4)
                std::printf(" %08X", rd<uint32_t>(rdram, dmg + i));
            std::printf("\n");
            std::fflush(stdout);
        }
        // (Host) the blow that killed one of our enemies: copy it for the
        // clients, whose copies then die of it too. exec_damage comes after
        // the HP was taken.
        if (g_captureKills.load(std::memory_order_relaxed) && isEnemy(rdram, obj))
        {
            const uint32_t sheet = rd<uint32_t>(rdram, obj + 0x48C);
            const uint32_t param = rd<uint32_t>(rdram, dmg + kDmgParam);
            if (inRam(sheet) && rd<int32_t>(rdram, sheet) <= 0 && inRam(param))
            {
                Hit h;
                h.object = obj;
                h.attacker = rd<uint32_t>(rdram, dmg + kDmgAttacker);
                std::memcpy(h.damage, rdram + (dmg & 0x01FFFFFFu), kDamageSize);
                std::memcpy(h.param, rdram + (param & 0x01FFFFFFu), kParamSize);
                std::lock_guard<std::mutex> lock(g_mutex);
                bool again = false; // (more hits on a body already at 0)
                for (const Hit &k : g_kills)
                    again = again || k.object == obj;
                if (!again && g_kills.size() < 64)
                    g_kills.push_back(h);
            }
        }
        // Our player hit an enemy: copy it for the host.
        if (g_capture.load(std::memory_order_relaxed))
        {
            const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
            const uint32_t param = rd<uint32_t>(rdram, dmg + kDmgParam);
            if (player && rd<uint32_t>(rdram, dmg + kDmgAttacker) == player && isEnemy(rdram, obj) && inRam(param))
            {
                Hit h;
                h.object = obj;
                std::memcpy(h.damage, rdram + (dmg & 0x01FFFFFFu), kDamageSize);
                std::memcpy(h.param, rdram + (param & 0x01FFFFFFu), kParamSize);
                // Our blow killed our copy: its AI back now, this frame, so it
                // dies the game's way as in a game of one (frozen, the body
                // stood there; enemies.cpp leaves it alone from now on).
                const uint32_t sheet = rd<uint32_t>(rdram, obj + 0x48C);
                if (inRam(sheet) && rd<int32_t>(rdram, sheet) <= 0)
                {
                    h.lethal = true;
                    const uint32_t flags = rd<uint32_t>(rdram, obj + 0x10C);
                    put32(rdram, obj + 0x10C, flags & ~0x200000u);
                }
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_hits.size() < 64)
                    g_hits.push_back(h);
            }
        }
    }
    kh2::callOriginal(g_execDamage, rdram, ctx, runtime);
}

} // namespace

bool install(PS2Runtime &runtime)
{
    const char *e = std::getenv("KH2_DAMAGE_LOG");
    g_log = e && *e == '1';
    g_execDamage = runtime.lookupFunction(kExecDamage);
    if (!g_execDamage || !runtime.replaceFunction(kExecDamage, &execDamageHook))
        return false;
    g_takeHit = runtime.lookupFunction(kTakeHit);
    if (!g_takeHit || !runtime.replaceFunction(kTakeHit, &takeHitHook))
        g_takeHit = nullptr;
    if (g_log)
        std::printf("[hits] logging damage\n");
    return true;
}

void setBlockedAttackers(std::vector<uint32_t> objects)
{
    std::lock_guard<std::mutex> lock(g_blockedMutex);
    g_blocked = std::move(objects);
}

void setProtectedTargets(std::vector<uint32_t> objects)
{
    std::lock_guard<std::mutex> lock(g_blockedMutex);
    g_protected = std::move(objects);
}

void setCapture(bool on)
{
    if (g_capture.exchange(on) && !on)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hits.clear();
    }
}

void setKillCapture(bool on)
{
    if (g_captureKills.exchange(on) && !on)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_kills.clear();
    }
}

std::vector<Hit> takeKills()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Hit> out;
    out.swap(g_kills);
    return out;
}

std::vector<Hit> takeHits()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Hit> out;
    out.swap(g_hits);
    return out;
}

void replay(uint32_t enemy, uint32_t attacker, const uint8_t (&damage)[kDamageSize],
            const uint8_t (&param)[kParamSize], bool lethal)
{
    struct Copy
    {
        uint8_t damage[kDamageSize];
        uint8_t param[kParamSize];
    };
    auto c = std::make_shared<Copy>();
    std::memcpy(c->damage, damage, kDamageSize);
    std::memcpy(c->param, param, kParamSize);
    kh2guest::post([enemy, attacker, c, lethal](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        uint8_t *ram = env.rdram;
        if (!first)
        {
            if (g_log)
            {
                std::printf("[hits] replayed: %08X now plays motion %u\n", enemy,
                            rd<uint32_t>(ram, enemy + kh2state::addr::ObjMotionIndex));
                std::fflush(stdout);
            }
            return std::nullopt;
        }
        // Still there and alive?
        bool live = false;
        for (const kh2state::ActorInfo &a : kh2state::actors(ram))
            live = live || a.address == enemy;
        const uint32_t sheet = live ? rd<uint32_t>(ram, enemy + 0x48C) : 0;
        if (!live || !inRam(sheet) || rd<int32_t>(ram, sheet) <= 0 || !inRam(rd<uint32_t>(ram, enemy)))
            return std::nullopt;
        const uint32_t base = kReplayBase + (g_replaySlot++ % kReplaySlots) * kReplayStride;
        const uint32_t dmg = base, prm = base + kDamageSize, atk = base + kDamageSize + kParamSize;
        std::memset(env.host(atk), 0, kAttackSize);
        put32(ram, atk + kAttackAttacker, attacker);
        put32(ram, atk + kAttackTarget, enemy);
        std::memcpy(env.host(prm), c->param, kParamSize);
        std::memcpy(env.host(dmg), c->damage, kDamageSize);
        uint32_t flags = (rd<uint32_t>(ram, dmg + kDmgFlags) & ~kFlagHandled) | kFlagHpTaken;
        if (lethal)
        {
            // The game takes the HP: all there is (the host's blow may have
            // been smaller than what our copy has left).
            flags &= ~kFlagHpTaken;
            const int32_t hp = rd<int32_t>(ram, sheet);
            if (rd<int32_t>(ram, dmg + kDmgAmount) < hp)
                put32(ram, dmg + kDmgAmount, static_cast<uint32_t>(hp));
        }
        put32(ram, dmg + kDmgFlags, flags);
        put32(ram, dmg + kDmgAttacker, attacker);
        put32(ram, dmg + kDmgTarget, enemy);
        put32(ram, dmg + kDmgAttack, atk);
        put32(ram, dmg + kDmgAttack2, atk);
        put32(ram, dmg + kDmgAttack3, atk);
        put32(ram, dmg + kDmgParam, prm);
        if (g_log)
        {
            std::printf("[hits] replaying %s on %08X (reaction %02X/%02X, by %08X)\n",
                        lethal ? "the host's killing blow" : "a client's hit", enemy, c->damage[0x26],
                        c->damage[0x27], attacker);
            std::fflush(stdout);
        }
        return kh2guest::Call{kTakeHit, {enemy, dmg}, {}};
    });
}

std::vector<uint8_t> encode(uint8_t world, uint8_t room, int16_t serial, uint16_t entry, const Hit &hit)
{
    std::vector<uint8_t> out;
    out.reserve(6 + kDamageSize + kParamSize);
    out.push_back(world);
    out.push_back(room);
    const uint16_t s = static_cast<uint16_t>(serial);
    out.push_back(static_cast<uint8_t>(s & 0xFF));
    out.push_back(static_cast<uint8_t>(s >> 8));
    out.push_back(static_cast<uint8_t>(entry & 0xFF));
    out.push_back(static_cast<uint8_t>(entry >> 8));
    out.insert(out.end(), hit.damage, hit.damage + kDamageSize);
    out.insert(out.end(), hit.param, hit.param + kParamSize);
    return out;
}

bool decode(const std::vector<uint8_t> &data, uint8_t &world, uint8_t &room, int16_t &serial, uint16_t &entry,
            Hit &hit)
{
    if (data.size() != 6 + kDamageSize + kParamSize)
        return false;
    world = data[0];
    room = data[1];
    serial = static_cast<int16_t>(data[2] | (data[3] << 8));
    entry = static_cast<uint16_t>(data[4] | (data[5] << 8));
    std::memcpy(hit.damage, data.data() + 6, kDamageSize);
    std::memcpy(hit.param, data.data() + 6 + kDamageSize, kParamSize);
    return true;
}
} // namespace kh2hits
