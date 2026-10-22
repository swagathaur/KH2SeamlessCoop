#include "hd_textures.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "device.hpp"
#include "gs_interface.hpp"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

// raylib (linked into the runner): raw-deflate decompression.
extern "C" unsigned char *DecompressData(const unsigned char *compData, int compDataSize, int *dataSize);
extern "C" void MemFree(void *ptr);

namespace kh2hd
{
namespace
{
constexpr size_t kHashBytes = 256;

// CRC-32 (zlib's) in the high half, Adler-32 in the low half: what
// tools/egs_hd_extract.py writes (both are fast in Python's zlib). Both can
// continue over more data, so the nested prefixes a match is confirmed with
// (1 KiB ... 256 KiB) are hashed in one pass (PrefixHash).
const std::array<std::array<uint32_t, 256>, 8> &crcTables()
{
    static const auto tables = [] {
        std::array<std::array<uint32_t, 256>, 8> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int k = 1; k < 8; ++k)
                t[size_t(k)][i] = (t[size_t(k) - 1][i] >> 8) ^ t[0][t[size_t(k) - 1][i] & 0xFF];
        return t;
    }();
    return tables;
}
// Slicing-by-8; c is the running (inverted) state.
uint32_t crc32Update(uint32_t c, const uint8_t *p, size_t n)
{
    const auto &t = crcTables();
    while (n >= 8)
    {
        uint32_t lo, hi;
        std::memcpy(&lo, p, 4);
        std::memcpy(&hi, p + 4, 4);
        lo ^= c;
        c = t[7][lo & 0xFF] ^ t[6][(lo >> 8) & 0xFF] ^ t[5][(lo >> 16) & 0xFF] ^ t[4][lo >> 24] ^ t[3][hi & 0xFF] ^
            t[2][(hi >> 8) & 0xFF] ^ t[1][(hi >> 16) & 0xFF] ^ t[0][hi >> 24];
        p += 8;
        n -= 8;
    }
    while (n--)
        c = t[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}
void adler32Update(uint32_t &a, uint32_t &b, const uint8_t *p, size_t n)
{
    while (n)
    {
        const size_t chunk = std::min<size_t>(n, 5552);
        for (size_t i = 0; i < chunk; ++i)
        {
            a += p[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        p += chunk;
        n -= chunk;
    }
}
uint64_t key64(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    adler32Update(a, b, p, n);
    return uint64_t(~crc32Update(0xFFFFFFFFu, p, n)) << 32 | (b << 16 | a);
}

// key64 of growing prefixes of the same bytes, each byte hashed once.
struct PrefixHash
{
    const uint8_t *p;
    size_t done = 0;
    uint32_t crc = 0xFFFFFFFFu, a = 1, b = 0;
    uint64_t upTo(size_t len)
    {
        if (len < done)
            return key64(p, len); // shorter than before: rare
        crc = crc32Update(crc, p + done, len - done);
        adler32Update(a, b, p + done, len - done);
        done = len;
        return uint64_t(~crc) << 32 | (b << 16 | a);
    }
};

// GS formats: bits per pixel of storage, and which bits of a 32-bit word a
// format owns (the "H" formats share words with each other).
uint32_t storageBits(uint32_t psm)
{
    switch (psm)
    {
    case 0x02: case 0x0A: case 0x32: case 0x3A: return 16;
    case 0x13: return 8;
    case 0x14: return 4;
    default: return 32; // CT32/24, Z32/24, T8H, T4HL, T4HH
    }
}
uint32_t wordMask(uint32_t psm)
{
    switch (psm)
    {
    case 0x1B: return 0xFF000000u; // T8H
    case 0x24: return 0x0F000000u; // T4HL
    case 0x2C: return 0xF0000000u; // T4HH
    case 0x01: case 0x31: return 0x00FFFFFFu; // CT24, Z24
    default: return 0xFFFFFFFFu;
    }
}

// DDS as the PC release ships them: 32-bit RGBA with channel masks, or DXT5.
uint32_t rd32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

void decodeDxt5Block(const uint8_t *b, uint8_t out[16][4])
{
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1])
        for (int i = 1; i < 7; ++i)
            a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    else
    {
        for (int i = 1; i < 5; ++i)
            a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t abits = 0;
    for (int i = 0; i < 6; ++i)
        abits |= uint64_t(b[2 + i]) << (8 * i);
    const uint16_t c0 = uint16_t(b[8] | b[9] << 8), c1 = uint16_t(b[10] | b[11] << 8);
    uint8_t c[4][3];
    auto expand = [](uint16_t v, uint8_t *o) {
        o[0] = uint8_t((v >> 11 & 31) * 255 / 31);
        o[1] = uint8_t((v >> 5 & 63) * 255 / 63);
        o[2] = uint8_t((v & 31) * 255 / 31);
    };
    expand(c0, c[0]);
    expand(c1, c[1]);
    for (int k = 0; k < 3; ++k)
    {
        c[2][k] = uint8_t((2 * c[0][k] + c[1][k]) / 3);
        c[3][k] = uint8_t((c[0][k] + 2 * c[1][k]) / 3);
    }
    const uint32_t cbits = rd32(b + 12);
    for (int i = 0; i < 16; ++i)
    {
        const uint8_t *col = c[cbits >> (2 * i) & 3];
        out[i][0] = col[0];
        out[i][1] = col[1];
        out[i][2] = col[2];
        out[i][3] = a[abits >> (3 * i) & 7];
    }
}

bool loadDds(const std::vector<uint8_t> &d, int &w, int &h, std::vector<uint8_t> &rgba)
{
    if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0)
        return false;
    h = int(rd32(&d[12]));
    w = int(rd32(&d[16]));
    const uint32_t pfFlags = rd32(&d[80]), fourcc = rd32(&d[84]), bits = rd32(&d[88]);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
        return false;
    rgba.assign(size_t(w) * size_t(h) * 4, 0);
    const uint8_t *src = d.data() + 128;
    const size_t avail = d.size() - 128;
    if ((pfFlags & 0x4) && fourcc == 0x35545844u) // "DXT5"
    {
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (avail < size_t(bw) * bh * 16)
            return false;
        uint8_t block[16][4];
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                decodeDxt5Block(src + (size_t(by) * bw + bx) * 16, block);
                for (int i = 0; i < 16; ++i)
                {
                    const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < w && y < h)
                        std::memcpy(&rgba[(size_t(y) * w + x) * 4], block[i], 4);
                }
            }
        return true;
    }
    if ((pfFlags & 0x40) && bits == 32) // RGB (+ alpha) with masks
    {
        if (avail < size_t(w) * h * 4)
            return false;
        const uint32_t masks[4] = {rd32(&d[92]), rd32(&d[96]), rd32(&d[100]), (pfFlags & 1) ? rd32(&d[104]) : 0};
        int shift[4];
        for (int k = 0; k < 4; ++k)
        {
            shift[k] = 0;
            if (masks[k])
                while (!((masks[k] >> shift[k]) & 1))
                    ++shift[k];
        }
        for (size_t i = 0, n = size_t(w) * h; i < n; ++i)
        {
            const uint32_t v = rd32(src + i * 4);
            for (int k = 0; k < 4; ++k)
                rgba[i * 4 + k] = masks[k] ? uint8_t((v & masks[k]) >> shift[k]) : 255;
        }
        return true;
    }
    return false;
}

