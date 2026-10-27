// GPU renderer built on paraLLEl-GS (third_party/parallel-gs, LGPL-3.0+), a
// Vulkan compute emulation of the PS2 Graphics Synthesizer.
//
// The runtime's own GS (a software rasteriser) still receives every GIF packet:
// it keeps the register state, FINISH/SIGNAL events and the transfer
// bookkeeping the game sees. A tap (patches/ps2recomp/0011) mirrors the same
// packets, plus register writes made by HLE helpers, into paraLLEl-GS, which
// does the drawing; the software rasteriser is switched off. Once per guest
// vsync the privileged registers (PMODE, DISPFBx, DISPLAYx, ...) are copied
// over and the frame is scanned out, then either presented into our own window
// (Win32 + Vulkan swapchain through Granite's WSI) or kept for read-back.
//
// The presentation code follows the PCSX2 integration that ships with
// paraLLEl-GS (misc/arch-pkgbuild-pcsx2, Hans-Kristian Arntzen, LGPL-3.0+).

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "pgs_renderer.h"
#if defined(__ANDROID__)
#include <android/native_window.h>
#include "android_host.h"
#endif
#include "hd_textures.h"

#include <algorithm>
#include <climits>
#include <atomic>
#if defined(__ANDROID__) || defined(__linux__)
#include <pthread.h>
#endif
#include <thread>
#include <deque>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "shaders/slangmosh.hpp"
#include "thread_id.hpp"
#include "wsi.hpp"

#include "imgui.h"
#if defined(_WIN32)
#include "backends/imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#include "runtime/gs/gs_gif_tap.h"
#include "runtime/ps2_memory.h"

using namespace Vulkan;
using namespace ParallelGS;

// A window of our own with a swapchain: Windows (Win32) and Android (the app's).
#if defined(_WIN32) || defined(__ANDROID__)
#define KH2_WSI 1
#else
#define KH2_WSI 0
#endif

namespace kh2pgs
{
void stopRenderThread();
void renderLoop();
namespace
{
Options g_opt;
std::mutex g_mutex; // GSInterface is single-threaded; the tap runs on guest threads
bool g_active = false;
bool g_closed = false;

// Headless: our own context and device. Windowed: owned by the WSI.
std::unique_ptr<Context> g_context;
std::unique_ptr<Device> g_headlessDevice;
Device *g_device = nullptr;
std::unique_ptr<GSInterface> g_gs;
ScanoutResult g_last;
// Render thread: the frame scanned out last time. Each frame presents the one
// before it, so the GPU has a whole frame to finish it before Android takes the
// buffer (its display queue waits for the previous frame's GPU work, and
// presenting at once made CPU and GPU take turns). One frame more latency.
std::deque<ScanoutResult> g_heldScanouts;
unsigned g_presentDelay = 1; // frames (2 tested: no better)
VkImageLayout g_lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;

Program *g_blitProgram = nullptr;
Program *g_uiProgram[2] = {nullptr, nullptr}; // [textured]

// On-screen UI (Dear ImGui): F1 toggles the settings menu, F2 the frame-rate
// display. The menu itself is drawn by a callback (overlay_ui.cpp).
std::function<void()> g_uiCallback;
std::function<void()> g_hudCallback;
std::function<void()> g_overlayCallback; // the runtime's own in-game menus (movie pause)
std::atomic<bool> g_overlayActive{false};
std::atomic<bool> g_hudVisible{false};
std::function<void()> g_worldCallback;
std::atomic<bool> g_worldActive{false};
std::atomic<float> g_picX{0}, g_picY{0}, g_picW{0}, g_picH{0};
std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_showFps{false};
bool g_imguiReady = false;
double g_toastUntil = 0.0;
std::string g_toastText = "F1 or Select+Start: settings   F2: frame rate   F3: game state   F12: screenshot   Alt+Enter: fullscreen";
std::mutex g_toastMutex;
std::atomic<bool> g_screenshotRequested{false};
Program *g_upscaleProgram = nullptr;
Program *g_sharpenProgram = nullptr;
ImageHandle g_fsrTarget;

// Granite looks up a per-thread index for its command pools. Every call into
// it happens under g_mutex, so all threads can share index 0; an unregistered
// thread would get 0 too, but with an error logged on every call.
std::atomic<int> g_pendingSsaa{0};        // 0: no change
std::atomic<int> g_pendingSsTextures{-1}; // -1: no change
std::atomic<int> g_pendingSharpest{-1};

void registerThread()
{
    static thread_local bool registered = false;
    if (!registered)
    {
        Util::register_thread_index(0);
        registered = true;
    }
}

// GIF data from the guest side is queued, not fed to paraLLEl-GS directly:
// the render thread holds g_mutex through scanout and presentation (which can
// wait for the display), and the EE thread must never wait for that. The queue
// is drained in order before every scanout and between frames. paraLLEl-GS
// only draws what is shown; everything the guest can observe of the GS stays
// with the runtime's own GS, so feeding it a little later is safe.
std::mutex g_queueMutex;
std::condition_variable g_queueDrained;

// Render thread (Options::renderThread; on by default on Android). The guest's
// vsync hands its frame over as a job (the GIF packets since the last one and
// the display registers) and goes on; this thread feeds paraLLEl-GS, answers
// readbacks as soon as their packets are in, and scans out and presents each
// job in order. Without it all of that ran in the vsync call itself, and the
// guest waited on it (on Android, mostly on GPU submissions).
struct PrivSnapshot
{
    uint64_t regs[15] = {};
};
struct FrameJob
{
    std::vector<uint8_t> packets;
    uint64_t end = 0; // packet count after them
    PrivSnapshot priv;
    uint64_t tick = 0;
    bool redraw = false; // present again without a new guest frame (paused)
};
std::deque<FrameJob> g_jobs; // under g_queueMutex
std::condition_variable g_renderWake, g_jobDone;
std::thread g_renderThread;
std::atomic<bool> g_renderStop{false};
std::atomic<bool> g_rbWake{false};
bool g_renderThreadOn = false;
constexpr size_t kMaxJobsAhead = 2;
// Where the time goes (logged with the GPU times): guest waits for readbacks
// and for a full queue, and the render thread feeding paraLLEl-GS.
// Main-thread frame steps (ns per 3 s): drain, flush, vsync, present parts.
std::atomic<uint64_t> g_stepNs[8];
const char *const kStepNames[8] = {"drain", "pgs flush", "pgs vsync", "begin_frame", "blit+ui", "submit", "end_frame", "next begin_frame"};
struct StepTimer
{
    int i;
    uint64_t t0;
    explicit StepTimer(int idx);
    ~StepTimer();
};
std::atomic<uint64_t> g_statReadbacksSkipped{0}, g_statReadbacksAsync{0}, g_statReadbacks{0}, g_statReadbackNs{0}, g_statQueueWaitNs{0}, g_statDrainNs{0},
    g_statReadbackServeNs{0};
uint64_t statNow()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}
StepTimer::StepTimer(int idx) : i(idx), t0(statNow()) {}
StepTimer::~StepTimer() { g_stepNs[i] += statNow() - t0; }
std::vector<uint8_t> g_queue;    // [u32 size][size bytes]...
std::vector<uint8_t> g_draining; // the render thread's side of the swap
uint64_t g_enqueued = 0;         // packets queued so far (under g_queueMutex)
uint64_t g_processed = 0;        // packets fed to paraLLEl-GS (render thread)
size_t g_lastPacketAt = 0;       // offset in g_queue of the newest packet (under g_queueMutex)
constexpr size_t kQueueLimit = 256u << 20; // past this the producer waits (render thread stuck)

// KH2_PGS_TRACE_FBP=<fbp>[:fromVsync[:lines]]: logs every change of drawing
// state for primitives drawn into that frame buffer (colour, texture, blend),
// parsed from the GIF stream as it is queued. Diagnostics only.
struct GifTrace
{
    bool on = false;
    uint32_t fbp = 0;
    uint64_t from = 0, to = ~0ull;
    int left = 0;
    uint64_t vsync = 0;
    uint64_t prim = 0, rgbaq = 0, frame[2] = {}, tex0[2] = {}, alpha[2] = {}, test[2] = {}, zbuf[2] = {}, bitblt = 0, trxpos = 0, trxreg = 0, scissor[2] = {}, xyoff[2] = {};
    std::string last;
    int big = 0;          // KH2_PGS_TRACE_BIG=<px>: log sprites at least this big
    int vtx = 0;          // vertices since the last PRIM write
    uint32_t vx = 0, vy = 0;
    GifTrace()
    {
        if (const char *b = std::getenv("KH2_PGS_TRACE_BIG"))
            big = std::atoi(b);
        const char *e = std::getenv("KH2_PGS_TRACE_FBP");
        if (!e)
            return;
        unsigned long long a = 0, b = 0, t = ~0ull;
        int n = 2000;
        std::sscanf(e, "%llu:%llu:%d:%llu", &a, &b, &n, &t);
        to = t;
        on = true;
        fbp = uint32_t(a);
        from = b;
        left = n;
    }
    void kick(uint32_t x, uint32_t y, uint32_t z = 0)
    {
        if (big > 0 && (prim & 7) == 6 && vsync >= from && vsync <= to && left > 0)
        {
            if ((vtx++ & 1) == 0)
            {
                vx = x;
                vy = y;
            }
            else
            {
                const int w = std::abs(int(x) - int(vx)) >> 4, h = std::abs(int(y) - int(vy)) >> 4;
                if (w >= big || h >= big)
                {
                    const int c = int((prim >> 9) & 1);
                    --left;
                    std::fprintf(stderr, "[big] v%llu fbp=%llu prim=%03llx %dx%d at %u,%u rgba=%08llx tex0=%016llx alpha=%llx\n",
                                 (unsigned long long)vsync, (unsigned long long)(frame[c] & 0x1FF),
                                 (unsigned long long)(prim & 0x7FF), w, h, std::min(vx, x) >> 4, std::min(vy, y) >> 4,
                                 (unsigned long long)(rgbaq & 0xFFFFFFFF), (unsigned long long)tex0[c],
                                 (unsigned long long)alpha[c]);
                }
            }
            return;
        }
        if (big > 0)
            return;
        if (vsync < from || vsync > to || left <= 0)
            return;
        const int c = int((prim >> 9) & 1);
        if (fbp != 999 && (frame[c] & 0x1FF) != fbp)
            return;
        char key[400];
        std::snprintf(key, sizeof(key), "fb=%llx prim=%03llx rgba=%08llx tex0=%016llx alpha=%llx test=%llx fbmsk=%08llx zbuf=%llx sc=%llx of=%llx sz=%x",
                      (unsigned long long)(frame[c] & 0xFFFFFFFF), (unsigned long long)(prim & 0x7FF), (unsigned long long)(rgbaq & 0xFFFFFFFF),
                      (unsigned long long)tex0[c], (unsigned long long)alpha[c], (unsigned long long)test[c],
                      (unsigned long long)(frame[c] >> 32), (unsigned long long)zbuf[c], (unsigned long long)scissor[c], (unsigned long long)xyoff[c], (prim & 7) == 6 ? z : 0u);
        if (last == key)
            return;
        last = key;
        --left;
        std::fprintf(stderr, "[trace] v%llu fbp=%u %s xy=%u,%u z=%08x\n", (unsigned long long)vsync, uint32_t(frame[c] & 0x1FF), key, x >> 4, y >> 4, z);
    }
    void clut(uint32_t r, uint64_t v)
    {
        const uint32_t cld = uint32_t(v >> 61);
        if (cld == 0 || vsync < from || vsync > to || left <= 0)
            return;
        --left;
        std::fprintf(stderr, "[trace] v%llu clut reg=%02x cld=%u cbp=%llu cpsm=%llu csm=%llu csa=%llu (tbp=%llu psm=%llu)\n",
                     (unsigned long long)vsync, r, cld, (unsigned long long)((v >> 37) & 0x3FFF),
                     (unsigned long long)((v >> 51) & 0xF), (unsigned long long)((v >> 55) & 1),
                     (unsigned long long)((v >> 56) & 0x1F), (unsigned long long)(v & 0x3FFF),
                     (unsigned long long)((v >> 20) & 0x3F));
    }
    void reg(uint32_t r, uint64_t v)
    {
        switch (r)
        {
        case 0x00: prim = v; vtx = 0; break;
        case 0x01: rgbaq = v; break;
        case 0x04:
        case 0x05: kick(uint32_t(v & 0xFFFF), uint32_t((v >> 16) & 0xFFFF), uint32_t(v >> 32)); break;
        case 0x06: tex0[0] = v; clut(r, v); break;
        case 0x07: tex0[1] = v; clut(r, v); break;
        case 0x16:
        case 0x17: clut(r, v); break;
        case 0x42: alpha[0] = v; break;
        case 0x43: alpha[1] = v; break;
        case 0x47: test[0] = v; break;
        case 0x48: test[1] = v; break;
        case 0x40: scissor[0] = v; break;
        case 0x41: scissor[1] = v; break;
        case 0x18: xyoff[0] = v; break;
        case 0x19: xyoff[1] = v; break;
        case 0x50: bitblt = v; break;
        case 0x51: trxpos = v; break;
        case 0x52: trxreg = v; break;
        case 0x53:
            if (vsync >= from && vsync <= to && left > 0)
            {
                --left;
                std::fprintf(stderr, "[trace] v%llu xfer dir=%llu sbp=%llu sbw=%llu spsm=%llx dbp=%llu dbw=%llu dpsm=%llx pos=%llx size=%llux%llu\n",
                             (unsigned long long)vsync, (unsigned long long)(v & 3), (unsigned long long)(bitblt & 0x3FFF),
                             (unsigned long long)((bitblt >> 16) & 0x3F), (unsigned long long)((bitblt >> 24) & 0x3F),
                             (unsigned long long)((bitblt >> 32) & 0x3FFF), (unsigned long long)((bitblt >> 48) & 0x3F),
                             (unsigned long long)((bitblt >> 56) & 0x3F), (unsigned long long)trxpos,
                             (unsigned long long)(trxreg & 0xFFF), (unsigned long long)((trxreg >> 32) & 0xFFF));
            }
            break;
        case 0x4e: zbuf[0] = v; break;
        case 0x4f: zbuf[1] = v; break;
        case 0x4c: frame[0] = v; break;
        case 0x4d: frame[1] = v; break;
        default: break;
        }
    }
    void packet(const uint8_t *data, uint32_t size)
    {
        uint32_t at = 0;
        while (at + 16 <= size)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, data + at, 8);
            std::memcpy(&hi, data + at + 8, 8);
            at += 16;
            const uint32_t nloop = uint32_t(lo & 0x7FFF);
            const uint32_t flg = uint32_t((lo >> 58) & 3);
            uint32_t nreg = uint32_t(lo >> 60);
            if (nreg == 0)
                nreg = 16;
            if ((lo >> 46) & 1)
            {
                prim = (lo >> 47) & 0x7FF;
                vtx = 0;
            }
            if (flg == 0)
            {
                for (uint32_t i = 0; i < nloop; ++i)
                    for (uint32_t j = 0; j < nreg && at + 16 <= size; ++j, at += 16)
                    {
                        const uint32_t r = uint32_t((hi >> (4 * j)) & 0xF);
                        uint32_t w[4];
                        std::memcpy(w, data + at, 16);
                        uint64_t d0;
                        std::memcpy(&d0, data + at, 8);
                        switch (r)
                        {
                        case 0x0: reg(0x00, d0); break;
                        case 0x1:
                            rgbaq = (w[0] & 0xFF) | ((w[1] & 0xFF) << 8) | ((w[2] & 0xFF) << 16) | (uint64_t(w[3] & 0xFF) << 24);
                            break;
                        case 0x4:
                        case 0x5:
                            if (!((w[3] >> 15) & 1))
                                kick(w[0] & 0xFFFF, w[1] & 0xFFFF, r == 0x5 ? w[2] : (w[2] >> 4) & 0xFFFFFF);
                            break;
                        case 0xE:
                        {
                            uint64_t a;
                            std::memcpy(&a, data + at + 8, 8);
                            reg(uint32_t(a & 0xFF), d0);
                            break;
                        }
                        default:
                            if (r >= 0x6 && r <= 0xD)
                                reg(r, d0);
                            break;
                        }
                    }
            }
            else if (flg == 1)
            {
                const uint32_t count = nloop * nreg;
                for (uint32_t k = 0; k < count && at + 8 <= size; ++k)
                {
                    uint64_t v;
                    std::memcpy(&v, data + at + 8 * (k & 1), 8);
                    reg(uint32_t((hi >> (4 * (k % nreg))) & 0xF), v);
                    if (k & 1)
                        at += 16;
                }
                if (count & 1)
                    at += 16;
            }
            else
                at += nloop * 16;
        }
    }
};
GifTrace g_trace;

