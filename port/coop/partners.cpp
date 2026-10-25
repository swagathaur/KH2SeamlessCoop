#include "partners.h"
#include "names.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "game_state.h"
#include "guest_call.h"
#include "hle_call.h"
#include "puppets.h"
#include "story.h"
#include "ps2_runtime.h"

namespace kh2partner
{
namespace
{
constexpr uint32_t kSaveChars = kh2state::addr::Save + 0x24F0;    // 13 records of kLoadoutSize
constexpr uint32_t kSaveParty = kh2state::addr::Save + 0x3534;    // per world: player, companion 1..3
constexpr uint32_t kSaveInventory = kh2state::addr::Save + 0x3580; // item counts
constexpr uint32_t kInventorySize = 320;
constexpr uint32_t kNoCharacter = 0x12;
constexpr uint32_t kObjSheet = 0x48C;     // YS::SHEET* (the party slot for party members)
constexpr uint32_t kObjFlags = 0x10C;     // 0x200000: may not act
constexpr uint32_t kNoAct = 0x200000u;
constexpr uint32_t kMotionChange = 0x0017F540; // YS::MOTION::change(MOTION*, id, blend, start)
constexpr uint32_t kSacrifice = 0x001C9860;    // drive form: take away the members it needs
constexpr uint32_t kEntrySize = 0x60, kEntryLook = 0x08, kEntryLookSize = 0x40;

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
template <typename T> void wr(uint8_t *ram, uint32_t a, T v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T)); }

std::mutex g_mutex;
std::atomic<bool> g_linked{false};
PS2Runtime::RecompiledFunction g_sacrifice = nullptr;

// ---- objentry records ------------------------------------------------------

uint32_t entryRecord(const uint8_t *rdram, uint32_t id)
{
    const uint32_t table = rd<uint32_t>(rdram, kh2state::addr::Obj0Ptr);
    if (!table || table >= 0x02000000u)
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

uint32_t entryByModel(const uint8_t *rdram, const char *model)
{
    const uint32_t table = rd<uint32_t>(rdram, kh2state::addr::Obj0Ptr);
    if (!table || table >= 0x02000000u)
        return 0;
    const uint32_t count = rd<uint32_t>(rdram, table + 4);
    for (uint32_t i = 0; i < count && i < 4096; ++i)
    {
        const uint32_t rec = table + 8 + i * kEntrySize;
        if (std::strncmp(reinterpret_cast<const char *>(rdram + ((rec + kEntryLook) & 0x01FFFFFFu)), model, 32) == 0)
            return rd<uint32_t>(rdram, rec) & 0xFFFF;
    }
    return 0;
}

// Looks we gave entries: entry -> (its own look, the entry whose look it has).
struct Dress
{
    uint8_t original[kEntryLookSize];
    uint16_t as = 0;
};
std::map<uint16_t, Dress> g_dress; // under g_mutex

// Host loop: give `entry` the look of `as` (0: its own). Takes effect for
// objects made afterwards.
void dress(uint8_t *rdram, uint16_t entry, uint16_t as)
{
    auto it = g_dress.find(entry);
    const uint16_t now = it == g_dress.end() ? 0 : it->second.as;
    if (now == as || entry == as)
        return;
    const uint32_t rec = entryRecord(rdram, entry);
    if (!rec)
        return;
    uint8_t *look = rdram + ((rec + kEntryLook) & 0x01FFFFFFu);
    if (!as)
    {
        if (it != g_dress.end())
        {
            std::memcpy(look, it->second.original, kEntryLookSize);
            g_dress.erase(it);
            std::printf("[partner] entry %04X has its own look again\n", entry);
        }
        return;
    }
    const uint32_t from = entryRecord(rdram, as);
    if (!from)
        return;
    if (it == g_dress.end())
    {
        Dress d;
        std::memcpy(d.original, look, kEntryLookSize);
        it = g_dress.emplace(entry, d).first;
    }
    std::memcpy(look, rdram + ((from + kEntryLook) & 0x01FFFFFFu), kEntryLookSize);
    it->second.as = as;
    std::printf("[partner] entry %04X dressed as %.32s (from the next room on)\n", entry,
                reinterpret_cast<const char *>(look));
    std::fflush(stdout);
}

uint16_t dressedAs(uint16_t entry)
{
    auto it = g_dress.find(entry);
    return it == g_dress.end() ? 0 : it->second.as;
}

// ---- party ------------------------------------------------------------------

struct Companion
{
    uint32_t object = 0;
    uint16_t entry = 0;
    int slot = 0; // 1 or 2
    char name[33] = {};
};

std::vector<Companion> companions(const uint8_t *rdram)
{
    std::vector<Companion> out;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        if (a.type != 1)
            continue;
        // The party's slots run down from the player's: companion n's stats
        // are at Slot1 - n * 0x268.
        const uint32_t sheet = rd<uint32_t>(rdram, a.address + kObjSheet);
        if (sheet > kh2state::addr::Slot1)
            continue;
        const uint32_t off = kh2state::addr::Slot1 - sheet;
        if (off % kh2state::addr::SlotStride)
            continue;
        const int slot = int(off / kh2state::addr::SlotStride);
        if (slot < 1 || slot > 2)
            continue;
        Companion c;
        c.object = a.address;
        c.entry = uint16_t(a.entryId);
        c.slot = slot;
        std::memcpy(c.name, a.name, sizeof(c.name));
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const Companion &a, const Companion &b) { return a.slot < b.slot; });
    return out;
}

