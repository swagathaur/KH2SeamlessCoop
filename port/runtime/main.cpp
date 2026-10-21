// kh2recomp: native runner for the statically recompiled KH2 Final Mix (SLPM_666.75).
//
// Normal use opens a window. With --headless it runs without one and doubles
// as a scriptable test harness: run N guest frames, save screenshots, feed
// scripted pad input, and exit with a code describing what happened.
//
// Structure follows DQ8Recomp's src/runtime/main.cpp (GPL-3.0).

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ps2_runtime.h"
#include "runtime/ps2_pad_host.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_native_iop.h"
#include "raylib.h"

#include "overlay_manager.h"
#include "pad2_hle.h"
#include "cd_iso.h"
#include "trace.h"
#include "dma_log.h"
#include "gs_image.h"
#include "gs_env.h"
#include "cd_callback.h"
#include "movie_eof.h"
#include "profiler.h"
#include "pgs_renderer.h"
#include "host_input.h"
#include "game_patches.h"
#include "game_state.h"
#include "coop.h"
#include "puppets.h"
#include "watchdog.h"
#include "overlay_ui.h"

namespace ps2_stubs
{
void setMpegLegacyGuestGlobals(bool enabled); // patches/ps2recomp/0004
}

// See hle_call.h: set while a replacement calls the function it replaced.
thread_local bool kh2_callingOriginal = false;

// Movie-path counters kept by the runtime's libmpeg (Kernel/Stubs/MPEG.cpp).
extern std::atomic<uint64_t> g_mpegDemuxCount;
extern std::atomic<uint64_t> g_mpegGetPictureCount;
extern std::atomic<uint64_t> g_mpegWriteFrameCount;
extern std::atomic<uint64_t> g_mpegDemuxRefusedCount;

