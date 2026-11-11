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
#include <vector>

class PS2Runtime;

namespace kh2gummi
{
// Hooks in the Gummi battle overlay (its functions, by address in its image).
bool install(PS2Runtime &runtime);

// A Teeny Ship flown by a player instead of the formation (k: 0 or 1).
// While one is piloted, the formation leaves it alone: it keeps the place its
// pilot steers it to (an offset from the main ship, inside the formation's
// room), and it fires on its pilot's button, not the main ship's.
struct PilotInput
{
    float stick[2] = {0, 0}; // -1..1 (x right, y up)
    bool fire = false;       // held
};
void setPilot(int k, bool on);
void setPilotInput(int k, const PilotInput &in);
// Test: Teeny `k` flown by a script (circles, firing now and then).
void testPilot(int k, bool on);

// A Gummi mode is on (the menu or a mission: its GAME_MODE's state, +0x1C, is
// 1): the field's co-op stands down (no party, room jumps or stand-ins).
bool inGummi(const uint8_t *rdram);

// --- Co-op: the same mission and ships on every machine ---------------------
// The host's launch (the Gummi battle mode starting, D_00103F68) gives the
// other players its Teeny Ships: the first client (lowest id) Teeny 0, the
// second Teeny 1, each with the Teeny blueprint they sent (GummiTeeny; none:
// the host's own, or a preset). Then the mission and all three blueprints go
// to the clients (GummiSortie), which start the same mission themselves.
//
// Each host-loop iteration (coop.cpp): who is in the session.
void setSession(bool active, bool host, uint8_t localId, const std::vector<uint8_t> &others);
// Host: a launch to send, once, as GummiSortie events (pieces that fit a
// packet: launch number, piece, pieces, then up to 900 bytes).
bool takeSortie(std::vector<std::vector<uint8_t>> &out);
// Host: a player's Teeny blueprint (GummiTeeny).
void onPlayerTeeny(uint8_t player, const std::vector<uint8_t> &plan);
// Client: our Teeny blueprint to send (the garage's first), and whether it
// changed since the last call.
bool ourTeeny(const uint8_t *rdram, std::vector<uint8_t> &out);
// Client: a piece of the host's launch.
void onHostSortie(const std::vector<uint8_t> &piece);

// During a mission, every other frame: the host's main ship (its place
// relative to the rail) and the piloted Teenies (GummiShips, host -> clients);
// a pilot's Teeny (GummiPilot, client -> host): its offset from the main ship
// and how many shots it has fired. Each machine fires a remote pilot's Teeny
// as its count goes up.
bool hostShips(const uint8_t *rdram, std::vector<uint8_t> &out);
void onHostShips(const std::vector<uint8_t> &data);
bool pilotState(std::vector<uint8_t> &out);
void onPilotState(uint8_t player, const std::vector<uint8_t> &data);
// No mission any more (back to the field, or the session ended).
void endMission();
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
