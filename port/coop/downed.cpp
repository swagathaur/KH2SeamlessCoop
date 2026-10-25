#include "downed.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "game_state.h"
#include "guest_call.h"
#include "host_input.h"
#include "pgs_renderer.h"

namespace kh2downed
{
namespace
{
constexpr uint32_t kSlotMin = 0x008;       // the party slot's minimum HP
constexpr uint32_t kFall = 0x0017F488;     // the motion start YS::SORA::_OVR__dead uses (MOTION*, id, f12, f13)
constexpr uint32_t kFallMotion = 0x36;
constexpr uint32_t kVtableAddHp = 0x78;

bool g_protected = false;   // we set the minimum to 1
int32_t g_minBefore = 0;    // the game's own minimum
bool g_downed = false;
double g_downedAt = 0;
int32_t g_lastHp = -1;
bool g_wiped = false;       // everyone down: the Game Over is under way
double g_lastFall = -100;
bool g_noFall = false;     // the fall left the bind pose for this player's character (reset with the session)
std::vector<bool> g_othersDown;

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }

constexpr uint32_t slot() { return kh2state::addr::Slot1; }

void protect(uint8_t *rdram, bool on)
{
    if (on == g_protected)
        return;
    if (on)
    {
        g_minBefore = rd<int32_t>(rdram, slot() + kSlotMin);
        if (g_minBefore < 1)
            wr<int32_t>(rdram, slot() + kSlotMin, 1);
    }
    else if (rd<int32_t>(rdram, slot() + kSlotMin) == 1 && g_minBefore < 1)
        wr<int32_t>(rdram, slot() + kSlotMin, g_minBefore);
    g_protected = on;
}

void standUp(const char *why)
{
    g_downed = false;
    kh2input::setGameBlocked(false);
    std::printf("[downed] back on your feet (%s)\n", why);
    std::fflush(stdout);
    kh2pgs::toast(std::string("Back on your feet (") + why + ")");
}

// The player falls (and stays down: started again if their own action code
// has moved them on).
void fall(uint8_t *rdram)
{
    const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
    if (!player || player >= 0x02000000u)
        return;
    g_lastFall = now();
    if (g_noFall)
        return;
    // Only if the character has the motion: Roxas's sets have no 0x36, and
    // starting it left him in his bind pose (arms out) until the next room.
    constexpr uint32_t kMotionIsExist = 0x001805A8; // YS::MOTION::is_exist(int)
    const uint32_t motion = player + kh2state::addr::ObjMotion;
    kh2guest::post([motion, step = 0](const kh2guest::Env &, uint32_t result, bool) mutable -> std::optional<kh2guest::Call> {
        switch (step++)
        {
        case 0:
            return kh2guest::Call{kMotionIsExist, {motion, kFallMotion}, {}};
        case 1:
        {
            if (result)
                return kh2guest::Call{kFall, {motion, kFallMotion}, {0.0f, 0.0f}};
            static bool told = false;
            if (!told)
            {
                told = true;
                std::printf("[downed] this character has no fall motion; staying up while down\n");
                std::fflush(stdout);
            }
            return std::nullopt;
        }
        default:
            return std::nullopt;
        }
    });
}

// Finish the player off the game's way (add_hp to 0: the Game Over follows).
void finish(uint8_t *rdram)
{
    kh2guest::post([](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        const uint32_t player = rd<uint32_t>(env.rdram, kh2state::addr::PlayerPtr);
        if (!player || player >= 0x02000000u)
            return std::nullopt;
        const uint32_t vt = rd<uint32_t>(env.rdram, player);
        const uint32_t fn = vt && vt < 0x02000000u ? rd<uint32_t>(env.rdram, vt + kVtableAddHp) : 0;
        const int32_t hp = rd<int32_t>(env.rdram, kh2state::addr::Slot1);
        if (!fn || hp <= 0)
            return std::nullopt;
        return kh2guest::Call{fn, {player, static_cast<uint32_t>(-hp), 0u, 0u}, {}};
    });
    (void)rdram;
}
} // namespace

bool downed() { return g_downed; }

