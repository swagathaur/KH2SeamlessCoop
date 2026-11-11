// sceCdSearchFile served from the disc image's ISO9660 directory.
//
// The runtime's sceCdSearchFile only finds loose files under the CD root and
// gives them made-up sector numbers starting at 0x100000. KH2 looks up its
// archives (KH2.IDX/KH2.IMG, OVL.IDX/OVL.IMG) this way and then reads sectors
// relative to the returned LSN. Those archives exist only inside the ISO, and
// KH2.IMG extends past sector 0x100000 on this 4.3 GB disc, so pseudo sectors
// would collide. Returning the file's real LSN instead makes every read fall
// through to the runtime's ISO path (reads outside registered loose files go
// straight to the image at that sector).

#include "cd_iso.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps2_runtime.h"

namespace kh2cd
{
namespace
{
constexpr uint32_t kSceCdSearchFile = 0x002F6FA0u; // wrapper of sceCdLayerSearchFile (layer 0)
constexpr uint32_t kSector = 2048u;

struct Entry
{
    uint32_t lsn;
    uint32_t size;
};

std::unordered_map<std::string, Entry> g_files; // "KH2.IMG", "PAD2\\DS2O.IRX", ...
std::string g_imagePath;

std::string normalize(std::string path)
{
    const size_t colon = path.find(':');
    if (colon != std::string::npos)
        path = path.substr(colon + 1);
    std::string out;
    for (char c : path)
    {
        if (c == '/')
            c = '\\';
        if (c == ';')
            break; // version suffix
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && out.front() == '\\')
        out.erase(out.begin());
    return out;
}

uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool readSectors(std::ifstream &iso, uint32_t lsn, uint32_t bytes, std::vector<uint8_t> &out)
{
    out.resize(bytes);
    iso.seekg(static_cast<std::streamoff>(lsn) * kSector);
    iso.read(reinterpret_cast<char *>(out.data()), bytes);
    return iso.good();
}

void walk(std::ifstream &iso, uint32_t lsn, uint32_t size, const std::string &prefix, int depth)
{
    std::vector<uint8_t> dir;
    if (depth > 8 || !readSectors(iso, lsn, size, dir))
        return;
    for (size_t i = 0; i < dir.size();)
    {
        const uint8_t len = dir[i];
        if (len == 0)
        {
            i = (i / kSector + 1) * kSector; // records never cross sectors
            continue;
        }
        const uint8_t *rec = &dir[i];
        const uint32_t childLsn = le32(rec + 2);
        const uint32_t childSize = le32(rec + 10);
        const bool isDir = (rec[25] & 2) != 0;
        const uint8_t nameLen = rec[32];
        std::string name(reinterpret_cast<const char *>(rec + 33), nameLen);
        i += len;
        if (nameLen == 1 && (name[0] == 0 || name[0] == 1))
            continue; // "." and ".."
        const std::string full = normalize(prefix.empty() ? name : prefix + "\\" + name);
        if (isDir)
            walk(iso, childLsn, childSize, full, depth + 1);
        else
            g_files[full] = {childLsn, childSize};
    }
}

void searchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t fileAddr = getRegU32(ctx, 4);
    const uint32_t nameAddr = getRegU32(ctx, 5);
    std::string path;
    for (uint32_t i = 0; i < 256; ++i)
    {
        const char c = static_cast<char>(rdram[(nameAddr + i) & PS2_RAM_MASK]);
        if (!c)
            break;
        path.push_back(c);
    }
    const std::string key = normalize(path);
    const auto it = g_files.find(key);
    if (it == g_files.end())
    {
        std::fprintf(stderr, "[kh2-cd] sceCdSearchFile: %s not on disc\n", path.c_str());
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    // sceCdlFILE: u32 lsn, u32 size, char name[16], u8 date[8]
    uint8_t *out = rdram + (fileAddr & PS2_RAM_MASK);
    std::memset(out, 0, 32);
    std::memcpy(out + 0, &it->second.lsn, 4);
    std::memcpy(out + 4, &it->second.size, 4);
    const size_t slash = key.find_last_of('\\');
    const std::string leaf = slash == std::string::npos ? key : key.substr(slash + 1);
    std::strncpy(reinterpret_cast<char *>(out + 8), leaf.c_str(), 15);
    static int logged = 0;
    if (logged++ < 32)
        std::printf("[kh2-cd] sceCdSearchFile %s -> lsn %u, %u bytes\n", path.c_str(), it->second.lsn,
                    it->second.size);
    setReturnS32(ctx, 1);
    ctx->pc = getRegU32(ctx, 31);
}
// KH2.IMG's compression (port of OpenKh.Kh2/Img.Decompress, MIT; also
// tools/kh2_compress.py): read from the end, a key byte, the size, then a
// backwards LZ stream.
std::vector<uint8_t> decompress(const std::vector<uint8_t> &src)
{
    std::vector<uint8_t> dst;
    long i = long(src.size()) - 1;
    uint8_t key = 0;
    while (i > 0)
    {
        key = src[size_t(i--)];
        if (key != 0)
            break;
    }
    if (i < 4)
        return dst;
    const uint32_t size = src[size_t(i)] | (src[size_t(i - 1)] << 8) | (src[size_t(i - 2)] << 16) |
                          (uint32_t(src[size_t(i - 3)]) << 24);
    i -= 4;
    if (size > 64u * 1024u * 1024u)
        return dst;
    dst.resize(size);
    long o = long(size) - 1;
    while (o >= 0 && i >= 0)
    {
        const uint8_t b = src[size_t(i--)];
        if (b == key && i >= 0)
        {
            const uint8_t dist = src[size_t(i--)];
            if (dist > 0 && i >= 0)
            {
                int n = src[size_t(i--)] + 3;
                for (; n > 0 && o >= 0; --n, --o)
                    dst[size_t(o)] = size_t(o) + dist < dst.size() ? dst[size_t(o) + dist] : 0;
            }
            else
                dst[size_t(o--)] = b;
        }
        else
            dst[size_t(o--)] = b;
    }
    return dst;
}

// The game's name hash (CRC-32, poly 0x04C11DB7, MSB first; kh2files::hash32).
uint32_t nameHash(const std::string &name)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char c : name)
    {
        crc ^= uint32_t(c) << 24;
        for (int k = 0; k < 8; ++k)
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return ~crc;
}

// An index entry: +0 hash, +4 hash16, +6 block (sectors - 1 in bits 0-13,
// compressed: bit 14), +8 sector in KH2.IMG, +C size.
bool findEntry(const std::vector<uint8_t> &idx, uint32_t hash, uint16_t &block, uint32_t &sector, uint32_t &size)
{
    if (idx.size() < 4)
        return false;
    const uint32_t n = le32(idx.data());
    for (uint32_t k = 0; k < n && 4 + (k + 1) * 16 <= idx.size(); ++k)
    {
        const uint8_t *e = idx.data() + 4 + k * 16;
        if (le32(e) == hash)
        {
            block = uint16_t(e[6] | (e[7] << 8));
            sector = le32(e + 8);
            size = le32(e + 12);
            return true;
        }
    }
    return false;
}

bool readEntry(std::ifstream &iso, uint32_t imgLsn, uint16_t block, uint32_t sector, uint32_t size,
               std::vector<uint8_t> &out)
{
    if (block & 0x4000)
    {
        std::vector<uint8_t> raw;
        if (!readSectors(iso, imgLsn + sector, ((block & 0x3FFFu) + 1u) * kSector, raw))
            return false;
        out = decompress(raw);
    }
    else if (!readSectors(iso, imgLsn + sector, size, out))
        return false;
    if (out.size() < size)
        return false;
    out.resize(size);
    return true;
}
} // namespace

