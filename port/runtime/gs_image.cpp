// sceGsSetDefLoadImage / sceGsExecLoadImage with the SDK's packet layout.
//
// The runtime's versions store a private 12-byte descriptor in the caller's
// sceGsLoadImage and treat the dbp argument as a 2 KB-unit address
// (vram_addr * 2048 / 256), so every KH2 texture upload landed 8x too far into
// GS memory (past the end of it, in practice) and the title logo was drawn
// from empty VRAM. In libgraph, dbp is already BITBLTBUF.DBP (64-word units).
//
// These replacements write the real 96-byte packet that libgraph builds:
//   qw0  GIFtag, PACKED, NLOOP=4, NREG=1, REGS=A+D
//   qw1  BITBLTBUF      qw2  TRXPOS      qw3  TRXREG      qw4  TRXDIR (0)
//   qw5  GIFtag, IMAGE, EOP, NLOOP = image size in quadwords
// and ExecLoadImage sends that header followed by the image as a PATH3 packet,
// so anything the game patches in the struct (it is plain guest memory) is
// honoured.

#include "gs_image.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "ps2_runtime.h"

namespace kh2gs
{
namespace
{
constexpr uint32_t kSetDefLoadImage = 0x002F28B8u;
constexpr uint32_t kExecLoadImage = 0x002F2BE0u;
constexpr uint32_t kHeaderBytes = 96u;

uint32_t transferBits(uint32_t psm)
{
    switch (psm)
    {
    case 0x00: return 32; // PSMCT32
    case 0x01: return 24; // PSMCT24
    case 0x02: case 0x0A: return 16; // PSMCT16, PSMCT16S
    case 0x13: return 8;  // PSMT8
    case 0x14: return 4;  // PSMT4
    case 0x1B: return 8;  // PSMT8H
    case 0x24: case 0x2C: return 4; // PSMT4HL, PSMT4HH
    case 0x30: return 32; // PSMZ32
    case 0x31: return 24; // PSMZ24
    case 0x32: case 0x3A: return 16; // PSMZ16, PSMZ16S
    default: return 32;
    }
}

int16_t s16(R5900Context *ctx, int reg)
{
    return static_cast<int16_t>(getRegU32(ctx, reg) & 0xFFFFu);
}

void put(uint8_t *q, uint64_t lo, uint64_t hi)
{
    std::memcpy(q, &lo, 8);
    std::memcpy(q + 8, &hi, 8);
}

// sceGsSetDefLoadImage(lp, dbp, dbw, dpsm, x, y, w, h); x..h arrive in t0..t3.
void setDefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    uint8_t *lp = rdram + (getRegU32(ctx, 4) & PS2_RAM_MASK);
    const uint64_t dbp = static_cast<uint16_t>(s16(ctx, 5)) & 0x3FFFu;
    const uint64_t dbw = static_cast<uint16_t>(s16(ctx, 6)) & 0x3Fu;
    const uint64_t dpsm = static_cast<uint16_t>(s16(ctx, 7)) & 0x3Fu;
    const uint64_t x = static_cast<uint16_t>(s16(ctx, 8)) & 0x7FFu;
    const uint64_t y = static_cast<uint16_t>(s16(ctx, 9)) & 0x7FFu;
    const uint64_t w = static_cast<uint16_t>(s16(ctx, 10)) & 0xFFFu;
    const uint64_t h = static_cast<uint16_t>(s16(ctx, 11)) & 0xFFFu;
    const uint64_t qwc = (w * h * transferBits(static_cast<uint32_t>(dpsm)) / 8u + 15u) / 16u;

    put(lp + 0x00, 4u | (1ull << 60), 0xEull);                       // PACKED, NREG=1, A+D
    put(lp + 0x10, (dbp << 32) | (dbw << 48) | (dpsm << 56), 0x50); // BITBLTBUF
    put(lp + 0x20, (x << 32) | (y << 48), 0x51);                    // TRXPOS
    put(lp + 0x30, w | (h << 32), 0x52);                            // TRXREG
    put(lp + 0x40, 0, 0x53);                                        // TRXDIR host->local
    put(lp + 0x50, (qwc & 0x7FFFu) | (1ull << 15) | (2ull << 58), 0); // IMAGE, EOP
    setReturnS32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}

// sceGsExecLoadImage(lp, srcaddr)
void execLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint8_t *lp = rdram + (getRegU32(ctx, 4) & PS2_RAM_MASK);
    const uint32_t src = getRegU32(ctx, 5) & PS2_RAM_MASK;
    uint64_t imageTag;
    std::memcpy(&imageTag, lp + 0x50, 8);
    const uint32_t qwc = static_cast<uint32_t>(imageTag & 0x7FFFu);
    const uint32_t bytes = qwc * 16u;
    if (!runtime->syncCoreSubsystems() || src + bytes > PS2_RAM_SIZE)
    {
        std::fprintf(stderr, "[kh2-gs] LoadImage rejected: %u qw from 0x%08X\n", qwc, src);
        setReturnS32(ctx, -1);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    static std::vector<uint8_t> packet;
    packet.resize(kHeaderBytes + bytes);
    std::memcpy(packet.data(), lp, kHeaderBytes);
    std::memcpy(packet.data() + kHeaderBytes, rdram + src, bytes);
    // Queued behind the DMA worker's VIF1/VU1 work (patch 0038) instead of
    // waiting for it: the order is the same, and the EE no longer stalls on a
    // busy VU1 (a quarter of its time in crowded scenes on Android).
    runtime->memory().queueGifPacketPath3(packet.data(), static_cast<uint32_t>(packet.size()));

    static int logged = 0;
    if (logged < 400)
    {
        ++logged;
        uint64_t bitblt;
        std::memcpy(&bitblt, lp + 0x10, 8);
        uint64_t trxreg;
        std::memcpy(&trxreg, lp + 0x30, 8);
        std::printf("[kh2-gs] LoadImage lp=0x%08X dbp=%u dbw=%u dpsm=0x%02x %ux%u %u qw from 0x%08X\n",
                    getRegU32(ctx, 4),
                    static_cast<uint32_t>((bitblt >> 32) & 0x3FFF), static_cast<uint32_t>((bitblt >> 48) & 0x3F),
                    static_cast<uint32_t>((bitblt >> 56) & 0x3F), static_cast<uint32_t>(trxreg & 0xFFF),
                    static_cast<uint32_t>((trxreg >> 32) & 0xFFF), qwc, src);
    }
    setReturnS32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    const bool ok = runtime.replaceFunction(kSetDefLoadImage, &setDefLoadImage) &&
                    runtime.replaceFunction(kExecLoadImage, &execLoadImage);
    if (!ok)
        std::fprintf(stderr, "[kh2-gs] could not bind sceGsSetDefLoadImage/sceGsExecLoadImage\n");
    return ok;
}
} // namespace kh2gs
