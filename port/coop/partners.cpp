#include "partners.h"
#include "names.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

#include "game_state.h"
#include "guest_call.h"
#include "net/protocol.h"
#include "hle_call.h"
#include "puppets.h"
#include "story.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

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
std::atomic<uint32_t> g_made[2] = {}; // party members spawnMissing made, by slot - 1
std::atomic<uint32_t> g_standIns[2] = {}; // dressed stand-in objects in this room (voiceHook)
PS2Runtime::RecompiledFunction g_sacrifice = nullptr;
// Drive forms without party members (setFormsFree): FUN_001C99B0(form), the
// form command's availability by the members it needs (0: all there), says 0.
constexpr uint32_t kFormMembers = 0x001C99B0;
PS2Runtime::RecompiledFunction g_formMembers = nullptr;
std::atomic<bool> g_formsFree{false};

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
    uint8_t joint = 0; // the record's weapon joint (+0x07), which goes with the model
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
            rdram[(rec + 7) & 0x01FFFFFFu] = it->second.joint;
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
        d.joint = rdram[(rec + 7) & 0x01FFFFFFu];
        it = g_dress.emplace(entry, d).first;
    }
    std::memcpy(look, rdram + ((from + kEntryLook) & 0x01FFFFFFu), kEntryLookSize);
    // The joint the weapon is held at belongs to the skeleton: Donald's
    // (2) on Roxas's model is his head.
    rdram[(rec + 7) & 0x01FFFFFFu] = rdram[(from + 7) & 0x01FFFFFFu];
    it->second.as = as;
    std::printf("[partner] entry %04X dressed as %.32s (from the next room on)\n", entry,
                reinterpret_cast<const char *>(look));
    std::fflush(stdout);
}

// A drive form's look ("P_EX100_BTLF") -> the character's own ("P_EX100"),
// by the look's own name (also while the entry is dressed); 0: not a form.
std::map<uint8_t, uint16_t> g_baseWeapon; // player -> their weapon out of a form (under g_mutex)
uint16_t baseLook(const uint8_t *rdram, uint16_t model)
{
    if (!model)
        return 0;
    const uint32_t rec = entryRecord(rdram, model);
    if (!rec)
        return 0;
    char name[33] = {};
    auto dressed = g_dress.find(model);
    if (dressed != g_dress.end())
        std::memcpy(name, dressed->second.original, 32);
    else
        std::memcpy(name, rdram + ((rec + kEntryLook) & 0x01FFFFFFu), 32);
    static const char *const kForms[] = {"_BTLF", "_MAGF", "_TRIF", "_ULTF", "_HTLF", "_KH1F"};
    if (std::strncmp(name, "P_EX", 4) != 0 || std::strlen(name) < 12)
        return 0;
    bool form = false;
    for (const char *f : kForms)
        form = form || std::strcmp(name + 7, f) == 0;
    if (!form)
        return 0;
    name[7] = 0;
    // The entry whose own look that is (not one of ours dressed in it).
    const uint32_t table = rd<uint32_t>(rdram, kh2state::addr::Obj0Ptr);
    if (!table || table >= 0x02000000u)
        return 0;
    const uint32_t count = rd<uint32_t>(rdram, table + 4);
    for (uint32_t i = 0; i < count && i < 4096; ++i)
    {
        const uint32_t r = table + 8 + i * kEntrySize;
        const uint16_t id = uint16_t(rd<uint32_t>(rdram, r) & 0xFFFF);
        if (g_dress.count(id))
            continue;
        if (std::strncmp(reinterpret_cast<const char *>(rdram + ((r + kEntryLook) & 0x01FFFFFFu)), name, 32) == 0)
            return id;
    }
    return 0;
}

bool lookInUse(const uint8_t *rdram, uint16_t entry)
{
    // The look's own name (also while the entry is dressed)...
    const uint32_t rec = entryRecord(rdram, entry);
    if (!rec)
        return true; // nothing to load
    char name[33] = {};
    auto dressed = g_dress.find(entry);
    if (dressed != g_dress.end())
        std::memcpy(name, dressed->second.original, 32);
    else
        std::memcpy(name, rdram + ((rec + kEntryLook) & 0x01FFFFFFu), 32);
    // ... worn by an object here.
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
    {
        const uint32_t r = rd<uint32_t>(rdram, a.address + kh2state::addr::ObjEntry);
        if (r && r < 0x02000000u &&
            std::strncmp(reinterpret_cast<const char *>(rdram + ((r + kEntryLook) & 0x01FFFFFFu)), name, 32) == 0)
            return true;
    }
    return false;
}

// A stand-in goes with what it holds: its weapons (objects whose owner,
// +0x570, it is) are not taken by its own leave, and one left behind asked
// its gone owner for its hand bone and the game crashed. Weapons first.
void removeWithWeapons(const uint8_t *rdram, uint32_t obj)
{
    constexpr uint32_t kWeaponOwner = 0x570;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.type == 5 && rd<uint32_t>(rdram, a.address + kWeaponOwner) == obj)
            kh2puppets::remove(a.address);
    kh2puppets::remove(obj);
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

