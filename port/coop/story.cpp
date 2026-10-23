#include "story.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2story
{
namespace
{
constexpr uint32_t kSave = 0x0032BB30u;    // the save data the game runs on (getSaveRam())
constexpr uint32_t kSaveSize = 0x10FC0u;
constexpr uint32_t kSaveLocation = 0x0Cu;  // world, room, spawn, (unused): where the save starts
constexpr uint32_t kMemcpy = 0x002FDD68u;  // memcpy(dst, src, n)

// The story: save offsets and sizes, concatenated into one block.
struct Range
{
    uint32_t offset, size;
};
constexpr Range kRanges[] = {
    {0x0010, 0x2440 - 0x0010}, // room programs, progress flags, rooms visited, SAVEFLAG flags
    {0x3534, 0x3580 - 0x3534}, // each world's party
};
constexpr uint32_t blockSize()
{
    uint32_t n = 0;
    for (const Range &r : kRanges)
        n += r.size;
    return n;
}
constexpr uint32_t kBlock = blockSize();

constexpr size_t kChunkData = 900; // run bytes per StoryChunk (packets stay under 1200)
constexpr uint8_t kChunkFull = 1, kChunkLast = 2;

PS2Runtime::RecompiledFunction g_memcpy = nullptr;
PS2Runtime::RecompiledFunction g_readBuff = nullptr; // diagnostics: KH2_EVENT_LOG=1


std::mutex g_mutex;
// Host.
std::vector<uint8_t> g_sent; // what the clients have
// Client.
std::vector<uint8_t> g_host(kBlock, 0); // the host's story as received
bool g_hostFull = false;                // all of it has arrived
bool g_active = false;                  // in use
std::vector<uint8_t> g_own;             // ours, put aside
uint8_t g_ownLocation[4] = {};
std::vector<std::pair<uint32_t, uint32_t>> g_pending; // block spans to write
Place g_return;
double g_lastSettle = 0;
std::vector<std::pair<uint32_t, uint8_t>> g_lastDiff; // (block offset, our value) at the last look

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint8_t *at(uint8_t *ram, uint32_t a) { return ram + (a & 0x01FFFFFFu); }

// sa::EVENT::readBuffAlloc(name, READ_WK*, type): an event's files being read.
void readBuffHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t name = getRegU32(ctx, 4);
    char text[64] = {};
    if (name >= 0x00100000u && name < 0x02000000u - 64)
        std::memcpy(text, at(rdram, name), sizeof(text) - 1);
    for (char &c : text)
        if (c && (c < 32 || c > 126))
            c = '?';
    std::printf("[story] event file %s (from %08X; s0 %08X s1 %08X s2 %08X)\n", text, getRegU32(ctx, 31),
                getRegU32(ctx, 16), getRegU32(ctx, 17), getRegU32(ctx, 18));
    std::fflush(stdout);
    kh2::callOriginal(g_readBuff, rdram, ctx, runtime);
}

void readBlock(const uint8_t *save, std::vector<uint8_t> &out)
{
    out.resize(kBlock);
    uint32_t o = 0;
    for (const Range &r : kRanges)
    {
        std::memcpy(out.data() + o, save + r.offset, r.size);
        o += r.size;
    }
}
// Write block bytes [from, from + len) to the save data at `save`.
void writeSpan(uint8_t *save, const std::vector<uint8_t> &block, uint32_t from, uint32_t len)
{
    uint32_t o = 0;
    for (const Range &r : kRanges)
    {
        const uint32_t a = from > o ? from : o;
        const uint32_t b = (from + len) < (o + r.size) ? (from + len) : (o + r.size);
        if (a < b)
            std::memcpy(save + r.offset + (a - o), block.data() + a, b - a);
        o += r.size;
    }
}
void writeBlock(uint8_t *save, const std::vector<uint8_t> &block) { writeSpan(save, block, 0, kBlock); }

struct Run
{
    uint32_t offset, size;
};
std::vector<std::vector<uint8_t>> pack(const std::vector<uint8_t> &block, const std::vector<Run> &runs, bool full)
{
    std::vector<std::vector<uint8_t>> out;
    std::vector<uint8_t> cur;
    auto flush = [&] {
        if (cur.size() > 1)
            out.push_back(cur);
        cur.clear();
    };
    for (Run r : runs)
        while (r.size)
        {
            if (cur.empty())
                cur.push_back(full ? kChunkFull : 0);
            const size_t room = kChunkData + 1 - cur.size();
            if (room < 8)
            {
                flush();
                continue;
            }
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(r.size, room - 4));
            cur.push_back(static_cast<uint8_t>(r.offset & 0xFF));
            cur.push_back(static_cast<uint8_t>(r.offset >> 8));
            cur.push_back(static_cast<uint8_t>(n & 0xFF));
            cur.push_back(static_cast<uint8_t>(n >> 8));
            cur.insert(cur.end(), block.begin() + r.offset, block.begin() + r.offset + n);
            r.offset += n;
            r.size -= n;
        }
    flush();
    if (full && !out.empty())
        out.back()[0] |= kChunkLast;
    return out;
}

