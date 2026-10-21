// Replay a captured VU1 program start and decode what it sends to the GS.
//
// Usage: kh2vureplay <capture stem> [max cycles] [--stop-at=PAIR]
//        kh2vureplay --selftest   (single-instruction micro-programs)
//   stem: the path without extension of a capture written with
//         PS2_VU_CAPTURE_DIR (<stem>.code, <stem>.data, <stem>.state)
//
// Runs the runtime's own VU1 interpreter from the captured PC with the
// captured registers and VU memory, and prints every XGKICK packet: each
// GIFtag, then the registers it writes (XYZ as screen coordinates with the
// ADC "no drawing kick" bit, colours, texture coordinates). Comparing that
// with what the microcode is meant to produce (tools/vudis.py) shows whether
// a rendering problem starts in VU1 or later in the GS.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

// The runtime links against the generated function table; this tool has none.
const uint32_t g_ps2RecompiledFunctionTableBase = 0;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {nullptr};

namespace
{
std::vector<uint8_t> readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const char *regName(unsigned r)
{
    static const char *names[16] = {"PRIM", "RGBAQ", "ST", "UV", "XYZF2", "XYZ2", "TEX0_1", "TEX0_2",
                                    "CLAMP_1", "CLAMP_2", "FOG", "res", "XYZF3", "XYZ3", "A+D", "NOP"};
    return names[r & 15];
}

unsigned g_packets = 0;
bool g_summary = false;
unsigned g_vertices = 0, g_kicks = 0, g_zeroKicks = 0, g_offscreenKicks = 0;

void decode(const uint8_t *data, uint32_t size)
{
    std::printf("== XGKICK packet %u, %u bytes\n", g_packets++, size);
    uint32_t off = 0;
    while (off + 16 <= size)
    {
        uint64_t lo, hi;
        std::memcpy(&lo, data + off, 8);
        std::memcpy(&hi, data + off + 8, 8);
        off += 16;
        const unsigned nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, pre = (lo >> 46) & 1;
        const unsigned prim = (lo >> 47) & 0x7FF, flg = (lo >> 58) & 3;
        unsigned nreg = (lo >> 60) & 0xF;
        if (!nreg)
            nreg = 16;
        std::printf("  GIFtag nloop=%u eop=%u pre=%u prim=%03x flg=%u nreg=%u regs=%016llx\n", nloop, eop, pre, prim,
                    flg, nreg, (unsigned long long)hi);
        if (flg != 0)
        {
            off += (flg == 1 ? ((nloop * nreg + 1) & ~1u) * 8 : nloop * 16);
            continue;
        }
        for (unsigned l = 0; l < nloop && off + 16 <= size; ++l)
        {
            if (!g_summary)
                std::printf("   %3u:", l);
            for (unsigned r = 0; r < nreg && off + 16 <= size; ++r, off += 16)
            {
                const unsigned reg = (hi >> (4 * r)) & 0xF;
                uint32_t w[4];
                std::memcpy(w, data + off, 16);
                switch (reg)
                {
                case 4: // XYZF2
                case 5: // XYZ2
                {
                    const double x = (w[0] & 0xFFFF) / 16.0, y = (w[1] & 0xFFFF) / 16.0;
                    const bool adc = ((w[3] >> 15) & 1) != 0;
                    ++g_vertices;
                    if (!adc)
                    {
                        ++g_kicks;
                        g_zeroKicks += (x == 0 && y == 0);
                        g_offscreenKicks += (x < 1024 || x > 3072 || y < 1024 || y > 3072);
                    }
                    if (g_summary)
                        break;
                }
                    std::printf(" %s(%.2f,%.2f z=%u%s)", regName(reg), (w[0] & 0xFFFF) / 16.0, (w[1] & 0xFFFF) / 16.0,
                                reg == 4 ? (w[2] >> 4) : w[2], ((w[3] >> 15) & 1) ? " ADC" : "");
                    break;
                case 1:
                    if (!g_summary)
                    std::printf(" RGBA(%u,%u,%u,%u)", w[0] & 0xFF, w[1] & 0xFF, w[2] & 0xFF, w[3] & 0xFF);
                    break;
                case 2:
                {
                    float f[3];
                    std::memcpy(f, w, 12);
                    if (!g_summary)
                    std::printf(" ST(%.4g,%.4g q=%.4g)", f[0], f[1], f[2]);
                    break;
                }
                default:
                    if (!g_summary)
                        std::printf(" %s(%08x %08x %08x %08x)", regName(reg), w[0], w[1], w[2], w[3]);
                }
            }
            if (!g_summary)
                std::printf("\n");
        }
        if (eop)
            break;
    }
}
} // namespace

