// Android: a GPU driver of the player's choice (e.g. Mesa's Turnip for Adreno),
// loaded with libadrenotools instead of the system Vulkan driver, as Android
// emulators do. The menu browses the shared storage for a driver package (.zip
// with meta.json, as published for emulators, or a bare .so); it is copied into
// the app's private storage (Android only loads libraries from there) and used
// from the next start. If a start with it does not get as far as creating the
// Vulkan device, the next start goes back to the system driver.
#if defined(__ANDROID__)

#include "android_host.h"

#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

extern "C" int kh2_kgsl_patch_driver(const char *libName);

namespace kh2android
{
namespace
{
namespace fs = std::filesystem;

std::string g_inUse = "System driver";
std::string g_notice;  // why the chosen driver was not used, shown in the menu
std::string g_message; // result of the last action in the menu
bool g_triedCustom = false;
std::string g_customLib; // file name of the custom driver library

fs::path driversDir() { return fs::path(internalDataDir()) / "gpu-drivers"; }

std::string readText(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool writeText(const fs::path &p, const std::string &text)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    return bool(f);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string currentChoice() { return readText(driversDir() / "current"); }

// ---- Zip (driver packages) ---------------------------------------------------
uint32_t rd16(const std::vector<uint8_t> &b, size_t o) { return o + 2 <= b.size() ? b[o] | (b[o + 1] << 8) : 0; }
uint32_t rd32(const std::vector<uint8_t> &b, size_t o)
{
    return o + 4 <= b.size() ? uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24)
                             : 0;
}

struct ZipEntry
{
    std::string name;
    std::vector<uint8_t> data;
};

// The files of a zip (stored or deflated), without directories.
bool readZip(const fs::path &path, std::vector<ZipEntry> &out, std::string &err)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 22)
    {
        err = "not a zip file";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = b.size() - 22 + 1; i-- > 0 && b.size() - i < 66000;)
        if (rd32(b, i) == 0x06054b50)
        {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos)
    {
        err = "not a zip file";
        return false;
    }
    const uint32_t count = rd16(b, eocd + 10);
    size_t p = rd32(b, eocd + 16);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (rd32(b, p) != 0x02014b50)
        {
            err = "damaged zip file";
            return false;
        }
        const uint32_t method = rd16(b, p + 10);
        const uint32_t csize = rd32(b, p + 20), usize = rd32(b, p + 24);
        const uint32_t nlen = rd16(b, p + 28), elen = rd16(b, p + 30), clen = rd16(b, p + 32);
        const uint32_t local = rd32(b, p + 42);
        std::string name(reinterpret_cast<const char *>(b.data() + p + 46), std::min<size_t>(nlen, b.size() - p - 46));
        p += 46 + nlen + elen + clen;
        if (name.empty() || name.back() == '/')
            continue;
        if (rd32(b, local) != 0x04034b50)
        {
            err = "damaged zip file";
            return false;
        }
        const size_t data = local + 30 + rd16(b, local + 26) + rd16(b, local + 28);
        if (data + csize > b.size())
        {
            err = "damaged zip file";
            return false;
        }
        ZipEntry e;
        e.name = fs::path(name).filename().string();
        if (method == 0)
            e.data.assign(b.begin() + data, b.begin() + data + csize);
        else if (method == 8)
        {
            e.data.resize(usize);
            z_stream z = {};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
            {
                err = "zlib";
                return false;
            }
            z.next_in = b.data() + data;
            z.avail_in = csize;
            z.next_out = e.data.data();
            z.avail_out = usize;
            const int r = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (r != Z_STREAM_END)
            {
                err = "could not unpack " + name;
                return false;
            }
        }
        else
        {
            err = "unsupported compression in " + name;
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

// "libraryName": "vulkan.ad07XX.so" from a driver package's meta.json.
std::string metaLibraryName(const std::string &json)
{
    const size_t k = json.find("\"libraryName\"");
    if (k == std::string::npos)
        return {};
    const size_t a = json.find('"', json.find(':', k) + 1);
    const size_t z = a == std::string::npos ? a : json.find('"', a + 1);
    return z == std::string::npos ? std::string() : json.substr(a + 1, z - a - 1);
}

std::string metaField(const std::string &json, const char *key)
{
    const size_t k = json.find(std::string("\"") + key + "\"");
    if (k == std::string::npos)
        return {};
    const size_t a = json.find('"', json.find(':', k) + 1);
    const size_t z = a == std::string::npos ? a : json.find('"', a + 1);
    return z == std::string::npos ? std::string() : json.substr(a + 1, z - a - 1);
}

// Copies a driver package into private storage and selects it.
bool install(const fs::path &src, std::string &msg)
{
    std::string id;
    for (char c : src.stem().string())
        id += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_') ? c : '_';
    if (id.empty())
        id = "driver";
    const fs::path dir = driversDir() / id;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    std::string lib, label = src.filename().string();

    if (lower(src.extension().string()) == ".so")
    {
        lib = src.filename().string();
        if (!fs::copy_file(src, dir / lib, fs::copy_options::overwrite_existing, ec))
        {
            msg = "Could not copy " + src.string() + ": " + ec.message();
            return false;
        }
    }
    else
    {
        std::vector<ZipEntry> files;
        std::string err;
        if (!readZip(src, files, err))
        {
            msg = src.filename().string() + ": " + err;
            return false;
        }
        std::string meta;
        for (const ZipEntry &e : files)
            if (lower(e.name) == "meta.json")
                meta.assign(e.data.begin(), e.data.end());
        lib = metaLibraryName(meta);
        const std::string name = metaField(meta, "name"), ver = metaField(meta, "driverVersion");
        if (!name.empty())
            label = name + (ver.empty() ? "" : " (" + ver + ")");
        for (const ZipEntry &e : files)
        {
            if (lib.empty() && lower(fs::path(e.name).extension().string()) == ".so")
                lib = e.name;
            std::ofstream o(dir / e.name, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char *>(e.data.data()), std::streamsize(e.data.size()));
        }
        if (lib.empty() || !fs::exists(dir / lib, ec))
        {
            msg = src.filename().string() + " has no driver library (.so) in it.";
            return false;
        }
    }
    writeText(dir / "library", lib);
    writeText(dir / "label", label);
    writeText(driversDir() / "current", id);
    fs::remove(driversDir() / "starting", ec);
    msg = "Installed " + label + ". Close the app and open it again to use it.";
    std::printf("[driver] installed %s (%s) from %s\n", label.c_str(), lib.c_str(), src.c_str());
    return true;
}

// ---- File browser -------------------------------------------------------------
fs::path g_browse;
std::vector<fs::directory_entry> g_entries;

void listDir(const fs::path &dir)
{
    g_browse = dir;
    g_entries.clear();
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
    {
        const std::string n = e.path().filename().string();
        if (n.empty() || n[0] == '.')
            continue;
        const std::string ext = lower(e.path().extension().string());
        if (e.is_directory(ec) || ext == ".zip" || ext == ".so")
            g_entries.push_back(e);
    }
    std::sort(g_entries.begin(), g_entries.end(), [](const fs::directory_entry &a, const fs::directory_entry &b) {
        std::error_code e1, e2;
        const bool da = a.is_directory(e1), db = b.is_directory(e2);
        if (da != db)
            return da;
        return lower(a.path().filename().string()) < lower(b.path().filename().string());
    });
}

void drawBrowser()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(display.x * 0.8f, display.y * 0.8f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Choose a GPU driver", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        return;
    ImGui::TextWrapped("A driver package (.zip, as made for Android emulators) or a driver library (.so).");
    ImGui::TextUnformatted(g_browse.c_str());
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.5f;
    ImGui::BeginChild("files", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    fs::path go;
    if (g_browse.has_parent_path() && g_browse != g_browse.root_path() && g_browse != "/storage" &&
        ImGui::Selectable(".. (up)"))
        go = g_browse.parent_path();
    for (const auto &e : g_entries)
    {
        std::error_code ec;
        const bool dir = e.is_directory(ec);
        const std::string label = (dir ? "[folder]  " : "") + e.path().filename().string();
        if (ImGui::Selectable(label.c_str()))
        {
            if (dir)
                go = e.path();
            else
            {
                install(e.path(), g_message);
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel"))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (!go.empty())
        listDir(go);
}
} // namespace

void *vulkanLoader()
{
    std::error_code ec;
    const fs::path dir = driversDir();
    fs::create_directories(dir, ec);
    const std::string id = currentChoice();
    if (id.empty())
        return nullptr;
    const std::string label = readText(dir / id / "label");
    if (fs::exists(dir / "starting", ec))
    {
        // The last start with this driver never reached a working Vulkan device.
        fs::remove(dir / "starting", ec);
        fs::remove(dir / "current", ec);
        g_notice = "The driver " + label + " stopped the game from starting last time, so the system driver is in use. "
                   "Choose it again to retry.";
        std::printf("[driver] %s failed last time; using the system driver\n", label.c_str());
        return nullptr;
    }
    const std::string lib = readText(dir / id / "library");
    writeText(dir / "starting", id);
    g_triedCustom = true;
    const std::string hookDir = nativeLibraryDir() + "/";
    const std::string driverDir = (dir / id).string() + "/";
    std::printf("[driver] loading %s (%s)\n", label.c_str(), lib.c_str());
    // Turnip: draw straight to memory instead of in on-chip bins. Binned
    // rendering left bands of the screen black (tested on an Adreno 740), and
    // the final present is one small full-screen pass that gains nothing from it.
    setenv("TU_DEBUG", "sysmem", 0);
    // Mesa emulates timeline semaphores on Qualcomm's kernel driver, and its
    // clean-up of old points blocked every submit for a few ms (several times a
    // frame). A driver thread of its own for submissions takes that off ours.
    setenv("MESA_VK_ENABLE_SUBMIT_THREAD", "1", 0);
    void *handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hookDir.c_str(),
                                              driverDir.c_str(), lib.c_str(), nullptr, nullptr);
    void *gipa = handle ? dlsym(handle, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa)
    {
        fs::remove(dir / "starting", ec);
        g_notice = "The driver " + label + " could not be loaded, so the system driver is in use.";
        std::printf("[driver] could not load %s: %s\n", lib.c_str(), dlerror());
        return nullptr;
    }
    g_customLib = lib;
    g_inUse = label;
    return gipa;
}

void vulkanStarted(const std::string &deviceInfo)
{
    // Keep the GPU at its full clock: the game reads GPU memory back about once a
    // frame and waits for it, and a GPU that powers down between those short
    // bursts made every wait ~30 ms.
    adrenotools_set_turbo(true);
    std::error_code ec;
    fs::remove(driversDir() / "starting", ec);
    if (!g_triedCustom)
        g_inUse = "System driver";
    if (!g_customLib.empty())
    {
        // Fix for Turnip on KGSL (android_kgsl_hook.c): its zero-timeout
        // timestamp polls waited for the GPU. The driver is only loaded once
        // Vulkan starts, so this comes here.
        const int patched = kh2_kgsl_patch_driver(g_customLib.c_str());
        std::printf("[driver] ioctl hook: %d import(s) patched\n", patched);
    }
    if (!deviceInfo.empty())
        g_inUse += " - " + deviceInfo;
    std::printf("[driver] in use: %s\n", g_inUse.c_str());
}

void vulkanFailed(const char *why)
{
    std::error_code ec;
    fs::remove(driversDir() / "starting", ec);
    if (!g_triedCustom)
        return;
    fs::remove(driversDir() / "current", ec);
    std::printf("[driver] %s with %s: going back to the system driver; closing (open the app again)\n", why,
                g_inUse.c_str());
    std::fflush(stdout);
    closeApp();
}

void drawDriverMenu()
{
    ImGui::SeparatorText("GPU driver");
    ImGui::TextWrapped("In use: %s", g_inUse.c_str());
    const std::string id = currentChoice();
    const std::string next = id.empty() ? "System driver" : readText(driversDir() / id / "label");
    ImGui::TextWrapped("Next start: %s", next.c_str());
    if (!g_notice.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", g_notice.c_str());
    if (ImGui::Button("Choose driver file..."))
    {
        std::error_code ec;
        const fs::path start = fs::exists("/storage/emulated/0/Download", ec) ? "/storage/emulated/0/Download"
                                                                              : "/storage/emulated/0";
        listDir(g_browse.empty() ? start : g_browse);
        ImGui::OpenPopup("Choose a GPU driver");
    }
    ImGui::SameLine();
    if (ImGui::Button("Use system driver"))
    {
        std::error_code ec;
        fs::remove(driversDir() / "current", ec);
        g_message = "The system driver is used from the next start.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Close app"))
        closeApp();
    drawBrowser();
    if (!g_message.empty())
        ImGui::TextWrapped("%s", g_message.c_str());
}
} // namespace kh2android

#endif