namespace
{
// KH2 memory map, from crt0 (0x0010001C) and FUN_00168548:
//   0x00100000-0x01E4C868  code, data, bss
//   0x01E4C868-0x01F18000  unreferenced by the game  <- runtime arena + callback stacks
//   0x01F18000-0x01FC0000  EE overlay arena
//   0x01FC0000-0x01FE0000  C heap (SetupHeap)
//   0x01FE0000-0x02000000  main thread stack (SetupThread)
constexpr uint32_t kRuntimeHeapBase = 0x01E50000u;
constexpr uint32_t kRuntimeHeapEnd = 0x01EF0000u;
constexpr uint32_t kCallbackStackFloor = 0x01EF0000u;
constexpr uint32_t kCallbackStackTop = 0x01F18000u;
constexpr uint32_t kGuestHeapCeiling = 0x01FE0000u; // what EndOfHeap reports

enum ExitCode : int
{
    kExitFramesReached = 0,
    kExitStoppedEarly = 1,   // runtime stopped on its own (missing function, guest exit, ...)
    kExitUsage = 2,
    kExitTimeout = 3,        // wall-clock limit hit before the frame target
    kExitInitFailed = 4,
};

struct Options
{
    std::string elf;
    std::string iso;
    std::string cdRoot;
    bool headless = false;
    uint64_t frames = 0;               // stop after this many guest frames (0 = run forever)
    double timeoutSeconds = 0.0;       // wall-clock limit (0 = none)
    uint64_t screenshotEvery = 0;      // save a PNG every N guest frames
    std::set<uint64_t> screenshotAt;   // ...and/or at these guest frames
    std::string shotDir = "shots";
    std::string input;                 // pad script (file or inline), see --help
    double statusEvery = -1.0;         // seconds between status lines (default 2 headless, off windowed)
    std::string missing = "stop";      // missing-function policy
    std::string sound = "native";      // native: run the disc's sound IRX on the emulated IOP
    bool mute = false;
    int volume = 100;                  // --volume: percent
    std::string trace;                 // comma-separated guest function addresses
    bool skipMovies = false;           // --skip-movies
    double speed = 1.0;                // --speed: guest time vs wall time, 0 = as fast as possible
    unsigned profileMs = 0;            // --profile: sampling interval, 0 = off
    std::string dmaLog;                // FRAME,COUNT: GIF DMA chains to decode
    std::set<uint64_t> gsDumpAt;       // guest frames at which to dump GS state + VRAM
    std::set<uint64_t> eeDumpAt;       // guest frames at which to print EE threads and sync objects
    std::vector<uint32_t> watch;       // guest words to print (hex and float) on each status line
    std::set<uint64_t> ramDumpAt;      // guest frames at which to write all 32 MB of RDRAM
    std::set<uint64_t> stackDumpAt;    // guest frames at which to print every host thread's stack
    std::string renderer = "auto";     // gpu | soft | auto (gpu when windowed)
    unsigned ssaa = 1;                 // GPU renderer super-sampling: 1, 2, 4, 8, 16
    bool fullscreen = false;
    unsigned windowW = 1280, windowH = 960;
    bool vsync = false;
    bool fsr = true;
    bool swapConfirm = false;          // controller: confirm on the right button instead of the bottom
    bool noRumble = false;
    bool showFps = false;
    float ffSpeed = 3.0f;              // --ff-speed: fast-forward (hold Tab)
    bool pauseUnfocused = false;
    bool noMouseCamera = false;
    float mouseSensitivity = 1.0f;
    std::string patches;               // game patches to enable, comma-separated
    uint32_t mashMask = 0;             // --mash: press these buttons every mashEvery frames
    uint64_t mashEvery = 0;
    uint64_t mashUntil = UINT64_MAX;  // --mash-until: stop mashing at this guest frame
    struct ScriptStep { uint64_t start, frames; uint32_t buttons; uint8_t lx, ly, rx, ry; };
    std::vector<ScriptStep> script;    // --script: timed buttons and stick positions
    unsigned watchdog = 0;             // seconds without a host-loop iteration before dumping stacks
    bool listPatches = false;
    kh2coop::Options coop;             // --host / --join / --name
    std::vector<std::pair<uint64_t, uint32_t>> spawnTests; // --spawn-test=FRAME:ENTRY
};

void usage()
{
    std::printf(
        "usage: kh2recomp <SLPM_666.75> [options]\n"
        "  --iso=PATH              disc image (needed for KH2.IMG/OVL.IMG sector reads)\n"
        "  --cd-root=DIR           directory for loose cdrom0: files (default: the ELF's)\n"
        "  --headless              no window (the options below also work windowed with --renderer=gpu)\n"
        "  --frames=N              stop after N guest frames (vsyncs), exit 0\n"
        "  --timeout=SEC           give up after SEC wall-clock seconds, exit 3\n"
        "  --screenshot-every=N    save shots/frame_XXXXXX.png every N guest frames\n"
        "  --screenshot-at=A,B,..  save shots at these guest frames\n"
        "  --spawn-test=FRAME:ID   experiment: create objentry ID (hex) in front of the player at FRAME\n"
        "  --ram-dump=A,B,..       write the guest's 32 MB of RAM to SHOTS/ram_FRAME.bin at these frames\n"
        "  --shots=DIR             screenshot directory (default: shots)\n"
        "  --input=SCRIPT          pad script, file or inline: '<frame> <BTN>[+BTN] [hold];...'\n"
        "                          buttons: UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE\n"
        "                                   START SELECT L1 R1 L2 R2 L3 R3\n"
        "  --status-every=SEC      status line interval (default 2, 0 = off)\n"
        "  --missing=stop|continue what to do on a call to untranslated code (default stop)\n"
        "  --sound=native|off      run LIBSD/LIBSSL on the emulated IOP, or stub them (default native)\n"
        "  --volume=N              sound volume, 0-100 (default 100)\n"
        "  --mute                  keep the sound driver running but play nothing\n"
        "  --trace=ADDR[,ADDR..]   log the first calls to these guest functions and count them\n"
        "  --skip-movies           end each movie as soon as it starts (debugging)\n"
        "  --speed=X               run guest time X times real time (0 = unpaced, as fast as possible)\n"
        "  --profile[=MS]          sample host threads every MS ms (default 1) and print hot functions\n"
        "  --dma-log=FRAME,COUNT   decode COUNT GIF-channel DMA chains from this guest frame on\n"
        "  --ee-dump=A,B,..        at these guest frames print EE threads, semaphores and event flags\n"
        "  --watch=ADDR[,ADDR..]   print these guest words (hex and as float) on every status line\n"
        "  --stack-dump=A,B,..     at these guest frames print every host thread's stack (the EE\n"
        "                          thread's shows the translated guest call chain)\n"
        "  --gs-dump=A,B,..        at these guest frames write GS registers and all of GS memory\n"
        "                          (as a 1024x1024 32-bit image) into the screenshot directory\n"
        "  --renderer=gpu|soft     GS renderer: gpu = paraLLEl-GS on Vulkan (default when windowed),\n"
        "                          soft = the runtime's software rasteriser (default headless)\n"
        "  --ssaa=N                GPU renderer super-sampling, N = 1, 2, 4, 8 or 16 (default 1)\n"
        "  --fullscreen            start fullscreen (Alt+Enter or F11 toggles)\n"
        "  --window=WxH            window size (default 1280x960)\n"
        "  --vsync                 present in sync with the display's refresh\n"
        "  --no-fsr                bilinear upscale to the window instead of FSR 1 + sharpening\n"
        "  --swap-confirm          controller: right face button confirms (default: bottom button)\n"
        "  --no-rumble             controller vibration off\n"
        "  --show-fps              show frame rates (F2 toggles; F1 or Select+Start opens settings)\n"
        "  --ff-speed=X            fast-forward speed while Tab is held (default 3)\n"
        "  --no-mouse-camera       don't turn the camera with the mouse (right button held)\n"
        "  --mouse-sensitivity=X   mouse camera speed (default 1)\n"
        "  --pause-unfocused       pause while the window is in the background\n"
        "  --patch=NAME[,NAME..]   enable game patches (see --list-patches); --widescreen = --patch=widescreen\n"
        "  --list-patches          list the available game patches and exit\n"
        "  --watchdog=SEC          print every thread's stack if the host loop stalls this long\n"
        "                          (default 20 with --headless, off otherwise)\n"
        "  --script=FILE           timed input: lines 'START FRAMES BUTTONS|- [LX LY [RX RY]]'\n"
        "                          (sticks 0-255, 128 = centre, LY 0 = up; START may be +N)\n"
        "  --host[=PORT]           host a co-op session (default port 27552)\n"
        "  --join=HOST[:PORT]      join a co-op session\n"
        "  --name=NAME             your name in co-op\n"
        "  --net-loss=F            testing: drop this fraction of outgoing co-op packets\n"
        "  --mash-until=FRAME      stop --mash at this guest frame\n"
        "  --mash=BTN[+BTN]:N      tap these buttons every N guest frames (gets through menus and\n"
        "                          dialogue whatever the load times)\n"
        "exit codes: 0 frame target reached, 1 runtime stopped early, 2 usage,\n"
        "            3 timeout, 4 initialisation failed\n");
}

// Active-high pad masks as ps2_pad.cpp uses them.
uint32_t buttonMask(const std::string &name)
{
    static const std::map<std::string, uint32_t> kButtons = {
        {"SELECT", 0x0001}, {"L3", 0x0002}, {"R3", 0x0004}, {"START", 0x0008}, {"UP", 0x0010},
        {"RIGHT", 0x0020}, {"DOWN", 0x0040}, {"LEFT", 0x0080}, {"L2", 0x0100}, {"R2", 0x0200},
        {"L1", 0x0400}, {"R1", 0x0800}, {"TRIANGLE", 0x1000}, {"CIRCLE", 0x2000}, {"CROSS", 0x4000},
        {"SQUARE", 0x8000}};
    const auto it = kButtons.find(name);
    return it == kButtons.end() ? 0u : it->second;
}

bool parse(int argc, char **argv, Options &o)
{
    auto value = [](const std::string &arg) { return arg.substr(arg.find('=') + 1); };
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") return false;
        else if (a.rfind("--iso=", 0) == 0) o.iso = value(a);
        else if (a.rfind("--cd-root=", 0) == 0) o.cdRoot = value(a);
        else if (a == "--headless") o.headless = true;
        else if (a.rfind("--frames=", 0) == 0) o.frames = std::stoull(value(a));
        else if (a.rfind("--timeout=", 0) == 0) o.timeoutSeconds = std::stod(value(a));
        else if (a.rfind("--screenshot-every=", 0) == 0) o.screenshotEvery = std::stoull(value(a));
        else if (a.rfind("--screenshot-at=", 0) == 0)
        {
            std::stringstream ss(value(a));
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) o.screenshotAt.insert(std::stoull(t));
        }
        else if (a.rfind("--shots=", 0) == 0) o.shotDir = value(a);
        else if (a.rfind("--input=", 0) == 0) o.input = value(a);
        else if (a.rfind("--status-every=", 0) == 0) o.statusEvery = std::stod(value(a));
        else if (a.rfind("--missing=", 0) == 0) o.missing = value(a);
        else if (a.rfind("--sound=", 0) == 0) o.sound = value(a);
        else if (a == "--mute") o.mute = true;
        else if (a.rfind("--volume=", 0) == 0) o.volume = std::clamp(std::stoi(value(a)), 0, 100);
        else if (a.rfind("--trace=", 0) == 0) o.trace = value(a);
        else if (a.rfind("--dma-log=", 0) == 0) o.dmaLog = value(a);
        else if (a == "--profile") o.profileMs = 1;
        else if (a == "--skip-movies") o.skipMovies = true;
        else if (a.rfind("--speed=", 0) == 0) o.speed = std::stod(value(a));
        else if (a.rfind("--profile=", 0) == 0) o.profileMs = std::max(1, std::stoi(value(a)));
        else if (a.rfind("--watch=", 0) == 0)
        {
            std::stringstream ss(value(a));
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) o.watch.push_back(static_cast<uint32_t>(std::stoul(t, nullptr, 16)));
        }
        else if (a.rfind("--ram-dump=", 0) == 0)
        {
            std::stringstream ss(value(a));
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) o.ramDumpAt.insert(std::stoull(t));
        }
        else if (a.rfind("--stack-dump=", 0) == 0)
        {
            std::stringstream ss(value(a));
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) o.stackDumpAt.insert(std::stoull(t));
        }
        else if (a.rfind("--ee-dump=", 0) == 0)
        {
            std::stringstream ss(value(a));
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) o.eeDumpAt.insert(std::stoull(t));
        }
        else if (a.rfind("--gs-dump=", 0) == 0)
        {
            std::stringstream ss(value(a));
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) o.gsDumpAt.insert(std::stoull(t));
        }
        else if (a.rfind("--renderer=", 0) == 0) o.renderer = value(a);
        else if (a.rfind("--ssaa=", 0) == 0) o.ssaa = static_cast<unsigned>(std::stoul(value(a)));
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a.rfind("--window=", 0) == 0)
        {
            const std::string v = value(a);
            const size_t x = v.find_first_of("xX");
            if (x == std::string::npos) return false;
            o.windowW = static_cast<unsigned>(std::stoul(v.substr(0, x)));
            o.windowH = static_cast<unsigned>(std::stoul(v.substr(x + 1)));
        }
        else if (a == "--vsync") o.vsync = true;
        else if (a == "--no-fsr") o.fsr = false;
        else if (a == "--swap-confirm") o.swapConfirm = true;
        else if (a == "--no-rumble") o.noRumble = true;
        else if (a == "--show-fps") o.showFps = true;
        else if (a.rfind("--ff-speed=", 0) == 0) o.ffSpeed = std::stof(value(a));
        else if (a == "--pause-unfocused") o.pauseUnfocused = true;
        else if (a == "--no-mouse-camera") o.noMouseCamera = true;
        else if (a.rfind("--mouse-sensitivity=", 0) == 0) o.mouseSensitivity = std::stof(value(a));
        else if (a.rfind("--patch=", 0) == 0) o.patches += (o.patches.empty() ? "" : ",") + value(a);
        else if (a == "--widescreen") o.patches += o.patches.empty() ? "widescreen" : ",widescreen";
        else if (a.rfind("--mash=", 0) == 0)
        {
            const std::string v = value(a);
            const size_t colon = v.find(':');
            if (colon == std::string::npos) return false;
            o.mashEvery = std::stoull(v.substr(colon + 1));
            std::stringstream ss(v.substr(0, colon));
            for (std::string b; std::getline(ss, b, '+');)
            {
                const uint32_t m = buttonMask(b);
                if (!m) { std::fprintf(stderr, "unknown button %s\n", b.c_str()); return false; }
                o.mashMask |= m;
            }
        }
        else if (a == "--host") o.coop.host = true;
        else if (a.rfind("--host=", 0) == 0) { o.coop.host = true; o.coop.port = static_cast<uint16_t>(std::stoul(value(a))); }
        else if (a.rfind("--join=", 0) == 0) o.coop.join = value(a);
        else if (a.rfind("--name=", 0) == 0) o.coop.name = value(a);
        else if (a.rfind("--net-loss=", 0) == 0) o.coop.simulatedLoss = std::stof(value(a));
        else if (a.rfind("--spawn-test=", 0) == 0)
        {
            const std::string v = value(a);
            const size_t colon = v.find(':');
            if (colon == std::string::npos) return false;
            o.spawnTests.emplace_back(std::stoull(v.substr(0, colon)),
                                      static_cast<uint32_t>(std::stoul(v.substr(colon + 1), nullptr, 16)));
        }
        else if (a.rfind("--mash-until=", 0) == 0) o.mashUntil = std::stoull(value(a));
        else if (a.rfind("--script=", 0) == 0)
        {
            // Lines: START FRAMES BUTTONS|- [LX LY [RX RY]]  (sticks 0..255, 128 centre;
            // LY 0 is up). '#' starts a comment. START may be +N, relative to the
            // end of the previous step.
            std::ifstream in(value(a));
            if (!in) { std::fprintf(stderr, "cannot read %s\n", value(a).c_str()); return false; }
            uint64_t prevEnd = 0;
            for (std::string line; std::getline(in, line);)
            {
                line = line.substr(0, line.find('#'));
                std::istringstream ls(line);
                std::string start, buttons;
                Options::ScriptStep st{0, 0, 0, 0x80, 0x80, 0x80, 0x80};
                if (!(ls >> start >> st.frames >> buttons))
                    continue;
                st.start = start[0] == '+' ? prevEnd + std::stoull(start.substr(1)) : std::stoull(start);
                if (buttons != "-")
                {
                    std::stringstream bs(buttons);
                    for (std::string b; std::getline(bs, b, '+');)
                    {
                        const uint32_t m = buttonMask(b);
                        if (!m) { std::fprintf(stderr, "unknown button %s\n", b.c_str()); return false; }
                        st.buttons |= m;
                    }
                }
                unsigned v[4] = {0x80, 0x80, 0x80, 0x80};
                for (unsigned &x : v)
                    if (!(ls >> x)) break;
                st.lx = uint8_t(v[0]); st.ly = uint8_t(v[1]); st.rx = uint8_t(v[2]); st.ry = uint8_t(v[3]);
                prevEnd = st.start + st.frames;
                o.script.push_back(st);
            }
            std::fprintf(stderr, "[harness] script: %zu steps, ends at frame %llu\n", o.script.size(),
                         static_cast<unsigned long long>(prevEnd));
        }
        else if (a.rfind("--watchdog=", 0) == 0) o.watchdog = static_cast<unsigned>(std::stoul(value(a)));
        else if (a == "--list-patches") { o.listPatches = true; o.elf = o.elf.empty() ? "-" : o.elf; }
        else if (!a.empty() && a[0] != '-' && o.elf.empty()) o.elf = a;
        else
        {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return !o.elf.empty();
}

