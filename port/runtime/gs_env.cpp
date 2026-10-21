// sceGsResetGraph wrapper that also fills libgraph's GParam.
//
// sceGsResetGraph stays a runtime stub (it resets the emulated GS), but the
// real one also records the video mode in a static sceGsGParam that
// sceGsGetGParam (0x002F1FA8, guest code) returns:
//   short sceGsInterMode, sceGsOutMode, sceGsFFMode, sceGsVersion
// libgraph's sceGsSetDefDispEnv, which KH2 now runs as guest code (see
// tools/gen_stubs.py), reads it to pick MAGH, DX, DY and DW. Left at zero the
// display setup is wrong, so this wrapper writes it the way ResetGraph does for
// modes 0 and 5 (the only ones that set it).

#include "gs_env.h"

#include <cstdio>
#include <cstring>

#include "ps2_runtime.h"
#include "hle_call.h"

namespace kh2gs
{
namespace
{
constexpr uint32_t kResetGraph = 0x002F1E18u;
constexpr uint32_t kGetGParam = 0x002F1FA8u; // lui v0, hi; jr ra; addiu v0, v0, lo
constexpr uint16_t kGsRevision = 0x1B;       // CSR.REV of a late GS; libgraph only stores it

PS2Runtime::RecompiledFunction g_resetGraph = nullptr;
uint32_t g_gparam = 0;

uint32_t word(const uint8_t *rdram, uint32_t addr)
{
    uint32_t w;
    std::memcpy(&w, rdram + (addr & PS2_RAM_MASK), 4);
    return w;
}

void resetGraph(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int16_t mode = static_cast<int16_t>(getRegU32(ctx, 4));
    const int16_t inter = static_cast<int16_t>(getRegU32(ctx, 5));
    const int16_t omode = static_cast<int16_t>(getRegU32(ctx, 6));
    const int16_t ffmode = static_cast<int16_t>(getRegU32(ctx, 7));
    kh2::callOriginal(g_resetGraph, rdram, ctx, runtime);
    if ((mode == 0 || mode == 5) && g_gparam)
    {
        const int16_t gp[4] = {inter, omode, static_cast<int16_t>(ffmode != 0), static_cast<int16_t>(kGsRevision)};
        std::memcpy(rdram + (g_gparam & PS2_RAM_MASK), gp, sizeof(gp));
        std::printf("[kh2-gs] ResetGraph(%d): GParam inter=%d omode=%d ffmode=%d\n", mode, inter, omode,
                    ffmode != 0);
    }
}
} // namespace

bool installEnv(PS2Runtime &runtime)
{
    const uint8_t *rdram = runtime.memory().getRDRAM();
    const uint32_t lui = word(rdram, kGetGParam), jr = word(rdram, kGetGParam + 4),
                   addiu = word(rdram, kGetGParam + 8);
    if ((lui & 0xFFFF0000u) != 0x3C020000u || jr != 0x03E00008u || (addiu & 0xFFFF0000u) != 0x24420000u)
    {
        std::fprintf(stderr, "[kh2-gs] sceGsGetGParam at 0x%08X is not the expected lui/jr/addiu\n", kGetGParam);
        return false;
    }
    g_gparam = ((lui & 0xFFFFu) << 16) + static_cast<uint32_t>(static_cast<int16_t>(addiu & 0xFFFFu));
    g_resetGraph = runtime.lookupFunction(kResetGraph);
    if (!g_resetGraph || !runtime.replaceFunction(kResetGraph, &resetGraph))
    {
        std::fprintf(stderr, "[kh2-gs] could not wrap sceGsResetGraph at 0x%08X\n", kResetGraph);
        return false;
    }
    std::printf("[kh2-gs] GParam at 0x%08X\n", g_gparam);
    return true;
}
} // namespace kh2gs
