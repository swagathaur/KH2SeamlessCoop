// High-level replacement for Sony's libpad2 (DualShock 2 driver library), which
// KH2 Final Mix uses instead of libpad. The PS2Recomp runtime implements only
// libpad, so these functions are bound by address and served from the
// runtime's host pad state (keyboard/gamepad and DQ8_PAD_SCRIPT input).
//
// Contract, from KH2's callers (FUN_00167b98 init, FUN_00167728 socket,
// FUN_001678f0 per-frame read):
//   scePad2GetState(socket)            1 = connected and stable; anything else = not ready
//   scePad2GetButtonProfile(s, buf)    >= 0; buf[2] & 0xF == 0xF enables analog handling
//   scePad2Read(socket, buf)           >= 0; buf[0..1] buttons active-low (same bit order
//                                      as libpad bytes 2..3), buf[2..5] sticks rx,ry,lx,ly,
//                                      buf[6..17] pressure
// i.e. a libpad report without its two header bytes.

#include "pad2_hle.h"

#include <cstdio>
#include <cstring>

#include "ps2_runtime.h"
#include "runtime/ps2_pad.h"
#include "host_input.h"

namespace kh2pad2
{
namespace
{
// Function starts in SLPM_666.75 (ps2_analyzer SCE signature matches).
constexpr uint32_t kInit = 0x002F5C70u;
constexpr uint32_t kEnd = 0x002F5CB0u;
constexpr uint32_t kCreateSocket = 0x002F5D18u;
constexpr uint32_t kDeleteSocket = 0x002F5E20u;
constexpr uint32_t kRead = 0x002F5E78u;
constexpr uint32_t kGetButtonProfile = 0x002F5F50u;
constexpr uint32_t kGetState = 0x002F6020u;
constexpr uint32_t kGetButtonInfo = 0x002F6160u;
constexpr uint32_t kSetButtonOrder = 0x002F6440u;
constexpr uint32_t kDbcInit = 0x002F4A78u;
constexpr uint32_t kVibGetProfile = 0x002F6528u;
constexpr uint32_t kVibSetActParam = 0x002F65A8u;

constexpr int kMaxSockets = 4;
int g_connectedPorts = 1; // only port 0 is fed by host input for now
int g_nextSocket = 0;
int g_socketPort[kMaxSockets] = {-1, -1, -1, -1};

uint8_t *guest(uint8_t *rdram, uint32_t addr)
{
    return rdram + (addr & PS2_RAM_MASK);
}

void finish(R5900Context *ctx, int32_t value)
{
    setReturnS32(ctx, value);
    ctx->pc = getRegU32(ctx, 31);
}

bool socketConnected(int socket)
{
    return socket >= 0 && socket < kMaxSockets && g_socketPort[socket] >= 0 &&
           g_socketPort[socket] < g_connectedPorts;
}

void ok(uint8_t *, R5900Context *ctx, PS2Runtime *) { finish(ctx, 1); }

void createSocket(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    // KH2 creates one socket per port in order (port 0, then port 1).
    const int socket = g_nextSocket < kMaxSockets ? g_nextSocket++ : -1;
    if (socket >= 0)
        g_socketPort[socket] = socket;
    std::printf("[pad2] CreateSocket -> %d\n", socket);
    finish(ctx, socket);
}

void deleteSocket(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const int socket = static_cast<int>(getRegU32(ctx, 4));
    if (socket >= 0 && socket < kMaxSockets)
        g_socketPort[socket] = -1;
    finish(ctx, 1);
}

void getState(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    finish(ctx, socketConnected(static_cast<int>(getRegU32(ctx, 4))) ? 1 : 0);
}

void getButtonProfile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    // Report a full DualShock 2: every button, both sticks, pressure.
    uint8_t *profile = guest(rdram, getRegU32(ctx, 5));
    std::memset(profile, 0xFF, 4);
    finish(ctx, socketConnected(static_cast<int>(getRegU32(ctx, 4))) ? 4 : -1);
}

void read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const int socket = static_cast<int>(getRegU32(ctx, 4));
    uint8_t *out = guest(rdram, getRegU32(ctx, 5));
    if (!socketConnected(socket))
    {
        finish(ctx, -1);
        return;
    }
    uint8_t report[32];
    PSPadBackend backend;
    backend.readState(g_socketPort[socket], 0, report, sizeof(report));
    std::memcpy(out, report + 2, 30);
    // Pressure bytes: libpad order right,left,up,down,tri,circle,cross,square,L1,R1,L2,R2.
    const uint16_t held = static_cast<uint16_t>(~(report[2] | (report[3] << 8)));
    static const uint16_t kPressureBits[12] = {0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000,
                                               0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};
    for (int i = 0; i < 12; ++i)
        out[6 + i] = (held & kPressureBits[i]) ? 0xFF : 0x00;
    finish(ctx, 18);
}

void getButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    std::memset(guest(rdram, getRegU32(ctx, 5)), 0, 4);
    finish(ctx, 0);
}


// libvib (DualShock 2 vibration). sceVibGetProfile(socket, profile) reports the
// actuators: bit 0 = small motor (on/off), bit 1 = large motor (0-255).
void vibGetProfile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const int socket = static_cast<int>(getRegU32(ctx, 4));
    if (!socketConnected(socket))
    {
        finish(ctx, -1);
        return;
    }
    guest(rdram, getRegU32(ctx, 5))[0] = 0x03;
    finish(ctx, 1);
}

// sceVibSetActParam(socket, profileSize, profile, dataSize, data): the data
// holds one field per actuator named in the profile, packed from bit 0 in
// profile order: 1 bit for the small motor, then 8 bits for the large one.
void vibSetActParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const int socket = static_cast<int>(getRegU32(ctx, 4));
    const int profileSize = static_cast<int>(getRegU32(ctx, 5));
    const uint8_t *profile = guest(rdram, getRegU32(ctx, 6));
    const int dataSize = static_cast<int>(getRegU32(ctx, 7));
    const uint8_t *data = guest(rdram, getRegU32(ctx, 8));
    static bool loggedArgs = false;
    if (!loggedArgs)
    {
        loggedArgs = true;
        std::printf("[pad2] first sceVibSetActParam(a0=%d a1=%d a2=0x%08X a3=%d t0=0x%08X)\n", socket, profileSize,
                    getRegU32(ctx, 6), dataSize, getRegU32(ctx, 8));
    }
    if (!socketConnected(socket) || g_socketPort[socket] != 0 || profileSize < 1 || dataSize < 1)
    {
        finish(ctx, 1);
        return;
    }
    uint32_t bits = data[0] | (dataSize > 1 ? uint32_t(data[1]) << 8 : 0u);
    float small = 0.0f, large = 0.0f;
    if (profile[0] & 0x01)
    {
        small = (bits & 1u) ? 1.0f : 0.0f;
        bits >>= 1;
    }
    if (profile[0] & 0x02)
        large = float(bits & 0xFFu) / 255.0f;
    static int logged = 0;
    if (logged < 8 && (small > 0 || large > 0))
    {
        ++logged;
        std::printf("[pad2] vibration small=%.0f large=%.2f (profile %02x data %02x %02x)\n", small, large, profile[0],
                    data[0], dataSize > 1 ? data[1] : 0);
    }
    kh2input::setRumble(small, large);
    finish(ctx, 1);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    struct Binding { uint32_t address; PS2Runtime::RecompiledFunction fn; const char *name; };
    const Binding bindings[] = {
        {kInit, &ok, "scePad2Init"},
        {kEnd, &ok, "scePad2End"},
        {kCreateSocket, &createSocket, "scePad2CreateSocket"},
        {kDeleteSocket, &deleteSocket, "scePad2DeleteSocket"},
        {kRead, &read, "scePad2Read"},
        {kGetButtonProfile, &getButtonProfile, "scePad2GetButtonProfile"},
        {kGetState, &getState, "scePad2GetState"},
        {kGetButtonInfo, &getButtonInfo, "scePad2GetButtonInfo"},
        {kSetButtonOrder, &ok, "scePad2SetButtonOrder"},
        {kDbcInit, &ok, "sceDbcInit"},
        {kVibGetProfile, &vibGetProfile, "sceVibGetProfile"},
        {kVibSetActParam, &vibSetActParam, "sceVibSetActParam"},
    };
    bool all = true;
    for (const Binding &b : bindings)
    {
        if (!runtime.replaceFunction(b.address, b.fn))
        {
            std::fprintf(stderr, "[pad2] could not bind %s at 0x%08X\n", b.name, b.address);
            all = false;
        }
    }
    std::printf("[pad2] libpad2 HLE %s\n", all ? "installed" : "partially installed");
    return all;
}
} // namespace kh2pad2
