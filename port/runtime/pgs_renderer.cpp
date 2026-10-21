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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "shaders/slangmosh.hpp"
#include "thread_id.hpp"
#include "wsi.hpp"

#include "runtime/gs/gs_gif_tap.h"
#include "runtime/ps2_memory.h"

using namespace Vulkan;
using namespace ParallelGS;

namespace kh2pgs
{
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
VkImageLayout g_lastLayout = VK_IMAGE_LAYOUT_UNDEFINED;

Program *g_blitProgram = nullptr;
Program *g_upscaleProgram = nullptr;
Program *g_sharpenProgram = nullptr;
ImageHandle g_fsrTarget;

// Granite looks up a per-thread index for its command pools. Every call into
// it happens under g_mutex, so all threads can share index 0; an unregistered
// thread would get 0 too, but with an error logged on every call.
void registerThread()
{
    static thread_local bool registered = false;
    if (!registered)
    {
        Util::register_thread_index(0);
        registered = true;
    }
}

void tapPacket(const uint8_t *data, uint32_t size, void *)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    registerThread();
    if (g_gs && size >= 16)
        g_gs->gif_transfer(3, data, size & ~15u);
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
    switch (msg)
    {
    case WM_CLOSE:
        g_closed = true;
        return 0;
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
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Kingdom Hearts II Final Mix (native)", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst,
                             nullptr);
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

void pumpMessages()
{
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

void copyPrivRegisters(const GSRegisters &src)
{
    PrivRegisterState &dst = g_gs->get_priv_register_state();
    const uint64_t regs[] = {src.pmode,  src.smode1,   src.smode2,   src.srfsh,    src.synch1,
                             src.synch2, src.syncv,    src.dispfb1,  src.display1, src.dispfb2,
                             src.display2, src.extbuf, src.extdata,  src.extwrite, src.bgcolor};
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i)
        dst.qwords_lo[i * 2] = regs[i];
    // The runtime's SetGsCrt does not fill in SMODE1/SMODE2 the way the BIOS
    // does; paraLLEl-GS needs them to pick the video mode. KH2 runs NTSC
    // interlaced: SMODE1 LC=32 (bits 3-9), CMOD=NTSC (bits 13-14); SMODE2 INT.
    if (src.smode1 == 0)
        dst.qwords_lo[1 * 2] = (32ull << 3) | (2ull << 13);
    if (src.smode2 == 0)
        dst.qwords_lo[2 * 2] = 1ull;
}

#if defined(_WIN32)
void present()
{
    if (!g_platform->g_inFrame)
        g_platform->g_inFrame = g_wsi->begin_frame();
    if (!g_platform->g_inFrame)
        return;
    Device &dev = g_wsi->get_device();
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
        if (g_opt.fsr && (!g_fsrTarget || g_fsrTarget->get_width() != w || g_fsrTarget->get_height() != h))
        {
            auto info = ImageCreateInfo::render_target(w, h, VK_FORMAT_R8G8B8A8_UNORM);
            info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            g_fsrTarget = dev.create_image(info);
        }
    }
    auto cmd = dev.request_command_buffer();
    const bool useFsr = g_opt.fsr && g_last.image && g_fsrTarget;
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
    cmd->end_render_pass();
    dev.submit(cmd);
    g_wsi->end_frame();
    // Begin the next frame right away, for pacing (as the PCSX2 integration does).
    g_platform->g_inFrame = g_wsi->begin_frame();
}
#endif
} // namespace

bool init(const Options &options)
{
    g_opt = options;
    registerThread();
    if (!Context::init_loader(nullptr))
    {
        std::fprintf(stderr, "[pgs] no Vulkan loader; using the software renderer\n");
        return false;
    }
#if defined(_WIN32)
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
            g_wsi.reset();
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
            return false;
        }
        g_device = &g_wsi->get_device();
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
    g_device->init_frame_contexts(12);

    ResourceLayout layout;
    Shaders<> suite(*g_device, layout, 0);
    g_blitProgram = g_device->request_program(suite.quad, suite.blit);
    g_upscaleProgram = g_device->request_program(suite.upscale_vert, suite.upscale_frag);
    g_sharpenProgram = g_device->request_program(suite.sharpen_vert, suite.sharpen_frag);

    g_gs = std::make_unique<GSInterface>();
    GSOptions gsOpt = {};
    gsOpt.super_sampling = toSuperSampling(g_opt.superSampling);
    gsOpt.ordered_super_sampling = true;
    gsOpt.dynamic_super_sampling = true;
    if (!g_gs->init(g_device, gsOpt))
    {
        std::fprintf(stderr, "[pgs] GS initialisation failed (missing GPU features?); using the software renderer\n");
        g_gs.reset();
        return false;
    }
    const auto &props = g_device->get_gpu_properties();
    std::printf("[pgs] paraLLEl-GS on %s, %ux super-sampling%s\n", props.deviceName, g_opt.superSampling,
                g_opt.windowed ? ", windowed" : "");

    ps2SetGifTap(&tapPacket, &tapRegister, nullptr);
    ps2SetSoftwareRasterEnabled(g_opt.keepSoftwareRaster);
    g_active = true;
    return true;
}

bool active() { return g_active; }

void shutdown()
{
    if (!g_active)
        return;
    ps2SetGifTap(nullptr, nullptr, nullptr);
    ps2SetSoftwareRasterEnabled(true);
    std::lock_guard<std::mutex> lock(g_mutex);
    registerThread();
    g_device->wait_idle();
    g_last = {};
    g_fsrTarget.reset();
    g_gs.reset();
    g_blitProgram = g_upscaleProgram = g_sharpenProgram = nullptr;
#if defined(_WIN32)
    g_wsi.reset();
    g_platform.reset();
    if (g_hwnd)
        DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
#endif
    g_headlessDevice.reset();
    g_context.reset();
    g_device = nullptr;
    g_active = false;
}

bool frame(const GSRegisters &priv, uint64_t vsyncTick)
{
#if defined(_WIN32)
    if (g_opt.windowed)
        pumpMessages();
#endif
    if (!g_active)
        return !g_closed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        registerThread();
        g_gs->flush();
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
        g_last = g_gs->vsync(info);
        g_lastLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
#if defined(_WIN32)
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
    return !g_closed;
}

void setAspect(float aspect) { g_opt.aspect = aspect; }

bool pumpWindow()
{
#if defined(_WIN32)
    if (g_opt.windowed)
        pumpMessages();
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

bool windowFocused()
{
#if defined(_WIN32)
    return g_hwnd ? g_focused.load() : true;
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