// The weapon objentry that goes with a character: "P_EX020" -> "W_EX020"
// (by the entry's own name, also while it is dressed as someone else).
uint16_t weaponOf(const uint8_t *rdram, uint16_t entry)
{
    const uint32_t rec = entryRecord(rdram, entry);
    if (!rec)
        return 0;
    char name[33] = {};
    auto dressed = g_dress.find(entry);
    if (dressed != g_dress.end())
        std::memcpy(name, dressed->second.original, 32);
    else
        std::memcpy(name, rdram + ((rec + kEntryLook) & 0x01FFFFFFu), 32);
    if (name[0] != 'P' || name[1] != '_')
        return 0;
    name[0] = 'W';
    return uint16_t(entryByModel(rdram, name));
}

// ---- empty party slots -------------------------------------------------------
//
// Where this game's party has no companion in a slot (Roxas's days, scenes the
// player is alone in), the other players still get a real party member to
// stand in: while they are in the session, the world's empty slots
// (save +0x3534, 0x12 = nobody) get Donald and Goofy, whose objects are then
// dressed as the players as anywhere else. Saves never see this: the bytes go
// back to "nobody" in the memory card image (kh2story::setSaveOverrides) and
// in the game when the session ends.
constexpr uint8_t kNobody = 0x12;
constexpr uint8_t kFillCharacters[2] = {1, 2};      // Donald, Goofy
constexpr uint16_t kFillEntries[2] = {0x005C, 0x005D}; // their objentries (P_EX020, P_EX030)
struct Fill
{
    uint32_t addr;
    uint8_t original;
};
std::vector<Fill> g_fill;   // under g_mutex
bool g_fillRefresh = false; // a slot in this room's world was just filled

void publishFill()
{
    std::vector<std::pair<uint32_t, uint8_t>> o;
    for (const Fill &f : g_fill)
        o.emplace_back(f.addr - kh2state::addr::Save, f.original);
    kh2story::setSaveOverrides(o);
}

void unfillAll(uint8_t *rdram)
{
    if (g_fill.empty())
        return;
    for (const Fill &f : g_fill)
        wr<uint8_t>(rdram, f.addr, f.original);
    std::printf("[partner] the party is the game's own again\n");
    std::fflush(stdout);
    g_fill.clear();
    publishFill();
}

