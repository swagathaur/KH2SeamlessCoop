// Hits: sharing the hits players land on the host's enemies.
//
// Every hit an object takes goes through YS::BTLOBJ::_OVR_exec_damage
// (0x00186148, obj, DAMAGE*), called at the end of the object's "take a hit"
// routine (0x001B3D00): that routine takes the HP (0x00186D48, through add_hp,
// unless the DAMAGE's flag 2 says it was already taken), notes the attacker
// (from the ATTACK's +0x08), plays the class's own reaction (vtable +0x68:
// flinch, knock-back, launch) and tells the object's AI script (VM callback 6),
// which may react in its own way.
//
// A client's enemies are puppets of the host's (enemies.cpp): a hit its player
// lands there takes HP that is sent to the host as before, and the DAMAGE is
// copied here, with its attack parameters (the DAMAGE's +0x20). The host
// replays it on its own copy of the enemy with flag 2 set, so the enemy
// flinches, is knocked back and reacts for the host (and so for every client
// that follows the host's enemy) without losing the HP twice. The ATTACK the
// host's routine reads is a stand-in: zeroed, with the attacker (the player's
// puppet or partner, or the host's own player) and the target.
//
// KH2_DAMAGE_LOG=1 also prints every DAMAGE the field's objects take.
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace kh2hits
{
constexpr uint32_t kDamageSize = 0x60;
constexpr uint32_t kParamSize = 0x80; // the attack parameters the DAMAGE points at (+0x20)

struct Hit
{
    uint32_t object = 0; // the enemy hit (this machine's)
    uint8_t damage[kDamageSize] = {};
    uint8_t param[kParamSize] = {};
};

bool install(PS2Runtime &runtime);

// Client: copy the hits this machine's player lands on enemies (off by default).
void setCapture(bool on);
// The hits copied since last time.
std::vector<Hit> takeHits();

// Host: play `damage` (from a client) on `enemy`, by `attacker`, without its
// HP (a guest job; nothing happens if the enemy is gone or dead by then).
void replay(uint32_t enemy, uint32_t attacker, const uint8_t (&damage)[kDamageSize],
            const uint8_t (&param)[kParamSize]);

// The EnemyHit game event: world, room, the enemy's spawn serial, the DAMAGE
// and its attack parameters.
std::vector<uint8_t> encode(uint8_t world, uint8_t room, int16_t serial, const Hit &hit);
bool decode(const std::vector<uint8_t> &data, uint8_t &world, uint8_t &room, int16_t &serial, Hit &hit);
} // namespace kh2hits