// memcpy(dst, src, n) with the save data on one side: the save menu making
// the memory card image, or a game being loaded into it.
void memcpyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = getRegU32(ctx, 6);
    if (n == kSaveSize)
    {
        const uint32_t dst = getRegU32(ctx, 4), src = getRegU32(ctx, 5);
        const bool saving = (src & 0x1FFFFFFFu) == kSave, loading = (dst & 0x1FFFFFFFu) == kSave;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active && (saving || loading))
        {
            std::memmove(at(rdram, dst), at(rdram, src), n);
            if (saving)
            {
                // The card gets this player's own story, and their own place.
                writeBlock(at(rdram, dst), g_own);
                std::memcpy(at(rdram, dst + kSaveLocation), g_ownLocation, sizeof(g_ownLocation));
                std::printf("[story] saving: your own story and place go in the save\n");
            }
            else
            {
                // A game loaded while connected: that is now our own story.
                readBlock(at(rdram, kSave), g_own);
                std::memcpy(g_ownLocation, at(rdram, kSave + kSaveLocation), sizeof(g_ownLocation));
                writeBlock(at(rdram, kSave), g_host);
                std::printf("[story] a game was loaded: kept as your own story; the host's stays in use\n");
            }
            std::fflush(stdout);
            setReturnU32(ctx, dst);
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }
    kh2::callOriginal(g_memcpy, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_memcpy = runtime.lookupFunction(kMemcpy);
    if (!g_memcpy || !runtime.replaceFunction(kMemcpy, &memcpyHook))
    {
        std::fprintf(stderr, "[story] could not hook memcpy (0x%08X)\n", kMemcpy);
        return false;
    }
    const char *log = std::getenv("KH2_EVENT_LOG");
    if (log && *log == '1')
    {
        constexpr uint32_t kReadBuff = 0x00227870u;
        g_readBuff = runtime.lookupFunction(kReadBuff);
        if (g_readBuff)
            runtime.replaceFunction(kReadBuff, &readBuffHook);
    }
    return true;
}

std::vector<std::vector<uint8_t>> hostCollect(const uint8_t *rdram, bool full)
{
    std::vector<uint8_t> cur;
    readBlock(rdram + kSave, cur);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Run> runs;
    if (full || g_sent.size() != kBlock)
    {
        runs.push_back({0, kBlock});
        full = true;
    }
    else
    {
        // Changed bytes, runs closer than 8 bytes merged.
        uint32_t i = 0;
        while (i < kBlock)
        {
            if (cur[i] == g_sent[i])
            {
                ++i;
                continue;
            }
            uint32_t end = i + 1, same = 0;
            for (uint32_t j = end; j < kBlock && same < 8; ++j)
            {
                if (cur[j] != g_sent[j])
                {
                    end = j + 1;
                    same = 0;
                }
                else
                    ++same;
            }
            runs.push_back({i, end - i});
            i = end;
        }
    }
    g_sent = cur;
    if (runs.empty())
        return {};
    return pack(cur, runs, full);
}

