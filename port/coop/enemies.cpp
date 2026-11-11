#include "enemies.h"
#include "hits.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>

#include "game_state.h"
#include "formcache.h"
#include "guest_call.h"
#include "partners.h"
#include "puppets.h"
#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2enemies
{
namespace
{
constexpr uint32_t kObjSheet = 0x48C;      // YS::SHEET*
constexpr uint32_t kObjSpawnRecord = 0x81C; // UNIT_DATA_OBJ* (the room's spawn entry)
constexpr uint32_t kRecordSerial = 0x1E;
constexpr uint32_t kVtableAddHp = 0x78;   // (obj, amount, sheet index, flag)
constexpr uint32_t kObjFlags = 0x10C;      // 0x200000: may not act (its AI does nothing)
constexpr uint32_t kNoAct = 0x200000u;
constexpr uint32_t kMotionChange = 0x0017F540; // YS::MOTION::change(MOTION*, id, blend, start)

// Clients' copies of the host's enemies are puppets: their own AI is off and
// they play the host's motions (KH2_ENEMY_AI=1 keeps their AI, as before).
bool puppetEnemies()
{
    static const bool keepAi = [] {
        const char *e = std::getenv("KH2_ENEMY_AI");
        return e && *e == '1';
    }();
    return !keepAi;
}

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }

std::mutex g_mutex;
kh2net::EnemyStates g_host; // latest from the host
double g_hostTime = -1;
struct Follow
{
    float shown[3] = {0, 0, 0};
    float yaw = 0;
    bool placed = false;
    int32_t expectedHp = -1; // what our copy should have, as far as we know
    bool killed = false;
    uint32_t object = 0;     // our copy
    uint16_t entry = 0;      // its objentry id
    bool frozen = false;     // we turned its AI off
    uint32_t flagsBefore = 0;
    uint16_t motion = 0xFFFF; // the host's motion we last started on it
    int sinceMotion = 0;
    double holdUntil = 0;    // our player just hit it: let its own reaction play
    bool holding = false;
    double killedAt = -1;    // when we finished it off (its body should go soon after)
    bool removed = false;    // we had to take the body away ourselves
    double firstSeen = -1;   // when our game made it (one the host never has goes)
    double hostDeadAt = -1;  // when the host's first said 0 HP (its killing blow should follow)
    bool ownKill = false;    // our player's blow killed it (hits.cpp gave it its AI back)
};
std::map<int16_t, Follow> g_follow;   // by serial, this room
std::map<uint32_t, double> g_killed;  // objects we finished off this room (when), whatever their serial
// The host's enemies this room has had (serial -> entry): one gone from the
// host died there, and our game's own of that serial (made when we walked
// into the same trigger later) dies too, rather than being fought alone.
std::map<int16_t, uint16_t> g_hostSeen;
// Copies we make of host enemies our game has not made (a wave the host set
// off): by serial.
struct Made
{
    double askedAt = 0;   // its files were requested
    bool posted = false;  // the spawn job is queued
    double postedAt = 0;
    uint32_t object = 0;  // what it made (0: not yet / failed)
};
std::map<int16_t, Made> g_made;
std::map<uint16_t, double> g_preloaded; // entry -> when its files were requested, this room
std::set<uint16_t> g_cached; // entries read into kh2formcache's batches instead (our player in a form), this room
std::vector<std::pair<uint32_t, uint32_t>> g_thaw; // (object, flags before) to give their AI back
uint16_t g_followRoom = 0xFFFF;
double g_sharedSince = -1; // when the host and we were first both in this room (clientTick)

// A client's own enemies: once the host and we have shared a room for a few
// seconds (its spawn list has run on both), the room's later spawns of
// enemies and bosses (waves, a mission's next group) are the host's to make;
// ours come as copies of theirs (makeMissing). FUN_00199890(record, group,
// pos), the step under YS::UNIT_DATA_OBJ::appear that calls the object
// factory, then makes nothing (its callers take a null object).
constexpr uint32_t kSpawnUnit = 0x00199890;
PS2Runtime::RecompiledFunction g_spawnUnit = nullptr;
std::atomic<bool> g_hostSpawns{false};

void spawnUnitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kSpawnUnit && g_hostSpawns.load(std::memory_order_relaxed))
    {
        const uint32_t record = getRegU32(ctx, 4);
        const uint32_t entry = inRam(record) ? (rd<uint32_t>(rdram, record) & 0xFFFF) : 0;
        uint8_t type = 0xFF;
        const uint32_t table = rd<uint32_t>(rdram, kh2state::addr::Obj0Ptr);
        if (entry && inRam(table))
        {
            const uint32_t count = rd<uint32_t>(rdram, table + 4);
            for (uint32_t i = 0; i < count && i < 4096; ++i)
                if ((rd<uint32_t>(rdram, table + 8 + i * 0x60) & 0xFFFF) == entry)
                {
                    type = rd<uint8_t>(rdram, table + 8 + i * 0x60 + 4);
                    break;
                }
        }
        if (type == 3 || type == 4 || type == 12 || type == 21) // boss, enemy, giant boss, large enemy (as scan())
        {
            static double lastLog = -10;
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (t - lastLog > 2.0)
            {
                lastLog = t;
                std::printf("[enemies] our game would make %04X #%d: the host's game makes the enemies\n", entry,
                            rd<int16_t>(rdram, record + kRecordSerial));
                std::fflush(stdout);
            }
            setReturnU32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    kh2::callOriginal(g_spawnUnit, rdram, ctx, runtime);
}
size_t g_lastSentCount = SIZE_MAX, g_lastFollowCount = SIZE_MAX; // for the log

double nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// An object's status bits: (+0xABC | +0xAC0) & ~+0xAC4 (set, set for now, cleared).
constexpr uint32_t kStatus = 0xABC;
uint32_t statusBits(const uint8_t *rdram, uint32_t obj)
{
    return (rd<uint32_t>(rdram, obj + kStatus) | rd<uint32_t>(rdram, obj + kStatus + 4)) &
           ~rd<uint32_t>(rdram, obj + kStatus + 8);
}

// The survive check (enemies.h kRule*) for a party member's hit on HP.
bool survives(uint32_t status, bool finisher)
{
    if (status & kRuleNever)
        return true;
    if ((status & kRuleFinisher) && !finisher)
        return true; // (HP is gauge 0: the 0x80000 case doesn't matter here)
    return false;    // (0x800: the party may; a client's hit is the party's)
}

// Last client hit per enemy: a finisher? (for the HP of it, which follows)
struct ClientHit
{
    bool skipsRules = false, finisher = false;
    double at = -100.0;
};
std::map<int16_t, ClientHit> g_clientHits;
// A client's HP that the rules stopped short of a kill, by enemy: its hit's
// own message can come just after the HP (they are taken on two threads),
// and if that hit was a finisher, the kill goes ahead then.
struct Blocked
{
    uint32_t object = 0;
    double at = -100.0;
};
std::map<int16_t, Blocked> g_blocked;
std::mutex g_rulesMutex; // g_clientHits, g_blocked

// Guest job: obj->add_hp(amount) (negative = damage; at 0 the object dies
// the game's own way).
// finisher >= 0: a client's hit (0 or 1: was it a finisher?), under the
// game's survive rules, as the game's own hit step would have applied them
// (finisher 2: an attack type that skips them).
void postAddHp(uint32_t obj, int32_t amount, int finisher = -1, int16_t serial = 0)
{
    kh2guest::post([obj, amount, finisher, serial](const kh2guest::Env &env, uint32_t, bool first) mutable -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        // Still a live enemy? (It may have gone since this was queued.)
        bool alive = false;
        for (const Local &e : scan(env.rdram))
            alive = alive || e.object == obj;
        if (!alive)
            return std::nullopt;
        const uint32_t vt = rd<uint32_t>(env.rdram, obj);
        if (!inRam(vt))
            return std::nullopt;
        const uint32_t fn = rd<uint32_t>(env.rdram, vt + kVtableAddHp);
        if (!fn)
            return std::nullopt;
        if (finisher >= 0 && amount < 0)
        {
            const uint32_t sheet = rd<uint32_t>(env.rdram, obj + 0x48C);
            if (inRam(sheet))
            {
                const int32_t hp = rd<int32_t>(env.rdram, sheet), floor = rd<int32_t>(env.rdram, sheet + 8);
                const uint32_t status = statusBits(env.rdram, obj);
                // (The hit's own message may have come since this was queued;
                // checked and the held-back kill noted under one lock, as the
                // message's side looks for it.)
                bool held = false;
                int32_t keep = 0;
                {
                    std::lock_guard<std::mutex> lock(g_rulesMutex);
                    if (finisher == 0 && serial)
                    {
                        const auto f = g_clientHits.find(serial);
                        if (f != g_clientHits.end() && nowSeconds() - f->second.at < 0.5)
                            finisher = f->second.skipsRules ? 2 : f->second.finisher ? 1 : 0;
                    }
                    if (hp + amount <= floor && finisher != 2 && survives(status, finisher != 0))
                    {
                        held = true;
                        keep = std::max(hp - floor - 1, 0);
                        if (serial)
                            g_blocked[serial] = {obj, nowSeconds()};
                    }
                }
                if (held)
                {
                    std::printf("[enemies] a client's hit of %d leaves %08X at %d HP (its rules %03X%s)\n", -amount,
                                obj, hp - keep, status & kRuleMask, finisher == 1 ? ", a finisher" : "");
                    std::fflush(stdout);
                    amount = -keep;
                    if (amount == 0)
                        return std::nullopt;
                }
            }
        }
        return kh2guest::Call{fn, {obj, static_cast<uint32_t>(amount), 0u, 0u}, {}};
    });
}
// Give the enemies we froze their AI back (host data gone or stale).
void thawAll(uint8_t *rdram)
{
    std::vector<uint32_t> live;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        live.push_back(a.address);
    for (auto &[serial, f] : g_follow)
    {
        if (!f.frozen)
            continue;
        f.frozen = false;
        f.motion = 0xFFFF;
        bool there = false;
        for (uint32_t a : live)
            there = there || a == f.object;
        if (!there)
            continue;
        const uint32_t flags = rd<uint32_t>(rdram, f.object + kObjFlags);
        const uint32_t back = (flags & ~kNoAct) | (f.flagsBefore & kNoAct);
        std::memcpy(rdram + ((f.object + kObjFlags) & 0x01FFFFFFu), &back, 4);
    }
}

