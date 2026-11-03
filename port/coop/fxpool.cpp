#include "fxpool.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "hle_call.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace kh2fxpool
{
namespace
{
// FUN_001DCDB8(allocator, size, size2): stores the allocator at 0x01D5BB00,
// takes `size` (the particle pool, kept at 0x01D5BB04) and `size2` from it
// through its vtable +0 (kn::MemoryAllocator's alloc, 0x001061D0; the call's
// return address 0x001DCDF4), and hands both to the particle system.
// FUN_001DCED0 gives them back through the vtable +4 (0x001062B8).
constexpr uint32_t kFxInit = 0x001DCDB8, kAlloc = 0x001061D0, kFree = 0x001062B8;
constexpr uint32_t kPoolAllocReturn = 0x001DCDF4;
constexpr uint32_t kOurPool = 0x03C00000u, kOurPoolSize = 0x00400000u;

PS2Runtime::RecompiledFunction g_fxInit = nullptr, g_alloc = nullptr, g_free = nullptr;
std::atomic<bool> g_logged{false};

void fxInitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kFxInit && getRegU32(ctx, 5) < kOurPoolSize)
    {
        if (!g_logged.exchange(true))
        {
            std::printf("[fxpool] the effect pool: %u KB of our own (the game asks %u KB)\n", kOurPoolSize / 1024,
                        getRegU32(ctx, 5) / 1024);
            std::fflush(stdout);
        }
        SET_GPR_U32(ctx, 5, kOurPoolSize);
    }
    kh2::callOriginal(g_fxInit, rdram, ctx, runtime);
}

void allocHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kAlloc && getRegU32(ctx, 31) == kPoolAllocReturn && getRegU32(ctx, 5) == kOurPoolSize)
    {
        SET_GPR_U32(ctx, 2, kOurPool);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_alloc, rdram, ctx, runtime);
}

void freeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t p = getRegU32(ctx, 5) & 0x1FFFFFFFu;
    if (ctx->pc == kFree && p >= kOurPool && p < kOurPool + kOurPoolSize)
    {
        ctx->pc = getRegU32(ctx, 31); // (ours: not the allocator's)
        return;
    }
    kh2::callOriginal(g_free, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    if (const char *e = std::getenv("KH2_FX_POOL"); e && *e == '0')
        return false; // (testing: the game's own 512 KB)
    if (PS2_RAM_SIZE < kOurPool + kOurPoolSize)
        return false;
    g_fxInit = runtime.lookupFunction(kFxInit);
    g_alloc = runtime.lookupFunction(kAlloc);
    g_free = runtime.lookupFunction(kFree);
    if (!g_fxInit || !g_alloc || !g_free || !runtime.replaceFunction(kFxInit, &fxInitHook) ||
        !runtime.replaceFunction(kAlloc, &allocHook) || !runtime.replaceFunction(kFree, &freeHook))
    {
        std::fprintf(stderr, "[fxpool] could not hook the effect pool\n");
        return false;
    }
    return true;
}
} // namespace kh2fxpool
