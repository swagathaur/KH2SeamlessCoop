#include "gummi.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "game_state.h"
#include "cd_iso.h"
#include "guest_call.h"
#include "hle_call.h"
#include "overlay_manager.h"
#include "ps2_runtime.h"

namespace kh2gummi
{
namespace
{
constexpr uint32_t kBattleMode = 0x0032BA48, kModeEnable = 0x00102080;
constexpr uint32_t kRoute = 0x0032BA7C, kVariant = 0x0032BA80, kLevel = 0x0032BA84, kEx = 0x0032BA88;
constexpr uint32_t kMainBuf = 0x003A6B00, kTeenyBuf = 0x003A7300, kTeenyBufSize = 0x400, kFlags = 0x0032BAD0;
constexpr uint32_t kGarage = kh2state::addr::Save + 0x55A0;
constexpr uint32_t kMainPlans = kGarage + 0x160, kMainPlanSize = 0x710;
constexpr uint32_t kTeenyPlans = kGarage + 0x8080, kTeenyPlanSize = 0x260;
constexpr int kSlots = 18;

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}

// A blueprint with blocks or decorations (the menu's own test, FUN_01F18A38).
bool usable(const uint8_t *ram, uint32_t plan)
{
    return rd<int16_t>(ram, plan + 8) > 0 || rd<int16_t>(ram, plan + 0xA) > 0;
}
// The preset blueprints, as the Gummi menu reads them (FUN_01F7F458,
// FUN_01F83888, FUN_01F83728): gumimenu/fm/planinfo.bar (in the Gummi
// files' own index, 000gumi.idx), its entry "0000" a BAR of its own whose
// entry 0 is an index (count, then 8 bytes: plan id s16, entry u16) and entry
// 1 the presets (count, then 0x20 bytes: kind, plan ids s16 at +2 main, +4
// Teeny 1, +6 Teeny 2, the gummi item granting it at +0xC); the other entries
// are the blueprints. Read from the disc image here, not by the game: its
// loader needs 000gumi.idx in the slot the field keeps its world's index in.
const char *const kPlanInfo = "gumimenu/fm/planinfo.bar";

struct Bar
{
    struct Entry
    {
        char name[5] = {};
        uint32_t offset = 0, size = 0;
    };
    std::vector<Entry> entries;
    const uint8_t *base = nullptr;
    size_t length = 0;
    bool read(const uint8_t *data, size_t len)
    {
        base = data;
        length = len;
        entries.clear();
        if (len < 0x10 || std::memcmp(data, "BAR", 3) != 0)
            return false;
        uint32_t n = 0;
        std::memcpy(&n, data + 4, 4);
        for (uint32_t i = 0; i < n && 0x10 + (i + 1) * 0x10 <= len; ++i)
        {
            const uint8_t *e = data + 0x10 + i * 0x10;
            Entry b;
            std::memcpy(b.name, e + 4, 4);
            std::memcpy(&b.offset, e + 8, 4);
            std::memcpy(&b.size, e + 12, 4);
            if (size_t(b.offset) + b.size <= len)
                entries.push_back(b);
        }
        return true;
    }
    const uint8_t *data(size_t i) const { return i < entries.size() ? base + entries[i].offset : nullptr; }
};

std::vector<uint8_t> g_planInfo; // the file
Bar g_plans;                     // its entry "0000"

bool planInfo()
{
    if (!g_plans.entries.empty())
        return true;
    if (!kh2cd::readGameFile(kPlanInfo, g_planInfo, "000gumi.idx"))
    {
        std::printf("[gummi] %s: not on the disc\n", kPlanInfo);
        return false;
    }
    Bar outer;
    if (outer.read(g_planInfo.data(), g_planInfo.size()))
        for (size_t i = 0; i < outer.entries.size(); ++i)
            if (std::memcmp(outer.entries[i].name, "0000", 4) == 0)
                g_plans.read(outer.data(i), outer.entries[i].size);
    std::printf("[gummi] %s: %zu bytes, %zu entries in its blueprints\n", kPlanInfo, g_planInfo.size(),
                g_plans.entries.size());
    if (g_plans.entries.size() < 2)
        g_plans.entries.clear();
    return !g_plans.entries.empty();
}

template <typename T> T get(const uint8_t *p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// Preset `p`'s plan id for ship `k` (0 main, 1 Teeny 1, 2 Teeny 2), or -1.
int16_t presetPlan(int p, int k)
{
    if (g_plans.entries.size() < 2 || p < 0)
        return -1;
    const uint8_t *list = g_plans.data(1);
    if (uint32_t(p) >= get<uint32_t>(list) || 4 + (p + 1) * 0x20 > int(g_plans.entries[1].size))
        return -1;
    return get<int16_t>(list + 4 + p * 0x20 + 2 + k * 2);
}
// A blueprint's bytes by plan id (empty: none).
std::vector<uint8_t> planById(int16_t id)
{
    if (g_plans.entries.size() < 2 || id < 0)
        return {};
    const uint8_t *index = g_plans.data(0);
    const uint32_t n = get<uint32_t>(index);
    for (uint32_t i = 0; i < n && 4 + (i + 1) * 8 <= g_plans.entries[0].size; ++i)
        if (get<int16_t>(index + 4 + i * 8) == id)
        {
            const uint16_t entry = get<uint16_t>(index + 4 + i * 8 + 2);
            if (const uint8_t *d = g_plans.data(entry))
                return std::vector<uint8_t>(d, d + g_plans.entries[entry].size);
        }
    return {};
}

// Fills the mission choice and starts the battle mode (or nothing if there is
// no main ship).
std::optional<kh2guest::Call> launch(uint8_t *ram, int route, int level, bool ex)
{
    auto put = [&](uint32_t a, uint32_t v) { std::memcpy(ram + a, &v, 4); };
    const int lv = level < 1 ? 1 : level > 3 ? 3 : level;
    put(kRoute, uint32_t(route));
    put(kLevel, uint32_t(lv));
    put(kVariant, uint32_t(lv)); // as FUN_00104210 sets it
    ram[kEx] = ex ? 1 : 0;
    std::printf("[gummi] starting route %d level %d%s: ships %u %u %u\n", route, lv, ex ? " EX" : "", ram[kFlags],
                ram[kFlags + 1], ram[kFlags + 2]);
    std::fflush(stdout);
    if (!ram[kFlags])
        return std::nullopt;
    return kh2guest::Call{kModeEnable, {kBattleMode}, {}};
}
// --- Piloted Teeny Ships -----------------------------------------------------
//
// The main ship (0x1440 bytes, vtable 0x01F81060) holds the Teeny Ships at
// +0x5D0/+0x5D4 and their formation at +0x5E0 (FUN_01F2CA10 each frame, 8
// kinds). The formation works out a target for each Teeny relative to the
// main ship: Teeny 0 position +0x10, rotation +0x20 (current +0x30 / +0x40),
// Teeny 1 +0x50 / +0x60 (+0x70 / +0x80); FUN_01F280A0 / FUN_01F281A0 then
// bring the current ones toward the targets, and FUN_01F28BF0 places the
// Teenies from the main ship's matrix. Firing: the main ship's fire
// (FUN_01F1EDxx) fires its own body (+0x3E0, FUN_01F2DCE0) and each Teeny's
// (FUN_01F27A68 -> FUN_01F2DCE0(teeny + 0x3A0)); lock-on missiles likewise
// (FUN_01F27A80 / FUN_01F27A98). The main ship's update is FUN_01F219A8
// (vtable +8), once a frame while the mission runs.
constexpr uint32_t kShipUpdate = 0x01F219A8, kApplyTeeny0 = 0x01F280A0, kApplyTeeny1 = 0x01F281A0;
constexpr uint32_t kTeenyFire = 0x01F27A68, kTeenyLockOn = 0x01F27A80, kTeenyMissile = 0x01F27A98;
constexpr uint32_t kBodyFire = 0x01F2DCE0, kTeenyBody = 0x3A0;
constexpr uint32_t kShipTeenies = 0x5D0, kShipFormation = 0x5E0, kFormationTeenies = 0x98;
constexpr uint32_t kFrameDelta = 0x00349E10; // the frame's time (1 at 60 fps)

PS2Runtime::RecompiledFunction g_shipUpdate = nullptr, g_apply[2] = {}, g_teenyFire = nullptr;

enum class Mode { Off, Local, Remote, Test };
struct Pilot
{
    Mode mode = Mode::Off;
    PilotInput in;
    bool wasFire = false;
    bool placed = false;  // offset taken from where the formation had it
    float offset[3] = {}; // the Teeny's place relative to the main ship
    int cooldown = 0;
    uint16_t shots = 0;       // Local/Test: shots fired here (sent)
    uint16_t remoteShots = 0; // Remote: its pilot's count, as last heard
    uint16_t shotsSeen = 0;   // Remote: fired here so far
    bool heard = false;       // Remote: offset heard from its pilot
};
std::mutex g_pilotMutex;
void enemiesStep(uint8_t *rdram);
std::atomic<uint32_t> g_dbgHitCalls{0}, g_dbgShots{0}; // (for the log)
Pilot g_pilots[2];
// Client: the main ship is the host's (its place from the host; our pad does
// not move it). The host's main ship, as last heard: its object's +0x10..+0x5F
// (position, rotation and the rest, relative to the rail: FUN_01F1C618).
constexpr uint32_t kShipPlace = 0x10, kShipPlaceSize = 0x50;
bool g_followShip = false;
bool g_heardShip = false;
uint8_t g_hostShip[kShipPlaceSize] = {};
constexpr uint32_t kPad0 = 0x0034D6C0, kPadSize = 0x60; // PAD(0): held u64 +0, pressed u64 +8, stick f32 x2 +0x30
constexpr uint64_t kFireButton = 0x100;

// Steering: units per frame at full stick, and the room around the main ship
// (the formation's own Teenies sit about 60 to each side of it).
constexpr float kSpeed = 2.0f, kRoomX = 110.f, kRoomY = 70.f;

PS2Runtime::RecompiledFunction hookOverlay(const char *overlay, uint32_t address, PS2Runtime::RecompiledFunction fn)
{
    for (size_t i = 0; i < kh2ovl::kOverlayCount; ++i)
        if (std::strcmp(kh2ovl::kOverlays[i].name, overlay) == 0)
        {
            PS2Runtime::FunctionRegion &r = kh2ovl::kOverlays[i].region();
            if (address < r.base || address >= r.end || !r.slots)
                return nullptr;
            PS2Runtime::RecompiledFunction &slot = r.slots[(address - r.base) >> 2];
            PS2Runtime::RecompiledFunction original = slot;
            if (original)
                slot = fn;
            return original;
        }
    return nullptr;
}

float rdf(const uint8_t *ram, uint32_t a) { return rd<float>(ram, a); }
void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, 4); }

