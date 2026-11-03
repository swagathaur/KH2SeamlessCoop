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

class PS2Runtime;
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
    int32_t hp = 0, maxHp = 0, minHp = 0;
    uint32_t sheet = 0; // YS::SHEET*: hp, max hp, min hp
    uint16_t rules = 0; // its "won't die from this hit" rules (kRule*)
};

// The game's "won't die from this hit" rules (the survive check at an enemy's
// vtable +0x58, 0x00186EC0, asked by the HP-from-a-hit step 0x00186D48 when a
// hit would take the HP to its floor; the hit then leaves floor + 1). The
// object's status bits ((+0xABC | +0xAC0) & ~+0xAC4), which a boss's script
// sets: 0x200 never; 0x400 only to a finisher (the attack's +0x12 bit 0x20);
// 0x800 only to a finisher or the party. kRuleAnyChannel stands for 0x80000
// (0x400 for every gauge, not only HP).
constexpr uint16_t kRuleNever = 0x200, kRuleFinisher = 0x400, kRuleParty = 0x800, kRuleAnyChannel = 0x1;
constexpr uint16_t kRuleMask = kRuleNever | kRuleFinisher | kRuleParty;

// Hooks the room spawn step (a client's own enemy spawns, see enemies.cpp).
bool install(PS2Runtime &runtime);

// Enemies and bosses in the object list that came from a spawn entry.
std::vector<Local> scan(const uint8_t *rdram);

// Host: what to send this frame.
kh2net::EnemyStates snapshot(const uint8_t *rdram, uint8_t world, uint8_t room, uint32_t tick);
// Host: a client hit enemy `serial` for `amount` (applied next frame).
void applyDamage(const uint8_t *rdram, uint8_t world, uint8_t room, int16_t serial, int32_t amount);
// Host: a client's hit on enemy `serial`: of an attack type that skips the
// rules above (5, 6: the HP step takes it as it is), a finisher? (For the HP
// of it, which follows.)
void noteClientHit(int16_t serial, bool skipsRules, bool finisher);

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
// Random enemy sets: a room's spawn script picks a group (RandomSpawn) or
// rolls for one (CasualSpawn) from the game's random number, so each game
// made its own pick and the client's enemies were not the host's. The host
// gives each room it enters a seed (sent with SpawnSeed); in a session, both
// games draw those picks from it, in the same order, so they pick the same.
// Host: this room's seed, made anew when the room changes (`fresh`: just made).
uint32_t hostSpawnSeed(uint8_t world, uint8_t room, bool &fresh);
// Client: the host's seed for a room (from the SpawnSeed event).
void onSpawnSeed(uint8_t world, uint8_t room, uint32_t seed);
// In a session (both sides) or not: the picks are shared only in one.
void setSpawnSeeding(bool on, bool host);

// Forget everything (session ended). The enemies whose AI we turned off get
// it back on the next thawAfterSession() (host loop, with the guest's memory).
void reset();
void thawAfterSession(uint8_t *rdram);

// Encoding of the EnemyDamage game event.
std::vector<uint8_t> encodeDamage(uint8_t world, uint8_t room, const Damage &d);
bool decodeDamage(const std::vector<uint8_t> &data, uint8_t &world, uint8_t &room, Damage &d);
} // namespace kh2enemies
