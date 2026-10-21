#include "file_override.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace ps2_stubs
{
bool mapHostFileToCd(const std::filesystem::path &hostPath, uint32_t &lbn, uint32_t &sectors); // patch 0018
} // namespace ps2_stubs

namespace kh2files
{
namespace
{
constexpr uint32_t kIndexTables[] = {0x01C6CFB0u, 0x01C6CFB4u, 0x01C6CFB8u};
constexpr uint32_t kMaxEntries = 0x10000;

struct Override
{
    std::string name;
    std::filesystem::path host;
    uint32_t h32 = 0;
    uint16_t h16 = 0;
    uint32_t size = 0;
    uint32_t lbn = 0;
    uint32_t sectors = 0;
    bool mapped = false;
    bool reported = false;
};
std::vector<Override> g_files;

uint32_t rd32(const uint8_t *ram, uint32_t a)
{
    uint32_t v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
    return v;
}
uint16_t rd16(const uint8_t *ram, uint32_t a)
{
    uint16_t v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), 2);
    return v;
}
} // namespace

uint32_t hash32(const std::string &name)
{
    // CRC-32, polynomial 0x04C11DB7, MSB first, init and final xor ~0
    // (FUN_001AE210; signed chars, as lb gives them).
    uint32_t c = 0xFFFFFFFFu;
    for (char ch : name)
    {
        c ^= uint32_t(int32_t(int8_t(ch))) << 24;
        for (int i = 0; i < 8; ++i)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return ~c;
}

uint16_t hash16(const std::string &name)
{
    // CRC-16-CCITT over the name read backwards, init 0xFFFF, final ~
    // (FUN_001AE270).
    uint16_t c = 0xFFFF;
    for (auto it = name.rbegin(); it != name.rend(); ++it)
    {
        c = uint16_t(c ^ uint16_t(int32_t(int8_t(*it)) << 8));
        for (int i = 0; i < 8; ++i)
            c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return uint16_t(~c);
}

size_t install(const std::vector<std::filesystem::path> &folders)
{
    std::map<std::string, std::filesystem::path> byName;
    for (const auto &folder : folders)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec))
            continue;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (!it->is_regular_file(ec))
                continue;
            std::string name = std::filesystem::relative(it->path(), folder, ec).generic_string();
            if (ec || name.empty())
                continue;
            byName[name] = it->path(); // later folders win
        }
    }
    g_files.clear();
    for (const auto &[name, host] : byName)
    {
        Override o;
        o.name = name;
        o.host = host;
        o.h32 = hash32(name);
        o.h16 = hash16(name);
        std::error_code ec;
        o.size = uint32_t(std::filesystem::file_size(host, ec));
        if (ec || o.size == 0)
            continue;
        o.mapped = ps2_stubs::mapHostFileToCd(host, o.lbn, o.sectors);
        if (!o.mapped || o.sectors == 0 || o.sectors > 0x4000)
        {
            std::fprintf(stderr, "[files] cannot use %s\n", host.string().c_str());
            continue;
        }
        g_files.push_back(o);
    }
    if (!g_files.empty())
        std::printf("[files] %zu file overrides\n", g_files.size());
    return g_files.size();
}

size_t count() { return g_files.size(); }

void tick(uint8_t *rdram)
{
    if (g_files.empty())
        return;
    for (uint32_t ptrAddr : kIndexTables)
    {
        const uint32_t table = rd32(rdram, ptrAddr);
        if (table < 0x00100000u || table >= 0x02000000u)
            continue;
        const uint32_t n = rd32(rdram, table);
        if (n == 0 || n > kMaxEntries || table + 4 + n * 16 > 0x02000000u)
            continue;
        const uint32_t first = table + 4;
        for (Override &o : g_files)
        {
            // Binary search on the CRC-32, then the run of equal ones.
            uint32_t lo = 0, hi = n;
            while (lo < hi)
            {
                const uint32_t mid = (lo + hi) / 2;
                if (rd32(rdram, first + mid * 16) < o.h32)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            uint32_t hit = 0;
            int matches = 0;
            for (uint32_t i = lo; i < n && rd32(rdram, first + i * 16) == o.h32; ++i)
            {
                const uint32_t e = first + i * 16;
                const bool checkSecondary = (rd16(rdram, e + 6) & 0x8000) != 0;
                if (!checkSecondary || rd16(rdram, e + 4) == o.h16)
                {
                    hit = e;
                    ++matches;
                }
            }
            if (!hit || matches > 1)
                continue;
            if (rd32(rdram, hit + 8) == o.lbn && rd32(rdram, hit + 12) == o.size)
                continue; // done already
            const uint16_t block = uint16_t((rd16(rdram, hit + 6) & 0x8000) | ((o.sectors - 1) & 0x3FFF));
            std::memcpy(rdram + (hit + 6), &block, 2); // not compressed, not streamed
            std::memcpy(rdram + (hit + 8), &o.lbn, 4);
            std::memcpy(rdram + (hit + 12), &o.size, 4);
            if (!o.reported)
            {
                o.reported = true;
                std::printf("[files] %s -> %s (%u bytes)\n", o.name.c_str(), o.host.string().c_str(), o.size);
                std::fflush(stdout);
            }
        }
    }
}
} // namespace kh2files