constexpr uint32_t kMemberEntries = 0x01C6CC20; // u16 objentry per party character (Donald 1, Goofy 2, ...)

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
        int slot = int(off / kh2state::addr::SlotStride);
        // Its place in the world's party, by who it is: a boss that takes a
        // party sheet for itself (Twilight Thorn has slot 1's) moves later
        // party members' sheets down, so the sheet alone can name the wrong
        // place. Each character's objentry is in the game's member table
        // (0x01C6CC10 + 0x10, a u16 per character; YS::FRIEND::GetEntryId).
        const uint8_t world = rd<uint8_t>(rdram, kh2state::addr::Now);
        if (world && world < 0x20)
            for (int k = 1; k <= 2; ++k)
            {
                const uint8_t c = rd<uint8_t>(rdram, kSaveParty + uint32_t(world) * 4u + uint32_t(k));
                if (c < 0x12 && rd<uint16_t>(rdram, kMemberEntries + uint32_t(c) * 2u) == uint16_t(a.entryId))
                    slot = k;
            }
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
constexpr uint8_t kWorldMap = 0x0F;
constexpr size_t kRecordLevel = 0x0F; // a character save record's level byte
constexpr uint8_t kFillCharacters[2] = {1, 2};      // Donald, Goofy
constexpr uint16_t kFillEntries[2] = {0x005C, 0x005D}; // their objentries (P_EX020, P_EX030)
struct Fill
{
    uint32_t addr;
    uint8_t original;
    uint8_t pick; // what we wrote
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
std::atomic<bool> g_fillHeld{false}; // --fill-hold

void fillParty(uint8_t *rdram, uint8_t world, size_t wanted)
{
    static const bool off = [] { const char *e = std::getenv("KH2_NO_FILL"); return e && *e == '1'; }();
    if (off)
        wanted = 0; // diagnostics: the game's own party only
    if (g_fillHeld.load(std::memory_order_relaxed))
        return;
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
            g_fill.push_back({addr, cur, pick});
            wr<uint8_t>(rdram, addr, pick);
            std::printf("[partner] party slot %d of world %02X was empty: %s stands in for a player\n", k, world,
                        pick == 1 ? "Donald" : "Goofy");
            std::fflush(stdout);
            changed = g_fillRefresh = true;
        }
        else if (size_t(k) <= wanted && it != g_fill.end() && cur != it->pick)
        {
            if (cur == kNobody)
            {
                // A room for the player alone (a boss, a story scene) clears
                // the world's party while it lasts; the players still need
                // their places there.
                wr<uint8_t>(rdram, addr, it->pick);
                std::printf("[partner] party slot %d of world %02X was cleared by the room: %s stands in again\n", k,
                            world, it->pick == 1 ? "Donald" : "Goofy");
                std::fflush(stdout);
            }
            else
            {
                // The game put someone there itself: theirs from now on.
                g_fill.erase(it);
                changed = true;
            }
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

// A room made for the player alone (Twilight Thorn, the Station of
// Awakening, story scenes) has no places for party members in its spawn list
// (only PLAYER, 0x236), and its party rule clears the world's party table
// (fillParty puts the stand-ins back). Once the room has settled, the missing
// ones are made the way its list would have: the placeholder FRIEND_1 /
// FRIEND_2 (0x237 / 0x238) resolves through the party table to the member's
// objentry (YS::FRIEND::GetEntryId), and the member's class takes a party
// sheet for it (YS::SHEET::AllocFriend), so it is a real party member: our
// link drives it as anywhere else. Behind the player to the left and right;
// once per room and slot.
constexpr uint32_t kFriendPlaceholder[2] = {0x0237, 0x0238};
uint32_t g_spawnRoom = 0xFFFFFFFFu; // world/room/event the attempts below are for
double g_roomSince = 0;
bool g_spawnTried[2] = {false, false};
// A stand-in made here by spawnMissing (the room had no place for the party
// member): this room set no memory aside for the member's files, so anything
// it loads later (its weapon's model, when it fights) goes to memory the
// pause menu reuses; on leaving the menu the weapon drew from what was there
// and the game crashed. Such a stand-in holds our weapon, which is loaded.
bool g_lateRoom[2] = {false, false};
// Looks (objentry ids) whose files we asked for this room, and when. A
// stand-in made in a look whose model was not loaded yet (a drive form's,
// P_EX100_BTLF) got its weapon at once, before its model: the weapon asked
// the stand-in's model (+0x670, still null) for its hand bone and the game
// crashed. The files are asked for first (into the room's cache, as a spawn
// list does) and the stand-in is made a moment later.
double nowSec();
std::map<uint16_t, double> g_lookAsked;
// A look some object here already wears (its model is loaded): asking for
// its files again made the game load them anew under that object (the
// host's own Sora, P_EX100), which then had no model and crashed.
bool lookInUse(const uint8_t *rdram, uint16_t entry);
bool lookReady(const uint8_t *rdram, uint16_t entry)
{
    if (!entry || lookInUse(rdram, entry))
        return true;
    auto it = g_lookAsked.find(entry);
    if (it == g_lookAsked.end())
    {
        g_lookAsked[entry] = nowSec();
        // Only a drive form's body look ("P_EX100_BTLF"): nothing here has
        // its files, and a stand-in made in it never gets a model otherwise.
        // Not weapons: asking again for one the game keeps (Sora's own
        // keyblade while he is in a form) took its model away and crashed.
        static const bool preload = [] {
            const char *e = std::getenv("KH2_LOOK_PRELOAD");
            return !(e && *e == '0');
        }();
        if (preload && baseLook(rdram, entry))
            kh2puppets::requestFormLoad(entry);
        return false;
    }
    return nowSec() - it->second >= 1.5;
}
bool g_roomOverridden = false;

double nowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
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
    uint32_t ride = 0;          // the board (our game's) it rides now
    uint32_t rideFlagsBefore = 0;
    int sinceRideMotion = 0;
    int32_t writtenHp = -1, writtenMp = -1;
    // Equipment: the character record we fill, its own contents, what we wrote.
    int character = -1;
    int level = -1; // that character's level byte as last seen (stats)
    std::vector<uint8_t> ownRecord, wroteRecord;
    std::vector<uint8_t> inventory; // as it was while the record was ours
};
std::map<uint8_t, State> g_states; // under g_mutex
std::vector<Link> g_links;          // under g_mutex
bool g_wantRefresh = false;         // under g_mutex

void stopRiding(uint8_t *rdram, State &s, uint32_t obj);

// Two players put on one spot (both games place their own player at a
// battle's start point) stood in each other: each game's player was pushed
// off the other's stand-in, which followed them, and both slid away for good.
// A stand-in closer than this to our player is shown that far away instead,
// straight away from us (on one spot: to a side picked by player number, the
// mirror of what the other game does). Apart, it is where they are.
void keepApart(const kh2state::State &st, uint8_t playerId, float (&pos)[3])
{
    constexpr float kApart = 90.0f; // a little more than two bodies
    if (!st.player.valid)
        return;
    const float dx = pos[0] - st.player.pos[0], dz = pos[2] - st.player.pos[2];
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d >= kApart || std::fabs(pos[1] - st.player.pos[1]) > 150.0f)
        return;
    float ux = 0, uz = 0;
    if (d > 1.0f)
    {
        ux = dx / d;
        uz = dz / d;
    }
    else
        ux = (playerId & 1) ? 1.0f : -1.0f;
    pos[0] = st.player.pos[0] + ux * kApart;
    pos[2] = st.player.pos[2] + uz * kApart;
}

