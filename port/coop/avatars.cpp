#include "avatars.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <mutex>
#include <chrono>

#include "game_state.h"
#include "puppets.h"

namespace kh2avatars
{
namespace
{
constexpr uint32_t kNoAct = 0x200000u;
constexpr uint32_t kObjFlags = 0x10C, kObjSheet = 0x48C;
constexpr uint32_t kSheetBase = 0x01E4E000u, kSheetSize = 0x268, kMaxAvatars = 3;
constexpr uint32_t kEntrySize = 0x60, kEntryLook = 0x08;
constexpr uint32_t kFriendTable = 0x01C60528; // YS::FRIEND::Get(i): the party members' objects
constexpr uint32_t kObjModel = 0x670, kModelParts = 0x104, kWeaponOwner = 0x570;
// The game's pointers to its player that a new player object takes over.
constexpr uint32_t kPlayerRefs[] = {0x00348638, 0x00348754, 0x0034ED04, 0x00352400, 0x01C5FF10,
                                    0x01C60018, 0x01C60024, 0x01C6C9AC, kh2state::addr::PlayerPtr};

struct State
{
    uint32_t object = 0;
    uint16_t entry = 0;      // the look it was made in
    bool pending = false;    // spawn job queued
    bool prepared = false;   // action off, own sheet
    uint32_t sheet = 0;
    uint16_t motion = 0xFFFF;
    int sinceMotion = 0;
    double lookAsked = -1;   // its look's files were asked for (requestFormLoad)
    uint16_t lookFor = 0;
    uint32_t gen = 0;
    double castUntil = 0;
    uint32_t proxy = 0;      // the party member holding their place (hidden)
    int32_t writtenHp = -1, writtenMp = -1; // what we last put in the sheet
};
std::mutex g_mutex; // g_states (spawn callbacks run on the EE thread)
std::map<uint8_t, State> g_states;
uint32_t g_gen = 0;
uint16_t g_room = 0xFFFF;
double g_roomSince = 0;
uint32_t g_ourPlayer = 0;
std::map<int, std::string> g_names; // character in their party place -> their name

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }

double nowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint32_t entryRecord(const uint8_t *rdram, uint32_t id)
{
    const uint32_t table = rd<uint32_t>(rdram, kh2state::addr::Obj0Ptr);
    if (!inRam(table))
        return 0;
    const uint32_t count = rd<uint32_t>(rdram, table + 4);
    for (uint32_t i = 0; i < count && i < 4096; ++i)
    {
        const uint32_t rec = table + 8 + i * kEntrySize;
        if ((rd<uint32_t>(rdram, rec) & 0xFFFF) == (id & 0xFFFF))
            return rec;
    }
    return 0;
}

// Some object here wears this entry's look (its files are loaded).
bool lookHere(const uint8_t *rdram, uint16_t entry)
{
    const uint32_t rec = entryRecord(rdram, entry);
    if (!rec)
        return false;
    const char *name = reinterpret_cast<const char *>(rdram + ((rec + kEntryLook) & 0x01FFFFFFu));
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        const uint32_t r = rd<uint32_t>(rdram, a.address + kh2state::addr::ObjEntry);
        if (inRam(r) && std::strncmp(reinterpret_cast<const char *>(rdram + ((r + kEntryLook) & 0x01FFFFFFu)), name, 32) == 0)
            return true;
    }
    return false;
}

// Draw none of an object's model's parts (ModelObj +0x104), nor its weapons'.
void hide(uint8_t *rdram, uint32_t obj)
{
    auto hideModel = [&](uint32_t o) {
        const uint32_t model = rd<uint32_t>(rdram, o + kObjModel);
        if (inRam(model))
            wr<uint32_t>(rdram, model + kModelParts, 0);
    };
    hideModel(obj);
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.type == 5 && rd<uint32_t>(rdram, a.address + kWeaponOwner) == obj)
            hideModel(a.address);
}

