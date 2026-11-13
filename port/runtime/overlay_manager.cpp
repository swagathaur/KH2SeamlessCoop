#include "overlay_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace kh2ovl
{
namespace
{
const uint8_t *g_rdram = nullptr;
std::atomic<const OverlayInfo *> g_active{nullptr};
std::atomic<uint32_t> g_switches{0u};

const OverlayInfo *identify()
{
    uint32_t word = 0;
    std::memcpy(&word, g_rdram + (kSignatureAddress & PS2_RAM_MASK), sizeof(word));
    const OverlayInfo *current = g_active.load(std::memory_order_acquire);
    if (current && current->signature == word)
        return current;
    for (size_t i = 0; i < kOverlayCount; ++i)
    {
        if (kOverlays[i].signature == word)
        {
            g_active.store(&kOverlays[i], std::memory_order_release);
            const uint32_t n = g_switches.fetch_add(1u) + 1u;
            std::printf("[kh2-overlay] #%u resident: %s\n", n, kOverlays[i].name);
            std::fflush(stdout);
            return &kOverlays[i];
        }
    }
    return nullptr;
}

PS2Runtime::FunctionRegion *resolve(uint32_t address, void *)
{
    if (address < kArenaStart || address >= kArenaEnd || !g_rdram)
        return nullptr;
    const OverlayInfo *info = identify();
    if (!info)
    {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            std::fprintf(stderr, "[kh2-overlay] call into arena at 0x%08X with no known overlay resident\n",
                         address);
        return nullptr;
    }
    return &info->region();
}
} // namespace

void install(PS2Runtime &runtime)
{
    g_rdram = runtime.memory().getRDRAM();
    PS2Runtime::setFunctionRegionResolver(&resolve, nullptr);
    std::printf("[kh2-overlay] arena 0x%08X..0x%08X, %zu overlays known\n", kArenaStart, kArenaEnd,
                kOverlayCount);
}

const char *activeName()
{
    // (The one whose signature is in the arena now. The last one called into
    // stays "active" after the arena has been given over to other data (the
    // movie overlay, gone after its movie, while a menu's work used the
    // arena: the movie code polled its stream count there, read the menu's
    // 1.0f as a stream starting, and the game was taken for playing a movie:
    // no HUD, no input, START the movie menu).)
    if (!g_rdram)
        return "none";
    const OverlayInfo *info = identify();
    return info ? info->name : "none";
}

uint32_t switchCount()
{
    return g_switches.load(std::memory_order_acquire);
}
} // namespace kh2ovl
