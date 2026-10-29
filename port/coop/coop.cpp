#include "coop.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <deque>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

#include "enemies.h"
#include "partners.h"
#include "scaling.h"
#include "game_state.h"
#include "hits.h"
#include "mission.h"
#include "spells.h"
#include "roomlock.h"
#include "avatars.h"
#include "names.h"
#include "puppets.h"
#include "story.h"
#include "downed.h"
#include "cutscene_skip.h"
#include "host_input.h"
#include "pgs_renderer.h"
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
// Whether g_session exists, readable without g_mutex: active() runs on every
// pass of the host loop (~1000 a second) and almost always finds no session.
std::atomic<bool> g_sessionExists{false};
std::map<uint8_t, Remote> g_remotes;
std::deque<std::string> g_log;
uint64_t g_lastSentFrame = UINT64_MAX;
double g_lastWarp = -100.0;
double g_hostRoomSince = 0.0;
uint16_t g_hostRoomSeen = 0xFFFF;
bool g_followPending = false; // the host moved since we last followed
int g_rejoins = 0;            // automatic joins after losing the host
int g_lastMapState = -1;      // the port mapping's state as last logged
double g_lastRejoin = -100.0;
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
// Shared story (story.h): the host sends its story and where it is (the
// scene); clients play in it and watch its cutscenes.
constexpr size_t kSceneSize = 11; // world, room, door, map, battle, event, cutscene
bool g_storyFull = false;             // host: send all of it next time (someone joined)
uint8_t g_sceneSent[kSceneSize] = {}; // host: the scene as last sent
bool g_sceneSentValid = false;
uint8_t g_hostScene[kSceneSize] = {}; // client: the host's scene
bool g_haveHostScene = false;
std::map<uint8_t, std::vector<uint8_t>> g_spellsKnown; // player -> their spell levels
bool g_storyWarp = false;              // client: reload the room in the host's story
uint32_t g_sceneStarted = 0xFFFFFFFFu; // client: the host's cutscene we started here
double g_lastSceneWarp = -100.0;
double g_strayCutSince = -1.0;         // client: in a cutscene the host is not in, since
double g_lastClientSkip = -100.0;
bool g_backToHost = false;              // client: ran into a story scene of its own elsewhere; go back
double g_battleDiffSince = -1.0;      // client: the host's battle differs from ours, since
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
// map: the room's map program (0xFFFF: the save's). A jump into a fight
// takes the host's: a mission's (08-03 map 71) places the party itself, and
// with the default one as well everyone was there twice.
void warpLocked(uint8_t world, uint8_t room, uint16_t door, uint16_t battle = 0xFFFF, uint16_t event = 0xFFFF,
                uint16_t map = 0xFFFF)
{
    addLog("going to " + kh2state::describe({world, room}));
    kh2guest::post([world, room, door, battle, event, map](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
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
        // One map jump at a time: a second one queued while the first is
        // pending (a story warp and a room reload in the same tick) crashed
        // the game in the room change.
        {
            uint32_t pending;
            std::memcpy(&pending, env.host(kh2state::addr::MapJumpTask), 4);
            if (pending)
            {
                std::printf("[coop] map jump to %02X-%02X dropped: another is under way\n", world, room);
                std::fflush(stdout);
                return std::nullopt;
            }
        }
        uint8_t loc[10];
        loc[0] = world;
        loc[1] = room;
        std::memcpy(loc + 2, &door, 2);
        const uint16_t keep = 0xFFFF; // map, battle, event: what the save says
        for (int i = 0; i < 3; ++i)
            std::memcpy(loc + 4 + 2 * i, &keep, 2);
        std::memcpy(loc + 4, &map, 2);
        if (const char *m = std::getenv("KH2_WARP_MAP")) // testing: this map program
        {
            const uint16_t map = uint16_t(std::atoi(m));
            std::memcpy(loc + 4, &map, 2);
        }
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

// Which player an object here is (0xFF: none of them; an enemy, a party member).
uint8_t playerIdOf(const uint8_t *rdram, uint32_t obj)
{
    uint32_t player = 0;
    std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
    if (obj && obj == player)
        return g_session ? g_session->localId() : 0xFF;
    for (const kh2partner::Link &l : kh2partner::links())
        if (l.object == obj && obj)
            return l.playerId;
    for (const kh2puppets::Assignment &a : kh2puppets::assignments())
        if (a.object == obj && obj)
            return a.playerId;
    return 0xFF;
}

void encodeScene(const kh2state::State &st, uint8_t (&out)[kSceneSize])
{
    out[0] = st.now.world;
    out[1] = st.now.room;
    std::memcpy(out + 2, &st.now.door, 2);
    std::memcpy(out + 4, &st.now.map, 2);
    std::memcpy(out + 6, &st.now.battle, 2);
    std::memcpy(out + 8, &st.now.event, 2);
    out[10] = st.control == 3 ? 1 : 0;
}

// Client: the session is over (or shared story turned off); our own story
// comes back, and we go back to where we were when the host's began.
void endStory(uint8_t *ram)
{
    kh2story::Place back;
    if (kh2story::end(ram, back))
    {
        addLog("back in your own story");
        if (back.world != 0)
            warpLocked(back.world, back.room, back.door);
    }
    kh2skip::setLocalSkip(true);
    kh2input::setBlockedButtons(0);
    g_haveHostScene = false;
    g_storyWarp = false;
    g_sceneStarted = 0xFFFFFFFFu;
    g_strayCutSince = -1.0;
    g_backToHost = false;
}

std::string hostName()
{
    auto it = g_remotes.find(0);
    return it != g_remotes.end() && !it->second.name.empty() ? it->second.name : std::string("The host");
}

// Shared story, every tick in a session.
void storyTick(uint8_t *ram, uint64_t guestFrame)
{
    const kh2state::State me = kh2state::read(ram);
    if (g_session->isHost())
    {
        if (!g_opt.sharedStory)
            return;
        // The story: what changed, 4 times a second (all of it for newcomers).
        if (g_storyFull || guestFrame % 15 == 0)
        {
            for (const std::vector<uint8_t> &p : kh2story::hostCollect(ram, g_storyFull))
                g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::StoryChunk), p);
            g_storyFull = false;
        }
        // Where we are and whether a cutscene is playing, when it changes.
        uint8_t sc[kSceneSize];
        encodeScene(me, sc);
        if (!g_sceneSentValid || std::memcmp(sc, g_sceneSent, kSceneSize) != 0)
        {
            std::memcpy(g_sceneSent, sc, kSceneSize);
            g_sceneSentValid = true;
            g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::Scene),
                                 std::vector<uint8_t>(sc, sc + kSceneSize));
        }
        return;
    }
    if (!g_opt.sharedStory)
    {
        endStory(ram);
        return;
    }
    uint32_t jumping;
    std::memcpy(&jumping, ram + kh2state::addr::MapJumpTask, 4);
    const bool cut = kh2skip::inCutscene(ram);
    if (!kh2story::active())
        kh2story::setReturnPlace({me.now.world, me.now.room, me.now.door});
    // In a room of the game (past the title screen and the new-game set-up)?
    const bool inGame = me.player.valid && me.now.world != 0 && me.now.world != 0xFF;
    // (Not while we fight: a fight this game started on its own runs on its
    // own story flags until it is over.)
    if (kh2story::clientTick(ram, inGame && me.control == 0 && !jumping && me.battle == 0,
                             inGame && me.control == 0 && !jumping))
    {
        addLog("playing in the host's story");
        g_storyWarp = true; // the room's programs are the host's now: load it again
    }
    if (!kh2story::active())
        return;
    // Only the host skips: ours is a vote; the pause menu's Skip is out of reach.
    kh2skip::setLocalSkip(false);
    kh2input::setBlockedButtons(cut ? 0x0008u : 0u);
    if (kh2skip::takeSkipVote())
        g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::SkipVote), {});
    if (!g_haveHostScene)
        return;
    const uint8_t hw = g_hostScene[0], hr = g_hostScene[1];
    if (hw == 0 || hw == 0xFF || me.now.world == 0 || me.now.world == 0xFF)
        return; // the host (or we) between rooms or not in the game
    uint16_t hdoor, hbattle, hevent;
    std::memcpy(&hdoor, g_hostScene + 2, 2);
    std::memcpy(&hbattle, g_hostScene + 6, 2);
    std::memcpy(&hevent, g_hostScene + 8, 2);
    uint16_t hmap;
    std::memcpy(&hmap, g_hostScene + 4, 2);
    const bool hostCut = g_hostScene[10] != 0 && hevent != 0 && hevent != 0xFFFF;
    if (g_storyWarp && hw != 0 && me.control == 0 && !jumping)
    {
        g_storyWarp = false;
        g_lastWarp = now();
        // With the host's fight, if any, in the same jump: loading the room
        // again for it a moment later (joining the host's battle) left the
        // first load's party there too (two Soras, two Goofys).
        if (!hostCut && hbattle != 0xFFFF && hbattle != 0)
        {
            g_lastSceneWarp = now();
            warpLocked(hw, hr, hdoor, hbattle, hevent, hmap);
        }
        else
            warpLocked(hw, hr, hdoor);
        return;
    }
    const uint32_t hostKey = uint32_t(hw) << 24 | uint32_t(hr) << 16 | hevent;
    const uint32_t myKey = uint32_t(me.now.world) << 24 | uint32_t(me.now.room) << 16 | me.now.event;
    // A cutscene the host is not in, wherever we are: it ended or was
    // skipped there, or our own game started one (its own story's, which is
    // not the one being played). The host's game leads.
    if (cut && !(hostCut && myKey == hostKey))
    {
        if (g_strayCutSince < 0)
            g_strayCutSince = now();
        // (Not a moment of control 3 that is no cutscene: a form change
        // (YS::CHANGEFORM's task at 0x00351184) or a reward screen still to
        // show (dk::TREASURE_INFO's pending infos at 0x00347D58); real
        // cutscenes last longer than two seconds.)
        uint32_t formTask = 0, infos = 0;
        std::memcpy(&formTask, ram + 0x00351184u, 4);
        std::memcpy(&infos, ram + 0x00347D58u, 4);
        if (formTask || infos)
            g_strayCutSince = now();
        if (now() - g_strayCutSince > 2.0 && now() - g_lastClientSkip > 3.0)
        {
            g_lastClientSkip = now();
            if (kh2skip::skipCutscene(ram))
            {
                addLog(hostCut ? "skipped: the host is in another cutscene" : "skipped: the host is not watching this cutscene");
                // Ahead of the host, in a room of its own: the story (and
                // its boss) waits for the host. Back to them, like a
                // guest at a boss door.
                if (!hostCut && (hw != me.now.world || hr != me.now.room))
                    g_backToHost = true;
            }
        }
    }
    else
        g_strayCutSince = -1.0;
    if (g_backToHost && (hw != me.now.world || hr != me.now.room) && !cut && me.control == 0 && !jumping &&
        now() - g_lastSceneWarp > 1.0)
    {
        g_backToHost = false;
        g_lastSceneWarp = now();
        addLog("the story goes on with the host: back to them");
        kh2pgs::toast(hostName() + " leads the story: this part starts when they get here", 5.0);
        warpLocked(hw, hr, hdoor);
        return;
    }
    if (hw == me.now.world && hr == me.now.room)
        g_backToHost = false;
    if (hw != me.now.world || hr != me.now.room)
    {
        // Elsewhere while the host's cutscene starts: come and watch it.
        if (hostCut && !cut && me.control == 0 && !jumping && g_sceneStarted != hostKey &&
            now() - g_lastSceneWarp > 3.0)
        {
            g_sceneStarted = hostKey;
            g_lastSceneWarp = now();
            addLog("joining the host's cutscene");
            warpLocked(hw, hr, hdoor, hbattle, hevent, hmap);
        }
        return;
    }
    if (!hostCut)
        g_sceneStarted = 0xFFFFFFFFu;
    // The host's cutscene. One that starts with the room's event plays here
    // too when the room loads again with that event. One a script started
    // inside the event we both run (a talk, say) cannot be started that way
    // yet: we say so, and catch up with whatever follows it.
    if (hostCut && !cut && !jumping && g_sceneStarted != hostKey)
    {
        if (hevent != me.now.event && now() - g_lastSceneWarp > 3.0)
        {
            g_sceneStarted = hostKey;
            g_lastSceneWarp = now();
            addLog("watching the host's cutscene");
            warpLocked(hw, hr, hdoor, hbattle, hevent, hmap);
        }
        else if (hevent == me.now.event)
        {
            g_sceneStarted = hostKey;
            kh2pgs::toast("The host is watching a cutscene");
            addLog("the host is watching a cutscene (not shown here yet)");
        }
    }
    // The host's battle (enemy groups) in this room, once it has stayed
    // different a moment: the room again with it, so its enemies are here.
    if (!hostCut && !cut && hbattle != 0xFFFF && hbattle != me.now.battle && me.control == 0 && !jumping &&
        me.battle == 0)
    {
        if (g_battleDiffSince < 0)
            g_battleDiffSince = now();
        else if (now() - g_battleDiffSince > 1.0 && now() - g_lastSceneWarp > 4.0)
        {
            g_lastSceneWarp = now();
            g_battleDiffSince = -1.0;
            addLog("joining the host's battle");
            warpLocked(hw, hr, hdoor, hbattle, hevent, hmap);
        }
    }
    else
        g_battleDiffSince = -1.0;
}