void releaseObject(uint8_t *rdram, State &s)
{
    if (!s.object)
        return;
    // Give it back to the game if it is still there.
    bool live = false;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        live = live || a.address == s.object;
    stopRiding(rdram, s, live ? s.object : 0);
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
    // The level is this game's: a party member's level follows the player's
    // EXP (save +0x36E0, the only EXP there is) on its own curve. With theirs
    // written here, every EXP we got levelled the stand-in up again (a LEVEL
    // UP after nearly every kill), and the full heal of each level-up went to
    // them as a heal.
    want[kRecordLevel] = rec[kRecordLevel];
    if (std::memcmp(rec, want.data(), kLoadoutSize) != 0)
        std::memcpy(rec, want.data(), kLoadoutSize);
    s.wroteRecord = want;
    s.inventory.assign(inv, inv + kInventorySize);
}

// FUN_001A8D40(MEMBER*, rule) override. A room's area script picks its
// party with a rule (opcode 15 of its map/battle/event programs): 0 is the
// player alone (Sora- or Roxas-only bosses, the Station of Awakening), 1 the
// player and the world's two companions, 6 the player and the first, others
// mixes (table at [0x01C6CC0C], 4 bytes a rule: 00 player, 01-03 the world's
// companions, 0x12 nobody). While other players are in the session, a rule
// with fewer companions than players becomes the one with the player and
// just the first one or two companions, so the room makes party members for
// them to stand in for as it loads (empty world slots are filled, see
// fillParty). Making one later instead (spawnMissing) put its files in memory
// the pause menu takes back: leaving the menu crashed in its weapon.
std::atomic<int> g_partyWanted{0};
std::atomic<bool> g_ruleOverridden{false}; // this room load's rule was raised (spawnMissing)
PS2Runtime::RecompiledFunction g_partyRule = nullptr;
constexpr uint32_t kPartyRule = 0x001A8D40;
constexpr uint32_t kPartyRules = 0x01C6CC0C;
void partyRuleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int wanted = g_partyWanted.load(std::memory_order_relaxed);
    const uint32_t rule = getRegU32(ctx, 5);
    const uint32_t table = rd<uint32_t>(rdram, kPartyRules);
    if (wanted > 0 && rule != 1 && rule < 16 && table && table < 0x02000000u)
    {
        int companions = 0;
        for (int i = 1; i < 4; ++i)
            companions += rd<uint8_t>(rdram, table + rule * 4 + i) != kNobody ? 1 : 0;
        if (companions < wanted)
        {
            // The rule with the player and exactly the first `wanted` companion slots.
            const uint8_t want[4] = {0x00, 0x01, uint8_t(wanted >= 2 ? 0x02 : kNobody), kNobody};
            uint32_t pick = 1;
            for (uint32_t i = 0; i < 16; ++i)
                if (std::memcmp(rdram + ((table + i * 4) & 0x01FFFFFFu), want, 4) == 0)
                {
                    pick = i;
                    break;
                }
            std::printf("[partner] the room's party rule %u (%d companion(s)) becomes %u for %d player(s)\n", rule,
                        companions, pick, wanted);
            std::fflush(stdout);
            SET_GPR_U32(ctx, 5, pick);
            g_ruleOverridden = true;
        }
    }
    kh2::callOriginal(g_partyRule, rdram, ctx, runtime);
}

// FUN_001C9860(form) override: while linked, forms leave the party alone.
void formMembersHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kFormMembers && g_formsFree.load(std::memory_order_relaxed))
    {
        setReturnU32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_formMembers, rdram, ctx, runtime);
}

void sacrificeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_linked.load(std::memory_order_relaxed) || g_formsFree.load(std::memory_order_relaxed))
    {
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    kh2::callOriginal(g_sacrifice, rdram, ctx, runtime);
}
// A party member's voices come from the voice bank of its character, found
// by its objentry's part number (+0x4C): Donald's or Goofy's for a stand-in.
// Two functions pick them: the objects' "say" method (vtable; attack, jump,
// hurt: 0x176CD0, OBJ*, type, priority) and YS::PARTY::voice_part (0x176D68,
// limits). When the player a stand-in plays is the same character as ours
// (Roxas or Sora), the stand-in is looked up as ours for the length of the
// call: that bank is loaded for our player, and the sound still plays at the
// stand-in. (Another character's bank is not loaded here: Donald's stays.)
PS2Runtime::RecompiledFunction g_voicePart = nullptr, g_voiceSay = nullptr;
constexpr uint32_t kVoicePart = 0x00176D68, kVoiceSay = 0x00176CD0;
void speakAsUs(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    if (obj && (obj == g_standIns[0].load(std::memory_order_relaxed) || obj == g_standIns[1].load(std::memory_order_relaxed)))
    {
        const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
        const uint32_t mine = player && player < 0x02000000u ? rd<uint32_t>(rdram, player + kh2state::addr::ObjEntry) : 0;
        const uint32_t theirs = rd<uint32_t>(rdram, obj + kh2state::addr::ObjEntry);
        // Same character: the stand-in wears our model (its record's model name).
        if (mine && mine < 0x02000000u && theirs && theirs < 0x02000000u &&
            std::memcmp(rdram + (mine & 0x01FFFFFFu) + 8, rdram + (theirs & 0x01FFFFFFu) + 8, 32) == 0)
        {
            static std::atomic<bool> told{false};
            if (!told.exchange(true))
            {
                std::printf("[partner] stand-in %08X speaks with our voice bank\n", obj);
                std::fflush(stdout);
            }
            wr<uint32_t>(rdram, obj + kh2state::addr::ObjEntry, mine);
            kh2::callOriginal(original, rdram, ctx, runtime);
            wr<uint32_t>(rdram, obj + kh2state::addr::ObjEntry, theirs);
            return;
        }
    }
    kh2::callOriginal(original, rdram, ctx, runtime);
}
void voicePartHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { speakAsUs(g_voicePart, rdram, ctx, runtime); }
void voiceSayHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { speakAsUs(g_voiceSay, rdram, ctx, runtime); }
// The party gauge's face (dk::FACE, a dk::Sprite, made by 0x001542C8 from
// the member's own files: FACE, OBJ*, kind). A stand-in in Roxas's or Sora's
// look brings the player's face, drawn at the size of the player's own gauge,
// which covers the party members' HP bars: drawn smaller there. The sprite's
// YI::SEQUENCE (+0x18) has a scale at +0x134 (height), +0x138 (width), +0x13C
// that counts when bit 1 of +0xB8 is set (YI::SEQUENCE scale setters, 0x00239E60).
PS2Runtime::RecompiledFunction g_makeFace = nullptr;
constexpr uint32_t kMakeFace = 0x001542C8;
float faceScale()
{
    static const float scale = [] {
        const char *e = std::getenv("KH2_PARTY_FACE_SCALE");
        const float v = e ? float(std::atof(e)) : 0.7f;
        return v > 0.1f && v <= 1.0f ? v : 0.7f;
    }();
    return scale;
}
void makeFaceHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t face = getRegU32(ctx, 4), obj = getRegU32(ctx, 5), kind = getRegU32(ctx, 6);
    kh2::callOriginal(g_makeFace, rdram, ctx, runtime);
    // A party member (objentry type 1) in Sora's or Roxas's look (P_EX1xx):
    // a stand-in, also before update() has seen it (gauges are made with the room).
    const uint32_t rec = obj && obj < 0x02000000u ? rd<uint32_t>(rdram, obj + kh2state::addr::ObjEntry) : 0;
    if (!rec || rec >= 0x02000000u || rd<uint8_t>(rdram, rec + 4) != 1 ||
        std::memcmp(rdram + ((rec + 8) & 0x01FFFFFFu), "P_EX1", 5) != 0)
        return;
    const float sc = faceScale();
    const uint32_t seq = face + 0x18;
    wr<float>(rdram, seq + 0x134, sc);
    wr<float>(rdram, seq + 0x138, sc);
    wr<float>(rdram, seq + 0x13C, 1.0f);
    wr<uint32_t>(rdram, seq + 0xB8, rd<uint32_t>(rdram, seq + 0xB8) | 2u);
    // The sprite's update (0x00138840) sets the sequence's +0x134 from its own
    // +0x180 every frame (the height): that one too.
    wr<float>(rdram, face + 0x180, sc);
    std::printf("[partner] party face of stand-in %08X drawn at %.0f%% (face %08X, kind %u)\n", obj, sc * 100.0f, face, kind);
    std::fflush(stdout);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    for (auto [address, original, hook] :
         {std::tuple{kVoicePart, &g_voicePart, &voicePartHook}, std::tuple{kVoiceSay, &g_voiceSay, &voiceSayHook},
          std::tuple{kMakeFace, &g_makeFace, &makeFaceHook}})
    {
        *original = runtime.lookupFunction(address);
        if (!*original || !runtime.replaceFunction(address, hook))
        {
            std::fprintf(stderr, "[partner] could not hook 0x%08X\n", address);
            *original = nullptr;
        }
    }
    g_sacrifice = runtime.lookupFunction(kSacrifice);
    if (!g_sacrifice || !runtime.replaceFunction(kSacrifice, &sacrificeHook))
    {
        std::fprintf(stderr, "[partner] could not hook 0x%08X\n", kSacrifice);
        g_sacrifice = nullptr;
        return false;
    }
    g_formMembers = runtime.lookupFunction(kFormMembers);
    if (!g_formMembers || !runtime.replaceFunction(kFormMembers, &formMembersHook))
    {
        std::fprintf(stderr, "[partner] could not hook 0x%08X\n", kFormMembers);
        g_formMembers = nullptr;
    }
    if (const char *e = std::getenv("KH2_TEST_FORMS_FREE"); e && *e == '1')
        g_formsFree = true;
    // On by default; KH2_PARTY_RULE=0 leaves rooms' party rules alone (then
    // spawnMissing makes the members late; see partyRuleHook).
    const char *ruleEnv = std::getenv("KH2_PARTY_RULE");
    g_partyRule = !(ruleEnv && *ruleEnv == '0') ? runtime.lookupFunction(kPartyRule) : nullptr;
    if (g_partyRule && !runtime.replaceFunction(kPartyRule, &partyRuleHook))
    {
        std::fprintf(stderr, "[partner] could not hook 0x%08X\n", kPartyRule);
        g_partyRule = nullptr;
    }
    return true;
}

