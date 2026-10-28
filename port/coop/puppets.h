// Puppets: each remote player in the same room as us is shown by an object
// that follows their position. Borrow mode moves an NPC already in the room
// (it keeps its own look); spawn mode creates an NPC-class object dressed in
// the player's own model and motions, and plays their current motion.
//
// How: YS::OBJ::calc_motion rebuilds every object's world matrix each frame
// through FUN_00193608 from its position (+0x540) and direction (+0x550).
// That function is overridden; for a borrowed object the override writes the
// remote player's (smoothed) position and facing first. Picking which NPC to
// borrow happens on the host loop (update()); the override reads a small
// table under a mutex.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
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

// Party members other players stand in for (partners.h): only the motion
// their player is playing may be started on them (object, motion index; up
// to two; an empty list: none).
void setMotionGuards(const std::vector<std::pair<uint32_t, int32_t>> &objects);

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
// Ask the game to load an objentry's model and motions (the objentry read
// request a room's spawn list makes, then CACHE_BUFF::Flush).
void requestPreload(uint32_t entryId);
// A drive form's look, read into a cache group of its own (as the drive does).
void requestFormLoad(uint32_t entryId);
// Give a puppet entry (0x084C or 0x084D) another entry's model and motion set
// (lookEntry 0 puts its own back). Takes effect for later loads and spawns.
void requestDress(uint32_t puppetEntry, uint32_t lookEntry);
// Start motion `id` on object `obj` (a guest job), if the object is still
// there and its motion set has that motion.
void postMotion(uint32_t obj, uint32_t id, float blend = 4.0f);
// Take object `obj` away (a guest job; its class's leave), if it is still there.
void remove(uint32_t obj);
// Make an object of `entryId` at `pos` facing `yaw` (a guest job, like a
// room's spawn list entry: placeholders such as FRIEND_1 (0x237) become the
// party member the room's party has in that place). `done` gets the object,
// or 0.
// `serial` (>= 0) goes in its spawn entry (as a room's list numbers its
// objects); `wanted` is asked before each step (false: dropped, done gets 0).
void spawnAt(uint32_t entryId, const float pos[3], float yaw, std::function<void(uint32_t)> done = {},
             int32_t serial = -1, std::function<bool(const uint8_t *)> wanted = {});

// Other objects whose position and facing come from elsewhere (the host's
// enemies, on clients): applied in the same matrix hook. Replaces the set.
struct Override
{
    uint32_t object = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0;
};
// Channel 0: the host's enemies (enemies.cpp); 1: partners (partners.cpp).
void setOverrides(std::vector<Override> overrides, int channel = 0);

struct Assignment
{
    uint8_t playerId;
    uint32_t object;
    char name[33];
};
std::vector<Assignment> assignments();
} // namespace kh2puppets
