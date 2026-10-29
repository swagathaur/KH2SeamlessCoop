#include "spells.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "game_state.h"
#include "partners.h"
#include "avatars.h"
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

// Spells the others know (net, SpellsKnown): each game loads their data too.
// FUN_001A6658(slot) picks a spell slot's record (Fire, Blizzard, Thunder,
// Cure, Magnet, Reflect) by what this save has learned (0: not learned);
// MAGIC::Read loads the files of those, and the room's magic table
// (0x00350800, 0x2C a slot, +0x28 the record, record +0x26 its command) is
// what a cast looks the command up in. A slot only the others know gets its
// first record (the data is there for their avatars' casts), and is hidden
// from this player's own menu (YS::MAGIC::GetCommand gives 0 for it).
constexpr uint32_t kSpellRecord = 0x001A6658, kGetCommand = 0x001A67B0;
constexpr uint32_t kMagicTable = 0x00350800, kMagicSlotSize = 0x2C, kMagicSlots = 6;
constexpr uint32_t kMagicData = 0x01C6CB28; // -> u32 ?, u32 count, records of 0x38 (byte 0: slot)
PS2Runtime::RecompiledFunction g_spellRecord = nullptr, g_getCommand = nullptr;
std::atomic<uint8_t> g_localLevels[kMagicSlots] = {};
std::atomic<uint8_t> g_othersKnow[kMagicSlots] = {};
std::atomic<bool> g_borrowed[kMagicSlots] = {};
std::atomic<bool> g_share{false};

uint32_t firstRecord(const uint8_t *rdram, uint32_t slot)
{
    uint32_t data = 0;
    std::memcpy(&data, rdram + kMagicData, 4);
    if (data < 0x00100000u || data >= 0x02000000u)
        return 0;
    uint32_t count = 0;
    std::memcpy(&count, rdram + data + 4, 4);
    for (uint32_t i = 0; i < count && i < 256; ++i)
    {
        const uint32_t rec = data + 8 + i * 0x38;
        if (rdram[rec] == slot)
            return rec;
    }
    return 0;
}

void spellRecordHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t slot = getRegU32(ctx, 4);
    kh2::callOriginal(g_spellRecord, rdram, ctx, runtime);
    if (slot >= kMagicSlots)
        return;
    const uint32_t rec = getRegU32(ctx, 2);
    g_localLevels[slot] = rec ? uint8_t(rdram[rec + 1] + 1) : 0;
    bool borrowed = false;
    if (!rec && g_share.load() && g_othersKnow[slot].load())
        if (const uint32_t first = firstRecord(rdram, slot))
        {
            setReturnU32(ctx, first);
            borrowed = true;
        }
    if (borrowed != g_borrowed[slot].load())
    {
        g_borrowed[slot] = borrowed;
        std::printf("[spells] spell slot %u: %s\n", slot,
                    borrowed ? "only the others know it; its data is loaded for their casts" : "ours");
        std::fflush(stdout);
    }
}

void getCommandHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t slot = getRegU32(ctx, 4);
    if (ctx->pc == kGetCommand && slot < kMagicSlots && g_borrowed[slot].load())
    {
        setReturnU32(ctx, 0); // not ours to cast
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_getCommand, rdram, ctx, runtime);
}

// A command the room's magic table has, for `command`'s element (theirs can
// be Fira where ours is Fire).
uint32_t localCommand(const uint8_t *rdram, uint32_t command)
{
    uint32_t slotOf = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < kMagicSlots; ++i)
    {
        uint32_t rec = 0;
        std::memcpy(&rec, rdram + kMagicTable + i * kMagicSlotSize + 0x28, 4);
        if (rec >= 0x00100000u && rec < 0x02000000u)
        {
            uint16_t c = 0;
            std::memcpy(&c, rdram + rec + 0x26, 2);
            if (c == command)
                return command;
        }
    }
    uint32_t data = 0;
    std::memcpy(&data, rdram + kMagicData, 4);
    if (data >= 0x00100000u && data < 0x02000000u)
    {
        uint32_t count = 0;
        std::memcpy(&count, rdram + data + 4, 4);
        for (uint32_t i = 0; i < count && i < 256 && slotOf == 0xFFFFFFFFu; ++i)
        {
            const uint32_t rec = data + 8 + i * 0x38;
            uint16_t c = 0;
            std::memcpy(&c, rdram + rec + 0x26, 2);
            if (c == command)
                slotOf = rdram[rec];
        }
    }
    if (slotOf < kMagicSlots)
    {
        uint32_t rec = 0;
        std::memcpy(&rec, rdram + kMagicTable + slotOf * kMagicSlotSize + 0x28, 4);
        if (rec >= 0x00100000u && rec < 0x02000000u)
        {
            uint16_t c = 0;
            std::memcpy(&c, rdram + rec + 0x26, 2);
            return c;
        }
    }
    return command;
}
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