void tapPacket(const uint8_t *data, uint32_t size, void *)
{
    size &= ~15u;
    if (size == 0)
        return;
    if (g_trace.on)
        g_trace.packet(data, size);
    std::unique_lock<std::mutex> lock(g_queueMutex);
    // Render thread: the guest stays at most kMaxJobsAhead frames ahead of the
    // screen (nothing else held it back once readbacks stopped waiting, and it
    // ran ahead drawing frames nobody would see).
    if (g_renderThreadOn && g_jobs.size() >= kMaxJobsAhead && !g_renderStop)
    {
        const uint64_t waitStart = statNow();
        g_jobDone.wait_for(lock, std::chrono::milliseconds(200),
                           [] { return g_jobs.size() < kMaxJobsAhead || g_renderStop.load(); });
        g_statQueueWaitNs += statNow() - waitStart;
    }
    bool queueOk = true;
    if (g_queue.size() > kQueueLimit)
    {
        const uint64_t waitStart = statNow();
        queueOk = g_queueDrained.wait_for(lock, std::chrono::milliseconds(100), [] { return g_queue.size() <= kQueueLimit; });
        g_statQueueWaitNs += statNow() - waitStart;
    }
    if (!queueOk)
    {
        // Nobody is drawing (shutting down, or the render thread is stuck):
        // drop the backlog rather than crawl the guest along 100 ms a packet.
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            std::fprintf(stderr, "[pgs] GS queue full and not drained; dropping it\n");
        }
        g_queue.clear();
    }
    const size_t at = g_queue.size();
    g_lastPacketAt = at;
    g_queue.resize(at + 4 + size);
    std::memcpy(g_queue.data() + at, &size, 4);
    std::memcpy(g_queue.data() + at + 4, data, size);
    ++g_enqueued;
}

// The guest reading GS memory back (local -> host: KH2 copies the finished
// frame for its pause and battle backgrounds). Only paraLLEl-GS has what was
// drawn, so the runtime's GS asks here (ps2SetGsReadback) and waits until
// the render thread has fed paraLLEl-GS everything up to that transfer,
// which then reads its VRAM.
std::mutex g_rbMutex;
std::condition_variable g_rbCond;
bool g_rbPending = false, g_rbDone = false;
uint64_t g_rbAfter = 0; // the request's packets are queued once this many were
size_t g_rbBytes = 0;
std::vector<uint8_t> g_rbData;
// Small reads that repeat every frame (on Android: KH2's 32x8 depth probe for
// the sun's lens flare) get the previous result at once and are refreshed in
// the background; waiting for the GPU each frame cost ~20-30 ms there.
bool g_rbAsync = false;
std::string g_rbKey;
std::map<std::string, std::vector<uint8_t>> g_rbCache;
std::atomic<uint64_t> g_rbAfterAtomic{UINT64_MAX}; // packet index to serve at, mid-batch
constexpr size_t kAsyncReadbackMaxBytes = 16u << 10;
// Answering a readback from the cache and dropping it from the stream (no GPU
// read at all) made things far slower on Turnip: the readback's flush was what
// kept paraLLEl-GS's batches small. Kept for experiments.
constexpr bool kCancelReadbacks = true;

uint32_t transferBits(uint32_t psm)
{
    switch (psm)
    {
    case 0x00: case 0x30: return 32;
    case 0x01: case 0x31: return 24;
    case 0x02: case 0x0A: case 0x32: case 0x3A: return 16;
    case 0x13: return 8;
    case 0x14: return 4;
    default: return 0;
    }
}

