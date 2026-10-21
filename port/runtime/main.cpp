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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
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
    double statusEvery = 2.0;          // seconds between status lines
    std::string missing = "stop";      // missing-function policy
    std::string sound = "native";      // native: run the disc's sound IRX on the emulated IOP
    bool mute = false;
    std::string trace;                 // comma-separated guest function addresses
    bool skipMovies = false;           // --skip-movies
    double speed = 1.0;                // --speed: guest time vs wall time, 0 = as fast as possible
    unsigned profileMs = 0;            // --profile: sampling interval, 0 = off
    std::string dmaLog;                // FRAME,COUNT: GIF DMA chains to decode
    std::set<uint64_t> gsDumpAt;       // guest frames at which to dump GS state + VRAM
};

void usage()
{
    std::printf(
        "usage: kh2recomp <SLPM_666.75> [options]\n"
        "  --iso=PATH              disc image (needed for KH2.IMG/OVL.IMG sector reads)\n"
        "  --cd-root=DIR           directory for loose cdrom0: files (default: the ELF's)\n"
        "  --headless              no window; enables the test-harness options below\n"
        "  --frames=N              stop after N guest frames (vsyncs), exit 0\n"
        "  --timeout=SEC           give up after SEC wall-clock seconds, exit 3\n"
        "  --screenshot-every=N    save shots/frame_XXXXXX.png every N guest frames\n"
        "  --screenshot-at=A,B,..  save shots at these guest frames\n"
        "  --shots=DIR             screenshot directory (default: shots)\n"
        "  --input=SCRIPT          pad script, file or inline: '<frame> <BTN>[+BTN] [hold];...'\n"
        "                          buttons: UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE\n"
        "                                   START SELECT L1 R1 L2 R2 L3 R3\n"
        "  --status-every=SEC      status line interval (default 2, 0 = off)\n"
        "  --missing=stop|continue what to do on a call to untranslated code (default stop)\n"
        "  --sound=native|off      run LIBSD/LIBSSL on the emulated IOP, or stub them (default native)\n"
        "  --mute                  keep the sound driver running but play nothing\n"
        "  --trace=ADDR[,ADDR..]   log the first calls to these guest functions and count them\n"
        "  --skip-movies           end each movie as soon as it starts (debugging)\n"
        "  --speed=X               run guest time X times real time (0 = unpaced, as fast as possible)\n"
        "  --profile[=MS]          sample host threads every MS ms (default 1) and print hot functions\n"
        "  --dma-log=FRAME,COUNT   decode COUNT GIF-channel DMA chains from this guest frame on\n"
        "  --gs-dump=A,B,..        at these guest frames write GS registers and all of GS memory\n"
        "                          (as a 1024x1024 32-bit image) into the screenshot directory\n"
        "exit codes: 0 frame target reached, 1 runtime stopped early, 2 usage,\n"
        "            3 timeout, 4 initialisation failed\n");
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
        else if (a.rfind("--trace=", 0) == 0) o.trace = value(a);
        else if (a.rfind("--dma-log=", 0) == 0) o.dmaLog = value(a);
        else if (a == "--profile") o.profileMs = 1;
        else if (a == "--skip-movies") o.skipMovies = true;
        else if (a.rfind("--speed=", 0) == 0) o.speed = std::stod(value(a));
        else if (a.rfind("--profile=", 0) == 0) o.profileMs = std::max(1, std::stoi(value(a)));
        else if (a.rfind("--gs-dump=", 0) == 0)
        {
            std::stringstream ss(value(a));
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) o.gsDumpAt.insert(std::stoull(t));
        }
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
    uint64_t presented = 0;
    int result = kExitStoppedEarly;
    std::vector<uint8_t> pixels;
};

double elapsed(const Harness &h)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - h.start).count();
}

void saveShot(Harness &h, uint64_t frame)
{
    uint32_t w = 0, hgt = 0;
    h.pixels.clear();
    if (!h.runtime->gs().copyLatchedHostPresentationFrame(h.pixels, w, hgt) || w == 0 || hgt == 0)
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
        // Recent GS events (the frontend keeps the last 512), grouped by draw target.
        const auto history = gs.getDebugHistory();
        std::map<std::string, std::pair<int, std::string>> groups;
        int kinds[5] = {};
        for (const GSDebugHistoryEntry &e : history)
        {
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
            auto &g = groups[key];
            g.first++;
            g.second = eg;
        }
        std::fprintf(f, "history: %zu events (giftag %d, register %d, draw %d, transfer %d, present %d)\n",
                     history.size(), kinds[0], kinds[1], kinds[2], kinds[3], kinds[4]);
        for (const auto &[key, g] : groups)
            std::fprintf(f, "  draws x%-4d %s | e.g. %s\n", g.first, key.c_str(), g.second.c_str());
        // Then every recorded event in order, for following one frame's packets.
        static const char *kKinds[] = {"giftag", "reg", "draw", "xfer", "present"};
        for (const GSDebugHistoryEntry &e : history)
        {
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

// Called once per host loop iteration by the runtime when presenting externally.
bool headlessPump(void *userData)
{
    Harness &h = *static_cast<Harness *>(userData);
    const Options &o = *h.opt;
    const uint64_t frame = ps2PadCurrentGuestFrame();
    ++h.presented;
    // No window, so the runtime's present loop never polls input; publishing an
    // idle host state is what merges the --input script in (ps2_pad.cpp).
    ps2PadPublishHostState(0u, 0u, 0x80808080u);
    kh2movie::poll(*h.runtime); // TODO: also needs a per-frame hook in windowed mode

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
        std::printf("[status] t=%.1fs guest_frame=%llu overlay=%s overlay_switches=%u%s%s%s\n", elapsed(h),
                    (unsigned long long)frame, kh2ovl::activeName(), kh2ovl::switchCount(), mpeg,
                    o.trace.empty() ? "" : " calls:", kh2trace::summary().c_str());
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
} // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parse(argc, argv, opt))
    {
        usage();
        return kExitUsage;
    }
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
    if (opt.headless)
        runtime.setExternalPresenter(&headlessPump, &harness); // must precede initialize()

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
        if (opt.mute)
            ps2_native_iop::setVolume(0.0f);
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
    if (!opt.trace.empty())
        kh2trace::install(runtime, opt.trace); // last, so it wraps the final implementations

    harness.start = harness.lastStatus = std::chrono::steady_clock::now();
    std::printf("[kh2] starting at 0x%08X\n", runtime.cpu().pc);
    std::fflush(stdout);
    kh2movie::setSkip(opt.skipMovies);
    if (opt.speed != 1.0)
    {
        runtime.eeScheduler().setHostSpeed(opt.speed);
        std::printf("[harness] host speed %s\n", opt.speed > 0.0 ? std::to_string(opt.speed).c_str() : "unpaced");
    }
    if (opt.profileMs)
        kh2prof::start(opt.profileMs);
    runtime.run();
    kh2prof::stopAndReport();

    if (!opt.headless)
        return 0;
    std::printf("[harness] exit %d after %.1fs, guest frame %llu, overlay %s\n", harness.result,
                elapsed(harness), (unsigned long long)ps2PadCurrentGuestFrame(), kh2ovl::activeName());
    return harness.result;
}
