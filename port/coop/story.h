// Shared story: while connected, a client plays in the host's story.
//
// KH2's story state lives in the save data the game runs on (0x0032BB30,
// layout as OpenKH's SaveDataFinalMix): each room's map/battle/event programs
// (+0x0010), the story progress flags (+0x1C90, YS::PROGRESS), rooms visited
// (+0x22F8), YS::SAVEFLAG's flags (+0x2400) and the party for each world
// (+0x3534). A client's own copy of those is put aside when the host's first
// arrives, and the host's is used instead, kept up to date as the host plays
// on; when the session ends the client's own comes back (and coop.cpp takes
// the player back to where they were). Everything else stays the client's:
// level, HP, items, equipment, abilities, munny, drive forms.
//
// Saving while connected keeps the client's story: the save menu copies the
// running data into the memory card image with memcpy(image, 0x0032BB30,
// 0x10FC0) (FUN_00240A70; the checksum is computed afterwards), and while the
// host's story is in use that copy puts the client's own story and location
// into the image. Loading a game while connected (the same memcpy the other
// way) takes the loaded story as the client's own and goes back to the host's.
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace kh2story
{
bool install(PS2Runtime &runtime);

// Host, every few frames: what changed in its story since last time (all of
// it when `full`), as StoryChunk event payloads.
std::vector<std::vector<uint8_t>> hostCollect(const uint8_t *rdram, bool full);

// Client: a StoryChunk from the host.
void onChunk(const std::vector<uint8_t> &data);
// Client, every host-loop iteration: start using the host's story once all of
// it has arrived and `mayStart` (this game is past its title screen, in a
// room), apply what changed since, and (when `settle`, the player being free
// to move) put back anything this game changed that the host's did not.
// Returns true the iteration the host's story is first used.
bool clientTick(uint8_t *rdram, bool settle, bool mayStart);
// The host's story is in use.
bool active();

struct Place
{
    uint8_t world = 0, room = 0;
    uint16_t door = 0;
};
// Client: the session is over; the client's own story goes back. True (and
// where the player was when it started) if it was in use.
bool end(uint8_t *rdram, Place &back);
// Testing: run the save menu's image step and say whose story it holds.
void testSave();
// Where the player was when the host's story was first used.
void setReturnPlace(const Place &p);
} // namespace kh2story
