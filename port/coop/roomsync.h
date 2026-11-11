// Room changes in co-op: every game says where it is going as soon as it
// decides (the game's map jump, FUN_00104F58(LOCATION*, ...), is called when
// a door is used or a story warp starts, before the fade and the load), so
// the others act on the intent, not on the arrival. And timings, for tests:
// "[timing] <event> <world-room> ms=<wall clock>" lines.
#pragma once

#include <cstdint>
#include <string>

class PS2Runtime;

namespace kh2roomsync
{
struct Location
{
    uint8_t world = 0, room = 0;
    uint16_t door = 0, map = 0xFFFF, battle = 0xFFFF, event = 0xFFFF;
    int64_t at = 0; // wall clock ms it was decided
};

bool install(PS2Runtime &runtime);
// The faster room changes (following the host's intent at once, party place
// holders hidden at once, no fixed wait before avatars, the others shown as
// soon as they are on their way). KH2_FAST_ROOMS=0: the old ways (for
// comparing timings).
bool fast();
// Wall clock, ms (the same on every game on one machine; for the logs).
int64_t wallMs();
// "[timing] what WW-RR ms=..." (on unless KH2_TIMING=0).
void timing(const char *what, uint8_t world, uint8_t room, int extra = -1);
// Map jumps this game decided since the last call (its own, ours or a
// follow's: the game's own room changes), oldest first; ones older than two
// seconds are dropped (nobody asked for them in time).
bool takeJump(Location &out);
// A room jump was asked for (a job of ours queued to make one, before the
// game's map jump runs): noted as if it had started.
void noteJumpAsked();
// Wall clock ms of the last map jump (started or asked for).
int64_t lastJumpMs();
} // namespace kh2roomsync
