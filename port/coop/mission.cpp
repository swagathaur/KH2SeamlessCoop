#include "mission.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2mission
{
namespace
{
constexpr uint32_t kMissionPtr = 0x01D49320; // YS::MISSION::GetCurrent()
constexpr uint32_t kTimerFrames = 0x00349DF8, kTimerCountDown = 0x00349E00; // dk::Timer
constexpr uint32_t kTimerOn = 0xC8;
constexpr uint32_t kCounts = 3, kCountOn = 0xF4, kCountValue = 0x114, kCountStride = 0x38;
constexpr uint32_t kGauges = 4, kGaugeOn = 0x19C, kGaugeMax = 0x1A8, kGaugeNow = 0x1BC, kGaugeStride = 0x34;
constexpr uint32_t kCombos = 2, kComboOn = 0x26C, kComboValue = 0x298, kComboStride = 0x40;
constexpr uint32_t kTrapFailed = 0x001A0018; // YS::trap_mission_failed(BD_VALUE*) -> MISSION::Failed()
constexpr uint32_t kFailed = 0x001BB768;     // YS::MISSION::Failed()
constexpr uint8_t kVersion = 1;

PS2Runtime::RecompiledFunction g_trapFailed = nullptr;
std::atomic<bool> g_following{false};
std::atomic<bool> g_failed{false};    // host: ours failed (to send)
std::atomic<bool> g_hostFailed{false}; // client: the host's did (ours may fail now)
std::atomic<int> g_blockedLogs{0};

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }

void trapFailedHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_following.load() && !g_hostFailed.load())
    {
        // Ours does not fail on its own: the host's game decides.
        if (g_blockedLogs.fetch_add(1) < 4)
        {
            std::printf("[mission] our mission would fail: waiting for the host's\n");
            std::fflush(stdout);
        }
        setReturnU32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    if (!g_following.load())
    {
        std::printf("[mission] the mission failed\n");
        std::fflush(stdout);
        g_failed = true;
    }
    kh2::callOriginal(g_trapFailed, rdram, ctx, runtime);
}

void put(std::vector<uint8_t> &out, const void *p, size_t n)
{
    const uint8_t *b = static_cast<const uint8_t *>(p);
    out.insert(out.end(), b, b + n);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_trapFailed = runtime.lookupFunction(kTrapFailed);
    if (!g_trapFailed || !runtime.replaceFunction(kTrapFailed, &trapFailedHook))
    {
        std::fprintf(stderr, "[mission] could not hook trap_mission_failed\n");
        g_trapFailed = nullptr;
        return false;
    }
    return true;
}

void setFollowing(bool on)
{
    if (g_following.exchange(on) != on && !on)
        g_hostFailed = false;
}

bool takeFailed() { return g_failed.exchange(false); }

// world u8, room u8, battle u16, version u8, timer on u8 + countdown u8 +
// frames i32, then per count: on u8 + i32; per gauge: on u8 + max i32 + now
// f32; per combo: on u8 + i32.
std::vector<uint8_t> snapshot(const uint8_t *rdram, uint8_t world, uint8_t room, uint16_t battle)
{
    std::vector<uint8_t> out;
    const uint32_t m = rd<uint32_t>(rdram, kMissionPtr);
    if (!inRam(m))
        return out;
    out.push_back(world);
    out.push_back(room);
    put(out, &battle, 2);
    out.push_back(kVersion);
    out.push_back(uint8_t(rd<uint32_t>(rdram, m + kTimerOn) & 1));
    out.push_back(rd<uint8_t>(rdram, kTimerCountDown));
    const int32_t frames = rd<int32_t>(rdram, kTimerFrames);
    put(out, &frames, 4);
    for (uint32_t i = 0; i < kCounts; ++i)
    {
        out.push_back(uint8_t(rd<uint32_t>(rdram, m + kCountOn + i * kCountStride) & 1));
        const int32_t v = rd<int32_t>(rdram, m + kCountValue + i * kCountStride);
        put(out, &v, 4);
    }
    for (uint32_t i = 0; i < kGauges; ++i)
    {
        out.push_back(uint8_t(rd<uint32_t>(rdram, m + kGaugeOn + i * kGaugeStride) & 1));
        const int32_t max = rd<int32_t>(rdram, m + kGaugeMax + i * kGaugeStride);
        const float now = rd<float>(rdram, m + kGaugeNow + i * kGaugeStride);
        put(out, &max, 4);
        put(out, &now, 4);
    }
    for (uint32_t i = 0; i < kCombos; ++i)
    {
        out.push_back(uint8_t(rd<uint32_t>(rdram, m + kComboOn + i * kComboStride) & 1));
        const int32_t v = rd<int32_t>(rdram, m + kComboValue + i * kComboStride);
        put(out, &v, 4);
    }
    return out;
}