// `wanted`: how many companions the other players need (0..2).
void fillParty(uint8_t *rdram, uint8_t world, size_t wanted)
{
    if (world == 0 || world >= 0x20)
        return;
    bool changed = false;
    for (int k = 1; k <= 2; ++k)
    {
        const uint32_t addr = kSaveParty + uint32_t(world) * 4u + uint32_t(k);
        auto it = std::find_if(g_fill.begin(), g_fill.end(), [&](const Fill &f) { return f.addr == addr; });
        const uint8_t cur = rd<uint8_t>(rdram, addr);
        if (size_t(k) <= wanted && it == g_fill.end() && cur == kNobody)
        {
            // Someone not already in this world's party.
            uint8_t pick = 0;
            for (uint8_t c : kFillCharacters)
            {
                bool used = false;
                for (int j = 0; j < 4; ++j)
                    used = used || rd<uint8_t>(rdram, kSaveParty + uint32_t(world) * 4u + uint32_t(j)) == c;
                if (!used && !pick)
                    pick = c;
            }
            if (!pick)
                continue;
            g_fill.push_back({addr, cur});
            wr<uint8_t>(rdram, addr, pick);
            std::printf("[partner] party slot %d of world %02X was empty: %s stands in for a player\n", k, world,
                        pick == 1 ? "Donald" : "Goofy");
            std::fflush(stdout);
            changed = g_fillRefresh = true;
        }
        else if (size_t(k) > wanted && it != g_fill.end())
        {
            wr<uint8_t>(rdram, addr, it->original);
            g_fill.erase(it);
            changed = true;
        }
    }
    if (changed)
        publishFill();
}

uint16_t fillEntry(uint8_t character)
{
    for (size_t i = 0; i < 2; ++i)
        if (kFillCharacters[i] == character)
            return kFillEntries[i];
    return 0;
}

uint32_t slotAddress(int slot) { return kh2state::addr::Slot1 - kh2state::addr::SlotStride * uint32_t(slot); }

// ---- per player link --------------------------------------------------------

struct State
{
    uint8_t playerId = 0;
    int slot = 0;
    uint16_t companionEntry = 0, companionWeapon = 0;
    uint32_t object = 0;         // the companion object we drive
    uint16_t lookAtCreation = 0;  // whose look its entry had when the object was made (0: its own)
    bool dressedAtCreation = false; // ... and that is the player's current look
    uint16_t modelSeen = 0;
    bool refreshAsked = false;   // we asked for the room to be reloaded for this object
    uint32_t flagsBefore = 0;
    uint16_t motion = 0xFFFF;
    uint16_t weaponSeen = 0;
    int sinceMotion = 0; // updates since we last started a motion on it
    int32_t writtenHp = -1, writtenMp = -1;
    // Equipment: the character record we fill, its own contents, what we wrote.
    int character = -1;
    std::vector<uint8_t> ownRecord, wroteRecord;
    std::vector<uint8_t> inventory; // as it was while the record was ours
};
std::map<uint8_t, State> g_states; // under g_mutex
std::vector<Link> g_links;          // under g_mutex
bool g_wantRefresh = false;         // under g_mutex

void releaseObject(uint8_t *rdram, State &s)
{
    if (!s.object)
        return;
    // Give it back to the game if it is still there.
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.address == s.object)
        {
            const uint32_t flags = rd<uint32_t>(rdram, s.object + kObjFlags);
            wr<uint32_t>(rdram, s.object + kObjFlags, (flags & ~kNoAct) | (s.flagsBefore & kNoAct));
            break;
        }
    s.object = 0;
    s.motion = 0xFFFF;
}

void restoreRecord(uint8_t *rdram, State &s)
{
    if (s.character >= 0 && !s.ownRecord.empty())
        std::memcpy(rdram + kSaveChars + uint32_t(s.character) * kLoadoutSize, s.ownRecord.data(), kLoadoutSize);
    s.character = -1;
    s.ownRecord.clear();
    s.wroteRecord.clear();
    s.inventory.clear();
}

void release(uint8_t *rdram, State &s)
{
    releaseObject(rdram, s);
    restoreRecord(rdram, s);
    if (s.companionEntry)
        dress(rdram, s.companionEntry, 0);
    if (s.companionWeapon)
        dress(rdram, s.companionWeapon, 0);
    s.companionEntry = s.companionWeapon = 0;
}

