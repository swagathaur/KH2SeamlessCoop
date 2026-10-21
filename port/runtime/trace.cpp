// Call tracing for guest functions, for boot debugging from the harness.
//
// Each traced address gets a wrapper installed with replaceFunction. A
// RecompiledFunction receives no address, but on entry ctx->pc is the
// function's own start, which identifies which original to forward to.

#include "trace.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "ps2_runtime.h"
#include "hle_call.h"

namespace kh2trace
{
namespace
{
struct Traced
{
    PS2Runtime::RecompiledFunction original = nullptr;
    uint32_t dumpBytes = 0; // ADDR:N hex-dumps N bytes at a0 for each logged call
    std::atomic<uint64_t> calls{0};
    uint64_t reported = 0;
    std::mutex raMutex;
    std::unordered_map<uint32_t, uint64_t> recentRa; // callers since the last summary
};

std::unordered_map<uint32_t, Traced *> g_traced;
std::vector<uint32_t> g_order;
// How many calls per function are logged in full (KH2_TRACE_LOG_FIRST, default 8).
const uint64_t kLogFirst = [] {
    const char *e = std::getenv("KH2_TRACE_LOG_FIRST");
    const long long v = e ? std::atoll(e) : 0;
    return v > 0 ? static_cast<uint64_t>(v) : 8ull;
}();

void wrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t address = ctx->pc;
    auto it = g_traced.find(address);
    if (it == g_traced.end())
    {
        std::fprintf(stderr, "[trace] wrapper entered at unknown pc 0x%08X\n", address);
        return;
    }
    Traced &t = *it->second;
    const uint64_t n = t.calls.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(t.raMutex);
        ++t.recentRa[getRegU32(ctx, 31)];
    }
    if (n <= kLogFirst)
    {
        uint64_t a0, a1;
        std::memcpy(&a0, &ctx->r[4], 8);
        std::memcpy(&a1, &ctx->r[5], 8);
        std::printf("[trace] 0x%08X call #%llu a0=%016llX a1=%016llX a2=%08X a3=%08X ra=%08X f12=%g f13=%g\n",
                    address, (unsigned long long)n, (unsigned long long)a0, (unsigned long long)a1,
                    getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 31), ctx->f[12], ctx->f[13]);
    }
    if (n <= kLogFirst && t.dumpBytes)
    {
        const uint32_t base = getRegU32(ctx, 4);
        for (uint32_t off = 0; off < t.dumpBytes; off += 16)
        {
            std::printf("[trace]   a0+%03x:", off);
            for (uint32_t i = 0; i < 16 && off + i < t.dumpBytes; i += 4)
            {
                uint32_t w;
                std::memcpy(&w, rdram + ((base + off + i) & PS2_RAM_MASK), 4);
                std::printf(" %08X", w);
            }
            std::printf("\n");
        }
    }
    kh2::callOriginal(t.original, rdram, ctx, runtime);
    if (n <= kLogFirst)
        std::printf("[trace] 0x%08X call #%llu -> v0=%08X\n", address, (unsigned long long)n, getRegU32(ctx, 2));
}
} // namespace

void install(PS2Runtime &runtime, const std::string &spec)
{
    std::stringstream ss(spec);
    for (std::string item; std::getline(ss, item, ',');)
    {
        if (item.empty())
            continue;
        char *rest = nullptr;
        const uint32_t address = static_cast<uint32_t>(std::strtoul(item.c_str(), &rest, 0));
        const uint32_t dump = (rest && *rest == ':') ? static_cast<uint32_t>(std::strtoul(rest + 1, nullptr, 0)) : 0;
        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address);
        if (!runtime.hasFunction(address) || !runtime.replaceFunction(address, &wrapper))
        {
            std::fprintf(stderr, "[trace] 0x%08X is not a translated function start\n", address);
            continue;
        }
        auto *t = new Traced;
        t->original = original;
        t->dumpBytes = dump;
        g_traced[address] = t;
        g_order.push_back(address);
        std::printf("[trace] tracing 0x%08X\n", address);
    }
}

std::string summary()
{
    std::ostringstream out;
    for (uint32_t address : g_order)
    {
        Traced &t = *g_traced[address];
        const uint64_t now = t.calls.load();
        char buf[64];
        std::snprintf(buf, sizeof(buf), " %08X:%llu(+%llu)", address, (unsigned long long)now,
                      (unsigned long long)(now - t.reported));
        out << buf;
        t.reported = now;
        // The busiest callers since the last line.
        std::vector<std::pair<uint64_t, uint32_t>> callers;
        {
            std::lock_guard<std::mutex> lock(t.raMutex);
            for (const auto &[ra, count] : t.recentRa)
                callers.push_back({count, ra});
            t.recentRa.clear();
        }
        std::sort(callers.rbegin(), callers.rend());
        for (size_t i = 0; i < callers.size() && i < 4; ++i)
        {
            std::snprintf(buf, sizeof(buf), "%s%08X*%llu", i ? "," : "[", callers[i].second,
                          (unsigned long long)callers[i].first);
            out << buf;
        }
        if (!callers.empty())
            out << "]";
    }
    return out.str();
}
} // namespace kh2trace
