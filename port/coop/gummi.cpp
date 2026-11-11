#include "gummi.h"

#include <atomic>
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

PS2Runtime::RecompiledFunction g_shipUpdate = nullptr, g_apply[2] = {}, g_teenyFire = nullptr,
                               g_teenyLockOn = nullptr, g_teenyMissile = nullptr;

struct Pilot
{
    bool on = false, test = false;
    PilotInput in;
    bool wasFire = false;
    bool placed = false;  // offset taken from where the formation had it
    float offset[3] = {}; // the Teeny's place relative to the main ship
    int cooldown = 0;
};
std::mutex g_pilotMutex;
Pilot g_pilots[2];

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

void shipUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kShipUpdate)
    {
        kh2guest::runPending(rdram, ctx, runtime, kShipUpdate); // may not return
        const uint32_t ship = getRegU32(ctx, 4);
        static uint64_t frame = 0;
        ++frame;
        std::lock_guard<std::mutex> lock(g_pilotMutex);
        for (int k = 0; k < 2; ++k)
        {
            Pilot &p = g_pilots[k];
            if (!p.on)
                continue;
            if (p.test)
                testScript(p, frame, k);
            const uint32_t teeny = rd<uint32_t>(rdram, ship + kShipTeenies + 4 * k);
            const bool press = p.in.fire && !p.wasFire;
            p.wasFire = p.in.fire;
            if (p.cooldown > 0)
                --p.cooldown;
            if (teeny && (press || (p.in.fire && p.cooldown == 0)))
            {
                p.cooldown = 8;
                kh2guest::postCall(kh2guest::Call{kBodyFire, {teeny + kTeenyBody}, {}});
            }
        }
    }
    kh2::callOriginal(g_shipUpdate, rdram, ctx, runtime);
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
        if (p.on)
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
            std::printf("[gummi] Teeny %d target %.1f %.1f %.1f current %.1f %.1f %.1f%s\n", K,
                        rdf(rdram, target), rdf(rdram, target + 4), rdf(rdram, target + 8), rdf(rdram, current),
                        rdf(rdram, current + 4), rdf(rdram, current + 8), p.on ? " (piloted)" : "");
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
        if (g_pilots[k].on && rd<uint32_t>(rdram, ship + kShipTeenies + 4 * k) == teeny)
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
void teenyLockOnHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kTeenyLockOn && piloted(rdram, getRegU32(ctx, 4)))
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_teenyLockOn, rdram, ctx, runtime);
}
void teenyMissileHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kTeenyMissile && piloted(rdram, getRegU32(ctx, 4)))
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_teenyMissile, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &)
{
    g_shipUpdate = hookOverlay("gumibattle", kShipUpdate, &shipUpdateHook);
    g_apply[0] = hookOverlay("gumibattle", kApplyTeeny0, &applyHook<0>);
    g_apply[1] = hookOverlay("gumibattle", kApplyTeeny1, &applyHook<1>);
    g_teenyFire = hookOverlay("gumibattle", kTeenyFire, &teenyFireHook);
    g_teenyLockOn = hookOverlay("gumibattle", kTeenyLockOn, &teenyLockOnHook);
    g_teenyMissile = hookOverlay("gumibattle", kTeenyMissile, &teenyMissileHook);
    const bool ok = g_shipUpdate && g_apply[0] && g_apply[1] && g_teenyFire && g_teenyLockOn && g_teenyMissile;
    if (!ok)
        std::fprintf(stderr, "[gummi] could not hook the Gummi battle overlay\n");
    return ok;
}

void setPilot(int k, bool on)
{
    if (k < 0 || k > 1)
        return;
    std::lock_guard<std::mutex> lock(g_pilotMutex);
    Pilot &p = g_pilots[k];
    if (p.on != on)
        p = Pilot{};
    p.on = on;
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
    setPilot(k, on);
    std::lock_guard<std::mutex> lock(g_pilotMutex);
    if (k >= 0 && k <= 1)
        g_pilots[k].test = on;
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