// The partner's equipment in the companion's save record; undo edits made here.
void syncRecord(uint8_t *rdram, State &s, const Remote &r, uint8_t world)
{
    const uint8_t character = rd<uint8_t>(rdram, kSaveParty + uint32_t(world) * 4u + uint32_t(s.slot));
    if (r.loadout.size() != kLoadoutSize || character >= 13 || character == 0)
    {
        restoreRecord(rdram, s);
        return;
    }
    uint8_t *rec = rdram + kSaveChars + uint32_t(character) * kLoadoutSize;
    uint8_t *inv = rdram + kSaveInventory;
    if (s.character != int(character))
    {
        restoreRecord(rdram, s);
        s.character = character;
        s.ownRecord.assign(rec, rec + kLoadoutSize);
    }
    // The menu changes the equipment part: counts, armour, accessories, items,
    // abilities (0x10..0xF4). The game itself rewrites current HP/MP (+4, +6)
    // when rooms change, which is no edit.
    constexpr size_t kEquipFrom = 0x10, kEquipTo = 0xF4;
    if (!s.wroteRecord.empty() && !s.inventory.empty() &&
        std::memcmp(rec + kEquipFrom, s.wroteRecord.data() + kEquipFrom, kEquipTo - kEquipFrom) != 0)
    {
        // Changed here (the party menu): theirs is not ours to change. Put the
        // record back and undo what the change did to our inventory.
        std::memcpy(inv, s.inventory.data(), kInventorySize);
        std::printf("[partner] player %u's equipment can only be changed by them; undone\n", s.playerId + 1);
        std::fflush(stdout);
    }
    // Theirs, except the weapon (the companion's class picks its weapon model
    // from it, and a keyblade id there is not one it knows) and the current
    // HP and MP bytes, which this game keeps for its own use.
    std::vector<uint8_t> want(r.loadout);
    std::memcpy(want.data(), s.ownRecord.data(), 2);
    want[4] = rec[4];
    want[6] = rec[6];
    if (std::memcmp(rec, want.data(), kLoadoutSize) != 0)
        std::memcpy(rec, want.data(), kLoadoutSize);
    s.wroteRecord = want;
    s.inventory.assign(inv, inv + kInventorySize);
}

// FUN_001C9860(form) override: while linked, forms leave the party alone.
void sacrificeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_linked.load(std::memory_order_relaxed))
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_sacrifice, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    g_sacrifice = runtime.lookupFunction(kSacrifice);
    if (!g_sacrifice || !runtime.replaceFunction(kSacrifice, &sacrificeHook))
    {
        std::fprintf(stderr, "[partner] could not hook 0x%08X\n", kSacrifice);
        g_sacrifice = nullptr;
        return false;
    }
    return true;
}

