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
    // Super-sampling factor: 1, 2, 4, 8 or 16 samples per pixel. The picture
    // goes out at 2x the PS2 resolution with 4 or 8, 4x with 16.
    unsigned superSampling = 1;
    // Keep the samples when the game draws the frame onto itself as a texture
    // (its full-screen effects): without, those passes drop back to 1 sample.
    bool superSampledTextures = true;
    // paraLLEl-GS hack: when the frame is finished by a scaling blit, show the
    // blit's (super-sampled) source instead.
    bool backbufferPromotion = false;
    // paraLLEl-GS hack: always read texture LOD 0 (sharper textures far away).
    bool disableMipmaps = false;
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

// Write the GIF packets of the next whole frame to path ([u32 size][bytes]...),
// for tools/gif_tris.py.
void requestGifCapture(const std::string &path);

// Read back the last scanned-out frame as RGBA8 (for screenshots).
bool readback(std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height);

// On-screen UI. The callback draws the settings menu with Dear ImGui while it
// is open (F1, or Select+Start through toggleMenu()); F2 shows frame rates.
void setUiCallback(std::function<void()> draw);
// Labels drawn over the game picture (co-op name tags): called every frame
// while setWorldOverlayActive(true), before the HUD and menu.
void setWorldCallback(std::function<void()> draw);
void setWorldOverlayActive(bool on);
// Where the game picture is in the window (pixels), for placing such labels.
void pictureRect(float &x, float &y, float &w, float &h);
// Drawn every frame while F3's panel is on (menu open or not).
void setHudCallback(std::function<void()> draw);
// The runtime's own in-game menus (the movie pause menu), drawn while active.
void setOverlayCallback(std::function<void()> draw);
void setOverlayActive(bool on);
bool hudVisible();
void setHudVisible(bool on);
bool menuOpen();
void setMenuOpen(bool open);
void toggleMenu();
bool showFps();
void setShowFps(bool show);

// Live settings.
unsigned superSampling();
bool superSampledTextures();
void setSuperSampledTextures(bool on);
bool sharpestTextures();
void setSharpestTextures(bool on);
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
