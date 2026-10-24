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
                   kDmgAttack2 = 0x58, kDmgAttack3 = 0x5C;
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
bool g_log = false;
int g_logged = 0;
std::atomic<bool> g_capture{false};
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

// Enemy or boss (objentry type 3 or 4)?
bool isEnemy(const uint8_t *rdram, uint32_t obj)
{
    const uint32_t rec = rd<uint32_t>(rdram, obj + kh2state::addr::ObjEntry);
    if (!inRam(rec))
        return false;
    const uint8_t type = rd<uint8_t>(rdram, rec + 4);
    return type == 3 || type == 4;
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
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_hits.size() < 64)
                    g_hits.push_back(h);
            }
        }
    }
    kh2::callOriginal(g_execDamage, rdram, ctx, runtime);
}

void put32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01FFFFFFu), &v, 4); }
} // namespace

bool install(PS2Runtime &runtime)
{
    const char *e = std::getenv("KH2_DAMAGE_LOG");
    g_log = e && *e == '1';
    g_execDamage = runtime.lookupFunction(kExecDamage);
    if (!g_execDamage || !runtime.replaceFunction(kExecDamage, &execDamageHook))
        return false;
    if (g_log)
        std::printf("[hits] logging damage\n");
    return true;
}

void setCapture(bool on)
{
    if (g_capture.exchange(on) && !on)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hits.clear();
    }
}

std::vector<Hit> takeHits()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Hit> out;
    out.swap(g_hits);
    return out;
}

void replay(uint32_t enemy, uint32_t attacker, const uint8_t (&damage)[kDamageSize],
            const uint8_t (&param)[kParamSize])
{
    struct Copy
    {
        uint8_t damage[kDamageSize];
        uint8_t param[kParamSize];
    };
    auto c = std::make_shared<Copy>();
    std::memcpy(c->damage, damage, kDamageSize);
    std::memcpy(c->param, param, kParamSize);
    kh2guest::post([enemy, attacker, c](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
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
        const uint32_t flags = (rd<uint32_t>(ram, dmg + kDmgFlags) & ~kFlagHandled) | kFlagHpTaken;
        put32(ram, dmg + kDmgFlags, flags);
        put32(ram, dmg + kDmgAttacker, attacker);
        put32(ram, dmg + kDmgTarget, enemy);
        put32(ram, dmg + kDmgAttack, atk);
        put32(ram, dmg + kDmgAttack2, atk);
        put32(ram, dmg + kDmgAttack3, atk);
        put32(ram, dmg + kDmgParam, prm);
        if (g_log)
        {
            std::printf("[hits] replaying a client's hit on %08X (reaction %02X/%02X, by %08X)\n", enemy,
                        c->damage[0x26], c->damage[0x27], attacker);
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