void testScript(Pilot &p, uint64_t frame, int k)
{
    const float t = float(frame) * 0.03f + float(k) * 3.14159f;
    p.in.stick[0] = std::cos(t);
    p.in.stick[1] = std::sin(t * 0.7f);
    p.in.fire = (frame / 20) % 3 == 0;
}

void fire(uint32_t teeny)
{
    ++g_dbgShots;
    kh2guest::postCall(kh2guest::Call{kBodyFire, {teeny + kTeenyBody}, {}}, {}, kh2guest::kGummi);
}

void shipUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc != kShipUpdate)
    {
        kh2::callOriginal(g_shipUpdate, rdram, ctx, runtime);
        return;
    }
    kh2guest::runPending(rdram, ctx, runtime, kShipUpdate, kh2guest::kGummi); // may not return
    enemiesStep(rdram);
    const uint32_t ship = getRegU32(ctx, 4);
    static uint64_t frame = 0;
    ++frame;
    uint8_t pad[kPadSize];
    std::memcpy(pad, rdram + kPad0, kPadSize);
    bool follow = false;
    {
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        for (int k = 0; k < 2; ++k)
        {
            Pilot &p = g_pilots[k];
            if (p.mode == Mode::Off)
                continue;
            const uint32_t teeny = rd<uint32_t>(rdram, ship + kShipTeenies + 4 * k);
            if (p.mode == Mode::Remote)
            {
                // Its pilot's shots, fired here too (a few at most at once).
                int n = 0;
                while (p.shotsSeen != p.remoteShots && n++ < 3)
                {
                    ++p.shotsSeen;
                    if (teeny)
                        fire(teeny);
                }
                p.shotsSeen = p.remoteShots;
                continue;
            }
            if (p.mode == Mode::Test)
                testScript(p, frame, k);
            else // Local: this machine's pad
            {
                uint64_t held = 0;
                std::memcpy(&held, pad, 8);
                float stick[2];
                std::memcpy(stick, pad + 0x30, 8);
                p.in.fire = (held & kFireButton) != 0;
                for (int i = 0; i < 2; ++i)
                    p.in.stick[i] = std::isfinite(stick[i]) ? std::fmax(-1.f, std::fmin(1.f, stick[i])) : 0.f;
            }
            const bool press = p.in.fire && !p.wasFire;
            p.wasFire = p.in.fire;
            if (p.cooldown > 0)
                --p.cooldown;
            if (teeny && (press || (p.in.fire && p.cooldown == 0)))
            {
                p.cooldown = 8;
                ++p.shots;
                fire(teeny);
            }
        }
        follow = g_followShip && g_heardShip;
        if (follow)
            std::memcpy(rdram + ship + kShipPlace, g_hostShip, kShipPlaceSize);
    }
    if (g_followShip)
    {
        // Our pad flies our Teeny, not the main ship: the main ship's update
        // sees none (buttons and stick cleared, put back after).
        std::memset(rdram + kPad0, 0, 0x10);
        std::memset(rdram + kPad0 + 0x30, 0, 8);
    }
    kh2::callOriginal(g_shipUpdate, rdram, ctx, runtime);
    if (g_followShip)
    {
        std::memcpy(rdram + kPad0, pad, 0x10);
        std::memcpy(rdram + kPad0 + 0x30, pad + 0x30, 8);
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        if (g_heardShip)
            std::memcpy(rdram + ship + kShipPlace, g_hostShip, kShipPlaceSize);
    }
}

// FUN_01F280A0 / FUN_01F281A0(formation): a piloted Teeny's target is where
// its pilot steers it.
template <int K> void applyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == (K == 0 ? kApplyTeeny0 : kApplyTeeny1))
    {
        const uint32_t f = getRegU32(ctx, 4);
        const uint32_t target = f + 0x10 + 0x40 * K, current = f + 0x30 + 0x40 * K;
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        Pilot &p = g_pilots[K];
        if (p.mode == Mode::Remote && p.heard)
        {
            for (int i = 0; i < 3; ++i)
                wrf(rdram, target + 4 * i, p.offset[i]);
            for (int i = 0; i < 3; ++i)
                wrf(rdram, target + 0x10 + 4 * i, 0.f);
        }
        else if (p.mode == Mode::Local || p.mode == Mode::Test)
        {
            if (!p.placed)
            {
                for (int i = 0; i < 3; ++i)
                    p.offset[i] = rdf(rdram, current + 4 * i);
                p.placed = true;
            }
            float dt = rdf(rdram, kFrameDelta);
            if (!(dt > 0 && dt < 4))
                dt = 1;
            p.offset[0] = std::fmax(-kRoomX, std::fmin(kRoomX, p.offset[0] + p.in.stick[0] * kSpeed * dt));
            p.offset[1] = std::fmax(-kRoomY, std::fmin(kRoomY, p.offset[1] + p.in.stick[1] * kSpeed * dt));
            for (int i = 0; i < 3; ++i)
                wrf(rdram, target + 4 * i, p.offset[i]);
            for (int i = 0; i < 3; ++i) // facing: the main ship's
                wrf(rdram, target + 0x10 + 4 * i, 0.f);
        }
        static int logged = 0;
        if (logged < 12 && (logged++ % 4) == 0)
            std::printf("[gummi] Teeny %d target %.1f %.1f %.1f current %.1f %.1f %.1f mode %d\n", K,
                        rdf(rdram, target), rdf(rdram, target + 4), rdf(rdram, target + 8), rdf(rdram, current),
                        rdf(rdram, current + 4), rdf(rdram, current + 8), int(p.mode));
    }
    kh2::callOriginal(g_apply[K], rdram, ctx, runtime);
}