// The PC release's .pkg format (tools/egs_extract.py; OpenKH's OpenKh.Egs): a
// 16-byte header that is also the key seed, the first 0x100 bytes of each
// piece encrypted, the data zlib-compressed.
const uint8_t kMaster[256] = {0x7E, 0x88, 0x97, 0x55, 0x0B, 0x06, 0xF1, 0x08, 0xEB, 0xBB, 0x14, 0x1C, 0xD8, 0x7A, 0xEC, 0x41, 0x34, 0xB2, 0xA3, 0x46, 0xEF, 0x6B, 0xFE, 0xE1, 0xCF, 0x53, 0xA5, 0x05, 0x12, 0xD2, 0x8E, 0x52, 0x4A, 0x80, 0xE9, 0x81, 0xB0, 0xF0, 0xB4, 0x9C, 0xFF, 0x0F, 0x15, 0x13, 0xDA, 0x73, 0x4E, 0x77, 0xBE, 0xD7, 0x30, 0xE5, 0xF6, 0x5A, 0x11, 0x37, 0x67, 0xBC, 0x83, 0x6F, 0x27, 0x76, 0xD0, 0xCD, 0x69, 0x0D, 0x2E, 0x51, 0x42, 0x90, 0xB8, 0xB6, 0x4C, 0xAD, 0xCE, 0x5B, 0x1A, 0x1F, 0xF5, 0xAF, 0x01, 0xF8, 0x5E, 0x3A, 0x6E, 0x68, 0x8B, 0xE8, 0x9F, 0xC9, 0xD9, 0x26, 0x92, 0x29, 0xC8, 0x33, 0x98, 0x32, 0x54, 0xD4, 0x44, 0x25, 0x66, 0xAC, 0x5F, 0x99, 0x21, 0xE4, 0x8F, 0x1D, 0xC2, 0xD5, 0xA4, 0x62, 0xF9, 0x02, 0x61, 0xDE, 0x59, 0xE7, 0x07, 0x9A, 0xFA, 0x2F, 0x95, 0x3F, 0x86, 0xD3, 0x78, 0xA7, 0x75, 0xED, 0xD6, 0x2D, 0x64, 0x87, 0xBD, 0xC7, 0xC1, 0xAA, 0xF2, 0x8C, 0x17, 0xCB, 0x31, 0x8A, 0xC3, 0xCC, 0x04, 0xEE, 0x6A, 0xAB, 0x5C, 0x22, 0x70, 0xCA, 0x9E, 0x71, 0x6D, 0x85, 0x45, 0x5D, 0xB9, 0xA9, 0xA6, 0x10, 0x47, 0xFB, 0x82, 0x7D, 0x84, 0x7B, 0xC6, 0xE2, 0x38, 0xFC, 0x2B, 0x0E, 0x20, 0x9D, 0xC5, 0xF3, 0x39, 0xA8, 0xA0, 0x65, 0x58, 0x43, 0x7C, 0xE3, 0x36, 0x18, 0x72, 0x49, 0x79, 0xAE, 0xD1, 0x74, 0x40, 0xC4, 0x91, 0x4F, 0x24, 0x63, 0xBF, 0xBA, 0x23, 0x96, 0x50, 0xB3, 0x57, 0xDF, 0x1E, 0x03, 0x48, 0x7F, 0x35, 0x4D, 0x3E, 0xE6, 0xA1, 0xDD, 0x09, 0x3C, 0x3D, 0x3B, 0x56, 0x8D, 0x93, 0x2A, 0x9B, 0x4B, 0x0C, 0x28, 0xB1, 0xE0, 0x60, 0x89, 0x19, 0xDB, 0x2C, 0xF7, 0x6C, 0xB5, 0x1B, 0x94, 0xC0, 0xDC, 0xEA, 0xB7, 0x0A, 0xF4, 0x16, 0xFD, 0xA2, 0x00};
const uint8_t kScramble[40] = {1, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 8, 0, 0, 0, 0x10, 0, 0, 0,
                               0x20, 0, 0, 0, 0x40, 0, 0, 0, 0x80, 0, 0, 0, 0x1B, 0, 0, 0, 0x36, 0, 0, 0};
constexpr int kPasses = 10;

std::array<uint8_t, 0xB0> pkgKey(const uint8_t seed[16])
{
    std::array<uint8_t, 0xB0> k{};
    for (int i = 0; i < 16; ++i)
        k[i] = seed[i] ? seed[i] : uint8_t(i);
    for (int i = 0; i < kPasses * 4; ++i)
    {
        uint8_t f[4] = {k[0x0C + i * 4], k[0x0D + i * 4], k[0x0E + i * 4], k[0x0F + i * 4]};
        if (i % 4 == 0)
        {
            const uint8_t g[4] = {uint8_t(kMaster[f[1]] ^ kScramble[i]), uint8_t(kMaster[f[2]] ^ kScramble[i + 1]),
                                  uint8_t(kMaster[f[3]] ^ kScramble[i + 2]), uint8_t(kMaster[f[0]] ^ kScramble[i + 3])};
            std::memcpy(f, g, 4);
        }
        for (int j = 0; j < 4; ++j)
            k[0x10 + i * 4 + j] = uint8_t(k[i * 4 + j] ^ f[j]);
    }
    return k;
}

void pkgDecrypt(std::vector<uint8_t> &data, const std::array<uint8_t, 0xB0> &key)
{
    for (size_t c = 0; c < std::min<size_t>(data.size(), 0x100); c += 0x10)
    {
        const size_t n = std::min<size_t>(data.size() - c, 0x10);
        for (int p = kPasses; p >= 0; --p)
            for (size_t j = 0; j < n; ++j)
                data[c + j] ^= key[0x10 * size_t(p) + j];
    }
}

struct Entry
{
    std::string path;
    std::string source;
    size_t bytes = 0; // hashed for the key...
    size_t skip = 0;  // ...after this many blank lead-in bytes
    std::vector<std::pair<size_t, uint64_t>> checks; // longer prefixes, shortest first
    // A model's or map's texture: its texture environment (palette variants
    // of one texture share the data and differ in CBP/CSA).
    bool variant = false;
    uint32_t vtbp = 0, vcbp = 0, vcsa = 0, vtbw = 0; // vtbw 0: not known
    // The part of the texture the image covers (its environment's CLAMP
    // region; vw 0: all of it).
    uint32_t vx = 0, vy = 0, vw = 0, vh = 0;
    // A frame of a texture animation (eyes, mouths): frame pframe of a strip
    // of pframes frames, each pw x ph texels ("p:" in the manifest).
    int pframe = -1, pframes = 0;
    uint32_t pw = 0, ph = 0;
};

// An animation frame the game sent into a texture with an HD image.
struct Patch
{
    uint32_t x, y, w, h;
    int entry;
};

struct Region
{
    uint32_t dbp, dbw, psm;
    uint32_t x, y, w, h;
    uint32_t firstPage, lastPage;
    uint32_t mask;
    int entry;
    uint64_t bytes;
    std::vector<int> alts; // entries with the same data (palette variants)
    std::vector<Patch> patches; // animation frames sent into it since
};

class Replacer final : public ParallelGS::TextureReplacer
{
public:
    // Leading bytes equal to the first, in whole 16-byte steps (as
    // tools/egs_hd_extract.py's leading_blank).
    static size_t leadingBlank(const uint8_t *bytes, size_t size)
    {
        const size_t end = std::min<size_t>(size, 65536);
        size_t n = 0;
        while (n + 16 <= end)
        {
            bool same = true;
            for (size_t i = 0; i < 16 && same; ++i)
                same = bytes[n + i] == bytes[0];
            if (!same)
                break;
            n += 16;
        }
        return n;
    }

