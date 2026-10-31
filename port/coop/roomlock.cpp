#include "roomlock.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "guest_call.h"
#include "hle_call.h"
#include "names.h"
#include "ps2_runtime.h"

namespace kh2roomlock
{
namespace
{
constexpr uint32_t kRoomShut = 0x00196768;   // (world, room) -> -2 shut, 0 open
constexpr uint32_t kShutText = 0x001967F0;   // (world, room, k) -> the reason's text
constexpr uint32_t kShowInfo = 0x0015B268;   // (text, f12 seconds): the game's message
constexpr uint32_t kInfoSeconds = 0x0036D340; // the float FUN_0019A6C0 passes
constexpr uint32_t kExitAsks = 0x0019A534;   // return address in the exit trigger
constexpr uint32_t kText = 0x01E4DD00u;      // free guest memory (names.cpp's slots end at 0x01E4DCC0)
constexpr uint32_t kTextSize = 0x80;
const char *kMessage = "The others are waiting for me";

PS2Runtime::RecompiledFunction g_shut = nullptr, g_shutText = nullptr;
std::atomic<bool> g_on{false};
std::atomic<uint16_t> g_open{0xFFFF};    // the host's room (world << 8 | room)
std::atomic<uint16_t> g_lastShut{0xFFFF}; // the room we last shut
bool g_textWritten = false;
double g_lastShown = -10.0;

double nowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool testAll()
{
    static const bool on = [] {
        const char *e = std::getenv("KH2_TEST_ROOM_LOCK");
        return e && *e == '1';
    }();
    return on;
}

uint32_t text(uint8_t *rdram)
{
    if (!g_textWritten)
    {
        g_textWritten = true;
        const std::string t = kh2names::encode(kMessage, kTextSize - 1);
        std::memset(rdram + kText, 0, kTextSize);
        std::memcpy(rdram + kText, t.data(), t.size());
    }
    return kText;
}

void shutHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t world = getRegU32(ctx, 4) & 0xFF, room = getRegU32(ctx, 5) & 0xFF;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t obj = getRegU32(ctx, 17); // the exit trigger's s1: the one walking out
    if (!kh2::callOriginalReturned(g_shut, rdram, ctx, runtime, kRoomShut))
        return;
    if (getRegU32(ctx, 2) != 0 || ra != kExitAsks || !(g_on.load() || testAll()))
        return;
    const uint16_t key = uint16_t(world << 8 | room);
    if (!testAll() && key == g_open.load())
        return;
    setReturnU32(ctx, 0xFFFFFFFEu); // shut
    g_lastShut = key;
    // The exit's kind (*(obj + 0x6C) + 2): 0 jumps straight away, and shut
    // just doesn't; the others say why themselves (through shutTextHook).
    uint32_t info = 0;
    std::memcpy(&info, rdram + ((obj + 0x6C) & 0x01FFFFFFu), 4);
    uint16_t kind = 0xFFFF;
    if (info >= 0x00100000u && info < 0x02000000u)
        std::memcpy(&kind, rdram + info + 2, 2);
    if (kind != 0 || nowSec() - g_lastShown < 4.0)
        return;
    g_lastShown = nowSec();
    float seconds = 0;
    std::memcpy(&seconds, rdram + kInfoSeconds, 4);
    const uint32_t at = text(rdram);
    std::printf("[roomlock] the way to %02X-%02X is shut: the host leads\n", world, room);
    std::fflush(stdout);
    kh2guest::postCall(kh2guest::Call{kShowInfo, {at}, {seconds}});
}

void shutTextHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t world = getRegU32(ctx, 4) & 0xFF, room = getRegU32(ctx, 5) & 0xFF;
    if (ctx->pc == kShutText && uint16_t(world << 8 | room) == g_lastShut.load())
    {
        setReturnU32(ctx, text(rdram));
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_shutText, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_shut = runtime.lookupFunction(kRoomShut);
    if (!g_shut || !runtime.replaceFunction(kRoomShut, &shutHook))
    {
        std::fprintf(stderr, "[roomlock] could not hook 0x%08X\n", kRoomShut);
        g_shut = nullptr;
        return false;
    }
    g_shutText = runtime.lookupFunction(kShutText);
    if (!g_shutText || !runtime.replaceFunction(kShutText, &shutTextHook))
        g_shutText = nullptr;
    return true;
}

void set(bool on, uint8_t hostWorld, uint8_t hostRoom)
{
    g_on = on;
    g_open = uint16_t(hostWorld << 8 | hostRoom);
    if (!on)
        g_lastShut = 0xFFFF;
}
} // namespace kh2roomlock
