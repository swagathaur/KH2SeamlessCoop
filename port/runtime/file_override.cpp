#include "file_override.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
    // Diagnostics: KH2_FILES_SKIP=a,b,... leaves out names containing any of
    // these (to find which override a problem comes from).
    std::vector<std::string> skips;
    if (const char *env = std::getenv("KH2_FILES_SKIP"))
    {
        std::string all = env;
        for (size_t at = 0; at <= all.size();)
        {
            const size_t comma = std::min(all.find(',', at), all.size());
            if (comma > at)
                skips.push_back(all.substr(at, comma - at));
            at = comma + 1;
        }
    }
    size_t refused = 0;
    for (const auto &[name, host] : byName)
    {
        if (std::any_of(skips.begin(), skips.end(), [&](const std::string &k) { return name.find(k) != std::string::npos; }))
            continue;
        // The PC release's field HUD sheets (field2d/us/xx0field.2dd) have
        // bigger pictures and their own layout (a 512x256 gauge sheet where
        // the disc has 256x256): with them this executable corrupts memory
        // and crashes in Olympus Coliseum. The disc's own are kept (their
        // words are in English anyway: LEVEL UP!, FINISH!, ...).
        static const std::string kFieldSheet = "field.2dd";
        if (name.rfind("field2d/", 0) == 0 && name.size() > kFieldSheet.size() &&
            name.compare(name.size() - kFieldSheet.size(), kFieldSheet.size(), kFieldSheet) == 0)
        {
            ++refused;
            continue;
        }
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
        std::printf("[files] %zu file overrides (%zu HUD sheets left out)\n", g_files.size(), refused);
    return g_files.size();
}

size_t count() { return g_files.size(); }

namespace
{
// What the last full pass over each index table found: its address and count,
// and the entry each override was written to. While those entries still hold
// the override (and the table has not moved), nothing needs searching: the
// search was ~10% of a core at 250 passes a second with the English files.
struct TablePass
{
    uint32_t table = 0, n = 0;
    uint64_t sample = 0; // fingerprint of 256 spread-out entries (a table being loaded changes it)
    uint32_t stablePasses = 0; // passes since the table last changed
    size_t cursor = 0;         // next written entry to check
    std::vector<std::pair<uint32_t, const Override *>> written; // entry address, override
};
TablePass g_pass[sizeof(kIndexTables) / sizeof(kIndexTables[0])];

uint64_t sampleEntries(const uint8_t *rdram, uint32_t table, uint32_t n)
{
    // Entries at most n/256 apart (fewer than a disc sector's 128 entries
    // for any table this size), so a sector of the table being read changes it.
    uint64_t h = n;
    const uint32_t step = std::max<uint32_t>(1, n / 256);
    for (uint32_t i = 0; i < n; i += step)
    {
        const uint32_t e = table + 4 + i * 16;
        h = (h ^ rd32(rdram, e) ^ (uint64_t(rd32(rdram, e + 8)) << 32)) * 0x100000001B3ull;
    }
    return h;
}

bool passStillValid(const uint8_t *rdram, TablePass &p, uint32_t table, uint32_t n)
{
    if (p.table != table || p.n != n)
        return false;
    // The fingerprint is ~800 scattered reads: every pass for 2 s after a
    // change (a table being loaded), then every 16th (64 ms). The written
    // entries are checked 64 a pass, in turn: reloading the table would undo
    // all of them at once.
    if ((p.stablePasses < 500 || (p.stablePasses & 15) == 0) && p.sample != sampleEntries(rdram, table, n))
        return false;
    const size_t count = p.written.size();
    for (size_t k = 0; k < std::min<size_t>(64, count); ++k)
    {
        if (p.cursor >= count)
            p.cursor = 0;
        const auto &[entry, o] = p.written[p.cursor++];
        if (rd32(rdram, entry) != o->h32 || rd32(rdram, entry + 8) != o->lbn || rd32(rdram, entry + 12) != o->size)
            return false;
    }
    ++p.stablePasses;
    return true;
}
} // namespace

void tick(uint8_t *rdram)
{
    if (g_files.empty())
        return;
    static const bool stats = std::getenv("KH2_FILES_STATS") != nullptr;
    struct Timed
    {
        bool on;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed()
        {
            if (!on)
                return;
            static uint64_t ns = 0, calls = 0;
            static auto last = t0;
            const auto t1 = std::chrono::steady_clock::now();
            ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            ++calls;
            if (t1 - last > std::chrono::seconds(10))
            {
                std::printf("[files] %llu passes in 10 s, %.3f ms total\n", (unsigned long long)calls, double(ns) / 1e6);
                ns = calls = 0;
                last = t1;
            }
        }
    } timed{stats};
    for (size_t t = 0; t < sizeof(kIndexTables) / sizeof(kIndexTables[0]); ++t)
    {
        const uint32_t ptrAddr = kIndexTables[t];
        const uint32_t table = rd32(rdram, ptrAddr);
        if (table < 0x00100000u || table >= 0x02000000u)
            continue;
        const uint32_t n = rd32(rdram, table);
        if (n == 0 || n > kMaxEntries || table + 4 + n * 16 > 0x02000000u)
            continue;
        TablePass &pass = g_pass[t];
        if (passStillValid(rdram, pass, table, n))
            continue;
        {
            // Diagnostics: KH2_FILES_STATS=1 logs each full pass's reason.
            static const bool stats = std::getenv("KH2_FILES_STATS") != nullptr;
            if (stats)
                std::printf("[files] full pass, table %zu at %08x n %u (was %08x n %u, %zu written, %u stable)\n", t, table,
                            n, pass.table, pass.n, pass.written.size(), pass.stablePasses);
        }
        pass.table = table;
        pass.n = n;
        pass.written.clear();
        pass.stablePasses = 0;
        struct SampleAfter // fingerprint the table as this pass leaves it
        {
            const uint8_t *ram;
            TablePass &p;
            ~SampleAfter() { p.sample = sampleEntries(ram, p.table, p.n); }
        } sampleAfter{rdram, pass};
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
            pass.written.emplace_back(hit, &o);
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
