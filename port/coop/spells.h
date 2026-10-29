// Spells in co-op: a player's cast is cast again by their stand-in in the
// other games, so its effects show there (work in progress).
//
// FUN_001A6178(caster, command id, combo) is the cast (from the player's
// command, FUN_0017A590): it finds the command in the magic table loaded
// for the room (6 records of 0x2C from 0x00350800, the id at +0x26), plays
// the cast motion and starts the spell's script on the caster.
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace kh2spells
{
bool install(PS2Runtime &runtime);
struct Cast
{
    uint16_t command = 0;
    uint8_t combo = 0;
};
// Our player's casts since the last call (copied while capture is on).
void setCapture(bool on);
std::vector<Cast> takeCasts();
// Cast `c` with `standIn` (a remote player's), for the look of it: its hits
// do nothing (hits.h setBlockedAttackers).
void castAs(uint32_t standIn, const Cast &c);
// Cast `c` with a player-class object (an avatar), through the player's own
// magic command (FUN_0017A590), which makes the spell's attack and effects.
void castAsPlayer(uint32_t avatar, const Cast &c);
// Spell levels this save has learned (6 slots: Fire, Blizzard, Thunder,
// Cure, Magnet, Reflect; 0 not learned), as last read for a room.
std::vector<uint8_t> knownSpells();
// What the other players know (the highest of theirs per slot): from the next
// room on its data is loaded here too, for their avatars' casts (share false:
// none, as alone).
void setOthersKnow(const std::vector<uint8_t> &levels, bool share);
std::vector<uint8_t> encode(const Cast &c, uint8_t world, uint8_t room);
bool decode(const std::vector<uint8_t> &data, Cast &c, uint8_t &world, uint8_t &room);

// Testing: KH2_TEST_CAST=FRAME:COMMAND casts COMMAND with the first party
// member at that guest frame (polled from the harness's frame hook).
void testTick(uint8_t *rdram, uint64_t frame);
} // namespace kh2spells