void makeSession()
{
    g_session = std::make_unique<kh2net::Session>();
    g_sessionExists.store(true, std::memory_order_release);
    g_remotes.clear();
    g_session->onLog = [](const std::string &s) { addLog(s); };
    g_session->onPeer = [](uint8_t id, const std::string &name, bool joined) {
        if (joined)
        {
            g_remotes[id].name = name;
            g_resendLoadout = true; // the newcomer needs ours
            if (g_session && g_session->isHost())
            {
                g_storyFull = true; // and the story, and where we are
                g_sceneSentValid = false;
            }
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
        if (type == static_cast<uint16_t>(kh2net::GameEventType::StoryChunk) && !g_session->isHost())
        {
            if (g_opt.sharedStory)
                kh2story::onChunk(data);
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::Scene) && !g_session->isHost())
        {
            if (data.size() == kSceneSize)
            {
                std::memcpy(g_hostScene, data.data(), kSceneSize);
                g_haveHostScene = true;
            }
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::EnemyScaling) && !g_session->isHost())
        {
            kh2scale::onHostSettings(data);
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::SkipVote) && g_session->isHost())
        {
            auto it = g_remotes.find(origin);
            const std::string who = it != g_remotes.end() && !it->second.name.empty() ? it->second.name : "A player";
            addLog(who + " asks to skip the cutscene");
            kh2pgs::toast(who + " asks to skip the cutscene (hold B to skip)");
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::SpellsKnown) && data.size() == 6)
        {
            g_spellsKnown[origin] = data;
            std::vector<uint8_t> all(6, 0);
            for (const auto &[id, levels] : g_spellsKnown)
                for (size_t i = 0; i < 6; ++i)
                    all[i] = std::max(all[i], levels[i]);
            kh2spells::setOthersKnow(all, kh2avatars::enabled());
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::SpellCast) && g_rdram)
        {
            kh2spells::Cast c;
            uint8_t world, room;
            if (!kh2spells::decode(data, c, world, room))
                return;
            const kh2state::State st = kh2state::read(g_rdram);
            if (st.now.world != world || st.now.room != room)
                return;
            uint32_t standIn = 0;
            for (const kh2partner::Link &l : kh2partner::links())
                if (l.playerId == origin && l.object)
                    standIn = l.object;
            for (const kh2puppets::Assignment &a : kh2puppets::assignments())
                if (!standIn && a.playerId == origin && a.object)
                    standIn = a.object;
            // (Work in progress, off by default: on a stand-in the cast runs
            // but shows nothing yet; KH2_SPELL_REPLAY=1 turns it on.)
            static const bool replay = [] {
                const char *e = std::getenv("KH2_SPELL_REPLAY");
                return e && *e == '1';
            }();
            if (kh2avatars::enabled())
            {
                if (const uint32_t avatar = kh2avatars::objectFor(origin))
                    kh2spells::castAsPlayer(avatar, c);
            }
            else if (standIn && replay)
                kh2spells::castAs(standIn, c);
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::MissionState) && !g_session->isHost() && g_rdram &&
            kh2story::active())
        {
            const kh2state::State st = kh2state::read(g_rdram);
            kh2mission::onHostState(const_cast<uint8_t *>(g_rdram), data, st.now.world, st.now.room, st.now.battle);
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::MissionFailed) && !g_session->isHost() && g_rdram &&
            kh2story::active())
        {
            addLog("the host's mission failed");
            kh2mission::onHostFailed(const_cast<uint8_t *>(g_rdram));
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::EnemyKill) && !g_session->isHost() && g_rdram &&
            g_opt.sharedEnemies && !data.empty())
        {
            // The blow that killed one of the host's enemies: our copy dies of it.
            uint8_t world, room;
            int16_t serial;
            uint16_t entry;
            kh2hits::Hit hit;
            const uint8_t by = data.back();
            const std::vector<uint8_t> body(data.begin(), data.end() - 1);
            if (!kh2hits::decode(body, world, room, serial, entry, hit))
                return;
            kh2enemies::onHostKill(const_cast<uint8_t *>(g_rdram), world, room, serial, entry, hit,
                                   attackerFor(g_rdram, by == 0xFF ? g_session->localId() : by), now());
            return;
        }
        if (type == static_cast<uint16_t>(kh2net::GameEventType::EnemyHit) && g_session->isHost() && g_rdram &&
            g_opt.sharedEnemies)
        {
            // A client's hit: its reaction, played on our copy of the enemy.
            uint8_t world, room;
            int16_t serial;
            uint16_t entry;
            kh2hits::Hit hit;
            if (!kh2hits::decode(data, world, room, serial, entry, hit))
                return;
            const kh2state::State st = kh2state::read(g_rdram);
            if (st.now.world != world || st.now.room != room)
                return;
            for (const kh2enemies::Local &e : kh2enemies::scan(g_rdram))
                if (e.serial == serial && e.entry == entry && e.hp > 0)
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
    c.openPort = g_opt.openPort;
    c.discover = true;
    c.preferredId = g_opt.preferredId;
    return c;
}
} // namespace