// Turns the newest queued packet's TRXDIR = 1 (local -> host) into 3 (transfer
// off), so paraLLEl-GS does not flush and wait for the GPU for a readback that
// is answered from the cache. Under g_queueMutex. False if it is not there.
bool cancelQueuedReadback()
{
    if (g_lastPacketAt + 4 > g_queue.size())
        return false;
    uint32_t size;
    std::memcpy(&size, g_queue.data() + g_lastPacketAt, 4);
    uint8_t *pkt = g_queue.data() + g_lastPacketAt + 4;
    if (g_lastPacketAt + 4 + size > g_queue.size())
        return false;
    uint64_t *found = nullptr;
    size_t q = 0;
    const size_t qwords = size / 16u;
    while (q < qwords)
    {
        uint64_t tag[2];
        std::memcpy(tag, pkt + q * 16, 16);
        ++q;
        const uint32_t nloop = uint32_t(tag[0] & 0x7FFF);
        const uint32_t flg = uint32_t((tag[0] >> 58) & 3);
        uint32_t nreg = uint32_t((tag[0] >> 60) & 15);
        if (nreg == 0)
            nreg = 16;
        if (flg == 0) // PACKED
        {
            for (uint32_t l = 0; l < nloop && q < qwords; ++l)
                for (uint32_t r = 0; r < nreg && q < qwords; ++r, ++q)
                {
                    const uint32_t desc = uint32_t((tag[1] >> (4 * r)) & 15);
                    if (desc != 0xE)
                        continue;
                    uint64_t *data = reinterpret_cast<uint64_t *>(pkt + q * 16);
                    if ((data[1] & 0xFF) == 0x53 && (data[0] & 3) == 1)
                        found = data;
                }
        }
        else if (flg == 1) // REGLIST
            q += (size_t(nloop) * nreg + 1) / 2;
        else // IMAGE
            q += nloop;
    }
    if (!found)
        return false;
    found[0] = (found[0] & ~3ull) | 3ull;
    return true;
}

bool readbackFromGpu(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t ssax, uint32_t ssay, uint32_t rrw, uint32_t rrh,
                     std::vector<uint8_t> &out, void *)
{
    static int logged = 0;
    static const int logLimit = std::getenv("KH2_PGS_READBACK_LOG") ? std::atoi(std::getenv("KH2_PGS_READBACK_LOG"))
#if defined(__ANDROID__)
                                                                        : 40;
#else
                                                                        : 0;
#endif
    if (logged++ < logLimit)
        std::printf("[pgs] readback sbp %u sbw %u psm 0x%02X at %u,%u %ux%u\n", sbp, sbw, spsm, ssax, ssay, rrw, rrh);
    static const bool disabled = std::getenv("KH2_PGS_NO_READBACK") != nullptr; // diagnostics
    if (!g_gs || g_opt.keepSoftwareRaster || disabled)
        return false;
    const uint32_t bits = transferBits(spsm);
    if (!bits || !rrw || !rrh)
        return false;
    uint64_t after;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        after = g_enqueued;
    }
    std::unique_lock<std::mutex> lock(g_rbMutex);
    const uint64_t rbStart = statNow();
#if defined(__ANDROID__)
    // A background read still in flight: answer this one from the cache too,
    // without waiting for it.
    if (g_rbPending && g_rbAsync && size_t(rrw) * rrh * bits / 8u <= kAsyncReadbackMaxBytes)
    {
        char k[96];
        std::snprintf(k, sizeof(k), "%u/%u/%u/%ux%u", sbp, sbw, spsm, rrw, rrh);
        auto it = g_rbCache.find(k);
        if (it != g_rbCache.end())
        {
            bool cancelled;
            {
                std::lock_guard<std::mutex> q(g_queueMutex);
                cancelled = kCancelReadbacks && cancelQueuedReadback();
            }
            if (cancelled)
            {
                out = it->second;
                g_statReadbacksSkipped++;
                return true;
            }
        }
    }
#endif
    // An earlier background read is served first (it is ahead in the stream).
    if (g_rbPending && !g_rbCond.wait_for(lock, std::chrono::seconds(2), [] { return !g_rbPending; }))
    {
        g_rbPending = false;
        g_rbAfterAtomic = UINT64_MAX;
    }
    g_rbBytes = size_t(rrw) * rrh * bits / 8u;
    g_rbAfter = after;
    g_rbDone = false;
    char key[96];
    // Not the position: KH2's sun-flare probe follows the sun across the screen
    // as the camera turns, and a miss there meant a full GPU round trip.
    std::snprintf(key, sizeof(key), "%u/%u/%u/%ux%u", sbp, sbw, spsm, rrw, rrh);
    g_rbKey = key;
#if defined(__ANDROID__)
    if (g_rbBytes <= kAsyncReadbackMaxBytes)
    {
        auto it = g_rbCache.find(g_rbKey);
        if (it != g_rbCache.end())
        {
            out = it->second;
            // Refresh every 8th time; the others skip paraLLEl-GS's flush and GPU wait.
            static unsigned s_refresh = 0;
            if ((++s_refresh & 7u) != 0)
            {
                bool cancelled;
                {
                    std::lock_guard<std::mutex> q(g_queueMutex);
                    cancelled = kCancelReadbacks && cancelQueuedReadback();
                }
                if (cancelled)
                {
                    g_statReadbacksSkipped++;
                    return true;
                }
            }
            g_rbAsync = true;
            g_rbPending = true;
            g_rbAfterAtomic = after;
            g_statReadbacksAsync++;
            g_rbWake = true;
            g_renderWake.notify_one();
            g_statReadbackNs += statNow() - rbStart;
            return true;
        }
    }
#endif
    g_rbAsync = false;
    g_rbPending = true;
    g_rbAfterAtomic = after;
    g_rbWake = true;
    g_renderWake.notify_one();
    const bool rbOk = g_rbCond.wait_for(lock, std::chrono::seconds(2), [] { return g_rbDone; });
    g_statReadbacks++;
    g_statReadbackNs += statNow() - rbStart;
    if (!rbOk)
    {
        g_rbPending = false;
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            std::fprintf(stderr, "[pgs] GS readback timed out; the guest gets stale data\n");
        }
        return false;
    }
    out.swap(g_rbData);
    g_rbData.clear();
    static int dumped = 0;
    if (std::getenv("KH2_PGS_READBACK_DUMP") && dumped < 4)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "readback_%d_%ux%u_psm%02X.bin", dumped++, rrw, rrh, spsm);
        if (FILE *f = std::fopen(name, "wb"))
        {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
        }
    }
    return true;
}

// Render thread, under g_mutex, after a drain.
void serviceReadback()
{
    std::lock_guard<std::mutex> lock(g_rbMutex);
    if (!g_rbPending || g_processed < g_rbAfter || !g_gs)
        return;
    const uint32_t words = uint32_t((g_rbBytes + 15u) / 16u);
    g_rbData.assign(size_t(words) * 16u, 0);
    const uint64_t serveStart = statNow();
    g_gs->read_transfer_fifo(g_rbData.data(), words);
    g_statReadbackServeNs += statNow() - serveStart;
    g_rbData.resize(g_rbBytes);
    g_rbPending = false;
    g_rbAfterAtomic = UINT64_MAX;
    if (g_rbBytes <= kAsyncReadbackMaxBytes)
        g_rbCache[g_rbKey] = g_rbData;
    if (g_rbAsync)
        g_rbData.clear();
    else
        g_rbDone = true;
    g_rbCond.notify_all();
}

// --gif-capture: 1 = start at the next frame(), 2 = capturing until the one after.
std::mutex g_capMutex;
std::string g_capPath;
int g_capState = 0;
FILE *g_capFile = nullptr;
size_t g_capBytes = 0;

// Render thread, under g_mutex.
// Feeds packets to paraLLEl-GS (render thread, under g_mutex); batchEnd is the
// packet count after them.
void feedPackets(std::vector<uint8_t> &buf, uint64_t batchEnd)
{
    const uint64_t drainStart = statNow();
    uint64_t packet = g_processed;
    if (g_gs)
    {
        size_t at = 0;
        while (at + 4 <= buf.size())
        {
            uint32_t size;
            std::memcpy(&size, buf.data() + at, 4);
            if (g_capFile)
            {
                std::fwrite(buf.data() + at, 1, 4u + size, g_capFile);
                g_capBytes += 4u + size;
            }
            g_gs->gif_transfer(3, buf.data() + at + 4, size);
            at += 4 + size;
            // A background readback is read right where it was asked for, before
            // what the guest drew after it.
            if (++packet == g_rbAfterAtomic.load(std::memory_order_relaxed))
            {
                g_processed = packet;
                serviceReadback();
            }
        }
    }
    g_statDrainNs += statNow() - drainStart;
    g_processed = batchEnd;
    serviceReadback();
}

void drainQueue()
{
    uint64_t batchEnd;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_jobs.empty())
            return; // frames handed over come first (render thread)
        if (g_queue.empty())
        {
            g_processed = g_enqueued;
            serviceReadback();
            return;
        }
        g_draining.swap(g_queue);
        batchEnd = g_enqueued;
    }
    g_queueDrained.notify_all();
    feedPackets(g_draining, batchEnd);
    g_draining.clear();
    if (g_draining.capacity() > (64u << 20))
        g_draining.shrink_to_fit();
}

void tapRegister(uint8_t reg, uint64_t value, void *)
{
    // As a one-register A+D packet.
    alignas(16) uint64_t packet[4];
    packet[0] = 1ull | (1ull << 15) | (1ull << 60); // NLOOP=1, EOP, PACKED, NREG=1
    packet[1] = 0xEull;                             // REGS = A+D
    packet[2] = value;
    packet[3] = reg;
    tapPacket(reinterpret_cast<const uint8_t *>(packet), sizeof(packet), nullptr);
}

SuperSampling toSuperSampling(unsigned n)
{
    switch (n)
    {
    case 2: return SuperSampling::X2;
    case 4: return SuperSampling::X4;
    case 8: return SuperSampling::X8;
    case 16: return SuperSampling::X16;
    default: return SuperSampling::X1;
    }
}

#if defined(_WIN32)
// ---- Window -----------------------------------------------------------------

HWND g_hwnd = nullptr;
std::atomic<bool> g_focused{false};
std::atomic<uint32_t> g_clientW{0}, g_clientH{0};
bool g_fullscreen = false;
void (*g_placedFn)(int, int, int, int) = nullptr;
void reportPlacement()
{
    RECT r;
    if (g_placedFn && g_hwnd && !g_fullscreen && !IsIconic(g_hwnd) && GetWindowRect(g_hwnd, &r))
        g_placedFn(r.left, r.top, r.right - r.left, r.bottom - r.top);
}
WINDOWPLACEMENT g_windowedPlacement{sizeof(WINDOWPLACEMENT)};