// Our copy dies: its AI back first (its own code plays the fall and the fade
// and removes the body; frozen, the body stayed), then add_hp to 0.
void killCopy(uint8_t *rdram, Follow &f, const Local &e, double now)
{
    if (f.frozen)
    {
        const uint32_t flags = rd<uint32_t>(rdram, e.object + kObjFlags);
        const uint32_t back = (flags & ~kNoAct) | (f.flagsBefore & kNoAct);
        std::memcpy(rdram + ((e.object + kObjFlags) & 0x01FFFFFFu), &back, 4);
        f.frozen = false;
    }
    f.killed = true;
    f.killedAt = now;
    g_killed[e.object] = now;
    postAddHp(e.object, -(e.hp > 0 ? e.hp : 0));
}

// A body still there a while after it died: take it away (its leave method,
// vtable +0x20, as trap_obj_leave_force does).
void removeBody(uint32_t obj)
{
    kh2guest::post([obj](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        bool live = false;
        for (const kh2state::ActorInfo &a : kh2state::actors(env.rdram))
            live = live || a.address == obj;
        if (!live)
            return std::nullopt;
        const uint32_t vt = rd<uint32_t>(env.rdram, obj);
        const uint32_t fn = inRam(vt) ? rd<uint32_t>(env.rdram, vt + 0x20) : 0;
        if (!inRam(fn))
            return std::nullopt;
        return kh2guest::Call{fn, {obj}, {}};
    });
}
} // namespace

