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
    uint16_t port = kh2net::kDefaultPort;
    float simulatedLoss = 0;
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

// For display.
bool active();
bool isHost();
std::string statusText();
std::vector<RemotePlayer> remotes();
std::vector<std::string> recentLog();
const Options &options();
} // namespace kh2coop
