// VU1 programs compiled while the game runs (docs/playing.md "Speed").
//
// The runtime records each VU1 program it meets that the build did not
// compile in (PS2_VU_PROGRAM_PROFILE, set by play.ps1): its microcode image
// and entry PC. Interpreted, such a program holds 3D scenes to a few frames a
// second. This watches the recordings, and for new ones runs
// tools/win/vu-hot.ps1 in the background: the same compiler the build uses
// (compile_vu_programs.py, then MSVC) makes a small DLL of them, linked
// against this executable's import library (kh2recomp.lib: the members the
// compiled code calls are exported, PS2X_VU_HOT_EXPORT). The DLL is loaded and
// its routines handed to the runtime (ps2_vu_program::addRoutines), which
// uses them from the next time the program runs: no restart.
//
// Modules stay in build\vu-hot\<this build> and are loaded at the start of
// later sessions of the same build. The next build of the game compiles the
// recordings in for good; what it compiled is listed next to the executable
// (vu-static-entries.txt) and not compiled again here.
// Windows with the Visual Studio build tools only; KH2_VU_HOT=0 turns it off.
#include "vu_hot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pgs_renderer.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace ps2_vu_program
{
struct Entry; // ps2_vu1_program.h (private to the runtime); only passed on here
void addRoutines(const Entry *entries, size_t count);
} // namespace ps2_vu_program

