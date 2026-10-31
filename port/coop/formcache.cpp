#include "formcache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <mutex>
#include <vector>

#include "game_state.h"
#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "puppets.h"

namespace kh2formcache
{
namespace
{
// YS::CACHE_BUFF: the header list (head, tail), the top of its memory, the
// top left once the walk placed the negative groups (from the top down).
constexpr uint32_t kHead = 0x0034DCA0, kTail = 0x0034DCA4, kTopAfterNegatives = 0x0034DCB0;
// The fixed regions' sizes (u32 per kind of group, FUN_00170D38), and where
// the walk starts.
constexpr uint32_t kRegionSizes = 0x01C5F460, kCacheStart = 0x004FA440;
// A header: +1 state (3: read in), +2 references, +4 file name, +0x28 group,
// +0x2C size, +0x30 data, +0x3C next.
constexpr uint32_t kHState = 0x01, kHName = 0x04, kHGroup = 0x28, kHSize = 0x2C, kHData = 0x30, kHNext = 0x3C;
constexpr int32_t kOld = 10000; // groups past this are old files (CACHE_BUFF::Clear)
// The walk (FUN_00170670), and the read request (FUN_00170400:
// CACHE_BUFF::ReadRequest(name, group, size) jumps there).
constexpr uint32_t kLayout = 0x00170670, kRequest = 0x00170400;
constexpr uint32_t kLayoutEnd = 0x001707D4, kRequestEnd = 0x00170514;
constexpr uint32_t kCacheFlush = 0x00170888; // YS::CACHE_BUFF::Flush(TASK*)
constexpr uint32_t kDriveTask = 0x00351184; // YS::CHANGEFORM's task (our own drive or revert)

// The batches: 4 of 4 groups (a form entry reads its model into the first,
// its weapons into the next ones: FUN_0016EB48(entry, group, 4, 7)).
constexpr int32_t kPoolFirst = 4980, kBatchGroups = 4, kBatches = 4;
constexpr int32_t kPoolEnd = kPoolFirst + kBatchGroups * kBatches;
bool isPool(int32_t g) { return g >= kPoolFirst && g < kPoolEnd; }
int32_t batchOf(int32_t g) { return kPoolFirst + (g - kPoolFirst) / kBatchGroups * kBatchGroups; }

// Room kept free under the negative groups (the room's spawn reads, -1, go
// on from the top down), and the room a form look needs to be read at all.
constexpr uint32_t kBelowNegatives = 0x80000, kLookNeeds = 0x280000;

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }
int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
std::string nameOf(const uint8_t *ram, uint32_t h)
{
    char n[37] = {};
    std::memcpy(n, ram + ((h + kHName) & 0x01FFFFFFu), 36);
    return n;
}
template <typename F> void eachHeader(const uint8_t *ram, F f)
{
    uint32_t h = rd<uint32_t>(ram, kHead);
    for (int i = 0; i < 2048 && inRam(h); ++i)
    {
        const uint32_t next = rd<uint32_t>(ram, h + kHNext);
        f(h);
        h = next;
    }
}

struct Batch
{
    uint16_t entry = 0;
    int64_t askedMs = 0, wantedMs = 0, usedMs = 0, flushedMs = 0;
    bool failed = false; // no room: the base look instead
    bool ready = false;  // all its files were in once
};
struct Block
{
    uint32_t addr = 0, size = 0;
};
std::mutex g_mutex;
std::map<int32_t, Batch> g_batches;   // by first group (under g_mutex)
std::map<uint32_t, Block> g_placed;   // header -> where we put it (under g_mutex)
std::vector<uint32_t> g_letGo;        // ours let go, until the next layout frees them (under g_mutex)

// The kind of region a group's files go to (FUN_00170D38): 0 packed behind
// the ones before, else a fixed region of its own.
int regionOf(const uint8_t *ram, int32_t g)
{
    if (g == 0x3F2)
        return 1;
    if (g >= 0x8FC && g < 0x8FC + 100)
        return 2;
    if (g >= 0x96A && g < 0x96A + 10)
        return 3;
    if (g >= 0x974 && g < 0x974 + 10)
        return 4;
    if (g >= 0x1388 && g < 0x1388 + 100)
        return 5;
    if (g >= 0x13EC && g < 0x13EC + 100)
        return rd<int32_t>(ram, kRegionSizes + 6 * 4) > 0 ? 6 : 0;
    return 0;
}

// Where the game's own files in use end, at most (the walk again, without
// our batches; the form regions counted as if there: Sora may drive).
uint32_t gameEnd(const uint8_t *ram)
{
    uint32_t cursor = kCacheStart, slots[7] = {};
    eachHeader(ram, [&](uint32_t h) {
        const int32_t g = rd<int32_t>(ram, h + kHGroup);
        if (g < 0 || g >= kOld || isPool(g))
            return;
        const uint32_t size = rd<uint32_t>(ram, h + kHSize);
        const int r = regionOf(ram, g);
        if (r == 0)
        {
            cursor += size;
            return;
        }
        if (!slots[r])
        {
            slots[r] = cursor;
            cursor += rd<uint32_t>(ram, kRegionSizes + r * 4);
        }
        slots[r] += size;
        cursor = std::max(cursor, slots[r]);
    });
    for (int r : {5, 6})
        if (!slots[r])
            if (const int32_t size = rd<int32_t>(ram, kRegionSizes + r * 4); size > 0)
                cursor += uint32_t(size);
    return cursor;
}
uint32_t freeTop(const uint8_t *ram)
{
    const uint32_t top = rd<uint32_t>(ram, kTopAfterNegatives);
    return top > kBelowNegatives ? top - kBelowNegatives : 0;
}
uint32_t align(uint32_t v) { return (v + 0x3Fu) & ~0x3Fu; }

// The first place in [lo, hi) with `size` free of our blocks.
uint32_t findRoom(uint32_t lo, uint32_t hi, uint32_t size)
{
    std::vector<Block> taken;
    for (auto &[h, b] : g_placed)
        if (std::find(g_letGo.begin(), g_letGo.end(), h) == g_letGo.end()) // (let go: free at the next layout)
            taken.push_back(b);
    std::sort(taken.begin(), taken.end(), [](const Block &a, const Block &b) { return a.addr < b.addr; });
    uint32_t at = align(lo);
    for (const Block &b : taken)
    {
        if (b.addr + b.size <= at)
            continue;
        if (b.addr >= at + size)
            break;
        at = align(b.addr + b.size);
    }
    return at + size <= hi ? at : 0;
}
uint32_t largestRoom(uint32_t lo, uint32_t hi)
{
    std::vector<Block> taken;
    for (auto &[h, b] : g_placed)
        if (std::find(g_letGo.begin(), g_letGo.end(), h) == g_letGo.end()) // (let go: free at the next layout)
            taken.push_back(b);
    std::sort(taken.begin(), taken.end(), [](const Block &a, const Block &b) { return a.addr < b.addr; });
    uint32_t at = align(lo), best = 0;
    for (const Block &b : taken)
    {
        if (b.addr > at && b.addr <= hi)
            best = std::max(best, b.addr - at);
        at = std::max(at, align(b.addr + b.size));
    }
    if (hi > at)
        best = std::max(best, hi - at);
    return best;
}

// A file let go: an old file (as CACHE_BUFF::Clear makes them, FUN_00170018).
void age(uint8_t *ram, uint32_t h)
{
    const int32_t g = rd<int32_t>(ram, h + kHGroup);
    if (g < kOld)
        wr<int32_t>(ram, h + kHGroup, g + kOld);
    g_letGo.push_back(h);
}

PS2Runtime::RecompiledFunction g_layout = nullptr, g_request = nullptr;

// A hooked guest function can be paused part way (the EE scheduler switches
// threads at its checkpoints): the original then returns to us with the pc
// where it is to go on, and finishes in later calls (through the hook again,
// with that pc). What follows the original is done once it has returned for
// good: pc back at the caller's return address, sp as it came in. (Taking
// the pc over before then sent the paused layout back to its caller with
// its stack frame still on: our player's revert ended the game's main thread,
// a return to address 0.)
struct Run
{
    bool active = false;
    uint32_t ra = 0, sp = 0;
};
bool finished(const R5900Context *ctx, const Run &run)
{
    return ctx->pc == run.ra && getRegU32(ctx, 29) == run.sp;
}

// The walk under way: our headers out of the list meanwhile.
Run g_layoutRun;
std::vector<uint32_t> g_unlinked;

void unlinkOurs(uint8_t *rdram)
{
    uint32_t prev = 0, h = rd<uint32_t>(rdram, kHead);
    for (int i = 0; i < 2048 && inRam(h); ++i)
    {
        const uint32_t next = rd<uint32_t>(rdram, h + kHNext);
        if (isPool(rd<int32_t>(rdram, h + kHGroup)))
        {
            if (prev)
                wr<uint32_t>(rdram, prev + kHNext, next);
            else
                wr<uint32_t>(rdram, kHead, next);
            wr<uint32_t>(rdram, h + kHNext, 0);
            g_unlinked.push_back(h);
        }
        else
            prev = h;
        h = next;
    }
    wr<uint32_t>(rdram, kTail, prev);
}
// Back in, where the game's own sorted insert (FUN_00170188) puts them.
void relinkOurs(uint8_t *rdram)
{
    for (uint32_t p : g_unlinked)
    {
        const int32_t g = rd<int32_t>(rdram, p + kHGroup);
        uint32_t prev = 0, cur = rd<uint32_t>(rdram, kHead);
        for (int i = 0; i < 2048 && inRam(cur) && !(g < rd<int32_t>(rdram, cur + kHGroup)); ++i)
        {
            prev = cur;
            cur = rd<uint32_t>(rdram, cur + kHNext);
        }
        if (!inRam(cur))
            cur = 0;
        wr<uint32_t>(rdram, p + kHNext, cur);
        if (prev)
            wr<uint32_t>(rdram, prev + kHNext, p);
        else
            wr<uint32_t>(rdram, kHead, p);
        if (!cur)
            wr<uint32_t>(rdram, kTail, p);
    }
}

void placeFresh(uint8_t *rdram, std::map<int32_t, std::vector<uint32_t>> &fresh);
void checkClash(uint8_t *rdram);

// Ours placed: where they were, or (new) in the free room above the game's
// files, a batch together.
void placeOurs(uint8_t *rdram)
{
    const std::vector<uint32_t> &ours = g_unlinked;
    for (auto it = g_placed.begin(); it != g_placed.end();)
        it = std::find(ours.begin(), ours.end(), it->first) == ours.end() ? g_placed.erase(it) : std::next(it);
    std::map<int32_t, std::vector<uint32_t>> fresh; // batch -> headers
    for (uint32_t h : ours)
    {
        const int32_t g = rd<int32_t>(rdram, h + kHGroup);
        if (!isPool(g))
            continue; // (let go meanwhile: the walk places it next time)
        auto it = g_placed.find(h);
        const uint32_t size = rd<uint32_t>(rdram, h + kHSize), data = rd<uint32_t>(rdram, h + kHData);
        if (it != g_placed.end() && it->second.size == size && it->second.addr == data)
            continue;
        if (it != g_placed.end())
            g_placed.erase(it);
        fresh[batchOf(g)].push_back(h);
    }
    if (!fresh.empty())
        placeFresh(rdram, fresh);
    checkClash(rdram);
}

void placeFresh(uint8_t *rdram, std::map<int32_t, std::vector<uint32_t>> &fresh)
{
    const uint32_t lo = gameEnd(rdram), hi = freeTop(rdram), top = rd<uint32_t>(rdram, kTopAfterNegatives);
    for (auto &[base, heads] : fresh)
    {
        uint32_t total = 0;
        for (uint32_t h : heads)
            total += align(rd<uint32_t>(rdram, h + kHSize));
        uint32_t at = findRoom(lo, hi, total);
        Batch *b = g_batches.count(base) ? &g_batches[base] : nullptr;
        std::printf("[formcache] batch %d (look %04X): %zu file(s), %X bytes at %08X (free room %08X-%08X)\n", base,
                    b ? b->entry : 0, heads.size(), total, at, lo, hi);
        if (!at)
        {
            // No room kept free for it: read where the old files are (no
            // one uses those), and not worn (the base look instead); let go
            // once no one wants it.
            if (b)
                b->failed = true;
            at = findRoom(lo, top, total);
            if (!at)
                at = align(lo);
            std::printf("[formcache]   no room: read at %08X, not worn\n", at);
        }
        for (uint32_t h : heads)
        {
            const uint32_t size = rd<uint32_t>(rdram, h + kHSize);
            wr<uint32_t>(rdram, h + kHData, at);
            g_placed[h] = Block{at, size};
            std::printf("[formcache]   %s at %08X (%X)\n", nameOf(rdram, h).c_str(), at, size);
            at += align(size);
        }
    }
    std::fflush(stdout);
}

// (A game file in use laid over one of ours: should not be.)
void checkClash(uint8_t *rdram)
{
    static int logged = 0;
    eachHeader(rdram, [&](uint32_t h) {
        const int32_t g = rd<int32_t>(rdram, h + kHGroup);
        if (g >= kOld || isPool(g) || logged >= 20)
            return;
        const uint32_t d = rd<uint32_t>(rdram, h + kHData), s = rd<uint32_t>(rdram, h + kHSize);
        for (auto &[p, b] : g_placed)
            if (d < b.addr + b.size && b.addr < d + s)
            {
                ++logged;
                std::printf("[formcache] %s (group %d, %08X+%X) lies over our %s\n", nameOf(rdram, h).c_str(), g, d, s,
                            nameOf(rdram, p).c_str());
                std::fflush(stdout);
                break;
            }
    });
}

// Old files (groups past 10000) are laid out after the game's own, and MOVED
// there (FUN_0016FC10 after each layout: the contents walked at the old
// place, copied, set up again at the new one): over ours, which are not in
// the walk. While we have files, one of ours stands in the walk for all of
// them, after the game's own and as big as the rest of the cache: the old
// files then lie past the top, and the walk frees them as it frees old files
// it has no room for. (Ours let go go the same way, unmoved.)
struct StandIn
{
    uint32_t header = 0, size = 0;
    int32_t group = 0;
};
StandIn g_standIn;
constexpr int32_t kStandInGroup = 9999;
void insertSorted(uint8_t *rdram, uint32_t p)
{
    const int32_t g = rd<int32_t>(rdram, p + kHGroup);
    uint32_t prev = 0, cur = rd<uint32_t>(rdram, kHead);
    for (int i = 0; i < 2048 && inRam(cur) && !(g < rd<int32_t>(rdram, cur + kHGroup)); ++i)
    {
        prev = cur;
        cur = rd<uint32_t>(rdram, cur + kHNext);
    }
    if (!inRam(cur))
        cur = 0;
    wr<uint32_t>(rdram, p + kHNext, cur);
    if (prev)
        wr<uint32_t>(rdram, prev + kHNext, p);
    else
        wr<uint32_t>(rdram, kHead, p);
    if (!cur)
        wr<uint32_t>(rdram, kTail, p);
}
void removeFromList(uint8_t *rdram, uint32_t p)
{
    uint32_t prev = 0, cur = rd<uint32_t>(rdram, kHead);
    for (int i = 0; i < 2048 && inRam(cur); ++i)
    {
        const uint32_t next = rd<uint32_t>(rdram, cur + kHNext);
        if (cur == p)
        {
            if (prev)
                wr<uint32_t>(rdram, prev + kHNext, next);
            else
                wr<uint32_t>(rdram, kHead, next);
            if (!next)
                wr<uint32_t>(rdram, kTail, prev);
            wr<uint32_t>(rdram, p + kHNext, 0);
            return;
        }
        prev = cur;
        cur = next;
    }
}
void standInBegin(uint8_t *rdram)
{
    if (g_standIn.header || g_unlinked.empty())
        return;
    const uint32_t h = g_unlinked.front();
    g_standIn = StandIn{h, rd<uint32_t>(rdram, h + kHSize), rd<int32_t>(rdram, h + kHGroup)};
    const uint32_t top = rd<uint32_t>(rdram, 0x0034DC9C); // (the cache's top)
    wr<int32_t>(rdram, h + kHGroup, kStandInGroup);
    wr<uint32_t>(rdram, h + kHSize, top > kCacheStart ? top - kCacheStart : 0x01000000u);
    insertSorted(rdram, h);
}
void standInEnd(uint8_t *rdram)
{
    if (!g_standIn.header)
        return;
    const uint32_t h = g_standIn.header;
    removeFromList(rdram, h);
    wr<int32_t>(rdram, h + kHGroup, g_standIn.group);
    wr<uint32_t>(rdram, h + kHSize, g_standIn.size);
    auto it = g_placed.find(h);
    wr<uint32_t>(rdram, h + kHData, it != g_placed.end() ? it->second.addr : 0u);
    g_standIn = StandIn{};
}

void layoutHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kLayout)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_layoutRun.active)
            g_layoutRun = Run{true, getRegU32(ctx, 31), getRegU32(ctx, 29)};
        unlinkOurs(rdram);
        standInBegin(rdram);
    }
    kh2::callOriginal(g_layout, rdram, ctx, runtime);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_layoutRun.active && finished(ctx, g_layoutRun))
    {
        g_layoutRun.active = false;
        standInEnd(rdram);
        relinkOurs(rdram);
        for (uint32_t h : g_letGo) // (freed by the walk, past the top)
            g_placed.erase(h);
        g_letGo.clear();
        placeOurs(rdram);
        g_unlinked.clear();
    }
}

