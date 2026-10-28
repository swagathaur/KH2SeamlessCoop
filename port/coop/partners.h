// Partners: the other players stand in for this game's party members.
//
// Each player plays their own game (own story, inventory and choices). When a
// session is on, the first other player takes the place of this game's first
// companion (Donald, or the world's own member in that slot) and the second
// player the second companion. The companion's object stays the game's own,
// so everything that treats party members as party members keeps working:
// Cure and other healing reach them, the party HUD shows them, drive forms
// see them. What changes:
//
//  - Look: the companion's objentry record (and its weapon's) is dressed in
//    the other player's model, motion set and weapon, as puppets.cpp does for
//    NPCs. It takes effect when the room's objects are next made: the next
//    room change, or a reload of the room coop.cpp makes when someone joins.
//  - Control: its AI is switched off (object +0x10C |= 0x200000, the "may
//    act" check), and its position, facing and motion follow the other
//    player's, through the puppets' matrix hook (overrides channel 1).
//  - Stats: its party slot (0x01C6C750 - slot * 0x268) shows the other
//    player's HP and MP. HP this game gives it (Cure, potions) is sent to
//    them as a heal; damage it takes here is ignored (their game decides).
//  - Equipment: its save record (save +0x24F0, 0x114 bytes a character)
//    carries the other player's armour, accessories, items and abilities, so
//    the party menu shows them. Changing them here is undone, together with
//    the inventory change the menu made.
//  - Drive forms: while linked, forms do not take party members away
//    (FUN_001C9860, which "sacrifices" the members a form needs, does nothing).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace kh2net
{
struct PlayerState;
}

namespace kh2partner
{
constexpr uint32_t kLoadoutSize = 0x114; // one character's save record

struct Remote
{
    uint8_t playerId = 0;
    std::string name;
    bool present = false; // has a body in the world
    uint8_t world = 0, room = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    uint16_t model = 0;  // objentry id of their character (0x54 Sora, 0x5A Roxas, ...)
    uint16_t weapon = 0; // objentry id of their weapon object (0 unknown)
    uint16_t motion = 0;
    int16_t rideSerial = -1; // what they ride (protocol.h PlayerState)
    uint16_t rideEntry = 0, rideJoint = 0, rideFlags = 0, rideMotion = 0;
    int32_t hp = 0, maxHp = 0, mp = 0, maxMp = 0;
    std::vector<uint8_t> loadout; // their own save record (kLoadoutSize), or empty
};

// HP / MP this game gave a partner, for its player.
struct Heal
{
    uint8_t playerId = 0;
    int32_t hp = 0, mp = 0;
};

struct Link
{
    uint8_t playerId = 0;
    int slot = 0;          // party slot 1 or 2
    uint32_t object = 0;   // the companion object driven now (0: not in this room)
    uint16_t entry = 0;    // the companion's objentry id
    bool dressed = false;  // the object was made with the player's look
};

// Hooks the drive form's party sacrifice (EE side).
bool install(PS2Runtime &runtime);

// Host loop, every iteration: link the remote players to the companions.
// Returns heals to send.
std::vector<Heal> update(uint8_t *rdram, const std::vector<Remote> &remotes);
// Undo everything (session over): AI back on, looks and records restored.
void reset(uint8_t *rdram);
// Testing (--fill-hold=FROM-TO): while on, empty party slots are not filled
// (a room loads with its party as the game left it).
void holdFill(bool on);

// This player's own data, for the others.
uint16_t localWeaponEntry(const uint8_t *rdram);
// What our player rides now (a skateboard), into the state we send.
void localRide(const uint8_t *rdram, kh2net::PlayerState &ps);
std::vector<uint8_t> localLoadout(const uint8_t *rdram);
// A partner here healed us.
void applyHeal(uint8_t *rdram, int32_t hp, int32_t mp);

// Remote players shown as companions now (for the UI and to keep them out of
// the puppets).
std::vector<Link> links();
bool linked(uint8_t playerId);
// A partner in our room still wears the companion's own look: true once,
// when the room should be reloaded to show theirs (coop.cpp does that when
// the player is free to move, and puts them back where they stood).
bool takeRefreshRequest();
} // namespace kh2partner