    // The manifest entry these bytes start (size: bytes available),
    // confirmed as far as possible; -1 if none.
    int bestMatch(const uint8_t *bytes, size_t size, size_t &matchLen, std::vector<int> *alts = nullptr) const
    {
        matchLen = 0;
        if (alts)
            alts->clear();
        const size_t skip = leadingBlank(bytes, size);
        if (skip + kHashBytes > size)
            return -1;
        const size_t n = kHashBytes;
        const auto range = m_byKey.equal_range(key64(bytes + skip, n));
        int match = -1;
        // A key shared by many textures is a pattern, not a texture.
        if (std::distance(range.first, range.second) > 32)
            return -1;
        // The candidates' checks are the same few prefix lengths: hash each
        // once, in one pass.
        PrefixHash prefix{bytes};
        std::array<std::pair<size_t, uint64_t>, 8> known{};
        size_t knownCount = 0;
        auto prefixKey = [&](size_t len) {
            for (size_t i = 0; i < knownCount; ++i)
                if (known[i].first == len)
                    return known[i].second;
            const uint64_t k = prefix.upTo(len);
            if (knownCount < known.size())
                known[knownCount++] = {len, k};
            return k;
        };
        for (auto it = range.first; it != range.second; ++it)
        {
            const Entry &e = m_entries[size_t(it->second)];
            if (e.bytes != n || e.skip != skip)
                continue;
            size_t agreed = skip + n;
            for (const auto &[len, hash] : e.checks)
            {
                if (len > size)
                    break;
                // A later difference (the game edits some sheets in memory, e.g.
                // the HUD's) leaves what agreed so far; a texture still has to
                // agree over 4 KiB of real content to count.
                if (prefixKey(len) != hash)
                    break;
                agreed = std::max(agreed, len);
            }
            // Sure: agreed over 4 KiB (or everything available), or over all
            // the file had after the texture's start.
            const bool sure = agreed >= std::min<size_t>(size, 4096) ||
                              e.checks.empty() || e.checks.back().first <= agreed;
            if (sure && agreed > matchLen)
            {
                match = it->second;
                matchLen = agreed;
                if (alts)
                    alts->clear();
            }
            else if (sure && agreed == matchLen && alts)
                alts->push_back(it->second);
        }
        return match;
    }