namespace
{
uint32_t hostSpawnSeedImpl(uint8_t world, uint8_t room, bool &fresh);
// The room spawn script's step, FUN_00181DA0(this, code): code[0] is the
// op; op 2 (RandomSpawn) registers one of its code[1] groups by the game's
// random number at 0x003638D0 (x 0x10DCD, then / ((2^32 + n - 1) / n)),
// op 3 (CasualSpawn) its group when that number / 0x28F5C29 is below its
// percent (as found on the PC release by the Kingdom-Hearts-II-Coop-
// Multiplayer project, F430; the same code here). For those two ops the
// number is set from the room's shared seed and the draw's count, and the
// game's own put back after.
constexpr uint32_t kAreaOp = 0x00181DA0, kAreaRandom = 0x003638D0;
PS2Runtime::RecompiledFunction g_areaOp = nullptr;
std::atomic<bool> g_seedOn{false}, g_seedHost{false};
std::mutex g_seedMutex;
uint16_t g_seedKey = 0xFFFF;     // the room the seed below is for (world << 8 | room)
uint32_t g_seed = 0;
uint32_t g_drawSeed = 0;         // the seed the draws below were counted under (game thread)
uint32_t g_draws = 0;

uint32_t mixSeed(uint32_t seed, uint16_t key, uint32_t draw)
{
    uint32_t x = seed ^ (uint32_t(key) * 0x9E3779B1u) ^ (draw * 0x85EBCA77u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x | 1u;
}

void areaOpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t code = getRegU32(ctx, 5);
    const uint16_t op = inRam(code) ? rd<uint16_t>(rdram, code) : 0xFFFF;
    if (ctx->pc != kAreaOp || (op != 2 && op != 3) || !g_seedOn.load(std::memory_order_relaxed))
    {
        kh2::callOriginal(g_areaOp, rdram, ctx, runtime);
        return;
    }
    const kh2state::State st = kh2state::read(rdram);
    const uint16_t key = uint16_t(st.now.world << 8 | st.now.room);
    uint32_t seed = 0;
    bool have = false;
    if (g_seedHost.load(std::memory_order_relaxed))
    {
        bool fresh = false;
        seed = hostSpawnSeedImpl(st.now.world, st.now.room, fresh);
        have = true;
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_seedMutex);
        have = g_seedKey == key;
        seed = g_seed;
    }
    if (!have)
    {
        static int logged = 0;
        if (logged++ < 10)
        {
            std::printf("[enemies] a random spawn pick in %02X-%02X before the host's seed came: our own\n",
                        st.now.world, st.now.room);
            std::fflush(stdout);
        }
        kh2::callOriginal(g_areaOp, rdram, ctx, runtime);
        return;
    }
    if (seed != g_drawSeed)
    {
        g_drawSeed = seed; // (a new seed with each room entered: its draws count from 0)
        g_draws = 0;
    }
    const uint32_t own = rd<uint32_t>(rdram, kAreaRandom);
    const uint32_t draw = g_draws++;
    const uint32_t shared = mixSeed(seed, key, draw);
    wr<uint32_t>(rdram, kAreaRandom, shared);
    const uint32_t after = shared * 0x10DCDu; // (what the op's draw leaves there)
    if (kh2::callOriginalReturned(g_areaOp, rdram, ctx, runtime, kAreaOp) || rd<uint32_t>(rdram, kAreaRandom) == after)
        wr<uint32_t>(rdram, kAreaRandom, own);
    static int logged = 0;
    if (logged++ < 40)
    {
        // (What the op picks from it: op 2 group (after / ((2^32 + n - 1) / n)) of n,
        // op 3 the roll (after / 0x28F5C29) against its percent.)
        const uint64_t n = uint64_t(uint16_t(rd<int16_t>(rdram, code + 2)));
        const uint64_t pick = op == 2 && n ? uint64_t(after) / (((1ull << 32) + n - 1) / n) : uint64_t(after) / 0x28F5C29u;
        std::printf("[enemies] random spawn op %u in %02X-%02X, draw %u: %s %llu (of %llu) from the shared seed\n", op,
                    st.now.world, st.now.room, draw, op == 2 ? "group" : "roll", (unsigned long long)pick,
                    (unsigned long long)(op == 2 ? n : 100));
        std::fflush(stdout);
    }
}
} // namespace

