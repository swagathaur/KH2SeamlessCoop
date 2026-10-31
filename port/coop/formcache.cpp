#include "formcache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
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
// YS::CACHE_BUFF: the header list (head, tail).
constexpr uint32_t kHead = 0x0034DCA0, kTail = 0x0034DCA4;
// A header: +1 state (3: read in), +2 references, +4 file name, +0x28 group,
// +0x24 flags, +0x2C size, +0x30 data (where the layout puts it), +0x34 where
// it is now, +0x3C next.
constexpr uint32_t kHState = 0x01, kHRefs = 0x02, kHName = 0x04, kHGroup = 0x28, kHSize = 0x2C, kHData = 0x30,
                   kHNow = 0x34, kHNext = 0x3C, kHFlags = 0x24;
constexpr int32_t kOld = 10000; // groups past this are old files (CACHE_BUFF::Clear)
// The layout (FUN_00170670), the pass after it that moves files to their new
// place and frees what lies past the cache's top (FUN_001707D8), and the read
// request (FUN_00170400: CACHE_BUFF::ReadRequest(name, group, size) jumps
// there). Every address of each is hooked: a paused call goes on through the
// hook, which so sees it finish.
constexpr uint32_t kLayout = 0x00170670, kLayoutEnd = 0x001707D4;
constexpr uint32_t kSettle = 0x001707D8, kSettleEnd = 0x00170888;
constexpr uint32_t kRequest = 0x00170400, kRequestEnd = 0x00170514;
constexpr uint32_t kCacheFlush = 0x00170888; // YS::CACHE_BUFF::Flush(TASK*)
constexpr uint32_t kDriveTask = 0x00351184;  // YS::CHANGEFORM's task (our own drive or revert)

// Our room: the RAM past the 32MB the game knows of (the runtime keeps 64MB;
// patch 0052). Nothing of the game's is ever there.
constexpr uint32_t kOurs = 0x02000000u, kOursEnd = 0x04000000u;

// The batches: 4 of 4 groups (a form entry reads its model into the first,
// its weapons into the next ones: FUN_0016EB48(entry, group, 4, 7)).
constexpr int32_t kPoolFirst = 4980, kBatchGroups = 4, kBatches = 4;
constexpr int32_t kPoolEnd = kPoolFirst + kBatchGroups * kBatches;
bool isPool(int32_t g) { return g >= kPoolFirst && g < kPoolEnd; }
int32_t batchOf(int32_t g) { return kPoolFirst + (g - kPoolFirst) / kBatchGroups * kBatchGroups; }

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x03FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x03FFFFFFu), &v, sizeof(T)); }
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; } // (the game's: headers, names)
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
    bool failed = false; // never came in: the base look instead
    bool ready = false;  // all its files were in once
};
struct Block
{
    uint32_t addr = 0, size = 0;
};
std::mutex g_mutex;
std::map<int32_t, Batch> g_batches; // by first group (under g_mutex)
std::map<uint32_t, Block> g_placed; // header -> its place in our room (under g_mutex)
// Ours let go (old files now): their place stays taken until the game has
// moved them out of it (to where it lays old files) or freed them.
std::map<uint32_t, Block> g_leaving; // (under g_mutex)
// Ours out of sight for the whole of our own drive or revert: the new form
// player finds its files by name (the first in the list, ours, before the
// form group): it took the avatar's copies, and the avatar's teardown later
// took its motion's helpers with it (a null call in our form player's motion).
std::vector<uint32_t> g_hiddenForDrive; // (under g_mutex)
int64_t g_driveSeenMs = 0;

uint32_t align(uint32_t v) { return (v + 0x3Fu) & ~0x3Fu; }
std::vector<Block> taken()
{
    std::vector<Block> t;
    for (auto &[h, b] : g_placed)
        t.push_back(b);
    for (auto &[h, b] : g_leaving)
        t.push_back(b);
    std::sort(t.begin(), t.end(), [](const Block &a, const Block &b) { return a.addr < b.addr; });
    return t;
}
// The first place with `size` free in our room (0: none).
uint32_t findRoom(uint32_t size)
{
    uint32_t at = kOurs;
    for (const Block &b : taken())
    {
        if (b.addr + b.size <= at)
            continue;
        if (b.addr >= at + size)
            break;
        at = align(b.addr + b.size);
    }
    return at + size <= kOursEnd ? at : 0;
}