void onHostState(uint8_t *rdram, const std::vector<uint8_t> &data, uint8_t world, uint8_t room, uint16_t battle)
{
    const size_t want = 5 + 6 + kCounts * 5 + kGauges * 9 + kCombos * 5;
    if (data.size() != want || data[4] != kVersion)
        return;
    uint16_t hb;
    std::memcpy(&hb, data.data() + 2, 2);
    if (data[0] != world || data[1] != room || hb != battle)
        return; // not the same fight
    const uint32_t m = rd<uint32_t>(rdram, kMissionPtr);
    if (!inRam(m))
        return;
    size_t p = 5;
    const bool timerOn = data[p] != 0, countDown = data[p + 1] != 0;
    int32_t frames;
    std::memcpy(&frames, data.data() + p + 2, 4);
    p += 6;
    if (timerOn && (rd<uint32_t>(rdram, m + kTimerOn) & 1) && countDown == (rd<uint8_t>(rdram, kTimerCountDown) != 0))
        wr<int32_t>(rdram, kTimerFrames, frames);
    for (uint32_t i = 0; i < kCounts; ++i, p += 5)
    {
        int32_t v;
        std::memcpy(&v, data.data() + p + 1, 4);
        if (data[p] && (rd<uint32_t>(rdram, m + kCountOn + i * kCountStride) & 1))
            wr<int32_t>(rdram, m + kCountValue + i * kCountStride, v);
    }
    for (uint32_t i = 0; i < kGauges; ++i, p += 9)
    {
        int32_t max;
        float now;
        std::memcpy(&max, data.data() + p + 1, 4);
        std::memcpy(&now, data.data() + p + 5, 4);
        if (!data[p] || !(rd<uint32_t>(rdram, m + kGaugeOn + i * kGaugeStride) & 1) || max <= 0)
            continue;
        const int32_t ours = rd<int32_t>(rdram, m + kGaugeMax + i * kGaugeStride);
        wr<float>(rdram, m + kGaugeNow + i * kGaugeStride, ours > 0 ? now * float(ours) / float(max) : now);
    }
    for (uint32_t i = 0; i < kCombos; ++i, p += 5)
    {
        int32_t v;
        std::memcpy(&v, data.data() + p + 1, 4);
        if (data[p] && (rd<uint32_t>(rdram, m + kComboOn + i * kComboStride) & 1))
            wr<int32_t>(rdram, m + kComboValue + i * kComboStride, v);
    }
}

void onHostFailed(uint8_t *rdram)
{
    std::printf("[mission] the host's mission failed: ours fails with it\n");
    std::fflush(stdout);
    g_hostFailed = true;
    if (!inRam(rd<uint32_t>(rdram, kMissionPtr)))
        return;
    kh2guest::post([](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
        {
            g_hostFailed = false; // (ours has failed now; the next one is the host's to fail again)
            return std::nullopt;
        }
        if (!inRam(rd<uint32_t>(env.rdram, kMissionPtr)))
        {
            g_hostFailed = false;
            return std::nullopt;
        }
        return kh2guest::Call{kFailed, {}, {}};
    });
}
} // namespace kh2mission
