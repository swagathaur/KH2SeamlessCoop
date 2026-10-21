// Host input when the GPU renderer owns the window (the runtime's raylib
// window, which normally polls input, is not created then).
//
// Controllers: XInput (Xbox and compatible pads, and most others through
// Steam Input), first connected pad. Keyboard: same layout as the runtime's
// raylib bindings (ps2_pad.cpp): arrows = D-pad, WASD = left stick, IJKL =
// right stick, Enter = Start, Tab = Select, Q/E = L1/R1, Shift = L2/R2,
// X/Space = Cross, C/Esc = Circle, Z = Square, V = Triangle, R/F = L3/R3.

#include "host_input.h"

#include <atomic>
#include <cstdint>

#include "runtime/ps2_pad_host.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#pragma comment(lib, "xinput.lib")
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
unsigned g_scanTick = 0;
std::atomic<uint16_t> g_rumbleSmall{0}, g_rumbleLarge{0};
uint16_t g_rumbleSentSmall = 0, g_rumbleSentLarge = 0;
std::atomic<bool> g_rumbleOn{true};
std::atomic<bool> g_suppressed{false};
void (*g_comboFn)() = nullptr;
bool g_comboHeld = false;

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

uint8_t keyAxis(int lowKey, int highKey, uint8_t fallback)
{
    const bool lo = key(lowKey), hi = key(highKey);
    if (!lo && !hi)
        return fallback;
    return lo == hi ? 0x80 : lo ? 0x00 : 0xFF;
}
#endif
} // namespace

void init(const Options &options) { g_opt = options; }
void setRumbleEnabled(bool on) { g_rumbleOn = on; }
bool rumbleEnabled() { return g_rumbleOn; }
void setConfirmOnBottom(bool on) { g_opt.confirmOnBottom = on; }
bool confirmOnBottom() { return g_opt.confirmOnBottom; }
void setSuppressed(bool on) { g_suppressed = on; }
void setMenuComboCallback(void (*fn)()) { g_comboFn = fn; }
bool controllerConnected() { return g_pad >= 0; }

void setRumble(float small, float large)
{
    auto clamp = [](float v) { return static_cast<uint16_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f); };
    g_rumbleSmall = clamp(small);
    g_rumbleLarge = clamp(large);
}

void poll(bool keyboardFocus)
{
    uint32_t held = 0;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
#if defined(_WIN32)
    // Look for a pad now and then rather than every call: XInputGetState on an
    // empty slot is slow.
    if (g_pad < 0 && (g_scanTick++ % 120) == 0)
    {
        for (DWORD i = 0; i < XUSER_MAX_COUNT && g_pad < 0; ++i)
        {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) == ERROR_SUCCESS)
                g_pad = static_cast<int>(i);
        }
    }
    if (g_pad >= 0)
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
    if (keyboardFocus)
    {
        struct Bind
        {
            int vk;
            uint32_t mask;
        };
        static const Bind kKeys[] = {
            {VK_UP, PAD_UP}, {VK_DOWN, PAD_DOWN}, {VK_LEFT, PAD_LEFT}, {VK_RIGHT, PAD_RIGHT},
            {'X', PAD_CROSS}, {VK_SPACE, PAD_CROSS}, {'C', PAD_CIRCLE}, {VK_ESCAPE, PAD_CIRCLE},
            {'Z', PAD_SQUARE}, {VK_NUMPAD0, PAD_SQUARE}, {'V', PAD_TRIANGLE}, {VK_NUMPAD1, PAD_TRIANGLE},
            {'Q', PAD_L1}, {'E', PAD_R1}, {VK_LSHIFT, PAD_L2}, {VK_RSHIFT, PAD_R2},
            {VK_RETURN, PAD_START}, {VK_TAB, PAD_SELECT}, {'R', PAD_L3}, {'F', PAD_R3},
        };
        for (const Bind &k : kKeys)
            if (key(k.vk))
                held |= k.mask;
        rx = keyAxis('J', 'L', rx);
        ry = keyAxis('I', 'K', ry);
        lx = keyAxis('A', 'D', lx);
        ly = keyAxis('W', 'S', ly);
    }
#else
    (void)keyboardFocus;
#endif
    // Select+Start: settings menu.
    const bool combo = (held & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START);
    if (combo && !g_comboHeld && g_comboFn)
        g_comboFn();
    g_comboHeld = combo;
    if (g_suppressed || combo)
    {
        held = 0;
        lx = ly = rx = ry = 0x80;
    }
    const uint32_t pressed = held & ~g_prevHeld;
    g_prevHeld = held;
    const uint32_t sticks = (uint32_t(rx) << 24) | (uint32_t(ry) << 16) | (uint32_t(lx) << 8) | ly;
    ps2PadPublishHostState(held, pressed, sticks);
}
} // namespace kh2input