PS2Runtime::RecompiledFunction g_layout = nullptr, g_settle = nullptr, g_request = nullptr;

// A hooked guest function can be paused part way (the EE scheduler switches
// threads at its checkpoints): the original then returns to us with the pc
// where it is to go on, and finishes in later calls through the hook. What
// follows it is done once it has returned for good: pc at the caller's
// return address, sp as it came in.
struct Run
{
    bool active = false;
    uint32_t ra = 0, sp = 0;
};
bool finished(const R5900Context *ctx, const Run &run)
{
    return ctx->pc == run.ra && getRegU32(ctx, 29) == run.sp;
}

// Ours are out of the list while the game lays its files out (they would
// move its own, and be moved).
std::vector<uint32_t> g_unlinked;
void unlinkOurs(uint8_t *rdram)
{
    uint32_t prev = 0, h = rd<uint32_t>(rdram, kHead);
    bool any = false;
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
            any = true;
        }
        else
            prev = h;
        h = next;
    }
    if (any)
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
    g_unlinked.clear();
}

// Ours placed: where they were, or (new, or read again) in our room, a batch
// together. Ours let go are forgotten once moved out or freed.
void placeOurs(uint8_t *rdram)
{
    std::vector<uint32_t> listed;
    eachHeader(rdram, [&](uint32_t h) { listed.push_back(h); });
    auto isListed = [&](uint32_t h) { return std::find(listed.begin(), listed.end(), h) != listed.end(); };
    for (auto it = g_leaving.begin(); it != g_leaving.end();)
        it = !isListed(it->first) || rd<uint32_t>(rdram, it->first + kHNow) != it->second.addr ? g_leaving.erase(it)
                                                                                            : std::next(it);
    for (auto it = g_placed.begin(); it != g_placed.end();)
        it = !isListed(it->first) || !isPool(rd<int32_t>(rdram, it->first + kHGroup)) ? g_placed.erase(it) : std::next(it);
    std::map<int32_t, std::vector<uint32_t>> fresh; // batch -> headers
    for (uint32_t h : listed)
    {
        const int32_t g = rd<int32_t>(rdram, h + kHGroup);
        if (!isPool(g))
            continue;
        auto it = g_placed.find(h);
        const uint32_t size = rd<uint32_t>(rdram, h + kHSize), data = rd<uint32_t>(rdram, h + kHData);
        if (it != g_placed.end() && it->second.size == size && it->second.addr == data)
            continue;
        if (it != g_placed.end())
            g_placed.erase(it);
        fresh[batchOf(g)].push_back(h);
    }
    for (auto &[base, heads] : fresh)
    {
        uint32_t total = 0;
        for (uint32_t h : heads)
            total += align(rd<uint32_t>(rdram, h + kHSize));
        uint32_t at = findRoom(total);
        Batch *b = g_batches.count(base) ? &g_batches[base] : nullptr;
        std::printf("[formcache] batch %d (look %04X): %zu file(s), %X bytes at %08X\n", base, b ? b->entry : 0,
                    heads.size(), total, at);
        if (!at)
        {
            // (32MB full: should not be. Not worn; read at the start.)
            if (b)
                b->failed = true;
            at = kOurs;
        }
        for (uint32_t h : heads)
        {
            const uint32_t size = rd<uint32_t>(rdram, h + kHSize);
            wr<uint32_t>(rdram, h + kHData, at);
            g_placed[h] = Block{at, size};
            at += align(size);
        }
    }
    if (!fresh.empty())
        std::fflush(stdout);
}

Run g_layoutRun, g_settleRun;
void layoutHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kLayout)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_layoutRun.active)
            g_layoutRun = Run{true, getRegU32(ctx, 31), getRegU32(ctx, 29)};
        unlinkOurs(rdram);
    }
    kh2::callOriginal(g_layout, rdram, ctx, runtime);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_layoutRun.active && finished(ctx, g_layoutRun))
    {
        g_layoutRun.active = false;
        relinkOurs(rdram);
        placeOurs(rdram);
    }
}

