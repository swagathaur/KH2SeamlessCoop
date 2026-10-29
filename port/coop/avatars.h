// Avatars: each remote player in our room is shown by a second player
// object (the player's own class, made from their character's objentry),
// rather than by a party member dressed in their look (partners.h).
//
// Being a player, it does what players do: their spells cast through the
// player's magic command make the spell's attack and effects, their drive
// forms are the player's, and motions meant for the player have a player to
// go to. What keeps it a puppet of the remote player's game:
//  - its action is off (+0x10C 0x200000): it does not answer our controller,
//    and its motions are the remote player's (puppets.h motion guards);
//  - a player object points the game's own "player" pointers at itself on
//    its first updates (controls, camera, targeting); ours go back to ours
//    every frame;
//  - it has an HP/MP sheet of its own (0x01E4E000 on, 0x268 each), holding
//    the remote player's HP and MP from their game;
//  - nothing it hits takes damage here, and nothing here hurts it
//    (hits.h): their game deals and takes their hits.
// Our own party (Donald, Goofy, the world's companions) stays as it is.
//
// On while KH2_AVATARS=1 (partners stand down then).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "partners.h"

class PS2Runtime;

namespace kh2avatars
{
bool install(PS2Runtime &runtime);
bool enabled();
// Host loop, every iteration (the same list partners get).
// Returns heals this game gave them (items, Cure on their party place).
std::vector<kh2partner::Heal> update(uint8_t *rdram, const std::vector<kh2partner::Remote> &remotes);
// Avatar objects and the party members holding their places, in this room
// (for hits.h), and player `id`'s avatar (0: none).
// Our own player object as the avatars know it (0: not yet): the game's
// player pointer is another object for a moment while an avatar is made.
uint32_t ourPlayer();
// Is `sheet` one an avatar's HP and MP live in (a party slot or our own)?
bool isAvatarSheet(uint32_t sheet);
std::vector<uint32_t> objects();
uint32_t objectFor(uint8_t playerId);
// The avatar is casting (spells.h castAsPlayer): its own motions (the cast's,
// whose cues make the spell) play for a moment, rather than its player's.
void noteCast(uint32_t avatar);
// Character (save index) -> remote player's name, for the party places they
// hold (names.h; set after the partners' update, which clears it).
std::map<int, std::string> names();
// Session over: take them away.
void reset(uint8_t *rdram);
} // namespace kh2avatars
