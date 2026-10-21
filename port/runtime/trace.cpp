// Call tracing for guest functions, for boot debugging from the harness.
//
// Each traced address gets a wrapper installed with replaceFunction. A
// RecompiledFunction receives no address, but on entry ctx->pc is the
// function's own start, which identifies which original to forward to.

#include "trace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "ps2_runtime.h"

namespace kh2trace
{
namespace
{
struct Traced
{
    PS2Runtime::RecompiledFunction original = nullptr;
    std::atomic<uint64_t> calls{0};
    uint64_t reported = 0;
};

std::unordered_map<uint32_t, Traced *> g_traced;
std::vector<uint32_t> g_order;
constexpr uint64_t kLogFirst = 8;

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
    if (n <= kLogFirst)
        std::printf("[trace] 0x%08X call #%llu a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X\n", address,
                    (unsigned long long)n, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                    getRegU32(ctx, 7), getRegU32(ctx, 31));
    t.original(rdram, ctx, runtime);
}
} // namespace

void install(PS2Runtime &runtime, const std::string &spec)
{
    std::stringstream ss(spec);
    for (std::string item; std::getline(ss, item, ',');)
    {
        if (item.empty())
            continue;
        const uint32_t address = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0));
        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address);
        if (!runtime.hasFunction(address) || !runtime.replaceFunction(address, &wrapper))
        {
            std::fprintf(stderr, "[trace] 0x%08X is not a translated function start\n", address);
            continue;
        }
        auto *t = new Traced;
        t->original = original;
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
    }
    return out.str();
}
} // namespace kh2trace
