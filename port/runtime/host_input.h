// Host input for our own window (the GPU renderer's): XInput controllers and
// the keyboard, published to the runtime's pad emulation. See host_input.cpp.
#pragma once

namespace kh2input
{
struct Options
{
    // Swap the face buttons so the bottom button (A / Cross position) is
    // Circle: KH2 Final Mix confirms with Circle, like other Japanese releases.
    bool confirmOnBottom = true;
};
void init(const Options &options);
// Sample controllers (always) and the keyboard (when our window has focus),
// then publish the merged state. Call once per host loop iteration.
void poll(bool keyboardFocus);
// Rumble, 0..1 for the small (high-frequency) and large (low-frequency) motor.
void setRumble(float small, float large);
void setRumbleEnabled(bool on);
bool rumbleEnabled();
// Confirm on the bottom face button (default) or the right one (Japanese layout).
void setConfirmOnBottom(bool on);
bool confirmOnBottom();
// While suppressed (the settings menu is open) the game sees an idle pad.
void setSuppressed(bool on);
// Called when Select+Start are pressed together (opens the settings menu).
void setMenuComboCallback(void (*fn)());
// A pad is connected.
bool controllerConnected();
} // namespace kh2input
