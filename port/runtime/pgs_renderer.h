// GPU renderer: paraLLEl-GS (Vulkan compute emulation of the PS2 GS) driven
// from the same GIF stream as the runtime's software GS. See pgs_renderer.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct GSRegisters;

namespace kh2pgs
{
struct Options
{
    // Super-sampling factor: 1, 2, 4, 8 or 16 samples per pixel.
    unsigned superSampling = 1;
    // Open a window and present into it (otherwise frames are only read back,
    // for screenshots and tests).
    bool windowed = false;
    unsigned windowWidth = 1280;
    unsigned windowHeight = 960;
    bool fullscreen = false;
    bool vsync = false;
    // Keep the software rasteriser running too (slow; for comparing output).
    bool keepSoftwareRaster = false;
    // Sharpened FSR upscale to the window instead of a bilinear blit.
    bool fsr = true;
    // Output aspect ratio; 0 means the game's own (4:3).
    float aspect = 0.0f;
};

// Creates the Vulkan device and the GS, and starts mirroring the GIF stream.
// Must be called before the guest starts. Returns false (and leaves the
// software path alone) if Vulkan or a suitable GPU is not available.
bool init(const Options &options);
bool active();
void shutdown();

// Call once per guest frame (vsync), from the presenting thread, with the
// guest's privileged GS registers. Scans out the frame and, when windowed,
// presents it. Returns false when the window was closed.
bool frame(const GSRegisters &priv, uint64_t vsyncTick);

// Display aspect for presentation (e.g. 16:9 with the widescreen patch).
void setAspect(float aspect);
// Client area size; applied on the next message pump (ignored in fullscreen).
void setWindowSize(unsigned w, unsigned h);
void windowSize(unsigned &w, unsigned &h);
// The aspect the picture is shown at (4:3 unless a patch says otherwise).
float displayAspect();
// F12 was pressed since the last call.
bool takeScreenshotRequest();
// A short message in the bottom-right corner.
void toast(const std::string &text, double seconds = 4.0);

// Process window messages only (between guest frames). False once closed.
bool pumpWindow();
// While the game is paused: process messages and show the last frame again
// with the UI on top (~60 Hz). False once closed.
bool redraw();

// Read back the last scanned-out frame as RGBA8 (for screenshots).
bool readback(std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height);

// On-screen UI. The callback draws the settings menu with Dear ImGui while it
// is open (F1, or Select+Start through toggleMenu()); F2 shows frame rates.
void setUiCallback(std::function<void()> draw);
// Drawn every frame while F3's panel is on (menu open or not).
void setHudCallback(std::function<void()> draw);
bool hudVisible();
void setHudVisible(bool on);
bool menuOpen();
void setMenuOpen(bool open);
void toggleMenu();
bool showFps();
void setShowFps(bool show);

// Live settings.
unsigned superSampling();
void setSuperSampling(unsigned n);
bool fsr();
void setFsr(bool on);
bool vsync();
void setVsync(bool on);
bool fullscreen();
void setFullscreen(bool on);
void requestClose();

// Window events since the last call (for the input layer).
bool windowFocused();
void *nativeWindow();
} // namespace kh2pgs
