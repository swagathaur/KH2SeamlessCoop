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
std::thread g_thread;
unsigned g_intervalMs = 1;
std::unordered_map<DWORD, std::unordered_map<DWORD64, uint32_t>> g_samples; // thread -> ip -> count
uint64_t g_total = 0;

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
        for (DWORD tid : threads)
        {
            HANDLE &h = handles[tid];
            if (!h)
                h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!h || SuspendThread(h) == (DWORD)-1)
                continue;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            const bool got = GetThreadContext(h, &ctx) != 0;
            // Resume before recording: the map insert can allocate, and the
            // suspended thread may hold the heap lock (that deadlocked runs).
            ResumeThread(h);
            if (got)
            {
                // Threads blocked in waits are sampled too; they show up as ntdll
                // wait functions, and the per-thread tables tell them apart.
                ++g_samples[tid][ctx.Rip];
                ++g_total;
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
    for (const char *needle : {"Wait", "Delay", "Sleep", "RemoveIoCompletion", "AlertByThreadId"})
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
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
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
    for (size_t t = 0; t < perThread.size() && t < 6; ++t)
    {
        const DWORD tid = perThread[t].second;
        std::map<std::string, uint64_t> bySym;
        for (const auto &[ip, c] : g_samples[tid])
            if (!isIdle(name(ip)))
                bySym[name(ip)] += c;
        std::printf("[profile] thread %lu: busy %.1f%% of the run\n", tid,
                    perThreadSamples ? 100.0 * perThread[t].first / perThreadSamples : 0.0);
        printTop(bySym, perThread[t].first ? perThread[t].first : 1, 12, "[profile]     ");
    }
    SymCleanup(GetCurrentProcess());
    std::fflush(stdout);
}
} // namespace kh2prof

#else

namespace kh2prof
{
void start(unsigned) { std::printf("[profile] not supported on this platform\n"); }
void stopAndReport() {}
} // namespace kh2prof

#endif
