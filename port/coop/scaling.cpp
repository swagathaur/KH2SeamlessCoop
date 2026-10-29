#include "scaling.h"
#include "avatars.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "game_state.h"
#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2scale
{
namespace
{
constexpr uint32_t kTakeHp = 0x00186D48; // (obj, DAMAGE*): HP off obj by DAMAGE+0x28 (obj->add_hp)
constexpr uint32_t kDmgAttacker = 0x10, kDmgFlags = 0x18, kDmgAmount = 0x28;
constexpr uint32_t kFlagHpTaken = 2;
constexpr uint32_t kObjSheet = 0x48C; // YS::SHEET*: HP +0, max HP +4

std::atomic<float> g_localHp{kDefaultHp}, g_localDamage{kDefaultDamage};
std::atomic<float> g_hostHp{-1.0f}, g_hostDamage{-1.0f}; // from the host (client); < 0: none yet
std::atomic<int> g_players{1};
std::atomic<int> g_hostPlayers{0};
std::atomic<bool> g_client{false};
PS2Runtime::RecompiledFunction g_takeHp = nullptr;

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }

uint8_t typeOf(const uint8_t *rdram, uint32_t obj)
{
    if (!inRam(obj))
        return 0xFF;
    const uint32_t rec = rd<uint32_t>(rdram, obj + kh2state::addr::ObjEntry);
    return inRam(rec) ? rd<uint8_t>(rdram, rec + 4) : 0xFF;
}
bool isEnemyType(uint8_t t) { return t == 3 || t == 4 || t == 12; } // boss, enemy, giant boss

float damageFactor()
{
    const int n = players();
    return n > 1 ? 1.0f + damagePerPlayer() * float(n - 1) : 1.0f;
}

// FUN_00186D48 override (EE thread): an enemy's hit on a player or party member
// takes more HP.
void takeHpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4), dmg = getRegU32(ctx, 5);
    const float f = damageFactor();
    if (f != 1.0f && inRam(dmg) && !(rd<uint32_t>(rdram, dmg + kDmgFlags) & kFlagHpTaken))
    {
        const uint8_t target = typeOf(rdram, obj);
        if ((target == 0 || target == 1) && isEnemyType(typeOf(rdram, rd<uint32_t>(rdram, dmg + kDmgAttacker))))
        {
            const int32_t amount = rd<int32_t>(rdram, dmg + kDmgAmount);
            if (amount > 0)
            {
                // The fraction carries over to the next hit (EE thread only), so
                // small hits (1 HP early on) also come out f times harder on average.
                static double carry = 0.0;
                const double exact = double(amount) * f + carry;
                const int32_t scaled = std::max<int32_t>(amount, int32_t(std::floor(exact)));
                carry = std::min(0.999, std::max(0.0, exact - double(scaled)));
                wr<int32_t>(rdram, dmg + kDmgAmount, scaled);
                static int logged = 0;
                if (logged < 100)
                {
                    ++logged;
                    std::printf("[scale] an enemy's hit on %08X: %d -> %d HP\n", obj, amount, scaled);
                    std::fflush(stdout);
                }
            }
        }
    }
    kh2::callOriginal(g_takeHp, rdram, ctx, runtime);
}

// Host: enemies this room: their own max HP and what we made it.
struct Scaled
{
    int32_t baseMax = 0, scaledMax = 0;
};
std::map<uint32_t, Scaled> g_scaled;
uint16_t g_room = 0xFFFF;
} // namespace

bool install(PS2Runtime &runtime)
{
    g_takeHp = runtime.lookupFunction(kTakeHp);
    if (!g_takeHp || !runtime.replaceFunction(kTakeHp, &takeHpHook))
    {
        std::fprintf(stderr, "[scale] could not hook 0x%08X\n", kTakeHp);
        g_takeHp = nullptr;
        return false;
    }
    return true;
}

void setLocal(float hp, float damage)
{
    g_localHp = std::max(0.0f, std::min(hp, 4.0f));
    g_localDamage = std::max(0.0f, std::min(damage, 4.0f));
}
float localHp() { return g_localHp; }
float localDamage() { return g_localDamage; }
float hpPerPlayer()
{
    const float h = g_hostHp;
    return g_client && h >= 0 ? h : float(g_localHp);
}
float damagePerPlayer()
{
    const float d = g_hostDamage;
    return g_client && d >= 0 ? d : float(g_localDamage);
}
int players()
{
    const int hp = g_hostPlayers;
    return g_client && hp > 0 ? hp : int(g_players);
}