// The main ship's fire and missiles reach a piloted Teeny no more.
bool piloted(uint8_t *rdram, uint32_t teeny)
{
    const uint32_t ship = rd<uint32_t>(rdram, 0x01F7DE44);
    if (ship < 0x00100000u || ship >= 0x02000000u)
        return false;
    std::lock_guard<std::mutex> lock(g_pilotMutex);
    for (int k = 0; k < 2; ++k)
        if (g_pilots[k].mode != Mode::Off && rd<uint32_t>(rdram, ship + kShipTeenies + 4 * k) == teeny)
            return true;
    return false;
}
void teenyFireHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kTeenyFire && piloted(rdram, getRegU32(ctx, 4)))
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_teenyFire, rdram, ctx, runtime);
}
bool g_clientMission = false; // this machine follows the host's mission (a client in one)
constexpr uint32_t kGummiSave = kh2state::addr::Save + 0x100E0, kGummiSaveSize = 0x10FC0 - 0x100E0;
std::vector<uint8_t> g_savedGummi; // client: our Gummi save data as the mission began
bool g_hostMission = false;   // this machine leads one with others in it (the host)
void clearEnemies();

// --- Co-op launch -------------------------------------------------------------
constexpr uint32_t kBattleInit = 0x00103F68; // the Gummi battle mode's start (loads the overlay)
constexpr uint32_t kTaskYield = 0x00102448;  // every task's yield: a frame hook in any mode
constexpr size_t kSortieHeader = 10, kSortieSize = kSortieHeader + 0x800 + 2 * kTeenyBufSize;
PS2Runtime::RecompiledFunction g_battleInit = nullptr, g_taskYield = nullptr;

std::mutex g_netMutex;
bool g_inSession = false, g_isHost = false;
uint8_t g_localId = 0;
std::vector<uint8_t> g_others;                  // the other players' ids, in order
std::vector<uint8_t> g_outSortie;               // host: to send
std::vector<uint8_t> g_inSortie;                // client: to start
std::atomic<bool> g_starting{false};            // client: our own start of the host's mission
uint8_t g_pilotIds[2] = {0xFF, 0xFF}; // who flies each Teeny in this mission
int g_notice = 0; // a launch to tell the player about (1), with g_pilotIds

// The host's garage's k-th Teeny blueprint (empty: none).
std::vector<uint8_t> garageTeeny(const uint8_t *ram, int k)
{
    int found = 0;
    for (int i = 0; i < kSlots; ++i)
    {
        const uint32_t p = kTeenyPlans + uint32_t(i) * kTeenyPlanSize;
        if (usable(ram, p) && found++ == k)
            return std::vector<uint8_t>(ram + p, ram + p + kTeenyPlanSize);
    }
    return {};
}

// A preset's Teeny blueprint, when the host has none (the first preset
// with two Teeny Ships).
std::vector<uint8_t> presetTeeny(int k)
{
    if (!planInfo())
        return {};
    const uint8_t *list = g_plans.data(1);
    const uint32_t n = get<uint32_t>(list);
    for (uint32_t p = 0; p < n; ++p)
        if (presetPlan(int(p), 1) >= 0 && presetPlan(int(p), 2) >= 0)
            return planById(presetPlan(int(p), 1 + k));
    return {};
}

// Host, at its launch: the Teeny Ships to the other players, and the mission
// for them.
void hostLaunch(uint8_t *ram)
{
    std::lock_guard<std::mutex> lock(g_netMutex);
    uint8_t pilots[2] = {0xFF, 0xFF};
    for (int k = 0; k < 2 && k < int(g_others.size()); ++k)
    {
        pilots[k] = g_others[size_t(k)];
        // The host's Teeny Ships (it has the parts): the one it chose in the
        // menu, else one of its garage's, else a preset.
        const char *from = "the host's choice";
        std::vector<uint8_t> plan;
        if (!ram[kFlags + 1 + k])
        {
            plan = garageTeeny(ram, k);
            from = "the host's garage";
            if (plan.empty())
            {
                plan = presetTeeny(k);
                from = "a preset";
            }
        }
        if (!plan.empty() && plan.size() <= kTeenyBufSize)
        {
            std::memset(ram + kTeenyBuf + k * kTeenyBufSize, 0, kTeenyBufSize);
            std::memcpy(ram + kTeenyBuf + k * kTeenyBufSize, plan.data(), plan.size());
            ram[kFlags + 1 + k] = usable(ram, kTeenyBuf + k * kTeenyBufSize) ? 1 : 0;
        }
        std::printf("[gummi] Teeny %d: player %u flies %s\n", k, pilots[k], from);
    }
    {
        std::lock_guard<std::mutex> pl(g_pilotMutex);
        for (int k = 0; k < 2; ++k)
        {
            g_pilots[k] = Pilot{};
            g_pilots[k].mode = pilots[k] != 0xFF ? Mode::Remote : Mode::Off;
        }
        g_followShip = false;
        g_heardShip = false;
    }
    g_pilotIds[0] = pilots[0];
    g_pilotIds[1] = pilots[1];
    g_notice = 1;
    clearEnemies();
    g_hostMission = !g_others.empty();
    g_clientMission = false;
    std::vector<uint8_t> b(kSortieSize, 0);
    b[0] = 1;
    b[1] = uint8_t(rd<uint32_t>(ram, kRoute));
    b[2] = uint8_t(rd<uint32_t>(ram, kLevel));
    b[3] = uint8_t(rd<uint32_t>(ram, kVariant));
    b[4] = ram[kEx];
    for (int k = 0; k < 3; ++k)
        b[5 + k] = ram[kFlags + k];
    b[8] = pilots[0];
    b[9] = pilots[1];
    std::memcpy(b.data() + kSortieHeader, ram + kMainBuf, 0x800);
    std::memcpy(b.data() + kSortieHeader + 0x800, ram + kTeenyBuf, 2 * kTeenyBufSize);
    g_outSortie = std::move(b);
    std::printf("[gummi] launch: route %u level %u%s, ships %u %u %u, to %zu player(s)\n", unsigned(g_outSortie[1]),
                unsigned(g_outSortie[2]), g_outSortie[4] ? " EX" : "", g_outSortie[5], g_outSortie[6], g_outSortie[7],
                g_others.size());
    std::fflush(stdout);
}

void battleInitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kBattleInit)
    {
        bool host = false;
        {
            std::lock_guard<std::mutex> lock(g_netMutex);
            host = g_inSession && g_isHost;
        }
        if (host)
            hostLaunch(rdram);
        else if (g_starting.exchange(false))
            std::printf("[gummi] starting the host's mission\n");
    }
    kh2::callOriginal(g_battleInit, rdram, ctx, runtime);
}

// Client: the host's mission, started here from any mode (the hook of every
// task's yield runs the job while one is waiting).
std::atomic<bool> g_startWaiting{false};
std::atomic<bool> g_flowWaiting{false}; // client: the host's retry or quit to make here
void taskYieldHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kTaskYield && (g_startWaiting.load(std::memory_order_relaxed) || g_flowWaiting.load(std::memory_order_relaxed)))
        kh2guest::runPending(rdram, ctx, runtime, kTaskYield, kh2guest::kGummi); // may not return
    kh2::callOriginal(g_taskYield, rdram, ctx, runtime);
}

