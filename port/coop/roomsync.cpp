#include "roomsync.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

#include "game_state.h"
#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2roomsync
{
namespace
{
PS2Runtime::RecompiledFunction g_mapJump = nullptr;
std::mutex g_mutex;
std::deque<Location> g_jumps;

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}

void mapJumpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kh2state::addr::MapJump)
    {
        const uint32_t loc = getRegU32(ctx, 4);
        if (loc >= 0x00100000u && loc < 0x02000000u)
        {
            Location l;
            l.world = rdram[loc];
            l.room = rdram[loc + 1];
            l.door = rd<uint16_t>(rdram, loc + 2);
            l.map = rd<uint16_t>(rdram, loc + 4);
            l.battle = rd<uint16_t>(rdram, loc + 6);
            l.event = rd<uint16_t>(rdram, loc + 8);
            l.at = wallMs();
            timing("jump", l.world, l.room, l.door);
            std::lock_guard<std::mutex> lock(g_mutex);
            g_jumps.push_back(l);
            if (g_jumps.size() > 8)
                g_jumps.pop_front();
        }
    }
    kh2::callOriginal(g_mapJump, rdram, ctx, runtime);
}
} // namespace

bool fast()
{
    static const bool on = [] {
        const char *e = std::getenv("KH2_FAST_ROOMS");
        return !e || *e != '0';
    }();
    return on;
}

int64_t wallMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void timing(const char *what, uint8_t world, uint8_t room, int extra)
{
    static const bool on = [] {
        const char *e = std::getenv("KH2_TIMING");
        return !e || *e != '0';
    }();
    if (!on)
        return;
    if (extra >= 0)
        std::printf("[timing] %s %02X-%02X %d ms=%lld\n", what, world, room, extra, (long long)wallMs());
    else
        std::printf("[timing] %s %02X-%02X ms=%lld\n", what, world, room, (long long)wallMs());
    std::fflush(stdout);
}

bool takeJump(Location &out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    while (!g_jumps.empty() && wallMs() - g_jumps.front().at > 2000)
        g_jumps.pop_front();
    if (g_jumps.empty())
        return false;
    out = g_jumps.front();
    g_jumps.pop_front();
    return true;
}

bool install(PS2Runtime &runtime)
{
    g_mapJump = runtime.lookupFunction(kh2state::addr::MapJump);
    if (!g_mapJump || !runtime.replaceFunction(kh2state::addr::MapJump, &mapJumpHook))
    {
        std::fprintf(stderr, "[roomsync] could not hook the map jump\n");
        return false;
    }
    return true;
}
} // namespace kh2roomsync
