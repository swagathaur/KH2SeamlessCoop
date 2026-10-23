// The co-op layer's runtime side: owns the network session, sends this
// player's state every other frame, and keeps a smoothed view of the other
// players for the HUD and (next) their puppets. Driven from the host loop.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/protocol.h"

namespace kh2coop
{
struct Options
{
    std::string name = "Player";
    bool host = false;
    std::string join;          // host address ("1.2.3.4" or "1.2.3.4:27552")
    uint64_t joinAtFrame = 0;  // testing: join once the game reaches this frame
    uint16_t port = kh2net::kDefaultPort;
    float simulatedLoss = 0;
    bool followHost = false;    // clients warp to the host's room when it changes
    bool sharedEnemies = true;  // clients follow the host's enemies (enemies.h)
    bool sharedStory = true;    // clients play in the host's story, rooms and cutscenes (story.h)
};

struct RemotePlayer
{
    uint8_t id = 0;
    std::string name;
    bool haveState = false;
    kh2net::PlayerState state; // smoothed, as of now
    double age = 0;            // seconds since their last update arrived
    double rttMs = 0;
    float loss = 0;
};

// Starts hosting or joining if the options ask for it.
void init(const Options &options);
// Once per host loop iteration; rdram is the guest's memory.
void tick(const uint8_t *rdram, uint64_t guestFrame);
void shutdown();

// Menu actions.
bool host(const std::string &name, uint16_t port);
bool join(const std::string &name, const std::string &address);
void leave();

// Clients: go to the room the host is in (now), and keep doing so when they
// move (follow).
void warpToHost();
// The game's own map jump to a room (next frame). battle: the room's btl
// script program (which enemy groups spawn); 0xFFFF lets the save decide.
void requestWarp(uint8_t world, uint8_t room, uint16_t door = 0, uint16_t battle = 0xFFFF, uint16_t event = 0xFFFF);
void setFollowHost(bool on);
bool followHost();
void setSharedEnemies(bool on);
// Testing: a stand-in partner that copies this player a second behind
// (partners.h), with no session.
void setTestPartner(bool on);
bool sharedEnemies();
// Clients play in the host's story while connected (their own comes back
// when they leave), go where the host goes and watch the host's cutscenes;
// only the host skips them (a client's skip is a vote).
void setSharedStory(bool on);
bool sharedStory();

// For display.
bool active();
bool isHost();
std::string statusText();
std::vector<RemotePlayer> remotes();
std::vector<std::string> recentLog();
const Options &options();
} // namespace kh2coop