// The settling pass (FUN_001707D8) frees every file that ends past the top
// the layout left (+0x34DCB0): all of ours. While it runs the top is moved to
// the end of our room (ours then go through it as files that stay put do);
// the game's own files past the real top are marked to be freed (+0x24 bit
// 1: the pass's second loop frees those, its first leaves them alone), as
// they would have been, not moved past it.
constexpr uint32_t kLaidTop = 0x0034DCB0;
uint32_t g_savedTop = 0;
void settleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kSettle)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_settleRun.active && (!g_placed.empty() || !g_leaving.empty()))
        {
            // Ours let go: freed by this pass (+0x24 bit 1), not moved into
            // the game's room first (their contents walked and set up again
            // there).
            for (auto &[h, b] : g_leaving)
                if (rd<uint32_t>(rdram, h + kHNow) == b.addr)
                    wr<uint32_t>(rdram, h + kHFlags, rd<uint32_t>(rdram, h + kHFlags) | 2u);
            g_settleRun = Run{true, getRegU32(ctx, 31), getRegU32(ctx, 29)};
            g_savedTop = rd<uint32_t>(rdram, kLaidTop);
            eachHeader(rdram, [&](uint32_t h) {
                const int32_t g = rd<int32_t>(rdram, h + kHGroup);
                if (g < 0 || isPool(g))
                    return;
                if (rd<uint32_t>(rdram, h + kHData) + rd<uint32_t>(rdram, h + kHSize) > g_savedTop)
                    wr<uint32_t>(rdram, h + kHFlags, rd<uint32_t>(rdram, h + kHFlags) | 2u);
            });
            wr<uint32_t>(rdram, kLaidTop, kOursEnd);
        }
    }
    kh2::callOriginal(g_settle, rdram, ctx, runtime);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_settleRun.active && finished(ctx, g_settleRun))
    {
        g_settleRun.active = false;
        if (rd<uint32_t>(rdram, kLaidTop) == kOursEnd)
            wr<uint32_t>(rdram, kLaidTop, g_savedTop);
    }
}

// Our batches and the game share no file: a read for one of our batches
// never takes a file the game has in another group (Sora's form files go
// when he reverts), nor an old copy (taken back as it was, not read again),
// nor a read of the game's one of ours (sharing it, the read starts over:
// the file read in again under the avatar using it). The other side's are
// out of sight while it looks (names changed until the request returns).
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
        eachHeader(rdram, [&](uint32_t h) {
            const int32_t g = rd<int32_t>(rdram, h + kHGroup);
            // (and another batch's: sharing one starts its read over)
            const bool other = forUs ? !isPool(g) || batchOf(g) != batchOf(group) : isPool(g);
            const bool already = std::find(g_hiddenForDrive.begin(), g_hiddenForDrive.end(), h) != g_hiddenForDrive.end();
            if (other && !already && std::strncmp(reinterpret_cast<const char *>(rdram + h + kHName), want, 36) == 0)
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

bool hookAll(PS2Runtime &runtime, uint32_t from, uint32_t to, PS2Runtime::RecompiledFunction hook,
             PS2Runtime::RecompiledFunction &original)
{
    original = runtime.lookupFunction(from);
    if (!original || !runtime.replaceFunction(from, hook))
    {
        original = nullptr;
        return false;
    }
    for (uint32_t a = from + 4; a < to; a += 4)
        runtime.replaceFunction(a, hook);
    return true;
}
} // namespace

bool install(PS2Runtime &runtime)
{
    if (const char *e = std::getenv("KH2_FORMCACHE"); e && *e == '0')
        return false; // (testing: the game's own cache alone)
    // Our room must be RAM of its own, not a mirror of the game's.
    if (PS2_RAM_SIZE < kOursEnd)
    {
        std::fprintf(stderr, "[formcache] the runtime has no RAM past the game's 32MB: form looks off\n");
        return false;
    }
    if (!hookAll(runtime, kLayout, kLayoutEnd, &layoutHook, g_layout) ||
        !hookAll(runtime, kSettle, kSettleEnd, &settleHook, g_settle) ||
        !hookAll(runtime, kRequest, kRequestEnd, &requestHook, g_request))
    {
        std::fprintf(stderr, "[formcache] could not hook the file cache: form looks off\n");
        g_layout = g_settle = g_request = nullptr;
        return false;
    }
    return true;
}

Look want(uint8_t *rdram, uint16_t entry)
{
    if (!g_layout || !g_settle || !g_request)
        return Look::NoRoom;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_layoutRun.active || !g_hiddenForDrive.empty())
        return Look::Loading; // (a layout paused part way, or our drive: ours are out of sight)
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
            const int32_t g = rd<int32_t>(rdram, h + kHGroup);
            if (isPool(g) && batchOf(g) == base)
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
            std::fflush(stdout);
            b.failed = true;
            return Look::NoRoom;
        }
        return Look::Loading;
    }
    // A free batch (none of its groups has files).
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
    if (!base)
    {
        static int logged = 0;
        if (logged++ < 10)
        {
            std::printf("[formcache] look %04X: no batch free\n", entry);
            std::fflush(stdout);
        }
        return Look::NoRoom;
    }
    Batch &b = g_batches[base];
    b.entry = entry;
    b.askedMs = b.wantedMs = b.usedMs = now;
    std::printf("[formcache] look %04X read into batch %d\n", entry, base);
    std::fflush(stdout);
    kh2puppets::requestFormLoad(entry, uint32_t(base));
    return Look::Loading;
}

