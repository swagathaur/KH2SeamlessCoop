// Summons with any party: the summon command is not greyed out ("?") for
// want of two friends in battle. In co-op a remote player stands in one of
// the friends' places, so the game's own rule left summons out entirely.
#pragma once

class PS2Runtime;

namespace kh2summons
{
bool install(PS2Runtime &runtime);
} // namespace kh2summons
