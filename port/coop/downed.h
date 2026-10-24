// Downed, not dead: in a session, a player whose HP runs out goes down
// instead of getting the Game Over, and the others can bring them back.
//
//  - While another player is in the session, this player's HP cannot go
//    below 1 (the sheet's minimum HP, +8 in the player's party slot, as the
//    game's scripted fights use it). A hit that takes it to 1 puts the player
//    down: their controls lock (the camera stays free), their character plays
//    its fall (motion 0x36, as YS::SORA::_OVR__dead), and the others are told.
//  - A partner who heals them (Cure, a potion, anything that heals a party
//    member: partners.h sends it as a heal) brings them back with that HP.
//    Otherwise they get up by themselves after a while (kGetUpSeconds) with a
//    quarter of their HP.
//  - When every player in the session is down at once, the minimum goes back
//    to the game's and each game takes its own Game Over (its own add_hp
//    finishes the player off): everyone gets the Game Over together, and with
//    shared story the clients follow the host from the Continue.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kh2downed
{
constexpr double kGetUpSeconds = 30.0;

struct Other
{
    std::string name;
    bool present = false; // in the world (not loading, not in a menu screen)
    bool downed = false;
};

// Once per host-loop iteration. `session`: in a session with at least one
// other player. Returns what happened, for the others (log lines / toasts).
void tick(uint8_t *rdram, bool session, const std::vector<Other> &others);
// This player is down now (sent to the others in the player state flags).
bool downed();
// Forget everything (session over): the game's own minimum HP back.
void reset(uint8_t *rdram);
} // namespace kh2downed