    void on_upload(const ParallelGS::BITBLTBUFBits &b, const ParallelGS::TRXPOSBits &pos,
                   const ParallelGS::TRXREGBits &reg, const void *data, size_t size) override
    {
        const uint32_t psm = b.DPSM, dbp = b.DBP, dbw = b.DBW;
        const uint32_t w = reg.RRW, h = reg.RRH;
        if (!w || !h || !size)
            return;
        // Pages (8 KiB, 32 blocks) the upload touches, roughly: rows before
        // its rectangle plus its own bytes.
        const uint64_t rowBytes = uint64_t(std::max(dbw, 1u)) * 64u * storageBits(psm) / 8u;
        const uint64_t first = dbp / 32u;
        const uint64_t span = uint64_t(pos.DSAY) * rowBytes + uint64_t(h) * rowBytes;
        const uint64_t last = first + std::max<uint64_t>(1, (span + 8191u) / 8192u) - 1u;
        const uint32_t mask = wordMask(psm);
        // Diagnostics: KH2_HD_WATCH=<tbp> logs every upload touching that
        // page and every texture lookup there (with its answer).
        static const char *watchEnv = std::getenv("KH2_HD_WATCH");
        static const uint32_t watch = watchEnv ? uint32_t(std::atoi(watchEnv)) : 0xFFFFFFFFu;
        if (watch != 0xFFFFFFFFu && first <= watch / 32u && watch / 32u <= last && m_watchLines < 4000)
        {
            ++m_watchLines;
            std::printf("[hd] watch #%ld upload dbp %u dbw %u psm 0x%02X %ux%u at %u,%u size %zu\n", m_finds, dbp, dbw,
                        psm, w, h, pos.DSAX, pos.DSAY, size);
        }
        const auto *bytes = static_cast<const uint8_t *>(data);
        // Palettes (small 32-bit uploads), for recolouring fonts.
        if (size <= 1024 && psm == 0x00 && pos.DSAX == 0 && pos.DSAY == 0)
            m_cluts[dbp] = {w, h, std::vector<uint8_t>(bytes, bytes + size)};
        size_t matchLen = 0;
        std::vector<int> alts;
        int match = bestMatch(bytes, size, matchLen, &alts);
        // The agreement has to include real content: blank stretches (cleared
        // buffers, transparent corners) look alike everywhere.
        if (match >= 0)
        {
            const size_t n2 = std::min<size_t>(size, matchLen);
            size_t differing = 0;
            for (size_t i = 1; i < n2; ++i)
                differing += bytes[i] != bytes[0];
            if (differing * 64 < n2)
                match = -1;
        }
        const bool frame = match >= 0 && m_entries[size_t(match)].pframes > 0;
        for (size_t i = 0; i < m_regions.size();)
        {
            Region &r = m_regions[i];
            // Small uploads (palettes) tucked into a texture's unused blocks
            // do not end it; anything bigger, or at its base, does.
            const bool tucked = size < 4096 && dbp != r.dbp;
            // A patch into the texture itself (KH2 animates faces by sending
            // a frame of the eyes or mouth into the character's texture, at
            // its base pointer with an offset): the HD image stays, with the
            // frame's HD image over that part when the PC has one.
            const bool patch = dbp == r.dbp && (pos.DSAX || pos.DSAY) && size * 4 <= r.bytes;
            if (patch)
            {
                auto overlaps = [&](const Patch &p) {
                    return p.x < pos.DSAX + w && pos.DSAX < p.x + p.w && p.y < pos.DSAY + h && pos.DSAY < p.y + p.h;
                };
                r.patches.erase(std::remove_if(r.patches.begin(), r.patches.end(), overlaps), r.patches.end());
                if (frame)
                    r.patches.push_back({pos.DSAX, pos.DSAY, w, h, match});
                ++i;
                continue;
            }
            if (!tucked && r.firstPage <= last && first <= r.lastPage && (r.mask & mask))
            {
                if (m_log && m_loggedDrops.insert(uint64_t(r.entry) << 32 | dbp).second)
                    std::printf("[hd] %s dropped: upload dbp %u psm 0x%02X %ux%u over it\n",
                                m_entries[size_t(r.entry)].path.c_str(), dbp, psm, w, h);
                m_regions.erase(m_regions.begin() + ptrdiff_t(i));
            }
            else
                ++i;
        }
        static const bool scan = std::getenv("KH2_HD_SCAN") != nullptr; // diagnostics: textures inside uploads
        if (scan && size >= 1024 && m_scanned.insert(key64(bytes, std::min<size_t>(size, 4096)) ^ size).second)
        {
            int found = 0;
            for (size_t off = 16; off + kHashBytes <= size && found < 64; off += 16)
            {
                size_t agreed = 0;
                const int m = bestMatch(bytes + off, size - off, agreed);
                size_t differing = 0;
                if (m >= 0)
                    for (size_t i = 1; i < agreed; ++i)
                        differing += bytes[off + i] != bytes[off];
                if (m >= 0 && agreed >= 1024 && differing * 16 >= agreed)
                {
                    ++found;
                    std::printf("[hd] scan: upload dbp %u dbw %u psm 0x%02X %ux%u at %u,%u (%zu bytes): %s at +0x%zx (%zu agree)\n",
                                dbp, dbw, psm, w, h, pos.DSAX, pos.DSAY, size, m_entries[size_t(m)].source.c_str(), off,
                                agreed);
                }
            }
        }
        // Diagnostics: KH2_HD_DUMP=<dbp>[:skip] writes 8 uploads there (after
        // the first <skip>) to hd_dump_<dbp>_<n>.bin, to compare with files.
        static const char *dumpEnv = std::getenv("KH2_HD_DUMP");
        static const long dumpSkip = dumpEnv && std::strchr(dumpEnv, ':') ? std::atol(std::strchr(dumpEnv, ':') + 1) : 0;
        if (dumpEnv && uint32_t(std::atoi(dumpEnv)) == dbp && m_dumpSeen++ >= dumpSkip && m_dumped < 8)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "hd_dump_%u_%d.bin", dbp, m_dumped++);
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fwrite(bytes, 1, size, f);
                std::fclose(f);
            }
        }
        // KH2_HD_TRACE: the first 2000 different uploads (by place and data).
        static const bool traceUp = std::getenv("KH2_HD_TRACE") != nullptr;
        if (traceUp && m_tracedUploads < 2000 &&
            m_tracedKeys.insert(key64(bytes, std::min<size_t>(size, 4096)) ^ (uint64_t(dbp) << 40 | size)).second)
        {
            ++m_tracedUploads;
            std::printf("[hd] trace upload dbp %u dbw %u psm 0x%02X %ux%u at %u,%u size %zu -> %s\n", dbp, dbw, psm, w, h,
                        pos.DSAX, pos.DSAY, size, match >= 0 ? m_entries[size_t(match)].source.c_str() : "-");
        }
        // An animation frame is only ever a part of another texture.
        if (match < 0 || frame)
            return;
        const Entry &e = m_entries[size_t(match)];
        m_regions.push_back({dbp, dbw, psm, pos.DSAX, pos.DSAY, w, h, uint32_t(first), uint32_t(last), mask, match,
                             uint64_t(size), std::move(alts), {}});
        if (m_log && m_loggedUploads.insert(match).second)
            std::printf("[hd] upload %s: dbp %u dbw %u psm 0x%02X %ux%u at %u,%u (%zu bytes agree) -> %s\n",
                        e.source.c_str(), dbp, dbw, psm, w, h, pos.DSAX, pos.DSAY, matchLen, e.path.c_str());
    }

    // The GPU renders into (or copies over) these pages: a texture uploaded
    // there is gone. Games render effects into spare texture memory (KH2's
    // pause blur, battle transitions); a stale HD image there would be drawn
    // in place of what was rendered.
    void on_gpu_write(uint32_t basePage, uint32_t pageWidth, uint32_t pageHeight, uint32_t pageStride,
                      uint32_t writeMask) override
    {
        // (A 24-bit frame buffer leaves the top byte of each word alone: KH2
        // keeps its fonts there, as 4-bit textures.)
        if (m_regions.empty() || !pageWidth || !pageHeight || !writeMask)
            return;
        std::array<uint64_t, 8> written{}; // 512 pages of 8 KiB: all of GS memory
        for (uint32_t yy = 0; yy < pageHeight; ++yy)
            for (uint32_t xx = 0; xx < pageWidth; ++xx)
            {
                const uint32_t page = (basePage + yy * pageStride + xx) & 511u;
                written[page >> 6] |= 1ull << (page & 63u);
            }
        for (size_t i = 0; i < m_regions.size();)
        {
            const Region &r = m_regions[i];
            bool hit = false;
            for (uint32_t page = r.firstPage; (r.mask & writeMask) && page <= r.lastPage && !hit; ++page)
                hit = (written[(page & 511u) >> 6] >> (page & 63u)) & 1u;
            if (hit)
            {
                if (m_log && m_loggedDrops.insert(uint64_t(r.entry) << 32 | 0xFFFFFFFFu).second)
                    std::printf("[hd] %s dropped: the GPU draws over it (page %u)\n",
                                m_entries[size_t(r.entry)].path.c_str(), basePage);
                m_regions.erase(m_regions.begin() + ptrdiff_t(i));
            }
            else
                ++i;
        }
    }

    bool find(const ParallelGS::TEX0Bits &tex0, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
              ParallelGS::TextureReplacement &out) override
    {
        ++m_finds;
        const bool hit = findImpl(tex0, x, y, w, h, out);
        static const char *watchEnv = std::getenv("KH2_HD_WATCH");
        if (watchEnv && uint32_t(std::atoi(watchEnv)) == tex0.TBP0 && m_watchLines < 4000)
        {
            ++m_watchLines;
            std::printf("[hd] watch #%ld find tbw %u psm 0x%02X tw/th %u/%u rect %u,%u %ux%u cbp %u csa %u -> %s\n", m_finds,
                        uint32_t(tex0.TBW), uint32_t(tex0.PSM), uint32_t(tex0.TW), uint32_t(tex0.TH), x, y, w, h,
                        uint32_t(tex0.CBP), uint32_t(tex0.CSA), hit ? "replaced" : "-");
        }
        return hit;
    }

    bool findImpl(const ParallelGS::TEX0Bits &tex0, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  ParallelGS::TextureReplacement &out)
    {
        if (!m_enabled || m_regions.empty())
            return false;
        // Diagnostics: KH2_HD_ONLY=<lo>-<hi> replaces only textures with TBP0
        // in that range (to find which one is wrong).
        static const char *only = std::getenv("KH2_HD_ONLY");
        static uint32_t onlyLo = 0, onlyHi = 0xFFFFFFFFu;
        static const bool onlyParsed = only && std::sscanf(only, "%u-%u", &onlyLo, &onlyHi) == 2;
        if (onlyParsed && (tex0.TBP0 < onlyLo || tex0.TBP0 > onlyHi))
            return false;
        static const bool trace = std::getenv("KH2_HD_TRACE") != nullptr;
        if (trace)
        {
            const uint64_t k = uint64_t(tex0.TBP0) << 40 | uint64_t(tex0.PSM) << 32 | uint64_t(tex0.TW) << 28 |
                               uint64_t(tex0.TH) << 24 | uint64_t(w) << 12 | h;
            if (m_traced.size() < 2000 && m_traced.insert(k).second)
            {
                std::string nearby;
                for (const Region &r : m_regions)
                    if (tex0.TBP0 >= r.dbp && uint64_t(tex0.TBP0 - r.dbp) * 256u < std::max<uint64_t>(r.bytes, 256))
                        nearby += " [" + std::to_string(r.dbp) + " psm " + std::to_string(r.psm) + " " + std::to_string(r.w) +
                                "x" + std::to_string(r.h) + " " + m_entries[size_t(r.entry)].source + "]";
                std::printf("[hd] trace tbp %u tbw %u psm 0x%02X tw/th %u/%u rect %u,%u %ux%u cbp %u csa %u;%s\n",
                            uint32_t(tex0.TBP0), uint32_t(tex0.TBW), uint32_t(tex0.PSM), uint32_t(tex0.TW),
                            uint32_t(tex0.TH), x, y, w, h, uint32_t(tex0.CBP), uint32_t(tex0.CSA), nearby.c_str());
            }
        }
        if (m_log)
        {
            const uint64_t k = uint64_t(tex0.TBP0) << 8 | uint64_t(tex0.PSM);
            if (m_loggedFinds.insert(k).second)
                std::printf("[hd] texture tbp %u psm 0x%02X tw/th %u/%u rect %u,%u %ux%u\n", uint32_t(tex0.TBP0),
                            uint32_t(tex0.PSM), uint32_t(tex0.TW), uint32_t(tex0.TH), x, y, w, h);
        }
        for (auto it = m_regions.rbegin(); it != m_regions.rend(); ++it)
        {
            const Region &r = *it;
            const uint32_t knownTbw = m_entries[size_t(r.entry)].vtbw;
            if (r.dbp != tex0.TBP0 || !directMatch(r, tex0, x, y) || (knownTbw && knownTbw != tex0.TBW))
            {
                // A sprite sheet: KH2 points TBP0 at a block inside it, or
                // at its start with the sheet's width (a part of it).
                int32_t sx = 0, sy = 0;
                uint32_t sw = 0, sh = 0;
                // A model's or map's texture is laid out with its own buffer
                // width; read with another (another object's texture sharing
                // the upload), the picture is not a part of it.
                if ((!knownTbw || knownTbw == tex0.TBW) && insideSheet(r, tex0, sx, sy, sw, sh) &&
                    x + uint32_t(sx) + w <= sw && y + uint32_t(sy) + h <= sh)
                {
                    const int chosen = pickVariant(r, tex0);
                    Vulkan::ImageHandle img = image(chosen);
                    if (!img)
                        return false;
                    if (Vulkan::ImageHandle c = recolouredFont(chosen, tex0))
                        img = c;
                    out.image = img;
                    out.x = -sx;
                    out.y = -sy;
                    out.width = sw;
                    out.height = sh;
                    traceHit(tex0, chosen, "sheet");
                    if (m_log && m_loggedHits.insert(chosen).second)
                        std::printf("[hd] replaced %s (sheet %ux%u, texture at %d,%d)\n",
                                    m_entries[size_t(chosen)].path.c_str(), sw, sh, sx, sy);
                    return true;
                }
                if (m_log && tex0.TBP0 > r.dbp && uint64_t(tex0.TBP0 - r.dbp) * 256u < r.bytes)
                {
                    const uint64_t k = uint64_t(tex0.TBP0) << 32 | uint64_t(tex0.PSM) << 24 | uint64_t(r.dbp);
                    if (m_loggedMisses.insert(k).second)
                        std::printf("[hd] inside but not used: tbp %u psm 0x%02X tbw %u tw/th %u/%u rect %u,%u %ux%u; "
                                    "upload dbp %u dbw %u psm 0x%02X %ux%u (%s)\n",
                                    uint32_t(tex0.TBP0), uint32_t(tex0.PSM), uint32_t(tex0.TBW), uint32_t(tex0.TW),
                                    uint32_t(tex0.TH), x, y, w, h, r.dbp, r.dbw, r.psm, r.w, r.h,
                                    m_entries[size_t(r.entry)].source.c_str());
                }
                continue;
            }
            uint32_t rx = r.x, ry = r.y, rw = r.w, rh = r.h;
            bool ok = false;
            if (r.psm == tex0.PSM)
                // Same format: the upload's rectangle, if the read starts in it.
                ok = x >= rx && y >= ry && x < rx + rw && y < ry + rh;
            else if (r.x == 0 && r.y == 0 && (r.psm == 0x00 || r.psm == 0x01) &&
                     (tex0.PSM == 0x13 || tex0.PSM == 0x14))
            {
                // KH2 uploads paletted textures as 32-bit blocks: the whole
                // texture, if the byte counts agree.
                const uint64_t tw = 1ull << uint32_t(tex0.TW), th = 1ull << uint32_t(tex0.TH);
                const uint64_t texBytes = tw * th * storageBits(tex0.PSM) / 8u;
                const uint64_t upBytes = uint64_t(r.w) * r.h * storageBits(r.psm) / 8u;
                if (texBytes == upBytes)
                {
                    rx = 0;
                    ry = 0;
                    rw = uint32_t(tw);
                    rh = uint32_t(th);
                    ok = x < rw && y < rh;
                }
            }
            if (!ok)
            {
                if (m_log)
                {
                    const uint64_t k = uint64_t(tex0.TBP0) << 32 | uint64_t(tex0.PSM) << 24 | uint64_t(w) << 12 | h;
                    if (m_loggedMisses.insert(k).second)
                        std::printf("[hd] no match: tbp %u psm 0x%02X tw/th %u/%u rect %u,%u %ux%u (upload psm 0x%02X %u,%u %ux%u)\n",
                                    uint32_t(tex0.TBP0), uint32_t(tex0.PSM), uint32_t(tex0.TW), uint32_t(tex0.TH), x, y,
                                    w, h, r.psm, r.x, r.y, r.w, r.h);
                }
                continue;
            }
            const int chosen = pickVariant(r, tex0);
            Vulkan::ImageHandle img = image(chosen);
            if (!img)
                return false;
            if (Vulkan::ImageHandle c = recolouredFont(chosen, tex0))
                img = c;
            const Entry &ce = m_entries[size_t(chosen)];
            if (ce.vw && ce.vh && ce.vx + ce.vw <= rw && ce.vy + ce.vh <= rh)
            {
                // The image is one part of an atlas.
                rx += ce.vx;
                ry += ce.vy;
                rw = ce.vw;
                rh = ce.vh;
            }
            if (!r.patches.empty())
                if (Vulkan::ImageHandle c = composite(chosen, r.patches, rx, ry, rw, rh))
                    img = c;
            out.image = img;
            out.x = rx;
            out.y = ry;
            out.width = rw;
            out.height = rh;
            traceHit(tex0, chosen, "direct");
            if (m_log && m_loggedHits.insert(chosen).second)
                std::printf("[hd] replaced %s (%ux%u texels as psm 0x%02X, %ux%u image)\n",
                            m_entries[size_t(chosen)].path.c_str(), rw, rh, uint32_t(tex0.PSM), img->get_width(),
                            img->get_height());
            return true;
        }
        return false;
    }

    // KH2_HD_TRACE: which image each texture (base, format, width) got.
    void traceHit(const ParallelGS::TEX0Bits &tex0, int chosen, const char *how)
    {
        static const bool trace = std::getenv("KH2_HD_TRACE") != nullptr;
        if (!trace)
            return;
        const uint64_t k = uint64_t(tex0.TBP0) << 40 | uint64_t(tex0.PSM) << 32 | uint64_t(tex0.TBW) << 24 |
                           uint64_t(uint32_t(chosen) & 0xFFFFFF);
        if (m_tracedHits.size() < 4000 && m_tracedHits.insert(k).second)
            std::printf("[hd] trace hit tbp %u tbw %u psm 0x%02X tw/th %u/%u csa %u -> %s %s (%s)\n", uint32_t(tex0.TBP0),
                        uint32_t(tex0.TBW), uint32_t(tex0.PSM), uint32_t(tex0.TW), uint32_t(tex0.TH),
                        uint32_t(tex0.CSA), how, m_entries[size_t(chosen)].source.c_str(),
                        m_entries[size_t(chosen)].path.c_str());
    }

    // The texture is the upload itself (same format), or all of a paletted
    // texture sent as 32-bit blocks.
    static bool directMatch(const Region &r, const ParallelGS::TEX0Bits &tex0, uint32_t x, uint32_t y)
    {
        if (r.psm == tex0.PSM)
            return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
        if (r.x == 0 && r.y == 0 && (r.psm == 0x00 || r.psm == 0x01) && (tex0.PSM == 0x13 || tex0.PSM == 0x14))
        {
            const uint64_t tw = 1ull << uint32_t(tex0.TW), th = 1ull << uint32_t(tex0.TH);
            return tw * th * storageBits(tex0.PSM) / 8u == uint64_t(r.w) * r.h * storageBits(r.psm) / 8u && x < tw &&
                   y < th;
        }
        return false;
    }

    // A paletted texture whose TBP0 lies inside a 32-bit upload (a sheet):
    // where it starts in the sheet, in its own texels, and the sheet's size.
    static bool insideSheet(const Region &r, const ParallelGS::TEX0Bits &tex0, int32_t &x0, int32_t &y0,
                            uint32_t &sheetW, uint32_t &sheetH)
    {
        if (!(r.psm == 0x00 || r.psm == 0x01) || r.x != 0 || r.y != 0 || !(tex0.PSM == 0x13 || tex0.PSM == 0x14))
            return false;
        const uint32_t tbp = tex0.TBP0;
        const uint64_t upBytes = uint64_t(r.w) * r.h * 4u;
        if (tbp < r.dbp || uint64_t(tbp - r.dbp) * 256u >= upBytes || !tex0.TBW)
            return false;
        // GS page geometry (texels) and block order within a page.
        static const uint8_t kBlocks8[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
                                               {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
        static const uint8_t kBlocks4[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
                                               {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
        const bool t8 = tex0.PSM == 0x13;
        const uint32_t pageW = 128, pageH = t8 ? 64 : 128, blockW = t8 ? 16 : 32, blockH = 16;
        sheetW = uint32_t(tex0.TBW) * 64u;
        if (sheetW % pageW)
            return false;
        const uint32_t bpp = t8 ? 8 : 4;
        sheetH = uint32_t(upBytes * 8u / bpp / sheetW);
        const uint32_t d = tbp - r.dbp;
        const uint32_t page = d / 32, block = d % 32;
        const uint32_t pagesPerRow = sheetW / pageW;
        int bx = -1, by = -1;
        const int rows = t8 ? 4 : 8, cols = t8 ? 8 : 4;
        for (int yy = 0; yy < rows && bx < 0; ++yy)
            for (int xx = 0; xx < cols; ++xx)
                if ((t8 ? kBlocks8[yy][xx] : kBlocks4[yy][xx]) == block)
                {
                    bx = xx;
                    by = yy;
                    break;
                }
        if (bx < 0)
            return false;
        x0 = int32_t((page % pagesPerRow) * pageW + uint32_t(bx) * blockW);
        y0 = int32_t((page / pagesPerRow) * pageH + uint32_t(by) * blockH);
        return uint32_t(x0) < sheetW && uint32_t(y0) < sheetH;
    }

    // Of the entries with this data, the one whose texture environment has
    // the palette the game draws with: same CBP and CSA, or the same CBP
    // relative to TBP0 (models are placed where there is room), or the same
    // CSA (KH2's maps keep several textures in one 4-bit atlas, each part
    // with its own CSA, and draw with the CLUT already loaded), or the first.
    int pickVariant(const Region &r, const ParallelGS::TEX0Bits &tex0) const
    {
        if (r.alts.empty())
            return r.entry;
        int relative = -1, sameCsa = -1;
        auto consider = [&](int i) {
            const Entry &e = m_entries[size_t(i)];
            if (!e.variant || e.vcsa != tex0.CSA)
                return false;
            if (e.vcbp == tex0.CBP)
                return true;
            if (relative < 0 && int(e.vcbp) - int(e.vtbp) == int(tex0.CBP) - int(tex0.TBP0))
                relative = i;
            if (sameCsa < 0)
                sameCsa = i;
            return false;
        };
        if (consider(r.entry))
            return r.entry;
        for (int i : r.alts)
            if (consider(i))
                return i;
        return relative >= 0 ? relative : sameCsa >= 0 ? sameCsa : r.entry;
    }

    // "pkg:<archive>:<entry offset>:<data offset>:<stored>:<real>:<type>"
    bool readPkg(const std::string &ref, std::vector<uint8_t> &out)
    {
        unsigned archive = 0;
        long long entryOff = 0, dataOff = 0;
        int stored = 0, real = 0;
        if (std::sscanf(ref.c_str(), "pkg:%u:%lld:%lld:%d:%d", &archive, &entryOff, &dataOff, &stored, &real) != 5 ||
            archive >= m_archives.size() || real <= 0)
            return false;
        std::ifstream in(m_archives[archive], std::ios::binary);
        uint8_t seed[16];
        if (!in.seekg(entryOff) || !in.read(reinterpret_cast<char *>(seed), 16))
            return false;
        size_t size = size_t(stored >= 0 ? stored : real);
        size += (16 - size % 16) % 16;
        std::vector<uint8_t> data(size);
        if (!in.seekg(dataOff) || !in.read(reinterpret_cast<char *>(data.data()), std::streamsize(size)))
            return false;
        if (stored > -2)
            pkgDecrypt(data, pkgKey(seed));
        if (stored >= 0)
        {
            int outSize = 0;
            unsigned char *raw = DecompressData(data.data() + 2, stored - 2, &outSize);
            if (!raw)
                return false;
            out.assign(raw, raw + std::min(outSize, real));
            MemFree(raw);
        }
        else
            out.assign(data.begin(), data.begin() + real);
        return true;
    }

    // An image's pixels, RGBA with the GS's alpha (0x80 opaque).
    bool pixels(int entry, int &w, int &h, std::vector<uint8_t> &rgba)
    {
        const std::string &ref = m_entries[size_t(entry)].path;
        std::string path;
        std::vector<uint8_t> file;
        if (ref.rfind("pkg:", 0) == 0)
        {
            path = ref;
            readPkg(ref, file);
        }
        else
        {
            path = (m_folder / ref).string();
            std::ifstream in(path, std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        w = h = 0;
        rgba.clear();
        if (file.size() > 4 && std::memcmp(file.data(), "DDS ", 4) == 0)
        {
            if (!loadDds(file, w, h, rgba))
                rgba.clear();
        }
        else if (!file.empty())
        {
            int comp = 0;
            if (stbi_uc *px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4))
            {
                rgba.assign(px, px + size_t(w) * size_t(h) * 4u);
                stbi_image_free(px);
            }
        }
        if (rgba.empty() || w <= 0 || h <= 0)
        {
            std::fprintf(stderr, "[hd] cannot load %s\n", path.c_str());
            return false;
        }
        static const bool tint = std::getenv("KH2_HD_TINT") != nullptr; // debugging: replaced = red
        for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i)
        {
            rgba[i * 4 + 3] = uint8_t((unsigned(rgba[i * 4 + 3]) * 128u + 127u) / 255u);
            if (tint)
            {
                rgba[i * 4 + 1] = 0;
                rgba[i * 4 + 2] = 0;
            }
        }
        return true;
    }

    Vulkan::ImageHandle upload(const std::vector<uint8_t> &rgba, int w, int h)
    {
        if (!m_device)
            return {};
        auto info = Vulkan::ImageCreateInfo::immutable_2d_image(unsigned(w), unsigned(h), VK_FORMAT_R8G8B8A8_UNORM);
        Vulkan::ImageInitialData init = {rgba.data(), 0, 0};
        return m_device->create_image(info, &init);
    }

    // The image for an entry. Decoding (inflating, DXT) runs on worker
    // threads so a new area does not stall the frame: until an image is
    // ready the game's own texture is drawn (KH2_HD_SYNC=1 waits instead,
    // for reproducible screenshots).
    Vulkan::ImageHandle image(int entry)
    {
        auto found = m_images.find(entry);
        if (found != m_images.end())
        {
            found->second.lastUse = m_finds;
            return found->second.image;
        }
        static const bool sync = std::getenv("KH2_HD_SYNC") != nullptr;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        bool ok = false;
        if (sync)
            ok = pixels(entry, w, h, rgba);
        else
        {
            std::unique_lock<std::mutex> lock(m_loadMutex);
            auto done = m_loaded.find(entry);
            if (done == m_loaded.end())
            {
                if (m_queued.insert(entry).second)
                {
                    m_queue.push_back(entry);
                    startWorkers();
                    m_loadCond.notify_one();
                }
                return {};
            }
            w = done->second.w;
            h = done->second.h;
            rgba = std::move(done->second.rgba);
            ok = done->second.ok;
            m_loaded.erase(done);
            m_queued.erase(entry);
        }
        Vulkan::ImageHandle img;
        if (ok)
            img = upload(rgba, w, h);
        const uint64_t bytes = img ? uint64_t(w) * uint64_t(h) * 4u : 0;
        m_images[entry] = {img, m_finds, bytes}; // a failure is remembered too
        m_imageBytes += bytes;
        trimImages();
        return img;
    }

    void startWorkers()
    {
        if (!m_workers.empty())
            return;
        const unsigned n = std::max(1u, std::min(3u, std::thread::hardware_concurrency() / 4u));
        for (unsigned i = 0; i < n; ++i)
            m_workers.emplace_back([this] { worker(); });
    }

    void worker()
    {
        for (;;)
        {
            int entry;
            {
                std::unique_lock<std::mutex> lock(m_loadMutex);
                m_loadCond.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop)
                    return;
                // Newest first: what the game asked for last is on screen now.
                entry = m_queue.back();
                m_queue.pop_back();
            }
            Loaded l;
            l.ok = pixels(entry, l.w, l.h, l.rgba);
            std::lock_guard<std::mutex> lock(m_loadMutex);
            m_loaded[entry] = std::move(l);
        }
    }

    void stopWorkers()
    {
        {
            std::lock_guard<std::mutex> lock(m_loadMutex);
            m_stop = true;
            m_queue.clear();
        }
        m_loadCond.notify_all();
        for (auto &t : m_workers)
            t.join();
        m_workers.clear();
        std::lock_guard<std::mutex> lock(m_loadMutex);
        m_stop = false;
        m_loaded.clear();
        m_queued.clear();
    }

    ~Replacer() override { stopWorkers(); }

    struct Loaded
    {
        bool ok = false;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    std::mutex m_loadMutex;
    std::condition_variable m_loadCond;
    std::deque<int> m_queue;
    std::set<int> m_queued;
    std::unordered_map<int, Loaded> m_loaded;
    std::vector<std::thread> m_workers;
    bool m_stop = false;

    // Keeps the images under a budget (KH2_HD_BUDGET_MB, default 1536):
    // the least recently used go (paraLLEl-GS holds its own references to
    // what the frame being drawn uses).
    void trimImages()
    {
        static const uint64_t budget = [] {
            const char *env = std::getenv("KH2_HD_BUDGET_MB");
            const long mb = env ? std::atol(env) : 1536;
            return uint64_t(std::max(mb, 64L)) << 20;
        }();
        if (m_imageBytes <= budget)
            return;
        std::vector<std::pair<long, int>> byAge;
        byAge.reserve(m_images.size());
        for (const auto &kv : m_images)
            if (kv.second.bytes)
                byAge.emplace_back(kv.second.lastUse, kv.first);
        std::sort(byAge.begin(), byAge.end());
        for (const auto &[use, entry] : byAge)
        {
            if (m_imageBytes <= budget * 3 / 4 || use >= m_finds)
                break;
            auto it = m_images.find(entry);
            m_imageBytes -= it->second.bytes;
            m_images.erase(it);
        }
    }

    // A pixel cache for compositing (base images and frame strips).
    const std::vector<uint8_t> *cachedPixels(int entry, int &w, int &h)
    {
        for (auto &c : m_pixelCache)
            if (c.entry == entry)
            {
                c.lastUse = m_finds;
                w = c.w;
                h = c.h;
                return c.rgba.empty() ? nullptr : &c.rgba;
            }
        PixelCache c;
        c.entry = entry;
        c.lastUse = m_finds;
        pixels(entry, c.w, c.h, c.rgba);
        if (m_pixelCache.size() >= 8)
            m_pixelCache.erase(std::min_element(m_pixelCache.begin(), m_pixelCache.end(),
                                                [](const PixelCache &a, const PixelCache &b) { return a.lastUse < b.lastUse; }));
        m_pixelCache.push_back(std::move(c));
        w = m_pixelCache.back().w;
        h = m_pixelCache.back().h;
        return m_pixelCache.back().rgba.empty() ? nullptr : &m_pixelCache.back().rgba;
    }

    // The 16 colours a 4-bit texture is drawn with (CLUT at CBP: an 8x2
    // upload, or a 16x16 one holding 16 palettes picked by CSA in CSM1
    // order), RGBA with the GS's alpha.
    bool palette16(const ParallelGS::TEX0Bits &tex0, std::array<std::array<uint8_t, 4>, 16> &out) const
    {
        auto it = m_cluts.find(uint32_t(tex0.CBP));
        if (it == m_cluts.end())
            return false;
        const Clut &c = it->second;
        if (c.w * c.h == 16 && c.data.size() >= 64)
        {
            for (int i = 0; i < 16; ++i)
                std::memcpy(out[size_t(i)].data(), &c.data[size_t(i) * 4u], 4);
            return true;
        }
        if (c.w == 16 && c.h == 16 && c.data.size() >= 1024)
        {
            // CSM1: within each 32 entries, 8-15 and 16-23 are swapped.
            for (int i = 0; i < 16; ++i)
            {
                const int index = int(tex0.CSA) * 16 + i;
                const int stored = (index & ~0x18) | ((index & 0x08) << 1) | ((index & 0x10) >> 1);
                std::memcpy(out[size_t(i)].data(), &c.data[size_t(stored) * 4u], 4);
            }
            return true;
        }
        return false;
    }

    // The PC's fonts are painted in one set of colours (grey letters, black
    // outline), where the disc's 4-bit fonts are recoloured by palette per
    // text box (dark letters without an outline in speech bubbles). The HD
    // font is turned back into its parts (outline .. fill, by brightness,
    // and coverage) and coloured with the palette the game draws with.
    Vulkan::ImageHandle recolouredFont(int entry, const ParallelGS::TEX0Bits &tex0)
    {
        if ((tex0.PSM != 0x14 && tex0.PSM != 0x24 && tex0.PSM != 0x2C) ||
            m_entries[size_t(entry)].source != "msg/us/fontimage.bar")
            return {};
        std::array<std::array<uint8_t, 4>, 16> pal{};
        if (!palette16(tex0, pal))
            return {};
        uint64_t key = 0xcbf29ce484222325ull ^ uint64_t(uint32_t(entry));
        for (const auto &col : pal)
            for (uint8_t b : col)
                key = (key ^ b) * 0x100000001b3ull;
        for (auto &f : m_fonts)
            if (f.key == key)
            {
                f.lastUse = m_finds;
                return f.image;
            }
        if (m_log && m_loggedFontPalettes++ < 12)
        {
            std::printf("[hd] font palette cbp %u csa %u:", uint32_t(tex0.CBP), uint32_t(tex0.CSA));
            for (int i = 0; i < 4; ++i)
                std::printf(" %02x%02x%02x/%02x", pal[size_t(i)][0], pal[size_t(i)][1], pal[size_t(i)][2], pal[size_t(i)][3]);
            std::printf("\n");
        }
        int w = 0, h = 0;
        const std::vector<uint8_t> *src = cachedPixels(entry, w, h);
        if (!src)
            return {};
        std::vector<uint8_t> out(src->size());
        // Measured against the disc's font (each 4-bit index's HD pixels):
        // index 1 is the dark outer edge, 2 the outline's inner part (about
        // 60 % of the fill's brightness), 3 the fill.
        const auto &c1 = pal[1], &c2 = pal[2], &c3 = pal[3];
        for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i)
        {
            const uint8_t *p = &(*src)[i * 4];
            const float t = std::min(1.0f, float(std::max({p[0], p[1], p[2]})) / 120.0f);
            const float coverage = float(p[3]) / 128.0f;
            const auto &lo = t < 0.6f ? c1 : c2;
            const auto &hi = t < 0.6f ? c2 : c3;
            const float f = t < 0.6f ? t / 0.6f : (t - 0.6f) / 0.4f;
            for (int k = 0; k < 3; ++k)
                out[i * 4 + size_t(k)] = uint8_t(float(lo[size_t(k)]) + (float(hi[size_t(k)]) - float(lo[size_t(k)])) * f + 0.5f);
            const float a = float(lo[3]) + (float(hi[3]) - float(lo[3])) * f;
            out[i * 4 + 3] = uint8_t(std::min(255.0f, a * coverage + 0.5f));
        }
        Vulkan::ImageHandle img = upload(out, w, h);
        if (m_fonts.size() >= 16)
            m_fonts.erase(std::min_element(m_fonts.begin(), m_fonts.end(),
                                           [](const Composite &x, const Composite &y) { return x.lastUse < y.lastUse; }));
        m_fonts.push_back({key, img, m_finds});
        return img;
    }

    // The base image with the animation frames the game has sent into the
    // texture drawn over it (texel rectangle rx, ry, rw x rh of the texture).
    Vulkan::ImageHandle composite(int base, const std::vector<Patch> &patches, uint32_t rx, uint32_t ry, uint32_t rw,
                                  uint32_t rh)
    {
        uint64_t key = 0xcbf29ce484222325ull;
        auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001b3ull; };
        mix(uint64_t(base));
        mix(uint64_t(rx) << 32 | ry);
        mix(uint64_t(rw) << 32 | rh);
        for (const Patch &p : patches)
        {
            mix(uint64_t(p.x) << 32 | p.y);
            mix(uint64_t(p.w) << 32 | p.h);
            mix(uint64_t(uint32_t(p.entry)));
        }
        for (auto &c : m_composites)
            if (c.key == key)
            {
                c.lastUse = m_finds;
                return c.image;
            }
        int bw = 0, bh = 0;
        const std::vector<uint8_t> *b = cachedPixels(base, bw, bh);
        if (!b || !rw || !rh)
            return {};
        std::vector<uint8_t> out = *b;
        const float sx = float(bw) / float(rw), sy = float(bh) / float(rh);
        for (const Patch &p : patches)
        {
            const Entry &e = m_entries[size_t(p.entry)];
            int fw = 0, fh = 0;
            const std::vector<uint8_t> *f = cachedPixels(p.entry, fw, fh);
            if (!f || p.x < rx || p.y < ry)
                continue;
            // The frame's rows in the strip, and the part of the sprite the
            // game sent (it may send fewer rows than a frame has).
            const float rowsPerFrame = float(fh) / float(e.pframes);
            const float top = float(e.pframe) * rowsPerFrame;
            const int dx0 = int(float(p.x - rx) * sx + 0.5f), dy0 = int(float(p.y - ry) * sy + 0.5f);
            const int dx1 = std::min(bw, int(float(p.x - rx + p.w) * sx + 0.5f));
            const int dy1 = std::min(bh, int(float(p.y - ry + p.h) * sy + 0.5f));
            for (int y = std::max(dy0, 0); y < dy1; ++y)
            {
                const float v = (float(y - dy0) + 0.5f) / sy; // texels into the patch
                const int srcY = std::min(fh - 1, int(top + v / float(e.ph) * rowsPerFrame));
                for (int x = std::max(dx0, 0); x < dx1; ++x)
                {
                    const float u = (float(x - dx0) + 0.5f) / sx;
                    const int srcX = std::min(fw - 1, int(u / float(e.pw) * float(fw)));
                    std::memcpy(&out[(size_t(y) * size_t(bw) + size_t(x)) * 4u],
                                &(*f)[(size_t(srcY) * size_t(fw) + size_t(srcX)) * 4u], 4);
                }
            }
        }
        // Diagnostics: KH2_HD_DUMP=<folder> writes each new composite and its
        // base image as raw RGBA (<name>_<w>x<h>.rgba).
        static const char *dump = std::getenv("KH2_HD_DUMP");
        if (dump && m_compDumped < 60)
        {
            auto write = [&](const char *tag, const std::vector<uint8_t> &px) {
                char name[512];
                std::snprintf(name, sizeof(name), "%s/%03d_%s_%dx%d.rgba", dump, m_compDumped, tag, bw, bh);
                if (FILE *fd = std::fopen(name, "wb"))
                {
                    std::fwrite(px.data(), 1, px.size(), fd);
                    std::fclose(fd);
                }
            };
            write("base", *b);
            write("comp", out);
            std::printf("[hd] dump %03d: %s rect %u,%u %ux%u, %zu patch(es)\n", m_compDumped,
                        m_entries[size_t(base)].path.c_str(), rx, ry, rw, rh, patches.size());
            for (const Patch &p : patches)
                std::printf("[hd]   patch %u,%u %ux%u <- %s\n", p.x, p.y, p.w, p.h, m_entries[size_t(p.entry)].path.c_str());
            ++m_compDumped;
        }
        Vulkan::ImageHandle img = upload(out, bw, bh);
        if (m_log && m_loggedComposites++ < 40)
            std::printf("[hd] composite %s with %zu animation frame(s)\n", m_entries[size_t(base)].source.c_str(),
                        patches.size());
        if (m_composites.size() >= 16)
            m_composites.erase(std::min_element(m_composites.begin(), m_composites.end(),
                                                [](const Composite &a, const Composite &c) { return a.lastUse < c.lastUse; }));
        m_composites.push_back({key, img, m_finds});
        return img;
    }

    struct ImageSlot
    {
        Vulkan::ImageHandle image;
        long lastUse = 0;
        uint64_t bytes = 0;
    };
    struct PixelCache
    {
        int entry = -1;
        int w = 0, h = 0;
        long lastUse = 0;
        std::vector<uint8_t> rgba;
    };
    struct Composite
    {
        uint64_t key;
        Vulkan::ImageHandle image;
        long lastUse;
    };

    std::filesystem::path m_folder;
    std::vector<std::string> m_archives;
    std::vector<Entry> m_entries;
    std::unordered_multimap<uint64_t, int> m_byKey;
    std::vector<Region> m_regions;
    int m_compDumped = 0; // KH2_HD_DUMP
    std::unordered_map<int, ImageSlot> m_images;
    uint64_t m_imageBytes = 0;
    std::vector<PixelCache> m_pixelCache;
    std::vector<Composite> m_composites;
    struct Clut
    {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> data;
    };
    std::unordered_map<uint32_t, Clut> m_cluts;
    std::vector<Composite> m_fonts;
    int m_loggedFontPalettes = 0;
    int m_loggedComposites = 0;
    Vulkan::Device *m_device = nullptr;
    std::atomic<bool> m_enabled{true};
    bool m_log = false;
    std::set<int> m_loggedUploads, m_loggedHits;
    std::set<uint64_t> m_loggedMisses, m_loggedDrops, m_loggedFinds, m_scanned, m_traced;
    int m_dumped = 0;
    long m_finds = 0;
    int m_watchLines = 0;
    long m_dumpSeen = 0;
    long m_tracedUploads = 0;
    std::set<uint64_t> m_tracedKeys, m_tracedHits;
};

std::unique_ptr<Replacer> g_replacer;
bool g_log = false;
} // namespace