bool readGameFile(const std::string &name, std::vector<uint8_t> &out, const std::string &subIndex)
{
    const auto idxFile = g_files.find("KH2.IDX"), img = g_files.find("KH2.IMG");
    if (idxFile == g_files.end() || img == g_files.end())
        return false;
    std::ifstream iso(g_imagePath, std::ios::binary);
    std::vector<uint8_t> idx;
    if (!iso || !readSectors(iso, idxFile->second.lsn, idxFile->second.size, idx))
        return false;
    uint16_t block = 0;
    uint32_t sector = 0, size = 0;
    if (!subIndex.empty())
    {
        std::vector<uint8_t> sub;
        if (findEntry(idx, nameHash(subIndex), block, sector, size) &&
            readEntry(iso, img->second.lsn, block, sector, size, sub) &&
            findEntry(sub, nameHash(name), block, sector, size))
            return readEntry(iso, img->second.lsn, block, sector, size, out);
    }
    return findEntry(idx, nameHash(name), block, sector, size) &&
           readEntry(iso, img->second.lsn, block, sector, size, out);
}

bool install(PS2Runtime &runtime, const std::string &isoPath)
{
    g_imagePath = isoPath;
    std::ifstream iso(isoPath, std::ios::binary);
    std::vector<uint8_t> pvd;
    if (!iso || !readSectors(iso, 16, kSector, pvd) || std::memcmp(&pvd[1], "CD001", 5) != 0)
    {
        std::fprintf(stderr, "[kh2-cd] %s is not a readable ISO9660 image; disc lookups will fail\n",
                     isoPath.c_str());
        return false;
    }
    const uint8_t *root = &pvd[156];
    walk(iso, le32(root + 2), le32(root + 10), "", 0);
    if (!runtime.replaceFunction(kSceCdSearchFile, &searchFile))
    {
        std::fprintf(stderr, "[kh2-cd] could not bind sceCdSearchFile at 0x%08X\n", kSceCdSearchFile);
        return false;
    }
    std::printf("[kh2-cd] %zu files indexed from the disc image\n", g_files.size());
    return true;
}
} // namespace kh2cd