void toggleFullscreen()
{
    const DWORD style = GetWindowLong(g_hwnd, GWL_STYLE);
    if (!g_fullscreen)
    {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(g_hwnd, &g_windowedPlacement) &&
            GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
        {
            SetWindowLong(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fullscreen = true;
        }
    }
    else
    {
        SetWindowLong(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_windowedPlacement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_imguiReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return 1;
    switch (msg)
    {
    case WM_CLOSE:
        reportPlacement();
        g_closed = true;
        return 0;
    case WM_EXITSIZEMOVE:
        reportPlacement();
        break;
    case WM_SIZE:
        g_clientW = LOWORD(lp);
        g_clientH = HIWORD(lp);
        return 0;
    case WM_ACTIVATE:
        g_focused = LOWORD(wp) != WA_INACTIVE;
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) // Alt+Enter
        {
            toggleFullscreen();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11)
        {
            toggleFullscreen();
            return 0;
        }
        if (wp == VK_F1)
        {
            g_menuOpen = !g_menuOpen;
            return 0;
        }
        if (wp == VK_F2)
        {
            g_showFps = !g_showFps;
            return 0;
        }
        if (wp == VK_F3)
        {
            g_hudVisible = !g_hudVisible;
            return 0;
        }
        if (wp == VK_F12)
        {
            g_screenshotRequested = true;
            return 0;
        }
        break;
    case WM_SYSCHAR: // no menu beep on Alt+Enter
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool createWindow()
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.lpszClassName = L"KH2RecompWindow";
    RegisterClassExW(&wc);
    RECT r{0, 0, (LONG)g_opt.windowWidth, (LONG)g_opt.windowHeight};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (g_opt.windowOuterW > 100 && g_opt.windowOuterH > 100)
    {
        w = g_opt.windowOuterW;
        h = g_opt.windowOuterH;
    }
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (g_opt.windowX != INT_MIN && g_opt.windowY != INT_MIN)
    {
        // Only where a monitor still is (a saved place on an unplugged screen).
        const RECT want{g_opt.windowX, g_opt.windowY, g_opt.windowX + w, g_opt.windowY + h};
        if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL))
        {
            x = g_opt.windowX;
            y = g_opt.windowY;
        }
    }
    std::wstring title(g_opt.title.size() + 1, L'\0');
    title.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, g_opt.title.c_str(), -1, title.data(), int(title.size())) > 0
                            ? std::wcslen(title.c_str())
                            : 0));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, title.empty() ? L"Kingdom Hearts II Final Mix (native)" : title.c_str(),
                             WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd)
        return false;
    ShowWindow(g_hwnd, SW_SHOW);
    RECT client;
    GetClientRect(g_hwnd, &client);
    g_clientW = client.right;
    g_clientH = client.bottom;
    g_focused = GetForegroundWindow() == g_hwnd;
    if (g_opt.fullscreen)
        toggleFullscreen();
    return true;
}

std::atomic<uint32_t> g_requestedSize{0}; // w << 16 | h, applied on the window thread

void pumpMessages()
{
    if (const uint32_t req = g_requestedSize.exchange(0))
    {
        if (g_hwnd && !g_fullscreen)
        {
            RECT r{0, 0, LONG(req >> 16), LONG(req & 0xFFFF)};
            AdjustWindowRect(&r, GetWindowLong(g_hwnd, GWL_STYLE), FALSE);
            SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

class Platform final : public WSIPlatform
{
public:
    VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override
    {
        VkWin32SurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hinstance = GetModuleHandleW(nullptr);
        info.hwnd = g_hwnd;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return surface;
    }
    std::vector<const char *> get_instance_extensions() override
    {
        return {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    }
    std::vector<const char *> get_device_extensions() override { return {VK_KHR_SWAPCHAIN_EXTENSION_NAME}; }
    uint32_t get_surface_width() override { return std::max<uint32_t>(1, g_clientW.load()); }
    uint32_t get_surface_height() override { return std::max<uint32_t>(1, g_clientH.load()); }
    bool alive(WSI &) override { return !g_closed; }
    void poll_input() override {}
    void poll_input_async(Granite::InputTrackerHandler *) override {}
    void event_swapchain_destroyed() override
    {
        WSIPlatform::event_swapchain_destroyed();
        g_inFrame = false;
    }
    const VkApplicationInfo *get_application_info() override
    {
        static const VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "kh2recomp", 0, "Granite", 0,
                                              VK_API_VERSION_1_3};
        return &app;
    }
    bool g_inFrame = false;
};

std::unique_ptr<Platform> g_platform;
std::unique_ptr<WSI> g_wsi;
uint32_t g_lastSurfaceW = 0, g_lastSurfaceH = 0;
#elif defined(__ANDROID__)
// ---- Window (Android: the app's, kh2android) -----------------------------------
std::atomic<bool> g_focused{true};
std::atomic<uint32_t> g_clientW{0}, g_clientH{0};
void (*g_placedFn)(int, int, int, int) = nullptr;

void updateSize()
{
    if (ANativeWindow *w = kh2android::window())
    {
        g_clientW = uint32_t(std::max(0, ANativeWindow_getWidth(w)));
        g_clientH = uint32_t(std::max(0, ANativeWindow_getHeight(w)));
    }
    else
        g_clientW = g_clientH = 0;
}

void pumpMessages()
{
    kh2android::pump();
    g_focused = kh2android::focused();
    if (kh2android::quitRequested())
        g_closed = true;
    updateSize();
    if (kh2android::takeBack())
        g_menuOpen = !g_menuOpen;
}

bool createWindow()
{
    updateSize();
    return kh2android::window() != nullptr;
}

class Platform final : public WSIPlatform
{
public:
    VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override
    {
        ANativeWindow *window = kh2android::window();
        if (!window)
            return VK_NULL_HANDLE;
        struct AndroidSurfaceInfo
        {
            VkStructureType sType;
            const void *pNext;
            VkFlags flags;
            ANativeWindow *window;
        } info = {VkStructureType(1000008000) /* VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR */, nullptr, 0, window};
        using CreateFn = VkResult(VKAPI_PTR *)(VkInstance, const AndroidSurfaceInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);
        auto create = reinterpret_cast<CreateFn>(vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!create || create(instance, &info, nullptr, &surface) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return surface;
    }
    std::vector<const char *> get_instance_extensions() override
    {
        return {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_android_surface"};
    }
    std::vector<const char *> get_device_extensions() override { return {VK_KHR_SWAPCHAIN_EXTENSION_NAME}; }
    uint32_t get_surface_width() override { return std::max<uint32_t>(1, g_clientW.load()); }
    uint32_t get_surface_height() override { return std::max<uint32_t>(1, g_clientH.load()); }
    bool alive(WSI &) override { return !g_closed; }
    void poll_input() override {}
    void poll_input_async(Granite::InputTrackerHandler *) override {}
    void event_swapchain_destroyed() override
    {
        WSIPlatform::event_swapchain_destroyed();
        g_inFrame = false;
    }
    const VkApplicationInfo *get_application_info() override
    {
        static const VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "kh2recomp", 0, "Granite", 0,
                                              VK_API_VERSION_1_1};
        return &app;
    }
    bool g_inFrame = false;
};

std::unique_ptr<Platform> g_platform;
std::unique_ptr<WSI> g_wsi;
uint32_t g_lastSurfaceW = 0, g_lastSurfaceH = 0;
bool g_surfaceLost = false;

// The app went to the background: its window goes, and the swapchain with it.
void onWindowLost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_wsi || g_surfaceLost)
        return;
    registerThread();
    if (g_platform->g_inFrame)
    {
        g_wsi->end_frame();
        g_platform->g_inFrame = false;
    }
    g_wsi->get_device().wait_idle();
    g_wsi->deinit_surface_and_swapchain();
    g_surfaceLost = true;
    std::printf("[pgs] window gone (in the background)\n");
}

// Back: a swapchain for the new window.
void onWindowRegained()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_wsi || !g_surfaceLost)
        return;
    registerThread();
    updateSize();
    const Context &ctx = g_wsi->get_context();
    VkSurfaceKHR surface = g_platform->create_surface(ctx.get_instance(), ctx.get_gpu());
    if (surface == VK_NULL_HANDLE)
    {
        std::fprintf(stderr, "[pgs] could not make a surface for the new window\n");
        return;
    }
    g_wsi->reinit_surface_and_swapchain(surface);
    g_surfaceLost = false;
    std::printf("[pgs] window back (%ux%u)\n", g_clientW.load(), g_clientH.load());
}

// ImGui without a platform backend: the display size, time, the gamepad (for
// moving through the menu) and touch (as the mouse).
void imguiAndroidNewFrame()
{
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(std::max<uint32_t>(1, g_clientW.load())), float(std::max<uint32_t>(1, g_clientH.load())));
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last = 0.0;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    io.DeltaTime = last > 0.0 ? float(std::max(1e-4, t - last)) : 1.0f / 60.0f;
    last = t;
    const kh2android::Pad pad = kh2android::pad();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto key = [&](ImGuiKey k, uint32_t bit) { io.AddKeyEvent(k, (pad.buttons & bit) != 0); };
    key(ImGuiKey_GamepadDpadUp, kh2android::PadUp);
    key(ImGuiKey_GamepadDpadDown, kh2android::PadDown);
    key(ImGuiKey_GamepadDpadLeft, kh2android::PadLeft);
    key(ImGuiKey_GamepadDpadRight, kh2android::PadRight);
    key(ImGuiKey_GamepadFaceDown, kh2android::PadA);
    key(ImGuiKey_GamepadFaceRight, kh2android::PadB);
    key(ImGuiKey_GamepadFaceLeft, kh2android::PadX);
    key(ImGuiKey_GamepadFaceUp, kh2android::PadY);
    key(ImGuiKey_GamepadL1, kh2android::PadL1);
    key(ImGuiKey_GamepadR1, kh2android::PadR1);
    key(ImGuiKey_GamepadL2, kh2android::PadL2);
    key(ImGuiKey_GamepadR2, kh2android::PadR2);
    key(ImGuiKey_GamepadStart, kh2android::PadStart);
    key(ImGuiKey_GamepadBack, kh2android::PadSelect);
    auto stick = [&](ImGuiKey neg, ImGuiKey pos, float v) {
        io.AddKeyAnalogEvent(neg, v < -0.3f, v < -0.3f ? -v : 0.0f);
        io.AddKeyAnalogEvent(pos, v > 0.3f, v > 0.3f ? v : 0.0f);
    };
    stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, pad.lx);
    stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, pad.ly);
    const kh2android::Touch touch = kh2android::touch();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (touch.down)
        io.AddMousePosEvent(touch.x, touch.y);
    io.AddMouseButtonEvent(0, touch.down);
}
#endif