void startHostMission()
{
    g_startWaiting = true;
    kh2guest::post([](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
        {
            g_startWaiting = false;
            return std::nullopt;
        }
        std::vector<uint8_t> b;
        {
            std::lock_guard<std::mutex> lock(g_netMutex);
            b.swap(g_inSortie);
        }
        uint8_t *ram = env.rdram;
        if (b.size() != kSortieSize || std::strcmp(kh2ovl::activeName(), "gumibattle") == 0)
        {
            g_startWaiting = false;
            return std::nullopt;
        }
        std::memcpy(ram + kMainBuf, b.data() + kSortieHeader, 0x800);
        std::memcpy(ram + kTeenyBuf, b.data() + kSortieHeader + 0x800, 2 * kTeenyBufSize);
        for (int k = 0; k < 3; ++k)
            ram[kFlags + k] = b[5 + k];
        const uint32_t level = b[2], variant = b[3];
        // Rewards are the host's alone: our Gummi save data (mission records,
        // gummi items, treasures: save +0x100E0..+0x10FC0) comes back as it
        // was when the mission ends.
        g_savedGummi.assign(ram + kGummiSave, ram + kGummiSave + kGummiSaveSize);
        g_starting = true;
        auto call = launch(ram, b[1], int(level), b[4] != 0);
        std::memcpy(ram + kVariant, &variant, 4); // (the host's, after launch's own)
        if (!call)
        {
            g_starting = false;
            g_startWaiting = false;
        }
        return call;
    }, kh2guest::kGummi);
}

// --- The mission's course: pause, retry, quit (phase 7) -------------------
//
// The host's pause (GumiPause, a menu mode: the battle mode's state goes to
// 2 under it) and game over menus put their choice in the pause block's +3
// (*0x0032BA78, GUMIBATTLE::GetPauseParam): 0 go on, 1 retry, 2 the Gummi
// menu, 3 the World Map. The menu's callback (pause FUN_01F19340, game over
// FUN_01F19730, results FUN_01F186A0, at the block's +4) retries with
// FUN_01F18510; FUN_00104108 leaves for the Gummi menu (2) or the World Map
// (3); the host's leaving is seen as its battle mode ending with that choice
// made (FUN_00104108 is reached by a direct call). A client follows the
// host's: it cannot pause (FUN_01F19240 opens the pause menu; passed by), its
// game stands still while the host's is under a menu (the game speed,
// 0x00349E0C, at 0), and it retries or leaves when the host does (GummiFlow).
constexpr uint32_t kPauseOpen = 0x01F19240, kRetry = 0x01F18510, kLeave = 0x00104108, kPauseBlock = 0x0032BA78;
constexpr uint32_t kPauseDone = 0x01F19340, kGameOverDone = 0x01F19730, kResultsDone = 0x01F186A0;
constexpr uint32_t kGameSpeed = 0x00349E0C;
PS2Runtime::RecompiledFunction g_pauseOpen = nullptr, g_pauseDone = nullptr, g_gameOverDone = nullptr,
                               g_resultsDone = nullptr;
std::vector<uint8_t> g_flowOut;        // host: [kind, value] to send (1 retry, 2 leave)
std::atomic<bool> g_hostPaused{false}; // client: the host's game is under a menu
bool g_frozen = false;                 // client: stood still for the host's menu
float g_speedBefore = 1.f;

void pauseOpenHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kPauseOpen && g_clientMission)
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_pauseOpen, rdram, ctx, runtime);
}

uint8_t pauseChoice(const uint8_t *ram)
{
    const uint32_t block = rd<uint32_t>(ram, kPauseBlock);
    return block >= 0x00100000u && block < 0x02000000u ? ram[block + 3] : 0;
}

// Host: a menu's callback, its choice made: a retry for the others too.
template <uint32_t Entry, PS2Runtime::RecompiledFunction *Original>
void menuDoneHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == Entry && g_hostMission && pauseChoice(rdram) == 1)
    {
        {
            std::lock_guard<std::mutex> lock(g_netMutex);
            g_flowOut = {1, 1};
        }
        clearEnemies(); // (the mission starts over: its enemies are named anew)
        std::printf("[gummi] the host retries the mission\n");
    }
    kh2::callOriginal(*Original, rdram, ctx, runtime);
}

// Client: stand still while the host's game is under a menu.
void setFrozen(uint8_t *ram, bool on)
{
    if (on == g_frozen)
        return;
    g_frozen = on;
    if (on)
    {
        g_speedBefore = rdf(ram, kGameSpeed);
        if (!(g_speedBefore > 0.f && g_speedBefore < 10.f))
            g_speedBefore = 1.f;
        wrf(ram, kGameSpeed, 0.f);
    }
    else
        wrf(ram, kGameSpeed, g_speedBefore);
    std::printf("[gummi] %s with the host's game\n", on ? "standing still" : "going on");
}

// Client: the host's retry or leaving, made here.
void followFlow(uint8_t kind, uint8_t value)
{
    g_flowWaiting = true;
    kh2guest::post(
        [kind, value](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
            if (!first)
            {
                g_flowWaiting = false;
                return std::nullopt;
            }
            uint8_t *ram = env.rdram;
            if (std::strcmp(kh2ovl::activeName(), "gumibattle") != 0 || !inGummi(ram))
            {
                g_flowWaiting = false;
                return std::nullopt;
            }
            const uint32_t block = rd<uint32_t>(ram, kPauseBlock);
            if (block >= 0x00100000u && block < 0x02000000u)
                ram[block + 3] = value;
            if (kind == 1)
                clearEnemies();
            std::printf("[gummi] %s, as the host did\n", kind == 1 ? "retrying the mission" : "leaving the mission");
            std::fflush(stdout);
            return kh2guest::Call{kind == 1 ? kRetry : kLeave, {}, {}};
        },
        kh2guest::kGummi);
}
} // namespace

bool inGummi(const uint8_t *ram)
{
    // (2: suspended under a menu mode: the pause, game over and results menus)
    constexpr uint32_t kMenuMode = 0x0032BA90, kModeState = 0x1C;
    const uint32_t b = rd<uint32_t>(ram, kBattleMode + kModeState), m = rd<uint32_t>(ram, kMenuMode + kModeState);
    return b == 1 || b == 2 || m == 1 || m == 2;
}

void setSession(bool active, bool host, uint8_t localId, const std::vector<uint8_t> &others)
{
    std::lock_guard<std::mutex> lock(g_netMutex);
    g_inSession = active;
    g_isHost = host;
    g_localId = localId;
    g_others = others;
    std::sort(g_others.begin(), g_others.end());
    // Host: a pilot who left gives their Teeny back to the formation.
    if (g_hostMission)
        for (int k = 0; k < 2; ++k)
            if (g_pilotIds[k] != 0xFF && !std::binary_search(g_others.begin(), g_others.end(), g_pilotIds[k]))
            {
                std::printf("[gummi] Teeny %d's pilot left: it flies in formation again\n", k);
                g_pilotIds[k] = 0xFF;
                std::lock_guard<std::mutex> pl(g_pilotMutex);
                g_pilots[k] = Pilot{};
            }
    if (!active)
    {
    }
}

constexpr size_t kPiece = 900;
bool takeSortie(std::vector<std::vector<uint8_t>> &out)
{
    std::lock_guard<std::mutex> lock(g_netMutex);
    if (g_outSortie.empty())
        return false;
    static uint8_t launches = 0;
    ++launches;
    const size_t pieces = (g_outSortie.size() + kPiece - 1) / kPiece;
    out.clear();
    for (size_t i = 0; i < pieces; ++i)
    {
        std::vector<uint8_t> p = {launches, uint8_t(i), uint8_t(pieces)};
        const size_t from = i * kPiece, n = std::min(kPiece, g_outSortie.size() - from);
        p.insert(p.end(), g_outSortie.begin() + long(from), g_outSortie.begin() + long(from + n));
        out.push_back(std::move(p));
    }
    g_outSortie.clear();
    return true;
}

