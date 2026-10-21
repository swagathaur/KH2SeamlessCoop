// Switchable game patches (widescreen and friends). See game_patches.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace kh2patches
{
// Enable patches by name, comma-separated. Unknown names are reported.
// Call after loadELF() and the other function replacements.
bool enable(PS2Runtime &runtime, const std::string &names);
// Switch one patch on or off while running (code variants are swapped back and
// data patch locations restored). Call from the presenter thread.
bool setEnabled(PS2Runtime &runtime, const std::string &name, bool on);
bool isEnabled(const std::string &name);
// Names and descriptions of the available patches.
struct Info
{
    const char *name;
    const char *description;
};
std::vector<Info> available();
// Repeat the enabled patches' data writes; call once per frame.
void tick(uint8_t *rdram);
// The display aspect the enabled patches render for (0 = the game's 4:3).
float aspect();
// Print the available patches (for --help / --list-patches).
void list();
} // namespace kh2patches