static void spawnMissing(uint8_t *rdram, const kh2state::State &st, const std::vector<Companion> &comps,
                         const std::vector<const Remote *> &order)
{
    const uint32_t key = uint32_t(st.now.world) << 24 | uint32_t(st.now.room) << 16 | st.now.event;
    if (key != g_spawnRoom)
    {
        g_spawnRoom = key;
        g_roomSince = nowSec();
        g_spawnTried[0] = g_spawnTried[1] = false;
        g_lateRoom[0] = g_lateRoom[1] = false;
        g_lookAsked.clear();
        g_roomOverridden = g_ruleOverridden.exchange(false);
    }
    // On by default; KH2_NO_SPAWN_FRIEND=1 turns it off (diagnostics).
    static const bool always = [] {
        const char *e = std::getenv("KH2_NO_SPAWN_FRIEND");
        return !(e && *e == '1');
    }();
    if (order.empty() || !(g_roomOverridden || always) || !st.player.valid || st.control != 0 || st.now.world == 0 ||
        st.now.world >= 0x20)
    {
        static uint32_t loggedKey = 0;
        if (always && loggedKey != key && !order.empty() && std::getenv("KH2_PUPPET_LOG"))
        {
            loggedKey = key;
            std::printf("[partner] spawnMissing waits: valid %d control %u world %02X\n", int(st.player.valid),
                        unsigned(st.control), unsigned(st.now.world));
            std::fflush(stdout);
        }
        return;
    }
    if (rd<uint32_t>(rdram, kh2state::addr::MapJumpTask))
    {
        g_roomSince = nowSec();
        return;
    }
    if (nowSec() - g_roomSince < 1.5)
        return;
    const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
    if (!player || player >= 0x02000000u)
        return;
    float pos[4], dir[4];
    std::memcpy(pos, rdram + ((player + kh2state::addr::ObjPos) & 0x01FFFFFFu), sizeof(pos));
    std::memcpy(dir, rdram + ((player + kh2state::addr::ObjDir) & 0x01FFFFFFu), sizeof(dir));
    for (size_t k = 1; k <= order.size() && k <= 2; ++k)
    {
        if (g_spawnTried[k - 1])
            continue;
        bool have = false;
        for (const Companion &c : comps)
            have = have || c.slot == int(k);
        const uint8_t character = rd<uint8_t>(rdram, kSaveParty + uint32_t(st.now.world) * 4u + uint32_t(k));
        {
            static uint32_t loggedKey[2] = {0, 0};
            if (loggedKey[k - 1] != key && std::getenv("KH2_PUPPET_LOG"))
            {
                loggedKey[k - 1] = key;
                std::printf("[partner] slot %zu here: have %d character %02X\n", k, int(have), character);
                std::fflush(stdout);
            }
        }
        if (have)
            continue;
        // (Bit 7: a member the story puts there, Ping in the Land of Dragons
        // is 0x83; skipping those left a remade stand-in missing.)
        const uint8_t who = character & 0x7F;
        if (who == kNobody || who == 0 || who >= 13)
            continue;
        // Made in the player's look straight away (no room reload): the
        // entry of that place is dressed first, as fillParty's are.
        const Remote &r = *order[k - 1];
        const uint16_t entry = fillEntry(character);
        if (entry)
        {
            State &s = g_states[r.playerId];
            if (!s.companionEntry)
            {
                s.companionEntry = entry;
                s.companionWeapon = weaponOf(rdram, entry);
            }
            if (r.model && dressedAs(entry) != r.model)
                dress(rdram, entry, r.model);
            if (!r.model || dressedAs(entry) != r.model)
                continue; // their look first (they may still be loading)
        }
        if (!(lookReady(rdram, r.model) & lookReady(rdram, r.weapon)))
            continue; // their look's files first
        g_spawnTried[k - 1] = true;
        g_lateRoom[k - 1] = true;
        {
            State &s = g_states[r.playerId];
            const uint16_t ours = localWeaponEntry(rdram);
            if (s.companionWeapon && ours)
            {
                dress(rdram, s.companionWeapon, ours);
                std::printf("[partner] party member %zu is made late here: it holds our weapon (theirs would load into "
                            "memory the pause menu reuses)\n",
                            k);
                std::fflush(stdout);
            }
        }
        // Behind the player, to the left (slot 1) or right (slot 2).
        const float side = k == 1 ? -1.0f : 1.0f;
        const float back = 150.0f, aside = 110.0f;
        const float at[3] = {pos[0] - dir[0] * back + dir[2] * aside * side, pos[1],
                             pos[2] - dir[2] * back - dir[0] * aside * side};
        const uint32_t placeholder = kFriendPlaceholder[k - 1];
        std::printf("[partner] this room has no place for party member %zu: making one behind the player\n", k);
        std::fflush(stdout);
        g_made[k - 1] = 0;
        kh2puppets::spawnAt(placeholder, at, dir[3], [placeholder, k](uint32_t obj) {
            g_made[k - 1] = obj;
            std::printf("[partner] placeholder %04X made -> %08X\n", placeholder, obj);
            std::fflush(stdout);
        });
    }
}