uint32_t hostSpawnSeed(uint8_t world, uint8_t room, bool &fresh) { return hostSpawnSeedImpl(world, room, fresh); }

namespace
{
uint32_t hostSpawnSeedImpl(uint8_t world, uint8_t room, bool &fresh)
{
    std::lock_guard<std::mutex> lock(g_seedMutex);
    const uint16_t key = uint16_t(world << 8 | room);
    fresh = false;
    if (key != g_seedKey)
    {
        static std::mt19937 rng{std::random_device{}()};
        g_seedKey = key;
        g_seed = rng() | 1u;
        fresh = true;
    }
    return g_seed;
}
} // namespace

void onSpawnSeed(uint8_t world, uint8_t room, uint32_t seed)
{
    std::lock_guard<std::mutex> lock(g_seedMutex);
    g_seedKey = uint16_t(world << 8 | room);
    g_seed = seed;
    std::printf("[enemies] the host's spawn seed for %02X-%02X came\n", world, room);
    std::fflush(stdout);
}

void setSpawnSeeding(bool on, bool host)
{
    g_seedHost = host;
    g_seedOn = on;
    if (!on)
    {
        std::lock_guard<std::mutex> lock(g_seedMutex);
        g_seedKey = 0xFFFF;
    }
}

bool install(PS2Runtime &runtime)
{
    g_areaOp = runtime.lookupFunction(kAreaOp);
    if (!g_areaOp || !runtime.replaceFunction(kAreaOp, &areaOpHook))
    {
        std::fprintf(stderr, "[enemies] could not hook the room spawn script (0x%08X)\n", kAreaOp);
        g_areaOp = nullptr;
    }
    g_spawnUnit = runtime.lookupFunction(kSpawnUnit);
    if (!g_spawnUnit || !runtime.replaceFunction(kSpawnUnit, &spawnUnitHook))
    {
        std::fprintf(stderr, "[enemies] could not hook 0x%08X\n", kSpawnUnit);
        g_spawnUnit = nullptr;
        return false;
    }
    return true;
}

std::vector<Local> scan(const uint8_t *rdram)
{
    std::vector<Local> out;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        if (a.type != 3 && a.type != 4 && a.type != 12 && a.type != 21) // boss, enemy, giant boss (Twilight Thorn), large enemy (Assault Rider, Large Body)
            continue;
        const uint32_t rec = rd<uint32_t>(rdram, a.address + kObjSpawnRecord);
        const uint32_t sheet = rd<uint32_t>(rdram, a.address + kObjSheet);
        if (!inRam(rec) || !inRam(sheet))
            continue;
        Local e;
        e.object = a.address;
        e.serial = rd<int16_t>(rdram, rec + kRecordSerial);
        e.entry = static_cast<uint16_t>(a.entryId);
        std::memcpy(e.pos, a.pos, sizeof(e.pos));
        e.yaw = rd<float>(rdram, a.address + kh2state::addr::ObjDir + 12);
        e.motion = static_cast<uint16_t>(rd<uint32_t>(rdram, a.address + kh2state::addr::ObjMotionIndex));
        e.sheet = sheet;
        e.hp = rd<int32_t>(rdram, sheet);
        e.maxHp = rd<int32_t>(rdram, sheet + 4);
        e.minHp = rd<int32_t>(rdram, sheet + 8);
        const uint32_t status = statusBits(rdram, a.address);
        e.rules = uint16_t((status & kRuleMask) | ((status & 0x80000u) ? kRuleAnyChannel : 0));
        out.push_back(e);
    }
    return out;
}

kh2net::EnemyStates snapshot(const uint8_t *rdram, uint8_t world, uint8_t room, uint32_t tick)
{
    kh2net::EnemyStates s;
    s.tick = tick;
    s.world = world;
    s.room = room;
    for (const Local &e : scan(rdram))
    {
        if (s.enemies.size() >= kh2net::kMaxEnemies)
            break;
        kh2net::EnemyState es;
        es.serial = e.serial;
        es.entry = e.entry;
        std::memcpy(es.pos, e.pos, sizeof(es.pos));
        es.yaw = e.yaw;
        es.motion = e.motion;
        es.hp = e.hp;
        es.maxHp = e.maxHp;
        es.minHp = e.minHp;
        es.rules = e.rules;
        s.enemies.push_back(es);
    }
    if (s.enemies.size() != g_lastSentCount)
    {
        g_lastSentCount = s.enemies.size();
        std::printf("[enemies] host: %zu enemies in %02X-%02X\n", s.enemies.size(), world, room);
        std::fflush(stdout);
    }
    return s;
}