// tools/egs_hd_extract.py's "#format" line: older indexes miss things.
constexpr int kManifestFormat = 4;

size_t load(const std::filesystem::path &folder)
{
    std::ifstream in(folder / "manifest.txt");
    if (!in)
    {
        std::fprintf(stderr, "[hd] no %s\n", (folder / "manifest.txt").string().c_str());
        return 0;
    }
    auto r = std::make_unique<Replacer>();
    r->m_folder = folder;
    r->m_log = g_log;
    int format = 0;
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("#format ", 0) == 0)
        {
            format = std::atoi(line.c_str() + 8);
            continue;
        }
        if (line.rfind("#archive ", 0) == 0)
        {
            // "#archive <index> <path>" (the path may have spaces)
            std::istringstream as(line.substr(9));
            size_t index = 0;
            as >> index;
            std::string rest;
            std::getline(as, rest);
            rest.erase(0, rest.find_first_not_of(' '));
            if (r->m_archives.size() <= index)
                r->m_archives.resize(index + 1);
            r->m_archives[index] = rest;
            continue;
        }
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        std::string key, path, source, checks, hashed, variant;
        if (!(ss >> key >> hashed >> path))
            continue;
        ss >> source >> checks >> variant;
        size_t bytes = 0, skip = 0;
        if (std::sscanf(hashed.c_str(), "%zu@%zu", &bytes, &skip) < 1)
            continue;
        Entry e{path, source, bytes, skip, {}};
        if (std::sscanf(variant.c_str(), "v:%u:%u:%u:%u:%u:%u:%u:%u", &e.vtbp, &e.vcbp, &e.vcsa, &e.vtbw, &e.vx, &e.vy,
                        &e.vw, &e.vh) >= 3)
            e.variant = true;
        else if (std::sscanf(variant.c_str(), "p:%d:%d:%u:%u", &e.pframe, &e.pframes, &e.pw, &e.ph) != 4 ||
                 e.pframe < 0 || e.pframe >= e.pframes || !e.pw || !e.ph)
        {
            e.pframe = -1;
            e.pframes = 0;
        }
        std::istringstream cs(checks);
        for (std::string item; std::getline(cs, item, ',');)
        {
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
                e.checks.emplace_back(std::stoull(item.substr(0, colon)), std::stoull(item.substr(colon + 1), nullptr, 16));
        }
        r->m_byKey.emplace(std::stoull(key, nullptr, 16), int(r->m_entries.size()));
        r->m_entries.push_back(std::move(e));
    }
    const size_t n = r->m_entries.size();
    std::printf("[hd] %zu high-resolution textures in %s\n", n, folder.string().c_str());
    if (format < kManifestFormat)
        std::fprintf(stderr,
                     "[hd] %s was built by an older tools/egs_hd_extract.py: run it again for the textures it "
                     "now finds (maps, animations)\n",
                     (folder / "manifest.txt").string().c_str());
    if (n)
        g_replacer = std::move(r);
    return n;
}

ParallelGS::TextureReplacer *attach(Vulkan::Device *device)
{
    if (!g_replacer)
        return nullptr;
    g_replacer->m_device = device;
    return g_replacer.get();
}

void detach()
{
    if (g_replacer)
    {
        g_replacer->stopWorkers();
        g_replacer->m_images.clear();
        g_replacer->m_imageBytes = 0;
        g_replacer->m_composites.clear();
        g_replacer->m_fonts.clear();
        g_replacer->m_device = nullptr;
    }
}

bool loaded() { return g_replacer != nullptr; }
bool enabled() { return g_replacer && g_replacer->m_enabled; }
void setEnabled(bool on)
{
    if (g_replacer)
        g_replacer->m_enabled = on;
}
void setLog(bool on)
{
    g_log = on;
    if (g_replacer)
        g_replacer->m_log = on;
}
} // namespace kh2hd
