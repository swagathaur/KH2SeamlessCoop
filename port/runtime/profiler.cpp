// --profile[=MS]: a sampling profiler for finding where host time goes.
//
// A background thread suspends every other thread in the process every MS
// milliseconds (default 1), records its instruction pointer, and resumes it.
// At exit the samples are symbolised with DbgHelp (the build writes a PDB) and
// the hottest functions are printed, overall and for the busiest threads.
// Translated guest functions appear under their generated names
// (FUN_..._0x<addr>), so this also shows which guest code is expensive.
// Windows only; elsewhere the option is accepted and does nothing.

#include "profiler.h"

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace kh2prof
{
namespace
{
std::atomic<bool> g_running{false};
std::atomic<bool> g_recording{true}; // --profile-from: off until that frame
std::thread g_thread;
unsigned g_intervalMs = 1;
std::unordered_map<DWORD, std::unordered_map<DWORD64, uint32_t>> g_samples; // thread -> ip -> count
uint64_t g_total = 0;

// Callers: each sample's top of stack is copied while the thread is suspended
// (a memcpy takes no locks) and unwound afterwards from the copy, so the
// report can say who called a hot library function (memmove, memchr...).
constexpr int kCallerDepth = 6;
struct Stack
{
    DWORD64 ip[kCallerDepth + 1];
    bool operator==(const Stack &o) const { return std::memcmp(ip, o.ip, sizeof(ip)) == 0; }
};
struct StackHash
{
    size_t operator()(const Stack &s) const
    {
        size_t h = 0;
        for (DWORD64 v : s.ip)
            h = h * 1000003u ^ std::hash<DWORD64>()(v);
        return h;
    }
};
std::unordered_map<DWORD, std::unordered_map<Stack, uint32_t, StackHash>> g_stacks;

// Unwinds a context whose stack pointer points into a copy of the stack
// (Rsp rebased by delta); frames that leave the copy end the walk.
Stack unwindCopyUnguarded(CONTEXT ctx, const uint8_t *copy, size_t copySize, DWORD64 realRsp, Stack &st);

// A frame can point outside the copy (a frame pointer into the live stack, a
// corrupt value): the walk stops there instead of faulting.
Stack unwindCopy(CONTEXT ctx, const uint8_t *copy, size_t copySize, DWORD64 realRsp)
{
    Stack st{};
    __try
    {
        unwindCopyUnguarded(ctx, copy, copySize, realRsp, st);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return st;
}

Stack unwindCopyUnguarded(CONTEXT ctx, const uint8_t *copy, size_t copySize, DWORD64 realRsp, Stack &st)
{
    st.ip[0] = ctx.Rip;
    const DWORD64 lo = reinterpret_cast<DWORD64>(copy), hi = lo + copySize;
    ctx.Rsp = lo + (ctx.Rsp - realRsp);
    for (int d = 1; d <= kCallerDepth; ++d)
    {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn)
        {
            // Leaf function: the return address is at [rsp].
            if (ctx.Rsp < lo || ctx.Rsp + 8 > hi)
                break;
            ctx.Rip = *reinterpret_cast<const DWORD64 *>(ctx.Rsp);
            ctx.Rsp += 8;
        }
        else
        {
            void *handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
        if (ctx.Rsp < lo || ctx.Rsp > hi || ctx.Rip == 0)
            break;
        st.ip[d] = ctx.Rip;
    }
    return st;
}

void sampler(DWORD self)
{
    timeBeginPeriod(1);
    std::vector<DWORD> threads;
    std::unordered_map<DWORD, HANDLE> handles;
    unsigned tick = 0;
    while (g_running.load(std::memory_order_relaxed))
    {
        if (tick++ % 200 == 0) // refresh the thread list now and then
        {
            threads.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te{sizeof(te)};
            const DWORD pid = GetCurrentProcessId();
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
                    threads.push_back(te.th32ThreadID);
            CloseHandle(snap);
        }
        if (!g_recording.load(std::memory_order_relaxed))
        {
            Sleep(g_intervalMs);
            continue;
        }
        for (DWORD tid : threads)
        {
            HANDLE &h = handles[tid];
            if (!h)
                h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!h || SuspendThread(h) == (DWORD)-1)
                continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            const bool got = GetThreadContext(h, &ctx) != 0;
            static thread_local std::vector<uint8_t> stackCopy(16384);
            size_t copied = 0;
            if (got)
            {
                // Only memory reads while suspended: the stack's top, as far as
                // it is committed (VirtualQuery is a system call, no user locks).
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<void *>(ctx.Rsp), &mbi, sizeof(mbi)))
                {
                    const DWORD64 regionEnd = reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize;
                    copied = static_cast<size_t>(std::min<DWORD64>(stackCopy.size(), regionEnd - ctx.Rsp));
                    std::memcpy(stackCopy.data(), reinterpret_cast<const void *>(ctx.Rsp), copied);
                }
            }
            // Resume before recording: the map insert can allocate, and the
            // suspended thread may hold the heap lock (that deadlocked runs).
            ResumeThread(h);
            if (got)
            {
                // Threads blocked in waits are sampled too; they show up as ntdll
                // wait functions, and the per-thread tables tell them apart.
                ++g_samples[tid][ctx.Rip];
                ++g_total;
                if (copied >= 64)
                    ++g_stacks[tid][unwindCopy(ctx, stackCopy.data(), copied, ctx.Rsp)];
            }
        }
        Sleep(g_intervalMs);
    }
    for (auto &[tid, h] : handles)
        if (h)
            CloseHandle(h);
    timeEndPeriod(1);
}

