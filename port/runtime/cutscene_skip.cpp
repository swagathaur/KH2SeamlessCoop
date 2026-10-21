#include "cutscene_skip.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "game_state.h"
#include "movie_eof.h"
#include "pgs_renderer.h"

namespace kh2skip
{
namespace
{
constexpr uint32_t kEventSkipping = 0x0035DE08; // sa::EVENT::isSkipping()
constexpr uint32_t kUncKick = 0x00352770;       // set by FUN_001D5A38 (kickUnc's tail)
constexpr double kHoldToSkip = 1.0;              // seconds

bool g_auto = false;
double g_lastSkip = -100.0;
double g_cutsceneSince = -1.0;
bool g_holdUsed = false; // this hold already skipped something
bool g_skippedThisScene = false; // until control comes back or the event changes
uint32_t g_sceneKey = 0;

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

void setAutoSkip(bool on) { g_auto = on; }
bool autoSkip() { return g_auto; }

bool inCutscene(const uint8_t *rdram)
{
    uint32_t control;
    std::memcpy(&control, rdram + kh2state::addr::Cntrl, 4);
    return control == 3;
}

bool skipCutscene(uint8_t *rdram)
{
    if (!inCutscene(rdram))
        return false;
    rdram[kEventSkipping] = 1;
    rdram[kUncKick] = 1;
    g_lastSkip = now();
    const kh2state::State st = kh2state::read(rdram);
    std::printf("[skip] cutscene skipped (%s, event %u)\n", kh2state::describe(st.now).c_str(), st.now.event);
    std::fflush(stdout);
    return true;
}

void tick(uint8_t *rdram, double skipHeld)
{
    const double t = now();
    const bool cutscene = inCutscene(rdram);
    // A scene: one event in one room (scenes can follow each other without
    // control coming back in between).
    const kh2state::State scene = kh2state::read(rdram);
    const uint32_t sceneKey = uint32_t(scene.now.world) << 24 | uint32_t(scene.now.room) << 16 | scene.now.event;
    if (!cutscene || sceneKey != g_sceneKey)
    {
        g_cutsceneSince = cutscene ? t : -1.0;
        g_skippedThisScene = false;
        g_sceneKey = sceneKey;
    }
    else if (g_cutsceneSince < 0)
        g_cutsceneSince = t;
    // A skip takes the event a moment (fade, next room) to wind up; skipping
    // again meanwhile could hang it. One skip per scene, then wait for
    // control to come back (or 15 s, for scenes that run straight into the
    // next one).
    const bool mayAuto = !g_skippedThisScene || t - g_lastSkip > 15.0;
    const bool mayManual = t - g_lastSkip > 5.0;

    if (skipHeld <= 0)
        g_holdUsed = false;
    else if (skipHeld >= kHoldToSkip && !g_holdUsed)
    {
        if (kh2movie::playing())
        {
            g_holdUsed = true;
            kh2movie::skipCurrent();
            kh2pgs::toast("Movie skipped");
        }
        else if (cutscene && mayManual && skipCutscene(rdram))
        {
            g_holdUsed = true;
            g_skippedThisScene = true;
            kh2pgs::toast("Cutscene skipped");
        }
    }
    // Automatic: once a cutscene has run a moment (its start is set up), and
    // again if it is still going a while after the last skip.
    // Only story events (an event number in the location): the Game Over
    // screen also stops control, with event 0, and is left alone.
    if (g_auto && cutscene && mayAuto && t - g_cutsceneSince > 0.5)
    {
        if (scene.now.event != 0 && scene.now.event != 0xFFFF && skipCutscene(rdram))
            g_skippedThisScene = true;
    }
}
} // namespace kh2skip
