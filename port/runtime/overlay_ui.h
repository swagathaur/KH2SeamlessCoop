// In-game settings menu for the GPU renderer's window. See overlay_ui.cpp.
#pragma once

#include <string>

class PS2Runtime;

namespace kh2ui
{
// Hook the menu into the renderer and input. iniPath: where "Save" writes.
void install(PS2Runtime &runtime, const std::string &iniPath);
// Once per host loop iteration (keeps game input off while the menu is open).
void tick();
} // namespace kh2ui