void onHostSortie(const std::vector<uint8_t> &piece)
{
    static uint8_t launch = 0, got = 0;
    static std::vector<uint8_t> data;
    if (piece.size() < 4 || piece[2] == 0 || piece[1] >= piece[2])
        return;
    if (piece[0] != launch || piece[1] == 0)
    {
        launch = piece[0];
        got = 0;
        data.assign(size_t(piece[2]) * kPiece, 0);
    }
    if (piece[1] != got)
        return; // (in order: the channel is reliable and ordered)
    std::memcpy(data.data() + size_t(piece[1]) * kPiece, piece.data() + 3, std::min(piece.size() - 3, kPiece));
    if (++got < piece[2])
        return;
    data.resize(kSortieSize);
    if (data[0] != 1)
        return;
    {
        std::lock_guard<std::mutex> lock(g_netMutex);
        g_inSortie = data;
        g_pilotIds[0] = data[8];
        g_pilotIds[1] = data[9];
        g_notice = 1;
        std::lock_guard<std::mutex> pl(g_pilotMutex);
        for (int k = 0; k < 2; ++k)
        {
            g_pilots[k] = Pilot{};
            g_pilots[k].mode = data[8 + k] == 0xFF ? Mode::Off : data[8 + k] == g_localId ? Mode::Local : Mode::Remote;
        }
        g_followShip = true;
        g_heardShip = false;
        clearEnemies();
        g_clientMission = true;
        g_hostMission = false;
        std::printf("[gummi] the host launched route %u level %u; Teeny pilots %d %d (we are %u)\n", data[1], data[2],
                    data[8] == 0xFF ? -1 : data[8], data[9] == 0xFF ? -1 : data[9], g_localId);
    }
    std::fflush(stdout);
    startHostMission();
}

namespace
{
void putF(std::vector<uint8_t> &b, float v)
{
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
    b.insert(b.end(), p, p + 4);
}
void putU16(std::vector<uint8_t> &b, uint16_t v)
{
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
constexpr uint32_t kShipPtr = 0x01F7DE44, kShipVtable = 0x01F81060;
uint32_t missionShip(const uint8_t *ram)
{
    if (!inGummi(ram) || std::strcmp(kh2ovl::activeName(), "gumibattle") != 0)
        return 0;
    const uint32_t ship = rd<uint32_t>(ram, kShipPtr);
    if (ship < 0x00100000u || ship >= 0x02000000u || rd<uint32_t>(ram, ship) != kShipVtable)
        return 0;
    return ship;
}

// --- Enemies in step (phase 6) ----------------------------------------------
//
// Every object of a mission is made by FUN_01F1E0C8(id, pos, rot); enemies
// (vtables 0x01F810B8 and 0x01F816C0) keep their HP at +0x3A4 (0: cannot be
// hurt; below 1 it is set to -1, and the enemy's own update then has it die,
// FUN_01F26590(enemy, credited): score, kill count, drop). Enemies are known
// the same way on every machine by where they come from: a wave spawner
// (FUN_01F42378(spawner): its record +4, among the spawn manager's at
// *0x01F91E50, 0xB8 each; wave +0x10; unit +0x12; the new object at +0x24
// after), a part of another (FUN_01F1B640(child, parent, part, index)), or
// else the order they appear in.
//
// The host's game decides. Every machine fires each pilot's Teeny (phase 5),
// so the host's own copy of a client's shots does the host's damage; nothing
// more is sent for hits. The host sends its enemies' deaths (an enemy known
// with HP below 0, or marked by FUN_01F26590, +0x3BC bit 0, as it goes); on
// a client an enemy dies only then (its HP set to -1, so it dies the game's
// own way in its update), and one hit to death there before keeps HP 1
// (FUN_01F26EE0 / 0x01F5F5D0, its update, hooked: FUN_01F26590 itself is
// reached by a tail jump, a direct call the hooks never see). The course's
// time (0x01F8F8E0 +0xC; its step, FUN_01F3C7A8, works out the rest from it),
// the main ship's HP (the player's +0x4A8 stats, +4 f32; damage FUN_01F203A8
// does nothing on a client) and the score (0x01F80398 +4) are the host's.
constexpr uint32_t kMakeObject = 0x01F1E0C8, kSpawnUnit = 0x01F42378, kAttachPart = 0x01F1B640;
constexpr uint32_t kEnemyHit = 0x01F26998, kEnemyDamage = 0x01F26B20;
constexpr uint32_t kCourseStep = 0x01F3C7A8, kCourse = 0x01F8F8E0, kCourseTime = 0xC;
constexpr uint32_t kShipDamage = 0x01F203A8, kShipStats = 0x4A8, kScore = 0x01F80398 + 4;
constexpr uint32_t kEnemyHp = 0x3A4, kEnemyFlags = 0x3BC, kEnemyVtable1 = 0x01F810B8, kEnemyVtable2 = 0x01F816C0;
constexpr uint32_t kSpawnRecords = 0x01F91E50, kSpawnRecordSize = 0xB8;

PS2Runtime::RecompiledFunction g_spawnUnit = nullptr, g_attachPart = nullptr,
                               g_enemyHit = nullptr, g_enemyDamage = nullptr,
                               g_courseStep = nullptr, g_shipDamage = nullptr;

std::mutex g_enemyMutex;
std::unordered_map<uint32_t, uint32_t> g_idOf;   // object -> id
std::unordered_map<uint32_t, uint32_t> g_objOf;  // id -> object
uint32_t g_madeCount = 0;                        // objects made in this mission (not by a spawner or as a part)
std::vector<uint32_t> g_killsOut;                // host: deaths to send
std::unordered_set<uint32_t> g_killsIn;          // client: the host's deaths not yet here
std::vector<std::pair<uint32_t, int32_t>> g_hitsOut; // client: hits to send
std::vector<std::pair<uint32_t, int32_t>> g_hitsIn;  // host: the clients' hits, to take off
uint32_t g_statHits = 0, g_statKills = 0;            // (for the log)
std::unordered_map<uint32_t, int32_t> g_hitBefore;   // client: enemies hit this frame, HP before
std::unordered_set<uint32_t> g_mayDie;               // client: enemies the host killed
std::unordered_set<uint32_t> g_reported;             // host: deaths sent
bool g_haveHostTime = false;
float g_hostTime = 0, g_hostHp = 0;
uint32_t g_hostScore = 0;
bool g_haveHostHp = false;

bool isEnemy(const uint8_t *ram, uint32_t o)
{
    if (o < 0x00100000u || o >= 0x02000000u)
        return false;
    const uint32_t v = rd<uint32_t>(ram, o);
    return v == kEnemyVtable1 || v == kEnemyVtable2;
}

void name(uint8_t *ram, uint32_t obj, uint32_t id)
{
    if (!obj)
        return;
    std::lock_guard<std::mutex> lock(g_enemyMutex);
    const auto old = g_idOf.find(obj);
    if (old != g_idOf.end())
        g_objOf.erase(old->second);
    g_idOf[obj] = id;
    g_objOf[id] = obj;
    g_mayDie.erase(obj); // (a new object where an old one was)
}

uint32_t idOf(uint32_t obj)
{
    std::lock_guard<std::mutex> lock(g_enemyMutex);
    const auto it = g_idOf.find(obj);
    return it == g_idOf.end() ? 0 : it->second;
}

// Each enemy, as it is made: both constructors (FUN_01F243A8, FUN_01F242A0)
// count it in the results with FUN_01F646A0(results, enemy). Named by the
// wave spawner that is making it (FUN_01F42378, at its entry), else by order.
// (A hook cannot run code after an overlay function's original returns: the
// original hands back to the dispatcher at its first call out, and the rest
// of it runs without the hook.)
constexpr uint32_t kCountSpawned = 0x01F646A0;
PS2Runtime::RecompiledFunction g_countSpawned = nullptr;
uint32_t g_pendingKey = 0; // a spawner's unit, being made
void countSpawnedHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kCountSpawned)
    {
        const uint32_t obj = getRegU32(ctx, 5);
        uint32_t id = g_pendingKey;
        g_pendingKey = 0;
        if (!id)
            id = 0x40000000u | (++g_madeCount & 0x3FFFFFFFu);
        name(rdram, obj, id);
    }
    kh2::callOriginal(g_countSpawned, rdram, ctx, runtime);
}

void spawnUnitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kSpawnUnit)
    {
        const uint32_t spawner = getRegU32(ctx, 4);
        const uint32_t rec = rd<uint32_t>(rdram, spawner + 4), base = rd<uint32_t>(rdram, kSpawnRecords);
        const uint32_t index = rec >= base ? (rec - base) / kSpawnRecordSize : 0xFFF;
        g_pendingKey = 0x10000000u | (index & 0xFFF) << 16 | (rd<uint16_t>(rdram, spawner + 0x10) & 0xFF) << 8 |
                       (rd<uint16_t>(rdram, spawner + 0x12) & 0xFF);
    }
    kh2::callOriginal(g_spawnUnit, rdram, ctx, runtime);
}

void attachPartHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kAttachPart)
    {
        const uint32_t child = getRegU32(ctx, 4), parent = getRegU32(ctx, 5), index = getRegU32(ctx, 7);
        const uint32_t p = idOf(parent);
        if (p)
            name(rdram, child, 0x80000000u | ((p * 31u + index + 1u) & 0x7FFFFFFFu));
    }
    kh2::callOriginal(g_attachPart, rdram, ctx, runtime);
}

// A client's hits: noted as they come (the HP before); each frame the HP lost
// goes to the host. An enemy dies on a client only when the host's dies
// (FUN_01F26590 is passed by otherwise, its HP kept at 1).
template <uint32_t Entry, PS2Runtime::RecompiledFunction *Original>
void enemyHitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == Entry)
        ++g_dbgHitCalls;
    if (ctx->pc == Entry && g_clientMission)
    {
        const uint32_t enemy = getRegU32(ctx, 4);
        if (isEnemy(rdram, enemy))
        {
            std::lock_guard<std::mutex> lock(g_enemyMutex);
            g_hitBefore.emplace(enemy, rd<int32_t>(rdram, enemy + kEnemyHp));
        }
    }
    kh2::callOriginal(*Original, rdram, ctx, runtime);
}

// On a client an enemy dies only when the host's has: before its update
// (FUN_01F26EE0, where HP below 0 has it die, FUN_01F26590 by a tail jump the
// hooks never see; the second class's update 0x01F5F5D0 runs it too), one
// the host has not killed keeps HP 1.
constexpr uint32_t kEnemyUpdate = 0x01F26EE0, kEnemyUpdate2 = 0x01F5F5D0;
PS2Runtime::RecompiledFunction g_enemyUpdate = nullptr, g_enemyUpdate2 = nullptr;
template <uint32_t Entry, PS2Runtime::RecompiledFunction *Original>
void enemyUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == Entry && g_clientMission)
    {
        const uint32_t enemy = getRegU32(ctx, 4);
        if (isEnemy(rdram, enemy) && rd<int32_t>(rdram, enemy + kEnemyHp) < 0)
        {
            bool may;
            {
                std::lock_guard<std::mutex> lock(g_enemyMutex);
                may = g_mayDie.count(enemy) != 0;
            }
            if (!may)
            {
                const int32_t one = 1;
                std::memcpy(rdram + enemy + kEnemyHp, &one, 4);
            }
        }
    }
    kh2::callOriginal(*Original, rdram, ctx, runtime);
}

// Once a frame, from the main ship's update (FUN_01F3C7A8, the course's step,
// is a task's loop: entered once, it yields each frame inside).
void enemiesStep(uint8_t *rdram)
{
    if (g_clientMission || g_hostMission)
    {
        std::lock_guard<std::mutex> lock(g_enemyMutex);
        if (g_hostMission)
        {
            // Our deaths (and escapes): HP below 0, or FUN_01F26590's mark
            // (+0x3BC bit 0) as it goes.
            for (const auto &[obj, id] : g_idOf)
                if (!g_reported.count(id) && isEnemy(rdram, obj) &&
                    ((rd<uint32_t>(rdram, obj + kEnemyFlags) & 1) || rd<int32_t>(rdram, obj + kEnemyHp) < 0))
                {
                    g_reported.insert(id);
                    g_killsOut.push_back(id);
                    ++g_statKills;
                }
            // The clients' hits.
            for (const auto &[id, dmg] : g_hitsIn)
            {
                const auto it = g_objOf.find(id);
                if (it == g_objOf.end() || !isEnemy(rdram, it->second))
                    continue;
                int32_t hp = rd<int32_t>(rdram, it->second + kEnemyHp);
                if (hp <= 0)
                    continue; // (dead already, or cannot be hurt)
                hp -= dmg;
                ++g_statHits;
                if (hp < 1)
                    hp = -1;
                std::memcpy(rdram + it->second + kEnemyHp, &hp, 4);
            }
            g_hitsIn.clear();
        }
        else
        {
            // The host's deaths.
            for (auto it = g_killsIn.begin(); it != g_killsIn.end();)
            {
                const auto o = g_objOf.find(*it);
                if (o == g_objOf.end())
                {
                    ++it; // (not here yet)
                    continue;
                }
                ++g_statKills;
                if (isEnemy(rdram, o->second))
                {
                    g_mayDie.insert(o->second);
                    if (rd<int32_t>(rdram, o->second + kEnemyHp) != 0)
                    {
                        const int32_t dead = -1;
                        std::memcpy(rdram + o->second + kEnemyHp, &dead, 4);
                    }
                }
                it = g_killsIn.erase(it);
            }
            // Our hits this frame, for the host.
            for (const auto &[enemy, before] : g_hitBefore)
            {
                if (!isEnemy(rdram, enemy) || before <= 0)
                    continue;
                const int32_t now = rd<int32_t>(rdram, enemy + kEnemyHp);
                const int32_t dmg = before - (now < 0 ? 0 : now);
                // (Not sent: every machine fires each pilot's Teeny, so the
                // host's own copy of our shots does the host's damage.)
                (void)dmg;
                if (now < 1 && !g_mayDie.count(enemy))
                {
                    const int32_t one = 1; // (alive until the host's dies)
                    std::memcpy(rdram + enemy + kEnemyHp, &one, 4);
                }
            }
            g_hitBefore.clear();
            const uint32_t course = kCourse;
            if (g_haveHostTime)
            {
                const float t = rdf(rdram, course + kCourseTime);
                const float d = g_hostTime - t;
                wrf(rdram, course + kCourseTime, std::fabs(d) > 30.f ? g_hostTime : t + d * 0.1f);
            }
            const uint32_t ship = rd<uint32_t>(rdram, 0x01F7DE44);
            if (g_haveHostHp && ship >= 0x00100000u && ship < 0x02000000u)
                wrf(rdram, ship + kShipStats + 4, g_hostHp);
            std::memcpy(rdram + kScore, &g_hostScore, 4);
        }
    }
}

// A client's main ship takes no damage of its own (its HP is the host's).
void shipDamageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kShipDamage && g_clientMission)
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_shipDamage, rdram, ctx, runtime);
}

void clearEnemies()
{
    std::lock_guard<std::mutex> lock(g_enemyMutex);
    g_idOf.clear();
    g_objOf.clear();
    g_madeCount = 0;
    g_killsOut.clear();
    g_killsIn.clear();
    g_hitsOut.clear();
    g_haveHostTime = g_haveHostHp = false;
    g_hostScore = 0;
    g_statHits = g_statKills = 0;
    g_hitBefore.clear();
    g_mayDie.clear();
    g_reported.clear();
    g_pendingKey = 0;
}
} // namespace