void reset(uint8_t *rdram)
{
    protect(rdram, false);
    if (g_downed)
        standUp("the session ended");
    g_wiped = false;
    g_lastHp = -1;
    g_noFall = false;
    g_othersDown.clear();
}

void tick(uint8_t *rdram, bool session, const std::vector<Other> &others)
{
    const kh2state::State st = kh2state::read(rdram);
    const bool inWorld = st.player.valid && st.now.world != 0 && st.now.world != 0xFF;
    int present = 0;
    for (const Other &o : others)
        present += o.present ? 1 : 0;

    // The others going down and getting up.
    if (g_othersDown.size() != others.size())
        g_othersDown.assign(others.size(), false);
    for (size_t i = 0; i < others.size(); ++i)
        if (others[i].downed != g_othersDown[i])
        {
            g_othersDown[i] = others[i].downed;
            const std::string text = others[i].downed ? others[i].name + " is down! Heal them (Cure, a potion) to bring them back"
                                                      : others[i].name + " is back on their feet";
            std::printf("[downed] %s\n", text.c_str());
            std::fflush(stdout);
            kh2pgs::toast(text, 5.0);
        }

    if (!session || !inWorld)
    {
        if (!session)
            reset(rdram);
        g_lastHp = -1;
        return;
    }
    const int32_t hp = rd<int32_t>(rdram, slot());
    const int32_t maxHp = rd<int32_t>(rdram, slot() + 4);
    if (g_wiped)
    {
        // The Game Over (and a Continue or a load) ends it: HP comes back.
        if (hp > 1 || st.control == 0 && hp > 0 && now() - g_downedAt > 60.0)
        {
            g_wiped = false;
            if (g_downed)
                standUp("the party is back");
        }
        g_lastHp = hp;
        return;
    }
    // Protected while someone else is here to help.
    protect(rdram, present > 0 || g_downed);

    if (!g_downed)
    {
        if (g_protected && hp <= 1 && hp >= 0 && g_lastHp > 1)
        {
            g_downed = true;
            g_downedAt = now();
            kh2input::setGameBlocked(true);
            fall(rdram);
            std::printf("[downed] you are down: a partner can heal you, or you get up in %.0f s\n", kGetUpSeconds);
            std::fflush(stdout);
            kh2pgs::toast("You are down! A partner can heal you, or you get up in 30 s", 5.0);
        }
    }
    else
    {
        if (hp > 1)
            standUp("a partner healed you");
        else if (now() - g_downedAt > kGetUpSeconds)
        {
            wr<int32_t>(rdram, slot(), maxHp > 4 ? maxHp / 4 : 1);
            standUp("you got up");
        }
        else
        {
            // Everyone down: the Game Over, together.
            bool all = present > 0;
            for (const Other &o : others)
                all = all && (!o.present || o.downed);
            if (all)
            {
                g_wiped = true;
                std::printf("[downed] everyone is down: Game Over\n");
                std::fflush(stdout);
                kh2input::setGameBlocked(false);
                protect(rdram, false);
                finish(rdram);
            }
            else
            {
                const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
                const uint32_t motion = player + kh2state::addr::ObjMotion;
                const uint32_t index = rd<uint32_t>(rdram, player + kh2state::addr::ObjMotionIndex);
                const bool noPose = player && player < 0x02000000u && rd<uint32_t>(rdram, motion + 0xC) == 0;
                if (noPose && index == kFallMotion && now() - g_lastFall > 0.2)
                {
                    // The fall started but its data went (Roxas: the set has
                    // the id, not the motion), which leaves the bind pose
                    // (arms out). Stand idle instead, and stop falling.
                    if (!g_noFall)
                    {
                        g_noFall = true;
                        std::printf("[downed] no fall motion for this character: standing while down\n");
                        std::fflush(stdout);
                    }
                    kh2guest::postCall({kFall, {motion, 0u}, {0.0f, 0.0f}});
                    g_lastFall = now();
                }
                else if (!g_noFall && now() - g_lastFall > 2.0 && index != kFallMotion)
                    fall(rdram);
            }
        }
    }
    g_lastHp = hp;
}
} // namespace kh2downed
