#include "local_players.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kh2local
{
namespace
{
std::mutex g_mutex;
int g_player = 1;
std::vector<std::string> g_args;
std::string g_exe;

#if defined(_WIN32)
HANDLE g_job = nullptr;
std::map<int, HANDLE> g_children; // player -> process

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// One argument, quoted for CommandLineToArgvW / the C runtime: backslashes
// count only before a quote (doubled there), a quote is escaped.
std::wstring quote(const std::wstring &a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
        return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        if (c == L'"')
            out.append(slashes * 2 + 1, L'\\');
        else
            out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
#endif

// Where the window places are kept: next to the program.
std::filesystem::path placesFile()
{
    std::error_code ec;
    std::filesystem::path exe = g_exe.empty() ? std::filesystem::path() : std::filesystem::absolute(g_exe, ec);
    const std::filesystem::path dir = exe.has_parent_path() ? exe.parent_path() : std::filesystem::current_path(ec);
    return dir / "kh2recomp-windows.ini";
}

// Settings and command line options a local player must not inherit.
bool notForOthers(const std::string &a)
{
    static const char *const kDrop[] = {"--host",  "--join=",   "--join-at=", "--name=",    "--player=", "--pad=",
                                        "--local-players=", "--fullscreen", "--volume=", "--mute", "--window-pos=",
                                        "--script=", "--input=", "--teleport=", "--poke=", "--warp-test=",
                                        // testing aids that act on one window only
                                        "--menu-at=", "--arrange-at=", "--story-save-test=", "--ram-dump=", "--gs-dump=",
                                        "--ee-dump=", "--stack-dump=", "--hurt-enemies=", "--partner-test=", "--spawn-test=",
                                        "--preload-test=", "--nat-test"};
    for (const char *d : kDrop)
        if (a.rfind(d, 0) == 0)
            return true;
    return false;
}
} // namespace

void setPlayer(int player)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_player = std::clamp(player, 1, 3);
}
int player()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_player;
}

void setLaunchArgs(const std::vector<std::string> &args, const std::string &exe)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_args = args;
    g_exe = exe;
#if defined(_WIN32)
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH))
        g_exe = std::filesystem::path(path).string();
#endif
}

std::string memoryCardFolder(int player) { return player <= 1 ? "mc0" : "mc0-p" + std::to_string(player); }

bool spawn(int player, const std::string &address, const std::string &name, int pad, std::string &error)
{
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(g_mutex);
    if (player < 2 || player > 3)
    {
        error = "only players 2 and 3";
        return false;
    }
    auto it = g_children.find(player);
    if (it != g_children.end())
    {
        if (WaitForSingleObject(it->second, 0) == WAIT_TIMEOUT)
        {
            error = "player " + std::to_string(player) + " is already playing";
            return false;
        }
        CloseHandle(it->second);
        g_children.erase(it);
    }
    if (!g_job)
    {
        // Local players go when this window goes.
        g_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (g_job)
            SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
    std::wstring cmd = quote(widen(g_exe));
    bool haveNoSettings = false;
    for (const std::string &a : g_args)
    {
        if (notForOthers(a))
            continue;
        haveNoSettings |= a == "--no-settings";
        if (a.rfind("--shots=", 0) == 0) // testing: screenshots of their own
        {
            cmd += L" " + quote(widen(a + "-p" + std::to_string(player)));
            continue;
        }
        cmd += L" " + quote(widen(a));
    }
    // Their settings are ours (in the arguments above), not the file's again.
    if (!haveNoSettings)
        cmd += L" --no-settings";
    for (const std::string &a : {"--player=" + std::to_string(player), "--pad=" + std::to_string(pad + 1),
                                 "--join=" + address, "--name=" + name, std::string("--volume=0")})
        cmd += L" " + quote(widen(a));

    // Their output goes to logs\local-pN.log.
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    const std::wstring logName = L"logs\\local-p" + std::to_wstring(player) + L".log";
    HANDLE log = CreateFileW(logName.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof(si)};
    if (log != INVALID_HANDLE_VALUE)
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = log;
        si.hStdError = log;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE,
                                   CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);
    if (!ok)
    {
        error = "could not start it (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (g_job)
        AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_children[player] = pi.hProcess;
    std::printf("[local] started player %d (controller %d)\n", player, pad + 1);
    std::fflush(stdout);
    return true;
#else
    (void)player;
    (void)address;
    (void)name;
    (void)pad;
    error = "local players need Windows for now";
    return false;
#endif
}

std::vector<int> running()
{
    std::vector<int> out;
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &[p, h] : g_children)
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT)
            out.push_back(p);
