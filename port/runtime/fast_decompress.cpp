// KH2's file decompressor, FUN_002EFCC0(buf, outSize, inSize), in native code.
//
// Compressed files (models, motion sets: most of a room's big files) are read
// whole and decompressed in place, back to front: the last non-zero byte is
// the marker; walking back from 5 bytes before it, a byte other than the
// marker is a literal, "marker 0" is the marker as a literal, and "marker D L"
// copies L+3 bytes from D bytes above the output (which runs back from the end
// of the output, buf + outSize - 1, over the input as it goes).
//
// The game's own loop is cheap on our side, but the runtime paces the guest
// to a PS2's EE (each branch charges cycles, and a frame cannot retire before
// its time): a byte at a time, the 2 MB file of a Land of Dragons room took
// 480 ms, a drive form's motion set about 300 ms, while the loader and the
// rest of the game waited. Done here it takes about a millisecond.
// KH2_FAST_DECOMPRESS=0: the game's own. KH2_FAST_DECOMPRESS=check: both, the
// results compared (each file decompressed twice; for tests).
#include "fast_decompress.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hle_call.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace kh2decomp
{
namespace
{
constexpr uint32_t kDecompress = 0x002EFCC0;
PS2Runtime::RecompiledFunction g_original = nullptr;
int g_mode = 1; // 0 off, 1 native, 2 check

// The game's algorithm over any byte store: at(a) is the byte at guest address a.
template <typename At> void decompress(At at, uint32_t buf, uint32_t outSize, uint32_t inSize)
{
    uint32_t p = buf + inSize - 1;
    uint8_t marker = at(p);
    while (marker == 0)
        marker = at(--p);
    uint32_t dst = buf + outSize - 1;
    p -= 5;
    if (p < buf)
        return;
    uint8_t b = at(p);
    for (;;)
    {
        --p;
        if (b != marker)
            at(dst--) = b;
        else
        {
            const uint8_t dist = at(p--);
            if (dist == 0)
                at(dst--) = b;
            else
            {
                const uint32_t len = uint32_t(at(p--)) + 3;
                for (uint32_t i = 0; i < len; ++i, --dst)
                    at(dst) = at(dst + dist);
            }
        }
        if (p < buf)
            return;
        b = at(p);
    }
}

void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc != kDecompress || g_mode == 0)
    {
        kh2::callOriginal(g_original, rdram, ctx, runtime);
        return;
    }
    const uint32_t buf = getRegU32(ctx, 4), outSize = getRegU32(ctx, 5), inSize = getRegU32(ctx, 6);
    const uint32_t span = outSize > inSize ? outSize : inSize;
    if (inSize == 0 || span == 0 || PS2Runtime::isSpecialAddress(buf) || PS2Runtime::isSpecialAddress(buf + span - 1) ||
        (buf & PS2_RAM_MASK) + span > PS2_RAM_SIZE || buf + span < buf)
    {
        static int told = 0;
        if (told++ < 20)
        {
            std::printf("[decompress] %08X out %u in %u: not plain RAM, the game's own\n", buf, outSize, inSize);
            std::fflush(stdout);
        }
        kh2::callOriginal(g_original, rdram, ctx, runtime); // (not a plain RAM buffer: the game's way)
        return;
    }
    uint8_t *const base = rdram + (buf & PS2_RAM_MASK);
    if (g_mode == 2)
    {
        // Ours on a copy, then the game's on the guest's: the same bytes?
        std::vector<uint8_t> copy(base, base + span);
        uint8_t outside = 0;
        bool strayed = false;
        decompress(
            [&](uint32_t a) -> uint8_t & {
                if (a - buf < copy.size())
                    return copy[a - buf];
                strayed = true;
                outside = base[a - buf]; // (past the buffer: read as the guest's)
                return outside;
            },
            buf, outSize, inSize);
        if (strayed)
        {
            std::printf("[decompress] %08X out %u in %u: ours reached past the buffer\n", buf, outSize, inSize);
            std::fflush(stdout);
        }
        const uint32_t ra = getRegU32(ctx, 31);
        kh2::callOriginal(g_original, rdram, ctx, runtime);
        if (ctx->pc != ra)
            return; // (did not finish in one go: not compared)
        static int checked = 0, bad = 0;
        const bool same = std::memcmp(copy.data(), base, span) == 0;
        bad += same ? 0 : 1;
        if (!same || ++checked % 50 == 1)
        {
            std::printf("[decompress] check %d: %08X out %u in %u: %s (%d bad so far)\n", checked, buf, outSize, inSize,
                        same ? "same" : "DIFFERENT", bad);
            std::fflush(stdout);
        }
        return;
    }
    decompress([&](uint32_t a) -> uint8_t & { return base[a - buf]; }, buf, outSize, inSize);
    ctx->pc = getRegU32(ctx, 31);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    if (const char *e = std::getenv("KH2_FAST_DECOMPRESS"))
        g_mode = std::strcmp(e, "0") == 0 ? 0 : std::strcmp(e, "check") == 0 ? 2 : 1;
    g_original = runtime.lookupFunction(kDecompress);
    if (!g_original || !runtime.replaceFunction(kDecompress, &hook))
    {
        g_original = nullptr;
        std::fprintf(stderr, "[decompress] could not hook FUN_002EFCC0: the game's own decompressor\n");
        return false;
    }
    return true;
}
} // namespace kh2decomp
