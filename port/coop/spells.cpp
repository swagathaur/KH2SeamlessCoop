#include "spells.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "game_state.h"
#include "partners.h"
#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2spells
{
namespace
{
constexpr uint32_t kCast = 0x001A6178; // (caster, command id, combo)
PS2Runtime::RecompiledFunction g_cast = nullptr;
std::atomic<int> g_logged{0};
std::atomic<bool> g_capture{false};
std::mutex g_mutex;
std::vector<Cast> g_casts;

void castHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kCast && g_logged.fetch_add(1) < 200)
    {
        uint32_t player = 0;
        std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
        const uint32_t caster = getRegU32(ctx, 4);
        std::printf("[spells] cast: caster %08X%s command %u combo %u (from %08X)\n", caster,
                    caster == player ? " (the player)" : "", getRegU32(ctx, 5), getRegU32(ctx, 6) & 0xFF,
                    getRegU32(ctx, 31));
        std::fflush(stdout);
    }
    if (ctx->pc == kCast && g_capture.load())
    {
        uint32_t player = 0;
        std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
        if (player && getRegU32(ctx, 4) == player)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_casts.size() < 16)
                g_casts.push_back(Cast{uint16_t(getRegU32(ctx, 5)), uint8_t(getRegU32(ctx, 6))});
        }
    }
    kh2::callOriginal(g_cast, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_cast = runtime.lookupFunction(kCast);
    if (!g_cast || !runtime.replaceFunction(kCast, &castHook))
    {
        g_cast = nullptr;
        return false;
    }
    return true;
}

void setCapture(bool on)
{
    if (g_capture.exchange(on) && !on)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_casts.clear();
    }
}

std::vector<Cast> takeCasts()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Cast> out;
    out.swap(g_casts);
    return out;
}

void castAs(uint32_t standIn, const Cast &c)
{
    const uint32_t command = c.command, combo = c.combo;
    kh2guest::post([standIn, command, combo](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        bool live = false;
        for (const kh2state::ActorInfo &a : kh2state::actors(env.rdram))
            live = live || a.address == standIn;
        if (!live)
            return std::nullopt;
        std::printf("[spells] stand-in %08X casts command %u (for its player)\n", standIn, command);
        std::fflush(stdout);
        return kh2guest::Call{kCast, {standIn, command, combo}, {}};
    });
}

std::vector<uint8_t> encode(const Cast &c, uint8_t world, uint8_t room)
{
    return {world, room, uint8_t(c.command & 0xFF), uint8_t(c.command >> 8), c.combo};
}

bool decode(const std::vector<uint8_t> &data, Cast &c, uint8_t &world, uint8_t &room)
{
    if (data.size() != 5)
        return false;
    world = data[0];
    room = data[1];
    c.command = uint16_t(data[2] | data[3] << 8);
    c.combo = data[4];
    return true;
}

void testTick(uint8_t *rdram, uint64_t frame)
{
    static const char *spec = std::getenv("KH2_TEST_CAST");
    if (!spec || !*spec)
        return;
    static uint64_t at = std::strtoull(spec, nullptr, 10);
    static uint32_t command = [] {
        const char *c = std::strchr(spec, ':');
        return c ? uint32_t(std::strtoul(c + 1, nullptr, 10)) : 0u;
    }();
    static bool done = false;
    if (done || frame < at)
        return;
    done = true;
    // A remote player's stand-in if there is one, else the first party member
    // (KH2_TEST_CAST_TARGET=player2: a second player object, not ours).
    uint32_t member = 0;
    if (const char *t = std::getenv("KH2_TEST_CAST_TARGET"); t && std::strcmp(t, "player2") == 0)
    {
        uint32_t player = 0;
        std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
        for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        {
            if (a.type == 0 && a.address != player && !member)
                member = a.address;
            if (a.type == 0)
            {
                uint32_t sheet = 0;
                std::memcpy(&sheet, rdram + a.address + 0x48C, 4);
                std::printf("[spells] test: player-class object %08X%s, sheet %08X\n", a.address,
                            a.address == player ? " (ours)" : "", sheet);
            }
        }
    }
    for (const kh2partner::Link &l : kh2partner::links())
        if (l.object && !member)
            member = l.object;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.type == 1 && !member)
            member = a.address;
    const uint32_t cmd = command;
    std::printf("[spells] test: party member %08X casts command %u\n", member, cmd);
    std::fflush(stdout);
    if (!member)
        return;
    kh2guest::post([member, cmd](const kh2guest::Env &, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
        {
            std::printf("[spells] test: cast done\n");
            std::fflush(stdout);
            return std::nullopt;
        }
        // KH2_TEST_CAST_VIA=command: through the player's own magic command
        // (FUN_0017A590(player, command)), not the cast itself.
        if (const char *v = std::getenv("KH2_TEST_CAST_VIA"); v && std::strcmp(v, "command") == 0)
            return kh2guest::Call{0x0017A590u, {member, cmd}, {}};
        return kh2guest::Call{kCast, {member, cmd, 0u}, {}};
    });
}
} // namespace kh2spells
