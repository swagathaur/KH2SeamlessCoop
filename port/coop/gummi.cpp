#include "gummi.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "game_state.h"
#include "cd_iso.h"
#include "guest_call.h"

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
} // namespace

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
