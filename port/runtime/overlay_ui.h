// In-game settings menu for the GPU renderer's window. See overlay_ui.cpp.
#pragma once

#include <string>

class PS2Runtime;

namespace kh2ui
{
struct Options
{
    float fastForwardSpeed = 3.0f; // while Tab is held
    bool pauseUnfocused = false;   // pause the game while the window is in the background
    double baseSpeed = 1.0;        // --speed
    float volume = 1.0f;           // 0..1
};
// Hook the menu into the renderer and input. iniPath: where "Save" writes.
void install(PS2Runtime &runtime, const std::string &iniPath, const Options &options);
// Once per host loop iteration: keeps game input off and the game paused while
// the menu is open, and applies fast-forward.
void tick();
// The game clock is stopped (menu open, or in the background).
bool paused();
} // namespace kh2ui