std::string symbolFor(DWORD64 ip)
{
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), ip, &disp, sym))
        return sym->Name;
    IMAGEHLP_MODULE64 mod{sizeof(mod)};
    if (SymGetModuleInfo64(GetCurrentProcess(), ip, &mod))
        return std::string("[") + mod.ModuleName + "]";
    return "[unknown]";
}

// Blocked threads sit in these; they are reported separately as idle.
bool isIdle(const std::string &name)
{
    for (const char *needle : {"Wait", "Delay", "Sleep", "RemoveIoCompletion", "AlertByThreadId", "GetMessage", "MessageCall"})
        if (name.find(needle) != std::string::npos)
            return true;
    return false;
}

void printTop(const std::map<std::string, uint64_t> &bySym, uint64_t total, int limit, const char *indent)
{
    std::vector<std::pair<uint64_t, std::string>> rows;
    for (const auto &[name, n] : bySym)
        rows.push_back({n, name});
    std::sort(rows.rbegin(), rows.rend());
    for (int i = 0; i < limit && i < (int)rows.size(); ++i)
        std::printf("%s%6.2f%%  %8llu  %s\n", indent, 100.0 * rows[i].first / total, (unsigned long long)rows[i].first,
                    rows[i].second.c_str());
}
} // namespace

void setRecording(bool on) { g_recording = on; }

void start(unsigned intervalMs)
{
    g_intervalMs = intervalMs ? intervalMs : 1;
    g_running = true;
    g_thread = std::thread([] { sampler(GetCurrentThreadId()); });
    std::printf("[profile] sampling every %u ms\n", g_intervalMs);
}