void applyDamage(const uint8_t *rdram, uint8_t world, uint8_t room, int16_t serial, int32_t amount)
{
    const kh2state::State st = kh2state::read(rdram);
    if (st.now.world != world || st.now.room != room || amount <= 0)
        return;
    for (const Local &e : scan(rdram))
        if (e.serial == serial && e.hp > 0)
        {
            std::printf("[enemies] a client hit %04X #%d for %d (hp %d)\n", e.entry, serial, amount, e.hp);
            std::fflush(stdout);
            int finisher = 0;
            {
                std::lock_guard<std::mutex> lock(g_rulesMutex);
                const auto f = g_clientHits.find(serial);
                if (f != g_clientHits.end() && nowSeconds() - f->second.at < 2.0)
                    finisher = f->second.skipsRules ? 2 : f->second.finisher ? 1 : 0;
            }
            postAddHp(e.object, -amount, finisher, serial);
            return;
        }
}

void noteClientHit(int16_t serial, bool skipsRules, bool finisher)
{
    Blocked late;
    {
        std::lock_guard<std::mutex> lock(g_rulesMutex);
        g_clientHits[serial] = {skipsRules, finisher, nowSeconds()};
        const auto b = g_blocked.find(serial);
        if (b != g_blocked.end())
        {
            if ((skipsRules || finisher) && nowSeconds() - b->second.at < 2.0)
                late = b->second;
            else if (skipsRules || finisher)
            {
                std::printf("[enemies] a client's finisher on #%d, %.2f s after a held-back kill: too late\n", serial,
                            nowSeconds() - b->second.at);
                std::fflush(stdout);
            }
            g_blocked.erase(b);
        }
    }
    if (late.object)
    {
        // The kill the rules held back a moment ago: this was its blow.
        std::printf("[enemies] a client's finisher on #%d came after its HP: the kill goes ahead\n", serial);
        std::fflush(stdout);
        postAddHp(late.object, -0x7FFFFFF, skipsRules ? 2 : 1, serial);
    }
    if (std::getenv("KH2_HIT_RULES_LOG"))
    {
        std::printf("[enemies] a client's hit on #%d:%s%s\n", serial, skipsRules ? " skips the rules" : "",
                    finisher ? " a finisher" : " (no finisher)");
        std::fflush(stdout);
    }
}

void onHostStates(const kh2net::EnemyStates &states, double now)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (states.tick < g_host.tick && now - g_hostTime < 1.0)
        return; // older than what we have
    g_host = states;
    g_hostTime = now;
}

// Host enemies our game has not made (the host walked into a trigger we did
// not): made here at the host's place, numbered as the host's, so they are
// followed like any other copy. Their files are requested first (as a
// room's spawn list does) and given a moment to load. One at a time; only
// in free play. Under g_mutex.
static void makeMissing(uint8_t *rdram, const std::vector<Local> &locals, double now)
{
    // Copies of ours the game has made again itself (we walked into the
    // trigger too): the game's own stays, ours goes.
    for (auto it = g_made.begin(); it != g_made.end(); ++it)
    {
        if (!it->second.object)
            continue;
        bool ours = false, other = false;
        for (const Local &e : locals)
            if (e.serial == it->first)
                (e.object == it->second.object ? ours : other) = true;
        if (ours && other)
        {
            std::printf("[enemies] #%d is here twice now: removing our copy\n", it->first);
            std::fflush(stdout);
            removeBody(it->second.object);
            it->second.object = 0;
        }
    }
    const kh2state::State st = kh2state::read(rdram);
    const bool freePlay = st.player.valid && st.control == 0 && !rd<uint32_t>(rdram, kh2state::addr::MapJumpTask);
    // Our player in a drive form (or one starting or ending): an entry's
    // files are not read the room's way then (its read request, group -1,
    // then a flush), which took the place of our form player's files: its
    // motion called through nothing (0x0017FE84) at once. They are read
    // into kh2formcache's batches instead, out of the game's way.
    const bool ourForm = rd<uint32_t>(rdram, 0x00351184u) != 0 ||
                         (st.player.valid && kh2partner::formBaseLook(rdram, uint16_t(st.player.entryId)) != 0);
    bool jobQueued = false;
    for (const auto &[ser, m] : g_made)
        jobQueued = jobQueued || (m.posted && !m.object && now - m.postedAt < 3.0);
    for (const kh2net::EnemyState &h : g_host.enemies)
    {
        if (h.hp <= 0)
            continue;
        bool have = false;
        for (const Local &e : locals)
            have = have || (e.serial == h.serial && e.entry == h.entry);
        if (have)
            continue;
        Made &m = g_made[h.serial];
        if (m.posted && (m.object || now - m.postedAt < 3.0))
            continue; // made (and gone since: it died here), or being made
        if (m.posted && !m.object)
            m.posted = false; // the last try made nothing: again
        auto pre = g_preloaded.find(h.entry);
        // (Not while our player is not there yet (a room loading): whether
        // it is in a form is not known then, and the read would only run
        // once it is there anyway.)
        if (pre == g_preloaded.end() && !st.player.valid)
            continue;
        if (ourForm && (pre == g_preloaded.end() || g_cached.count(h.entry)))
        {
            // (kh2formcache's batch: Ready once its files are in.)
            const kh2formcache::Look look = kh2formcache::want(rdram, h.entry);
            if (!g_cached.count(h.entry))
            {
                g_cached.insert(h.entry);
                std::printf("[enemies] %04X's files read into our cache (our player is in a form)\n", h.entry);
                std::fflush(stdout);
            }
            if (look != kh2formcache::Look::Ready || !freePlay || jobQueued)
                continue;
            if (pre == g_preloaded.end())
                pre = g_preloaded.emplace(h.entry, now - 10.0).first;
        }
        else if (pre == g_preloaded.end())
        {
            g_preloaded[h.entry] = now;
            std::printf("[enemies] %04X's files read the room's way (our player %04X, control %u)\n", h.entry,
                        unsigned(st.player.entryId), st.control);
            std::fflush(stdout);
            kh2puppets::requestPreload(h.entry);
            continue;
        }
        if (now - pre->second < 1.5 || !freePlay || jobQueued)
            continue;
        m.posted = true;
        m.postedAt = now;
        jobQueued = true;
        const int16_t serial = h.serial;
        const uint16_t entry = h.entry;
        const uint16_t room = g_followRoom;
        std::printf("[enemies] the host's %04X #%d is not in our game: making it\n", entry, serial);
        std::fflush(stdout);
        kh2puppets::spawnAt(
            entry, h.pos, h.yaw,
            [serial, entry, room](uint32_t obj) {
                std::printf("[enemies] made %04X #%d -> %08X\n", entry, serial, obj);
                std::fflush(stdout);
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_followRoom != room)
                    return;
                auto it = g_made.find(serial);
                if (it != g_made.end())
                    it->second.object = obj;
            },
            serial,
            [room](const uint8_t *ram) {
                const kh2state::State now = kh2state::read(ram);
                return uint16_t(now.now.world << 8 | now.now.room) == room && now.control == 0;
            });
        break;
    }
}

