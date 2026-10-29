#include "cutscene_skip.h"

#include <atomic>
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
std::atomic<bool> g_localSkip{true}; // false: a co-op client in the host's story
std::atomic<bool> g_vote{false};
double g_lastSkip = -100.0;
double g_cutsceneSince = -1.0;
bool g_holdUsed = false; // this hold already skipped something
bool g_skippedThisScene = false; // until control comes back or the event changes
uint32_t g_sceneKey = 0;

// The screen fades (dk::Fade, 0x110 bytes each from dk::getAllFade's
// 0x003493A0; the 7th, 0x00349A00, is getWideMaskBeforeFade, the cutscenes'
// mask): +0x13 how dark (0x80 black), +0xFC state (3 fading out, 4 held
// black, 1 and 2 fading in, 0 clear), +0x100 time left, +0x104 darkness per
// unit of it. A cutscene skipped as it starts (a co-op client catching up
// with the host, which is past it; --skip-cutscenes) can leave its mask
// held black: the scene's own fade-in was skipped with it, and the screen
// stayed black for the whole fight that followed.
constexpr uint32_t kFades = 0x003493A0, kFadeSize = 0x110, kFadeCount = 7;
double g_heldBlackSince[kFadeCount] = {-1, -1, -1, -1, -1, -1, -1};

void clearStuckFades(uint8_t *rdram, double t, bool cutscene)
{
    uint32_t control = 0, jumping = 0;
    std::memcpy(&control, rdram + kh2state::addr::Cntrl, 4);
    std::memcpy(&jumping, rdram + kh2state::addr::MapJumpTask, 4);
    const bool playing = !cutscene && control == 0 && !jumping && t - g_lastSkip < 60.0;
    for (uint32_t i = 0; i < kFadeCount; ++i)
    {
        uint8_t *f = rdram + kFades + i * kFadeSize;
        int32_t state;
        std::memcpy(&state, f + 0xFC, 4);
        if (!playing || state != 4 || f[0x13] < 0x80)
        {
            g_heldBlackSince[i] = -1;
            continue;
        }
        if (g_heldBlackSince[i] < 0)
            g_heldBlackSince[i] = t;
        if (t - g_heldBlackSince[i] < 1.0)
            continue;
        // Fade it in the game's way (state 2: over 16 frames).
        const float frames = 16.0f, perFrame = 128.0f / frames;
        const int32_t fadingIn = 2;
        std::memcpy(f + 0x100, &frames, 4);
        std::memcpy(f + 0x104, &perFrame, 4);
        std::memcpy(f + 0xFC, &fadingIn, 4);
        g_heldBlackSince[i] = -1;
        std::printf("[skip] fade %u (0x%08X) stayed black after a skipped cutscene: fading it in\n", i,
                    kFades + i * kFadeSize);
        std::fflush(stdout);
    }
}

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

void setAutoSkip(bool on) { g_auto = on; }
void setLocalSkip(bool allowed) { g_localSkip = allowed; }
bool localSkip() { return g_localSkip; }
bool takeSkipVote() { return g_vote.exchange(false); }
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
        else if (cutscene && !g_localSkip)
        {
            // The host's story: the host decides; this is a vote.
            g_holdUsed = true;
            g_vote = true;
            kh2pgs::toast("Asked to skip: the host decides");
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
    if (g_auto && g_localSkip && cutscene && mayAuto && t - g_cutsceneSince > 0.5)
    {
        if (scene.now.event != 0 && scene.now.event != 0xFFFF && skipCutscene(rdram))
            g_skippedThisScene = true;
    }
    clearStuckFades(rdram, t, cutscene);
}
} // namespace kh2skip