bool host(const std::string &name, uint16_t port)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_opt.name = name;
    g_opt.port = port;
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
    g_opt.join = address;
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
    g_sessionExists.store(false, std::memory_order_release);
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
    else if (!options.join.empty() && !options.joinAtFrame)
        join(options.name, options.join);
}

void shutdown()
{
    leave();
    // Let the router hear that the port is closed again before we go.
    kh2net::PortMapper::waitForAll(3000);
}

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
        // Testing: another weapon than ours (KH2_TEST_PARTNER_WEAPON=077D, hex).
        static const uint16_t testWeapon = [] {
            const char *e = std::getenv("KH2_TEST_PARTNER_WEAPON");
            return e ? uint16_t(std::strtoul(e, nullptr, 16)) : uint16_t(0);
        }();
        if (testWeapon && me.weapon)
            me.weapon = testWeapon;
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
            p.rideSerial = ps.rideSerial;
            p.rideEntry = ps.rideEntry;
            p.rideJoint = ps.rideJoint;
            p.rideFlags = ps.rideFlags;
            p.rideMotion = ps.rideMotion;
            p.hp = ps.hp;
            p.maxHp = ps.maxHp;
            p.mp = ps.mp;
            p.maxMp = ps.maxMp;
            p.loadout = r.loadout;
            list.push_back(p);
        }
    // Avatars (avatars.h): second players for them; the partners stand down.
    std::vector<kh2partner::Heal> heals;
    if (kh2avatars::enabled())
    {
        heals = kh2avatars::update(ram, list);
        list.clear();
    }
    for (const kh2partner::Heal &h : kh2partner::update(ram, list))
        heals.push_back(h);
    if (kh2avatars::enabled())
        kh2names::set(kh2avatars::names());
    kh2partner::setFormsFree(kh2avatars::enabled() && g_session && g_session->active());
    for (const kh2partner::Heal &h : heals)
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
        if (h.hp < 0)
            addLog("player " + std::to_string(h.playerId + 1) + " was hit here for " + std::to_string(-h.hp) + " HP");
        else
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
    // --join-at: join now (as a player would once their game is running).
    if (g_opt.joinAtFrame && guestFrame >= g_opt.joinAtFrame && !g_opt.join.empty())
    {
        g_opt.joinAtFrame = 0;
        join(g_opt.name, g_opt.join);
    }
    // A client that lost the host (not one turned away) tries again for a
    // minute: a game stalled by loading, a network hiccup.
    {
        bool retry = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_session && g_session->status() == kh2net::Session::Status::Failed && !g_opt.join.empty() &&
                !g_opt.host && g_rejoins < 12 && now() - g_lastRejoin > 5.0)
            {
                const std::string &e = g_session->lastError();
                retry = e.rfind("lost the host (timed out)", 0) == 0 || e.rfind("no answer", 0) == 0;
            }
        }
        if (retry)
        {
            ++g_rejoins;
            g_lastRejoin = now();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                addLog("joining again (" + std::to_string(g_rejoins) + ")");
            }
            join(g_opt.name, g_opt.join);
        }
    }
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
        kh2downed::reset(ram);
        endStory(ram);
        kh2puppets::update(rdram, {});
        kh2scale::tick(ram, false, false, 1);
        return;
    }
    const auto before = g_session->status();
    g_rdram = rdram;
    g_session->update(now());
    g_rdram = nullptr;
    if (g_session->status() == kh2net::Session::Status::Failed && before != kh2net::Session::Status::Failed)
        addLog(g_session->lastError());
    if (g_session->status() == kh2net::Session::Status::Connected)
        g_rejoins = 0;
    // What the router said about the port (host), once.
    if (g_session->isHost())
    {
        const kh2net::PortMapper::Status pm = g_session->portMapping();
        const int state = static_cast<int>(pm.state);
        if (state != g_lastMapState)
        {
            g_lastMapState = state;
            if (pm.state == kh2net::PortMapper::State::Mapped)
                addLog("the router opened port " + std::to_string(pm.external.port) + " (" + pm.method + "); others join at " +
                       g_session->shareAddress().str() + (pm.detail.empty() ? "" : " - " + pm.detail));
            else if (pm.state == kh2net::PortMapper::State::Failed)
                addLog("the router did not open the port: " + pm.detail);
        }
    }
    if (!g_session->active())
    {
        kh2downed::reset(ram);
        endStory(ram);
        kh2hits::setCapture(false);
        kh2spells::setCapture(false);
        kh2hits::setBlockedAttackers({});
        kh2hits::setProtectedTargets({});
        kh2spells::setOthersKnow({}, false);
        if (kh2avatars::enabled())
            kh2avatars::reset(ram);
        kh2puppets::update(rdram, {});
        kh2scale::tick(ram, false, false, 1);
        return;
    }
    // Enemy HP and damage scaling (scaling.h): the host's settings, sent
    // every two seconds (also to whoever just joined).
    kh2scale::tick(ram, true, g_session->isHost(), int(g_remotes.size()) + 1);
    if (g_session->isHost() && guestFrame % 120 == 0)
        g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::EnemyScaling), kh2scale::encode());
    updatePartners(ram, guestFrame);
    storyTick(ram, guestFrame);
    // Spells (spells.h): ours go to the others, whose stand-in of us casts
    // them for the look of it; a stand-in's hits do nothing here.
    {
        std::vector<uint32_t> standIns;
        for (const kh2partner::Link &l : kh2partner::links())
            if (l.object)
                standIns.push_back(l.object);
        for (const kh2puppets::Assignment &a : kh2puppets::assignments())
            if (a.object)
                standIns.push_back(a.object);
        std::vector<uint32_t> avatars = kh2avatars::objects();
        standIns.insert(standIns.end(), avatars.begin(), avatars.end());
        kh2hits::setProtectedTargets(avatars);
        kh2hits::setBlockedAttackers(std::move(standIns));
        kh2spells::setCapture(true);
        if (guestFrame % 120 == 60)
            g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::SpellsKnown), kh2spells::knownSpells());
        const kh2state::State st = kh2state::read(ram);
        for (const kh2spells::Cast &c : kh2spells::takeCasts())
            g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::SpellCast),
                                 kh2spells::encode(c, st.now.world, st.now.room));
    }
    // Missions (mission.h): the host's leads.
    kh2mission::setFollowing(!g_session->isHost() && kh2story::active());
    if (g_session->isHost())
    {
        if (kh2mission::takeFailed())
            g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::MissionFailed), {});
        if (guestFrame % 10 == 0)
        {
            const kh2state::State st = kh2state::read(ram);
            std::vector<uint8_t> m = kh2mission::snapshot(ram, st.now.world, st.now.room, st.now.battle);
            if (!m.empty())
                g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::MissionState), m);
        }
    }
    // Downed players (downed.h): who else is here, and who is down.
    {
        std::vector<kh2downed::Other> others;
        const double t = now();
        for (auto &[id, r] : g_remotes)
        {
            kh2net::PlayerState ps;
            kh2downed::Other o;
            o.name = r.name.empty() ? "Player " + std::to_string(id + 1) : r.name;
            if (r.interp.sample(t, ps) && t - r.interp.lastArrival() < 5.0)
            {
                o.present = (ps.flags & 1) != 0;
                o.downed = (ps.flags & 2) != 0;
            }
            others.push_back(o);
        }
        kh2downed::tick(ram, !others.empty(), others);
    }
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
            // Other players are always their stand-in: the party member in
            // their party slot, made for rooms that have none (partners.cpp).
            // The NPC copies (puppets.h) behaved differently (animation speed,
            // no weapon), so they are only a diagnostic now: KH2_NPC_PUPPETS=1
            // shows them for players without a stand-in here.
            static const bool npcPuppets = [] {
                const char *e = std::getenv("KH2_NPC_PUPPETS");
                return e && *e == '1';
            }();
            if (!npcPuppets)
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
    {
        kh2net::PlayerState host;
        const bool lead = (g_opt.followHost || kh2story::active()) && !g_session->isHost() && hostState(host) &&
                          host.world != 0 && host.world != 0xFF;
        kh2roomlock::set(lead, lead ? host.world : 0xFF, lead ? host.room : 0xFF);
    }
    if ((g_opt.followHost || kh2story::active()) && !g_session->isHost())
    {
        kh2net::PlayerState host;
        const kh2state::State me = kh2state::read(rdram);
        uint32_t jumping;
        std::memcpy(&jumping, rdram + kh2state::addr::MapJumpTask, 4);
        const bool meInGame = me.player.valid && me.now.world != 0 && me.now.world != 0xFF;
        if (hostState(host) && host.world != 0 && host.world != 0xFF && meInGame)
        {
            // Follow the host when *they* move (and once on joining): in
            // between, a client may wander off on their own.
            const uint16_t key = uint16_t(host.world << 8 | host.room);
            if (key != g_hostRoomSeen)
            {
                g_hostRoomSeen = key;
                g_hostRoomSince = now();
                g_followPending = true;
            }
            const bool elsewhere = host.world != me.now.world || host.room != me.now.room;
            if (!elsewhere)
                g_followPending = false;
            else if (g_followPending && me.control == 0 && me.battle == 0 && !jumping && now() - g_hostRoomSince > 1.0 &&
                     now() - g_lastWarp > 5.0)
            {
                g_lastWarp = now();
                g_followPending = false;
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
            // Killing blows first, so they arrive with (or before) the 0 HP.
            for (const kh2hits::Hit &h : kh2hits::takeKills())
                for (const kh2enemies::Local &e : kh2enemies::scan(rdram))
                    if (e.object == h.object)
                    {
                        std::vector<uint8_t> data = kh2hits::encode(me.now.world, me.now.room, e.serial, e.entry, h);
                        data.push_back(playerIdOf(rdram, h.attacker));
                        g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::EnemyKill), data);
                        break;
                    }
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
            // (Only for copies of the host's enemies: one this game spawned on
            // its own has no counterpart there.)
            for (const kh2hits::Hit &h : kh2hits::takeHits())
            {
                int16_t serial;
                uint16_t entry;
                if (h.lethal)
                    kh2enemies::onOwnKill(h.object);
                if (kh2enemies::followed(h.object, serial, entry))
                    g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::EnemyHit),
                                         kh2hits::encode(me.now.world, me.now.room, serial, entry, h));
            }
            for (const kh2enemies::Damage &d :
                 kh2enemies::clientTick(const_cast<uint8_t *>(rdram), me.now.world, me.now.room, now()))
                g_session->sendEvent(static_cast<uint16_t>(kh2net::GameEventType::EnemyDamage),
                                     kh2enemies::encodeDamage(me.now.world, me.now.room, d));
        }
    }
    kh2hits::setCapture(g_opt.sharedEnemies && !g_session->isHost());
    kh2hits::setKillCapture(g_opt.sharedEnemies && g_session->isHost());
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
        kh2partner::localRide(rdram, ps);
    }
    if (kh2downed::downed())
        ps.flags |= 2; // down (downed.h)
    ps.hp = st.party[0].hp;
    ps.maxHp = st.party[0].maxHp;
    ps.mp = st.party[0].mp;
    ps.maxMp = st.party[0].maxMp;
    ps.weapon = kh2partner::localWeaponEntry(rdram);
    g_session->sendState(ps);
}