bool hostShips(const uint8_t *ram, std::vector<uint8_t> &out)
{
    const uint32_t ship = missionShip(ram);
    if (!ship)
        return false;
    static uint32_t seq = 0;
    out.clear();
    out.push_back(2);
    ++seq;
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&seq), reinterpret_cast<const uint8_t *>(&seq) + 4);
    out.insert(out.end(), ram + ship + kShipPlace, ram + ship + kShipPlace + kShipPlaceSize);
    {
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        for (int k = 0; k < 2; ++k)
        {
            const Pilot &p = g_pilots[k];
            out.push_back(p.mode == Mode::Remote && p.heard ? 1 : 0);
            for (int i = 0; i < 3; ++i)
                putF(out, p.offset[i]);
            putU16(out, p.remoteShots);
        }
    }
    putF(out, rdf(ram, kCourse + kCourseTime));
    putF(out, rdf(ram, ship + kShipStats + 4));
    const uint32_t score = rd<uint32_t>(ram, kScore);
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(&score), reinterpret_cast<const uint8_t *>(&score) + 4);
    out.push_back(rd<uint32_t>(ram, kBattleMode + 0x1C) == 2 ? 1 : 0); // under a menu
    std::lock_guard<std::mutex> lock(g_enemyMutex);
    if (seq % 300 == 0)
        std::printf("[gummi] host: %zu objects known, %u hits from clients taken, %u deaths sent, time %.1f (hits on enemies %u)\n",
                    g_idOf.size(), g_statHits, g_statKills, rdf(ram, kCourse + kCourseTime), g_dbgHitCalls.load());
    const size_t n = std::min<size_t>(g_killsOut.size(), 100);
    out.push_back(uint8_t(n));
    for (size_t i = 0; i < n; ++i)
        out.insert(out.end(), reinterpret_cast<const uint8_t *>(&g_killsOut[i]),
                   reinterpret_cast<const uint8_t *>(&g_killsOut[i]) + 4);
    g_killsOut.erase(g_killsOut.begin(), g_killsOut.begin() + long(n));
    return true;
}

void onHostShips(const std::vector<uint8_t> &d)
{
    constexpr size_t kFixed = 1 + 4 + kShipPlaceSize + 2 * 15 + 13 + 1;
    if (d.size() < kFixed || d[0] != 2 || d.size() != kFixed + size_t(d[kFixed - 1]) * 4)
        return;
    uint32_t seq;
    std::memcpy(&seq, d.data() + 1, 4);
    if (seq % 300 == 0)
    {
        std::lock_guard<std::mutex> lock(g_enemyMutex);
        std::printf("[gummi] client: %zu objects known, %u host deaths applied, %zu waiting, host time %.1f (hits on enemies %u, Teeny shots %u)\n",
                    g_idOf.size(), g_statKills, g_killsIn.size(), g_hostTime, g_dbgHitCalls.load(), g_dbgShots.load());
    }
    {
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        std::memcpy(g_hostShip, d.data() + 5, kShipPlaceSize);
        g_heardShip = true;
        const uint8_t *t = d.data() + 5 + kShipPlaceSize;
        for (int k = 0; k < 2; ++k, t += 15)
        {
            Pilot &p = g_pilots[k];
            if (p.mode != Mode::Remote || !t[0])
                continue;
            std::memcpy(p.offset, t + 1, 12);
            p.remoteShots = uint16_t(t[13] | t[14] << 8);
            if (!p.heard)
                p.shotsSeen = p.remoteShots;
            p.heard = true;
        }
    }
    const uint8_t *q = d.data() + 5 + kShipPlaceSize + 30;
    std::lock_guard<std::mutex> lock(g_enemyMutex);
    std::memcpy(&g_hostTime, q, 4);
    std::memcpy(&g_hostHp, q + 4, 4);
    std::memcpy(&g_hostScore, q + 8, 4);
    g_hostPaused = q[12] != 0;
    g_haveHostTime = g_haveHostHp = true;
    for (size_t i = 0; i < d[kFixed - 1]; ++i)
    {
        uint32_t id;
        std::memcpy(&id, d.data() + kFixed + i * 4, 4);
        g_killsIn.insert(id); // (applied in the next frame's hook, where the game's memory is ours)
    }
}

bool pilotState(std::vector<uint8_t> &out)
{
    out.clear();
    {
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        int k = 0;
        for (; k < 2; ++k)
            if (g_pilots[k].mode == Mode::Local && g_pilots[k].placed)
                break;
        if (k < 2)
        {
            const Pilot &p = g_pilots[k];
            out.push_back(uint8_t(k));
            for (int i = 0; i < 3; ++i)
                putF(out, p.offset[i]);
            putU16(out, p.shots);
        }
        else
        {
            out.push_back(0xFF);
            out.insert(out.end(), 14, 0);
        }
    }
    std::lock_guard<std::mutex> lock(g_enemyMutex);
    const size_t n = std::min<size_t>(g_hitsOut.size(), 100);
    if (out[0] == 0xFF && n == 0)
        return false;
    out.push_back(uint8_t(n));
    for (size_t i = 0; i < n; ++i)
    {
        out.insert(out.end(), reinterpret_cast<const uint8_t *>(&g_hitsOut[i].first),
                   reinterpret_cast<const uint8_t *>(&g_hitsOut[i].first) + 4);
        out.insert(out.end(), reinterpret_cast<const uint8_t *>(&g_hitsOut[i].second),
                   reinterpret_cast<const uint8_t *>(&g_hitsOut[i].second) + 4);
    }
    g_hitsOut.erase(g_hitsOut.begin(), g_hitsOut.begin() + long(n));
    return true;
}

void onPilotState(uint8_t player, const std::vector<uint8_t> &d)
{
    if (d.size() < 16 || d.size() != 16 + size_t(d[15]) * 8)
        return;
    if (d[0] <= 1 && g_pilotIds[d[0]] == player)
    {
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        Pilot &p = g_pilots[d[0]];
        if (p.mode == Mode::Remote)
        {
            std::memcpy(p.offset, d.data() + 1, 12);
            p.remoteShots = uint16_t(d[13] | d[14] << 8);
            if (!p.heard)
                p.shotsSeen = p.remoteShots;
            p.heard = true;
        }
    }
    std::lock_guard<std::mutex> lock(g_enemyMutex);
    for (size_t i = 0; i < d[15]; ++i)
    {
        uint32_t id;
        int32_t dmg;
        std::memcpy(&id, d.data() + 16 + i * 8, 4);
        std::memcpy(&dmg, d.data() + 20 + i * 8, 4);
        g_hitsIn.emplace_back(id, dmg); // (applied in the next frame's hook)
    }
}

bool takeNotice(uint8_t pilots[2])
{
    std::lock_guard<std::mutex> lock(g_netMutex);
    if (!g_notice)
        return false;
    g_notice = 0;
    pilots[0] = g_pilotIds[0];
    pilots[1] = g_pilotIds[1];
    return true;
}

bool takeFlow(std::vector<uint8_t> &out)
{
    std::lock_guard<std::mutex> lock(g_netMutex);
    if (g_flowOut.empty())
        return false;
    out.swap(g_flowOut);
    g_flowOut.clear();
    return true;
}

void onHostFlow(const std::vector<uint8_t> &d)
{
    if (d.size() == 2 && g_clientMission && (d[0] == 1 || d[0] == 2))
        followFlow(d[0], d[1]);
}

void hostTick(const uint8_t *ram)
{
    static bool was = false, led = false;
    static uint8_t choice = 0;
    const uint32_t b = rd<uint32_t>(ram, kBattleMode + 0x1C);
    const bool in = b == 1 || b == 2;
    if (in)
    {
        choice = pauseChoice(ram);
        led = g_hostMission;
    }
    else if (was && led && (choice == 2 || choice == 3))
    {
        std::lock_guard<std::mutex> lock(g_netMutex);
        g_flowOut = {2, choice};
        std::printf("[gummi] the host leaves the mission (%s)\n", choice == 2 ? "Gummi menu" : "World Map");
        std::fflush(stdout);
    }
    if (!in)
        choice = 0;
    was = in;
}

void clientTick(uint8_t *ram)
{
    setFrozen(ram, g_clientMission && g_hostPaused.load() && inGummi(ram));
}

void endMission(uint8_t *ram)
{
    if (ram)
        setFrozen(ram, false);
    g_hostPaused = false;
    if (!g_savedGummi.empty() && ram)
    {
        std::memcpy(ram + kGummiSave, g_savedGummi.data(), g_savedGummi.size());
        std::printf("[gummi] the mission's rewards are the host's: our Gummi save data is as it was\n");
        g_savedGummi.clear();
    }
    g_clientMission = g_hostMission = false;
    clearEnemies();
    std::lock_guard<std::mutex> lock(g_pilotMutex);
    for (Pilot &p : g_pilots)
        if (p.mode != Mode::Test)
            p = Pilot{};
    g_followShip = false;
    g_heardShip = false;
}