// Debugging: KH2_DEBUG_HOOK=ADDR (hex) logs the first calls of that guest
// function: its arguments, the caller and what $a0 points at.
PS2Runtime::RecompiledFunction g_debugOriginal = nullptr;
uint32_t g_debugAddr = 0;
std::atomic<int> g_debugCalls{0};
void debugHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const int skip = [] {
        const char *e = std::getenv("KH2_DEBUG_HOOK_SKIP");
        return e ? std::atoi(e) : 0;
    }();
    const int n = g_debugCalls.fetch_add(1);
    // Each (a0, caller) once.
    static std::mutex m;
    static std::vector<std::pair<uint32_t, uint32_t>> seen;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(m);
        const auto key = std::make_pair(getRegU32(ctx, 4), getRegU32(ctx, 31));
        bool have = false;
        for (const auto &k : seen)
            have = have || k == key;
        if (!have && seen.size() < 40)
        {
            seen.push_back(key);
            fresh = true;
        }
    }
    if (n >= skip && fresh && ctx->pc == g_debugAddr)
    {
        uint32_t player = 0;
        std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
        std::printf("[debug-hook] %08X(a0 %08X, a1 %08X, a2 %08X) from %08X; player %08X\n", g_debugAddr,
                    getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 31), player);
        std::fflush(stdout);
    }
    kh2::callOriginal(g_debugOriginal, rdram, ctx, runtime);
}