void tick(uint8_t *rdram, bool inSession, bool host, int count)
{
    g_players = inSession ? std::max(1, count) : 1;
    g_client = inSession && !host;
    if (!inSession)
    {
        g_hostHp = -1.0f;
        g_hostDamage = -1.0f;
        g_hostPlayers = 0;
    }
    if (!host)
    {
        g_scaled.clear();
        return;
    }
    const kh2state::State st = kh2state::read(rdram);
    const uint16_t room = uint16_t(st.now.world) << 8 | st.now.room;
    if (room != g_room)
    {
        g_room = room;
        g_scaled.clear();
    }
    const int n = players();
    const float f = n > 1 ? 1.0f + hpPerPlayer() * float(n - 1) : 1.0f;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        if (!isEnemyType(a.type))
            continue;
        const uint32_t sheet = rd<uint32_t>(rdram, a.address + kObjSheet);
        if (!inRam(sheet))
            continue;
        // Never our sheet or an avatar's (avatars.h: a party slot). Enemies'
        // sheets come from the same pool as the party slots, so only those.
        if (sheet == kh2state::addr::Slot1 || kh2avatars::isAvatarSheet(sheet))
        {
            static int skipped = 0;
            if (skipped++ < 5)
            {
                std::printf("[scale] %s %08X (type %u) has a party sheet (%08X): left alone\n", a.name, a.address,
                            unsigned(a.type), sheet);
                std::fflush(stdout);
            }
            continue;
        }
        const int32_t hp = rd<int32_t>(rdram, sheet), max = rd<int32_t>(rdram, sheet + 4);
        if (hp <= 0 || max <= 0)
            continue;
        // A new enemy (or another at this address): its own max is the base.
        auto it = g_scaled.find(a.address);
        if (it == g_scaled.end() || it->second.scaledMax != max)
            it = g_scaled.insert_or_assign(a.address, Scaled{max, max}).first;
        const int32_t want = std::max<int32_t>(1, int32_t(std::lround(double(it->second.baseMax) * f)));
        if (want == max)
            continue;
        // Scaled up (or back down when the number of players changed): HP keeps its share.
        const int32_t newHp =
            std::max<int32_t>(1, std::min(want, int32_t(std::lround(double(hp) * double(want) / double(max)))));
        wr<int32_t>(rdram, sheet + 4, want);
        wr<int32_t>(rdram, sheet, newHp);
        it->second.scaledMax = want;
        static int logged = 0;
        if (logged < 200)
        {
            ++logged;
            std::printf("[scale] %s %08X: HP %d/%d -> %d/%d (%d players)\n", a.name, a.address, hp, max, newHp, want, n);
            std::fflush(stdout);
        }
    }
}

std::vector<uint8_t> encode()
{
    std::vector<uint8_t> out(12);
    const float h = g_localHp, d = g_localDamage;
    const int32_t n = g_players;
    std::memcpy(out.data(), &h, 4);
    std::memcpy(out.data() + 4, &d, 4);
    std::memcpy(out.data() + 8, &n, 4);
    return out;
}

void onHostSettings(const std::vector<uint8_t> &data)
{
    if (data.size() != 12)
        return;
    float h, d;
    int32_t n;
    std::memcpy(&h, data.data(), 4);
    std::memcpy(&d, data.data() + 4, 4);
    std::memcpy(&n, data.data() + 8, 4);
    if (!(h >= 0.0f && h <= 4.0f) || !(d >= 0.0f && d <= 4.0f) || n < 1 || n > 8)
        return;
    if (h != g_hostHp || d != g_hostDamage || n != g_hostPlayers)
    {
        std::printf("[scale] the host's scaling: enemy HP +%.0f%%, damage +%.0f%% per extra player (%d players)\n",
                    h * 100.0f, d * 100.0f, int(n));
        std::fflush(stdout);
    }
    g_hostHp = h;
    g_hostDamage = d;
    g_hostPlayers = n;
}

void leftSession()
{
    g_hostHp = -1.0f;
    g_hostDamage = -1.0f;
    g_hostPlayers = 0;
    g_client = false;
    g_players = 1;
}
} // namespace kh2scale
