// Host input for our own window (the GPU renderer's): XInput controllers and
// the keyboard, published to the runtime's pad emulation. See host_input.cpp.
#pragma once

#include <string>

#include "key_bindings.h"

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
// Keyboard layout (key-* lines in kh2recomp.ini; the menu's Keyboard section).
void setKeyBindings(const kh2keys::Bindings &b);
kh2keys::Bindings keyBindings();
// While the menu waits for a key to bind, the keyboard does not reach the game.
void setCapturingKey(bool on);
// A key or mouse button pressed since the last call (Windows VK), or 0.
int newlyPressedKey();
// How long the skip button (Cross: B / X / Space) has been held, in seconds.
double skipHeldSeconds();
// Called when Select+Start are pressed together (opens the settings menu).
void setMenuComboCallback(void (*fn)());
// Right mouse button + mouse movement turns the camera (right stick).
void setMouseCamera(bool on);
bool mouseCamera();
void setMouseSensitivity(float s);
float mouseSensitivity();
// The fast-forward key (Tab) is held.
bool fastForwardHeld();
// A pad is connected, and what it is ("" if none).
bool controllerConnected();
std::string controllerName();
} // namespace kh2input