void onChunk(const std::vector<uint8_t> &data)
{
    if (data.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint8_t flags = data[0];
    size_t p = 1;
    while (p + 4 <= data.size())
    {
        const uint32_t off = data[p] | (data[p + 1] << 8), len = data[p + 2] | (data[p + 3] << 8);
        p += 4;
        if (p + len > data.size() || off + len > kBlock)
            return;
        std::memcpy(g_host.data() + off, data.data() + p, len);
        p += len;
        if (g_active)
            g_pending.emplace_back(off, len);
    }
    if ((flags & kChunkFull) && (flags & kChunkLast))
        g_hostFull = true;
}

bool clientTick(uint8_t *rdram, bool settle, bool mayStart)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    uint8_t *save = rdram + kSave;
    if (!g_active)
    {
        if (!g_hostFull || !mayStart)
            return false;
        readBlock(save, g_own);
        std::memcpy(g_ownLocation, save + kSaveLocation, sizeof(g_ownLocation));
        writeBlock(save, g_host);
        g_active = true;
        g_pending.clear();
        g_lastSettle = now();
        std::printf("[story] playing in the host's story (yours is kept for when you leave)\n");
        std::fflush(stdout);
        return true;
    }
    for (const auto &[off, len] : g_pending)
        writeSpan(save, g_host, off, len);
    g_pending.clear();
    // Our own game may have changed the story where the host's did not (an
    // event it ran first, say): the host's wins, once the player is free.
    // Only what has stayed different for a while: both games run the same
    // events, and ours may set a flag a moment before the host's news of it
    // arrives (undoing it then could start the event again).
    if (!settle)
        g_lastDiff.clear();
    else if (now() - g_lastSettle > 1.5)
    {
        g_lastSettle = now();
        std::vector<uint8_t> cur;
        readBlock(save, cur);
        std::vector<std::pair<uint32_t, uint8_t>> diff;
        uint32_t changed = 0;
        size_t k = 0;
        for (uint32_t i = 0; i < kBlock; ++i)
        {
            if (cur[i] == g_host[i])
                continue;
            diff.emplace_back(i, cur[i]);
            while (k < g_lastDiff.size() && g_lastDiff[k].first < i)
                ++k;
            if (k < g_lastDiff.size() && g_lastDiff[k].first == i && g_lastDiff[k].second == cur[i])
            {
                writeSpan(save, g_host, i, 1);
                ++changed;
            }
        }
        g_lastDiff = std::move(diff);
        if (changed)
        {
            std::printf("[story] %u byte(s) of story put back to the host's\n", changed);
            std::fflush(stdout);
        }
    }
    return false;
}

bool active()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active;
}

void testSave()
{
    // The save menu's step that makes the memory card image (FUN_00240A70),
    // into a buffer from the room's heap (AREA::Alloc; the game's own image
    // buffer exists only in the save menu), then a look at what it holds.
    constexpr uint32_t kAreaAlloc = 0x001050D8u, kMakeImage = 0x00240A70u;
    auto image = std::make_shared<uint32_t>(0);
    auto step = std::make_shared<int>(0);
    kh2guest::post([image, step](const kh2guest::Env &env, uint32_t last, bool) -> std::optional<kh2guest::Call> {
        switch ((*step)++)
        {
        case 0:
            return kh2guest::Call{kAreaAlloc, {kSaveSize}, {}};
        case 1:
            *image = last;
            if (*image < 0x00100000u || *image + kSaveSize > 0x02000000u)
            {
                std::printf("[story] save test: no buffer (%08X)\n", *image);
                std::fflush(stdout);
                return std::nullopt;
            }
            return kh2guest::Call{kMakeImage, {*image}, {}};
        default:
            break;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<uint8_t> got, live;
        readBlock(env.host(*image), got);
        readBlock(env.rdram + kSave, live);
        const bool own = g_active && got == g_own;
        const bool running = got == live;
        std::printf("[story] save test: the image holds %s (own story %s, the running story %s, place %02X-%02X)\n",
                    own ? "your own story" : running ? "the running story" : "neither", own ? "yes" : "no",
                    running ? "yes" : "no", env.host(*image)[kSaveLocation], env.host(*image)[kSaveLocation + 1]);
        std::fflush(stdout);
        return std::nullopt;
    });
}

void setReturnPlace(const Place &p)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_return = p;
}

bool end(uint8_t *rdram, Place &back)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool was = g_active;
    if (g_active)
    {
        writeBlock(rdram + kSave, g_own);
        std::printf("[story] back to your own story\n");
        std::fflush(stdout);
    }
    g_active = false;
    g_hostFull = false;
    g_pending.clear();
    g_lastDiff.clear();
    g_sent.clear();
    back = g_return;
    return was;
}
} // namespace kh2story
