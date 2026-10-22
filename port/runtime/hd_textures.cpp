#include "hd_textures.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
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
// tools/egs_hd_extract.py writes (both are fast in Python's zlib).
uint32_t crc32(const uint8_t *p, size_t n)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
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
    return b << 16 | a;
}
uint64_t key64(const uint8_t *p, size_t n) { return uint64_t(crc32(p, n)) << 32 | adler32(p, n); }

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
                if (key64(bytes, len) != hash)
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
        for (size_t i = 0; i < m_regions.size();)
        {
            const Region &r = m_regions[i];
            // Small uploads (palettes) tucked into a texture's unused blocks
            // do not end it; anything bigger, or at its base, does.
            const bool tucked = size < 4096 && dbp != r.dbp;
            // A patch into the texture itself (KH2 animates faces by sending
            // a few rows of the eyes and mouth into the character's texture,
            // at its base pointer with an offset): the HD image stays.
            const bool patch = dbp == r.dbp && (pos.DSAX || pos.DSAY) && size * 4 <= r.bytes;
            if (!tucked && !patch && r.firstPage <= last && first <= r.lastPage && (r.mask & mask))
            {
                if (m_log && m_loggedDrops.insert(uint64_t(r.entry) << 32 | dbp).second)
                    std::printf("[hd] %s dropped: upload dbp %u psm 0x%02X %ux%u over it\n",
                                m_entries[size_t(r.entry)].path.c_str(), dbp, psm, w, h);
                m_regions.erase(m_regions.begin() + ptrdiff_t(i));
            }
            else
                ++i;
        }
        const auto *bytes = static_cast<const uint8_t *>(data);
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
        size_t matchLen = 0;
        std::vector<int> alts;
        const int match = bestMatch(bytes, size, matchLen, &alts);
        // KH2_HD_TRACE: the first 2000 different uploads (by place and data).
        static const bool traceUp = std::getenv("KH2_HD_TRACE") != nullptr;
        if (traceUp && m_tracedUploads < 2000 &&
            m_tracedKeys.insert(key64(bytes, std::min<size_t>(size, 4096)) ^ (uint64_t(dbp) << 40 | size)).second)
        {
            ++m_tracedUploads;
            std::printf("[hd] trace upload dbp %u dbw %u psm 0x%02X %ux%u at %u,%u size %zu -> %s\n", dbp, dbw, psm, w, h,
                        pos.DSAX, pos.DSAY, size, match >= 0 ? m_entries[size_t(match)].source.c_str() : "-");
        }
        if (match < 0)
            return;
        // The agreement has to include real content: blank stretches (cleared
        // buffers, transparent corners) look alike everywhere.
        {
            const size_t n2 = std::min<size_t>(size, matchLen);
            size_t differing = 0;
            for (size_t i = 1; i < n2; ++i)
                differing += bytes[i] != bytes[0];
            if (differing * 64 < n2)
                return;
        }
        const Entry &e = m_entries[size_t(match)];
        m_regions.push_back({dbp, dbw, psm, pos.DSAX, pos.DSAY, w, h, uint32_t(first), uint32_t(last), mask, match,
                             uint64_t(size), std::move(alts)});
        if (m_log && m_loggedUploads.insert(match).second)
            std::printf("[hd] upload %s: dbp %u dbw %u psm 0x%02X %ux%u at %u,%u (%zu bytes agree) -> %s\n",
                        e.source.c_str(), dbp, dbw, psm, w, h, pos.DSAX, pos.DSAY, matchLen, e.path.c_str());
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
            const Entry &ce = m_entries[size_t(chosen)];
            if (ce.vw && ce.vh && ce.vx + ce.vw <= rw && ce.vy + ce.vh <= rh)
            {
                // The image is one part of an atlas.
                rx += ce.vx;
                ry += ce.vy;
                rw = ce.vw;
                rh = ce.vh;
            }
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

    Vulkan::ImageHandle image(int entry)
    {
        auto found = m_images.find(entry);
        if (found != m_images.end())
            return found->second;
        Vulkan::ImageHandle img;
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
        int w = 0, h = 0, comp = 0;
        std::vector<uint8_t> dds;
        stbi_uc *px = nullptr;
        if (file.size() > 4 && std::memcmp(file.data(), "DDS ", 4) == 0)
        {
            if (loadDds(file, w, h, dds))
                px = dds.data();
        }
        else if (!file.empty())
            px = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
        if (px && m_device)
        {
            // GS texture alpha: 0x80 is opaque.
            static const bool tint = std::getenv("KH2_HD_TINT") != nullptr; // debugging: replaced = red
            for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i)
            {
                px[i * 4 + 3] = stbi_uc((unsigned(px[i * 4 + 3]) * 128u + 127u) / 255u);
                if (tint)
                {
                    px[i * 4 + 1] = 0;
                    px[i * 4 + 2] = 0;
                }
            }
            auto info = Vulkan::ImageCreateInfo::immutable_2d_image(unsigned(w), unsigned(h), VK_FORMAT_R8G8B8A8_UNORM);
            Vulkan::ImageInitialData init = {px, 0, 0};
            img = m_device->create_image(info, &init);
        }
        else
            std::fprintf(stderr, "[hd] cannot load %s\n", path.c_str());
        if (px && dds.empty())
            stbi_image_free(px);
        m_images[entry] = img; // a failure is remembered too
        return img;
    }

    std::filesystem::path m_folder;
    std::vector<std::string> m_archives;
    std::vector<Entry> m_entries;
    std::unordered_multimap<uint64_t, int> m_byKey;
    std::vector<Region> m_regions;
    std::unordered_map<int, Vulkan::ImageHandle> m_images;
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
    for (std::string line; std::getline(in, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
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
        g_replacer->m_images.clear();
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