void tick(uint8_t *rdram)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_hiddenForDrive.empty())
    {
        const int64_t t = nowMs();
        if (rd<uint32_t>(rdram, kDriveTask))
            g_driveSeenMs = t;
        else if (t - g_driveSeenMs > 1000 && !g_requestRun.active)
        {
            std::vector<uint32_t> listed;
            eachHeader(rdram, [&](uint32_t h) { listed.push_back(h); });
            for (uint32_t h : g_hiddenForDrive)
                if (std::find(listed.begin(), listed.end(), h) != listed.end())
                    rdram[h + kHName] ^= 0x80;
            g_hiddenForDrive.clear();
        }
    }
    if (g_batches.empty() || g_layoutRun.active)
        return;
    const int64_t now = nowMs();
    // What the objects here use: their model data (+0x8B0, +0x7AC) and motion
    // set (+0x140, the file's data).
    std::vector<uint32_t> used;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        for (uint32_t off : {0x8B0u, 0x7ACu, 0x140u})
            used.push_back(rd<uint32_t>(rdram, a.address + off));
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
            // (Held by more than its own read: someone has it.)
            inUse = inUse || rd<uint16_t>(rdram, h + kHRefs) > 1;
            const uint32_t d = rd<uint32_t>(rdram, h + kHData), s = rd<uint32_t>(rdram, h + kHSize);
            for (uint32_t u : used)
                inUse = inUse || (d && u >= d && u < d + s);
        });
        if (inUse || drive || now - b.wantedMs < 3000 || now - b.askedMs < 5000)
            b.usedMs = now;
        // Gone (a room change made them old files), or no one's for 15 s
        // (a form's effects linger after its object): old files.
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
                wr<int32_t>(rdram, h + kHGroup, rd<int32_t>(rdram, h + kHGroup) + kOld);
                if (auto p = g_placed.find(h); p != g_placed.end())
                {
                    g_leaving[h] = p->second;
                    g_placed.erase(p);
                }
            }
            it = g_batches.erase(it);
            continue;
        }
        ++it;
    }
}

void hideForDrive(uint8_t *rdram)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_driveSeenMs = nowMs();
    eachHeader(rdram, [&](uint32_t h) {
        if (isPool(rd<int32_t>(rdram, h + kHGroup)) &&
            std::find(g_hiddenForDrive.begin(), g_hiddenForDrive.end(), h) == g_hiddenForDrive.end())
        {
            rdram[h + kHName] ^= 0x80;
            g_hiddenForDrive.push_back(h);
        }
    });
}

void reset(uint8_t *)
{
    // (Let go by tick() once no object uses them.)
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[base, b] : g_batches)
        b.wantedMs = b.askedMs = 0;
}
} // namespace kh2formcache
