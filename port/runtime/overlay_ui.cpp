// The in-game settings menu (F1, or Select+Start on a controller), drawn with
// Dear ImGui by the GPU renderer. Changes apply immediately; "Save" writes them
// to kh2recomp.ini next to the executable, keeping lines it does not manage.

#include "overlay_ui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

#include "game_patches.h"
#include "host_input.h"
#include "pgs_renderer.h"
#include "ps2_runtime.h"

namespace kh2ui
{
namespace
{
PS2Runtime *g_runtime = nullptr;
std::filesystem::path g_iniPath;
std::string g_status;

const unsigned kSsaa[] = {1, 2, 4, 8, 16};
const char *kSsaaNames[] = {"Off (native)", "2x", "4x", "8x", "16x"};

void applyAspect()
{
    kh2pgs::setAspect(kh2patches::aspect());
}

// Rewrites the keys this menu owns; other lines (elf, iso, comments) stay.
bool save()
{
    std::vector<std::string> kept;
    {
        std::ifstream in(g_iniPath);
        static const std::set<std::string> owned = {"ssaa",       "no-fsr",  "vsync",   "fullscreen",
                                                    "widescreen", "patch",   "swap-confirm", "no-rumble",
                                                    "show-fps"};
        for (std::string line; std::getline(in, line);)
        {
            std::string key = line.substr(0, line.find_first_of("=#;"));
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t\r") + 1);
            if (!owned.count(key))
                kept.push_back(line);
        }
    }
    std::ofstream out(g_iniPath, std::ios::trunc);
    if (!out)
        return false;
    for (const std::string &line : kept)
        out << line << "\n";
    out << "ssaa = " << kh2pgs::superSampling() << "\n";
    if (!kh2pgs::fsr())
        out << "no-fsr = 1\n";
    if (kh2pgs::vsync())
        out << "vsync = 1\n";
    if (kh2pgs::fullscreen())
        out << "fullscreen = 1\n";
    std::string patches;
    for (const kh2patches::Info &p : kh2patches::available())
        if (kh2patches::isEnabled(p.name))
            patches += (patches.empty() ? "" : ",") + std::string(p.name);
    if (!patches.empty())
        out << "patch = " << patches << "\n";
    if (!kh2input::confirmOnBottom())
        out << "swap-confirm = 1\n";
    if (!kh2input::rumbleEnabled())
        out << "no-rumble = 1\n";
    if (kh2pgs::showFps())
        out << "show-fps = 1\n";
    return true;
}

void draw()
{
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    bool open = true;
    if (!ImGui::Begin("Settings", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Display");
    int ssaa = 0;
    for (int i = 0; i < 5; ++i)
        if (kSsaa[i] == kh2pgs::superSampling())
            ssaa = i;
    if (ImGui::Combo("Super-sampling", &ssaa, kSsaaNames, 5))
        kh2pgs::setSuperSampling(kSsaa[ssaa]);
    ImGui::SetItemTooltip("Renders each pixel from several samples: smoother edges, less shimmer.");
    bool fsr = kh2pgs::fsr();
    if (ImGui::Checkbox("Sharp upscaling (AMD FSR 1)", &fsr))
        kh2pgs::setFsr(fsr);
    bool full = kh2pgs::fullscreen();
    if (ImGui::Checkbox("Fullscreen (Alt+Enter)", &full))
        kh2pgs::setFullscreen(full);
    bool vs = kh2pgs::vsync();
    if (ImGui::Checkbox("VSync", &vs))
        kh2pgs::setVsync(vs);
    bool fps = kh2pgs::showFps();
    if (ImGui::Checkbox("Show frame rate (F2)", &fps))
        kh2pgs::setShowFps(fps);

    ImGui::SeparatorText("Game patches");
    const std::vector<kh2patches::Info> patches = kh2patches::available();
    if (patches.empty())
        ImGui::TextDisabled("None built (tools/game_patches.py).");
    for (const kh2patches::Info &p : patches)
    {
        bool on = kh2patches::isEnabled(p.name);
        if (ImGui::Checkbox(p.description, &on) && g_runtime)
        {
            kh2patches::setEnabled(*g_runtime, p.name, on);
            applyAspect();
        }
    }

    ImGui::SeparatorText("Controls");
    int layout = kh2input::confirmOnBottom() ? 0 : 1;
    const char *layouts[] = {"Bottom button confirms (international)", "Right button confirms (Japanese)"};
    if (ImGui::Combo("Layout", &layout, layouts, 2))
        kh2input::setConfirmOnBottom(layout == 0);
    bool rumble = kh2input::rumbleEnabled();
    if (ImGui::Checkbox("Vibration", &rumble))
        kh2input::setRumbleEnabled(rumble);
    ImGui::TextDisabled(kh2input::controllerConnected() ? "Controller connected" : "No controller (keyboard: see docs)");

    ImGui::Separator();
    if (ImGui::Button("Save settings"))
        g_status = save() ? "Saved to " + g_iniPath.filename().string() : "Could not write " + g_iniPath.string();
    ImGui::SameLine();
    if (ImGui::Button("Resume"))
        open = false;
    ImGui::SameLine();
    if (ImGui::Button("Quit game"))
        kh2pgs::requestClose();
    if (!g_status.empty())
        ImGui::TextDisabled("%s", g_status.c_str());
    ImGui::End();
    if (!open)
        kh2pgs::setMenuOpen(false);
}

void onCombo() { kh2pgs::toggleMenu(); }
} // namespace

void install(PS2Runtime &runtime, const std::string &iniPath)
{
    g_runtime = &runtime;
    g_iniPath = iniPath;
    kh2pgs::setUiCallback(&draw);
    kh2input::setMenuComboCallback(&onCombo);
}

void tick()
{
    kh2input::setSuppressed(kh2pgs::menuOpen());
}
} // namespace kh2ui
