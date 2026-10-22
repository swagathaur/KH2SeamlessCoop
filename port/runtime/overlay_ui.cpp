// The in-game settings menu (F1, or Select+Start on a controller), drawn with
// Dear ImGui by the GPU renderer. Changes apply immediately; "Save" writes them
// to kh2recomp.ini next to the executable, keeping lines it does not manage.

#include "overlay_ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

#include "game_patches.h"
#include "hd_textures.h"
#include "game_state.h"
#include "coop.h"
#include "puppets.h"
#include "host_input.h"
#include "movie_eof.h"
#include "cutscene_skip.h"
#include "pgs_renderer.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_native_iop.h"

namespace kh2ui
{
namespace
{
PS2Runtime *g_runtime = nullptr;
std::filesystem::path g_iniPath;
std::string g_status;
Options g_opt;
bool g_paused = false;
bool g_testFastForward = false;
double g_speed = 1.0;

// Keyboard rebinding: the action waiting for a key (-1: none), and whether the
// key is added to its list rather than replacing it.
int g_bindAction = -1;
bool g_bindAdd = false;

void stopCapture()
{
    g_bindAction = -1;
    kh2input::setCapturingKey(false);
}

void drawKeyboard()
{
    if (g_bindAction >= 0)
    {
        const int vk = kh2input::newlyPressedKey();
        if (vk == 0x1B && !g_bindAdd) // Escape cancels a replace
            stopCapture();
        else if (vk)
        {
            kh2keys::Bindings b = kh2input::keyBindings();
            auto &list = b.keys[g_bindAction];
            if (!g_bindAdd)
                list.clear();
            if (std::find(list.begin(), list.end(), vk) == list.end())
                list.push_back(vk);
            kh2input::setKeyBindings(b);
            stopCapture();
        }
    }
    if (!ImGui::CollapsingHeader("Keyboard"))
        return;
    ImGui::TextDisabled("Click a binding, then press a key (Esc cancels). + adds a second key.");
    kh2keys::Bindings b = kh2input::keyBindings();
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
    {
        for (int i = 0; i < kh2keys::ActionCount; ++i)
        {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kh2keys::actionLabel(kh2keys::Action(i)));
            ImGui::TableNextColumn();
            const bool waiting = g_bindAction == i;
            const std::string text = waiting ? (g_bindAdd ? "Press a key to add..." : "Press a key...")
                                             : kh2keys::keyListText(b.keys[i]);
            if (ImGui::Button(text.c_str(), ImVec2(200, 0)))
            {
                g_bindAction = i;
                g_bindAdd = false;
                kh2input::newlyPressedKey(); // forget keys already down
                kh2input::setCapturingKey(true);
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("+"))
            {
                g_bindAction = i;
                g_bindAdd = true;
                kh2input::newlyPressedKey();
                kh2input::setCapturingKey(true);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
            {
                b.keys[i].clear();
                kh2input::setKeyBindings(b);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Default keys"))
    {
        stopCapture();
        kh2input::setKeyBindings(kh2keys::Bindings::defaults());
    }
}

const unsigned kSsaa[] = {1, 2, 4, 8, 16};
const char *kSsaaNames[] = {"Off (PS2 resolution)", "2x", "4x (2x resolution)", "8x (2x resolution)",
                            "16x (4x resolution)"};

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
                                                    "show-fps",   "ff-speed", "pause-unfocused",
                                                    "no-mouse-camera", "mouse-sensitivity", "volume", "mute", "window",
                                                    "no-name-tags", "skip-movies", "skip-cutscenes",
                                                    "no-ssaa-textures", "no-mipmaps", "hd-textures"};
        for (std::string line; std::getline(in, line);)
        {
            std::string key = line.substr(0, line.find_first_of("=#;"));
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t\r") + 1);
            if (!owned.count(key) && key.rfind("key-", 0) != 0)
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
    if (!kh2pgs::superSampledTextures())
        out << "no-ssaa-textures = 1\n";
    if (kh2pgs::sharpestTextures())
        out << "no-mipmaps = 1\n";
    if (kh2hd::loaded() && kh2hd::enabled())
        out << "hd-textures = 1\n";
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
    if (!kh2input::mouseCamera())
        out << "no-mouse-camera = 1\n";
    out << "mouse-sensitivity = " << kh2input::mouseSensitivity() << "\n";
    {
        unsigned w = 0, h = 0;
        kh2pgs::windowSize(w, h);
        if (w && h && !kh2pgs::fullscreen())
            out << "window = " << w << "x" << h << "\n";
    }
    out << "volume = " << int(g_opt.volume * 100.0f + 0.5f) << "\n";
    out << "ff-speed = " << g_opt.fastForwardSpeed << "\n";
    if (g_opt.pauseUnfocused)
        out << "pause-unfocused = 1\n";
    if (kh2movie::skipping())
        out << "skip-movies = 1\n";
    if (kh2skip::autoSkip())
        out << "skip-cutscenes = 1\n";
    if (!g_opt.nameTags)
        out << "no-name-tags = 1\n";
    for (const std::string &line : kh2input::keyBindings().iniLines())
        out << line << "\n";
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
    ImGui::SetItemTooltip("Renders each pixel from several samples: smoother edges, less shimmer, and a\n"
                          "sharper picture (2x the PS2's resolution from 4x, 4x from 16x).");
    bool ssTex = kh2pgs::superSampledTextures();
    if (ImGui::Checkbox("Super-sample screen effects", &ssTex))
        kh2pgs::setSuperSampledTextures(ssTex);
    ImGui::SetItemTooltip("KH2 redraws the finished frame for its screen effects. Off, that pass drops\n"
                          "back to PS2 resolution.");
    bool sharpTex = kh2pgs::sharpestTextures();
    if (ImGui::Checkbox("Sharpest textures", &sharpTex))
        kh2pgs::setSharpestTextures(sharpTex);
    ImGui::SetItemTooltip("Always use full-size textures, even far away (no mipmaps). Sharper at high\n"
                          "resolution; can shimmer.");
    if (kh2hd::loaded())
    {
        bool hd = kh2hd::enabled();
        if (ImGui::Checkbox("HD textures (PC release)", &hd))
            kh2hd::setEnabled(hd);
        ImGui::SetItemTooltip("Menus, fonts and other 2D art from the PC release's remastered textures.");
    }
    bool fsr = kh2pgs::fsr();
    if (ImGui::Checkbox("Sharp upscaling (AMD FSR 1)", &fsr))
        kh2pgs::setFsr(fsr);
    {
        // Window sizes for the shown aspect; the picture is letterboxed otherwise.
        const bool wide = kh2pgs::displayAspect() > 1.5f;
        static const unsigned k43[][2] = {{640, 480}, {960, 720}, {1280, 960}, {1440, 1080}, {1920, 1440}, {2880, 2160}};
        static const unsigned k169[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        const auto *sizes = wide ? k169 : k43;
        const int count = wide ? 5 : 6;
        unsigned w = 0, h = 0;
        kh2pgs::windowSize(w, h);
        char current[32];
        std::snprintf(current, sizeof(current), "%u x %u", w, h);
        ImGui::BeginDisabled(kh2pgs::fullscreen());
        if (ImGui::BeginCombo("Window size", current))
        {
            for (int i = 0; i < count; ++i)
            {
                char label[32];
                std::snprintf(label, sizeof(label), "%u x %u", sizes[i][0], sizes[i][1]);
                if (ImGui::Selectable(label, sizes[i][0] == w && sizes[i][1] == h))
                    kh2pgs::setWindowSize(sizes[i][0], sizes[i][1]);
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    bool full = kh2pgs::fullscreen();
    if (ImGui::Checkbox("Fullscreen (Alt+Enter)", &full))
        kh2pgs::setFullscreen(full);
    bool vs = kh2pgs::vsync();
    if (ImGui::Checkbox("VSync", &vs))
        kh2pgs::setVsync(vs);
    bool fps = kh2pgs::showFps();
    if (ImGui::Checkbox("Show frame rate (F2)", &fps))
        kh2pgs::setShowFps(fps);
    bool hud = kh2pgs::hudVisible();
    if (ImGui::Checkbox("Show game state (F3)", &hud))
        kh2pgs::setHudVisible(hud);

    ImGui::SeparatorText("Sound");
    int vol = int(g_opt.volume * 100.0f + 0.5f);
    if (ImGui::SliderInt("Volume", &vol, 0, 100, "%d%%"))
    {
        g_opt.volume = vol / 100.0f;
        ps2_native_iop::setVolume(g_opt.volume);
    }

    ImGui::SeparatorText("Speed");
    ImGui::SliderFloat("Fast-forward (hold Tab)", &g_opt.fastForwardSpeed, 1.5f, 8.0f, "%.1fx");
    ImGui::Checkbox("Pause when the window is in the background", &g_opt.pauseUnfocused);
    bool skipMovies = kh2movie::skipping();
    if (ImGui::Checkbox("Skip movies", &skipMovies))
        kh2movie::setSkip(skipMovies);
    ImGui::SetItemTooltip("Skip the pre-rendered movies (hold B, or Tab, to skip the one playing)");
    bool skipCut = kh2skip::autoSkip();
    if (ImGui::Checkbox("Skip cutscenes", &skipCut))
        kh2skip::setAutoSkip(skipCut);
    ImGui::SetItemTooltip("Skip every in-engine cutscene, as the pause menu's Skip does (hold B to skip just one)");

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

    ImGui::SeparatorText("Co-op (preview: players see each other's position)");
    {
        static char name[32] = "";
        static char address[64] = "";
        static bool seeded = false;
        if (!seeded)
        {
            seeded = true;
            std::snprintf(name, sizeof(name), "%s", kh2coop::options().name.c_str());
            std::snprintf(address, sizeof(address), "%s", kh2coop::options().join.c_str());
        }
        ImGui::TextUnformatted(kh2coop::statusText().c_str());
        if (!kh2coop::active())
        {
            ImGui::InputText("Your name", name, sizeof(name));
            if (ImGui::Button("Host"))
                kh2coop::host(name, kh2coop::options().port);
            ImGui::SetItemTooltip("Others join with your IP address (port %u, UDP, must be reachable)",
                                  unsigned(kh2coop::options().port));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180);
            ImGui::InputTextWithHint("##addr", "host IP[:port]", address, sizeof(address));
            ImGui::SameLine();
            if (ImGui::Button("Join") && address[0])
                kh2coop::join(name, address);
        }
        else
        {
            for (const kh2coop::RemotePlayer &r : kh2coop::remotes())
                ImGui::BulletText("%s  %.0f ms", r.name.c_str(), r.rttMs);
            if (!kh2coop::isHost())
            {
                if (ImGui::Button("Go to the host's room"))
                    kh2coop::warpToHost();
                ImGui::SameLine();
                bool follow = kh2coop::followHost();
                if (ImGui::Checkbox("Follow the host", &follow))
                    kh2coop::setFollowHost(follow);
                ImGui::SetItemTooltip("Go to the host's room whenever they change rooms (when you are free to move)");
                bool shared = kh2coop::sharedEnemies();
                if (ImGui::Checkbox("Shared enemies", &shared))
                    kh2coop::setSharedEnemies(shared);
                ImGui::SetItemTooltip("In the host's room, enemies are where the host's are, with the host's HP; your hits count for everyone");
            }
            bool puppets = kh2puppets::enabled();
            if (ImGui::Checkbox("Show other players", &puppets))
                kh2puppets::setEnabled(puppets);
            ImGui::SameLine();
            ImGui::Checkbox("Name tags", &g_opt.nameTags);
            int pm = kh2puppets::mode() == kh2puppets::Mode::Spawn ? 0 : 1;
            const char *modes[] = {"Their own character", "An NPC already in the room"};
            if (ImGui::Combo("Shown as", &pm, modes, 2))
                kh2puppets::setMode(pm ? kh2puppets::Mode::Borrow : kh2puppets::Mode::Spawn);
            if (ImGui::Button("Leave session"))
                kh2coop::leave();
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
    bool mouse = kh2input::mouseCamera();
    if (ImGui::Checkbox("Mouse camera (hold right button)", &mouse))
        kh2input::setMouseCamera(mouse);
    float sens = kh2input::mouseSensitivity();
    if (ImGui::SliderFloat("Mouse sensitivity", &sens, 0.25f, 4.0f, "%.2f"))
        kh2input::setMouseSensitivity(sens);
    {
        const std::string pad = kh2input::controllerName();
        if (pad.empty())
            ImGui::TextDisabled("No controller: keyboard (layout below)");
        else
            ImGui::TextDisabled("Controller: %s", pad.c_str());
    }
    drawKeyboard();

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
    {
        stopCapture();
        kh2pgs::setMenuOpen(false);
    }
}

// F3: live game state (the co-op layer's view of the game).
void drawHud()
{
    if (!g_runtime)
        return;
    const kh2state::State st = kh2state::read(g_runtime->memory().getRDRAM());
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x - 10, 10), ImGuiCond_FirstUseEver, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (!ImGui::Begin("Game state (F3)", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing))
    {
        ImGui::End();
        return;
    }
    ImGui::Text("%s", kh2state::describe(st.now).c_str());
    ImGui::Text("door %u  map %u  battle %u  event %u", st.now.door, st.now.map, st.now.battle, st.now.event);
    static const char *kBattle[] = {"none", "battle", "forced battle"};
    ImGui::Text("control %u  battle %s  speed %.2f", st.control, st.battle < 3 ? kBattle[st.battle] : "?",
                st.gameSpeed);
    if (st.reaction)
        ImGui::Text("reaction command %04X", st.reaction);
    if (st.cutsceneLength)
        ImGui::Text("cutscene %u / %u", st.cutsceneTime, st.cutsceneLength);
    if (st.player.valid)
        ImGui::Text("player %08X  pos %.0f, %.0f, %.0f  facing %.2f", st.player.address, st.player.pos[0],
                    st.player.pos[1], st.player.pos[2], st.player.facing);
    if (ImGui::BeginTable("party", 4, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("slot");
        ImGui::TableSetupColumn("HP");
        ImGui::TableSetupColumn("MP");
        ImGui::TableSetupColumn("Drive");
        ImGui::TableHeadersRow();
        for (int i = 0; i < 3; ++i)
        {
            const kh2state::Unit &u = st.party[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", i + 1);
            ImGui::TableNextColumn();
            ImGui::Text("%d/%d", u.hp, u.maxHp);
            ImGui::TableNextColumn();
            ImGui::Text("%d/%d", u.mp, u.maxMp);
            ImGui::TableNextColumn();
            ImGui::Text("%u/%u", u.drive, u.maxDrive);
        }
        ImGui::EndTable();
    }
    if (kh2coop::active())
    {
        ImGui::SeparatorText(kh2coop::statusText().c_str());
        for (const kh2coop::RemotePlayer &r : kh2coop::remotes())
        {
            if (!r.haveState)
            {
                ImGui::Text("%s: waiting", r.name.c_str());
                continue;
            }
            const bool sameRoom = r.state.world == st.now.world && r.state.room == st.now.room;
            const float dx = r.state.pos[0] - st.player.pos[0], dz = r.state.pos[2] - st.player.pos[2];
            ImGui::Text("%s: %s  %s  hp %d/%d  %.0f ms%s", r.name.c_str(),
                        kh2state::describe({r.state.world, r.state.room}).c_str(),
                        sameRoom ? (std::to_string(int(std::sqrt(dx * dx + dz * dz))) + " away").c_str() : "",
                        r.state.hp, r.state.maxHp, r.rttMs, r.age > 1.0 ? "  (no updates)" : "");
        }
    }
    for (const kh2puppets::Assignment &a : kh2puppets::assignments())
        ImGui::TextDisabled("player %u shown as %s", a.playerId + 1, a.name);
    for (const std::string &line : kh2coop::recentLog())
        ImGui::TextDisabled("%s", line.c_str());
    const std::vector<kh2state::ActorInfo> actors = kh2state::actors(g_runtime->memory().getRDRAM());
    if (ImGui::CollapsingHeader(("Actors (" + std::to_string(actors.size()) + ")").c_str()))
    {
        if (ImGui::BeginTable("actors", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("object");
            ImGui::TableSetupColumn("id");
            ImGui::TableSetupColumn("model");
            ImGui::TableSetupColumn("type");
            ImGui::TableSetupColumn("position");
            ImGui::TableHeadersRow();
            for (const kh2state::ActorInfo &a : actors)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%08X", a.address);
                ImGui::TableNextColumn();
                ImGui::Text("%04X", a.entryId);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(a.name);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kh2state::actorTypeName(a.type));
                ImGui::TableNextColumn();
                ImGui::Text("%.0f, %.0f, %.0f", a.pos[0], a.pos[1], a.pos[2]);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// Co-op: each other player's name over their puppet.
void drawNameTags()
{
    const uint8_t *ram = g_runtime->memory().getRDRAM();
    kh2state::Camera cam;
    if (!kh2state::readCamera(ram, cam))
        return;
    float px, py, pw, ph;
    kh2pgs::pictureRect(px, py, pw, ph);
    if (pw <= 0 || ph <= 0)
        return;
    const std::vector<kh2coop::RemotePlayer> players = kh2coop::remotes();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    for (const kh2puppets::Assignment &a : kh2puppets::assignments())
    {
        if (!a.object)
            continue;
        std::string name;
        for (const kh2coop::RemotePlayer &r : players)
            if (r.id == a.playerId)
                name = r.name;
        if (name.empty())
            continue;
        float pos[4];
        std::memcpy(pos, ram + ((a.object + kh2state::addr::ObjPos) & 0x01FFFFFFu), sizeof(pos));
        pos[1] -= 200.0f; // above the head (up is -y)
        float x, y, depth;
        if (!kh2state::project(cam, pos, x, y, depth) || std::fabs(x) > 1.2f || std::fabs(y) > 1.2f)
            continue;
        const ImVec2 at(px + (x * 0.5f + 0.5f) * pw, py + (y * 0.5f + 0.5f) * ph);
        // Fade out with distance (1 near, 0.35 far).
        const float alpha = std::clamp(1.0f - (depth - 600.0f) / 4000.0f, 0.35f, 1.0f);
        const ImVec2 size = ImGui::CalcTextSize(name.c_str());
        const ImVec2 tl(at.x - size.x * 0.5f - 6, at.y - size.y - 4), br(at.x + size.x * 0.5f + 6, at.y);
        dl->AddRectFilled(tl, br, ImGui::GetColorU32(ImVec4(0.05f, 0.05f, 0.1f, 0.55f * alpha)), 5.0f);
        dl->AddText(ImVec2(tl.x + 6, tl.y + 2), ImGui::GetColorU32(ImVec4(1.0f, 0.95f, 0.75f, alpha)), name.c_str());
    }
}

void onCombo() { kh2pgs::toggleMenu(); }
} // namespace

void install(PS2Runtime &runtime, const std::string &iniPath, const Options &options)
{
    g_opt = options;
    g_speed = options.baseSpeed;
    g_runtime = &runtime;
    g_iniPath = iniPath;
    kh2pgs::setUiCallback(&draw);
    kh2pgs::setHudCallback(&drawHud);
    kh2pgs::setWorldCallback(&drawNameTags);
    kh2input::setMenuComboCallback(&onCombo);
}

void tick()
{
    kh2input::setSuppressed(kh2pgs::menuOpen());
    if (!g_runtime)
        return;
    kh2pgs::setWorldOverlayActive(g_opt.nameTags && kh2coop::active() && !kh2puppets::assignments().empty());
    const bool paused = kh2pgs::menuOpen() || (g_opt.pauseUnfocused && !kh2pgs::windowFocused());
    if (paused != g_paused)
    {
        g_paused = paused;
        g_runtime->eeScheduler().setHostPaused(paused);
        ps2_native_iop::setPaused(paused);
    }
    bool ff = kh2input::fastForwardHeld() || g_testFastForward;
    if (ff && kh2movie::playing())
    {
        // Movies follow their audio, which does not speed up: fast-forward
        // only stuttered them. Tab skips the movie instead.
        static double s_lastSkip = -10.0;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (t - s_lastSkip > 2.0)
        {
            s_lastSkip = t;
            kh2movie::skipCurrent();
            kh2pgs::toast("Movie skipped");
        }
        ff = false;
    }
    const double speed = ff ? g_opt.fastForwardSpeed : g_opt.baseSpeed;
    if (speed != g_speed)
    {
        g_speed = speed;
        g_runtime->eeScheduler().setHostSpeed(speed);
    }
}

bool paused() { return g_paused; }
void setTestFastForward(bool on) { g_testFastForward = on; }
} // namespace kh2ui