// ---- Presentation -----------------------------------------------------------

// FSR 1 constants (AMD FidelityFX, MIT); same set-up as the PCSX2 integration.
void fsrEasuCon(float *con0, float *con1, float *con2, float *con3, float inW, float inH, float outW, float outH)
{
    con0[0] = inW / outW;
    con0[1] = inH / outH;
    con0[2] = 0.5f * inW / outW - 0.5f;
    con0[3] = 0.5f * inH / outH - 0.5f;
    con1[0] = 1.0f / inW;
    con1[1] = 1.0f / inH;
    con1[2] = 1.0f / inW;
    con1[3] = -1.0f / inH;
    con2[0] = -1.0f / inW;
    con2[1] = 2.0f / inH;
    con2[2] = 1.0f / inW;
    con2[3] = 2.0f / inH;
    con3[0] = 0.0f / inW;
    con3[1] = 4.0f / inH;
    con3[2] = con3[3] = 0.0f;
}

uint16_t floatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int exp = int((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0)
        return uint16_t(sign);
    if (exp >= 31)
        return uint16_t(sign | 0x7C00u);
    return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
}

void fsrRcasCon(float *con, float sharpness)
{
    sharpness = std::exp2(-sharpness);
    const uint32_t half = floatToHalf(sharpness);
    con[0] = sharpness;
    const uint32_t halves = half | (half << 16);
    std::memcpy(&con[1], &halves, sizeof(halves));
    con[2] = con[3] = 0.0f;
}

void fullscreenTriangle(CommandBuffer &cmd)
{
    const float verts[] = {-1.0f, -1.0f, -1.0f, 3.0f, 3.0f, -1.0f};
    std::memcpy(cmd.allocate_vertex_data(0, sizeof(verts), 2 * sizeof(float)), verts, sizeof(verts));
    cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, 0);
}

void renderFsr(CommandBuffer &cmd, const ImageView &view)
{
    cmd.image_barrier(*g_fsrTarget, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    RenderPassInfo rp = {};
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &g_fsrTarget->get_view();
    rp.store_attachments = 1u << 0;
    cmd.begin_render_pass(rp);
    struct Constants
    {
        float params[4][4];
    } constants;
    const float w = float(view.get_image().get_width());
    const float h = float(view.get_image().get_height());
    auto *params = cmd.allocate_typed_constant_data<Constants>(1, 0, 1);
    fsrEasuCon(constants.params[0], constants.params[1], constants.params[2], constants.params[3], w, h,
               cmd.get_viewport().width, cmd.get_viewport().height);
    *params = constants;
    struct Push
    {
        float width, height;
    } push = {cmd.get_viewport().width, cmd.get_viewport().height};
    cmd.push_constants(&push, 0, sizeof(push));
    fullscreenTriangle(cmd);
    cmd.set_texture(0, 0, view, StockSampler::NearestClamp);
    cmd.set_program(g_upscaleProgram);
    cmd.set_opaque_state();
    cmd.set_depth_test(false, false);
    cmd.draw(3);
    cmd.end_render_pass();
    cmd.image_barrier(*g_fsrTarget, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void renderRcas(CommandBuffer &cmd, const ImageView &view, float x, float y, float w, float h)
{
    struct Constants
    {
        float params[4];
        int32_t range[4];
    } constants;
    fsrRcasCon(constants.params, 0.5f);
    constants.range[0] = 0;
    constants.range[1] = 0;
    constants.range[2] = int(view.get_view_width()) - 1;
    constants.range[3] = int(view.get_view_height()) - 1;
    *cmd.allocate_typed_constant_data<Constants>(1, 0, 1) = constants;
    fullscreenTriangle(cmd);
    cmd.set_texture(0, 0, view, StockSampler::NearestClamp);
    cmd.set_opaque_state();
    cmd.set_depth_test(false, false);
    cmd.set_program(g_sharpenProgram);
    struct Push
    {
        float width, height;
    } push = {w, h};
    cmd.push_constants(&push, 0, sizeof(push));
    cmd.set_viewport({x, y, w, h, 0.0f, 1.0f});
    cmd.draw(3);
}

void renderBlit(CommandBuffer &cmd, const ImageView &view, float x, float y, float w, float h)
{
    cmd.set_texture(0, 0, view, StockSampler::LinearClamp);
    cmd.set_opaque_state();
    cmd.set_depth_test(false, false);
    cmd.set_program(g_blitProgram);
    cmd.set_viewport({x, y, w, h, 0.0f, 1.0f});
    cmd.draw(3);
}

PrivSnapshot snapshotPriv(const GSRegisters &src)
{
    PrivSnapshot p;
    const uint64_t regs[] = {src.pmode,  src.smode1,   src.smode2,   src.srfsh,    src.synch1,
                             src.synch2, src.syncv,    src.dispfb1,  src.display1, src.dispfb2,
                             src.display2, src.extbuf, src.extdata,  src.extwrite, src.bgcolor};
    std::memcpy(p.regs, regs, sizeof(regs));
    return p;
}

void copyPrivRegisters(const PrivSnapshot &snap)
{
    PrivRegisterState &dst = g_gs->get_priv_register_state();
    const uint64_t *regs = snap.regs;
    for (size_t i = 0; i < 15; ++i)
        dst.qwords_lo[i * 2] = regs[i];
    struct
    {
        uint64_t smode1, smode2;
    } src = {regs[1], regs[2]};
    // The runtime's SetGsCrt does not fill in SMODE1/SMODE2 the way the BIOS
    // does; paraLLEl-GS needs them to pick the video mode. KH2 runs NTSC
    // interlaced: SMODE1 LC=32 (bits 3-9), CMOD=NTSC (bits 13-14); SMODE2 INT.
    if (src.smode1 == 0)
        dst.qwords_lo[1 * 2] = (32ull << 3) | (2ull << 13);
    if (src.smode2 == 0)
        dst.qwords_lo[2 * 2] = 1ull;
}

double nowSeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Frame rates: guest frames (vsyncs) and presented frames, over ~0.5 s.
struct RateCounter
{
    double start = 0.0;
    unsigned count = 0;
    float rate = 0.0f;
    void tick(double now)
    {
        if (start == 0.0)
            start = now;
        ++count;
        if (now - start >= 0.5)
        {
            rate = float(count / (now - start));
            start = now;
            count = 0;
        }
    }
};
RateCounter g_guestRate, g_presentRate;

#if defined(__ANDROID__)
// GPU time per paraLLEl-GS stage (and its CPU-side waits), every 3 seconds:
// "[gpu] shading 9.81 ms/frame (x14.2)" = 9.81 ms of GPU time per presented
// frame, in 14.2 dispatches per frame on average.
void logGpuTimes(Device &dev)
{
    static double s_last = 0.0;
    const double now = nowSeconds();
    if (s_last == 0.0)
        s_last = now;
    if (now - s_last < 3.0)
        return;
    s_last = now;
    std::string line = "[gpu]";
    dev.timestamp_log([&](const std::string &tag, const TimestampIntervalReport &r) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), " | %s %.2f ms (x%.1f)", tag.c_str(), 1000.0 * r.time_per_frame_context,
                      r.accumulations_per_frame_context);
        line += buf;
    });
    const double secs = 3.0;
    std::printf("%s | game %.1f fps, present %.1f fps\n", line.c_str(), g_guestRate.rate, g_presentRate.rate);
    std::string steps = "[frame] ms per second:";
    for (int i = 0; i < 8; ++i)
    {
        char b[64];
        std::snprintf(b, sizeof(b), " %s %.0f |", kStepNames[i], double(g_stepNs[i].exchange(0)) / 1e6 / secs);
        steps += b;
    }
    std::printf("%s\n", steps.c_str());
    std::printf("[cpu] readbacks %.1f/s (+%.1f/s from the previous frame, %.1f/s of them without a GPU read), guest waits %.1f ms/s for them (GPU side %.1f ms/s) | guest waits for a full "
                "queue %.1f ms/s | render thread feeding paraLLEl-GS %.1f ms/s\n",
                double(g_statReadbacks.exchange(0)) / secs, double(g_statReadbacksAsync.exchange(0) + g_statReadbacksSkipped.load()) / secs,
                double(g_statReadbacksSkipped.exchange(0)) / secs,
                double(g_statReadbackNs.exchange(0)) / 1e6 / secs,
                double(g_statReadbackServeNs.exchange(0)) / 1e6 / secs, double(g_statQueueWaitNs.exchange(0)) / 1e6 / secs,
                double(g_statDrainNs.exchange(0)) / 1e6 / secs);
    dev.timestamp_log_reset();
}
#endif


// ImGui 1.92 texture requests (the font atlas), as Granite images.
void uiUpdateTextures(CommandBuffer &cmd)
{
    for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
    {
        switch (tex->Status)
        {
        case ImTextureStatus_WantCreate:
        {
            ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(tex->Width, tex->Height, VK_FORMAT_R8G8B8A8_UNORM);
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.initial_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ImageInitialData init = {tex->GetPixels()};
            ImageHandle img = g_device->create_image(info, &init);
            tex->SetTexID(reinterpret_cast<ImTextureID>(img.get()));
            tex->BackendUserData = img.release();
            tex->Status = ImTextureStatus_OK;
            break;
        }
        case ImTextureStatus_WantUpdates:
        {
            auto *img = static_cast<Image *>(tex->BackendUserData);
            const int x = tex->UpdateRect.x, y = tex->UpdateRect.y, w = tex->UpdateRect.w, h = tex->UpdateRect.h;
            const int pitch = w * tex->BytesPerPixel;
            cmd.image_barrier(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT);
            auto *dst = static_cast<uint8_t *>(cmd.update_image(*img, {x, y, 0}, {uint32_t(w), uint32_t(h), 1}, w, h,
                                                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}));
            for (int row = 0; row < h; ++row)
                std::memcpy(dst + size_t(pitch) * row, tex->GetPixelsAt(x, y + row), size_t(pitch));
            cmd.image_barrier(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            tex->Status = ImTextureStatus_OK;
            break;
        }
        case ImTextureStatus_WantDestroy:
            if (auto *img = static_cast<Image *>(tex->BackendUserData))
                img->release_reference();
            tex->SetTexID(ImTextureID_Invalid);
            tex->BackendUserData = nullptr;
            tex->Status = ImTextureStatus_Destroyed;
            break;
        default:
            break;
        }
    }
}

