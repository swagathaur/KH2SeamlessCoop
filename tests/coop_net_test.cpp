// Loopback test of the co-op session: a host and two clients on 127.0.0.1,
// with simulated packet loss. Checks joining, the roster, state relay,
// reliable ordered events (exactly once, in order), rejection of a fourth
// player and of a different game build, and timeouts.
// Build: see CMakeLists.txt (KH2_BUILD_TESTS), or
//   g++ -std=c++20 -Iport/coop/net tests/coop_net_test.cpp port/coop/net/*.cpp
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "interpolation.h"
#include "session.h"

using namespace kh2net;

namespace
{
int g_failures = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

struct Node
{
    Session s;
    std::string name;
    std::map<uint8_t, std::vector<uint32_t>> events; // origin -> event payloads in arrival order
    std::map<uint8_t, PlayerState> lastState;
    EnemyStates lastEnemies;
    int enemyPackets = 0;
    int joins = 0, leaves = 0;

    explicit Node(std::string n) : name(std::move(n))
    {
        s.onEvent = [this](uint8_t origin, uint16_t type, const std::vector<uint8_t> &d) {
            if (type == 7 && d.size() == 4)
            {
                uint32_t v;
                std::memcpy(&v, d.data(), 4);
                events[origin].push_back(v);
            }
        };
        s.onState = [this](const PlayerState &st) { lastState[st.origin] = st; };
        s.onEnemies = [this](const EnemyStates &e) {
            lastEnemies = e;
            ++enemyPackets;
        };
        s.onPeer = [this](uint8_t, const std::string &, bool joined) { (joined ? joins : leaves)++; };
    }
};

void step(std::vector<Node *> nodes, double &now, int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        now += 1.0 / 60.0;
        for (Node *n : nodes)
            n->s.update(now);
    }
}
} // namespace