// --- micro-tests: tiny hand-assembled programs for single instructions -------
namespace asmvu
{
constexpr uint32_t kUpperNop = 0x000002FFu;
constexpr uint32_t kLowerNop = 0x8000033Cu;
uint32_t dest(const char *d)
{
    uint32_t v = 0;
    for (; *d; ++d)
        v |= *d == 'x' ? 8 : *d == 'y' ? 4 : *d == 'z' ? 2 : *d == 'w' ? 1 : 0;
    return v << 21;
}
// upper op with fd = fs op ft (bc variants: op = base + bc)
uint32_t upper(uint32_t op, const char *d, unsigned fd, unsigned fs, unsigned ft)
{
    return dest(d) | ft << 16 | fs << 11 | fd << 6 | op;
}
// upper "special" (ITOF/FTOI/...): index = (bits10-6 << 2) | bits1-0, op bits 5-0 = 0x3C | (index & 3)
uint32_t upperSpecial(uint32_t index, const char *d, unsigned ft, unsigned fs)
{
    return dest(d) | ft << 16 | fs << 11 | ((index >> 2) & 31) << 6 | 0x3C | (index & 3);
}
uint32_t lowerSpecial(uint32_t index, const char *d, unsigned it, unsigned is)
{
    return 0x40u << 25 | dest(d) | it << 16 | is << 11 | ((index >> 2) & 31) << 6 | 0x3C | (index & 3);
}
uint32_t sq(const char *d, unsigned fs, unsigned it, int imm)
{
    return 0x01u << 25 | dest(d) | it << 16 | fs << 11 | (static_cast<uint32_t>(imm) & 0x7FF);
}
uint32_t lq(const char *d, unsigned ft, unsigned is, int imm)
{
    return 0x00u << 25 | dest(d) | ft << 16 | is << 11 | (static_cast<uint32_t>(imm) & 0x7FF);
}
} // namespace asmvu

struct MicroTest
{
    const char *name;
    std::vector<std::pair<uint32_t, uint32_t>> pairs; // upper, lower
    float expect[4];                                    // data qword 0 after the run
};