std::vector<Damage> clientTick(uint8_t *rdram, uint8_t world, uint8_t room, double now)
{
    std::vector<Damage> damage;
    std::vector<kh2puppets::Override> overrides;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint16_t key = uint16_t(world << 8 | room);
    if (key != g_followRoom)
    {
        g_follow.clear();
        g_killed.clear();
        g_hostSeen.clear();
        g_made.clear();
        g_preloaded.clear();
        g_cached.clear();
        g_followRoom = key;
        g_sharedSince = -1;
    }
    const bool fresh = g_hostTime >= 0 && now - g_hostTime < 1.0 && g_host.world == world && g_host.room == room;
    if (fresh && g_sharedSince < 0)
        g_sharedSince = now; // both of us in this room from now
    g_hostSpawns = fresh && g_sharedSince >= 0 && now - g_sharedSince > 3.0;
    if (!fresh)
    {
        if (g_lastFollowCount != 0 && g_lastFollowCount != SIZE_MAX)
        {
            std::printf("[enemies] client: no current enemy data from the host (%s)\n",
                        g_hostTime < 0 ? "none yet" : "stale or another room");
            std::fflush(stdout);
        }
        g_lastFollowCount = 0;
        thawAll(rdram);
        kh2puppets::setOverrides({});
        return damage;
    }
    for (const kh2net::EnemyState &x : g_host.enemies)
        g_hostSeen[x.serial] = x.entry;
    const std::vector<Local> locals = scan(rdram);
    makeMissing(rdram, locals, now);
    for (const Local &e : locals)
    {
        const kh2net::EnemyState *h = nullptr;
        for (const kh2net::EnemyState &x : g_host.enemies)
            if (x.serial == e.serial && x.entry == e.entry)
                h = &x;
        Follow &f = g_follow[e.serial];
        if (f.object != e.object)
        {
            // Another object with this serial (our game made its own after
            // our copy went): nothing of the old one's record applies. Kept,
            // "killed" stopped the game's own new one dying with the host's.
            f = Follow{};
            f.object = e.object;
            f.entry = e.entry;
            f.firstSeen = now;
            if (auto k = g_killed.find(e.object); k != g_killed.end() && now - k->second < 10.0)
            {
                f.killed = true; // (two of one serial for a moment: ours dying, the game's own)
                f.killedAt = k->second;
            }
        }
        if (!h)
        {
            // Gone on the host (dead and removed) after we had seen it there.
            // add_hp ending at 0 runs BTLOBJ::dead, also for an amount of 0
            // (our copy can already be at 0 without having died).
            auto seen = g_hostSeen.find(e.serial);
            const bool hostHadIt = seen != g_hostSeen.end() && seen->second == e.entry;
            if ((f.placed || hostHadIt) && !f.killed)
            {
                std::printf("[enemies] %04X #%d is gone on the host\n", e.entry, e.serial);
                std::fflush(stdout);
                killCopy(rdram, f, e, now);
            }
            else if (!f.placed && !hostHadIt && !f.killed && f.firstSeen >= 0 && now - f.firstSeen > 3.0 &&
                     now - g_sharedSince > 6.0) // (the host's room has had time to make its own)
            {
                // Our game made it (we set off a wave, or our own wave program
                // went on to the next one) and the host's game has not: in the
                // host's room the host's enemies are the only ones. It dies the
                // game's way, so a wave fight our game started (and its walls)
                // still ends; the host's waves arrive as copies (makeMissing).
                std::printf("[enemies] %04X #%d is not in the host's game; taking it away\n", e.entry, e.serial);
                std::fflush(stdout);
                killCopy(rdram, f, e, now);
            }
            else if (f.killed && !f.removed && now - f.killedAt > 5.0)
            {
                f.removed = true;
                std::printf("[enemies] %04X #%d: its body is still here; removing it\n", e.entry, e.serial);
                std::fflush(stdout);
                removeBody(e.object);
            }
            continue;
        }
        // Damage our player did to our copy since we last looked.
        if (!f.killed && f.expectedHp >= 0 && e.hp < f.expectedHp) // (our kill's own add_hp is no hit)
        {
            damage.push_back(Damage{e.serial, f.expectedHp - e.hp});
            std::printf("[enemies] hit %04X #%d for %d (motion %u)\n", e.entry, e.serial, f.expectedHp - e.hp,
                        e.motion);
            std::fflush(stdout);
            // The hit's reaction (flinch, knock-back) is our copy's own, made
            // by the game's damage code: give it time before the host's
            // motion is put back.
            f.holdUntil = now + 0.6;
            f.holding = true;
        }
        if (f.ownKill && !f.killed)
        {
            // Our blow killed it (its damage went to the host above): it dies
            // here of it, its own way; the host's goes when that damage lands.
            f.frozen = false;
            f.killed = true;
            f.killedAt = now;
            g_killed[e.object] = now;
            std::printf("[enemies] %04X #%d: our blow killed it\n", e.entry, e.serial);
            std::fflush(stdout);
        }
        if (f.killed)
        {
            // Dying its own way: the host's HP, place and motion no longer apply.
            if (!f.removed && now - f.killedAt > 5.0)
            {
                f.removed = true;
                std::printf("[enemies] %04X #%d: its body is still here; removing it\n", e.entry, e.serial);
                std::fflush(stdout);
                removeBody(e.object);
            }
            continue;
        }
        int32_t hp = e.hp;
        if (h->hp <= 0)
        {
            // Dead on the host: the blow that killed it comes as its own event
            // (onHostKill) and our copy dies of it. Without one by now (a death
            // that was no hit), it just dies.
            if (!f.killed && f.hostDeadAt < 0)
                f.hostDeadAt = now;
            if (!f.killed && now - f.hostDeadAt > 0.4)
            {
                std::printf("[enemies] %04X #%d died on the host\n", e.entry, e.serial);
                std::fflush(stdout);
                killCopy(rdram, f, e, now); // dies the game's way
            }
            else if (!f.removed && now - f.killedAt > 5.0)
            {
                f.removed = true;
                std::printf("[enemies] %04X #%d: its body is still here; removing it\n", e.entry, e.serial);
                std::fflush(stdout);
                removeBody(e.object);
            }
        }
        else
        {
            // The host's HP, also when it is higher: its enemies can have
            // more (scaling.h), and a copy with fewer would die here first.
            if (h->maxHp > 0 && h->maxHp != e.maxHp)
                std::memcpy(rdram + ((e.sheet + 4) & 0x01FFFFFFu), &h->maxHp, 4);
            // And its floor: a boss's script on the host raises it before a
            // move it must live to make; ours does not run (it follows), and
            // our blow killed our copy while the host's lived on.
            if (h->minHp >= 0 && h->minHp < h->maxHp && h->minHp != e.minHp)
            {
                std::memcpy(rdram + ((e.sheet + 8) & 0x01FFFFFFu), &h->minHp, 4);
                std::printf("[enemies] %04X #%d: the host's HP floor %d\n", e.entry, e.serial, h->minHp);
                std::fflush(stdout);
            }
            // And its survive rules (enemies.h), for the same reason: our
            // hits on our copy then take the HP the host's would.
            if (h->rules != e.rules)
            {
                const uint32_t bits = kRuleMask | 0x80000u;
                const uint32_t want = (h->rules & kRuleMask) | ((h->rules & kRuleAnyChannel) ? 0x80000u : 0);
                uint32_t w[3];
                std::memcpy(w, rdram + ((e.object + kStatus) & 0x01FFFFFFu), sizeof(w));
                w[0] = (w[0] & ~bits) | want;
                w[1] &= ~bits;
                w[2] &= ~bits;
                std::memcpy(rdram + ((e.object + kStatus) & 0x01FFFFFFu), w, sizeof(w));
                std::printf("[enemies] %04X #%d: the host's survive rules %03X\n", e.entry, e.serial, h->rules);
                std::fflush(stdout);
            }
            if (h->hp != hp)
            {
                std::memcpy(rdram + (e.sheet & 0x01FFFFFFu), &h->hp, 4);
                hp = h->hp;
            }
        }
        f.expectedHp = hp;
        // Position: ease toward the host's (20 updates a second; smooth the steps).
        if (!f.placed)
        {
            std::memcpy(f.shown, h->pos, sizeof(f.shown));
            f.yaw = h->yaw;
            f.placed = true;
        }
        else
        {
            for (int i = 0; i < 3; ++i)
                f.shown[i] += (h->pos[i] - f.shown[i]) * 0.35f;
            float dy = std::remainder(h->yaw - f.yaw, 6.2831853f);
            f.yaw += dy * 0.35f;
        }
        if (hp > 0)
        {
            kh2puppets::Override o;
            o.object = e.object;
            std::memcpy(o.pos, f.shown, sizeof(o.pos));
            o.yaw = f.yaw;
            overrides.push_back(o);
            // A new copy first plays its own way in (a Shadow rising out of the
            // floor is its own routine: frozen at once, it stayed flat).
            static const double spawnGrace = [] {
                const char *e = std::getenv("KH2_ENEMY_SPAWN_GRACE");
                const double v = e ? std::atof(e) : 2.0;
                return v >= 0.0 && v < 30.0 ? v : 2.0;
            }();
            if (puppetEnemies() && f.firstSeen >= 0 && now - f.firstSeen < spawnGrace)
                continue; // (the override above is already in the list)
            if (puppetEnemies())
            {
                // Its AI off; it does what the host's does.
                const uint32_t flags = rd<uint32_t>(rdram, e.object + kObjFlags);
                if (!f.frozen)
                {
                    f.frozen = true;
                    f.flagsBefore = flags;
                }
                if (!(flags & kNoAct))
                {
                    const uint32_t v = flags | kNoAct;
                    std::memcpy(rdram + ((e.object + kObjFlags) & 0x01FFFFFFu), &v, 4);
                }
                ++f.sinceMotion;
                if (now < f.holdUntil)
                    continue; // (the override above is already in the list)
                if (f.holding)
                {
                    f.holding = false;
                    f.motion = 0xFFFF; // back to the host's motion
                }
                if (h->motion != f.motion || (e.motion != h->motion && f.sinceMotion > 20))
                {
                    f.motion = h->motion;
                    f.sinceMotion = 0;
                    kh2puppets::postMotion(e.object, h->motion, 4.0f);
                }
            }
        }
    }
    if (overrides.size() != g_lastFollowCount)
    {
        g_lastFollowCount = overrides.size();
        std::printf("[enemies] client: following %zu of the host's %zu enemies\n", overrides.size(),
                    g_host.enemies.size());
        std::fflush(stdout);
    }
    kh2puppets::setOverrides(overrides);
    return damage;
}