void uiDraw(CommandBuffer &cmd, uint32_t width, uint32_t height)
{
    const ImDrawData *data = ImGui::GetDrawData();
    if (!data || data->CmdListsCount == 0)
        return;
    cmd.set_viewport({0, 0, float(width), float(height), 0, 1});
    const float invSize[2] = {1.0f / float(width), 1.0f / float(height)};
    for (int n = 0; n < data->CmdListsCount; ++n)
    {
        const ImDrawList *list = data->CmdLists[n];
        std::memcpy(cmd.allocate_vertex_data(0, list->VtxBuffer.Size * sizeof(ImDrawVert), sizeof(ImDrawVert)),
                    list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(cmd.allocate_index_data(list->IdxBuffer.Size * sizeof(ImDrawIdx), VK_INDEX_TYPE_UINT16),
                    list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
        cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos));
        cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv));
        cmd.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col));
        for (int i = 0; i < list->CmdBuffer.Size; ++i)
        {
            const ImDrawCmd &dc = list->CmdBuffer[i];
            const int x0 = int(std::max(0.0f, dc.ClipRect.x)), y0 = int(std::max(0.0f, dc.ClipRect.y));
            const int x1 = int(std::min(float(width), dc.ClipRect.z)), y1 = int(std::min(float(height), dc.ClipRect.w));
            if (x1 <= x0 || y1 <= y0)
                continue;
            cmd.set_scissor({{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}});
            auto *img = reinterpret_cast<Image *>(dc.GetTexID());
            if (img)
                cmd.set_texture(0, 0, img->get_view(), StockSampler::LinearClamp);
            cmd.push_constants(invSize, 0, sizeof(invSize));
            cmd.set_program(g_uiProgram[img ? 1 : 0]);
            cmd.set_transparent_sprite_state();
            cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            cmd.set_depth_test(false, false);
            cmd.set_cull_mode(VK_CULL_MODE_NONE);
            cmd.draw_indexed(dc.ElemCount, 1, dc.IdxOffset, int32_t(dc.VtxOffset), 0);
        }
    }
}

#if KH2_WSI
// Builds this frame's UI; false when nothing is on screen.
bool uiBuild()
{
    if (!g_imguiReady)
        return false;
    const double now = nowSeconds();
    const bool toast = now < g_toastUntil;
    const bool hud = g_hudVisible && g_hudCallback;
    const bool world = g_worldActive && g_worldCallback;
    const bool overlay = g_overlayActive && g_overlayCallback;
    if (!g_menuOpen && !g_showFps && !toast && !hud && !world && !overlay)
        return false;
#if defined(_WIN32)
    ImGui_ImplWin32_NewFrame();
#else
    imguiAndroidNewFrame();
#endif
    ImGui::NewFrame();
    if (g_showFps)
    {
        ImGui::SetNextWindowPos(ImVec2(10, 10));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("game %.1f fps  |  display %.1f fps", g_guestRate.rate, g_presentRate.rate);
        ImGui::End();
    }
    if (toast && !g_menuOpen)
    {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(size.x - 10, size.y - 10), 0, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        {
            std::lock_guard<std::mutex> lock(g_toastMutex);
            ImGui::TextUnformatted(g_toastText.c_str());
        }
        ImGui::End();
    }
    if (world)
        g_worldCallback();
    if (hud)
        g_hudCallback();
    if (overlay && !g_menuOpen)
        g_overlayCallback();
    if (g_menuOpen && g_uiCallback)
        g_uiCallback();
    ImGui::Render();
    return true;
}

void present()
{
#if defined(__ANDROID__)
    if (g_surfaceLost)
        return;
#endif
    if (!g_platform->g_inFrame)
    {
        StepTimer st(3);
        g_platform->g_inFrame = g_wsi->begin_frame();
    }
    if (!g_platform->g_inFrame)
        return;
    Device &dev = g_wsi->get_device();
    StepTimer stBlit(4);
    float vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    if (g_last.image)
    {
        const uint32_t sw = dev.get_swapchain_view().get_view_width();
        const uint32_t sh = dev.get_swapchain_view().get_view_height();
        uint32_t w = sw, h = sh;
        float gameAspect = g_opt.aspect > 0.0f ? g_opt.aspect : 4.0f / 3.0f;
        if (g_last.mode_width && g_last.mode_height)
            gameAspect *= (float(g_last.internal_width) / float(g_last.mode_width)) /
                          (float(g_last.internal_height) / float(g_last.mode_height));
        if (float(sw) / float(sh) > gameAspect)
            w = uint32_t(std::lround(float(sh) * gameAspect));
        else
            h = uint32_t(std::lround(float(sw) / gameAspect));
        w = std::max<uint32_t>(w, 1);
        h = std::max<uint32_t>(h, 1);
        vpX = std::round(0.5f * float(sw - w));
        vpY = std::round(0.5f * float(sh - h));
        vpW = float(w);
        vpH = float(h);
        g_picX = vpX;
        g_picY = vpY;
        g_picW = vpW;
        g_picH = vpH;
        if (g_opt.fsr && (!g_fsrTarget || g_fsrTarget->get_width() != w || g_fsrTarget->get_height() != h))
        {
            auto info = ImageCreateInfo::render_target(w, h, VK_FORMAT_R8G8B8A8_UNORM);
            info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            g_fsrTarget = dev.create_image(info);
        }
    }
    auto cmd = dev.request_command_buffer();
    const bool ui = uiBuild();
    if (ui)
        uiUpdateTextures(*cmd);
    // FSR is an upscaler: with 16x super-sampling the picture (4x the PS2's)
    // can be taller than the window, and then a bilinear blit does better.
    const bool useFsr = g_opt.fsr && g_last.image && g_fsrTarget && g_fsrTarget->get_height() > g_last.image->get_height();
    if (useFsr)
        renderFsr(*cmd, g_last.image->get_view());
    cmd->begin_render_pass(dev.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly));
    if (g_last.image)
    {
        if (useFsr)
            renderRcas(*cmd, g_fsrTarget->get_view(), vpX, vpY, vpW, vpH);
        else
            renderBlit(*cmd, g_last.image->get_view(), vpX, vpY, vpW, vpH);
    }
    if (ui)
        uiDraw(*cmd, dev.get_swapchain_view().get_view_width(), dev.get_swapchain_view().get_view_height());
    cmd->end_render_pass();
    {
        StepTimer st(5);
        dev.submit(cmd);
    }
    {
        StepTimer st(6);
        g_wsi->end_frame();
    }
    g_presentRate.tick(nowSeconds());
#if defined(__ANDROID__)
    logGpuTimes(dev);
#endif
    // Begin the next frame right away, for pacing (as the PCSX2 integration does).
    {
        StepTimer st(7);
        g_platform->g_inFrame = g_wsi->begin_frame();
    }
}
#endif
} // namespace


// Compiled GPU pipelines persist across runs in pipeline_cache.bin next to the
// executable: paraLLEl-GS compiles a pipeline the first time a new blend or
// texture setup appears, stalling the frame by 10-170 ms (the "Stalled
// compile" warnings). With the cache only the first ever run pays for it.
std::string pipelineCachePath()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, n);
    const size_t slash = path.find_last_of("\\/");
    path = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    return path + "pipeline_cache.bin";
#else
    return "pipeline_cache.bin";
#endif
}
size_t g_pipelineCacheSaved = 0;
std::chrono::steady_clock::time_point g_pipelineCacheCheck{};

void loadPipelineCache()
{
    std::vector<uint8_t> data;
    if (FILE *f = std::fopen(pipelineCachePath().c_str(), "rb"))
    {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0)
        {
            data.resize(size_t(size));
            if (std::fread(data.data(), 1, data.size(), f) != data.size())
                data.clear();
        }
        std::fclose(f);
    }
    g_device->init_pipeline_cache(data.empty() ? nullptr : data.data(), data.size());
    g_pipelineCacheSaved = data.size();
    std::printf("[pgs] pipeline cache: %zu bytes loaded\n", data.size());
}

// Writes the cache if it grew (at most every 20 s unless forced).
void savePipelineCache(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - g_pipelineCacheCheck < std::chrono::seconds(20))
        return;
    g_pipelineCacheCheck = now;
    const size_t size = g_device->get_pipeline_cache_size();
    if (!size || size == g_pipelineCacheSaved)
        return;
    std::vector<uint8_t> data(size);
    if (!g_device->get_pipeline_cache_data(data.data(), data.size()))
        return;
    const std::string path = pipelineCachePath(), tmp = path + ".tmp";
    if (FILE *f = std::fopen(tmp.c_str(), "wb"))
    {
        const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        if (ok)
        {
            std::remove(path.c_str());
            if (std::rename(tmp.c_str(), path.c_str()) == 0)
                g_pipelineCacheSaved = size;
        }
    }
}