int runMicroTests()
{
    using namespace asmvu;
    const uint32_t MAXbcW = 0x13, MINIbcZ = 0x16, ADDbcX = 0x00;
    std::vector<MicroTest> tests = {
        {"MAXw.z vf1, vf0, vf0  (vf1.z = max(0, 1))",
         {{upper(MAXbcW, "z", 1, 0, 0), kLowerNop}, {kUpperNop, sq("xyzw", 1, 0, 0)}},
         {0, 0, 1, 0}},
        {"MINIz.w vf1, vf0, vf0  (vf1.w = min(1, 0))",
         {{upper(MINIbcZ, "w", 1, 0, 0), kLowerNop}, {kUpperNop, sq("xyzw", 1, 0, 0)}},
         {0, 0, 0, 0}},
        {"MAXw.z vf1 then LQ.xy vf1 in the next pair (KH2 pairs 84/85)",
         {{upper(MAXbcW, "z", 1, 0, 0), kLowerNop}, {kUpperNop, lq("xy", 1, 0, 1)}, {kUpperNop, sq("xyzw", 1, 0, 0)}},
         {3, 4, 1, 0}},
        {"LQ.xy vf1 then MAXw.z vf1 in the next pair",
         {{kUpperNop, lq("xy", 1, 0, 1)}, {upper(MAXbcW, "z", 1, 0, 0), kLowerNop}, {kUpperNop, sq("xyzw", 1, 0, 0)}},
         {3, 4, 1, 0}},
        {"MAXw.z vf1 | LQ.xy vf1 in one pair (hardware: both lane sets land)",
         {{upper(MAXbcW, "z", 1, 0, 0), lq("xy", 1, 0, 1)}, {kUpperNop, sq("xyzw", 1, 0, 0)}},
         {3, 4, 1, 0}},
        {"ADDx.xyzw vf2, vf0, vf0 (w + x bc: 0+0,0+0,0+0,1+0)",
         {{upper(ADDbcX, "xyzw", 2, 0, 0), kLowerNop}, {kUpperNop, sq("xyzw", 2, 0, 0)}},
         {0, 0, 0, 1}},
        {"ITOF0.xy vf1 of ints (3,4) loaded from data[1]",
         {{kUpperNop, lq("xyzw", 1, 0, 1)}, {kUpperNop, kLowerNop}, {kUpperNop, kLowerNop}, {kUpperNop, kLowerNop},
          {upperSpecial(0x10, "xy", 1, 1), kLowerNop}, {kUpperNop, sq("xyzw", 1, 0, 0)}},
         {3, 4, 0, 0}},
    };
    int failed = 0;
    for (const MicroTest &t : tests)
    {
        std::vector<uint8_t> code(16384, 0), data(16384, 0);
        size_t pc = 0;
        for (const auto &[up, lo] : t.pairs)
        {
            std::memcpy(&code[pc * 8], &lo, 4);
            std::memcpy(&code[pc * 8 + 4], &up, 4);
            ++pc;
        }
        // four NOP pairs to drain pipelines, then E-bit and its delay slot
        for (int i = 0; i < 4; ++i, ++pc)
        {
            std::memcpy(&code[pc * 8], &kLowerNop, 4);
            std::memcpy(&code[pc * 8 + 4], &kUpperNop, 4);
        }
        const uint32_t ebit = kUpperNop | (1u << 30);
        std::memcpy(&code[pc * 8], &kLowerNop, 4);
        std::memcpy(&code[pc * 8 + 4], &ebit, 4);
        ++pc;
        std::memcpy(&code[pc * 8], &kLowerNop, 4);
        std::memcpy(&code[pc * 8 + 4], &kUpperNop, 4);
        // data[1] = ints (3, 4) in x, y; as floats for LQ tests too
        const int32_t ints[4] = {3, 4, 0, 0};
        const float floats[4] = {3, 4, 0, 0};
        const bool wantsInts = std::strstr(t.name, "ITOF") != nullptr;
        std::memcpy(&data[16], wantsInts ? static_cast<const void *>(ints) : static_cast<const void *>(floats), 16);
        PS2Memory memory;
        memory.initialize();
        GS gs;
        VU1Interpreter vu(VU1Interpreter::Unit::VU1);
        vu.setCompiledExecutionEnabled(false);
        vu.reset();
        vu.execute(code.data(), 16384, data.data(), 16384, gs, &memory, 0, 0, 0, 100000);
        float got[4];
        std::memcpy(got, data.data(), 16);
        const bool ok = std::memcmp(got, t.expect, 16) == 0;
        failed += !ok;
        std::printf("%s %s: got (%g, %g, %g, %g) expected (%g, %g, %g, %g)\n", ok ? "PASS" : "FAIL", t.name, got[0],
                    got[1], got[2], got[3], t.expect[0], t.expect[1], t.expect[2], t.expect[3]);
    }
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "--selftest") == 0)
        return runMicroTests();
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: kh2vureplay <capture stem> [max cycles]\n");
        return 2;
    }
    const std::string stem = argv[1];
    std::vector<uint8_t> code = readFile(stem + ".code"), data = readFile(stem + ".data"),
                         state = readFile(stem + ".state");
    if (code.empty() || data.empty() || state.size() != sizeof(VU1State))
    {
        std::fprintf(stderr, "cannot read %s.{code,data,state} (state %zu bytes, expected %zu)\n", stem.c_str(),
                     state.size(), sizeof(VU1State));
        return 2;
    }
    PS2Memory memory;
    if (!memory.initialize())
        return 4;
    memory.setGifPacketCallback(&decode);
    GS gs;
    VU1Interpreter vu(VU1Interpreter::Unit::VU1);
    vu.setCompiledExecutionEnabled(false);
    std::memcpy(&vu.state(), state.data(), sizeof(VU1State));
    const VU1State &st = vu.state();
    std::printf("replay %s: pc=%u (pair %u) top=%u itop=%u\n", stem.c_str(), st.pc, st.pc / 8, st.top, st.itop);
    uint32_t maxCycles = 1u << 22;
    int stopAt = -1;
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--summary") == 0)
            g_summary = true;
        else if (std::strncmp(argv[i], "--stop-at=", 10) == 0)
            stopAt = std::atoi(argv[i] + 10);
        else
            maxCycles = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 0));
    }
    if (stopAt >= 0)
    {
        // E bit on that pair: the program ends after the following pair, and the
        // registers are printed below.
        uint32_t up;
        std::memcpy(&up, &code[stopAt * 8 + 4], 4);
        up |= 1u << 30;
        std::memcpy(&code[stopAt * 8 + 4], &up, 4);
    }
    vu.resume(code.data(), static_cast<uint32_t>(code.size()), data.data(), static_cast<uint32_t>(data.size()), gs,
              &memory, st.top, st.itop, maxCycles);
    std::printf("stopped at pc=%u (pair %u) after %llu cycles, %u packets\n", vu.state().pc, vu.state().pc / 8,
                (unsigned long long)vu.state().cycles, g_packets);
    std::printf("vertices %u, drawing kicks %u, kicks at (0,0) %u, kicks off-screen %u\n", g_vertices, g_kicks,
                g_zeroKicks, g_offscreenKicks);
    if (g_summary)
        return 0;
    const VU1State &end = vu.state();
    std::printf("Q=%g P=%g I=%g mac=%04x status=%04x clip=%06x\n", end.q, end.p, end.i, end.mac, end.status, end.clip);
    for (int r = 0; r < 32; ++r)
        std::printf("vf%-2d (%g, %g, %g, %g)%s", r, end.vf[r][0], end.vf[r][1], end.vf[r][2], end.vf[r][3],
                    r % 2 ? "\n" : "    ");
    for (int r = 0; r < 16; ++r)
        std::printf("vi%-2d %d%s", r, end.vi[r], r % 8 == 7 ? "\n" : "  ");
    return 0;
}
