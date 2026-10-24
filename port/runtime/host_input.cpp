// Host input when the GPU renderer owns the window (the runtime's raylib
// window, which normally polls input, is not created then).
//
// Controllers: SDL3's gamepad API when built in (Xbox, PlayStation, Switch and
// most others, with rumble), else XInput (Xbox and compatible pads, and most
// others through Steam Input); the first connected pad. Keyboard: rebindable
// (key_bindings.h); by default the runtime's raylib layout (ps2_pad.cpp):
// arrows = D-pad, WASD = left stick, IJKL = right stick (or the mouse with the
// right button held), Enter = Start, Backspace = Select, Tab = fast-forward,
// Q/E = L1/R1, Shift = L2/R2, X/Space = Cross, C/Esc = Circle, Z = Square,
// V = Triangle, R/F = L3/R3.

#include "host_input.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

#include "key_bindings.h"
#include "runtime/ps2_pad_host.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
#endif

#if defined(KH2_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

namespace kh2input
{
namespace
{
// Active-high button masks as the runtime's pad emulation uses them.
constexpr uint32_t PAD_SELECT = 0x0001u, PAD_L3 = 0x0002u, PAD_R3 = 0x0004u, PAD_START = 0x0008u;
constexpr uint32_t PAD_UP = 0x0010u, PAD_RIGHT = 0x0020u, PAD_DOWN = 0x0040u, PAD_LEFT = 0x0080u;
constexpr uint32_t PAD_L2 = 0x0100u, PAD_R2 = 0x0200u, PAD_L1 = 0x0400u, PAD_R1 = 0x0800u;
constexpr uint32_t PAD_TRIANGLE = 0x1000u, PAD_CIRCLE = 0x2000u, PAD_CROSS = 0x4000u, PAD_SQUARE = 0x8000u;

Options g_opt;
uint32_t g_prevHeld = 0;
int g_pad = -1; // XInput user index in use
std::atomic<int> g_padChoice{-1}; // -1 the first controller; 0.. the Nth connected one; -2 none
int g_padChoiceSeen = -1;
unsigned g_scanTick = 0;
std::atomic<uint16_t> g_rumbleSmall{0}, g_rumbleLarge{0};
uint16_t g_rumbleSentSmall = 0, g_rumbleSentLarge = 0;
std::atomic<bool> g_rumbleOn{true};
std::atomic<bool> g_suppressed{false};
std::atomic<uint32_t> g_blocked{0}; // buttons the game does not see now
std::atomic<bool> g_gameBlocked{false}; // the game sees an idle pad (a downed co-op player)
std::atomic<uint32_t> g_rawHeld{0};   // buttons before suppression (our own menus read these)
std::atomic<uint32_t> g_rawSticks{0x80808080u};
void (*g_comboFn)() = nullptr;
bool g_comboHeld = false;
bool g_fastForward = false;
std::atomic<bool> g_mouseCamera{true};
std::atomic<float> g_mouseSensitivity{1.0f};
std::mutex g_keysMutex;
kh2keys::Bindings g_keys = kh2keys::Bindings::defaults();
std::atomic<bool> g_capturing{false}; // the menu is waiting for a key to bind
double g_skipSince = -1.0;
std::atomic<double> g_skipHeld{0.0};
std::mutex g_nameMutex;
std::string g_padName;

uint8_t axisByte(int v, bool invert);

#if defined(KH2_HAVE_SDL3)
bool g_sdlReady = false;
SDL_Gamepad *g_sdlPad = nullptr;
unsigned g_sdlScan = 0;
uint16_t g_sdlSentSmall = 0, g_sdlSentLarge = 0;
uint64_t g_sdlRumbleAt = 0;

void sdlInit()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    g_sdlReady = SDL_Init(SDL_INIT_GAMEPAD);
    if (!g_sdlReady)
        return;
    // Nothing drains SDL's event queue: poll state instead.
    SDL_SetGamepadEventsEnabled(false);
    SDL_SetJoystickEventsEnabled(false);
}

// Fills the pad state from an SDL gamepad; false when there is none.
bool sdlPoll(uint32_t &held, uint8_t &lx, uint8_t &ly, uint8_t &rx, uint8_t &ry)
{
    if (!g_sdlReady)
        return false;
    SDL_UpdateGamepads(); // also notices pads being plugged in
    if (g_sdlPad && !SDL_GamepadConnected(g_sdlPad))
    {
        SDL_CloseGamepad(g_sdlPad);
        g_sdlPad = nullptr;
        std::lock_guard<std::mutex> lock(g_nameMutex);
        g_padName.clear();
    }
    const int choice = g_padChoice.load();
    if (choice != g_padChoiceSeen && g_sdlPad)
    {
        SDL_CloseGamepad(g_sdlPad); // another controller was chosen
        g_sdlPad = nullptr;
        std::lock_guard<std::mutex> lock(g_nameMutex);
        g_padName.clear();
    }
#if !defined(_WIN32)
    g_padChoiceSeen = choice; // (on Windows, poll() notes it after the XInput side has seen it too)
#endif
    if (choice == -2)
        return false;
    if (!g_sdlPad && (g_sdlScan++ % 60) == 0)
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        const int want = choice < 0 ? 0 : choice;
        if (ids && count > want)
        {
            g_sdlPad = SDL_OpenGamepad(ids[want]);
            if (g_sdlPad)
            {
                const char *name = SDL_GetGamepadName(g_sdlPad);
                std::lock_guard<std::mutex> lock(g_nameMutex);
                g_padName = name ? name : "Controller";
                g_sdlSentSmall = g_sdlSentLarge = 0;
            }
        }
        SDL_free(ids);
    }
    if (!g_sdlPad)
        return false;
    auto button = [](SDL_GamepadButton b) { return SDL_GetGamepadButton(g_sdlPad, b); };
    const uint32_t bottom = g_opt.confirmOnBottom ? PAD_CIRCLE : PAD_CROSS;
    const uint32_t right = g_opt.confirmOnBottom ? PAD_CROSS : PAD_CIRCLE;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_UP)) held |= PAD_UP;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) held |= PAD_DOWN;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) held |= PAD_LEFT;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) held |= PAD_RIGHT;
    if (button(SDL_GAMEPAD_BUTTON_START)) held |= PAD_START;
    if (button(SDL_GAMEPAD_BUTTON_BACK) || button(SDL_GAMEPAD_BUTTON_TOUCHPAD)) held |= PAD_SELECT;
    if (button(SDL_GAMEPAD_BUTTON_LEFT_STICK)) held |= PAD_L3;
    if (button(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) held |= PAD_R3;
    if (button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) held |= PAD_L1;
    if (button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) held |= PAD_R1;
    if (button(SDL_GAMEPAD_BUTTON_SOUTH)) held |= bottom;
    if (button(SDL_GAMEPAD_BUTTON_EAST)) held |= right;
    if (button(SDL_GAMEPAD_BUTTON_WEST)) held |= PAD_SQUARE;
    if (button(SDL_GAMEPAD_BUTTON_NORTH)) held |= PAD_TRIANGLE;
    if (SDL_GetGamepadAxis(g_sdlPad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) held |= PAD_L2;
    if (SDL_GetGamepadAxis(g_sdlPad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) held |= PAD_R2;
    // SDL sticks are positive down, like the PS2's (0 = up).
    lx = axisByte(SDL_GetGamepadAxis(g_sdlPad, SDL_GAMEPAD_AXIS_LEFTX), false);
    ly = axisByte(SDL_GetGamepadAxis(g_sdlPad, SDL_GAMEPAD_AXIS_LEFTY), false);
    rx = axisByte(SDL_GetGamepadAxis(g_sdlPad, SDL_GAMEPAD_AXIS_RIGHTX), false);
    ry = axisByte(SDL_GetGamepadAxis(g_sdlPad, SDL_GAMEPAD_AXIS_RIGHTY), false);

    // Rumble: SDL effects time out, so refresh them while they last.
    const bool rumble = g_rumbleOn && !g_suppressed;
    const uint16_t small = rumble ? g_rumbleSmall.load() : 0, large = rumble ? g_rumbleLarge.load() : 0;
    const uint64_t now = SDL_GetTicks();
    if (small != g_sdlSentSmall || large != g_sdlSentLarge || ((small || large) && now - g_sdlRumbleAt > 100))
    {
        SDL_RumbleGamepad(g_sdlPad, large, small, (small || large) ? 250 : 0);
        g_sdlSentSmall = small;
        g_sdlSentLarge = large;
        g_sdlRumbleAt = now;
    }
    return true;
}
#endif

uint8_t axisByte(int v, bool invert)
{
    // XInput: -32768..32767, up positive. PS2: 0..255, 0x80 centre, up = 0.
    int b = (v + 32768) >> 8;
    if (invert)
        b = 255 - b;
    return static_cast<uint8_t>(b < 0 ? 0 : b > 255 ? 255 : b);
}

#if defined(_WIN32)
bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }


// Mouse camera: while the right button is held, mouse movement becomes the
// right stick. The stick sets a turning rate, so the deflection follows the
// mouse's recent speed (an exponentially decaying sum of its movement), and
// the cursor is held in place so it never runs into the screen edge.
struct MouseLook
{
    bool active = false;
    POINT anchor{};
    double accX = 0, accY = 0;
    LARGE_INTEGER last{};
};
MouseLook g_mouse;

bool mouseStick(uint8_t &rx, uint8_t &ry)
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double dt = g_mouse.last.QuadPart ? double(now.QuadPart - g_mouse.last.QuadPart) / double(freq.QuadPart) : 0.0;
    g_mouse.last = now;
    if (!g_mouseCamera || !key(VK_RBUTTON))
    {
        g_mouse.active = false;
        return false;
    }
    POINT p;
    GetCursorPos(&p);
    if (!g_mouse.active)
    {
        g_mouse.active = true;
        g_mouse.anchor = p;
        g_mouse.accX = g_mouse.accY = 0;
        return true;
    }
    constexpr double kTau = 0.06; // seconds of movement the deflection averages
    const double decay = std::exp(-dt / kTau);
    g_mouse.accX = g_mouse.accX * decay + (p.x - g_mouse.anchor.x);
    g_mouse.accY = g_mouse.accY * decay + (p.y - g_mouse.anchor.y);
    if (p.x != g_mouse.anchor.x || p.y != g_mouse.anchor.y)
        SetCursorPos(g_mouse.anchor.x, g_mouse.anchor.y);
    // ~1500 px/s at sensitivity 1 is a full deflection.
    const double scale = 127.0 * g_mouseSensitivity.load() / 1500.0 / kTau;
    auto axis = [&](double acc) {
        const double v = 128.0 + acc * scale;
        return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
    };
    rx = axis(g_mouse.accX);
    ry = axis(g_mouse.accY);
    return true;
}
#endif
} // namespace