bool install(PS2Runtime &runtime)
{
    g_spellRecord = runtime.lookupFunction(kSpellRecord);
    if (!g_spellRecord || !runtime.replaceFunction(kSpellRecord, &spellRecordHook))
        g_spellRecord = nullptr;
    g_getCommand = runtime.lookupFunction(kGetCommand);
    if (!g_getCommand || !runtime.replaceFunction(kGetCommand, &getCommandHook))
        g_getCommand = nullptr;
    if (const char *e = std::getenv("KH2_TEST_OTHERS_KNOW")) // e.g. 100000: the others know Fire
    {
        std::vector<uint8_t> levels(kMagicSlots, 0);
        for (uint32_t i = 0; i < kMagicSlots && e[i]; ++i)
            levels[i] = uint8_t(e[i] - '0');
        setOthersKnow(levels, true);
    }
    if (const char *e = std::getenv("KH2_DEBUG_HOOK"))
    {
        g_debugAddr = uint32_t(std::strtoul(e, nullptr, 16));
        g_debugOriginal = runtime.lookupFunction(g_debugAddr);
        if (!g_debugOriginal || !runtime.replaceFunction(g_debugAddr, &debugHook))
            g_debugOriginal = nullptr;
    }
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

void castAsPlayer(uint32_t avatar, const Cast &c)
{
    const uint32_t command = c.command;
    kh2guest::post([avatar, command](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
        if (!first)
            return std::nullopt;
        bool live = false;
        for (const kh2state::ActorInfo &a : kh2state::actors(env.rdram))
            live = live || a.address == avatar;
        if (!live)
            return std::nullopt;
        kh2avatars::noteCast(avatar);
        const uint32_t ours = localCommand(env.rdram, command);
        std::printf("[spells] avatar %08X casts command %u (for its player%s)\n", avatar, ours,
                    ours != command ? ", as this room's level of it" : "");
        std::fflush(stdout);
        return kh2guest::Call{0x0017A590u, {avatar, ours}, {}};
    });
}

std::vector<uint8_t> knownSpells()
{
    std::vector<uint8_t> out(kMagicSlots);
    for (uint32_t i = 0; i < kMagicSlots; ++i)
        out[i] = g_localLevels[i].load();
    return out;
}

void setOthersKnow(const std::vector<uint8_t> &levels, bool share)
{
    g_share = share;
    for (uint32_t i = 0; i < kMagicSlots; ++i)
        g_othersKnow[i] = i < levels.size() ? levels[i] : 0;
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

// Prototype (KH2_TEST_P2=1): a second player object (--spawn-test=F:54)
// that ignores our controller (no action, +0x10C 0x200000) and has an HP/MP
// sheet of its own (a copy of ours at 0x01E4E000; +0x48C points there).
void testSecondPlayer(uint8_t *rdram, uint64_t frame)
{
    static const bool on = [] {
        const char *e = std::getenv("KH2_TEST_P2");
        return e && *e == '1';
    }();
    if (!on)
        return;
    constexpr uint32_t kSheet = 0x01E4E000, kSheetSize = 0x268;
    static uint32_t prepared = 0;
    uint32_t player = 0;
    std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
    // Only once our player is there, and only an object made after it.
    static uint32_t ourPlayer = 0;
    if (!player || frame < 2900)
        return;
    if (!ourPlayer)
        ourPlayer = player;
    uint32_t p2 = 0;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.type == 0 && a.address != ourPlayer && !p2)
            p2 = a.address;
    auto rd32 = [&](uint32_t a) {
        uint32_t v;
        std::memcpy(&v, rdram + a, 4);
        return v;
    };
    if (!p2)
        return;
    {
        // The game's own pointers to "the player" (controls, camera,
        // targeting, ...): the second player's first updates point them at
        // it, and ours stopped answering the controller. Ours again, every
        // frame.
        static const uint32_t kPlayerRefs[] = {0x00348638, 0x00348754, 0x0034ED04, 0x00352400,
                                               0x01C5FF10, 0x01C60018, 0x01C60024, 0x01C6C9AC,
                                               kh2state::addr::PlayerPtr};
        static int restoredTotal = 0;
        for (uint32_t a : kPlayerRefs)
            if (rd32(a) == p2)
            {
                std::memcpy(rdram + a, &ourPlayer, 4);
                if (restoredTotal++ < 40)
                    std::printf("[p2] f%llu %08X pointed at the new player: ours again\n", (unsigned long long)frame, a);
            }
    }
    if (prepared != p2)
    {
        prepared = p2;
        const uint32_t flags = rd32(p2 + 0x10C) | 0x200000u;
        std::memcpy(rdram + p2 + 0x10C, &flags, 4);
        const uint32_t sheet = rd32(p2 + 0x48C);
        if (sheet >= 0x00100000u && sheet < 0x02000000u - kSheetSize)
        {
            std::memcpy(rdram + kSheet, rdram + sheet, kSheetSize);
            std::memcpy(rdram + p2 + 0x48C, &kSheet, 4);
        }
        std::printf("[p2] second player %08X: no action, its own sheet at %08X (was %08X)\n", p2, kSheet, sheet);
        std::fflush(stdout);
    }
    if (frame % 30 == 0)
    {
        float a[3], b[3];
        std::memcpy(a, rdram + player + kh2state::addr::ObjPos, 12);
        std::memcpy(b, rdram + p2 + kh2state::addr::ObjPos, 12);
        const uint32_t ours = rd32(player + 0x48C), theirs = rd32(p2 + 0x48C);
        std::printf("[p2] f%llu ours %.0f,%.0f,%.0f hp %d mp %d | p2 %.0f,%.0f,%.0f hp %d mp %d motion %u flags %08X\n",
                    (unsigned long long)frame, a[0], a[1], a[2], int32_t(rd32(ours)), int32_t(rd32(ours + 0x180)), b[0],
                    b[1], b[2], int32_t(rd32(theirs)), int32_t(rd32(theirs + 0x180)), rd32(p2 + 0x154), rd32(p2 + 0x10C));
        std::fflush(stdout);
    }
}

void testTick(uint8_t *rdram, uint64_t frame)
{
    testSecondPlayer(rdram, frame);
    static const bool logMp = std::getenv("KH2_TEST_LOG_MP") != nullptr;
    if (logMp && frame % 30 == 0)
    {
        uint32_t player = 0, sheet = 0;
        std::memcpy(&player, rdram + kh2state::addr::PlayerPtr, 4);
        if (player >= 0x00100000u && player < 0x02000000u)
            std::memcpy(&sheet, rdram + player + 0x48C, 4);
        if (sheet >= 0x00100000u && sheet < 0x02000000u)
        {
            int32_t hp = 0, mp = 0;
            uint32_t flags = 0;
            std::memcpy(&hp, rdram + sheet, 4);
            std::memcpy(&mp, rdram + sheet + 0x180, 4);
            std::memcpy(&flags, rdram + player + 0x7F8, 4);
            std::printf("[spells] f%llu player %08X sheet %08X hp %d mp %d flags7F8 %08X\n",
                        (unsigned long long)frame, player, sheet, hp, mp, flags);
            std::fflush(stdout);
        }
    }
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
    // An avatar (avatars.h) first.
    uint32_t avatarFirst = 0;
    for (uint32_t o : kh2avatars::objects())
        if (!avatarFirst)
            avatarFirst = o;
    if (avatarFirst)
    {
        std::printf("[spells] test: avatar %08X casts command %u\n", avatarFirst, command);
        castAsPlayer(avatarFirst, Cast{uint16_t(command), 0});
        return;
    }
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
