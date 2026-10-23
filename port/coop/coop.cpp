#include "coop.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "enemies.h"
#include "partners.h"
#include "game_state.h"
#include "hits.h"
#include "puppets.h"
#include "guest_call.h"
#include "net/interpolation.h"
#include "net/session.h"

namespace kh2coop
{
namespace
{
// The game's own build check: everyone must run the same executable.
constexpr uint32_t kGameCrc = 0xF266B00Bu;

struct Remote
{
    std::string name;
    kh2net::Interpolator interp{0.1};
    std::vector<uint8_t> loadout; // their character save record (Loadout event)
};

std::mutex g_mutex; // tick() runs on the host loop, the UI reads from the render path
Options g_opt;
std::unique_ptr<kh2net::Session> g_session;
std::map<uint8_t, Remote> g_remotes;
std::deque<std::string> g_log;
uint64_t g_lastSentFrame = UINT64_MAX;
double g_lastWarp = -100.0;
double g_hostRoomSince = 0.0;
uint16_t g_hostRoomSeen = 0xFFFF;
const uint8_t *g_rdram = nullptr; // during tick(), for session callbacks
uint64_t g_lastEnemyFrame = UINT64_MAX;
// Partners (partners.h): heals for this player, our loadout as last sent.
std::vector<kh2partner::Heal> g_healsIn;
std::vector<uint8_t> g_sentLoadout;
double g_loadoutSentAt = -100.0;
bool g_resendLoadout = false;
// --partner-test: a stand-in partner that copies this player, a second behind.
bool g_testPartner = false;
std::deque<kh2partner::Remote> g_echo;
bool g_partnersReset = false; // undo the partners on the next tick (session over)
// Reloading the room so partners show their own look (partners.h): wanted,
// then the map jump under way, then the player put back where they stood.
struct Refresh
{
    bool wanted = false, jumping = false, sawJump = false;
    uint8_t world = 0, room = 0;
    float pos[4] = {0, 0, 0, 1}, dir[4] = {0, 0, 1, 0};
    double since = 0, landed = 0;
};
Refresh g_refresh;


double now()
{
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

void addLog(const std::string &line)
{
    std::printf("[coop] %s\n", line.c_str());
    std::fflush(stdout);
    g_log.push_back(line);
    while (g_log.size() > 8)
        g_log.pop_front();
}

// Queue the game's own map jump (runs on the EE thread next frame).
void warpLocked(uint8_t world, uint8_t room, uint16_t door, uint16_t battle = 0xFFFF, uint16_t event = 0xFFFF)
{
    addLog("going to " + kh2state::describe({world, room}));
    kh2guest::post([world, room, door, battle, event](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
        {
            uint32_t task;
            std::memcpy(&task, env.host(kh2state::addr::MapJumpTask), 4);
            uint8_t next[10];
            std::memcpy(next, env.host(0x0032BAF0), sizeof(next));
            std::printf("[coop] map jump queued: task %08X, next %02X-%02X door %02X%02X\n", task, next[0], next[1],
                        next[3], next[2]);
            std::fflush(stdout);
            return std::nullopt;
        }
        uint8_t loc[10];
        loc[0] = world;
        loc[1] = room;
        std::memcpy(loc + 2, &door, 2);
        const uint16_t keep = 0xFFFF; // map, battle, event: what the save says
        for (int i = 0; i < 3; ++i)
            std::memcpy(loc + 4 + 2 * i, &keep, 2);
        std::memcpy(loc + 6, &battle, 2);
        std::memcpy(loc + 8, &event, 2);
        std::memcpy(env.host(env.scratch), loc, sizeof(loc));
        return kh2guest::Call{kh2state::addr::MapJump, {env.scratch, 1u, 0u, 0u, 0u}, {}};
    });
}

// The host's last known state, if we are a client.
bool hostState(kh2net::PlayerState &out)
{
    auto it = g_remotes.find(0);
    if (it == g_remotes.end() || it->second.interp.empty())
        return false;
    return it->second.interp.sample(now(), out);
}

// The object that stands for player `id` in our game: their partner (the
// companion they replace) or puppet in this room, else our own player.
uint32_t attackerFor(const uint8_t *rdram, uint8_t id)
{
    for (const kh2partner::Link &l : kh2partner::links())
        if (l.playerId == id && l.object)
            return l.object;
    for (const kh2puppets::Assignment &a : kh2puppets::assignments())
        if (a.playerId == id && a.object)
            return a.object;
    uint32_t player = 0;
    std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
    return player;
}

void makeSession()
{
    g_session = std::make_unique<kh2net::Session>();
    g_remotes.clear();
    g_session->onLog = [](const std::string &s) { addLog(s); };
    g_session->onPeer = [](uint8_t id, const std::string &name, bool joined) {
        if (joined)
        {
            g_remotes[id].name = name;
            g_resendLoadout = true; // the newcomer needs ours
        }
        else
            g_remotes.erase(id);
    };
    g_session->onState = [](const kh2net::PlayerState &s) {
        Remote &r = g_remotes[s.origin];
        r.interp.push(s, now());
    };
    g_session->onEnemies = [](const kh2net::EnemyStates &s) {
        if (g_opt.sharedEnemies)
            kh2enemies::onHostStates(s, now());
    };
    g_session->onEvent = [](uint8_t origin, uint16_t type, const std::vector<uint8_t> &data) {
        if (type == static_cast<uint16_t>(kh2net::GameEventType::Loadout) && data.size() == kh2partner::kLoadoutSize)
        {
            g_remotes[origin].loadout = data;
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::PartnerHeal) && data.size() == 9 && g_session &&
            data[0] == g_session->localId())
        {
            kh2partner::Heal h;
            std::memcpy(&h.hp, data.data() + 1, 4);
            std::memcpy(&h.mp, data.data() + 5, 4);
            g_healsIn.push_back(h);
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::EnemyHit) && g_session->isHost() && g_rdram &&
            g_opt.sharedEnemies)
        {
            // A client's hit: its reaction, played on our copy of the enemy.
            uint8_t world, room;
            int16_t serial;
            kh2hits::Hit hit;
            if (!kh2hits::decode(data, world, room, serial, hit))
                return;
            const kh2state::State st = kh2state::read(g_rdram);
            if (st.now.world != world || st.now.room != room)
                return;
            for (const kh2enemies::Local &e : kh2enemies::scan(g_rdram))
                if (e.serial == serial && e.hp > 0)
                {
                    kh2hits::replay(e.object, attackerFor(g_rdram, origin), hit.damage, hit.param);
                    break;
                }
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::EnemyDamage) && g_session->isHost() && g_rdram &&
            g_opt.sharedEnemies)
        {
            uint8_t world, room;
            kh2enemies::Damage d;
            if (kh2enemies::decodeDamage(data, world, room, d))
                kh2enemies::applyDamage(g_rdram, world, room, d.serial, d.amount);
        }
    };
}