void stopAndReport()
{
    if (!g_running.exchange(false))
        return;
    g_thread.join();
    // Symbols from the executable's folder only: with a symbol server in
    // _NT_SYMBOL_PATH, a stray caller address made DbgHelp try to download
    // (and wait out timeouts) at the report.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS);
    char exeDir[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (char *slash = std::strrchr(exeDir, '\\'))
        *slash = 0;
    SymInitialize(GetCurrentProcess(), exeDir, TRUE);
    std::unordered_map<DWORD64, std::string> cache;
    auto name = [&](DWORD64 ip) -> const std::string & {
        auto it = cache.find(ip);
        if (it == cache.end())
            it = cache.emplace(ip, symbolFor(ip)).first;
        return it->second;
    };
    std::map<std::string, uint64_t> overall;
    std::vector<std::pair<uint64_t, DWORD>> perThread; // active samples, thread
    uint64_t active = 0, perThreadTotal = 0;
    for (const auto &[tid, ips] : g_samples)
    {
        uint64_t n = 0;
        for (const auto &[ip, c] : ips)
        {
            const std::string &fn = name(ip);
            perThreadTotal += c;
            if (isIdle(fn))
                continue;
            overall[fn] += c;
            n += c;
        }
        active += n;
        perThread.push_back({n, tid});
    }
    std::sort(perThread.rbegin(), perThread.rend());
    const uint64_t perThreadSamples = g_samples.empty() ? 0 : perThreadTotal / g_samples.size();
    std::printf("[profile] %llu samples over %zu threads (~%llu per thread); %llu active (not waiting).\n",
                (unsigned long long)g_total, g_samples.size(), (unsigned long long)perThreadSamples,
                (unsigned long long)active);
    std::printf("[profile] top functions by active samples, all threads (%% of active):\n");
    printTop(overall, active ? active : 1, 40, "[profile]   ");
    // Translated guest functions only, further down the list.
    {
        std::map<std::string, uint64_t> guest;
        for (const auto &[fn, n] : overall)
            if (fn.find("_0x") != std::string::npos)
                guest[fn] = n;
        std::printf("[profile] top translated guest functions:\n");
        printTop(guest, active ? active : 1, 60, "[profile]   guest ");
    }
    for (size_t t = 0; t < perThread.size() && t < 10; ++t)
    {
        const DWORD tid = perThread[t].second;
        std::map<std::string, uint64_t> bySym;
        for (const auto &[ip, c] : g_samples[tid])
            if (!isIdle(name(ip)))
                bySym[name(ip)] += c;
        std::printf("[profile] thread %lu: busy %.1f%% of the run\n", tid,
                    perThreadSamples ? 100.0 * perThread[t].first / perThreadSamples : 0.0);
        printTop(bySym, perThread[t].first ? perThread[t].first : 1, 12, "[profile]     ");
        // Who calls the thread's hottest functions: the first frame outside
        // the function itself and the C runtime / system libraries.
        std::vector<std::pair<uint64_t, std::string>> top;
        for (const auto &[fn, n] : bySym)
            top.push_back({n, fn});
        std::sort(top.rbegin(), top.rend());
        const auto library = [](const std::string &n) {
            return n.rfind("mem", 0) == 0 || n.rfind("str", 0) == 0 || n.rfind("Rtl", 0) == 0 || n.rfind("Nt", 0) == 0 ||
                   n[0] == '[' || n.rfind("std::", 0) == 0 || n.find("operator new") != std::string::npos ||
                   n.find("free") != std::string::npos || n.find("malloc") != std::string::npos;
        };
        for (size_t k = 0; k < top.size() && k < 8; ++k)
        {
            std::map<std::string, uint64_t> callers;
            uint64_t n = 0;
            for (const auto &[stack, c] : g_stacks[tid])
            {
                if (name(stack.ip[0]) != top[k].second)
                    continue;
                std::string caller = "?";
                for (int d = 1; d <= kCallerDepth && stack.ip[d]; ++d)
                {
                    const std::string &cn = name(stack.ip[d]);
                    if (cn != top[k].second && !library(cn))
                    {
                        caller = cn;
                        break;
                    }
                }
                callers[caller] += c;
                n += c;
            }
            if (n == 0)
                continue;
            std::printf("[profile]     callers of %s:\n", top[k].second.c_str());
            printTop(callers, n, 5, "[profile]         ");
        }
    }
    SymCleanup(GetCurrentProcess());
    std::fflush(stdout);
}
} // namespace kh2prof

#else

namespace kh2prof
{
void start(unsigned) { std::printf("[profile] not supported on this platform\n"); }
void setRecording(bool) {}
void stopAndReport() {}
} // namespace kh2prof

#endif
