// Android host (the APK): the app's window, the gamepad and touch, storage
// access and the game's data folder. Android only; see android_host.cpp.
#pragma once

#if defined(__ANDROID__)

#include <cstdint>
#include <string>
#include <vector>

struct ANativeWindow;

namespace kh2android
{
// Gamepad buttons, by position (Android's BUTTON_A is the bottom face button).
enum PadButton : uint32_t
{
    PadA = 1u << 0,      // bottom
    PadB = 1u << 1,      // right
    PadX = 1u << 2,      // left
    PadY = 1u << 3,      // top
    PadL1 = 1u << 4,
    PadR1 = 1u << 5,
    PadL2 = 1u << 6,
    PadR2 = 1u << 7,
    PadL3 = 1u << 8,
    PadR3 = 1u << 9,
    PadStart = 1u << 10,
    PadSelect = 1u << 11,
    PadUp = 1u << 12,
    PadDown = 1u << 13,
    PadLeft = 1u << 14,
    PadRight = 1u << 15,
};

struct Pad
{
    bool connected = false;
    uint32_t buttons = 0;              // PadButton bits
    float lx = 0, ly = 0, rx = 0, ry = 0; // -1..1, down/right positive
    std::string name;
};

// Before anything else: storage permission, the data folder (KH2 on the
// shared storage, with the disc image and the game folder), the log
// (KH2/logs/play.log and logcat), and the working directory set to the data
// folder. Returns the command line for the game (argv[0] first); empty when
// the game cannot start (the reason was shown on screen).
std::vector<std::string> prepare();

// Handles the app's events (window, focus, input). Call often from the
// thread main() runs on. While the app is in the background (no window) it
// waits here, so the game pauses.
void pump();

// The window to present into (null while there is none), and a counter that
// changes whenever it was replaced.
ANativeWindow *window();
unsigned windowGeneration();
bool focused();
// The app is being closed.
bool quitRequested();

// Called on the presenting thread when the window goes away (the app goes to
// the background) and when a new one arrives: the renderer drops and makes its
// swapchain there.
void setWindowCallbacks(void (*lost)(), void (*regained)());

Pad pad();
// The Back button since the last call (opens the settings menu).
bool takeBack();

// Touch, for the menu: the first finger, in window pixels.
struct Touch
{
    bool down = false;
    float x = 0, y = 0;
};
Touch touch();

// The data folder found by prepare(), e.g. /storage/emulated/0/KH2.
const std::string &dataDir();

// The app's private storage (e.g. /data/user/0/<package>/files) and the folder
// its native libraries were unpacked to.
std::string internalDataDir();
std::string nativeLibraryDir();
// Closes the app (the player reopens it, e.g. after choosing a GPU driver).
void closeApp();

// GPU driver (android_driver.cpp): vkGetInstanceProcAddr of the driver chosen in
// the menu, or null for the system one; vulkanStarted() once the device exists.
void *vulkanLoader();
void vulkanStarted(const std::string &deviceInfo);
// The GPU renderer could not start: with a chosen driver, that choice is
// dropped and the app closes (the next start uses the system driver).
void vulkanFailed(const char *why);
// The menu section for it (ImGui, inside the settings window).
void drawDriverMenu();

// Once per presented frame: the frame's duration, for Android's performance hint
// (the CPU governor raises the game threads' clocks when frames run late).
void frameTimeHint(int64_t frameNs);
} // namespace kh2android

#endif