kh2net::SessionConfig config(const std::string &name, uint16_t port)
{
    kh2net::SessionConfig c;
    c.name = name;
    c.gameCrc = kGameCrc;
    c.port = port;
    c.simulatedLoss = g_opt.simulatedLoss;
    return c;
}
} // namespace

bool host(const std::string &name, uint16_t port)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_opt.name = name;
    makeSession();
    if (!g_session->host(config(name, port)))
    {
        addLog("could not host: " + g_session->lastError());
        return false;
    }
    return true;
}

bool join(const std::string &name, const std::string &address)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_opt.name = name;
    kh2net::Address a;
    if (!kh2net::Address::parse(address, kh2net::kDefaultPort, a))
    {
        addLog("cannot find " + address);
        return false;
    }
    makeSession();
    if (!g_session->join(config(name, 0), a))
    {
        addLog("could not join: " + g_session->lastError());
        return false;
    }
    return true;
}

void leave()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session && g_session->active())
        addLog("left the session");
    g_session.reset();
    g_remotes.clear();
    kh2enemies::reset();
    g_sentLoadout.clear();
    g_partnersReset = true;
}

void init(const Options &options)
{
    g_opt = options;
    if (options.host)
        host(options.name, options.port);
    else if (!options.join.empty())
        join(options.name, options.join);
}

void shutdown() { leave(); }

