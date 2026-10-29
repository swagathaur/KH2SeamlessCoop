// A client can't leave the room on their own: the host leads (the client
// follows them from room to room, coop.cpp). Walking into an exit to any
// room other than the host's is answered the game's own way for a room you
// may not enter yet ("The others are waiting for me ..."):
//
//  - an exit trigger (FUN_0019A480, the area's exits) asks FUN_00196768(world,
//    room) whether the room is shut (-2: shut, by the save's table at
//    +0x1EF8 or FUN_001D52A0); a shut room is not jumped to;
//  - exits that ask the player to walk through (FUN_0019A6C0) then show the
//    reason, FUN_001967F0(world, room, 0)'s text, with FUN_0015B268(text,
//    seconds); plain exits just don't jump, so we show it ourselves.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace kh2roomlock
{
bool install(PS2Runtime &runtime);
// Each frame of a session: on (a client following the host) with the host's
// room, which stays open. KH2_TEST_ROOM_LOCK=1: always on, every room shut.
void set(bool on, uint8_t hostWorld, uint8_t hostRoom);
} // namespace kh2roomlock
