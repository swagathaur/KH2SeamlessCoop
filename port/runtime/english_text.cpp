#include "english_text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#include "ps2_runtime.h"

namespace kh2english
{
namespace
{
constexpr uint32_t kRegionGet = 0x00105C30u; // lui t7, 0x34; jr ra; lw v0, -0x3504(t7)
constexpr uint32_t kRegion = 0x0033CAFCu;
constexpr int32_t kUs = 2;

// Return addresses (jal + 8) of the "Japanese?" checks in text code.
constexpr uint32_t kTextSites[] = {
    0x0015ABF0u, 0x0015AC04u, // save-slot world name (Tz::MCard::ChgWorldName)
    0x001AAB5Cu, 0x001AAB70u, // font set-up: glyph cell heights
    0x001AB834u,              // font: glyph width scale
    0x00247394u, 0x002473A8u, // name formatting
    0x002476B4u, 0x002476C8u, //
    0x0028DE48u, 0x0028DE5Cu, // Tz::MenuUtil: padding
    0x0028DF40u, 0x0028DF54u, //
    0x002A8650u, 0x002A8664u, // message formatting
    0x002B5F90u, 0x002B5FA4u, // command menu list items
    0x002B6050u, 0x002B6064u, //
};
// Calls that keep the real region (checked, so new callers get logged).
constexpr uint32_t kRealSites[] = {
    0x00105880u,              // save product code (KH2J)
    0x0016797Cu, 0x0016798Cu, // pad: Final Mix button layout, right-stick camera
    0x001AFD20u,              // YS::VOICE (Final Mix voice table)
    0x00232358u, 0x00240140u, 0x002402F0u, // memory card
    0x002BE4C4u,              // worldmap::Libretto
    0x002EF998u, 0x002EFBC8u, // product codes
};

bool g_active = false;
std::mutex g_mutex;
std::set<uint32_t> g_seen;

void regionGet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t ra = getRegU32(ctx, 31);
    int32_t region;
    std::memcpy(&region, rdram + kRegion, 4);
    if (std::find(std::begin(kTextSites), std::end(kTextSites), ra) != std::end(kTextSites))
        region = kUs;
    else if (std::find(std::begin(kRealSites), std::end(kRealSites), ra) == std::end(kRealSites))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_seen.insert(ra).second)
            std::printf("[english] REGION::Get from unlisted 0x%08X (region %d)\n", ra - 8, region);
    }
    setReturnS32(ctx, region);
    ctx->pc = ra;
}

uint32_t word(const uint8_t *rdram, uint32_t addr)
{
    uint32_t w;
    std::memcpy(&w, rdram + (addr & PS2_RAM_MASK), 4);
    return w;
}
} // namespace

bool install(PS2Runtime &runtime)
{
    const uint8_t *rdram = runtime.memory().getRDRAM();
    if (word(rdram, kRegionGet) != 0x3C0F0034u || word(rdram, kRegionGet + 4) != 0x03E00008u ||
        word(rdram, kRegionGet + 8) != 0x8DE2CAFCu)
    {
        std::fprintf(stderr, "[english] REGION::Get at 0x%08X is not the expected code\n", kRegionGet);
        return false;
    }
    if (!runtime.replaceFunction(kRegionGet, &regionGet))
    {
        std::fprintf(stderr, "[english] could not replace REGION::Get\n");
        return false;
    }
    g_active = true;
    std::printf("[english] international text layout\n");
    return true;
}

bool active() { return g_active; }
} // namespace kh2english
