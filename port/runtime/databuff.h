// YS::DATABUFF::Alloc (0x001B1400) keeps the game's resident data files (the
// objentry table, system tables, messages, ...) in a pool it fills from the
// bottom with no end check, sizing each by its file (rounded to 64 bytes).
// The pool ends where the item-picture buffer starts: 0x01D44400 (FUN_001B1EE8
// loads itempic/item-NNN.imd to 0x01D44440 and clears 0x01D44400).
//
// With the English text in place of msg/jp/* (file_override), sys.bar is
// 16 KB bigger: its own end went into the item-picture buffer (system text
// overwritten by the first item picture shown), and the last file, 15jigsaw.bin (the journal's puzzle-piece
// table that Tz::JmNewInfo walks), was placed at 0x01D44AC0: in the
// item-picture buffer. The first item picture shown overwrote it, and the
// pause menu's journal check (FUN_002AC2E8) then walked 1.7 billion entries
// of picture data: minutes of black screen on pausing and at story scenes.
//
// Allocations that would run past the pool's end go to guest memory the game
// never uses instead: 0x01EC0000-0x01EF0000, the top of what main.cpp gives
// the runtime's heap (which used about 100 bytes of 640 KB).
#pragma once

class PS2Runtime;

namespace kh2databuff
{
bool install(PS2Runtime &runtime);
} // namespace kh2databuff