bool onHostKill(uint8_t *rdram, uint8_t world, uint8_t room, int16_t serial, uint16_t entry, const kh2hits::Hit &hit,
                uint32_t attacker, double now)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (uint16_t(world << 8 | room) != g_followRoom)
        return false;
    auto it = g_follow.find(serial);
    if (it == g_follow.end() || it->second.killed || it->second.entry != entry || !it->second.object)
        return false;
    Follow &f = it->second;
    bool live = false;
    int32_t hp = 0;
    for (const Local &e : scan(rdram))
        if (e.object == f.object)
        {
            live = true;
            hp = e.hp;
        }
    if (!live || hp <= 0)
        return false;
    // Its AI back (its own code plays the death and takes the body away),
    // then the host's blow, with all its HP.
    if (f.frozen)
    {
        const uint32_t flags = rd<uint32_t>(rdram, f.object + kObjFlags);
        const uint32_t back = (flags & ~kNoAct) | (f.flagsBefore & kNoAct);
        std::memcpy(rdram + ((f.object + kObjFlags) & 0x01FFFFFFu), &back, 4);
        f.frozen = false;
    }
    f.killed = true;
    f.killedAt = now;
    g_killed[f.object] = now;
    std::printf("[enemies] %04X #%d was killed on the host: our copy takes the blow (%d HP)\n", entry, serial, hp);
    std::fflush(stdout);
    kh2hits::replay(f.object, attacker, hit.damage, hit.param, true);
    return true;
}

