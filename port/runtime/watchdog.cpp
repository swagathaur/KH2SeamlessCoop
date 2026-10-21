// Stall watchdog for unattended runs. The presenter loop calls beat() every
// iteration; when it has not for `seconds`, every thread's stack is captured
// and printed with symbols (from the build's PDB), once per stall. That turns
// a hung test run into a report of where each thread is waiting.
//
// Stacks are unwound with RtlLookupFunctionEntry/RtlVirtualUnwind while the
// thread is suspended (no allocation, so no heap-lock deadlock), and only
// symbolised after it has been resumed.

#include "watchdog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <exception>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace kh2watch
{
namespace
{
std::atomic<bool> g_running{false};
std::atomic<int64_t> g_lastBeat{0};
std::thread g_thread;
unsigned g_seconds = 20;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr int kMaxFrames = 40;

int unwind(CONTEXT ctx, DWORD64 *frames)
{
    int n = 0;
    while (n < kMaxFrames && ctx.Rip)
    {
        frames[n++] = ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn)
        {
            // Leaf function: return address is at the top of the stack.
            if (!ctx.Rsp)
                break;
            ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    return n;
}

std::string describe(DWORD64 ip)
{
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    std::string out;
    if (SymFromAddr(GetCurrentProcess(), ip, &disp, sym))
        out = sym->Name;
    else
        out = "?";
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD lineDisp = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), ip, &lineDisp, &line))
    {
        const char *file = line.FileName;
        for (const char *p = file; *p; ++p)
            if (*p == '\\' || *p == '/')
                file = p + 1;
        out += " (" + std::string(file) + ":" + std::to_string(line.LineNumber) + ")";
    }
    return out;
}

void dumpAll(DWORD self, FILE *out = stdout)
{
    static bool symInit = false;
    if (!symInit)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        symInit = true;
    }
    std::vector<DWORD> tids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
            tids.push_back(te.th32ThreadID);
    CloseHandle(snap);

    struct Stack
    {
        DWORD tid;
        int n;
        DWORD64 frames[kMaxFrames];
    };
    std::vector<Stack> stacks(tids.size());
    for (size_t i = 0; i < tids.size(); ++i)
    {
        stacks[i].tid = tids[i];
        stacks[i].n = 0;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tids[i]);
        if (!h)
            continue;
        if (SuspendThread(h) != (DWORD)-1)
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(h, &ctx))
                stacks[i].n = unwind(ctx, stacks[i].frames);
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    std::fprintf(out, "[watchdog] stacks of %zu threads:\n", stacks.size());
    for (const Stack &s : stacks)
    {
        std::fprintf(out, "[watchdog] thread %lu\n", s.tid);
        for (int f = 0; f < s.n; ++f)
            std::fprintf(out, "[watchdog]   %2d %016llx %s\n", f, (unsigned long long)s.frames[f],
                         describe(s.frames[f]).c_str());
    }
    std::fflush(out);
}

// --- Crash reports -------------------------------------------------------
std::string g_crashDir = "crashes";
std::atomic<bool> g_crashing{false};

void symInitOnce()
{
    static bool done = false;
    if (!done)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        done = true;
    }
}

// Writes crashes\kh2recomp-<time>.txt (what and where, every thread's
// stack) and a minidump next to it; says where on stderr.
void report(const char *what, EXCEPTION_POINTERS *ep)
{
    if (g_crashing.exchange(true))
        return; // a second fault while reporting the first
    CreateDirectoryA(g_crashDir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char base[512];
    std::snprintf(base, sizeof(base), "%s\\kh2recomp-%04u%02u%02u-%02u%02u%02u", g_crashDir.c_str(), t.wYear, t.wMonth,
                  t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::string txt = std::string(base) + ".txt", dmp = std::string(base) + ".dmp";
    FILE *f = std::fopen(txt.c_str(), "w");
    if (!f)
        f = stderr;
    symInitOnce();
    std::fprintf(f, "kh2recomp crashed: %s\n", what);
    if (ep && ep->ExceptionRecord)
    {
        const EXCEPTION_RECORD *er = ep->ExceptionRecord;
        std::fprintf(f, "exception %08lX at %016llx %s\n", er->ExceptionCode,
                     (unsigned long long)(uintptr_t)er->ExceptionAddress,
                     describe(reinterpret_cast<DWORD64>(er->ExceptionAddress)).c_str());
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            std::fprintf(f, "  %s address %016llx\n", er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading",
                         (unsigned long long)er->ExceptionInformation[1]);
    }
    std::fprintf(f, "crashing thread %lu:\n", GetCurrentThreadId());
    if (ep && ep->ContextRecord)
    {
        DWORD64 frames[kMaxFrames];
        const int n = unwind(*ep->ContextRecord, frames);
        for (int i = 0; i < n; ++i)
            std::fprintf(f, "  %2d %016llx %s\n", i, (unsigned long long)frames[i], describe(frames[i]).c_str());
    }
    dumpAll(GetCurrentThreadId(), f);
    HANDLE h = CreateFileA(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                          MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), ep ? &mei : nullptr,
                          nullptr, nullptr);
        CloseHandle(h);
    }
    if (f != stderr)
        std::fclose(f);
    std::fprintf(stderr, "[kh2] crashed (%s); report: %s\n", what, txt.c_str());
    std::fflush(stderr);
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS *ep)
{
    report("unhandled exception", ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate()
{
    report("std::terminate (an uncaught C++ exception or a failed check)", nullptr);
    std::_Exit(3);
}

void onAbort(int)
{
    report("abort()", nullptr);
    std::_Exit(3);
}

void run(DWORD self)
{
    bool reported = false;
    while (g_running.load())
    {
        Sleep(500);
        const int64_t since = nowMs() - g_lastBeat.load();
        if (since > int64_t(g_seconds) * 1000)
        {
            if (!reported)
            {
                dumpAll(self);
                reported = true;
            }
        }
        else
            reported = false;
    }
}
} // namespace

void start(unsigned seconds)
{
    if (g_running.exchange(true))
        return;
    g_seconds = seconds ? seconds : 20;
    g_lastBeat = nowMs();
    g_thread = std::thread([] { run(GetCurrentThreadId()); });
}

void beat() { g_lastBeat.store(nowMs(), std::memory_order_relaxed); }

void dumpNow() { dumpAll(GetCurrentThreadId()); }

void installCrashHandler(const char *dir)
{
    if (dir && *dir)
        g_crashDir = dir;
    SetUnhandledExceptionFilter(&onUnhandled);
    std::set_terminate(&onTerminate);
    std::signal(SIGABRT, &onAbort);
    // No "this program has stopped working" box: the report is the message.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

void stop()
{
    if (!g_running.exchange(false))
        return;
    g_thread.join();
}
} // namespace kh2watch

#else

namespace kh2watch
{
void start(unsigned) {}
void beat() {}
void stop() {}
void dumpNow() {}
void installCrashHandler(const char *) {}
} // namespace kh2watch

#endif
