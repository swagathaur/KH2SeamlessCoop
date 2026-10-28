// Shared enemies, first version: the host's game is the authority.
//
// Every machine spawns a room's enemies from the same room data, so an enemy
// is known by the serial of its spawn entry (the record an object keeps at
// +0x81C; serial at its +0x1E). The host sends its enemies' positions,
// facing, motion and HP ~20 times a second. A client in the same room moves
// its copies to those positions (through the puppets' matrix hook), never
// lets their HP be above the host's, and kills them when the host's are dead.
// Damage a client does to its copy is sent to the host, which applies it with
// the object's own add_hp (vtable +0x78), so everyone's hits count once.
#pragma once

#include "hits.h"

#include <cstdint>
#include <vector>

#include "net/protocol.h"

namespace kh2enemies
{
struct Local
{
    uint32_t object = 0;
    int16_t serial = 0;
    uint16_t entry = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    uint16_t motion = 0;
    int32_t hp = 0, maxHp = 0;
    uint32_t sheet = 0; // YS::SHEET*: hp, max hp, min hp
};

// Enemies and bosses in the object list that came from a spawn entry.
std::vector<Local> scan(const uint8_t *rdram);

// Host: what to send this frame.
kh2net::EnemyStates snapshot(const uint8_t *rdram, uint8_t world, uint8_t room, uint32_t tick);
// Host: a client hit enemy `serial` for `amount` (applied next frame).
void applyDamage(const uint8_t *rdram, uint8_t world, uint8_t room, int16_t serial, int32_t amount);

// Client: the host's latest enemies (from the session callback).
void onHostStates(const kh2net::EnemyStates &states, double now);
struct Damage
{
    int16_t serial;
    int32_t amount;
};
// Client, once a host-loop iteration: follow the host's enemies; returns the
// damage this machine's player did since last time, for the host.
std::vector<Damage> clientTick(uint8_t *rdram, uint8_t world, uint8_t room, double now);
// Client: is our `object` a copy of one of the host's enemies we follow now
// (alive)? Its serial and objentry id if so.
bool followed(uint32_t object, int16_t &serial, uint16_t &entry);

// Client: our player's blow left our copy `object` at 0 HP (hits.cpp).
void onOwnKill(uint32_t object);

// Client: the host's enemy `serial` died of `hit` (by `attacker`, our object
// for whoever struck it): our copy takes that blow with all its HP and dies
// the game's way. false: no live copy to kill.
bool onHostKill(uint8_t *rdram, uint8_t world, uint8_t room, int16_t serial, uint16_t entry, const kh2hits::Hit &hit,
                uint32_t attacker, double now);
// Forget everything (session ended). The enemies whose AI we turned off get
// it back on the next thawAfterSession() (host loop, with the guest's memory).
void reset();
void thawAfterSession(uint8_t *rdram);

// Encoding of the EnemyDamage game event.
std::vector<uint8_t> encodeDamage(uint8_t world, uint8_t room, const Damage &d);
bool decodeDamage(const std::vector<uint8_t> &data, uint8_t &world, uint8_t &room, Damage &d);
} // namespace kh2enemies
