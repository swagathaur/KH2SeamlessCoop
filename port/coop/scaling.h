// Enemy HP and damage scaling for co-op (host's settings).
//
// With N players in the session, enemies have 1 + hpPerPlayer * (N - 1) times
// their HP (host: scaled once, the first time each is seen; clients' copies
// follow the host's HP, enemies.cpp) and their hits on players and party
// members do 1 + damagePerPlayer * (N - 1) times the damage (each game scales
// the hits its enemies land: FUN_00186D48 takes DAMAGE+0x28 off the target's
// HP, and is given the scaled amount). The host's values are sent to clients.
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace kh2scale
{
constexpr float kDefaultHp = 0.5f, kDefaultDamage = 0.25f;

bool install(PS2Runtime &runtime);
// This player's settings (used when hosting, or playing alone).
void setLocal(float hpPerPlayer, float damagePerPlayer);
float localHp();
float localDamage();
// What the session uses now (the host's, on a client) and its player count.
float hpPerPlayer();
float damagePerPlayer();
int players();
// Host loop. host: in a session and hosting; players: everyone in it.
void tick(uint8_t *rdram, bool inSession, bool host, int players);
std::vector<uint8_t> encode();                  // host -> clients
void onHostSettings(const std::vector<uint8_t> &data); // client
void leftSession();
} // namespace kh2scale
