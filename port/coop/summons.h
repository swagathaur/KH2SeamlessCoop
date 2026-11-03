// Summons in co-op.
//
// With any party: the summon command is not greyed out ("?") for want of two
// friends in battle. In co-op a remote player stands in one of the friends'
// places, so the game's own rule left summons out entirely.
//
// Seen by the other players: our summon (YS::SUMMON's object while its mode
// is on) goes out with our player state, its objentry, position, facing and
// motion. A remote player's summon in our room is shown by an NPC-class
// object of ours dressed in the summon's look (puppets.h summonEntry): its
// files are read into kh2formcache's pool past the game's 32 MB, it follows
// their summon's position and motion, and nothing of the summon's own class
// runs here (their game plays the summon; its hits are theirs).
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace kh2net
{
struct PlayerState;
}
namespace kh2partner
{
struct Remote;
}

namespace kh2summons
{
bool install(PS2Runtime &runtime);

// Our summon, if one is out, into the state we send.
void localSummon(const uint8_t *rdram, kh2net::PlayerState &ps);
// Host loop, every iteration: remote players' summons here.
void update(uint8_t *rdram, const std::vector<kh2partner::Remote> &remotes);
// The objects showing remote summons (for hits.h: their hits do nothing here).
std::vector<uint32_t> objects();
// Session over: take them away.
void reset(uint8_t *rdram);
} // namespace kh2summons
