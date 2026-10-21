#include "coop.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "enemies.h"
#include "game_state.h"
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
void warpLocked(uint8_t world, uint8_t room, uint16_t door, uint16_t battle = 0xFFFF)
{
    addLog("going to " + kh2state::describe({world, room}));
    kh2guest::post([world, room, door, battle](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
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

void makeSession()
{
    g_session = std::make_unique<kh2net::Session>();
    g_remotes.clear();
    g_session->onLog = [](const std::string &s) { addLog(s); };
    g_session->onPeer = [](uint8_t id, const std::string &name, bool joined) {
        if (joined)
            g_remotes[id].name = name;
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
    g_session->onEvent = [](uint8_t, uint16_t type, const std::vector<uint8_t> &data) {
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

void tick(const uint8_t *rdram, uint64_t guestFrame)
{
    std::lock_guard<std::mutex> lock(g_mutex);
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
        kh2puppets::update(rdram, {});
        return;
    }
    // Remote players with a body drive their puppets.
    {
        std::vector<kh2puppets::Target> targets;
        const double t = now();
        for (auto &[id, r] : g_remotes)
        {
            kh2net::PlayerState ps;
            if (!r.interp.sample(t, ps) || !(ps.flags & 1) || t - r.interp.lastArrival() > 3.0)
                continue;
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
            for (const kh2enemies::Damage &d :
                 kh2enemies::clientTick(const_cast<uint8_t *>(rdram), me.now.world, me.now.room, now()))
                g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::EnemyDamage),
                                     kh2enemies::encodeDamage(me.now.world, me.now.room, d));
    }
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

void requestWarp(uint8_t world, uint8_t room, uint16_t door, uint16_t battle)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    warpLocked(world, room, door, battle);
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
bool followHost() { return g_opt.followHost; }
} // namespace kh2coop