// The remote players as partners (partners.h); heals they got here go out.
void updatePartners(uint8_t *ram, uint64_t guestFrame)
{
    std::vector<kh2partner::Remote> list;
    const double t = now();
    if (g_testPartner)
    {
        // Stand-in: this player as they were 60 frames ago, a little to the side.
        const kh2state::State st = kh2state::read(ram);
        kh2partner::Remote me;
        me.playerId = 7;
        me.name = "Echo";
        me.present = st.player.valid;
        me.world = st.now.world;
        me.room = st.now.room;
        for (int i = 0; i < 3; ++i)
            me.pos[i] = st.player.pos[i];
        me.pos[0] += 120.0f;
        me.yaw = st.player.facing;
        me.model = uint16_t(st.player.entryId);
        me.weapon = kh2partner::localWeaponEntry(ram);
        me.motion = uint16_t(st.player.motion);
        me.hp = st.party[0].hp;
        me.maxHp = st.party[0].maxHp;
        me.mp = st.party[0].mp;
        me.maxMp = st.party[0].maxMp;
        me.loadout = kh2partner::localLoadout(ram);
        g_echo.push_back(me);
        while (g_echo.size() > 60)
            g_echo.pop_front();
        list.push_back(g_echo.front());
    }
    else if (g_session && g_session->active())
        for (auto &[id, r] : g_remotes)
        {
            kh2net::PlayerState ps;
            if (!r.interp.sample(t, ps) || t - r.interp.lastArrival() > 3.0)
                continue;
            kh2partner::Remote p;
            p.playerId = id;
            p.name = r.name;
            p.present = (ps.flags & 1) != 0;
            p.world = ps.world;
            p.room = ps.room;
            std::memcpy(p.pos, ps.pos, sizeof(p.pos));
            p.yaw = ps.rotY;
            p.model = ps.model;
            p.weapon = ps.weapon;
            p.motion = ps.anim;
            p.hp = ps.hp;
            p.maxHp = ps.maxHp;
            p.mp = ps.mp;
            p.maxMp = ps.maxMp;
            p.loadout = r.loadout;
            list.push_back(p);
        }
    for (const kh2partner::Heal &h : kh2partner::update(ram, list))
    {
        if (g_testPartner)
        {
            kh2partner::applyHeal(ram, h.hp, h.mp);
            continue;
        }
        std::vector<uint8_t> data(9);
        data[0] = h.playerId;
        std::memcpy(data.data() + 1, &h.hp, 4);
        std::memcpy(data.data() + 5, &h.mp, 4);
        g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::PartnerHeal), data);
        addLog("healed player " + std::to_string(h.playerId + 1) + " for " + std::to_string(h.hp) + " HP");
    }
    if (kh2partner::takeRefreshRequest())
        g_refresh.wanted = true;
    for (const kh2partner::Heal &h : g_healsIn)
        kh2partner::applyHeal(ram, h.hp, h.mp);
    g_healsIn.clear();
    // Our equipment, for the others' menus: when it changes (at most once a
    // second) and when someone joins.
    if (g_session && g_session->active() && !g_testPartner && guestFrame % 30 == 0)
    {
        std::vector<uint8_t> mine = kh2partner::localLoadout(ram);
        if ((mine != g_sentLoadout || g_resendLoadout) && t - g_loadoutSentAt > 1.0)
        {
            g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::Loadout), mine);
            g_sentLoadout = mine;
            g_loadoutSentAt = t;
            g_resendLoadout = false;
        }
    }
}

// Reload the room (same room, same door) when the player is free to move, and
// put them back where they were once it has loaded.
void stepRefresh(uint8_t *ram)
{
    Refresh &r = g_refresh;
    if (!r.wanted && !r.jumping)
        return;
    const kh2state::State st = kh2state::read(ram);
    uint32_t task = 0, player = 0;
    std::memcpy(&task, ram + kh2state::addr::MapJumpTask, 4);
    std::memcpy(&player, ram + kh2state::addr::PlayerPtr, 4);
    if (r.wanted && !r.jumping)
    {
        if (!st.player.valid || st.control != 0 || st.battle != 0 || task || st.now.world == 0)
            return;
        r.wanted = false;
        r.jumping = true;
        r.sawJump = false;
        r.world = st.now.world;
        r.room = st.now.room;
        std::memcpy(r.pos, ram + ((player + kh2state::addr::ObjPos) & 0x01FFFFFFu), sizeof(r.pos));
        std::memcpy(r.dir, ram + ((player + kh2state::addr::ObjDir) & 0x01FFFFFFu), sizeof(r.dir));
        r.since = now();
        addLog("reloading the room to show your partners");
        warpLocked(st.now.world, st.now.room, st.now.door);
        return;
    }
    if (task)
        r.sawJump = true;
    if (now() - r.since > 20.0)
    {
        r.jumping = false; // gave up
        return;
    }
    if (!r.sawJump || task || !st.player.valid || st.now.world != r.world || st.now.room != r.room)
    {
        r.landed = 0;
        return;
    }
    // Let the room finish placing everyone first.
    if (r.landed == 0)
        r.landed = now();
    if (now() - r.landed < 0.3)
        return;
    std::memcpy(ram + ((player + kh2state::addr::ObjPos) & 0x01FFFFFFu), r.pos, sizeof(r.pos));
    std::memcpy(ram + ((player + kh2state::addr::ObjDir) & 0x01FFFFFFu), r.dir, sizeof(r.dir));
    r.jumping = false;
}

