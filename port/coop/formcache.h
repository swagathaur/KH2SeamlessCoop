// The drive-form looks avatars wear, read into the file cache where nothing
// moves them and they move nothing.
//
// YS::CACHE_BUFF lays its files out again (FUN_00170670) whenever it reads
// more: the header list (sorted by group) is walked and every file is given
// an address anew, packed behind the ones before it (some groups in fixed
// regions: Sora's form groups 5000-5099 in one of 3 MB). The game never
// moves a file something still uses: within a room only Sora's form files
// come and go, and they are the last. An avatar's form files anywhere in
// that walk moved Sora's (and were moved by his drive) while in use: a pure
// virtual call, the game's main thread ended (a hang), or garbage drawn.
//
// So an avatar's form look is read into a batch of groups of its own
// (4980 up) that the walk leaves out: each of its files is placed once, in
// the free room above everything else, and stays put until no object uses it.
// Such a file is never shared with Sora's own form files (those go when he
// reverts); Sora's drive may share ours (they stay while he uses them).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace kh2formcache
{
bool install(PS2Runtime &runtime);

enum class Look
{
    Loading, // asked for, or being read
    Ready,   // all its files are in
    NoRoom,  // no room for it here (wear the base look)
};
// Each frame an avatar wants form look `entry` (or wears it): read it if it
// is not here; how it is. (Game thread.)
Look want(uint8_t *rdram, uint16_t entry);

// Each frame (game thread): batches no one wants or uses any more go.
void tick(uint8_t *rdram);
// The session is over: all batches go.
void reset(uint8_t *rdram);
} // namespace kh2formcache
