#include "guest_call.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

namespace kh2guest
{
namespace
{
struct Active
{
    Step step;
    uint32_t lastResult = 0;
    bool first = true;
};

std::mutex g_mutex;
std::deque<Step> g_queue;
std::unique_ptr<Active> g_active; // EE thread only
// A call is running: the hook can be reached again from inside it (the object
// factory builds the new object's matrix, and that object briefly is the
// "player"), and must then leave the job alone.
bool g_inFlight = false;
std::chrono::steady_clock::time_point g_inFlightSince;

Env envFor(uint8_t *rdram, R5900Context *ctx)
{
    // Below the interrupted frame; the invocation runs on its own stack.
    return Env{rdram, (getRegU32(ctx, 29) - 0x400u) & ~15u};
}
} // namespace

void post(Step job)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.push_back(std::move(job));
}

void postCall(Call call, std::function<void(const Env &, uint32_t)> done)
{
    auto pending = std::make_shared<Call>(std::move(call));
    post([pending, done](const Env &env, uint32_t result, bool first) -> std::optional<Call> {
        if (first)
            return *pending;
        if (done)
            done(env, result);
        return std::nullopt;
    });
}

void runPending(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t hookAddress)
{
    if (g_inFlight)
    {
        // A call that never finished (its thread was torn down with a room,
        // say) must not block every later job.
        if (std::chrono::steady_clock::now() - g_inFlightSince < std::chrono::seconds(10))
            return;
        std::fprintf(stderr, "[guest] call did not finish in 10 s; job dropped\n");
        g_inFlight = false;
        g_active.reset();
    }
    for (;;)
    {
        if (!g_active)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_queue.empty())
                return;
            g_active = std::make_unique<Active>();
            g_active->step = std::move(g_queue.front());
            g_queue.pop_front();
        }
        const Env env = envFor(rdram, ctx);
        std::optional<Call> call = g_active->step(env, g_active->lastResult, g_active->first);
        g_active->first = false;
        if (!call)
        {
            g_active.reset(); // finished; maybe start the next job
            continue;
        }
        if (!runtime->hasFunction(call->address))
        {
            std::fprintf(stderr, "[guest] no function at 0x%08X; job dropped\n", call->address);
            g_active.reset();
            continue;
        }

        EeScheduler &scheduler = runtime->eeScheduler();
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::HleCall;
        R5900Context c = *ctx;
        int reg = 4; // a0..a3, t0..t3 (EABI)
        for (uint32_t a : call->args)
        {
            SET_GPR_U32(&c, reg, a); // (evaluates reg twice: no ++ inside)
            ++reg;
        }
        int f = 12;
        for (float v : call->fargs)
            c.f[f++] = v;
        SET_GPR_U32(&c, 29, scheduler.invocationStackTop());
        SET_GPR_U32(&c, 31, 0u);
        c.pc = call->address;
        invocation.context = c;
        invocation.onComplete = [](const R5900Context &finished, R5900Context &) {
            g_inFlight = false;
            if (g_active)
                g_active->lastResult = getRegU32(&finished, 2);
        };
        g_inFlight = true;
        g_inFlightSince = std::chrono::steady_clock::now();
        // Run the hook again (same registers) once the call has finished.
        ctx->pc = hookAddress;
        scheduler.invokeCurrent(std::move(invocation)); // does not return
    }
}
} // namespace kh2guest