// A party member's voices (attack, jump, hurt) play from the voice bank its
// voice context names (object +0x9B8 -> context, +0x1C: a slot of the bank
// table at 0x01C94090, 12 bytes each: flags, part number, variant), chosen
// when it was made, by its part number: Donald's for a stand-in. When the
// stand-in is the same character as our player (it wears our model), it
// speaks from our player's bank instead, which is loaded here.
static void useOurVoices(uint8_t *rdram, uint32_t obj)
{
    constexpr uint32_t kVoiceCtx = 0x9B8, kCtxBank = 0x1C, kBanks = 0x01C94090;
    const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
    if (!player || player >= 0x02000000u)
        return;
    const uint32_t mine = rd<uint32_t>(rdram, player + kh2state::addr::ObjEntry);
    const uint32_t theirs = rd<uint32_t>(rdram, obj + kh2state::addr::ObjEntry);
    if (!mine || mine >= 0x02000000u || !theirs || theirs >= 0x02000000u ||
        std::memcmp(rdram + (mine & 0x01FFFFFFu) + 8, rdram + (theirs & 0x01FFFFFFu) + 8, 32) != 0)
        return;
    const uint32_t ctx = rd<uint32_t>(rdram, obj + kVoiceCtx);
    if (!ctx || ctx >= 0x02000000u)
        return;
    const uint32_t part = rd<uint16_t>(rdram, mine + 0x4C);
    for (uint32_t slot = 0; slot < 3; ++slot)
        if (rd<uint32_t>(rdram, kBanks + slot * 12 + 4) == part)
        {
            if (rd<uint32_t>(rdram, ctx + kCtxBank) != slot)
            {
                wr<uint32_t>(rdram, ctx + kCtxBank, slot);
                std::printf("[partner] stand-in %08X speaks from our voice bank (slot %u, part %u)\n", obj, slot, part);
                std::fflush(stdout);
            }
            return;
        }
}

namespace
{
// ---- riding (skateboards) ------------------------------------------------
//
// Hopping on a board attaches the board to the player (YS::OBJ::attach,
// 0x001C0D38: object +0x570 the parent, +0x574 joint, +0x576 flags), and the
// rider's motions come from the board's motion set: YS::MOTION +0xF0 the
// board's axes (board +0x80), +0xFC the board, +0x100 its motion set; a
// motion change with those set (0x00180C88) "captures" the rider's motion
// from the board's set (YS::MOTION::Capture, entries 252.. there; the
// rider's own set has no such motions). The board plays its own (151..).
// A stand-in rides our game's board of the same room serial the same way.
constexpr uint32_t kAttach = 0x001C0D38, kDetach = 0x001C0E60, kCaptureChange = 0x00180C88;
constexpr uint32_t kObjParent = 0x570, kObjSpawnRecord = 0x81C, kRecordSerial = 0x1E;
constexpr uint32_t kMotCaptureAxes = 0xF0, kMotCaptureObj = 0xFC, kMotCaptureSet = 0x100, kObjAxes = 0x80;

static int16_t spawnSerial(const uint8_t *rdram, uint32_t obj)
{
    const uint32_t rec = rd<uint32_t>(rdram, obj + kObjSpawnRecord);
    return rec >= 0x00100000u && rec < 0x02000000u ? rd<int16_t>(rdram, rec + kRecordSerial) : int16_t(-1);
}

static uint32_t findRide(const uint8_t *rdram, uint16_t entry, int16_t serial)
{
    if (serial < 0)
        return 0;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.entryId == entry && spawnSerial(rdram, a.address) == serial)
            return a.address;
    return 0;
}

static void setCapture(uint8_t *rdram, uint32_t obj, uint32_t board)
{
    const uint32_t m = obj + kh2state::addr::ObjMotion;
    wr<uint32_t>(rdram, m + kMotCaptureAxes, board ? board + kObjAxes : 0);
    wr<uint32_t>(rdram, m + kMotCaptureObj, board);
    wr<uint32_t>(rdram, m + kMotCaptureSet, board ? rd<uint32_t>(rdram, board + kh2state::addr::ObjMotion) : 0);
}

// The stand-in gets off its board (or the board went with its room).
void stopRiding(uint8_t *rdram, State &s, uint32_t obj)
{
    if (!s.ride)
        return;
    bool live = false;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        live = live || a.address == s.ride;
    if (live)
    {
        if (obj && rd<uint32_t>(rdram, s.ride + kObjParent) == obj)
            kh2guest::postCall({kDetach, {s.ride}, {}});
        const uint32_t flags = rd<uint32_t>(rdram, s.ride + kObjFlags);
        wr<uint32_t>(rdram, s.ride + kObjFlags, (flags & ~kNoAct) | (s.rideFlagsBefore & kNoAct));
    }
    if (obj)
        setCapture(rdram, obj, 0);
    std::printf("[partner] stand-in %08X gets off %08X\n", obj, s.ride);
    std::fflush(stdout);
    s.ride = 0;
    s.motion = 0xFFFF; // its own motions again
}

// Drives a stand-in whose player rides `board` (ours); false: not riding.
static bool ride(uint8_t *rdram, State &s, uint32_t obj, const Remote &r)
{
    const uint32_t board = findRide(rdram, r.rideEntry, r.rideSerial);
    if (board != s.ride)
        stopRiding(rdram, s, obj);
    if (!board)
        return false;
    if (!s.ride)
    {
        s.ride = board;
        s.rideFlagsBefore = rd<uint32_t>(rdram, board + kObjFlags);
        s.motion = 0xFFFF;
        s.sinceRideMotion = 100;
        std::printf("[partner] stand-in %08X rides %08X (entry %04X #%d)\n", obj, board, r.rideEntry, r.rideSerial);
        std::fflush(stdout);
    }
    // The board is ours to move while they ride it (its own routine off).
    wr<uint32_t>(rdram, board + kObjFlags, rd<uint32_t>(rdram, board + kObjFlags) | kNoAct);
    if (rd<uint32_t>(rdram, board + kObjParent) != obj)
        kh2guest::postCall({kAttach, {board, obj, r.rideJoint, r.rideFlags}, {}});
    setCapture(rdram, obj, board);
    const uint32_t playing = rd<uint32_t>(rdram, obj + kh2state::addr::ObjMotionIndex);
    ++s.sinceMotion;
    if (r.motion != s.motion || (playing != r.motion && s.sinceMotion > 4))
    {
        s.motion = r.motion;
        s.sinceMotion = 0;
        kh2guest::postCall({kCaptureChange, {obj + kh2state::addr::ObjMotion, r.motion}, {4.0f, 0.0f}});
    }
    ++s.sinceRideMotion;
    if (rd<uint32_t>(rdram, board + kh2state::addr::ObjMotionIndex) != r.rideMotion && s.sinceRideMotion > 4)
    {
        s.sinceRideMotion = 0;
        kh2puppets::postMotion(board, r.rideMotion, 4.0f);
    }
    return true;
}

} // namespace

