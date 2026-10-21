#include "guest_call.h"

#include <cstdio>
#include <mutex>
#include <vector>

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace kh2guest
{
namespace
{
std::mutex g_mutex;
std::vector<Job> g_jobs;
} // namespace

void post(Job job)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jobs.push_back(std::move(job));
}

void runPending(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_jobs.empty())
            return;
        jobs.swap(g_jobs);
    }
    const R5900Context saved = *ctx;
    // Scratch below the live frame, and the callees' stack below that.
    const uint32_t sp = getRegU32(ctx, 29);
    Env env{rdram, ctx, runtime, (sp - 0x200u) & ~15u};
    for (Job &job : jobs)
    {
        SET_GPR_U32(ctx, 29, env.scratch - 0x40u);
        job(env);
        *ctx = saved;
    }
}

uint32_t call(const Env &env, uint32_t address, std::initializer_list<uint32_t> args, std::initializer_list<float> fargs)
{
    PS2Runtime::RecompiledFunction fn = env.runtime->lookupFunction(address);
    if (!fn)
    {
        std::fprintf(stderr, "[guest] no function at 0x%08X\n", address);
        return 0;
    }
    R5900Context *ctx = env.ctx;
    const R5900Context saved = *ctx;
    int reg = 4; // a0..a3, then t0..t3 (EABI)
    for (uint32_t a : args)
    {
        // (SET_GPR_U32 evaluates its register argument twice: no reg++ in it.)
        SET_GPR_U32(ctx, reg, a);
        ++reg;
    }
    int f = 12;
    for (float v : fargs)
        ctx->f[f++] = v;
    SET_GPR_U32(ctx, 31, 0u);
    fn(env.rdram, ctx, env.runtime);
    const uint32_t result = getRegU32(ctx, 2);
    *ctx = saved;
    return result;
}
} // namespace kh2guest