void setMouseCamera(bool on) { g_mouseCamera = on; }
bool mouseCamera() { return g_mouseCamera; }
void setMouseSensitivity(float s) { g_mouseSensitivity = s; }
float mouseSensitivity() { return g_mouseSensitivity; }

void init(const Options &options)
{
    g_opt = options;
    g_padChoice = options.controller;
#if defined(KH2_HAVE_SDL3)
    sdlInit();
#endif
}
void setRumbleEnabled(bool on) { g_rumbleOn = on; }
bool rumbleEnabled() { return g_rumbleOn; }
void setConfirmOnBottom(bool on) { g_opt.confirmOnBottom = on; }
bool confirmOnBottom() { return g_opt.confirmOnBottom; }
void setSuppressed(bool on) { g_suppressed = on; }
void setBlockedButtons(uint32_t mask) { g_blocked = mask; }
void setGameBlocked(bool on) { g_gameBlocked = on; }
void setController(int choice) { g_padChoice = choice; }
int controller() { return g_padChoice.load(); }
uint32_t rawButtons() { return g_rawHeld.load(); }
uint32_t rawSticks() { return g_rawSticks.load(); }
void setScriptRaw(uint32_t buttons, uint32_t sticks)
{
    g_rawHeld = buttons;
    g_rawSticks = sticks;
}
void setKeyBindings(const kh2keys::Bindings &b)
{
    std::lock_guard<std::mutex> lock(g_keysMutex);
    g_keys = b;
}
kh2keys::Bindings keyBindings()
{
    std::lock_guard<std::mutex> lock(g_keysMutex);
    return g_keys;
}
void setCapturingKey(bool on) { g_capturing = on; }
double skipHeldSeconds() { return g_skipHeld.load(); }
int newlyPressedKey()
{
#if defined(_WIN32)
    // Keys (and mouse buttons) that went down since the last call.
    static bool down[256] = {};
    int found = 0;
    for (int vk = 1; vk < 256; ++vk)
    {
        const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (now && !down[vk] && !found && !kh2keys::reserved(vk) && vk != 0x10 && vk != 0x11 && vk != 0x12)
            found = vk;
        down[vk] = now;
    }
    return found;
#else
    return 0;
#endif
}
void setMenuComboCallback(void (*fn)()) { g_comboFn = fn; }
bool controllerConnected()
{
#if defined(KH2_HAVE_SDL3)
    if (g_sdlPad)
        return true;
#endif
    return g_pad >= 0;
}