// Our batches and the game share no file: a read for one of our batches
// never takes a file the game has in another group (Sora's form files go
// when he reverts), nor a read of the game's one of ours (sharing it, the
// read starts over: the file read in again under the avatar using it).
// The other side's are out of sight while it looks (names changed until
// the read request has returned).
Run g_requestRun;
std::vector<uint32_t> g_hidden;
void requestHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t name = getRegU32(ctx, 4);
    if (ctx->pc == kRequest && inRam(name) && !g_requestRun.active)
    {
        const int32_t group = int32_t(getRegU32(ctx, 5));
        const bool forUs = isPool(group);
        char want[40] = {};
        std::memcpy(want, rdram + (name & 0x01FFFFFFu), 36);
        if (!forUs && group >= 0x1388 && group < 0x1388 + 200 && !rd<uint32_t>(rdram, kDriveTask))
        {
            static int logged = 0;
            if (logged++ < 40)
            {
                std::printf("[formcache] a read of %s into form group %d not by our drive (from %08X)\n", want, group,
                            getRegU32(ctx, 31));
                std::fflush(stdout);
            }
        }
        eachHeader(rdram, [&](uint32_t h) {
            const int32_t g = rd<int32_t>(rdram, h + kHGroup);
            // (For ours: also an old copy, which the read would take back
            // into our group as it was, not read again.)
            const bool other = forUs ? !isPool(g) : isPool(g);
            if (other && std::strncmp(reinterpret_cast<const char *>(rdram + h + kHName), want, 36) == 0)
            {
                rdram[h + kHName] ^= 0x80;
                g_hidden.push_back(h);
            }
        });
        g_requestRun = Run{true, getRegU32(ctx, 31), getRegU32(ctx, 29)};
    }
    kh2::callOriginal(g_request, rdram, ctx, runtime);
    if (g_requestRun.active && finished(ctx, g_requestRun))
    {
        g_requestRun.active = false;
        for (uint32_t h : g_hidden)
            rdram[h + kHName] ^= 0x80;
        g_hidden.clear();
    }
}
} // namespace