bool init(const Options &options)
{
    g_opt = options;
    registerThread();
#if defined(__ANDROID__)
    if (!Context::init_loader(reinterpret_cast<PFN_vkGetInstanceProcAddr>(kh2android::vulkanLoader())))
#else
    if (!Context::init_loader(nullptr))
#endif
    {
        std::fprintf(stderr, "[pgs] no Vulkan loader; using the software renderer\n");
        return false;
    }
#if KH2_WSI
    if (g_opt.windowed)
    {
        if (!createWindow())
            return false;
        g_platform = std::make_unique<Platform>();
        g_wsi = std::make_unique<WSI>();
        g_wsi->set_platform(g_platform.get());
        g_wsi->set_backbuffer_format(BackbufferFormat::UNORM);
        g_wsi->set_present_mode(g_opt.vsync ? PresentMode::SyncToVBlank : PresentMode::UnlockedNoTearing);
        if (!g_wsi->init_simple(1, {}))
        {
            std::fprintf(stderr, "[pgs] Vulkan device/swapchain creation failed; using the software renderer\n");
#if defined(__ANDROID__)
            kh2android::vulkanFailed("no Vulkan device");
#endif
            g_wsi.reset();
#if defined(_WIN32)
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
#endif
            return false;
        }
        g_device = &g_wsi->get_device();
#if defined(__ANDROID__)
        kh2android::setWindowCallbacks(&onWindowLost, &onWindowRegained);
#endif
    }
    else
#endif
    {
        g_context = std::make_unique<Context>();
        g_context->set_num_thread_indices(1);
        if (!g_context->init_instance_and_device(nullptr, 0, nullptr, 0, 0))
        {
            std::fprintf(stderr, "[pgs] Vulkan device creation failed; using the software renderer\n");
            g_context.reset();
            return false;
        }
        g_headlessDevice = std::make_unique<Device>();
        g_headlessDevice->set_context(*g_context);
        g_device = g_headlessDevice.get();
    }
#if defined(__ANDROID__)
    {
        const auto &gp = g_device->get_gpu_properties();
        const auto &df = g_device->get_device_features();
        std::string info = gp.deviceName;
        if (df.vk12_props.driverName[0])
            info += std::string(", ") + df.vk12_props.driverName + " " + df.vk12_props.driverInfo;
        kh2android::vulkanStarted(info);
    }
#endif
    loadPipelineCache();
    g_device->init_frame_contexts(12);

    ResourceLayout layout;
    Shaders<> suite(*g_device, layout, 0);
    g_blitProgram = g_device->request_program(suite.quad, suite.blit);
    g_upscaleProgram = g_device->request_program(suite.upscale_vert, suite.upscale_frag);
    g_sharpenProgram = g_device->request_program(suite.sharpen_vert, suite.sharpen_frag);
    // ui_frag[textured][sRGB backbuffer]; ours is UNORM.
    g_uiProgram[0] = g_device->request_program(suite.ui_vert, suite.ui_frag[0][0]);
    g_uiProgram[1] = g_device->request_program(suite.ui_vert, suite.ui_frag[1][0]);

    g_gs = std::make_unique<GSInterface>();
    GSOptions gsOpt = {};
    gsOpt.super_sampling = toSuperSampling(g_opt.superSampling);
    gsOpt.ordered_super_sampling = true;
    gsOpt.dynamic_super_sampling = true;
    gsOpt.super_sampled_textures = g_opt.superSampledTextures;
    if (!g_gs->init(g_device, gsOpt))
    {
        std::fprintf(stderr, "[pgs] GS initialisation failed (missing GPU features?); using the software renderer\n");
#if defined(__ANDROID__)
        kh2android::vulkanFailed("paraLLEl-GS cannot run");
#endif
        g_gs.reset();
        return false;
    }
    {
        Hacks hacks = {};
        hacks.backbuffer_promotion = g_opt.backbufferPromotion;
        hacks.disable_mipmaps = g_opt.disableMipmaps;
        g_gs->set_hacks(hacks);
    }
#if defined(__ANDROID__)
    {
        // The texture-region analysis pass (an upload optimisation) does not
        // build on Adreno drivers ("Failed to link shaders"); without it whole
        // textures are uploaded, which is fine at native resolution.
        DebugMode dbg = {};
        dbg.disable_sampler_feedback = true;
        // GPU time per stage, logged every few seconds (logGpuTimes).
        dbg.timestamps = true;
        g_gs->set_debug_mode(dbg);
    }
#endif
    if (ParallelGS::TextureReplacer *replacer = kh2hd::attach(g_device))
        g_gs->set_texture_replacer(replacer);
    const auto &props = g_device->get_gpu_properties();
    std::printf("[pgs] paraLLEl-GS on %s, %ux super-sampling%s\n", props.deviceName, g_opt.superSampling,
                g_opt.windowed ? ", windowed" : "");

#if KH2_WSI
    if (g_opt.windowed)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendRendererName = "kh2recomp-granite";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
        ImGui::StyleColorsDark();
#if defined(_WIN32)
        const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);
        const float scale = std::max(1.0f, dpi) * 1.25f;
#else
        // A handheld's screen: big enough to read and to touch.
        const float scale = std::max(1.5f, float(std::min(g_clientW.load(), g_clientH.load())) / 400.0f);
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
        io.BackendPlatformName = "kh2recomp-android";
#endif
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetStyle().FontScaleDpi = scale;
#if defined(_WIN32)
        ImGui_ImplWin32_Init(g_hwnd);
#endif
        g_imguiReady = true;
        g_toastUntil = nowSeconds() + 8.0;
    }
#endif
    ps2SetGifTap(&tapPacket, &tapRegister, nullptr);
    ps2SetGsReadback(&readbackFromGpu, nullptr);
    ps2SetSoftwareRasterEnabled(g_opt.keepSoftwareRaster);
    g_active = true;
    g_renderThreadOn = options.renderThread;
    if (g_renderThreadOn)
    {
        g_renderStop = false;
        g_renderThread = std::thread(renderLoop);
        std::printf("[pgs] render thread on\n");
    }
    return true;
}

bool active() { return g_active; }

void shutdown()
{
    if (!g_active)
        return;
    ps2SetGifTap(nullptr, nullptr, nullptr);
    ps2SetGsReadback(nullptr, nullptr);
    ps2SetSoftwareRasterEnabled(true);
    stopRenderThread();
    std::lock_guard<std::mutex> lock(g_mutex);
    registerThread();
    g_device->wait_idle();
    savePipelineCache(true);
    if (g_gs)
        g_gs->set_texture_replacer(nullptr);
    kh2hd::detach();
#if KH2_WSI
    if (g_imguiReady)
    {
        for (ImTextureData *tex : ImGui::GetPlatformIO().Textures)
            if (auto *img = static_cast<Image *>(tex->BackendUserData))
            {
                img->release_reference();
                tex->BackendUserData = nullptr;
            }
#if defined(_WIN32)
        ImGui_ImplWin32_Shutdown();
#endif
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
#endif
    g_last = {};
    g_heldScanouts.clear();
    g_fsrTarget.reset();
    g_gs.reset();
    g_blitProgram = g_upscaleProgram = g_sharpenProgram = nullptr;
#if KH2_WSI
    g_wsi.reset();
    g_platform.reset();
#endif
#if defined(_WIN32)
    if (g_hwnd)
        DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
#endif
    g_headlessDevice.reset();
    g_context.reset();
    g_device = nullptr;
    g_active = false;
}

// Settings changes from the menu (render thread, g_mutex held).
void applyPending()
{
    if (!g_gs)
        return;
    const int ssaa = g_pendingSsaa.exchange(0);
    const int ssTex = g_pendingSsTextures.exchange(-1);
    const int sharp = g_pendingSharpest.exchange(-1);
    bool rate = false;
    if (ssaa > 0 && unsigned(ssaa) != g_opt.superSampling)
    {
        g_opt.superSampling = unsigned(ssaa);
        rate = true;
    }
    if (ssTex >= 0 && bool(ssTex) != g_opt.superSampledTextures)
    {
        g_opt.superSampledTextures = ssTex != 0;
        rate = true;
    }
    if (rate)
        g_gs->set_super_sampling_rate(toSuperSampling(g_opt.superSampling), true, g_opt.superSampledTextures);
    if (sharp >= 0 && bool(sharp) != g_opt.disableMipmaps)
    {
        g_opt.disableMipmaps = sharp != 0;
        Hacks hacks = {};
        hacks.backbuffer_promotion = g_opt.backbufferPromotion;
        hacks.disable_mipmaps = g_opt.disableMipmaps;
        g_gs->set_hacks(hacks);
    }
}

// One guest frame: its GIF packets (the job's, or what is queued), then
// scanout and present. Under g_mutex.
void processFrame(const PrivSnapshot &priv, uint64_t vsyncTick, FrameJob *job)
{
    g_trace.vsync = vsyncTick;
    auto drainForFrame = [job] {
        if (job)
        {
            feedPackets(job->packets, job->end);
            job->packets.clear();
        }
        else
            drainQueue();
    };
    {
        {
            std::lock_guard<std::mutex> cap(g_capMutex);
            if (g_capState == 2)
            {
                // Packets of the frame that just ended were drained before this call.
                drainForFrame();
                std::fclose(g_capFile);
                g_capFile = nullptr;
                g_capState = 0;
                std::printf("[pgs] GIF capture: %zu bytes in %s\n", g_capBytes, g_capPath.c_str());
            }
            else if (g_capState == 1)
            {
                drainForFrame(); // the previous frame's tail stays out
                g_capFile = std::fopen(g_capPath.c_str(), "wb");
                g_capBytes = 0;
                g_capState = g_capFile ? 2 : 0;
            }
        }
        {
            StepTimer st(0);
            drainForFrame();
        }
        savePipelineCache(false);
        {
            StepTimer st(1);
            g_gs->flush();
        }
        // KH2_PGS_VRAM_DUMP=<tick>[,<tick>...]: writes paraLLEl-GS's VRAM
        // (4 MB, swizzled) to vram_<tick>.bin. Diagnostics only.
        if (const char *dump = std::getenv("KH2_PGS_VRAM_DUMP"))
        {
            for (const char *p = dump; *p;)
            {
                char *end = nullptr;
                const unsigned long long t = std::strtoull(p, &end, 10);
                if (end == p)
                    break;
                if (t == vsyncTick)
                {
                    const void *mem = g_gs->map_vram_read(0, 4u << 20);
                    char name[64];
                    std::snprintf(name, sizeof(name), "vram_%llu.bin", t);
                    if (FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(mem, 1, 4u << 20, f);
                        std::fclose(f);
                        std::fprintf(stderr, "[pgs] wrote %s\n", name);
                    }
                }
                p = *end ? end + 1 : end;
            }
        }
        applyPending();
        copyPrivRegisters(priv);
        VSyncInfo info = {};
        info.phase = uint32_t(vsyncTick & 1u);
        info.force_progressive = true;
        info.anti_blur = true;
        info.overscan = false;
        info.crtc_offsets = false;
        info.adapt_to_internal_horizontal_resolution = true;
        info.raw_circuit_scanout = true;
        info.high_resolution_scanout = g_opt.superSampling > 1;
        info.dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
        info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
        {
            StepTimer st(2);
            ScanoutResult scan = g_gs->vsync(info);
            if (g_renderThreadOn)
            {
                g_heldScanouts.push_back(std::move(scan));
                while (g_heldScanouts.size() > g_presentDelay + 1u)
                    g_heldScanouts.pop_front();
                g_last = g_heldScanouts.front();
            }
            else
                g_last = std::move(scan);
        }
        g_guestRate.tick(nowSeconds());
#if defined(__ANDROID__)
        {
            static double s_lastFrame = 0.0;
            const double t = nowSeconds();
            if (s_lastFrame > 0.0)
                kh2android::frameTimeHint(int64_t((t - s_lastFrame) * 1e9));
            s_lastFrame = t;
        }
#endif
        g_lastLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
#if KH2_WSI
        if (g_opt.windowed)
        {
            const uint32_t w = g_clientW, h = g_clientH;
            if (w && h)
                present();
        }
        else
#endif
            g_device->next_frame_context();
    }
}

// Hands a frame to the render thread; waits only while it is kMaxJobsAhead
// frames behind.
void postJob(const PrivSnapshot &priv, uint64_t tick, bool redraw)
{
    std::unique_lock<std::mutex> lock(g_queueMutex);
    g_jobDone.wait(lock, [] { return g_jobs.size() < kMaxJobsAhead || g_renderStop.load(); });
    if (g_renderStop)
        return;
    FrameJob job;
    job.packets.swap(g_queue);
    job.end = g_enqueued;
    job.priv = priv;
    job.tick = tick;
    job.redraw = redraw;
    g_jobs.push_back(std::move(job));
    lock.unlock();
    g_queueDrained.notify_all();
    g_renderWake.notify_one();
}

void renderLoop()
{
#if defined(__ANDROID__) || defined(__linux__)
    pthread_setname_np(pthread_self(), "PGS-Render");
#endif
    while (!g_renderStop)
    {
        FrameJob job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_renderWake.wait_for(lock, std::chrono::milliseconds(2),
                                  [] { return g_renderStop.load() || !g_jobs.empty() || g_rbWake.load(); });
            if (g_renderStop)
                break;
            if (!g_jobs.empty())
            {
                job = std::move(g_jobs.front());
                g_jobs.pop_front();
                haveJob = true;
            }
        }
        if (haveJob)
            g_jobDone.notify_all();
        std::lock_guard<std::mutex> lock(g_mutex);
        registerThread();
        if (!g_gs)
            continue;
        if (!haveJob)
        {
            // Between frames: keep paraLLEl-GS fed, and serve readbacks.
            g_rbWake = false;
            drainQueue();
        }
        else if (job.redraw)
        {
            feedPackets(job.packets, job.end);
            g_gs->flush();
#if KH2_WSI
            if (g_opt.windowed && g_clientW && g_clientH)
                present();
#endif
        }
        else
            processFrame(job.priv, job.tick, &job);
    }
}

