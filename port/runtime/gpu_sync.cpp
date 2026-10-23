#include "gpu_sync.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"

namespace kh2gpusync
{
namespace
{
constexpr uint32_t kWaitIdle = 0x00134030u;
// lui t6, 0x1000; ori t6, t6, 0x9000; lw t7, 0(t6); andi t7, t7, 0x100
constexpr uint32_t kExpected[] = {0x3C0E1000u, 0x35CE9000u, 0x8DCF0000u, 0x31EF0100u};

void waitIdle(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    setReturnS32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    const char *e = std::getenv("KH2_GPU_SYNC");
    if (e && *e == '1')
        return false;
    const uint8_t *rdram = runtime.memory().getRDRAM();
    for (uint32_t i = 0; i < 4; ++i)
    {
        uint32_t w;
        std::memcpy(&w, rdram + kWaitIdle + i * 4u, 4);
        if (w != kExpected[i])
        {
            std::fprintf(stderr, "[gpu-sync] 0x%08X is not the expected wait loop\n", kWaitIdle);
            return false;
        }
    }
    if (!runtime.replaceFunction(kWaitIdle, &waitIdle))
    {
        std::fprintf(stderr, "[gpu-sync] could not replace 0x%08X\n", kWaitIdle);
        return false;
    }
    std::printf("[gpu-sync] VIF1/VU1/GIF idle wait replaced\n");
    return true;
}
} // namespace kh2gpusync