std::string controllerName()
{
    std::lock_guard<std::mutex> lock(g_nameMutex);
    if (!g_padName.empty())
        return g_padName;
    return g_pad >= 0 ? "Xbox controller (XInput)" : "";
}

void setRumble(float small, float large)
{
    auto clamp = [](float v) { return static_cast<uint16_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f); };
    g_rumbleSmall = clamp(small);
    g_rumbleLarge = clamp(large);
}

bool fastForwardHeld() { return g_fastForward; }

void poll(bool keyboardFocus)
{
    uint32_t held = 0;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    bool sdlPad = false;
#if defined(KH2_HAVE_SDL3)
    sdlPad = sdlPoll(held, lx, ly, rx, ry);
#endif
#if defined(_WIN32)
    // Look for a pad now and then rather than every call: XInputGetState on an
    // empty slot is slow. (Not when SDL has a pad: it would be the same one.)
    const int choice = g_padChoice.load();
    if (choice != g_padChoiceSeen)
    {
        g_pad = -1; // another controller was chosen: look again now
        g_scanTick = 0;
    }
    if (!sdlPad && g_pad < 0 && choice != -2 && (g_scanTick++ % 120) == 0)
    {
        // The Nth connected controller (the first by default).
        int seen = 0;
        const int want = choice < 0 ? 0 : choice;
        for (DWORD i = 0; i < XUSER_MAX_COUNT && g_pad < 0; ++i)
        {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) == ERROR_SUCCESS && seen++ == want)
                g_pad = static_cast<int>(i);
        }
    }
    g_padChoiceSeen = choice;
    if (!sdlPad && g_pad >= 0)
    {
        XINPUT_STATE st{};
        if (XInputGetState(static_cast<DWORD>(g_pad), &st) != ERROR_SUCCESS)
        {
            g_pad = -1;
        }
        else
        {
            const WORD b = st.Gamepad.wButtons;
            const uint32_t bottom = g_opt.confirmOnBottom ? PAD_CIRCLE : PAD_CROSS;
            const uint32_t right = g_opt.confirmOnBottom ? PAD_CROSS : PAD_CIRCLE;
            if (b & XINPUT_GAMEPAD_DPAD_UP) held |= PAD_UP;
            if (b & XINPUT_GAMEPAD_DPAD_DOWN) held |= PAD_DOWN;
            if (b & XINPUT_GAMEPAD_DPAD_LEFT) held |= PAD_LEFT;
            if (b & XINPUT_GAMEPAD_DPAD_RIGHT) held |= PAD_RIGHT;
            if (b & XINPUT_GAMEPAD_START) held |= PAD_START;
            if (b & XINPUT_GAMEPAD_BACK) held |= PAD_SELECT;
            if (b & XINPUT_GAMEPAD_LEFT_THUMB) held |= PAD_L3;
            if (b & XINPUT_GAMEPAD_RIGHT_THUMB) held |= PAD_R3;
            if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) held |= PAD_L1;
            if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) held |= PAD_R1;
            if (b & XINPUT_GAMEPAD_A) held |= bottom;
            if (b & XINPUT_GAMEPAD_B) held |= right;
            if (b & XINPUT_GAMEPAD_X) held |= PAD_SQUARE;
            if (b & XINPUT_GAMEPAD_Y) held |= PAD_TRIANGLE;
            if (st.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) held |= PAD_L2;
            if (st.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) held |= PAD_R2;
            lx = axisByte(st.Gamepad.sThumbLX, false);
            ly = axisByte(st.Gamepad.sThumbLY, true);
            rx = axisByte(st.Gamepad.sThumbRX, false);
            ry = axisByte(st.Gamepad.sThumbRY, true);

            const bool rumble = g_rumbleOn && !g_suppressed;
            const uint16_t s = rumble ? g_rumbleSmall.load() : 0, l = rumble ? g_rumbleLarge.load() : 0;
            if (s != g_rumbleSentSmall || l != g_rumbleSentLarge)
            {
                XINPUT_VIBRATION vib{l, s}; // left motor = low frequency (large)
                XInputSetState(static_cast<DWORD>(g_pad), &vib);
                g_rumbleSentSmall = s;
                g_rumbleSentLarge = l;
            }
        }
    }
    if (keyboardFocus && !g_capturing)
    {
        kh2keys::Bindings keys;
        {
            std::lock_guard<std::mutex> lock(g_keysMutex);
            keys = g_keys;
        }
        auto any = [&](kh2keys::Action a) {
            for (int vk : keys.keys[a])
                if (key(vk))
                    return true;
            return false;
        };
        static const std::pair<kh2keys::Action, uint32_t> kButtons[] = {
            {kh2keys::DpadUp, PAD_UP},     {kh2keys::DpadDown, PAD_DOWN}, {kh2keys::DpadLeft, PAD_LEFT},
            {kh2keys::DpadRight, PAD_RIGHT}, {kh2keys::Cross, PAD_CROSS}, {kh2keys::Circle, PAD_CIRCLE},
            {kh2keys::Square, PAD_SQUARE}, {kh2keys::Triangle, PAD_TRIANGLE}, {kh2keys::L1, PAD_L1},
            {kh2keys::R1, PAD_R1},         {kh2keys::L2, PAD_L2},         {kh2keys::R2, PAD_R2},
            {kh2keys::L3, PAD_L3},         {kh2keys::R3, PAD_R3},         {kh2keys::Start, PAD_START},
            {kh2keys::Select, PAD_SELECT},
        };
        for (const auto &[action, mask] : kButtons)
            if (any(action))
                held |= mask;
        g_fastForward = any(kh2keys::FastForward);
        auto axis = [&](kh2keys::Action low, kh2keys::Action high, uint8_t fallback) -> uint8_t {
            const bool lo = any(low), hi = any(high);
            if (!lo && !hi)
                return fallback;
            return lo == hi ? 0x80 : lo ? 0x00 : 0xFF;
        };
        rx = axis(kh2keys::RStickLeft, kh2keys::RStickRight, rx);
        ry = axis(kh2keys::RStickUp, kh2keys::RStickDown, ry);
        if (!g_suppressed)
            mouseStick(rx, ry);
        lx = axis(kh2keys::LStickLeft, kh2keys::LStickRight, lx);
        ly = axis(kh2keys::LStickUp, kh2keys::LStickDown, ly);
    }
