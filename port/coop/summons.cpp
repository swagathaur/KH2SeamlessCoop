#include "summons.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "formcache.h"
#include "game_state.h"
#include "net/protocol.h"
#include "partners.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "puppets.h"

namespace kh2summons
{
namespace
{
// FUN_001C9AB8: may we summon? True if FUN_001BC010 says so, or if both
// friends (YS::FRIEND::Get(0), Get(1)) are there and can make way
// (YS::FRIEND::can_summon_sacrifice, 0x0017E3B8). The summon command's state
// (FUN_001C9B38, command type 0x48) is 2, shown as "?", when it is false.
constexpr uint32_t kCanSummon = 0x001C9AB8;
PS2Runtime::RecompiledFunction g_canSummon = nullptr;

void canSummonHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kCanSummon)
    {
        SET_GPR_U32(ctx, 2, 1);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g_canSummon(rdram, ctx, runtime);
}

// kn::ObjObserver's destructor (its vtable's slot 1) starts at 0x00135F70
// with "lui $t7, 0x34"; the function map starts it one instruction later
// (FUN_00135F74, which goes on from $t7). A call to the real start had no
// function and was skipped (a null call, from 0x0010C894 as a party member
// let go of its observers when a summon sent it away): the observer stayed
// linked in the game's list of them (0x00347D08) after its memory was freed.
constexpr uint32_t kObserverDtor = 0x00135F70, kObserverDtorBody = 0x00135F74;
PS2Runtime::RecompiledFunction g_observerDtorBody = nullptr;
void observerDtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    SET_GPR_U32(ctx, 15, 0x00340000u);
    ctx->pc = kObserverDtorBody;
    g_observerDtorBody(rdram, ctx, runtime);
}

// YS::SUMMON: the summon's object, and its mode (0: none out).
constexpr uint32_t kSummonObject = 0x01D49960, kSummonMode = 0x01D49968;
constexpr uint8_t kSummonType = 19; // objentry type of the summons' class
constexpr uint32_t kNoAct = 0x200000u; // +0x10C: its own action off

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }
bool live(const uint8_t *rdram, uint32_t obj)
{
    if (!obj)
        return false;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.address == obj)
            return true;
    return false;
}
double nowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A remote player's summon shown here.
struct Shown
{
    uint16_t look = 0;     // the summon's objentry id it wears
    uint32_t object = 0;   // our NPC showing it (0: none yet)
    bool pending = false;  // being made
    int slot = -1;         // its private entry (puppets.h summonEntry)
    uint16_t motion = 0xFFFF;
    int sinceMotion = 0;
    double noRoomAt = -100;
};
std::mutex g_mutex;
std::map<uint8_t, Shown> g_shown; // by player id
uint32_t g_gen = 1;               // a spawn answered after reset() is dropped
bool g_dressed[2] = {false, false};
uint16_t g_dressedAs[2] = {0, 0};

void take(Shown &s)
{
    if (s.object)
    {
        std::printf("[summons] a remote summon (look %04X) goes: %08X\n", s.look, s.object);
        std::fflush(stdout);
        kh2puppets::remove(s.object);
    }
    s.object = 0;
    s.pending = false;
    s.motion = 0xFFFF;
}
} // namespace

void localSummon(const uint8_t *rdram, kh2net::PlayerState &ps)
{
    ps.summon = 0;
    if (rd<uint32_t>(rdram, kSummonMode) == 0)
        return;
    const uint32_t obj = rd<uint32_t>(rdram, kSummonObject);
    if (!inRam(obj) || !live(rdram, obj))
        return;
    const uint32_t rec = rd<uint32_t>(rdram, obj + kh2state::addr::ObjEntry);
    if (!inRam(rec) || rd<uint8_t>(rdram, rec + 4) != kSummonType)
        return;
    ps.summon = uint16_t(rd<uint32_t>(rdram, rec) & 0xFFFF);
    for (int i = 0; i < 3; ++i)
        ps.summonPos[i] = rd<float>(rdram, obj + kh2state::addr::ObjPos + 4u * uint32_t(i));
    ps.summonYaw = rd<float>(rdram, obj + kh2state::addr::ObjDir + 12);
    ps.summonMotion = uint16_t(rd<uint32_t>(rdram, obj + kh2state::addr::ObjMotionIndex));
    static uint16_t logged = 0;
    if (logged != ps.summon)
    {
        logged = ps.summon;
        std::printf("[summons] our summon %08X (look %04X) goes out with our state\n", obj, ps.summon);
        std::fflush(stdout);
    }
}

