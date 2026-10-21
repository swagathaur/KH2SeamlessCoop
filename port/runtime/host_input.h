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
} // namespace kh2input