void onOwnKill(uint32_t object)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[serial, f] : g_follow)
        if (f.object == object && !f.killed)
            f.ownKill = true;
}

bool followed(uint32_t object, int16_t &serial, uint16_t &entry)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &[ser, f] : g_follow)
        if (f.object == object && f.placed && !f.killed)
        {
            serial = ser;
            entry = f.entry;
            return true;
        }
    return false;
}

void reset()
{
    g_hostSpawns = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &[serial, f] : g_follow)
        if (f.frozen)
            g_thaw.emplace_back(f.object, f.flagsBefore);
    g_host = {};
    g_hostTime = -1;
    g_follow.clear();
    g_killed.clear();
    g_hostSeen.clear();
    g_made.clear();
    g_preloaded.clear();
    g_cached.clear();
    g_followRoom = 0xFFFF;
    g_sharedSince = -1;
    kh2puppets::setOverrides({});
}

std::vector<uint8_t> encodeDamage(uint8_t world, uint8_t room, const Damage &d)
{
    std::vector<uint8_t> out;
    kh2net::ByteWriter w(out);
    w.u8(world);
    w.u8(room);
    w.u16(static_cast<uint16_t>(d.serial));
    w.i32(d.amount);
    return out;
}

bool decodeDamage(const std::vector<uint8_t> &data, uint8_t &world, uint8_t &room, Damage &d)
{
    kh2net::ByteReader r(data.data(), data.size());
    world = r.u8();
    room = r.u8();
    d.serial = static_cast<int16_t>(r.u16());
    d.amount = r.i32();
    return r.ok();
}
void thawAfterSession(uint8_t *rdram)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_thaw.empty())
        return;
    std::vector<uint32_t> live;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        live.push_back(a.address);
    for (const auto &[obj, before] : g_thaw)
    {
        bool there = false;
        for (uint32_t a : live)
            there = there || a == obj;
        if (!there)
            continue;
        const uint32_t flags = rd<uint32_t>(rdram, obj + kObjFlags);
        const uint32_t back = (flags & ~kNoAct) | (before & kNoAct);
        std::memcpy(rdram + ((obj + kObjFlags) & 0x01FFFFFFu), &back, 4);
    }
    g_thaw.clear();
}
} // namespace kh2enemies