std::vector<Heal> update(uint8_t *rdram, const std::vector<Remote> &remotes)
{
    std::vector<Heal> heals;
    std::vector<kh2puppets::Override> overrides;
    std::vector<std::pair<uint32_t, int32_t>> guards;
    std::lock_guard<std::mutex> lock(g_mutex);
    const kh2state::State st = kh2state::read(rdram);
    const std::vector<Companion> comps = companions(rdram);

    // Remotes in id order take companion slots 1 and 2.
    std::vector<const Remote *> order;
    for (const Remote &r : remotes)
        order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const Remote *a, const Remote *b) { return a->playerId < b->playerId; });
    if (order.size() > 2)
        order.resize(2);
    if (st.player.valid)
        fillParty(rdram, st.now.world, order.size());

    // Players gone from the session.
    for (auto it = g_states.begin(); it != g_states.end();)
    {
        bool still = false;
        for (const Remote *r : order)
            still = still || r->playerId == it->first;
        if (!still)
        {
            release(rdram, it->second);
            it = g_states.erase(it);
        }
        else
            ++it;
    }

    g_links.clear();
    g_wantRefresh = false;
    for (size_t i = 0; i < order.size(); ++i)
    {
        const Remote &r = *order[i];
        State &s = g_states[r.playerId];
        s.playerId = r.playerId;
        s.slot = int(i) + 1;
        const Companion *c = nullptr;
        for (const Companion &x : comps)
            if (x.slot == s.slot)
                c = &x;

        // Was the object made in a player's look? (Asked before dressing now.)
        const uint16_t lookNow = c ? dressedAs(c->entry) : 0;
        // Look: the companion's entry (and weapon) wear the player's.
        // A slot we just filled: its object comes with the next room, so
        // dress its entry now and it is made in the player's look.
        if (!c && !s.companionEntry && g_fillRefresh && st.now.world < 0x20)
        {
            const uint8_t character =
                rd<uint8_t>(rdram, kSaveParty + uint32_t(st.now.world) * 4u + uint32_t(s.slot));
            const uint16_t entry = fillEntry(character);
            for (const Fill &f : g_fill)
                if (entry && f.addr == kSaveParty + uint32_t(st.now.world) * 4u + uint32_t(s.slot))
                {
                    s.companionEntry = entry;
                    s.companionWeapon = weaponOf(rdram, entry);
                }
        }
        if (c && c->entry != s.companionEntry)
        {
            if (s.companionEntry)
                dress(rdram, s.companionEntry, 0);
            if (s.companionWeapon)
                dress(rdram, s.companionWeapon, 0);
            s.companionEntry = c->entry;
            s.companionWeapon = weaponOf(rdram, c->entry);
        }
        if (s.companionEntry && r.model)
            dress(rdram, s.companionEntry, r.model);
        if (r.weapon && r.weapon != s.weaponSeen) // 0 while their room loads
        {
            s.weaponSeen = r.weapon;
            std::printf("[partner] player %u's weapon is entry %04X (shown on entry %04X)\n", r.playerId + 1, r.weapon,
                        s.companionWeapon);
            std::fflush(stdout);
        }
        // The weapon only with the body: a companion made in its own look
        // holding another character's weapon (bones that are not there)
        // crashed the game.
        if (s.companionWeapon && r.weapon && r.model && dressedAs(s.companionEntry) == r.model)
            dress(rdram, s.companionWeapon, r.weapon);
        else if (s.companionWeapon && dressedAs(s.companionEntry) == 0)
            dress(rdram, s.companionWeapon, 0);

        // A new companion object: made with their look or not?
        const uint32_t obj = c ? c->object : 0;
        if (obj != s.object)
        {
            releaseObject(rdram, s);
            s.object = obj;
            if (obj)
            {
                s.flagsBefore = rd<uint32_t>(rdram, obj + kObjFlags);
                s.lookAtCreation = lookNow;
                s.refreshAsked = false;
                std::printf("[partner] player %u is %s (party slot %d, %s)\n", r.playerId + 1, c->name, s.slot,
                            lookNow ? "in a partner's look" : "their look from the next room on");
                std::fflush(stdout);
            }
        }

        if (r.model && r.model != s.modelSeen)
        {
            s.modelSeen = r.model; // a new look (a drive form): worth another reload
            s.refreshAsked = false;
        }
        // Their current look (a drive form changes it) against the object's.
        s.dressedAtCreation = obj && s.lookAtCreation && s.lookAtCreation == r.model;
        const bool here = obj && r.present && r.world == st.now.world && r.room == st.now.room;
        // Elsewhere (another room, loading): the companion waits out of sight,
        // still and out of reach, rather than acting in a look its AI does not
        // know.
        const bool away = obj && s.lookAtCreation && !here;
        if (obj)
        {
            uint32_t flags = rd<uint32_t>(rdram, obj + kObjFlags);
            const uint32_t want = (here || away) ? (flags | kNoAct) : ((flags & ~kNoAct) | (s.flagsBefore & kNoAct));
            if (want != flags)
                wr<uint32_t>(rdram, obj + kObjFlags, want);
        }
        if (away && st.player.valid)
        {
            kh2puppets::Override o;
            o.object = obj;
            o.pos[0] = st.player.pos[0];
            o.pos[1] = st.player.pos[1] + 50000.0f; // far below (up is -y)
            o.pos[2] = st.player.pos[2];
            overrides.push_back(o);
        }
        if (here)
        {
            kh2puppets::Override o;
            o.object = obj;
            std::memcpy(o.pos, r.pos, sizeof(o.pos));
            o.yaw = r.yaw;
            overrides.push_back(o);
            // Their motions only make sense on their own motion set. Also put
            // theirs back when this game started another (a hit reaction).
            const uint32_t playing = rd<uint32_t>(rdram, obj + kh2state::addr::ObjMotionIndex);
            ++s.sinceMotion;
            if (s.dressedAtCreation)
                guards.emplace_back(obj, static_cast<int32_t>(r.motion)); // its own AI's motions kept off
            if (s.dressedAtCreation && (r.motion != s.motion || (playing != r.motion && s.sinceMotion > 4)))
            {
                s.motion = r.motion;
                s.sinceMotion = 0;
                kh2guest::postCall({kMotionChange, {obj + kh2state::addr::ObjMotion, r.motion}, {4.0f, 0.0f}});
            }
        }

        // Stats: their HP and MP in the companion's party slot; what this game
        // added since we wrote it is a heal for them, and what it took (our
        // enemies hitting their stand-in) is damage for them.
        if (c && r.maxHp > 0)
        {
            const uint32_t base = slotAddress(s.slot);
            const int32_t hpNow = rd<int32_t>(rdram, base + kh2state::addr::SlotHp);
            const int32_t mpNow = rd<int32_t>(rdram, base + kh2state::addr::SlotMp);
            Heal h;
            h.playerId = r.playerId;
            if (s.writtenHp >= 0 && hpNow != s.writtenHp && hpNow >= 0)
                h.hp = hpNow - s.writtenHp;
            if (s.writtenMp >= 0 && mpNow > s.writtenMp)
                h.mp = mpNow - s.writtenMp;
            if (h.hp != 0 || h.mp > 0)
                heals.push_back(h);
            // Never 0 here: a companion at 0 HP goes down in this game.
            const int32_t hp = std::max<int32_t>(1, std::min(r.hp + h.hp, r.maxHp));
            const int32_t mp = std::max<int32_t>(0, std::min(r.mp + h.mp, r.maxMp));
            wr<int32_t>(rdram, base + kh2state::addr::SlotMaxHp, r.maxHp);
            wr<int32_t>(rdram, base + kh2state::addr::SlotHp, hp);
            wr<int32_t>(rdram, base + kh2state::addr::SlotMaxMp, r.maxMp);
            wr<int32_t>(rdram, base + kh2state::addr::SlotMp, mp);
            s.writtenHp = hp;
            s.writtenMp = mp;
        }
        else
            s.writtenHp = s.writtenMp = -1;

        if (st.now.world != 0 && st.now.world < 19)
            syncRecord(rdram, s, r, st.now.world);

        // In our room in the old look: a reload of the room shows theirs now.
        if (here && !s.dressedAtCreation && r.model && dressedAs(s.companionEntry) == r.model && !s.refreshAsked)
            g_wantRefresh = true;

        Link l;
        l.playerId = r.playerId;
        l.slot = s.slot;
        l.object = here ? obj : 0;
        l.entry = s.companionEntry;
        l.dressed = s.dressedAtCreation;
        g_links.push_back(l);
    }
    g_linked = !g_links.empty();
    if (g_fillRefresh)
    {
        // The room again, with the new party members, once their entries
        // wear the players' looks (a player still loading has none yet).
        bool dressed = true;
        for (const auto &[id, st2] : g_states)
            if (!st2.object && st2.companionEntry && !dressedAs(st2.companionEntry))
                dressed = false;
        if (dressed)
        {
            g_fillRefresh = false;
            g_wantRefresh = true;
        }
    }
    kh2puppets::setOverrides(overrides, 1);
    kh2puppets::setMotionGuards(guards);
    // The party members they stand in for go by their names in menus.
    {
        std::map<int, std::string> names;
        if (st.now.world < 0x20)
            for (size_t i = 0; i < order.size(); ++i)
            {
                if (order[i]->name.empty())
                    continue;
                const uint8_t character =
                    rd<uint8_t>(rdram, kSaveParty + uint32_t(st.now.world) * 4u + uint32_t(i + 1));
                if (character > 0 && character < 13)
                    names[character] = order[i]->name;
            }
        kh2names::set(names);
    }
    return heals;
}