bool install(PS2Runtime &runtime)
{
    g_battleInit = runtime.lookupFunction(kBattleInit);
    if (!g_battleInit || !runtime.replaceFunction(kBattleInit, &battleInitHook))
        std::fprintf(stderr, "[gummi] could not hook the Gummi battle mode's start\n");
    g_taskYield = runtime.lookupFunction(kTaskYield);
    if (!g_taskYield || !runtime.replaceFunction(kTaskYield, &taskYieldHook))
        std::fprintf(stderr, "[gummi] could not hook the task yield\n");
    g_shipUpdate = hookOverlay("gumibattle", kShipUpdate, &shipUpdateHook);
    g_apply[0] = hookOverlay("gumibattle", kApplyTeeny0, &applyHook<0>);
    g_apply[1] = hookOverlay("gumibattle", kApplyTeeny1, &applyHook<1>);
    g_teenyFire = hookOverlay("gumibattle", kTeenyFire, &teenyFireHook);
    g_countSpawned = hookOverlay("gumibattle", kCountSpawned, &countSpawnedHook);
    g_spawnUnit = hookOverlay("gumibattle", kSpawnUnit, &spawnUnitHook);
    g_attachPart = hookOverlay("gumibattle", kAttachPart, &attachPartHook);
    g_enemyHit = hookOverlay("gumibattle", kEnemyHit, &enemyHitHook<kEnemyHit, &g_enemyHit>);
    g_enemyDamage = hookOverlay("gumibattle", kEnemyDamage, &enemyHitHook<kEnemyDamage, &g_enemyDamage>);
    g_enemyUpdate = hookOverlay("gumibattle", kEnemyUpdate, &enemyUpdateHook<kEnemyUpdate, &g_enemyUpdate>);
    g_enemyUpdate2 = hookOverlay("gumibattle", kEnemyUpdate2, &enemyUpdateHook<kEnemyUpdate2, &g_enemyUpdate2>);
    g_shipDamage = hookOverlay("gumibattle", kShipDamage, &shipDamageHook);
    g_pauseOpen = hookOverlay("gumibattle", kPauseOpen, &pauseOpenHook);
    g_pauseDone = hookOverlay("gumibattle", kPauseDone, &menuDoneHook<kPauseDone, &g_pauseDone>);
    g_gameOverDone = hookOverlay("gumibattle", kGameOverDone, &menuDoneHook<kGameOverDone, &g_gameOverDone>);
    g_resultsDone = hookOverlay("gumibattle", kResultsDone, &menuDoneHook<kResultsDone, &g_resultsDone>);
    if (!g_pauseOpen || !g_pauseDone || !g_gameOverDone || !g_resultsDone)
        std::fprintf(stderr, "[gummi] could not hook the mission's menus\n");
    if (!g_countSpawned || !g_spawnUnit || !g_attachPart || !g_enemyHit || !g_enemyDamage || !g_enemyUpdate || !g_enemyUpdate2 ||
        !g_shipDamage)
        std::fprintf(stderr, "[gummi] could not hook the mission's enemies\n");
    const bool ok = g_shipUpdate && g_apply[0] && g_apply[1] && g_teenyFire;
    if (!ok)
        std::fprintf(stderr, "[gummi] could not hook the Gummi battle overlay\n");
    return ok;
}

void setPilot(int k, bool on)
{
    if (k < 0 || k > 1)
        return;
    std::lock_guard<std::mutex> lock(g_pilotMutex);
    g_pilots[k] = Pilot{};
    g_pilots[k].mode = on ? Mode::Local : Mode::Off;
}

void setPilotInput(int k, const PilotInput &in)
{
    if (k < 0 || k > 1)
        return;
    std::lock_guard<std::mutex> lock(g_pilotMutex);
    g_pilots[k].in = in;
}

void testPilot(int k, bool on)
{
    if (k < 0 || k > 1)
        return;
    std::lock_guard<std::mutex> lock(g_pilotMutex);
    g_pilots[k] = Pilot{};
    g_pilots[k].mode = on ? Mode::Test : Mode::Off;
}

void listPresets()
{
    if (!planInfo())
        return;
    const uint8_t *list = g_plans.data(1);
    const uint32_t n = get<uint32_t>(list);
    std::string s = "[gummi] presets:";
    char buf[80];
    for (uint32_t i = 0; i < n && 4 + (i + 1) * 0x20 <= g_plans.entries[1].size; ++i)
    {
        const uint8_t *r = list + 4 + i * 0x20;
        std::snprintf(buf, sizeof(buf), " %u(k%d %d/%d/%d i%X)", i, get<int16_t>(r), get<int16_t>(r + 2),
                      get<int16_t>(r + 4), get<int16_t>(r + 6), unsigned(get<uint16_t>(r + 0xC)));
        s += buf;
    }
    std::printf("%s\n", s.c_str());
    std::fflush(stdout);
}

void testStartPreset(int route, int level, bool ex, int mainPreset, int teeny0, int teeny1)
{
    if (!planInfo())
        return;
    const std::vector<uint8_t> ships[3] = {planById(presetPlan(mainPreset, 0)), planById(presetPlan(teeny0, 1)),
                                           planById(presetPlan(teeny1, 2))};
    kh2guest::post([=](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        uint8_t *ram = env.rdram;
        const uint32_t to[3] = {kMainBuf, kTeenyBuf, kTeenyBuf + kTeenyBufSize};
        const uint32_t cap[3] = {0x800, kTeenyBufSize, kTeenyBufSize};
        for (int k = 0; k < 3; ++k)
        {
            ram[kFlags + k] = 0;
            if (ships[k].empty() || ships[k].size() > cap[k])
                continue;
            std::memcpy(ram + to[k], ships[k].data(), ships[k].size());
            ram[kFlags + k] = usable(ram, to[k]) ? 1 : 0;
        }
        return launch(ram, route, level, ex);
    });
}

std::string garage(const uint8_t *ram)
{
    std::string s = "garage valid " + std::to_string(ram[kGarage]) + " main:";
    char buf[48];
    for (int i = 0; i < kSlots; ++i)
    {
        const uint32_t p = kMainPlans + i * kMainPlanSize;
        if (usable(ram, p))
        {
            std::snprintf(buf, sizeof(buf), " %d(%d+%d,ab%d)", i, rd<int16_t>(ram, p + 8), rd<int16_t>(ram, p + 0xA),
                          int(rd<int8_t>(ram, p + 0xC)));
            s += buf;
        }
    }
    s += " teeny:";
    for (int i = 0; i < kSlots; ++i)
    {
        const uint32_t p = kTeenyPlans + i * kTeenyPlanSize;
        if (usable(ram, p))
        {
            std::snprintf(buf, sizeof(buf), " %d(%d+%d)", i, rd<int16_t>(ram, p + 8), rd<int16_t>(ram, p + 0xA));
            s += buf;
        }
    }
    std::snprintf(buf, sizeof(buf), " | teeny system %u", unsigned(ram[0x0033C990u + 0xAC]));
    return s + buf;
}

void testStart(int route, int level, bool ex, int mainSlot, int teeny0, int teeny1)
{
    kh2guest::post([=](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        uint8_t *ram = env.rdram;
        const auto load = [&](int slot, uint32_t plans, uint32_t size, uint32_t to, int flag) {
            ram[kFlags + flag] = 0;
            if (slot < 0 || slot >= kSlots)
                return;
            const uint32_t p = plans + uint32_t(slot) * size;
            if (!usable(ram, p))
            {
                std::printf("[gummi] blueprint %d for ship %d is empty\n", slot, flag);
                return;
            }
            std::memcpy(ram + to, ram + p, size);
            ram[kFlags + flag] = 1;
        };
        load(mainSlot, kMainPlans, kMainPlanSize, kMainBuf, 0);
        load(teeny0, kTeenyPlans, kTeenyPlanSize, kTeenyBuf, 1);
        load(teeny1, kTeenyPlans, kTeenyPlanSize, kTeenyBuf + kTeenyBufSize, 2);
        return launch(ram, route, level, ex);
    });
}
} // namespace kh2gummi