std::vector<Heal> update(uint8_t *rdram, const std::vector<Remote> &remotes)
{
    std::vector<Heal> heals;
    std::vector<kh2puppets::Override> overrides;
    std::vector<std::pair<uint32_t, int32_t>> guards;
    std::lock_guard<std::mutex> lock(g_mutex);
    const kh2state::State st = kh2state::read(rdram);
    const std::vector<Companion> comps = companions(rdram);

    // KH2_FORM_LOOKS=0: a player in a drive form shows in their own look
    // here (their weapon: the one they had before the form). By default the
    // stand-in takes the form's look, once its files are loaded (lookReady).
    std::vector<Remote> shown(remotes.begin(), remotes.end());
    for (Remote &r : shown)
    {
        static const bool formLooks = [] {
            const char *e = std::getenv("KH2_FORM_LOOKS");
            return !(e && *e == '0');
        }();
        const uint16_t base = formLooks ? 0 : baseLook(rdram, r.model);
        if (base && base != r.model)
        {
            r.model = base;
            auto w = g_baseWeapon.find(r.playerId);
            r.weapon = w == g_baseWeapon.end() ? 0 : w->second;
        }
        else if (r.weapon && r.model)
            g_baseWeapon[r.playerId] = r.weapon;
    }
    // Remotes in id order take companion slots 1 and 2.
    std::vector<const Remote *> order;
    for (const Remote &r : shown)
        order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const Remote *a, const Remote *b) { return a->playerId < b->playerId; });
    if (order.size() > 2)
        order.resize(2);
    // The World Map (world 0F) has no party and nobody walks there: no
    // stand-ins, fills or looks. With Donald put in its party slot, the host
    // stopped at the map's new-world event (Hollow Bastion now open) for good.
    if (st.now.world == kWorldMap)
        order.clear();
    g_partyWanted = int(order.size());
    if (st.player.valid)
        fillParty(rdram, st.now.world, order.size());
    spawnMissing(rdram, st, comps, order);
    // The room's own party member can turn up after spawnMissing gave up
    // waiting for it (a slow load): two in one place. Ours goes.
    for (int k = 1; k <= 2; ++k)
    {
        const uint32_t made = g_made[k - 1].load();
        if (!made)
            continue;
        bool ours = false, other = false;
        for (const Companion &c : comps)
            if (c.slot == k)
                (c.object == made ? ours : other) = true;
        if (!ours)
        {
            bool live = false;
            for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
                live = live || a.address == made;
            if (!live)
                g_made[k - 1] = 0; // gone with its room
            continue;
        }
        if (other)
        {
            std::printf("[partner] the room made party member %d after all: taking ours (%08X) away\n", k, made);
            std::fflush(stdout);
            g_made[k - 1] = 0;
            removeWithWeapons(rdram, made);
        }
    }

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
        const uint16_t ourWeapon =
            s.slot >= 1 && s.slot <= 2 && g_lateRoom[s.slot - 1] ? localWeaponEntry(rdram) : uint16_t(0);
        if (s.companionWeapon && ourWeapon)
            dress(rdram, s.companionWeapon, ourWeapon);
        else if (s.companionWeapon && r.weapon && r.model && dressedAs(s.companionEntry) == r.model)
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
                static bool testOldLook = std::getenv("KH2_TEST_OLD_LOOK") != nullptr; // testing: the first one counts as old
                if (testOldLook && lookNow)
                {
                    testOldLook = false;
                    s.lookAtCreation = 0;
                }
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
        // A battle's own short cinematic (a reaction command, a finisher:
        // its timer runs while the player still has control) is staged for
        // the player alone: the stand-ins step out of it, and back after.
        const bool cinematic = st.control == 0 && st.cutsceneLength != 0 && st.cutsceneLength != 0xFFFFFFFFu;
        static const bool kHide = std::getenv("KH2_HIDE_STAND_INS") != nullptr; // diagnostics
        const bool away = obj && s.lookAtCreation && (!here || cinematic || kHide);
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
        if (s.ride && !(here && !cinematic && s.dressedAtCreation))
            stopRiding(rdram, s, obj);
        if (here && !cinematic)
        {
            kh2puppets::Override o;
            o.object = obj;
            std::memcpy(o.pos, r.pos, sizeof(o.pos));
            o.yaw = r.yaw;
            keepApart(st, r.playerId, o.pos);
            overrides.push_back(o);
            // Their motions only make sense on their own motion set. Also put
            // theirs back when this game started another (a hit reaction).
            const bool riding = s.dressedAtCreation && ride(rdram, s, obj, r);
            const uint32_t playing = rd<uint32_t>(rdram, obj + kh2state::addr::ObjMotionIndex);
            if (!riding)
                ++s.sinceMotion;
            // A player who has stood still since their room loaded has no
            // motion index yet (0xFFFF): they are idle (0). Left as it was,
            // the stand-in kept the pose it was made in (arms out).
            const uint32_t motion = r.motion == 0xFFFFu ? 0u : r.motion;
            if (s.dressedAtCreation)
            {
                guards.emplace_back(obj, static_cast<int32_t>(motion)); // its own AI's motions kept off
                useOurVoices(rdram, obj);
            }
            if (!riding && s.dressedAtCreation && (motion != s.motion || (playing != motion && s.sinceMotion > 4)))
            {
                static const bool kLog = std::getenv("KH2_PUPPET_LOG") != nullptr;
                if (kLog)
                {
                    std::printf("[partner] stand-in %08X motion %u (sent %u; playing %u, pose %08X)\n", obj, motion,
                                unsigned(r.motion), playing,
                                rd<uint32_t>(rdram, obj + kh2state::addr::ObjMotion + 0xC));
                    std::fflush(stdout);
                }
                s.motion = motion;
                s.sinceMotion = 0;
                kh2puppets::postMotion(obj, motion, 4.0f);
            }
        }

        // Stats: their HP and MP in the companion's party slot; what this game
        // added since we wrote it is a heal for them, and what it took (our
        // enemies hitting their stand-in) is damage for them.
        if (c && r.maxHp > 0)
        {
            // The stand-in's own sheet: by slot it can be someone else's (a
            // boss that took a party sheet, as Twilight Thorn takes slot 1's).
            uint32_t base = slotAddress(s.slot);
            const uint32_t sheet = rd<uint32_t>(rdram, c->object + kObjSheet);
            if (sheet && sheet <= kh2state::addr::Slot1 && (kh2state::addr::Slot1 - sheet) % kh2state::addr::SlotStride == 0)
                base = sheet;
            const int32_t hpNow = rd<int32_t>(rdram, base + kh2state::addr::SlotHp);
            const int32_t mpNow = rd<int32_t>(rdram, base + kh2state::addr::SlotMp);
            Heal h;
            h.playerId = r.playerId;
            // A level-up of the stand-in's character here fills its HP and MP:
            // that is no heal for them.
            const int level = s.character >= 0 ? rd<uint8_t>(rdram, kSaveChars + uint32_t(s.character) * kLoadoutSize +
                                                                         uint32_t(kRecordLevel))
                                               : -1;
            const bool levelled = level >= 0 && s.level >= 0 && level != s.level;
            s.level = level;
            if (s.writtenHp >= 0 && hpNow != s.writtenHp && hpNow >= 0 && !(levelled && hpNow > s.writtenHp))
                h.hp = hpNow - s.writtenHp;
            if (s.writtenMp >= 0 && mpNow > s.writtenMp && !levelled)
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

        // In our room in the old look (made before their entry wore their
        // look, or they changed form): it goes, and spawnMissing makes the
        // party member again, in their look now. Reloading the room for it
        // put everyone back at the door for a moment.
        if (here && !s.dressedAtCreation && r.model && dressedAs(s.companionEntry) == r.model && !s.refreshAsked &&
            lookReady(rdram, r.model) & lookReady(rdram, r.weapon))
        {
            s.refreshAsked = true;
            if (s.slot >= 1 && s.slot <= 2)
                g_spawnTried[s.slot - 1] = false;
            std::printf("[partner] player %u's stand-in %08X is in an old look: making it again\n", r.playerId + 1, obj);
            std::fflush(stdout);
            removeWithWeapons(rdram, obj);
        }

        Link l;
        l.playerId = r.playerId;
        l.slot = s.slot;
        l.object = here ? obj : 0;
        l.entry = s.companionEntry;
        l.dressed = s.dressedAtCreation;
        g_links.push_back(l);
    }
    g_linked = !g_links.empty();
    {
        uint32_t ids[2] = {0, 0};
        size_t n = 0;
        for (const Link &l : g_links)
            if (l.object && l.dressed && n < 2)
                ids[n++] = l.object;
        g_standIns[0] = ids[0];
        g_standIns[1] = ids[1];
    }
    if (g_fillRefresh)
    {
        // The room again, with the new party members, once their entries
        // wear the players' looks (a player still loading has none yet).
        bool dressed = true;
        for (const auto &[id, st2] : g_states)
            if (!st2.object && st2.companionEntry && !dressedAs(st2.companionEntry))
                dressed = false;
        if (dressed)
            g_fillRefresh = false; // (spawnMissing makes them, no room reload)
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
        // (Nobody to stand in for: names are cleared once and then left to
        // whoever else sets them, the avatars.)
        static bool hadNames = false;
        if (!names.empty() || hadNames)
            kh2names::set(names);
        hadNames = !names.empty();
    }
    return heals;
}

void holdFill(bool on) { g_fillHeld = on; }

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

void localRide(const uint8_t *rdram, kh2net::PlayerState &ps)
{
    const uint32_t player = rd<uint32_t>(rdram, kh2state::addr::PlayerPtr);
    if (!player || player >= 0x02000000u)
        return;
    for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
        if (a.address != player && rd<uint32_t>(rdram, a.address + kObjParent) == player)
        {
            const int16_t serial = spawnSerial(rdram, a.address);
            if (serial < 0)
                continue;
            ps.rideSerial = serial;
            ps.rideEntry = uint16_t(a.entryId);
            ps.rideJoint = rd<uint16_t>(rdram, a.address + 0x574);
            ps.rideFlags = rd<uint16_t>(rdram, a.address + 0x576);
            ps.rideMotion = uint16_t(rd<uint32_t>(rdram, a.address + kh2state::addr::ObjMotionIndex));
            return;
        }
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

void setFormsFree(bool free)
{
    static const bool test = [] {
        const char *e = std::getenv("KH2_TEST_FORMS_FREE");
        return e && *e == '1';
    }();
    g_formsFree = free || test;
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
