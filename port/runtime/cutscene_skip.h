// Skipping cutscenes: hold B (the PS2's Cross; X or Space on the keyboard)
// for a second to skip the pre-rendered movie or in-engine cutscene that is
// playing, or skip them all automatically (--skip-cutscenes; movies have
// --skip-movies).
//
// In-engine cutscenes are skipped the way the pause menu's "Skip" does it
// (FUN_0026E754 -> dk::kickUnc, 0x00225E28): sa::EVENT's skip flag at
// 0x0035DE08 (what EVENT::isSkipping reads) and the byte at 0x00352770 set
// to 1; the event then winds itself up.
#pragma once

#include <cstdint>

namespace kh2skip
{
void setAutoSkip(bool on);
bool autoSkip();
// A cutscene (the game's control state 3) is running.
bool inCutscene(const uint8_t *rdram);
// Skip the in-engine cutscene now (no-op outside one).
bool skipCutscene(uint8_t *rdram);
// Once per host loop iteration. skipHeld: how long the skip button has been
// held (seconds; 0 if not).
void tick(uint8_t *rdram, double skipHeld);
} // namespace kh2skip
