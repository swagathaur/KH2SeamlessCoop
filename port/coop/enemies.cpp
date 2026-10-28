#include "enemies.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "game_state.h"
#include "guest_call.h"
#include "puppets.h"

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
std::vector<std::pair<uint32_t, uint32_t>> g_thaw; // (object, flags before) to give their AI back
uint16_t g_followRoom = 0xFFFF;
double g_sharedSince = -1; // when the host and we were first both in this room (clientTick)
size_t g_lastSentCount = SIZE_MAX, g_lastFollowCount = SIZE_MAX; // for the log

// Guest job: obj->add_hp(amount) (negative = damage; at 0 the object dies
// the game's own way).
void postAddHp(uint32_t obj, int32_t amount)
{
    kh2guest::post([obj, amount](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
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

std::vector<Local> scan(const uint8_t *rdram)
{
    std::vector<Local> out;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        if (a.type != 3 && a.type != 4 && a.type != 12) // boss, enemy, giant boss (Twilight Thorn)
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
            postAddHp(e.object, -amount);
            return;
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
        if (pre == g_preloaded.end())
        {
            g_preloaded[h.entry] = now;
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
        g_followRoom = key;
        g_sharedSince = -1;
    }
    const bool fresh = g_hostTime >= 0 && now - g_hostTime < 1.0 && g_host.world == world && g_host.room == room;
    if (fresh && g_sharedSince < 0)
        g_sharedSince = now; // both of us in this room from now
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
        int32_t hp = e.hp;
        if (h->hp <= 0)
        {
            if (!f.killed)
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