bool active()
{
    if (!g_sessionExists.load(std::memory_order_acquire))
        return false;
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

NatInfo natInfo()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    NatInfo n;
    if (!g_session)
        return n;
    using S = kh2net::Session::Status;
    n.hosting = g_session->status() == S::Hosting;
    n.connecting = g_session->status() == S::Connecting;
    if (g_session->publicAddress().valid())
        n.mine = g_session->publicAddress().str();
    if (n.hosting)
    {
        const kh2net::Address share = g_session->shareAddress();
        if (share.valid() && share.ip != 0)
            n.share = share.str();
        const kh2net::PortMapper::Status pm = g_session->portMapping();
        using M = kh2net::PortMapper::State;
        switch (pm.state)
        {
        case M::Idle:
            n.router = "Not asking the router to open the port (forward UDP " + std::to_string(g_session->port()) +
                       " yourself, or use Let in)";
            break;
        case M::Working:
            n.router = "Asking the router to open the port...";
            break;
        case M::Mapped:
            n.router = "The router opened the port (" + pm.method + ")" + (pm.detail.empty() ? "" : ": " + pm.detail);
            break;
        case M::Failed:
            n.router = "The router did not open the port (" + pm.detail + "). Players outside your network: "
                       "forward UDP " + std::to_string(g_session->port()) + " on the router, or use Let in";
            break;
        }
    }
    return n;
}

bool letIn(const std::string &address)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    kh2net::Address a;
    if (!g_session || !g_session->isHost() || !kh2net::Address::parse(address, kh2net::kDefaultPort, a))
        return false;
    g_session->invite(a);
    return true;
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
void setSharedStory(bool on)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_opt.sharedStory = on;
}
bool sharedStory() { return g_opt.sharedStory; }
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