// State shared with the headless presenter callback.
struct Harness
{
    PS2Runtime *runtime = nullptr;
    const Options *opt = nullptr;
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point lastStatus;
    uint64_t lastShotFrame = UINT64_MAX;
    uint64_t lastDumpFrame = UINT64_MAX;
    uint64_t lastGpuFrame = UINT64_MAX;
    uint64_t lastEeDumpFrame = UINT64_MAX;
    uint64_t lastStackDumpFrame = UINT64_MAX;
    uint64_t lastRamDumpFrame = UINT64_MAX;
    uint64_t lastSpawnFrame = UINT64_MAX;
    uint64_t presented = 0;
    int result = kExitStoppedEarly;
    std::vector<uint8_t> pixels;
    std::filesystem::path screenshotDir = "screenshots";
};

double elapsed(const Harness &h)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - h.start).count();
}

void saveShot(Harness &h, uint64_t frame)
{
    uint32_t w = 0, hgt = 0;
    h.pixels.clear();
    bool ok = false;
    if (kh2pgs::active())
    {
        std::vector<uint32_t> rgba;
        ok = kh2pgs::readback(rgba, w, hgt);
        if (ok)
        {
            h.pixels.resize(rgba.size() * 4);
            std::memcpy(h.pixels.data(), rgba.data(), h.pixels.size());
        }
    }
    else
        ok = h.runtime->gs().copyLatchedHostPresentationFrame(h.pixels, w, hgt);
    if (!ok || w == 0 || hgt == 0)
    {
        std::printf("[harness] frame %llu: no displayable frame yet\n", (unsigned long long)frame);
        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%06llu.png", (unsigned long long)frame);
    const std::string path = (std::filesystem::path(h.opt->shotDir) / name).string();
    Image img{h.pixels.data(), static_cast<int>(w), static_cast<int>(hgt), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    if (ExportImage(img, path.c_str()))
        std::printf("[harness] saved %s (%ux%u)\n", path.c_str(), w, hgt);
    else
        std::fprintf(stderr, "[harness] could not write %s\n", path.c_str());
}

// F12: the current picture at the shown aspect, to screenshots\ next to the
// executable (or the working directory).
void saveUserShot(const std::filesystem::path &dir)
{
    std::vector<uint32_t> rgba;
    uint32_t w = 0, hgt = 0;
    if (!kh2pgs::readback(rgba, w, hgt) || !w || !hgt)
        return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "kh2_%Y%m%d_%H%M%S", &tm);
    std::filesystem::path path = dir / (std::string(name) + ".png");
    for (int i = 2; std::filesystem::exists(path, ec); ++i)
        path = dir / (std::string(name) + "_" + std::to_string(i) + ".png");
    Image img{rgba.data(), static_cast<int>(w), static_cast<int>(hgt), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    // Stretch to the display aspect (the GS picture's pixels are not square).
    Image out = ImageCopy(img);
    const int outW = static_cast<int>(std::lround(hgt * kh2pgs::displayAspect()));
    ImageResize(&out, outW, static_cast<int>(hgt));
    const bool ok = ExportImage(out, path.string().c_str());
    UnloadImage(out);
    kh2pgs::toast(ok ? "Screenshot saved: " + path.filename().string() : "Could not save " + path.string());
}

// GS state and the whole of GS local memory, to see where drawing lands.
void dumpGs(Harness &h, uint64_t frame)
{
    GS &gs = h.runtime->gs();
    const GSDebugSnapshot snap = gs.getDebugSnapshot();
    const GSRegisters &pr = h.runtime->memory().gs();
    char name[64];
    std::snprintf(name, sizeof(name), "gs_%06llu.txt", (unsigned long long)frame);
    const std::string txt = (std::filesystem::path(h.opt->shotDir) / name).string();
    if (FILE *f = std::fopen(txt.c_str(), "w"))
    {
        std::fprintf(f, "frame %llu\n", (unsigned long long)frame);
        std::fprintf(f, "PMODE %016llx DISPFB1 %016llx DISPLAY1 %016llx DISPFB2 %016llx DISPLAY2 %016llx BGCOLOR %016llx\n",
                     (unsigned long long)pr.pmode, (unsigned long long)pr.dispfb1, (unsigned long long)pr.display1,
                     (unsigned long long)pr.dispfb2, (unsigned long long)pr.display2, (unsigned long long)pr.bgcolor);
        for (int i = 0; i < 2; ++i)
        {
            const GSContext &c = snap.ctx[i];
            std::fprintf(f, "ctx%d FRAME fbp=%u fbw=%u psm=%u fbmsk=%08x ZBUF zbp=%u SCISSOR %u..%u x %u..%u "
                            "XYOFFSET %u,%u TEST %016llx ALPHA %016llx\n",
                         i, c.frame.fbp, c.frame.fbw, c.frame.psm, c.frame.fbmsk, c.zbuf.zbp, c.scissor.x0,
                         c.scissor.x1, c.scissor.y0, c.scissor.y1, c.xyoffset.ofx, c.xyoffset.ofy,
                         (unsigned long long)c.test, (unsigned long long)c.alpha);
        }
        std::fprintf(f, "PRIM type=%d tme=%d abe=%d ctxt=%d\n", (int)snap.prim.type, (int)snap.prim.tme,
                     (int)snap.prim.abe, (int)snap.prim.ctxt);
        std::fprintf(f, "display base bytes %u, preferred source %d fbp=%u fbw=%u psm=%u -> dest fbp %u\n",
                     snap.lastDisplayBaseBytes, (int)snap.hasPreferredDisplaySource,
                     snap.preferredDisplaySourceFrame.fbp, snap.preferredDisplaySourceFrame.fbw,
                     snap.preferredDisplaySourceFrame.psm, snap.preferredDisplayDestFbp);
        std::fprintf(f, "host presentation %ux%u displayFbp=%u sourceFbp=%u preferred=%d has=%d\n",
                     snap.hostPresentationWidth, snap.hostPresentationHeight, snap.hostPresentationDisplayFbp,
                     snap.hostPresentationSourceFbp, (int)snap.hostPresentationUsedPreferred,
                     (int)snap.hasHostPresentationFrame);
        std::fprintf(f, "native image uploads %llu, native packed GIF packets %llu\n",
                     (unsigned long long)gs.nativeImageUploadCount(),
                     (unsigned long long)gs.nativePackedGIFPacketCount());
        // The last complete frame: events between the last two presents
        // (the frontend keeps the most recent 32k events, patch 0006).
        const auto history = gs.getDebugHistory();
        size_t frameEnd = history.size(), frameBegin = 0;
        for (size_t i = history.size(); i-- > 0;)
            if (history[i].kind == GSDebugEventKind::Present)
            {
                if (frameEnd == history.size())
                    frameEnd = i;
                else
                {
                    frameBegin = i + 1;
                    break;
                }
            }
        struct Group
        {
            int draws = 0;
            double area = 0;
            std::string example;
        };
        std::map<std::string, Group> groups;
        int kinds[5] = {};
        double totalArea = 0;
        for (size_t i = frameBegin; i < frameEnd; ++i)
        {
            const GSDebugHistoryEntry &e = history[i];
            kinds[std::min<int>(4, (int)e.kind)]++;
            if (e.kind != GSDebugEventKind::Draw)
                continue;
            char key[160];
            std::snprintf(key, sizeof(key), "fbp=%u fbw=%u psm=%u fbmsk=%08x prim=%d tme=%d abe=%d tbp=%u tpsm=%u",
                          e.frame.fbp, e.frame.fbw, e.frame.psm, e.frame.fbmsk, (int)e.prim.type, (int)e.prim.tme,
                          (int)e.prim.abe, e.tex0.tbp0, e.tex0.psm);
            char eg[200];
            std::snprintf(eg, sizeof(eg), "verts=%u x %.0f..%.0f y %.0f..%.0f a %u..%u test=%llx alpha=%llx",
                          e.vertexCount, e.xMin, e.xMax, e.yMin, e.yMax, e.aMin, e.aMax,
                          (unsigned long long)e.test, (unsigned long long)e.alpha);
            Group &g = groups[key];
            g.draws++;
            const double area = std::max(0.0, (double)(e.xMax - e.xMin)) * std::max(0.0, (double)(e.yMax - e.yMin));
            g.area += area;
            totalArea += area;
            g.example = eg;
        }
        std::fprintf(f, "last frame: events %zu..%zu of %zu (giftag %d, register %d, draw %d, transfer %d, present %d), "
                        "draw bounding boxes %.0f px\n",
                     frameBegin, frameEnd, history.size(), kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], totalArea);
        std::vector<std::pair<double, std::string>> order;
        for (const auto &[key, g] : groups)
            order.push_back({g.area, key});
        std::sort(order.rbegin(), order.rend());
        for (const auto &[area, key] : order)
        {
            const Group &g = groups[key];
            std::fprintf(f, "  draws x%-5d area %9.0f  %s | e.g. %s\n", g.draws, area, key.c_str(), g.example.c_str());
        }
        // Then every event of that frame in order.
        static const char *kKinds[] = {"giftag", "reg", "draw", "xfer", "present"};
        for (size_t i = frameBegin; i <= frameEnd && i < history.size(); ++i)
        {
            const GSDebugHistoryEntry &e = history[i];
            const int k = std::min<int>(4, (int)e.kind);
            std::fprintf(f, "%6llu v%-5llu %-7s ", (unsigned long long)e.seq, (unsigned long long)e.vsyncTick,
                         kKinds[k]);
            switch (e.kind)
            {
            case GSDebugEventKind::GifTag:
                std::fprintf(f, "size=%u nloop=%u flg=%u nreg=%u\n", e.gifSizeBytes, e.gifNloop, e.gifFlg,
                             e.gifNreg);
                break;
            case GSDebugEventKind::Register:
                std::fprintf(f, "%02x = %016llx\n", e.reg, (unsigned long long)e.regValue);
                break;
            case GSDebugEventKind::Draw:
                std::fprintf(f, "prim=%d tme=%d abe=%d fbp=%u psm=%u tbp=%u tpsm=%u cbp=%u verts=%u x %.0f..%.0f y "
                                "%.0f..%.0f a %u..%u\n",
                             (int)e.prim.type, (int)e.prim.tme, (int)e.prim.abe, e.frame.fbp, e.frame.psm,
                             e.tex0.tbp0, e.tex0.psm, e.tex0.cbp, e.vertexCount, e.xMin, e.xMax, e.yMin, e.yMax,
                             e.aMin, e.aMax);
                break;
            case GSDebugEventKind::Transfer:
                std::fprintf(f, "dir=%u pixels=%u dbp=%u dbw=%u dpsm=%u dsax=%u dsay=%u rrw=%u rrh=%u\n", e.trxdir,
                             e.transferPixels, e.bitbltbuf.dbp, e.bitbltbuf.dbw, e.bitbltbuf.dpsm, e.trxpos.dsax,
                             e.trxpos.dsay, e.trxreg.rrw, e.trxreg.rrh);
                break;
            default:
                std::fprintf(f, "displayFbp=%u sourceFbp=%u %ux%u preferred=%d\n", e.displayFbp, e.sourceFbp,
                             e.width, e.height, (int)e.usedPreferred);
                break;
            }
        }
        std::fclose(f);
    }
    // All 4 MB of GS memory as PSMCT32, 1024 pixels wide (FBW 16), alpha forced opaque.
    std::vector<uint8_t> vram(1024u * 1024u * 4u);
    uint64_t nonzero = 0;
    for (uint32_t y = 0; y < 1024; ++y)
        for (uint32_t x = 0; x < 1024; ++x)
        {
            const uint32_t v = gs.ReadVram(0x00 /* PSMCT32 */, 0, 16, x, y);
            nonzero += (v & 0x00FFFFFFu) != 0;
            uint8_t *p = &vram[(y * 1024u + x) * 4u];
            p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = 0xFF;
        }
    std::snprintf(name, sizeof(name), "vram_%06llu.png", (unsigned long long)frame);
    const std::string png = (std::filesystem::path(h.opt->shotDir) / name).string();
    Image img{vram.data(), 1024, 1024, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    ExportImage(img, png.c_str());
    std::printf("[harness] GS dump at frame %llu: %s, %s (%llu non-black pixels of 1048576)\n",
                (unsigned long long)frame, txt.c_str(), png.c_str(), (unsigned long long)nonzero);
}

// Screenshots, GS dumps, status lines and the frame/time limits; shared by the
// headless harness and the windowed GPU presenter. False = stop.
bool harnessTick(Harness &h, uint64_t frame)
{
    const Options &o = *h.opt;
    kh2watch::beat();
    if (frame != h.lastShotFrame &&
        ((o.screenshotEvery && frame % o.screenshotEvery == 0) || o.screenshotAt.count(frame)))
    {
        h.lastShotFrame = frame;
        saveShot(h, frame);
    }
    if (o.gsDumpAt.count(frame) && frame != h.lastDumpFrame)
    {
        h.lastDumpFrame = frame;
        dumpGs(h, frame);
    }
    for (const auto &[at, entry] : o.spawnTests)
        if (frame == at && frame != h.lastSpawnFrame)
        {
            h.lastSpawnFrame = frame;
            kh2puppets::requestSpawn(entry);
        }
    if (o.ramDumpAt.count(frame) && frame != h.lastRamDumpFrame)
    {
        h.lastRamDumpFrame = frame;
        char name[64];
        std::snprintf(name, sizeof(name), "ram_%06llu.bin", (unsigned long long)frame);
        const std::filesystem::path path = std::filesystem::path(o.shotDir) / name;
        std::error_code ec;
        std::filesystem::create_directories(o.shotDir, ec);
        if (FILE *f = std::fopen(path.string().c_str(), "wb"))
        {
            std::fwrite(h.runtime->memory().getRDRAM(), 1, 32u * 1024u * 1024u, f);
            std::fclose(f);
            std::printf("[harness] wrote %s\n", path.string().c_str());
        }
    }
    if (o.stackDumpAt.count(frame) && frame != h.lastStackDumpFrame)
    {
        h.lastStackDumpFrame = frame;
        std::printf("[harness] host stacks at guest frame %llu\n", (unsigned long long)frame);
        kh2watch::dumpNow();
    }
    if (o.eeDumpAt.count(frame) && frame != h.lastEeDumpFrame)
    {
        h.lastEeDumpFrame = frame;
        const EeKernelSnapshot k = h.runtime->eeScheduler().snapshot();
        std::printf("[ee] frame %llu: running thread %d, %zu threads\n", (unsigned long long)frame,
                    k.runningThreadId, k.threads.size());
        for (const EeThreadSnapshot &t : k.threads)
            std::printf("[ee]   thread %2d prio %2d/%2d status %d wait %d id %d pc %08X ra %08X sp %08X entry %08X "
                        "suspend %d wakeups %u\n",
                        t.id, t.currentPriority, t.initialPriority, (int)t.status, (int)t.waitReason, t.waitId, t.pc,
                        t.ra, t.sp, t.entry, t.suspendCount, t.wakeupCount);
        for (const EeSemaphoreSnapshot &sem : k.semaphores)
            std::printf("[ee]   sema %2d count %d/%d waiters %u\n", sem.id, sem.count, sem.maxCount, sem.waiters);
        for (const EeEventFlagSnapshot &ef : k.eventFlags)
            std::printf("[ee]   eventflag %2d bits %08X init %08X attr %X waiters %u\n", ef.id, ef.bits, ef.initBits,
                        ef.attr, ef.waiters);
        std::fflush(stdout);
    }

    const auto now = std::chrono::steady_clock::now();
    if (o.statusEvery > 0 && std::chrono::duration<double>(now - h.lastStatus).count() >= o.statusEvery)
    {
        h.lastStatus = now;
        char mpeg[128] = "";
        if (g_mpegDemuxCount.load() || g_mpegGetPictureCount.load())
            std::snprintf(mpeg, sizeof(mpeg), " mpeg: demux=%llu refused=%llu pictures=%llu frames=%llu",
                          (unsigned long long)g_mpegDemuxCount.load(), (unsigned long long)g_mpegDemuxRefusedCount.load(),
                          (unsigned long long)g_mpegGetPictureCount.load(),
                          (unsigned long long)g_mpegWriteFrameCount.load());
        std::string watched;
        for (uint32_t addr : o.watch)
        {
            uint32_t v;
            std::memcpy(&v, h.runtime->memory().getRDRAM() + (addr & 0x01FFFFFFu), 4);
            float f;
            std::memcpy(&f, &v, 4);
            char w[64];
            std::snprintf(w, sizeof(w), " [%08X]=%08X(%g)", addr, v, f);
            watched += w;
        }
        const kh2state::State gs = kh2state::read(h.runtime->memory().getRDRAM());
        char game[160];
        std::snprintf(game, sizeof(game), " | %s ev=%u ctl=%u btl=%u hp=%d/%d pos=%.0f,%.0f,%.0f",
                      kh2state::describe(gs.now).c_str(), gs.now.event, gs.control, gs.battle, gs.party[0].hp,
                      gs.party[0].maxHp, gs.player.pos[0], gs.player.pos[1], gs.player.pos[2]);
        std::string coop;
        if (kh2coop::active())
        {
            coop = " | coop: " + kh2coop::statusText();
            for (const kh2coop::RemotePlayer &r : kh2coop::remotes())
            {
                char rb[160];
                std::snprintf(rb, sizeof(rb), "; %s %s pos=%.0f,%.0f,%.0f rtt=%.0fms", r.name.c_str(),
                              r.haveState ? kh2state::describe({r.state.world, r.state.room}).c_str() : "-",
                              r.state.pos[0], r.state.pos[1], r.state.pos[2], r.rttMs);
                coop += rb;
            }
        }
        std::printf("[status] t=%.1fs guest_frame=%llu overlay=%s overlay_switches=%u%s%s%s%s%s%s\n", elapsed(h),
                    (unsigned long long)frame, kh2ovl::activeName(), kh2ovl::switchCount(), game, mpeg,
                    (kh2cd::stats() + coop).c_str(), watched.c_str(), o.trace.empty() ? "" : " calls:",
                    kh2trace::summary().c_str());
        std::fflush(stdout);
    }

    if (o.frames && frame >= o.frames)
    {
        std::printf("[harness] reached %llu guest frames in %.1fs\n", (unsigned long long)frame, elapsed(h));
        h.result = kExitFramesReached;
        return false;
    }
    if (o.timeoutSeconds > 0 && elapsed(h) >= o.timeoutSeconds)
    {
        std::printf("[harness] timeout after %.1fs at guest frame %llu\n", elapsed(h), (unsigned long long)frame);
        h.result = kExitTimeout;
        return false;
    }
    return true;
}

// Called once per host loop iteration by the runtime when presenting externally.
bool headlessPump(void *userData)
{
    Harness &h = *static_cast<Harness *>(userData);
    const Options &o = *h.opt;
    const uint64_t frame = ps2PadCurrentGuestFrame();
    ++h.presented;
    // No window, so the runtime's present loop never polls input; publishing an
    // idle host state is what merges the --input script in (ps2_pad.cpp).
    {
        // --mash: held for 4 frames out of every N.
        const bool mash = o.mashEvery && frame < o.mashUntil;
        const uint32_t held = (mash && frame % o.mashEvery < 4) ? o.mashMask : 0u;
        uint32_t pressed = (mash && frame % o.mashEvery == 0) ? o.mashMask : 0u;
        uint32_t heldAll = held;
        uint32_t sticks = 0x80808080u;
        for (const Options::ScriptStep &st : o.script)
            if (frame >= st.start && frame < st.start + st.frames)
            {
                heldAll |= st.buttons;
                if (frame == st.start)
                    pressed |= st.buttons;
                sticks = (uint32_t(st.rx) << 24) | (uint32_t(st.ry) << 16) | (uint32_t(st.lx) << 8) | st.ly;
            }
        ps2PadPublishHostState(heldAll, pressed, sticks);
    }
    kh2movie::poll(*h.runtime);
    kh2patches::tick(h.runtime->memory().getRDRAM());
    kh2coop::tick(h.runtime->memory().getRDRAM(), frame);
    if (kh2pgs::active() && frame != h.lastGpuFrame)
    {
        h.lastGpuFrame = frame;
        kh2pgs::frame(h.runtime->memory().gs(), frame);
    }
    else if (kh2pgs::active())
        kh2pgs::pumpWindow(); // feeds queued GS data between frames
    return harnessTick(h, frame);
}

// Called once per host loop iteration when the GPU renderer owns the window.
bool playPump(void *userData)
{
    Harness &h = *static_cast<Harness *>(userData);
    const uint64_t frame = ps2PadCurrentGuestFrame();
    kh2ui::tick();
    if (kh2pgs::takeScreenshotRequest())
        saveUserShot(h.screenshotDir);
    kh2input::poll(kh2pgs::windowFocused());
    kh2movie::poll(*h.runtime);
    kh2patches::tick(h.runtime->memory().getRDRAM());
    kh2coop::tick(h.runtime->memory().getRDRAM(), frame);
    if (frame != h.lastGpuFrame)
    {
        h.lastGpuFrame = frame;
        if (!kh2pgs::frame(h.runtime->memory().gs(), frame))
            return false; // window closed
    }
    else if (!(kh2ui::paused() ? kh2pgs::redraw() : kh2pgs::pumpWindow()))
        return false;
    return harnessTick(h, frame);
}
} // namespace

// Settings file: kh2recomp.ini next to the executable (or in the working
// directory). One "key = value" per line, keys as the command-line options
// without "--" ("ssaa = 4", "fullscreen = 1", "patch = widescreen", "elf =
// game/SLPM_666.75", "iso = ..."); '#' or ';' start comments. Command-line
// arguments come after it, so they override it.
std::vector<std::string> readSettings(const char *argv0)
{
    std::vector<std::string> out;
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;
    const auto exe = std::filesystem::absolute(argv0, ec);
    if (!ec)
        candidates.push_back(exe.parent_path() / "kh2recomp.ini");
    candidates.push_back(std::filesystem::current_path(ec) / "kh2recomp.ini");
    for (const auto &path : candidates)
    {
        FILE *f = std::fopen(path.string().c_str(), "r");
        if (!f)
            continue;
        char line[1024];
        while (std::fgets(line, sizeof(line), f))
        {
            std::string l(line);
            const size_t hash = l.find_first_of("#;");
            if (hash != std::string::npos)
                l.erase(hash);
            auto trim = [](std::string v) {
                const size_t b = v.find_first_not_of(" \t\r\n");
                const size_t e = v.find_last_not_of(" \t\r\n");
                return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
            };
            const size_t eq = l.find('=');
            const std::string key = trim(eq == std::string::npos ? l : l.substr(0, eq));
            const std::string val = eq == std::string::npos ? std::string() : trim(l.substr(eq + 1));
            if (key.empty())
                continue;
            if (key == "elf")
                out.push_back(val);
            else if (val.empty() || val == "1" || val == "true" || val == "yes" || val == "on")
                out.push_back("--" + key);
            else if (val == "0" || val == "false" || val == "no" || val == "off")
                continue;
            else
                out.push_back("--" + key + "=" + val);
        }
        std::fclose(f);
        std::printf("[kh2] settings from %s\n", path.string().c_str());
        break;
    }
    return out;
}

int main(int argc, char **argv)
{
    Options opt;
    std::vector<std::string> merged = readSettings(argv[0]);
    // An ELF given on the command line replaces one from the settings.
    bool cliElf = false;
    for (int i = 1; i < argc; ++i)
        cliElf |= argv[i][0] != '-';
    if (cliElf)
        merged.erase(std::remove_if(merged.begin(), merged.end(),
                                    [](const std::string &a) { return !a.empty() && a[0] != '-'; }),
                     merged.end());
    for (int i = 1; i < argc; ++i)
        merged.push_back(argv[i]);
    std::vector<char *> mergedArgv{argv[0]};
    for (std::string &a : merged)
        mergedArgv.push_back(a.data());
    if (!parse(static_cast<int>(mergedArgv.size()), mergedArgv.data(), opt))
    {
        usage();
        return kExitUsage;
    }
    if (opt.listPatches)
    {
        std::printf("game patches (--patch=NAME[,NAME...]):\n");
        kh2patches::list();
        return 0;
    }
    if (opt.statusEvery < 0)
        opt.statusEvery = opt.headless ? 2.0 : 0.0;
    if (!opt.input.empty())
    {
#if defined(_WIN32)
        _putenv_s("DQ8_PAD_SCRIPT", opt.input.c_str());
#else
        setenv("DQ8_PAD_SCRIPT", opt.input.c_str(), 1);
#endif
    }
    if (!opt.screenshotAt.empty() || opt.screenshotEvery || !opt.gsDumpAt.empty())
        std::filesystem::create_directories(opt.shotDir);

    std::printf("kh2recomp -- loading %s%s\n", opt.elf.c_str(), opt.headless ? " (headless)" : "");

    PS2Runtime runtime;
    Harness harness;
    harness.runtime = &runtime;
    harness.opt = &opt;
    const bool wantGpu = opt.renderer == "gpu" || (opt.renderer == "auto" && !opt.headless);
    bool gpu = false;
    if (wantGpu)
    {
        kh2pgs::Options g;
        g.superSampling = opt.ssaa;
        g.windowed = !opt.headless;
        g.windowWidth = opt.windowW;
        g.windowHeight = opt.windowH;
        g.fullscreen = opt.fullscreen;
        g.vsync = opt.vsync;
        g.fsr = opt.fsr;
        gpu = kh2pgs::init(g);
        if (!gpu && opt.renderer == "gpu" && opt.headless)
            return kExitInitFailed;
    }
    kh2input::Options inputOpt;
    inputOpt.confirmOnBottom = !opt.swapConfirm;
    kh2input::init(inputOpt);
    kh2input::setRumbleEnabled(!opt.noRumble);
    kh2input::setMouseCamera(!opt.noMouseCamera);
    kh2input::setMouseSensitivity(opt.mouseSensitivity);
    if (gpu)
        kh2pgs::setShowFps(opt.showFps);
    // Both must precede initialize().
    if (opt.headless)
        runtime.setExternalPresenter(&headlessPump, &harness);
    else if (gpu)
        runtime.setExternalPresenter(&playPump, &harness);

    if (!runtime.initialize("KH2FM Co-op Recomp"))
    {
        std::fprintf(stderr, "[kh2] PS2Runtime::initialize() failed\n");
        return kExitInitFailed;
    }
    // KH2's sound runs on the IOP: Sony's LIBSD plus LIBSSL (Square's sound
    // library), loaded by name through sceSifLoadModule (0x0030E770).
    if (opt.sound == "native")
    {
        ps2_native_iop::setModules(runtime, {"LIBSD.IRX", "LIBSSL.IRX"});
        if (opt.mute || opt.volume < 100)
            ps2_native_iop::setVolume(opt.mute ? 0.0f : opt.volume / 100.0f);
    }

    if (!runtime.loadELF(opt.elf))
    {
        std::fprintf(stderr, "[kh2] loadELF(%s) failed\n", opt.elf.c_str());
        return kExitInitFailed;
    }

    // After loadELF(), which resets I/O paths and the heap/stack ranges.
    PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
    if (!opt.iso.empty()) paths.cdImage = opt.iso;
    if (!opt.cdRoot.empty()) paths.cdRoot = opt.cdRoot;
    PS2Runtime::setIoPaths(paths);
    std::printf("[kh2] cd root %s, cd image %s\n", paths.cdRoot.string().c_str(),
                paths.cdImage.empty() ? "(none)" : paths.cdImage.string().c_str());

    runtime.setGuestHeapCeiling(kGuestHeapCeiling);
    runtime.configureGuestHeap(kRuntimeHeapBase, kRuntimeHeapEnd);
    runtime.setAsyncCallbackStackRange(kCallbackStackFloor, kCallbackStackTop);
    std::printf("[kh2] runtime heap 0x%08X-0x%08X, callback stacks 0x%08X-0x%08X\n", kRuntimeHeapBase,
                kRuntimeHeapEnd, kCallbackStackFloor, kCallbackStackTop);

    runtime.setMissingFunctionPolicy(opt.missing == "continue"
                                         ? PS2Runtime::MissingFunctionPolicy::ContinueToTarget
                                         : PS2Runtime::MissingFunctionPolicy::Stop);
    kh2ovl::install(runtime);
    kh2pad2::install(runtime);
    kh2gs::install(runtime);
    kh2gs::installEnv(runtime);
    // ovl_movie's libmpeg is bound to the runtime's (ffmpeg) libmpeg, which by
    // default also pokes Dragon Quest VIII's libmpeg globals (patch 0004).
    ps2_stubs::setMpegLegacyGuestGlobals(false);
    if (!paths.cdImage.empty())
        kh2cd::install(runtime, paths.cdImage.string());
    kh2cd::installCallbacks(runtime);
    if (!opt.dmaLog.empty())
        kh2dma::install(runtime, opt.dmaLog);
    if (!opt.gsDumpAt.empty())
        runtime.gs().setDebugHistoryPaused(false); // the frontend records nothing while paused (default)
    if (!opt.patches.empty())
    {
        if (!kh2patches::enable(runtime, opt.patches))
            return kExitUsage;
        if (kh2patches::aspect() > 0.0f)
            kh2pgs::setAspect(kh2patches::aspect());
    }
    if (gpu && !opt.headless)
    {
        std::error_code ec;
        const auto exeDir = std::filesystem::absolute(argv[0], ec).parent_path();
        kh2ui::Options uiOpt;
        uiOpt.fastForwardSpeed = opt.ffSpeed;
        uiOpt.pauseUnfocused = opt.pauseUnfocused;
        uiOpt.baseSpeed = opt.speed;
        uiOpt.volume = opt.mute ? 0.0f : opt.volume / 100.0f;
        kh2ui::install(runtime, (exeDir / "kh2recomp.ini").string(), uiOpt);
        harness.screenshotDir = exeDir / "screenshots";
    }
    if (!opt.trace.empty())
        kh2trace::install(runtime, opt.trace); // last, so it wraps the final implementations

    harness.start = harness.lastStatus = std::chrono::steady_clock::now();
    std::printf("[kh2] starting at 0x%08X\n", runtime.cpu().pc);
    std::fflush(stdout);
    kh2movie::setSkip(opt.skipMovies);
    kh2puppets::install(runtime);
    kh2coop::init(opt.coop);
    if (opt.speed != 1.0)
    {
        runtime.eeScheduler().setHostSpeed(opt.speed);
        std::printf("[harness] host speed %s\n", opt.speed > 0.0 ? std::to_string(opt.speed).c_str() : "unpaced");
    }
    if (opt.profileMs)
        kh2prof::start(opt.profileMs);
    if (opt.watchdog || opt.headless)
        kh2watch::start(opt.watchdog ? opt.watchdog : 20);
    runtime.run();
    kh2coop::shutdown();
    kh2watch::stop();
    kh2prof::stopAndReport();
    kh2pgs::shutdown();

    if (!opt.headless && !(opt.frames || opt.timeoutSeconds > 0))
        return 0;
    std::printf("[harness] exit %d after %.1fs, guest frame %llu, overlay %s\n", harness.result,
                elapsed(harness), (unsigned long long)ps2PadCurrentGuestFrame(), kh2ovl::activeName());
    return harness.result;
}