bool live(const uint8_t *rdram, uint32_t obj)
{
    if (!obj)
        return false;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.address == obj)
            return true;
    return false;
}
} // namespace

bool enabled()
{
    static const bool on = [] {
        const char *e = std::getenv("KH2_AVATARS");
        return e && *e == '1';
    }();
    return on;
}

std::vector<uint32_t> objects()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<uint32_t> out;
    for (auto &[id, s] : g_states)
    {
        if (s.object)
            out.push_back(s.object);
        if (s.proxy)
            out.push_back(s.proxy);
    }
    return out;
}

uint32_t objectFor(uint8_t playerId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_states.find(playerId);
    return it == g_states.end() ? 0 : it->second.object;
}

void noteCast(uint32_t avatar)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[id, s] : g_states)
        if (s.object == avatar)
            s.castUntil = nowSec() + 2.0;
}

std::vector<kh2partner::Heal> update(uint8_t *rdram, const std::vector<kh2partner::Remote> &remotes)
{
    std::vector<kh2partner::Heal> heals;
    std::map<int, std::string> names;
    const kh2state::State st = kh2state::read(rdram);
    const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
    const uint16_t room = uint16_t(st.now.world << 8 | st.now.room);
    const bool jumping = rd<uint32_t>(rdram, kh2state::addr::MapJumpTask) != 0;
    const double t = nowSec();
    std::vector<kh2puppets::Override> overrides;
    std::vector<std::pair<uint32_t, int32_t>> guards;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (room != g_room || jumping)
    {
        // A new room: the old one's objects are gone with it.
        g_room = room;
        g_roomSince = t;
        ++g_gen;
        for (auto &[id, s] : g_states)
        {
            s.object = 0;
            s.pending = false;
            s.prepared = false;
            s.motion = 0xFFFF;
            s.proxy = 0;
            s.writtenHp = s.writtenMp = -1;
        }
    }
    // Ours: the object the game made as the player for this room (a new
    // avatar's first updates point PlayerPtr at itself too).
    bool ourKnown = false;
    for (auto &[id, s] : g_states)
        ourKnown = ourKnown || (s.object && player == s.object);
    if (player && !ourKnown)
        g_ourPlayer = player;
    const bool settled = st.player.valid && st.control == 0 && !jumping && t - g_roomSince > 1.5;
    const bool cutscene = st.control == 3;
    uint32_t sheetIndex = 0;
    for (const kh2partner::Remote &r : remotes)
    {
        State &s = g_states[r.playerId];
        // Their HP and MP live in the party slot they take (slots 1 and 2:
        // the party gauges, items and Cure all use those), else in a sheet
        // of our own (a third player).
        {
            const uint32_t k = sheetIndex++;
            s.sheet = k < 2 ? kh2state::addr::Slot1 - (k + 1) * kh2state::addr::SlotStride
                            : kSheetBase + (k % kMaxAvatars) * kSheetSize;
        }
        const bool here = r.present && r.world == st.now.world && r.room == st.now.room;
        if (s.object && !live(rdram, s.object))
        {
            s.object = 0;
            s.prepared = false;
        }
        uint16_t want = r.model ? r.model : 0x0054;
        static const uint16_t testEntry = [] { // testing: KH2_TEST_AVATAR_ENTRY=0054 (hex)
            const char *e = std::getenv("KH2_TEST_AVATAR_ENTRY");
            return e ? uint16_t(std::strtoul(e, nullptr, 16)) : uint16_t(0);
        }();
        if (testEntry)
            want = testEntry;
        // Not here, or a cutscene (its actors are the story's): away.
        if (!here || cutscene || (s.object && s.entry != want))
        {
            if (s.object)
            {
                std::printf("[avatars] player %u's avatar %08X goes (%s)\n", r.playerId + 1, s.object,
                            !here ? "not here" : cutscene ? "cutscene" : "a new look");
                std::fflush(stdout);
                kh2puppets::remove(s.object);
                s.object = 0;
                s.prepared = false;
                s.motion = 0xFFFF;
            }
            continue;
        }
        if (!s.object)
        {
            if (s.pending || !settled)
                continue;
            // Their look's files: in use here already, or read for it.
            if (!lookHere(rdram, want))
            {
                if (s.lookFor != want)
                {
                    s.lookFor = want;
                    s.lookAsked = t;
                    kh2puppets::requestFormLoad(want);
                    continue;
                }
                if (t - s.lookAsked < 1.5)
                    continue;
            }
            s.pending = true;
            s.entry = want;
            const uint32_t gen = g_gen;
            const uint8_t id = r.playerId;
            std::printf("[avatars] making player %u's avatar (entry %04X)\n", r.playerId + 1, want);
            std::fflush(stdout);
            kh2puppets::spawnAt(want, r.pos, r.yaw, [id, gen](uint32_t obj) {
                std::lock_guard<std::mutex> lock(g_mutex);
                State &s = g_states[id];
                s.pending = false;
                if (gen != g_gen || !obj)
                {
                    if (obj)
                        kh2puppets::remove(obj);
                    return;
                }
                s.object = obj;
                s.prepared = false;
                std::printf("[avatars] player %u's avatar is %08X\n", id + 1, obj);
                std::fflush(stdout);
            });
            continue;
        }
        const uint32_t obj = s.object;
        if (!s.prepared)
        {
            s.prepared = true;
            const uint32_t from = rd<uint32_t>(rdram, obj + kObjSheet);
            if (inRam(from) && s.sheet >= kSheetBase) // (a party slot's is the game's own)
                std::memcpy(rdram + s.sheet, rdram + from, kSheetSize);
            wr<uint32_t>(rdram, obj + kObjSheet, s.sheet);
        }
        const uint32_t flags = rd<uint32_t>(rdram, obj + kObjFlags);
        if (!(flags & kNoAct))
            wr<uint32_t>(rdram, obj + kObjFlags, flags | kNoAct);
        // Their name for the character in their party place (the party
        // gauge, item targets, menus: names.h).
        if (!r.name.empty() && st.now.world < 0x20 && sheetIndex <= 2)
        {
            const uint8_t character =
                rd<uint8_t>(rdram, kh2state::addr::Save + 0x3534 + uint32_t(st.now.world) * 4u + sheetIndex) & 0x7F;
            if (character > 0 && character < 13)
                names[character] = r.name;
        }
        // Their place in the party: the member in slot (index + 1), hidden
        // and frozen where the avatar is, on the avatar's sheet. The party
        // gauge shows it, items and Cure can be used on it, and what they
        // give goes to its player as a heal.
        {
            const uint32_t slot = sheetIndex - 1; // 0 or 1 (party slots 1 and 2)
            const uint32_t proxy = slot < 2 ? rd<uint32_t>(rdram, kFriendTable + slot * 4) : 0;
            s.proxy = proxy && live(rdram, proxy) && proxy != obj ? proxy : 0;
            if (s.proxy)
            {
                const uint32_t pf = rd<uint32_t>(rdram, s.proxy + kObjFlags);
                if (!(pf & kNoAct))
                    wr<uint32_t>(rdram, s.proxy + kObjFlags, pf | kNoAct);
                hide(rdram, s.proxy);
                kh2puppets::Override po;
                po.object = s.proxy;
                std::memcpy(po.pos, r.pos, sizeof(po.pos));
                po.yaw = r.yaw;
                overrides.push_back(po);
            }
        }
        if (std::getenv("KH2_AVATAR_LOG"))
        {
            static int n = 0;
            if (++n % 60 == 0)
                std::printf("[avatars] player %u: sheet %08X hp %d/%d mp %d (written %d) proxy %08X\n", r.playerId + 1,
                            s.sheet, rd<int32_t>(rdram, s.sheet), rd<int32_t>(rdram, s.sheet + 4),
                            rd<int32_t>(rdram, s.sheet + 0x180), s.writtenHp, s.proxy);
        }
        if (r.maxHp > 0)
        {
            // What this game added since we wrote it (a potion, Cure) is a
            // heal for them.
            const int32_t hpNow = rd<int32_t>(rdram, s.sheet + kh2state::addr::SlotHp);
            const int32_t mpNow = rd<int32_t>(rdram, s.sheet + kh2state::addr::SlotMp);
            kh2partner::Heal h;
            h.playerId = r.playerId;
            if (s.writtenHp >= 0 && hpNow > s.writtenHp)
                h.hp = hpNow - s.writtenHp;
            if (s.writtenMp >= 0 && mpNow > s.writtenMp)
                h.mp = mpNow - s.writtenMp;
            if (h.hp > 0 || h.mp > 0)
            {
                std::printf("[avatars] player %u healed here: +%d HP, +%d MP\n", r.playerId + 1, h.hp, h.mp);
                std::fflush(stdout);
                heals.push_back(h);
            }
            const int32_t hp = std::max<int32_t>(1, std::min(r.hp + h.hp, r.maxHp));
            const int32_t mp = std::max<int32_t>(0, std::min(r.mp + h.mp, r.maxMp));
            wr<int32_t>(rdram, s.sheet + kh2state::addr::SlotMaxHp, r.maxHp);
            wr<int32_t>(rdram, s.sheet + kh2state::addr::SlotHp, hp);
            wr<int32_t>(rdram, s.sheet + kh2state::addr::SlotMaxMp, r.maxMp);
            wr<int32_t>(rdram, s.sheet + kh2state::addr::SlotMp, mp);
            s.writtenHp = hp;
            s.writtenMp = mp;
        }
        kh2puppets::Override o;
        o.object = obj;
        std::memcpy(o.pos, r.pos, sizeof(o.pos));
        o.yaw = r.yaw;
        overrides.push_back(o);
        const uint32_t motion = r.motion == 0xFFFFu ? 0u : r.motion;
        if (t < s.castUntil)
        {
            guards.emplace_back(obj, -1); // its own (the cast's)
            s.motion = 0xFFFF;
            continue;
        }
        guards.emplace_back(obj, static_cast<int32_t>(motion));
        const uint32_t playing = rd<uint32_t>(rdram, obj + kh2state::addr::ObjMotionIndex);
        ++s.sinceMotion;
        if (motion != s.motion || (playing != motion && s.sinceMotion > 4))
        {
            s.motion = motion;
            s.sinceMotion = 0;
            kh2puppets::postMotion(obj, motion, 4.0f);
        }
    }
    // Players who left the session.
    for (auto it = g_states.begin(); it != g_states.end();)
    {
        bool still = false;
        for (const kh2partner::Remote &r : remotes)
            still = still || r.playerId == it->first;
        if (!still)
        {
            if (it->second.object)
                kh2puppets::remove(it->second.object);
            it = g_states.erase(it);
        }
        else
            ++it;
    }
    // The game's "player" pointers stay on ours.
    if (g_ourPlayer)
        for (auto &[id, s] : g_states)
            if (s.object)
                for (uint32_t a : kPlayerRefs)
                    if (rd<uint32_t>(rdram, a) == s.object)
                        wr<uint32_t>(rdram, a, g_ourPlayer);
    kh2puppets::setOverrides(overrides, 1);
    kh2puppets::setMotionGuards(guards);
    g_names = names;
    return heals;
}

std::map<int, std::string> names()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_names;
}

void reset(uint8_t *rdram)
{
    (void)rdram;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[id, s] : g_states)
        if (s.object)
            kh2puppets::remove(s.object);
    g_states.clear();
    kh2puppets::setOverrides({}, 1);
    kh2puppets::setMotionGuards({});
}
} // namespace kh2avatars