#endif
    return out;
}

bool loadPlace(int player, int &x, int &y, int &w, int &h)
{
    std::ifstream f(placesFile());
    std::string line;
    const std::string key = "player" + std::to_string(player) + " =";
    while (std::getline(f, line))
        if (line.rfind(key, 0) == 0)
        {
            std::istringstream in(line.substr(key.size()));
            if (in >> x >> y >> w >> h && w > 100 && h > 100)
                return true;
        }
    return false;
}

void savePlace(int player, int x, int y, int w, int h)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const std::filesystem::path path = placesFile();
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        std::string line;
        const std::string key = "player" + std::to_string(player) + " =";
        while (std::getline(f, line))
            if (line.rfind(key, 0) != 0)
                lines.push_back(line);
    }
    lines.push_back("player" + std::to_string(player) + " = " + std::to_string(x) + " " + std::to_string(y) + " " +
                    std::to_string(w) + " " + std::to_string(h));
    std::sort(lines.begin(), lines.end());
    std::ofstream out(path, std::ios::trunc);
    for (const std::string &l : lines)
        out << l << "\n";
}

int arrangeWindows()
{
#if defined(_WIN32)
    // Our windows (class KH2RecompWindow), by the player number in their title.
    struct Found
    {
        HWND hwnd;
        int player;
    };
    std::vector<Found> wins;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            wchar_t cls[64] = {};
            GetClassNameW(h, cls, 64);
            if (std::wstring(cls) != L"KH2RecompWindow" || !IsWindowVisible(h))
                return TRUE;
            wchar_t title[256] = {};
            GetWindowTextW(h, title, 256);
            const std::wstring t(title);
            int p = 1;
            const size_t at = t.find(L"Player ");
            if (at != std::wstring::npos)
                p = _wtoi(t.c_str() + at + 7);
            reinterpret_cast<std::vector<Found> *>(lp)->push_back({h, p});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&wins));
    if (wins.empty())
        return 0;
    std::sort(wins.begin(), wins.end(), [](const Found &a, const Found &b) { return a.player < b.player; });
    // This window's monitor (the first player's if we cannot tell).
    HWND anchor = wins[0].hwnd;
    for (const Found &f : wins)
        if (f.player == player())
            anchor = f.hwnd;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT a = mi.rcWork;
    const int W = a.right - a.left, H = a.bottom - a.top;
    std::vector<RECT> cells;
    if (wins.size() == 1)
        cells = {a};
    else if (wins.size() == 2)
        cells = {{a.left, a.top, a.left + W / 2, a.bottom}, {a.left + W / 2, a.top, a.right, a.bottom}};
    else
        cells = {{a.left, a.top, a.left + W / 2, a.bottom},
                 {a.left + W / 2, a.top, a.right, a.top + H / 2},
                 {a.left + W / 2, a.top + H / 2, a.right, a.bottom}};
    int placed = 0;
    for (size_t i = 0; i < wins.size() && i < cells.size(); ++i)
    {
        const RECT &c = cells[i];
        ShowWindow(wins[i].hwnd, SW_RESTORE);
        SetWindowPos(wins[i].hwnd, nullptr, c.left, c.top, c.right - c.left, c.bottom - c.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // Each copy saves its own place when it closes or is moved; save
        // these now too, in case one is closed by the job instead.
        savePlace(wins[i].player, c.left, c.top, c.right - c.left, c.bottom - c.top);
        ++placed;
    }
    return placed;
#else
    return 0;
#endif
}
} // namespace kh2local