int main()
{
    const uint32_t crc = 0xF266B00Bu;
    SessionConfig hc;
    hc.name = "Sora";
    hc.gameCrc = crc;
    hc.port = 0; // any free port
    hc.simulatedLoss = 0.2f;

    Node host("host"), c1("c1"), c2("c2");
    CHECK(host.s.host(hc));
    Address ha;
    CHECK(Address::parse("127.0.0.1:" + std::to_string(host.s.port()), kDefaultPort, ha));

    SessionConfig cc = hc;
    cc.port = 0;
    cc.name = "Riku";
    CHECK(c1.s.join(cc, ha));
    cc.name = "Kairi";
    CHECK(c2.s.join(cc, ha));

    double now = 0;
    step({&host, &c1, &c2}, now, 120);
    CHECK(c1.s.status() == Session::Status::Connected);
    CHECK(c2.s.status() == Session::Status::Connected);
    CHECK(host.s.roster().size() == 3);
    CHECK(c1.s.roster().size() == 3);
    CHECK(c2.s.roster().size() == 3);
    CHECK(c1.s.localId() != c2.s.localId());
    std::printf("ids: c1=%u c2=%u\n", c1.s.localId(), c2.s.localId());

    // A fourth player is turned away; so is a different build.
    {
        Node c3("c3");
        SessionConfig c3c = cc;
        c3c.simulatedLoss = 0;
        c3.s.join(c3c, ha);
        step({&host, &c1, &c2, &c3}, now, 240);
        CHECK(c3.s.status() == Session::Status::Failed);
        std::printf("4th player: %s\n", c3.s.lastError().c_str());
    }

    // States and events flow both ways for 10 seconds with 20% loss.
    const int kEvents = 300;
    int sent = 0;
    for (int f = 0; f < 600; ++f)
    {
        now += 1.0 / 60.0;
        for (Node *n : {&host, &c1, &c2})
        {
            PlayerState st;
            st.tick = static_cast<uint32_t>(f);
            st.world = 2;
            st.pos[0] = static_cast<float>(f);
            st.hp = 100 + n->s.localId();
            if (f % 2 == 0)
                n->s.sendState(st);
            if (f % 3 == 0)
            {
                // The host's enemies; clients sending them must be ignored.
                EnemyStates es;
                es.tick = static_cast<uint32_t>(f);
                es.world = 2;
                es.room = 4;
                for (int k = 0; k < 3; ++k)
                {
                    EnemyState e;
                    e.serial = static_cast<int16_t>(39 + k);
                    e.entry = 0x13E;
                    e.pos[0] = static_cast<float>(f + k);
                    e.hp = 32 - k - (n == &host ? 0 : 100);
                    e.maxHp = 32;
                    es.enemies.push_back(e);
                }
                n->s.sendEnemies(es);
            }
            if (sent < kEvents && f % 2 == 0)
            {
                const uint32_t v = static_cast<uint32_t>(sent);
                n->s.sendEvent(7, std::vector<uint8_t>(reinterpret_cast<const uint8_t *>(&v),
                                                       reinterpret_cast<const uint8_t *>(&v) + 4));
            }
            n->s.update(now);
        }
        if (f % 2 == 0 && sent < kEvents)
            ++sent;
    }
    step({&host, &c1, &c2}, now, 300); // let resends finish
    for (Node *n : {&host, &c1, &c2})
    {
        const uint8_t self = n->s.localId();
        for (uint8_t origin : {uint8_t(0), c1.s.localId(), c2.s.localId()})
        {
            if (origin == self)
                continue;
            const auto &ev = n->events[origin];
            bool ordered = ev.size() == static_cast<size_t>(kEvents);
            for (size_t i = 0; ordered && i < ev.size(); ++i)
                ordered = ev[i] == i;
            if (!ordered)
                std::printf("%s from %u: %zu events\n", n->name.c_str(), origin, ev.size());
            CHECK(ordered);
            CHECK(n->lastState.count(origin));
            if (n->lastState.count(origin))
                CHECK(n->lastState[origin].hp == 100 + origin);
        }
    }
    // Clients have the host's enemies (and only the host's).
    for (Node *n : {&c1, &c2})
    {
        CHECK(n->enemyPackets > 50);
        CHECK(n->lastEnemies.enemies.size() == 3);
        if (n->lastEnemies.enemies.size() == 3)
        {
            CHECK(n->lastEnemies.room == 4);
            CHECK(n->lastEnemies.enemies[1].serial == 40);
            CHECK(n->lastEnemies.enemies[2].hp == 30);
        }
    }
    CHECK(host.enemyPackets == 0);
    for (const PeerInfo &p : host.s.roster())
        std::printf("host sees %u %s rtt=%.1fms loss=%.2f%s\n", p.id, p.name.c_str(), p.rttMs, p.loss,
                    p.local ? " (you)" : "");

    // c2 goes silent: the host drops it after the timeout and tells c1.
    const int c1LeavesBefore = c1.leaves;
    step({&host, &c1}, now, 60 * 7);
    CHECK(host.s.roster().size() == 2);
    CHECK(c1.s.roster().size() == 2);
    CHECK(c1.leaves == c1LeavesBefore + 1);
    CHECK(c1.s.status() == Session::Status::Connected);

    // The host leaving ends the session for clients.
    host.s.leave();
    step({&c1}, now, 30);
    CHECK(c1.s.status() == Session::Status::Failed);
    std::printf("c1 after host left: %s\n", c1.s.lastError().c_str());

    // Wrong game build.
    {
        Node h2("h2"), bad("bad");
        SessionConfig c = hc;
        c.simulatedLoss = 0;
        h2.s.host(c);
        Address a;
        Address::parse("127.0.0.1:" + std::to_string(h2.s.port()), kDefaultPort, a);
        c.gameCrc = 1;
        c.port = 0;
        bad.s.join(c, a);
        step({&h2, &bad}, now, 30);
        CHECK(bad.s.status() == Session::Status::Failed);
        std::printf("wrong build: %s\n", bad.s.lastError().c_str());
    }

    // Interpolation: halfway between two snapshots 2 ticks apart.
    {
        Interpolator in(0.1);
        PlayerState a, b;
        a.tick = 0;
        b.tick = 6;
        b.pos[0] = 60.0f;
        in.push(a, 10.0);
        in.push(b, 10.1);
        PlayerState s;
        CHECK(in.sample(10.15, s)); // shows t=10.05, half way
        CHECK(std::fabs(s.pos[0] - 30.0f) < 1.0f);
        std::printf("interpolated x=%.2f (expect 30)\n", s.pos[0]);
    }

    std::printf(g_failures ? "%d FAILURES\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
