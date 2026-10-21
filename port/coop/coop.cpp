#include "coop.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "game_state.h"
#include "puppets.h"
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
    g_session->onEvent = [](uint8_t, uint16_t, const std::vector<uint8_t> &) {};
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
    g_session->update(now());
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
            targets.push_back(tg);
        }
        kh2puppets::update(rdram, targets);
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
    if (st.player.valid)
    {
        for (int i = 0; i < 3; ++i)
            ps.pos[i] = st.player.pos[i];
        ps.rotY = st.player.facing;
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
} // namespace kh2coop
