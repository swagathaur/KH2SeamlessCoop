// Puppets, first version ("ghosts"): each remote player in the same room as
// us borrows an NPC already there and moves it to where that player is.
// The NPC keeps its own model and animation for now; driving the right model
// and its motions comes next.
//
// How: YS::OBJ::calc_motion rebuilds every object's world matrix each frame
// through FUN_00193608 from its position (+0x540) and direction (+0x550).
// That function is overridden; for a borrowed object the override writes the
// remote player's (smoothed) position and facing first. Picking which NPC to
// borrow happens on the host loop (update()); the override reads a small
// table under a mutex.
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace kh2puppets
{
struct Target
{
    uint8_t playerId = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
    uint8_t world = 0, room = 0;
    uint16_t model = 0;  // the remote player's objentry id (0x5A Roxas, ...)
    uint16_t motion = 0; // their current motion index (MOTION +0x14)
};

enum class Mode
{
    Borrow, // move an NPC already in the room
    Spawn,  // create an NPC-class copy of the player's character (experimental)
};
void setMode(Mode mode);
Mode mode();

bool install(PS2Runtime &runtime);
void setEnabled(bool on);
bool enabled();
// Host loop: where the remote players are now (only those with a body).
void update(const uint8_t *rdram, const std::vector<Target> &targets);

// Experiment: on the next frame, create an object from the objentry table
// with the game's factory (FUN_0016F000) `distance` units in front of the
// player. The result is printed ([puppets] spawned ...).
void requestSpawn(uint32_t entryId, float distance = 150.0f);
// Experiment: ask the game to load an objentry's model and motions
// (FUN_0016EB48, what loading a drive form uses).
void requestPreload(uint32_t entryId);

struct Assignment
{
    uint8_t playerId;
    uint32_t object;
    char name[33];
};
std::vector<Assignment> assignments();
} // namespace kh2puppets