void reset(uint8_t *rdram)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &[id, s] : g_states)
        release(rdram, s);
    g_states.clear();
    for (auto it = g_dress.begin(); it != g_dress.end();)
    {
        const uint16_t entry = it->first;
        ++it;
        dress(rdram, entry, 0);
    }
    g_links.clear();
    g_linked = false;
    unfillAll(rdram);
    kh2puppets::setOverrides({}, 1);
    kh2puppets::setMotionGuards({});
    kh2names::set({});
}

uint16_t localWeaponEntry(const uint8_t *rdram)
{
    // The player's weapon: the weapon object whose owner (+0x570) is the player.
    constexpr uint32_t kWeaponOwner = 0x570;
    const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
    if (!player)
        return 0;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.type == 5 && rd<uint32_t>(rdram, a.address + kWeaponOwner) == player)
            return uint16_t(a.entryId);
    return 0;
}

std::vector<uint8_t> localLoadout(const uint8_t *rdram)
{
    const kh2state::State st = kh2state::read(rdram);
    uint8_t character = 0;
    if (st.now.world != 0 && st.now.world < 19)
        character = rd<uint8_t>(rdram, kSaveParty + uint32_t(st.now.world) * 4u);
    if (character >= 13)
        character = 0;
    const uint8_t *rec = rdram + kSaveChars + uint32_t(character) * kLoadoutSize;
    return std::vector<uint8_t>(rec, rec + kLoadoutSize);
}

