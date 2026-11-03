// A larger effect pool. Each area start, the particle system
// (FUN_001DCDB8 under AREA::start) takes its pool from the room's allocator,
// 512 KB (the size at 0x0034ECEC). Two players' spells, forms, summons and
// Limits in one room are more than one player's: a full pool fails new
// particles, and some effects crash then (as the Kingdom-Hearts-II-Coop-
// Multiplayer project measured on the PC release, up to 1.6 MB in Data
// Roxas). The pool is 4 MB of the runtime's RAM past the game's 32 MB
// instead (0x03C00000-0x04000000); the room's allocator never sees it.
#pragma once

class PS2Runtime;

namespace kh2fxpool
{
bool install(PS2Runtime &runtime);
} // namespace kh2fxpool
