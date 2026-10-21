// sceCdCallback and read-completion callbacks.
//
// libcdvd lets a program register one function that runs when a CD command
// finishes: callback(SCECdFuncRead) after sceCdRead. KH2's asynchronous file
// loader relies on it: FUN_001AE8D8 starts sceCdRead and returns, and the
// registered callback later calls the requester's completion routine (stored
// at 0x00350CB0). ovl_movie streams its PSS this way and waits for that
// completion before demuxing.
//
// The runtime's sceCdCallback returns 0 without keeping the function, and its
// sceCdRead completes synchronously without notifying anyone, so the movie
// waited forever on a black screen. Here sceCdCallback records the function
// and sceCdRead, after the runtime's read, runs it as an interrupt-context
// invocation, as cdvdman would on the next CD interrupt.

#include "cd_callback.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "hle_call.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

namespace kh2cd
{
namespace
{
constexpr uint32_t kSceCdCallback = 0x002F6698u;
constexpr uint32_t kSceCdRead = 0x002F7BE8u;
constexpr uint32_t kSceCdFuncRead = 1u;

PS2Runtime::RecompiledFunction g_read = nullptr;
uint32_t g_callback = 0;
uint64_t g_fired = 0;
uint64_t g_reads = 0;
uint64_t g_failed = 0;

void cdCallback(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t previous = g_callback;
    g_callback = getRegU32(ctx, 4);
    std::printf("[kh2-cd] sceCdCallback(0x%08X), previous 0x%08X\n", g_callback, previous);
    setReturnU32(ctx, previous);
    ctx->pc = getRegU32(ctx, 31);
}

void cdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t lsn = getRegU32(ctx, 4), sectors = getRegU32(ctx, 5), buf = getRegU32(ctx, 6);
    kh2::callOriginal(g_read, rdram, ctx, runtime); // reads synchronously, sets v0 and pc = ra
    ++g_reads;
    if (getRegU32(ctx, 2) == 0 && g_failed++ < 16)
        std::printf("[kh2-cd] sceCdRead(lsn %u, %u sectors, buf 0x%08X) failed\n", lsn, sectors, buf);
    const uint32_t callback = g_callback;
    if (callback == 0 || getRegU32(ctx, 2) == 0 || !runtime->hasFunction(callback))
        return;

    EeScheduler &scheduler = runtime->eeScheduler();
    const GuestThread *thread = scheduler.currentThread();
    const bool inGuest = thread && thread->inGuestCall;

    GuestInvocation invocation{};
    invocation.kind = GuestInvocationKind::Interrupt;
    R5900Context cb = *ctx;
    SET_GPR_U32(&cb, 4, kSceCdFuncRead);
    SET_GPR_U32(&cb, 29, inGuest ? scheduler.invocationStackTop() : getRegU32(ctx, 29));
    SET_GPR_U32(&cb, 31, 0u);
    cb.pc = callback;
    invocation.context = cb;
    if (g_fired++ < 4)
        std::printf("[kh2-cd] read complete -> callback 0x%08X\n", callback);

    if (inGuest)
    {
        std::vector<GuestInvocation> invocations;
        invocations.push_back(std::move(invocation));
        scheduler.invokeCurrentSequence(std::move(invocations)); // runs it, then resumes at ctx->pc
    }
    else
    {
        scheduler.queueInvocation(std::move(invocation));
    }
}
} // namespace

std::string stats()
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), " cd: reads=%llu failed=%llu", (unsigned long long)g_reads,
                  (unsigned long long)g_failed);
    return buf;
}

bool installCallbacks(PS2Runtime &runtime)
{
    g_read = runtime.lookupFunction(kSceCdRead);
    const bool ok = g_read && runtime.replaceFunction(kSceCdCallback, &cdCallback) &&
                    runtime.replaceFunction(kSceCdRead, &cdRead);
    if (!ok)
        std::fprintf(stderr, "[kh2-cd] could not bind sceCdCallback/sceCdRead\n");
    return ok;
}
} // namespace kh2cd