bool install(PS2Runtime &runtime)
{
    if (const char *e = std::getenv("KH2_FORMCACHE"); e && *e == '0')
        return false; // (testing: the game's own cache layout alone)
    // Every address of the two functions goes through the hook (a paused
    // call goes on from where it stopped: we see it finish).
    g_layout = runtime.lookupFunction(kLayout);
    if (!g_layout || !runtime.replaceFunction(kLayout, &layoutHook))
    {
        g_layout = nullptr;
        return false;
    }
    for (uint32_t a = kLayout + 4; a < kLayoutEnd; a += 4)
        runtime.replaceFunction(a, &layoutHook);
    g_request = runtime.lookupFunction(kRequest);
    if (!g_request || !runtime.replaceFunction(kRequest, &requestHook))
        g_request = nullptr;
    else
        for (uint32_t a = kRequest + 4; a < kRequestEnd; a += 4)
            runtime.replaceFunction(a, &requestHook);
    return true;
}

Look want(uint8_t *rdram, uint16_t entry)
{
    if (!g_layout || !g_request)
        return Look::NoRoom;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_layoutRun.active)
        return Look::Loading; // (a walk paused part way: ours are out of the list)
    const int64_t now = nowMs();
    for (auto &[base, b] : g_batches)
    {
        if (b.entry != entry)
            continue;
        b.wantedMs = now;
        if (b.failed)
            return Look::NoRoom;
        int files = 0, in = 0;
        eachHeader(rdram, [&](uint32_t h) {
            if (batchOf(rd<int32_t>(rdram, h + kHGroup)) == base && isPool(rd<int32_t>(rdram, h + kHGroup)))
            {
                ++files;
                in += rdram[h + kHState] == 3 && g_placed.count(h) ? 1 : 0;
            }
        });
        if (b.ready || (files && in == files && now - b.askedMs > 300))
        {
            b.ready = true;
            return Look::Ready;
        }
        // (The reads go on with each CACHE_BUFF::Flush: the game's own come
        // with its room loads, so ours are pushed on while we wait.)
        if (now - b.flushedMs > 400)
        {
            b.flushedMs = now;
            kh2guest::postCall(kh2guest::Call{kCacheFlush, {0u}, {}});
        }
        if (now - b.askedMs > 15000)
        {
            std::printf("[formcache] look %04X: %d of %d file(s) in after 15 s: given up here\n", entry, in, files);
            eachHeader(rdram, [&](uint32_t h) {
                if (batchOf(rd<int32_t>(rdram, h + kHGroup)) == base && isPool(rd<int32_t>(rdram, h + kHGroup)))
                    std::printf("[formcache]   %s: state %u, %s\n", nameOf(rdram, h).c_str(), rdram[h + kHState],
                                g_placed.count(h) ? "placed" : "not placed");
            });
            std::fflush(stdout);
            b.failed = true;
            return Look::NoRoom;
        }
        return Look::Loading;
    }
    // A free batch (none of its groups has files), and room for a look.
    int32_t base = 0;
    for (int32_t k = 0; k < kBatches && !base; ++k)
    {
        const int32_t b = kPoolFirst + k * kBatchGroups;
        bool used = g_batches.count(b) != 0;
        eachHeader(rdram, [&](uint32_t h) {
            const int32_t g = rd<int32_t>(rdram, h + kHGroup);
            used = used || (isPool(g) && batchOf(g) == b);
        });
        if (!used)
            base = b;
    }
    const uint32_t room = largestRoom(gameEnd(rdram), freeTop(rdram));
    if (!base || room < kLookNeeds)
    {
        static int logged = 0;
        if (logged++ < 10)
        {
            std::printf("[formcache] look %04X: %s (largest free room %X)\n", entry, base ? "no room here" : "no batch free",
                        room);
            std::fflush(stdout);
        }
        return Look::NoRoom;
    }
    Batch &b = g_batches[base];
    b.entry = entry;
    b.askedMs = b.wantedMs = b.usedMs = now;
    std::printf("[formcache] look %04X read into batch %d (largest free room %X)\n", entry, base, room);
    std::fflush(stdout);
    kh2puppets::requestFormLoad(entry, uint32_t(base));
    return Look::Loading;
}