void tick(const uint8_t *rdram, uint64_t guestFrame)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    uint8_t *ram = const_cast<uint8_t *>(rdram);
    kh2enemies::thawAfterSession(ram);
    if (g_partnersReset)
    {
        g_partnersReset = false;
        kh2partner::reset(ram);
    }
    if (!g_session || !g_session->active())
    {
        if (g_testPartner)
            updatePartners(ram, guestFrame);
    }
    stepRefresh(ram);
    if (!g_session)
    {
        kh2puppets::update(rdram, {});
        return;
    }
    const auto before = g_session->status();
    g_rdram = rdram;
    g_session->update(now());
    g_rdram = nullptr;
    if (g_session->status() == kh2net::Session::Status::Failed && before != kh2net::Session::Status::Failed)
        addLog(g_session->lastError());
    if (!g_session->active())
    {
        kh2hits::setCapture(false);
        kh2puppets::update(rdram, {});
        return;
    }
    updatePartners(ram, guestFrame);
    // Remote players with a body drive their puppets, unless they are one of
    // our party members (partners.h).
    {
        std::vector<kh2puppets::Target> targets;
        const double t = now();
        for (auto &[id, r] : g_remotes)
        {
            kh2net::PlayerState ps;
            if (!r.interp.sample(t, ps) || !(ps.flags & 1) || t - r.interp.lastArrival() > 3.0)
                continue;
            if (kh2partner::linked(id))
            {
                bool here = false;
                for (const kh2partner::Link &l : kh2partner::links())
                    here = here || (l.playerId == id && l.object);
                if (here)
                    continue;
            }
            kh2puppets::Target tg;
            tg.playerId = id;
            std::memcpy(tg.pos, ps.pos, sizeof(tg.pos));
            tg.yaw = ps.rotY;
            tg.world = ps.world;
            tg.room = ps.room;
            tg.model = ps.model;
            tg.motion = ps.anim;
            targets.push_back(tg);
        }
        kh2puppets::update(rdram, targets);
    }
    // Clients following the host: once the host has been in another room for
    // a moment and we are in free control, go there too.
    if (g_opt.followHost && !g_session->isHost())
    {
        kh2net::PlayerState host;
        const kh2state::State me = kh2state::read(rdram);
        uint32_t jumping;
        std::memcpy(&jumping, rdram + kh2state::addr::MapJumpTask, 4);
        if (hostState(host) && host.world != 0)
        {
            const uint16_t key = uint16_t(host.world << 8 | host.room);
            if (key != g_hostRoomSeen)
            {
                g_hostRoomSeen = key;
                g_hostRoomSince = now();
            }
            const bool elsewhere = host.world != me.now.world || host.room != me.now.room;
            if (elsewhere && me.control == 0 && !jumping && now() - g_hostRoomSince > 1.0 && now() - g_lastWarp > 5.0)
            {
                g_lastWarp = now();
                warpLocked(host.world, host.room, host.door);
            }
        }
    }
    // Enemies: the host sends its own ~20 times a second; clients follow them
    // and report their hits.
    if (g_opt.sharedEnemies)
    {
        const kh2state::State me = kh2state::read(rdram);
        if (g_session->isHost())
        {
            if (guestFrame / 3 != g_lastEnemyFrame / 3 && me.now.world != 0)
            {
                g_lastEnemyFrame = guestFrame;
                g_session->sendEnemies(
                    kh2enemies::snapshot(rdram, me.now.world, me.now.room, static_cast<uint32_t>(guestFrame)));
            }
        }
        else
        {
            // Our hits first (their reactions), then the HP they took.
            for (const kh2hits::Hit &h : kh2hits::takeHits())
                for (const kh2enemies::Local &e : kh2enemies::scan(rdram))
                    if (e.object == h.object)
                    {
                        g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::EnemyHit),
                                             kh2hits::encode(me.now.world, me.now.room, e.serial, h));
                        break;
                    }
            for (const kh2enemies::Damage &d :
                 kh2enemies::clientTick(const_cast<uint8_t *>(rdram), me.now.world, me.now.room, now()))
                g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::EnemyDamage),
                                     kh2enemies::encodeDamage(me.now.world, me.now.room, d));
        }
    }
    kh2hits::setCapture(g_opt.sharedEnemies && !g_session->isHost());
    // 30 updates a second: every other guest frame.
    if (guestFrame / 2 == g_lastSentFrame / 2)
        return;
    g_lastSentFrame = guestFrame;
    const kh2state::State st = kh2state::read(rdram);
    kh2net::PlayerState ps;
    ps.tick = static_cast<uint32_t>(guestFrame);
    ps.world = st.now.world;
    ps.room = st.now.room;
    ps.door = st.now.door;
    if (st.player.valid)
    {
        for (int i = 0; i < 3; ++i)
            ps.pos[i] = st.player.pos[i];
        ps.rotY = st.player.facing;
        ps.model = static_cast<uint16_t>(st.player.entryId);
        ps.anim = static_cast<uint16_t>(st.player.motion);
        ps.flags |= 1; // has a body in the world
    }
    ps.hp = st.party[0].hp;
    ps.maxHp = st.party[0].maxHp;
    ps.mp = st.party[0].mp;
    ps.maxMp = st.party[0].maxMp;
    ps.weapon = kh2partner::localWeaponEntry(rdram);
    g_session->sendState(ps);
}