#else
    (void)keyboardFocus;
#endif
    if (!keyboardFocus)
        g_fastForward = false;
    // Select+Start: settings menu.
    const bool combo = (held & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START);
    if (combo && !g_comboHeld && g_comboFn)
        g_comboFn();
    g_comboHeld = combo;
    g_rawHeld = held;
    g_rawSticks = (uint32_t(rx) << 24) | (uint32_t(ry) << 16) | (uint32_t(lx) << 8) | ly;
    if (g_suppressed || combo)
    {
        held = 0;
        lx = ly = rx = ry = 0x80;
    }
    if (g_gameBlocked)
    {
        // Downed: the camera stays free (right stick), nothing else.
        held = 0;
        lx = ly = 0x80;
    }
    // The skip button (Cross: B by default, X/Space on the keyboard), timed
    // before the menu's suppression so a hold is seen whatever the game does.
    {
        using namespace std::chrono;
        const double t = duration<double>(steady_clock::now().time_since_epoch()).count();
        if (held & PAD_CROSS)
        {
            if (g_skipSince < 0)
                g_skipSince = t;
            g_skipHeld = t - g_skipSince;
        }
        else
        {
            g_skipSince = -1.0;
            g_skipHeld = 0.0;
        }
    }
    held &= ~g_blocked.load();
    const uint32_t pressed = held & ~g_prevHeld;
    g_prevHeld = held;
    const uint32_t sticks = (uint32_t(rx) << 24) | (uint32_t(ry) << 16) | (uint32_t(lx) << 8) | ly;
    ps2PadPublishHostState(held, pressed, sticks);
}
} // namespace kh2input
