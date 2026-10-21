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
    if (!cutscene)
        g_cutsceneSince = -1.0;
    else if (g_cutsceneSince < 0)
        g_cutsceneSince = t;

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
        else if (cutscene && skipCutscene(rdram))
        {
            g_holdUsed = true;
            kh2pgs::toast("Cutscene skipped");
        }
    }
    // Automatic: once a cutscene has run a moment (its start is set up), and
    // again if it is still going a while after the last skip.
    if (g_auto && cutscene && t - g_cutsceneSince > 0.5 && t - g_lastSkip > 2.0)
        skipCutscene(rdram);
}
} // namespace kh2skip
