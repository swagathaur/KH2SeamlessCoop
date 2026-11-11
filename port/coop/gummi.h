// Gummi missions (work in progress, docs/gummi-coop.md).
//
// A mission is the Gummi battle game mode (0x0032BA48), started by
// GAME_MODE::enable (0x00102080) like the Gummi menu does at launch. What
// it plays: route 0x0032BA7C, level 0x0032BA84 and variant 0x0032BA80 (the
// course file gumibattle/gm%02d%c.gbx), EX 0x0032BA88. Its ships are built
// from three blueprint buffers the menu fills at launch, each with a flag:
// the main ship 0x003A6B00 (0x0032BAD0), Teeny 0 0x003A7300 (0x0032BAD1),
// Teeny 1 0x003A7700 (0x0032BAD2). A blueprint (gm::PLAN) has no pointers,
// so its bytes can be copied as they are.
//
// The save's garage (save +0x55A0, 0x00330ED0): a valid byte, then 18 main
// ship blueprints of 0x710 from +0x160 and 18 Teeny blueprints of 0x260 from
// +0x8080.
#pragma once

#include <cstdint>
#include <string>

namespace kh2gummi
{
// Test: start mission `route` at `level` (1-3), EX or not, with the save's
// main ship blueprint `mainSlot` and Teeny blueprints `teeny0` / `teeny1`
// (-1: none), as the menu would.
void testStart(int route, int level, bool ex, int mainSlot, int teeny0, int teeny1);
// The same with the game's preset blueprints (gumimenu/<lang>/planinfo.bar,
// the ones missions award): the main ship of preset `mainPreset`, Teeny 0 of
// preset `teeny0` (its first Teeny), Teeny 1 of `teeny1` (its second); -1 none.
void testStartPreset(int route, int level, bool ex, int mainPreset, int teeny0, int teeny1);
// The presets (one line; loads the file the first time).
void listPresets();
// What the garage holds (one line).
std::string garage(const uint8_t *rdram);
} // namespace kh2gummi