void stopRenderThread()
{
    if (!g_renderThread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_renderStop = true;
    }
    g_renderWake.notify_all();
    g_jobDone.notify_all();
    g_renderThread.join();
    g_renderThreadOn = false;
}

bool frame(const GSRegisters &priv, uint64_t vsyncTick)
{
#if KH2_WSI
    if (g_opt.windowed)
        pumpMessages();
#endif
    if (!g_active)
        return !g_closed;
    if (g_renderThreadOn)
    {
        postJob(snapshotPriv(priv), vsyncTick, false);
        return !g_closed;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    registerThread();
    processFrame(snapshotPriv(priv), vsyncTick, nullptr);
    return !g_closed;
}

void setAspect(float aspect) { g_opt.aspect = aspect; }
float displayAspect() { return g_opt.aspect > 0.0f ? g_opt.aspect : 4.0f / 3.0f; }
bool takeScreenshotRequest() { return g_screenshotRequested.exchange(false); }
void requestGifCapture(const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_capMutex);
    if (g_capState != 0)
        return;
    g_capPath = path;
    g_capState = 1;
}
void toast(const std::string &text, double seconds)
{
    std::lock_guard<std::mutex> lock(g_toastMutex);
    g_toastText = text;
    g_toastUntil = nowSeconds() + seconds;
}

void setUiCallback(std::function<void()> draw) { g_uiCallback = std::move(draw); }
void setHudCallback(std::function<void()> draw) { g_hudCallback = std::move(draw); }
void setOverlayCallback(std::function<void()> draw) { g_overlayCallback = std::move(draw); }
void setOverlayActive(bool on) { g_overlayActive = on; }
void setWorldCallback(std::function<void()> draw) { g_worldCallback = std::move(draw); }
void setWorldOverlayActive(bool on) { g_worldActive = on; }
void pictureRect(float &x, float &y, float &w, float &h)
{
    x = g_picX;
    y = g_picY;
    w = g_picW;
    h = g_picH;
}
bool hudVisible() { return g_hudVisible; }
void setHudVisible(bool on) { g_hudVisible = on; }
bool menuOpen() { return g_menuOpen; }
void setMenuOpen(bool open) { g_menuOpen = open; }
void toggleMenu() { g_menuOpen = !g_menuOpen; }
bool showFps() { return g_showFps; }
void setShowFps(bool show) { g_showFps = show; }

unsigned superSampling() { return g_opt.superSampling; }
// The settings menu runs on the render thread with g_mutex held (inside
// present()), so these only record the change; applyPending() makes it at the
// next frame.
void setSuperSampling(unsigned n) { g_pendingSsaa = int(n); }
bool superSampledTextures() { return g_opt.superSampledTextures; }
void setSuperSampledTextures(bool on) { g_pendingSsTextures = on ? 1 : 0; }
bool sharpestTextures() { return g_opt.disableMipmaps; }
void setSharpestTextures(bool on) { g_pendingSharpest = on ? 1 : 0; }

bool fsr() { return g_opt.fsr; }
void setFsr(bool on) { g_opt.fsr = on; }
bool vsync() { return g_opt.vsync; }
void setVsync(bool on)
{
#if KH2_WSI
    g_opt.vsync = on;
    if (g_wsi)
        g_wsi->set_present_mode(on ? PresentMode::SyncToVBlank : PresentMode::UnlockedNoTearing);
#else
    (void)on;
#endif
}
bool fullscreen()
{
#if defined(_WIN32)
    return g_fullscreen;
#else
    return false;
#endif
}
void setFullscreen(bool on)
{
#if defined(_WIN32)
    if (g_hwnd && on != g_fullscreen)
        toggleFullscreen();
#else
    (void)on;
#endif
}
void requestClose() { g_closed = true; }
void setWindowSize(unsigned w, unsigned h)
{
#if defined(_WIN32)
    if (w && h && w < 65536 && h < 65536)
        g_requestedSize = (uint32_t(w) << 16) | uint32_t(h);
#else
    (void)w;
    (void)h;
#endif
}
void windowSize(unsigned &w, unsigned &h)
{
#if KH2_WSI
    w = g_clientW;
    h = g_clientH;
#else
    w = g_opt.windowWidth;
    h = g_opt.windowHeight;
#endif
}

bool pumpWindow()
{
#if KH2_WSI
    if (g_opt.windowed)
        pumpMessages();
#endif
    if (g_active && g_renderThreadOn)
        g_renderWake.notify_one();
    else if (g_active)
    {
        // Keep up between frames so a frame's worth never piles up.
        std::lock_guard<std::mutex> lock(g_mutex);
        registerThread();
        drainQueue();
    }
    return !g_closed;
}

bool redraw()
{
#if KH2_WSI
    if (!g_opt.windowed)
        return !g_closed;
    pumpMessages();
    // Paced to ~60 Hz: without guest frames nothing else limits this.
    static double last = 0.0;
    const double now = nowSeconds();
    if (now - last < 1.0 / 60.0)
        return !g_closed;
    last = now;
    if (g_active && g_renderThreadOn)
    {
        bool idle;
        {
            std::lock_guard<std::mutex> q(g_queueMutex);
            idle = g_jobs.empty();
        }
        if (idle)
            postJob(PrivSnapshot{}, 0, true);
        return !g_closed;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    registerThread();
    if (g_active && g_clientW && g_clientH)
    {
        // GS data drained between frames (pumpWindow) sits in a command buffer
        // paraLLEl-GS has not submitted yet; ending the WSI frame waits for
        // every requested command buffer, so present without flushing first
        // hung the game whenever it paused mid-frame (the settings menu during
        // a busy 3D scene).
        drainQueue();
        g_gs->flush();
        present();
    }
#endif
    return !g_closed;
}

bool readback(std::vector<uint32_t> &rgba, uint32_t &width, uint32_t &height)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    registerThread();
    if (!g_active || !g_last.image)
        return false;
    const Image &img = *g_last.image;
    width = img.get_width();
    height = img.get_height();
    BufferCreateInfo bi = {};
    bi.size = VkDeviceSize(width) * height * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.domain = BufferDomain::CachedHost;
    auto buffer = g_device->create_buffer(bi, nullptr);
    auto cmd = g_device->request_command_buffer();
    cmd->image_barrier(img, g_lastLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                       VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd->copy_image_to_buffer(*buffer, img, 0, {0, 0, 0}, {width, height, 1}, 0, 0,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    cmd->image_barrier(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_lastLayout, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                 VK_ACCESS_HOST_READ_BIT);
    Fence fence;
    g_device->submit(cmd, &fence);
    fence->wait();
    const auto *src = static_cast<const uint32_t *>(g_device->map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
    if (!src)
        return false;
    rgba.assign(src, src + size_t(width) * height);
    g_device->unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
    const VkFormat fmt = img.get_format();
    const bool bgra = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t &p : rgba)
    {
        if (bgra)
            p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
        p |= 0xFF000000u;
    }
    return true;
}

void setWindowPlacedCallback(void (*fn)(int, int, int, int))
{
#if defined(_WIN32)
    g_placedFn = fn;
#else
    (void)fn;
#endif
}

bool windowFocused()
{
#if defined(_WIN32)
    return g_hwnd ? g_focused.load() : true;
#elif defined(__ANDROID__)
    return g_focused.load();
#else
    return true;
#endif
}

void *nativeWindow()
{
#if defined(_WIN32)
    return g_hwnd;
#else
    return nullptr;
#endif
}
} // namespace kh2pgs