namespace kh2vuhot
{
namespace
{
std::atomic<bool> g_stop{false};

std::vector<std::string> readLines(const fs::path &path)
{
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            lines.push_back(line);
    }
    return lines;
}

#if defined(_WIN32)
// Runs the compile script, hidden and below normal priority (the game keeps
// the CPU); returns its exit code, or -1 if it could not start.
int runScript(const fs::path &root, const fs::path &job)
{
    const fs::path script = root / "tools" / "win" / "vu-hot.ps1";
    std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" -Job \"" +
                       job.wstring() + L"\" -Root \"" + root.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, root.wstring().c_str(), &si, &pi))
        return -1;
    while (WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT)
    {
        if (g_stop.load())
        {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// Loads a compiled module and hands its routines to the runtime.
size_t loadModule(const fs::path &dll)
{
    HMODULE module = LoadLibraryW(dll.wstring().c_str());
    if (!module)
    {
        std::fprintf(stderr, "[vu-hot] could not load %s (error %lu)\n", dll.string().c_str(), GetLastError());
        return 0;
    }
    using EntriesFn = const ps2_vu_program::Entry *(*)(size_t *);
    auto entries = reinterpret_cast<EntriesFn>(GetProcAddress(module, "kh2_vu_hot_entries"));
    if (!entries)
        return 0;
    size_t count = 0;
    const ps2_vu_program::Entry *list = entries(&count);
    if (!list || !count)
        return 0;
    ps2_vu_program::addRoutines(list, count); // the module stays loaded for the rest of the run
    return count;
}
#endif

void watch(fs::path root, fs::path profile, fs::path exeDir)
{
#if defined(_WIN32)
    // Modules are kept per build of the game (its size and time): a later
    // session of the same build loads them at once, another build's are
    // removed (they were linked against its exports).
    std::error_code ec;
    const fs::path exe = exeDir / "kh2recomp.exe";
    const auto exeTime = fs::last_write_time(exe, ec).time_since_epoch().count();
    const std::string stamp = std::to_string(fs::file_size(exe, ec)) + "-" + std::to_string(exeTime);
    const fs::path base = root / "build" / "vu-hot";
    const fs::path hot = base / stamp;
    fs::create_directories(hot, ec);
    for (const auto &dir : fs::directory_iterator(base, ec))
        if (dir.is_directory() && dir.path().filename() != stamp)
            fs::remove_all(dir.path(), ec);

    std::set<std::string> done;
    for (const std::string &line : readLines(exeDir / "vu-static-entries.txt"))
        done.insert(line); // compiled in by the build
    int jobs = 0;
    size_t cached = 0;
    for (const auto &dir : fs::directory_iterator(hot, ec))
    {
        const fs::path dll = dir.path() / "hot.dll";
        const std::string name = dir.path().filename().string();
        if (name.rfind("job", 0) == 0)
            jobs = std::max(jobs, std::atoi(name.c_str() + 3));
        if (!fs::exists(dll, ec))
            continue;
        if (const size_t n = loadModule(dll))
        {
            cached += n;
            for (const std::string &line : readLines(dir.path() / "profile" / "entries.txt"))
                done.insert(line);
        }
    }
    if (cached)
    {
        std::printf("[vu-hot] %zu routines from earlier sessions\n", cached);
        std::fflush(stdout);
    }
    const fs::path list = profile / "entries.txt";
    fs::file_time_type seen{};
    auto changedAt = std::chrono::steady_clock::now();
    while (!g_stop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!fs::exists(list, ec))
            continue;
        const fs::file_time_type now = fs::last_write_time(list, ec);
        if (now != seen)
        {
            seen = now;
            changedAt = std::chrono::steady_clock::now();
            continue;
        }
        // A scene meets its programs in a burst: wait for it to settle.
        if (std::chrono::steady_clock::now() - changedAt < std::chrono::milliseconds(1500))
            continue;
        std::vector<std::string> pending;
        for (const std::string &line : readLines(list))
            if (!done.count(line))
                pending.push_back(line);
        if (pending.empty())
            continue;

        const fs::path job = hot / ("job" + std::to_string(++jobs));
        const fs::path jobProfile = job / "profile";
        fs::create_directories(jobProfile, ec);
        {
            std::ofstream out(jobProfile / "entries.txt");
            for (const std::string &line : pending)
            {
                out << line << '\n';
                const std::string hash = line.substr(0, line.find(' '));
                const fs::path image = profile / ("vu1-" + hash + ".code");
                fs::copy_file(image, jobProfile / image.filename(), fs::copy_options::skip_existing, ec);
            }
        }
        for (const std::string &line : pending)
            done.insert(line); // tried once per run, whether it works or not
        std::printf("[vu-hot] compiling %zu new VU1 program entr%s in the background\n", pending.size(),
                    pending.size() == 1 ? "y" : "ies");
        std::fflush(stdout);
        const auto start = std::chrono::steady_clock::now();
        const int code = runScript(root, job);
        if (g_stop.load())
            break;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        size_t loaded = 0;
        if (code == 0)
            loaded = loadModule(job / "hot.dll");
        if (loaded)
        {
            std::printf("[vu-hot] %zu routines live after %.0f s\n", loaded, secs);
            kh2pgs::toast("3D programs optimised (" + std::to_string(pending.size()) + " new)", 3.0);
        }
        else
        {
            std::fprintf(stderr, "[vu-hot] compiling failed (exit %d); see %s\n", code,
                         (job / "log.txt").string().c_str());
        }
        std::fflush(stdout);
    }
#else
    (void)root;
    (void)profile;
    (void)exeDir;
#endif
}
} // namespace

void start(const std::filesystem::path &exeDir)
{
#if defined(_WIN32)
    if (const char *off = std::getenv("KH2_VU_HOT"); off && *off == '0')
        return;
    const char *profile = std::getenv("PS2_VU_PROGRAM_PROFILE");
    if (!profile || !*profile)
        return;
    std::error_code ec;
    const fs::path root = fs::current_path(ec);
    const fs::path vswhere = fs::path(std::getenv("ProgramFiles(x86)") ? std::getenv("ProgramFiles(x86)") : "") /
                             "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    if (!fs::exists(root / "tools" / "win" / "vu-hot.ps1", ec) || !fs::exists(exeDir / "kh2recomp.lib", ec) ||
        !fs::exists(vswhere, ec))
    {
        std::printf("[vu-hot] off (needs the project's build tools next to this build)\n");
        return;
    }
    // Detached: the game can end with exit() anywhere; stop() only asks it to.
    std::thread(watch, root, fs::path(profile), exeDir).detach();
#else
    (void)exeDir;
#endif
}

void stop() { g_stop = true; }
} // namespace kh2vuhot
