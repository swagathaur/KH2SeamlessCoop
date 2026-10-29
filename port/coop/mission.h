// Missions (YS::MISSION) in co-op: the host's game leads.
//
// A mission (the Land of Dragons' camp fights, the Twilight Town wizard
// tower...) keeps its state in one object, *0x01D49320 (null: none running):
// its timer (dk::Timer, one for the game: frames left at 0x00349DF8), counts
// ("hidden enemies left": +0x114, 0x38 apart, on at +0xF4 bit 0), gauges
// (morale: current f32 +0x1BC of max i32 +0x1A8, 0x34 apart, on at +0x19C)
// and combo counters (+0x298, 0x40 apart, on at +0x26C). Each game ran its
// own, so a client's morale ran out on its own and it got a Game Over the
// host never had (and was left on that screen).
//
// The host sends its mission's state a few times a second; a client in the
// same room and fight takes it over. A client's mission does not fail on
// its own (trap_mission_failed does nothing there): it fails when the
// host's does (MissionFailed), so both get the Game Over and both continue.
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace kh2mission
{
bool install(PS2Runtime &runtime);
// Whether this game follows a host's missions (a client in the host's story).
void setFollowing(bool on);
// Host: the mission's state (empty: none running), and whether ours failed
// since the last call.
std::vector<uint8_t> snapshot(const uint8_t *rdram, uint8_t world, uint8_t room, uint16_t battle);
bool takeFailed();
// Client.
void onHostState(uint8_t *rdram, const std::vector<uint8_t> &data, uint8_t world, uint8_t room, uint16_t battle);
void onHostFailed(uint8_t *rdram);
} // namespace kh2mission
