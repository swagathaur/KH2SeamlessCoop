// --dma-log=FRAME,COUNT: decode GIF-channel sceDmaSend calls.
//
// Wraps sceDmaSend (0x002F3918). For the first COUNT calls on the GIF channel
// (0x1000A000) from guest frame FRAME onwards it walks the source chain the way the DMAC
// would (tags in scratchpad or RDRAM) and prints every DMAtag and each GIFtag
// at the start of a payload, before forwarding to the runtime's implementation.
// That shows what KH2 hands the GS without needing the GS side to cooperate.

#include "dma_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_runtime.h"
#include "runtime/ps2_pad_host.h"

namespace kh2dma
{
namespace
{
constexpr uint32_t kSceDmaSend = 0x002F3918u;
constexpr uint32_t kGifChannel = 0x1000A000u;

PS2Runtime::RecompiledFunction g_original = nullptr;
uint64_t g_skip = 0, g_count = 0, g_seen = 0; // g_skip: first guest frame

// Returns a host pointer for 16 bytes at a DMA address (SPR or RDRAM), or null.
const uint8_t *ptr(PS2Runtime *rt, uint32_t addr)
{
    const bool spr = (addr & 0x80000000u) || (addr >> 28) == 7u;
    if (spr)
        return rt->memory().getScratchpad() + (addr & 0x3FF0u);
    return rt->memory().getRDRAM() + (addr & 0x01FFFFF0u);
}

void describeGif(const uint8_t *p, uint32_t qwc, char *out, size_t n)
{
    uint64_t lo, hi;
    std::memcpy(&lo, p, 8);
    std::memcpy(&hi, p + 8, 8);
    const unsigned nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, pre = (lo >> 46) & 1;
    const unsigned prim = (lo >> 47) & 0x7FF, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 0xF;
    std::snprintf(out, n, "GIFtag nloop=%u eop=%u pre=%u prim=%03x flg=%u nreg=%u regs=%016llx (payload %u qw)",
                  nloop, eop, pre, prim, flg, nreg, (unsigned long long)hi, qwc);
}

void walk(PS2Runtime *rt, uint32_t tadr)
{
    static const char *kIds[] = {"refe", "cnt", "next", "ref", "refs", "call", "ret", "end"};
    uint32_t asr[2] = {}, asp = 0;
    for (int i = 0; i < 256; ++i)
    {
        const uint8_t *t = ptr(rt, tadr);
        uint64_t tag;
        std::memcpy(&tag, t, 8);
        const uint32_t qwc = tag & 0xFFFF, id = (tag >> 28) & 7;
        uint32_t addr = (tag >> 32) & 0x7FFFFFFFu;
        if ((tag >> 63) & 1)
            addr |= 0x80000000u;
        uint32_t data = tadr + 16, next = tadr + 16;
        bool end = false;
        switch (id)
        {
        case 0: data = addr; end = true; break;
        case 1: next = data + qwc * 16; break;
        case 2: next = addr; break;
        case 3: case 4: data = addr; break;
        case 5: if (asp < 2) asr[asp++] = data + qwc * 16; next = addr; break;
        case 6: if (asp) next = asr[--asp]; else end = true; break;
        default: end = true; break;
        }
        char gif[200] = "";
        if (qwc)
            describeGif(ptr(rt, data), qwc, gif, sizeof(gif));
        std::printf("[dma]   tag@%08X %-4s qwc=%-4u addr=%08X data@%08X %s\n", tadr, kIds[id], qwc, addr, data,
                    gif);
        // Walk the GIF packets inside the payload; print A+D register writes.
        if (qwc)
        {
            const uint8_t *base = ptr(rt, data);
            uint32_t q = 0;
            while (q < qwc)
            {
                uint64_t lo, hi;
                std::memcpy(&lo, base + q * 16, 8);
                std::memcpy(&hi, base + q * 16 + 8, 8);
                const uint32_t nloop = lo & 0x7FFF, flg = (lo >> 58) & 3;
                const uint32_t nreg = ((lo >> 60) & 0xF) ? ((lo >> 60) & 0xF) : 16;
                if (q)
                {
                    char g[200];
                    describeGif(base + q * 16, qwc - q, g, sizeof(g));
                    std::printf("[dma]     +%u %s\n", q, g);
                }
                ++q;
                if (flg == 0)
                {
                    for (uint32_t i = 0; i < nloop * nreg && q < qwc; ++i, ++q)
                    {
                        const uint32_t r = (hi >> (4 * (i % nreg))) & 0xF;
                        uint64_t d0, d1;
                        std::memcpy(&d0, base + q * 16, 8);
                        std::memcpy(&d1, base + q * 16 + 8, 8);
                        if (r == 0xE)
                            std::printf("[dma]       a+d %02llx = %016llx\n", (unsigned long long)(d1 & 0xFF),
                                        (unsigned long long)d0);
                    }
                }
                else
                {
                    const uint32_t words = flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
                    q += words;
                }
            }
        }
        if (end)
            return;
        tadr = next;
    }
    std::printf("[dma]   (chain longer than 256 tags, stopped)\n");
}

void wrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t channel = getRegU32(ctx, 4), tadr = getRegU32(ctx, 5);
    if (channel == kGifChannel)
    {
        const uint64_t frame = ps2PadCurrentGuestFrame();
        if (frame >= g_skip && g_seen < g_count)
        {
            const uint64_t n = ++g_seen;
            std::printf("[dma] GIF send #%llu (guest frame %llu) tadr=%08X ra=%08X\n", (unsigned long long)n,
                        (unsigned long long)frame, tadr, getRegU32(ctx, 31));
            walk(runtime, tadr);
            std::fflush(stdout);
        }
    }
    g_original(rdram, ctx, runtime);
}
} // namespace

void install(PS2Runtime &runtime, const std::string &spec)
{
    const char *s = spec.c_str();
    char *end = nullptr;
    g_skip = std::strtoull(s, &end, 0);
    g_count = (end && *end == ',') ? std::strtoull(end + 1, nullptr, 0) : 4;
    g_original = runtime.lookupFunction(kSceDmaSend);
    if (!g_original || !runtime.replaceFunction(kSceDmaSend, &wrapper))
    {
        std::fprintf(stderr, "[dma] could not wrap sceDmaSend at 0x%08X\n", kSceDmaSend);
        return;
    }
    std::printf("[dma] logging %llu GIF sends from guest frame %llu\n", (unsigned long long)g_count,
                (unsigned long long)g_skip);
}
} // namespace kh2dma