bool active()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_session && g_session->active();
}

bool isHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_session && g_session->isHost();
}

std::string statusText()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_session)
        return "Not connected";
    using S = kh2net::Session::Status;
    switch (g_session->status())
    {
    case S::Hosting:
        return "Hosting on port " + std::to_string(g_session->port()) + " (" +
               std::to_string(g_session->roster().size()) + "/" + std::to_string(kh2net::kMaxPlayers) + " players)";
    case S::Connecting:
        return "Connecting...";
    case S::Connected:
        return "Connected as player " + std::to_string(g_session->localId() + 1);
    case S::Failed:
        return "Failed: " + g_session->lastError();
    default:
        return "Not connected";
    }
}

std::vector<RemotePlayer> remotes()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<RemotePlayer> out;
    if (!g_session)
        return out;
    const double t = now();
    for (const kh2net::PeerInfo &p : g_session->roster())
    {
        if (p.local)
            continue;
        RemotePlayer r;
        r.id = p.id;
        r.name = p.name;
        r.rttMs = p.rttMs;
        r.loss = p.loss;
        auto it = g_remotes.find(p.id);
        if (it != g_remotes.end())
        {
            r.haveState = it->second.interp.sample(t, r.state);
            r.age = t - it->second.interp.lastArrival();
        }
        out.push_back(r);
    }
    return out;
}

std::vector<std::string> recentLog()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return {g_log.begin(), g_log.end()};
}

const Options &options() { return g_opt; }

void warpToHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    kh2net::PlayerState host;
    if (!g_session || g_session->isHost() || !hostState(host) || host.world == 0)
    {
        addLog("no host location yet");
        return;
    }
    g_lastWarp = now();
    warpLocked(host.world, host.room, host.door);
}

void requestWarp(uint8_t world, uint8_t room, uint16_t door, uint16_t battle, uint16_t event)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    warpLocked(world, room, door, battle, event);
}

void setFollowHost(bool on) { g_opt.followHost = on; }
void setSharedEnemies(bool on)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_opt.sharedEnemies = on;
    if (!on)
        kh2enemies::reset();
}
bool sharedEnemies() { return g_opt.sharedEnemies; }
void setTestPartner(bool on)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_testPartner && !on)
        g_partnersReset = true;
    g_testPartner = on;
    g_echo.clear();
}
bool followHost() { return g_opt.followHost; }
} // namespace kh2coop