void applyHeal(uint8_t *rdram, int32_t hp, int32_t mp)
{
    const uint32_t base = slotAddress(0);
    if (hp < 0)
    {
        // Hurt by an enemy in another player's game: taken the game's way
        // (the player's add_hp, vtable +0x78), so the minimum HP while
        // downed-protected and a Game Over when alone both apply.
        const int32_t amount = hp;
        kh2guest::post([amount](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
            if (!first)
                return std::nullopt;
            const uint32_t player = rd<uint32_t>(env.rdram, kh2state::addr::PlayerPtr);
            if (!player || player >= 0x02000000u)
                return std::nullopt;
            const uint32_t vt = rd<uint32_t>(env.rdram, player);
            const uint32_t fn = vt && vt < 0x02000000u ? rd<uint32_t>(env.rdram, vt + 0x78) : 0;
            if (!fn || rd<int32_t>(env.rdram, kh2state::addr::Slot1) <= 0)
                return std::nullopt;
            return kh2guest::Call{fn, {player, static_cast<uint32_t>(amount), 0u, 0u}, {}};
        });
        std::printf("[partner] hit in a partner's game: %d HP\n", hp);
        std::fflush(stdout);
        hp = 0;
        if (mp <= 0)
            return;
    }
    if (hp > 0)
    {
        const int32_t now = rd<int32_t>(rdram, base + kh2state::addr::SlotHp);
        const int32_t max = rd<int32_t>(rdram, base + kh2state::addr::SlotMaxHp);
        if (now > 0) // a heal does not raise the dead
            wr<int32_t>(rdram, base + kh2state::addr::SlotHp, std::min(max, now + hp));
    }
    if (mp > 0)
    {
        const int32_t now = rd<int32_t>(rdram, base + kh2state::addr::SlotMp);
        const int32_t max = rd<int32_t>(rdram, base + kh2state::addr::SlotMaxMp);
        wr<int32_t>(rdram, base + kh2state::addr::SlotMp, std::min(max, now + mp));
    }
    std::printf("[partner] healed by a partner: +%d HP, +%d MP\n", hp, mp);
    std::fflush(stdout);
}

std::vector<Link> links()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_links;
}

bool takeRefreshRequest()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_wantRefresh)
        return false;
    g_wantRefresh = false;
    for (auto &[id, s] : g_states)
        if (s.object && !s.dressedAtCreation)
            s.refreshAsked = true;
    return true;
}

bool linked(uint8_t playerId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Link &l : g_links)
        if (l.playerId == playerId)
            return true;
    return false;
}
} // namespace kh2partner