void tick(uint8_t *rdram)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const int64_t now = nowMs();
    static int64_t lastSaid = 0;
    const bool say = !g_batches.empty() && now - lastSaid > 5000;
    if (say)
        lastSaid = now;
    if (say && g_layoutRun.active)
        std::printf("[formcache] (a layout is still under way, %zu of ours out of the list)\n", g_unlinked.size());
    if (g_batches.empty() || g_layoutRun.active)
        return;
    // What the objects here use: their model data (+0x8B0, +0x7AC) and motion
    // set (+0x140, the file's data).
    std::vector<uint32_t> used, usedBy;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        used.push_back(rd<uint32_t>(rdram, a.address + 0x8B0));
        used.push_back(rd<uint32_t>(rdram, a.address + 0x7AC));
        used.push_back(rd<uint32_t>(rdram, a.address + 0x140)); // (its motion set's data)
        for (int k = 0; k < 3; ++k)
            usedBy.push_back(a.address);
    }
    const bool drive = rd<uint32_t>(rdram, kDriveTask) != 0;
    for (auto it = g_batches.begin(); it != g_batches.end();)
    {
        const int32_t base = it->first;
        Batch &b = it->second;
        std::vector<uint32_t> heads;
        bool inUse = false;
        eachHeader(rdram, [&](uint32_t h) {
            const int32_t g = rd<int32_t>(rdram, h + kHGroup);
            if (!isPool(g) || batchOf(g) != base)
                return;
            heads.push_back(h);
            // (Still referenced by more than its read (the read's own one stays,
            // as with Sora's form files): someone holds it.)
            if (rd<uint16_t>(rdram, h + 2) > 1)
            {
                if (say && !inUse)
                    std::printf("[formcache]   %s still has %u reference(s)\n", nameOf(rdram, h).c_str(),
                                unsigned(rd<uint16_t>(rdram, h + 2)));
                inUse = true;
            }
            const uint32_t d = rd<uint32_t>(rdram, h + kHData), s = rd<uint32_t>(rdram, h + kHSize);
            for (size_t k = 0; k < used.size(); ++k)
                if (d && used[k] >= d && used[k] < d + s)
                {
                    if (say && !inUse)
                        std::printf("[formcache]   %s used by %08X (field %zu)\n", nameOf(rdram, h).c_str(), usedBy[k], k % 3);
                    inUse = true;
                }
        });
        if (inUse || drive || now - b.wantedMs < 3000 || now - b.askedMs < 5000)
            b.usedMs = now;
        if (say)
            std::printf("[formcache] batch %d (look %04X): %zu file(s), in use %d, drive %d, wanted %lld ms ago\n", base,
                        b.entry, heads.size(), inUse, drive, (long long)(now - b.wantedMs));
        // Gone (a room change made them old), or no one's for 15 s (effects of a form linger after its object): old files.
        if (heads.empty() && now - b.askedMs > 5000)
        {
            it = g_batches.erase(it);
            continue;
        }
        if (now - b.usedMs > 15000)
        {
            std::printf("[formcache] look %04X (batch %d) no longer used: %zu file(s) let go\n", b.entry, base, heads.size());
            std::fflush(stdout);
            for (uint32_t h : heads)
            {
                age(rdram, h); // (its place is let go once it is an old file)
            }
            it = g_batches.erase(it);
            continue;
        }
        ++it;
    }
}

void reset(uint8_t *)
{
    // (Let go by tick() once no object uses them: our own form may.)
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[base, b] : g_batches)
        b.wantedMs = b.askedMs = 0;
}
} // namespace kh2formcache