void update(uint8_t *rdram, const std::vector<kh2partner::Remote> &remotes)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const kh2state::State st = kh2state::read(rdram);
    const double t = nowSec();
    const bool cutscene = st.control == 3;
    std::vector<kh2puppets::Override> overrides;
    std::vector<std::pair<uint32_t, int32_t>> guards;
    for (const kh2partner::Remote &r : remotes)
    {
        Shown &s = g_shown[r.playerId];
        const bool here = r.present && st.player.valid && r.world == st.now.world && r.room == st.now.room &&
                          !cutscene && r.summon != 0;
        if (s.object && !live(rdram, s.object))
        {
            s.object = 0; // (gone with the room)
            s.motion = 0xFFFF;
        }
        if (!here || (s.object && s.look != r.summon))
        {
            take(s);
            if (!here)
                continue;
        }
        if (!s.object)
        {
            if (s.pending || t - s.noRoomAt < 5.0)
                continue;
            // A private entry of ours: the one this player had, or a free one.
            if (s.slot < 0)
            {
                bool used[2] = {false, false};
                for (auto &[id, o] : g_shown)
                    if (o.slot >= 0 && o.slot < 2)
                        used[o.slot] = true;
                s.slot = !used[0] ? 0 : !used[1] ? 1 : -1;
                if (s.slot < 0)
                    continue;
            }
            const kh2formcache::Look look = kh2formcache::want(rdram, r.summon);
            if (look == kh2formcache::Look::NoRoom)
            {
                std::printf("[summons] no room for player %u's summon look %04X here\n", r.playerId + 1, r.summon);
                std::fflush(stdout);
                s.noRoomAt = t;
                continue;
            }
            if (look == kh2formcache::Look::Loading)
                continue;
            const uint32_t entry = kh2puppets::summonEntry(s.slot);
            if (!g_dressed[s.slot] || g_dressedAs[s.slot] != r.summon)
            {
                kh2puppets::requestDress(entry, r.summon);
                g_dressed[s.slot] = true;
                g_dressedAs[s.slot] = r.summon;
            }
            s.pending = true;
            s.look = r.summon;
            const uint32_t gen = g_gen;
            const uint8_t id = r.playerId;
            std::printf("[summons] showing player %u's summon (look %04X, entry %04X)\n", id + 1, r.summon, entry);
            std::fflush(stdout);
            kh2puppets::spawnAt(entry, r.summonPos, r.summonYaw, [id, gen](uint32_t obj) {
                std::lock_guard<std::mutex> lock(g_mutex);
                Shown &s2 = g_shown[id];
                s2.pending = false;
                if (gen != g_gen || !obj)
                {
                    if (obj)
                        kh2puppets::remove(obj);
                    return;
                }
                s2.object = obj;
                s2.motion = 0xFFFF;
                std::printf("[summons] player %u's summon is shown by %08X\n", id + 1, obj);
                std::fflush(stdout);
            });
            continue;
        }
        const uint32_t obj = s.object;
        kh2formcache::want(rdram, s.look); // (its files stay while it wears them)
        const uint32_t flags = rd<uint32_t>(rdram, obj + 0x10C);
        if (!(flags & kNoAct))
            wr<uint32_t>(rdram, obj + 0x10C, flags | kNoAct);
        kh2puppets::Override o;
        o.object = obj;
        std::memcpy(o.pos, r.summonPos, sizeof(o.pos));
        o.yaw = r.summonYaw;
        overrides.push_back(o);
        guards.emplace_back(obj, int32_t(r.summonMotion));
        const uint32_t playing = rd<uint32_t>(rdram, obj + kh2state::addr::ObjMotionIndex);
        ++s.sinceMotion;
        if (r.summonMotion != s.motion || (playing != r.summonMotion && s.sinceMotion > 4))
        {
            s.motion = r.summonMotion;
            s.sinceMotion = 0;
            kh2puppets::postMotion(obj, r.summonMotion, 4.0f);
        }
    }
    // Players who left the session.
    for (auto it = g_shown.begin(); it != g_shown.end();)
    {
        bool still = false;
        for (const kh2partner::Remote &r : remotes)
            still = still || r.playerId == it->first;
        if (!still)
        {
            take(it->second);
            it = g_shown.erase(it);
        }
        else
            ++it;
    }
    kh2puppets::setOverrides(std::move(overrides), 3);
    kh2puppets::setMotionGuards(guards, 2);
}

std::vector<uint32_t> objects()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<uint32_t> out;
    for (auto &[id, s] : g_shown)
        if (s.object)
            out.push_back(s.object);
    return out;
}

void reset(uint8_t *rdram)
{
    (void)rdram;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[id, s] : g_shown)
        take(s);
    g_shown.clear();
    ++g_gen;
    kh2puppets::setOverrides({}, 3);
    kh2puppets::setMotionGuards({}, 2);
}

bool install(PS2Runtime &runtime)
{
    if (!runtime.hasFunction(kObserverDtor))
    {
        g_observerDtorBody = runtime.lookupFunction(kObserverDtorBody);
        if (!g_observerDtorBody || !runtime.registerFunction(kObserverDtor, &observerDtor))
            std::fprintf(stderr, "[summons] could not add kn::ObjObserver's destructor\n");
    }
    if (const char *e = std::getenv("KH2_SUMMON_PARTY"); e && *e == '1')
        return false; // (testing: the game's own rule, two friends)
    g_canSummon = runtime.lookupFunction(kCanSummon);
    if (!g_canSummon || !runtime.replaceFunction(kCanSummon, &canSummonHook))
    {
        g_canSummon = nullptr;
        std::fprintf(stderr, "[summons] could not hook the party check\n");
        return false;
    }
    return true;
}
} // namespace kh2summons
