#include "enemies.h"

#include <cmath>
#include <cstdio>
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
};
std::map<int16_t, Follow> g_follow;   // by serial, this room
uint16_t g_followRoom = 0xFFFF;
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
} // namespace

std::vector<Local> scan(const uint8_t *rdram)
{
    std::vector<Local> out;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        if (a.type != 3 && a.type != 4) // boss, enemy
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

std::vector<Damage> clientTick(uint8_t *rdram, uint8_t world, uint8_t room, double now)
{
    std::vector<Damage> damage;
    std::vector<kh2puppets::Override> overrides;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint16_t key = uint16_t(world << 8 | room);
    if (key != g_followRoom)
    {
        g_follow.clear();
        g_followRoom = key;
    }
    const bool fresh = g_hostTime >= 0 && now - g_hostTime < 1.0 && g_host.world == world && g_host.room == room;
    if (!fresh)
    {
        if (g_lastFollowCount != 0 && g_lastFollowCount != SIZE_MAX)
        {
            std::printf("[enemies] client: no current enemy data from the host (%s)\n",
                        g_hostTime < 0 ? "none yet" : "stale or another room");
            std::fflush(stdout);
        }
        g_lastFollowCount = 0;
        kh2puppets::setOverrides({});
        return damage;
    }
    for (const Local &e : scan(rdram))
    {
        const kh2net::EnemyState *h = nullptr;
        for (const kh2net::EnemyState &x : g_host.enemies)
            if (x.serial == e.serial && x.entry == e.entry)
                h = &x;
        Follow &f = g_follow[e.serial];
        if (!h)
        {
            // Gone on the host (dead and removed) after we had seen it there.
            if (f.placed && !f.killed && e.hp > 0)
            {
                f.killed = true;
                postAddHp(e.object, -e.hp);
            }
            continue;
        }
        // Damage our player did to our copy since we last looked.
        if (f.expectedHp >= 0 && e.hp < f.expectedHp)
        {
            damage.push_back(Damage{e.serial, f.expectedHp - e.hp});
            std::printf("[enemies] hit %04X #%d for %d\n", e.entry, e.serial, f.expectedHp - e.hp);
            std::fflush(stdout);
        }
        int32_t hp = e.hp;
        if (h->hp < hp)
        {
            if (h->hp <= 0)
            {
                if (!f.killed)
                {
                    f.killed = true;
                    std::printf("[enemies] %04X #%d died on the host\n", e.entry, e.serial);
                    std::fflush(stdout);
                    postAddHp(e.object, -hp); // dies the game's way
                }
            }
            else
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

void reset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_host = {};
    g_hostTime = -1;
    g_follow.clear();
    g_followRoom = 0xFFFF;
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
} // namespace kh2enemies
