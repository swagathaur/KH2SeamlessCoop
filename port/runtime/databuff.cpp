#include "databuff.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2databuff
{
namespace
{
constexpr uint32_t kAlloc = 0x001B1400u;     // YS::DATABUFF::Alloc(const char *name, int)
constexpr uint32_t kPoolPtr = 0x00350E88u;   // its next free address
constexpr uint32_t kPoolEnd = 0x01D44400u;   // the item-picture buffer's start
constexpr uint32_t kSpareBase = 0x01EC0000u; // unused by the game; taken from the top of the runtime's heap
constexpr uint32_t kSpareEnd = 0x01EF0000u;  // the callback stacks (main.cpp)

PS2Runtime::RecompiledFunction g_original = nullptr;
std::mutex g_mutex;
uint32_t g_spare = kSpareBase;

uint32_t rd32(const uint8_t *rdram, uint32_t a)
{
    uint32_t v;
    std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4);
    return v;
}

void alloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t name = getRegU32(ctx, 4);
    const uint32_t before = rd32(rdram, kPoolPtr);
    kh2::callOriginal(g_original, rdram, ctx, runtime);
    const uint32_t got = getRegU32(ctx, 2);
    const uint32_t after = rd32(rdram, kPoolPtr);
    if (!got || got != before || after <= kPoolEnd || after <= before)
        return;
    const uint32_t size = after - before;
    char file[48] = {};
    if (name && name < 0x02000000u)
        std::memcpy(file, rdram + name, sizeof(file) - 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_spare + size > kSpareEnd)
    {
        std::fprintf(stderr,
                     "[databuff] %s (%u bytes) runs past the data pool (0x%08X-0x%08X) and there is no spare room "
                     "left: left where the game put it\n",
                     file, size, got, after);
        return;
    }
    // Undo the pool's bump (the next file starts where this one would have)
    // and hand out spare memory instead.
    std::memcpy(rdram + kPoolPtr, &before, 4);
    const uint32_t at = g_spare;
    g_spare += (size + 63u) & ~63u;
    setReturnU32(ctx, at);
    std::printf("[databuff] %s (%u bytes) would run past the data pool's end (0x%08X > 0x%08X): put at 0x%08X\n",
                file, size, after, kPoolEnd, at);
    std::fflush(stdout);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    // Only JAL callers (0x001B1458, 0x001B14A0): the function table reaches us.
    g_original = runtime.lookupFunction(kAlloc);
    if (!g_original || !runtime.replaceFunction(kAlloc, &alloc))
    {
        std::fprintf(stderr, "[databuff] could not hook YS::DATABUFF::Alloc (0x%08X)\n", kAlloc);
        g_original = nullptr;
        return false;
    }
    return true;
}
} // namespace kh2databuff
