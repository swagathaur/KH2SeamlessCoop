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
#include "guest_call.h"
#include "hle_call.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include <atomic>

namespace kh2avatars
{
namespace
{
constexpr uint32_t kNoAct = 0x200000u;
constexpr uint32_t kObjFlags = 0x10C, kObjSheet = 0x48C;
constexpr uint32_t kSheetBase = 0x01E4E000u, kSheetSize = 0x268, kMaxAvatars = 3;
constexpr uint32_t kEntrySize = 0x60, kEntryLook = 0x08;
constexpr uint32_t kFriendTable = 0x01C60528; // YS::FRIEND::Get(i): the party members' objects
constexpr uint32_t kObjModelData = 0x7AC; // 0 until the model's files are in
constexpr uint32_t kObjModel = 0x670, kModelParts = 0x104, kWeaponOwner = 0x570;
// The game's pointers to its player that a new player object takes over.
constexpr uint32_t kPlayerRefs[] = {0x00348638, 0x00348754, 0x0034ED04, 0x00352400, 0x01C5FF10,
                                    0x01C60018, 0x01C60024, 0x01C6C9AC, kh2state::addr::PlayerPtr};

constexpr uint32_t kHud = 0x003485EC;

void removeAvatar(uint8_t *rdram, uint32_t obj);

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
    double quietUntil = 0;   // no heals read until then (an avatar made or gone: the slot is set anew)
    double madeAt = 0;       // when it was made (prepared)
    bool plainLook = false;  // their form's look did not load here: their own look instead (this room)
    bool hiddenForCut = false; // hidden while a cutscene plays
};
std::mutex g_mutex; // g_states (spawn callbacks run on the EE thread)
std::map<uint8_t, State> g_states;
uint32_t g_gen = 0;
uint16_t g_room = 0xFFFF;
double g_roomSince = 0;
uint32_t g_ourPlayer = 0;
std::atomic<uint32_t> g_ourPlayerShared{0};
std::atomic<uint32_t> g_avatarObjs[kMaxAvatars] = {}; // for the EE-side hooks
std::atomic<bool> g_avatarPending{false}; // an avatar's spawn job is under way

// YS::PLAYER::_OVR_control (0x00178888): a player's per-frame command menu
// update (the command object at +0xB3C, which every player object points
// at: there is one). An avatar ran it too, and our button presses went
// through the command menu twice.
constexpr uint32_t kPlayerControl = 0x00178888;
PS2Runtime::RecompiledFunction g_control = nullptr;
void controlHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    for (const auto &a : g_avatarObjs)
        if (obj && a.load(std::memory_order_relaxed) == obj && ctx->pc == kPlayerControl)
        {
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    kh2::callOriginal(g_control, rdram, ctx, runtime);
}

// A player-class object's teardown (0x00178280, the vtable's; other classes'
// tail-jump to it) clears the game's player pointer (0x0034E948) and runs
// FUN_001781E8: resets for the player (the battle state among them) and the
// release of its command object (+0xB3C: for an avatar, the one shared
// PLAYER_COMMAND). An avatar going (a new look, a cutscene) took our player
// and the fight's state with it. An avatar's goes straight to the base
// object's teardown (0x00175928).
constexpr uint32_t kPlayerTeardown = 0x00178280, kObjTeardown = 0x00175928;
PS2Runtime::RecompiledFunction g_teardown = nullptr;
std::atomic<uint32_t> g_leaving[4] = {}; // avatars removeAvatar sent off (no longer in g_avatarObjs)
std::atomic<uint32_t> g_leavingNext{0};
std::atomic<uint32_t> g_torn[8] = {}; // avatars torn down (by the game or us): not to be sent off again
std::atomic<int64_t> g_tornAt[8] = {};  // when (ms): an address is only theirs for a moment (memory is reused)
std::atomic<uint32_t> g_tornNext{0};
std::atomic<int64_t> g_leavingAt[4] = {};
std::atomic<int64_t> g_avatarTornAt{-1000}; // (ms) an avatar's teardown: its releases follow at once
int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
// Was `obj` an avatar that left (or is leaving) within the last two seconds?
bool justLeft(uint32_t obj)
{
    const int64_t t = nowMs();
    for (int i = 0; i < 8; ++i)
        if (obj && g_torn[i].load() == obj && t - g_tornAt[i].load() < 2000)
            return true;
    for (int i = 0; i < 4; ++i)
        if (obj && g_leaving[i].load() == obj && t - g_leavingAt[i].load() < 2000)
            return true;
    return false;
}
// Out of the game's lists of players (0x01D48DA0, and the one it moves
// leaving players to, 0x01D48DA8: head, tail; next at +0x8B4). An avatar
// is no player of this game: in the list, a drive check saw a player in a
// form (an avatar in theirs) and Drive was greyed out; and the player
// teardown we skip would have taken it out (an event start walked the list
// into a freed avatar).
bool unlinkPlayer(uint8_t *rdram, uint32_t obj)
{
    bool found = false;
    for (uint32_t list : {0x01D48DA0u, 0x01D48DA8u})
    {
        uint32_t prev = 0, cur = 0;
        std::memcpy(&cur, rdram + list, 4);
        for (int guard = 0; cur && guard < 64; ++guard)
        {
            uint32_t next = 0;
            std::memcpy(&next, rdram + ((cur + 0x8B4) & 0x01FFFFFFu), 4);
            if (cur == obj)
            {
                if (prev)
                    std::memcpy(rdram + ((prev + 0x8B4) & 0x01FFFFFFu), &next, 4);
                else
                    std::memcpy(rdram + list, &next, 4);
                uint32_t tail = 0;
                std::memcpy(&tail, rdram + list + 4, 4);
                if (tail == obj)
                    std::memcpy(rdram + list + 4, &prev, 4);
                // (Its own next is left as it is: the game may be walking
                // the list as it kills objects (a cutscene's start kills
                // the players: ours too, to remake it with the event's
                // files). A next of 0 here ended that walk at the avatar,
                // our player lived on with its motion file refilled under
                // it, and crashed at 0x0017FE84.)
                found = true;
                break;
            }
            prev = cur;
            cur = next;
        }
    }
    return found;
}

void teardownHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    bool avatar = false;
    for (const auto &a : g_avatarObjs)
        avatar = avatar || (obj && a.load(std::memory_order_relaxed) == obj);
    for (int i = 0; i < 4; ++i)
        avatar = avatar || (obj && g_leaving[i].load() == obj && nowMs() - g_leavingAt[i].load() < 5000);
    if (avatar && ctx->pc == kPlayerTeardown)
    {
        {
            const uint32_t i = g_tornNext.fetch_add(1) % 8;
            g_tornAt[i].store(nowMs());
            g_torn[i].store(obj);
        }
        std::printf("[avatars] %08X is torn down as an object (not as the player)\n", obj);
        std::fflush(stdout);
        unlinkPlayer(rdram, obj);
        // Its motion (+0x140) lets go of nothing: its animation instances
        // (motion +0x108, 8 of them, 8 apart), the motion set's refcount
        // (+0x148) and its callback (+0x194) are the ones our player's
        // motion uses when the avatar wears our look; the motion teardown
        // (FUN_00180CE0) released them and our player's animation was gone
        // (a crash at 0x0017FE84). A few bytes are left behind instead.
        {
            const uint32_t motion = (obj + 0x140) & 0x01FFFFFFu;
            const uint32_t zero = 0;
            for (uint32_t i = 0; i < 8; ++i)
                std::memcpy(rdram + motion + 0x108 + i * 8, &zero, 4);
            std::memcpy(rdram + motion + 0x148, &zero, 4);
            std::memcpy(rdram + motion + 0x194, &zero, 4);
            // (and the chain FUN_00180EF0 marks, +0x18C/+0x190: its entries
            // are in the motion file, which a cutscene starting has just
            // filled anew for our player)
            std::memcpy(rdram + motion + 0x18C, &zero, 4);
            std::memcpy(rdram + motion + 0x190, &zero, 4);
        }
        g_avatarTornAt = nowMs();
        ctx->pc = kObjTeardown; // (a0 and ra as they are: a tail call)
        return;
    }
    kh2::callOriginal(g_teardown, rdram, ctx, runtime);
}


// Orbs: a prize's effect (FUN_001A52A8, with its type's amounts) gives HP
// through the object's add_hp (0x00187520) from 0x001A5308, to every party
// member: also to the one holding a remote player's place. An orb heals only
// whoever picked it up (their HP comes over as usual), so what an orb gave
// the place here is no heal for them. Potions and Cure come by the sheet's
// own heal (from 0x001A1980) and are.
constexpr uint32_t kAddHp = 0x00187520, kOrbHeal = 0x001A5308;
PS2Runtime::RecompiledFunction g_addHp = nullptr;
std::atomic<uint32_t> g_orbSheet[4] = {};
std::atomic<int32_t> g_orbHp[4] = {};
void addHpHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int32_t amount = static_cast<int32_t>(getRegU32(ctx, 5));
    if (ctx->pc == kAddHp && amount > 0 && getRegU32(ctx, 31) == kOrbHeal)
    {
        uint32_t sheet = 0;
        std::memcpy(&sheet, rdram + ((getRegU32(ctx, 4) + 0x48C) & 0x01FFFFFFu), 4);
        if (sheet)
            for (int i = 0; i < 4; ++i)
            {
                uint32_t cur = g_orbSheet[i].load();
                if (cur == sheet || (cur == 0 && g_orbSheet[i].compare_exchange_strong(cur, sheet)) || cur == sheet)
                {
                    g_orbHp[i].fetch_add(amount);
                    break;
                }
            }
    }
    kh2::callOriginal(g_addHp, rdram, ctx, runtime);
}
// What orbs gave `sheet` since last asked.
int32_t takeOrbHp(uint32_t sheet)
{
    for (int i = 0; i < 4; ++i)
        if (g_orbSheet[i].load() == sheet)
            return g_orbHp[i].exchange(0);
    return 0;
}


// A file's last reference: YS::CACHE_BUFF::Free (0x00170950) drops the count
// (u16 at +2) of the file's cache header (list at *0x0034DCA0: data at +0x30,
// next at +0x3C). An avatar in our player's look uses our player's files
// without a reference of its own (they were there already), and its
// teardown gave one back: the motion file our player still used was freed,
// read over by the next load, and our player's animation was gone (a crash
// at 0x0017FE84 at a mission's end). The last reference to the file our
// player's motion uses is not given back.
constexpr uint32_t kCacheFree = 0x00170950, kCacheHeaders = 0x0034DCA0;
PS2Runtime::RecompiledFunction g_cacheFree = nullptr;
std::atomic<uint32_t> g_ourPlayerForFree{0};
void cacheFreeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Within an avatar's teardown (it runs at once after teardownHook, on
    // this thread): the avatar wears a look whose files our player (or the
    // room) had already; it holds no references of its own to give back.
    const int64_t sinceTeardown = nowMs() - g_avatarTornAt.load(std::memory_order_relaxed);
    if (ctx->pc == kCacheFree && sinceTeardown >= 0 && sinceTeardown < 20)
    {
        static int logged = 0;
        if (logged++ < 20)
        {
            std::printf("[avatars] an avatar's teardown gives back file %08X: kept (not theirs)\n", getRegU32(ctx, 4));
            std::fflush(stdout);
        }
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    // (Diagnostics: the frees for a few seconds after an avatar's teardown.)
    if (ctx->pc == kCacheFree && sinceTeardown >= 0 && sinceTeardown < 4000)
    {
        const uint32_t a0 = getRegU32(ctx, 4);
        uint32_t h = 0;
        std::memcpy(&h, rdram + kCacheHeaders, 4);
        char name[40] = {};
        uint16_t refs = 0;
        uint32_t found = 0;
        for (int i = 0; i < 400 && h >= 0x00100000u && h < 0x02000000u; ++i)
        {
            uint32_t data = 0, next = 0;
            std::memcpy(&data, rdram + h + 0x30, 4);
            std::memcpy(&next, rdram + h + 0x3C, 4);
            if (h == a0 || data == a0)
            {
                found = h;
                std::memcpy(&refs, rdram + h + 2, 2);
                std::memcpy(name, rdram + h + 4, 39);
                break;
            }
            h = next;
        }
        std::printf("[avatars] free %08X (header %08X %s refs %u) from %08X, %lld ms after a teardown\n", a0, found, name,
                    unsigned(refs), getRegU32(ctx, 31), static_cast<long long>(sinceTeardown));
        std::fflush(stdout);
    }
    kh2::callOriginal(g_cacheFree, rdram, ctx, runtime);
}


// A mission's end: its script calls trap_mission_complete (0x0019FFE8) and
// then plays its ending, which swaps our player's motions. An avatar torn
// down as that ending starts (by the game or by us) took our player's
// animation with it (a crash at 0x0017FE84). Avatars go at the call, in
// play, and stay away until the ending is over.
constexpr uint32_t kMissionComplete = 0x0019FFE8;
PS2Runtime::RecompiledFunction g_missionComplete = nullptr;
std::atomic<int64_t> g_missionEndAt{-100000};
void missionCompleteHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kMissionComplete)
    {
        g_missionEndAt = nowMs();
        std::printf("[avatars] the mission is complete: avatars away until its ending is over\n");
        std::fflush(stdout);
    }
    kh2::callOriginal(g_missionComplete, rdram, ctx, runtime);
}


// The party gauge's face: dk::FACE::getFaceSed asks the party member for a
// face image from its own files (FUN_0016C060(obj, ...): obj +0x7A0's file,
// its face entries). Asked of a place holder, the avatar holding that place
// answers instead: the other player's face, not Ping's or Goofy's.
constexpr uint32_t kObjFace = 0x0016C060;
PS2Runtime::RecompiledFunction g_objFace = nullptr;
std::atomic<uint32_t> g_faceProxy[kMaxAvatars] = {}, g_faceAvatar[kMaxAvatars] = {};
void objFaceHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    // Asked of an avatar itself: the HUD's own gauge, while the HUD pointed
    // at a new avatar for a moment (one in a drive form gave us its face).
    // Ours answers.
    bool ownGauge = false;
    if (ctx->pc == kObjFace && obj)
        for (uint32_t i = 0; i < kMaxAvatars; ++i)
            if (g_faceAvatar[i].load(std::memory_order_relaxed) == obj || g_avatarObjs[i].load(std::memory_order_relaxed) == obj)
            {
                if (const uint32_t ours = g_ourPlayerShared.load())
                    SET_GPR_U32(ctx, 4, ours);
                ownGauge = true;
                break;
            }
    // (One still being made is not listed yet: while one is, any
    // player-class object but ours is it.)
    if (ctx->pc == kObjFace && obj && !ownGauge && g_avatarPending.load(std::memory_order_relaxed))
    {
        const uint32_t ours = g_ourPlayerShared.load();
        uint32_t entry = 0;
        std::memcpy(&entry, rdram + ((obj + kh2state::addr::ObjEntry) & 0x01FFFFFFu), 4);
        if (ours && obj != ours && entry >= 0x00100000u && entry < 0x02000000u && rdram[entry + 4] == 0)
        {
            bool proxy = false;
            for (uint32_t i = 0; i < kMaxAvatars; ++i)
                proxy = proxy || g_faceProxy[i].load(std::memory_order_relaxed) == obj;
            if (!proxy)
            {
                SET_GPR_U32(ctx, 4, ours);
                ownGauge = true;
            }
        }
    }
    if (ctx->pc == kObjFace && obj && !ownGauge)
        for (uint32_t i = 0; i < kMaxAvatars; ++i)
            if (g_faceProxy[i].load(std::memory_order_relaxed) == obj)
            {
                const uint32_t avatar = g_faceAvatar[i].load(std::memory_order_relaxed);
                if (avatar)
                    SET_GPR_U32(ctx, 4, avatar);
                break;
            }
    kh2::callOriginal(g_objFace, rdram, ctx, runtime);
}

// The friend gauge reads its face once, when a party member is first set on
// it (FUN_00155300 -> FUN_001542C8, guarded by gauge +0x15A2). An avatar is
// made a few seconds after that, so the gauge is told again once it exists:
// the flag is cleared and the setter called, and the face is asked of the
// place holder (objFaceHook answers with the avatar's).
constexpr uint32_t kFriendGaugeSet = 0x00155300;
PS2Runtime::RecompiledFunction g_friendGaugeSet = nullptr;
constexpr uint32_t kMaxGauges = 4;
std::atomic<uint32_t> g_friendGauges[kMaxGauges] = {};
void friendGaugeSetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gauge = getRegU32(ctx, 4);
    if (ctx->pc == kFriendGaugeSet && gauge)
    {
        bool known = false;
        for (auto &g : g_friendGauges)
            known = known || g.load() == gauge;
        if (!known)
            for (auto &g : g_friendGauges)
            {
                uint32_t empty = 0;
                if (g.compare_exchange_strong(empty, gauge))
                    break;
            }
    }
    kh2::callOriginal(g_friendGaugeSet, rdram, ctx, runtime);
}
// Re-set every friend gauge showing `proxy`, so its face is read again.
void refreshFace(uint8_t *rdram, uint32_t proxy)
{
    if (!g_friendGaugeSet || !proxy)
        return;
    for (auto &g : g_friendGauges)
    {
        const uint32_t gauge = g.load();
        uint32_t shown = 0;
        if (gauge)
            std::memcpy(&shown, rdram + ((gauge + 0x34) & 0x01FFFFFFu), 4);
        if (!gauge || shown != proxy)
            continue;
        std::printf("[avatars] gauge %08X shows %08X: its face is read again\n", gauge, proxy);
        std::fflush(stdout);
        kh2guest::post([gauge, proxy](const kh2guest::Env &env, uint32_t, bool first) -> std::optional<kh2guest::Call> {
            uint32_t now = 0;
            std::memcpy(&now, env.host(gauge + 0x34), 4);
            if (!first || now != proxy)
                return std::nullopt;
            env.rdram[(gauge + 0x15A2) & 0x01FFFFFFu] = 0;
            return kh2guest::Call{kFriendGaugeSet, {gauge}, {}};
        });
    }
}

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

// Hide an object the game's way: its vtable +0x44 (what the event scripts'
// trap_obj_hide calls; YS::OBJ::is_hide then sees 0x80 or 0x100 at +0x10C).
// (A party member's +0x670 is not a ModelObj as a player's is: writing a
// parts mask there wrote into its mesh, and Ping showed as a head.)
std::map<uint32_t, double> g_lastHideAsk; // per object
void show(uint8_t *rdram, uint32_t obj)
{
    const uint32_t vt = inRam(obj) ? rd<uint32_t>(rdram, obj) : 0;
    const uint32_t fn = inRam(vt) ? rd<uint32_t>(rdram, vt + 0x48) : 0;
    if (inRam(fn))
        kh2guest::postCall(kh2guest::Call{fn, {obj}, {}});
}
void hide(uint8_t *rdram, uint32_t obj, double t)
{
    if (rd<uint32_t>(rdram, obj + kObjFlags) & 0x180u)
        return; // hidden
    double &last = g_lastHideAsk[obj];
    if (t - last < 0.5)
        return;
    last = t;
    const uint32_t vt = rd<uint32_t>(rdram, obj);
    const uint32_t fn = inRam(vt) ? rd<uint32_t>(rdram, vt + 0x44) : 0;
    if (!inRam(fn))
        return;
    kh2guest::postCall(kh2guest::Call{fn, {obj}, {}});
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

bool install(PS2Runtime &runtime)
{
    g_control = runtime.lookupFunction(kPlayerControl);
    if (!g_control || !runtime.replaceFunction(kPlayerControl, &controlHook))
    {
        g_control = nullptr;
        return false;
    }
    g_addHp = runtime.lookupFunction(kAddHp);
    if (!g_addHp || !runtime.replaceFunction(kAddHp, &addHpHook))
        g_addHp = nullptr;
    g_cacheFree = runtime.lookupFunction(kCacheFree);
    if (!g_cacheFree || !runtime.replaceFunction(kCacheFree, &cacheFreeHook))
        g_cacheFree = nullptr;
    g_missionComplete = runtime.lookupFunction(kMissionComplete);
    if (!g_missionComplete || !runtime.replaceFunction(kMissionComplete, &missionCompleteHook))
        g_missionComplete = nullptr;
    g_objFace = runtime.lookupFunction(kObjFace);
    if (!g_objFace || !runtime.replaceFunction(kObjFace, &objFaceHook))
        g_objFace = nullptr;
    g_friendGaugeSet = runtime.lookupFunction(kFriendGaugeSet);
    if (!g_friendGaugeSet || !runtime.replaceFunction(kFriendGaugeSet, &friendGaugeSetHook))
        g_friendGaugeSet = nullptr;
    g_teardown = runtime.lookupFunction(kPlayerTeardown);
    if (!g_teardown || !runtime.replaceFunction(kPlayerTeardown, &teardownHook))
        g_teardown = nullptr;
    return true;
}

bool enabled()
{
    static const bool on = [] {
        // On unless turned off (KH2_AVATARS=0).
        const char *e = std::getenv("KH2_AVATARS");
        return !(e && *e == '0');
    }();
    return on;
}

uint32_t ourPlayer() { return g_ourPlayerShared.load(); }

std::atomic<uint32_t> g_usedSheets[kMaxAvatars] = {};
bool isAvatarSheet(uint32_t sheet)
{
    for (const auto &x : g_usedSheets)
        if (sheet && x.load() == sheet)
            return true;
    return false;
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
            s.plainLook = false;
            s.hiddenForCut = false;
        }
    }
    // Ours: the object the game made as the player for this room (a new
    // avatar's first updates point PlayerPtr at itself too).
    // (Not a player-class object being made for an avatar: for a moment it
    // is "the player" before its spawn job has told us; not one that has
    // gone, whose address the game's pointers may still hold.)
    bool ourKnown = false;
    for (auto &[id, s] : g_states)
        ourKnown = ourKnown || (s.object && player == s.object) || s.pending;
    ourKnown = ourKnown || justLeft(player);
    if (player && !ourKnown && live(rdram, player))
        g_ourPlayer = player;
    g_ourPlayerShared = g_ourPlayer;
    {
        bool pending = false;
        for (auto &[id, s2] : g_states)
            pending = pending || s2.pending;
        g_avatarPending = pending;
    }
    g_ourPlayerForFree = g_states.empty() ? 0u : g_ourPlayer; // (only with avatars about)
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
        const bool missionEnding = nowMs() - g_missionEndAt.load() < 30000;
        const bool here = r.present && r.world == st.now.world && r.room == st.now.room && !missionEnding;
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
        // Their own look, also in a drive form: a form's files read in for an
        // avatar were half there in a crowded room (a mission) when it was
        // made, and its motion crashed; and the form's files our player has
        // (if we are in it too) go when we revert, under the avatar.
        // (Now on by default: KH2_AVATAR_FORMS=0 gives the base look.)
        static const bool forms = [] { // on unless KH2_AVATAR_FORMS=0
            const char *e = std::getenv("KH2_AVATAR_FORMS");
            return !(e && *e == '0');
        }();
        if (const uint16_t base = kh2partner::formBaseLook(rdram, want))
            if (s.plainLook || !forms)
                want = base;
        // A cutscene (its actors are the story's): hidden the game's way
        // until it is over, not torn down (an avatar in our own look shares
        // its motion data with our player: tearing it down at a cutscene's
        // start left our player's motion without its animation, a crash).
        if (cutscene && here && s.object && s.entry == want)
        {
            if (!s.hiddenForCut)
            {
                std::printf("[avatars] player %u's avatar %08X hidden (cutscene)\n", r.playerId + 1, s.object);
                std::fflush(stdout);
                s.hiddenForCut = true;
            }
            hide(rdram, s.object, t);
            continue;
        }
        if (s.hiddenForCut && s.object && !cutscene)
        {
            s.hiddenForCut = false;
            show(rdram, s.object);
        }
        // Not here, or not made yet in a cutscene: away.
        if (!here || cutscene || (s.object && s.entry != want))
        {
            if (s.object)
            {
                std::printf("[avatars] player %u's avatar %08X goes (%s)\n", r.playerId + 1, s.object,
                            !here ? "not here" : cutscene ? "cutscene" : "a new look");
                std::fflush(stdout);
                removeAvatar(rdram, s.object);
                s.object = 0;
                s.prepared = false;
                s.motion = 0xFFFF;
                s.quietUntil = t + 1.0;
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
                for (auto &t : g_torn) // (a new object at an old one's address)
                    if (t.load() == obj)
                        t.store(0);
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
            s.writtenHp = s.writtenMp = -1;
            s.quietUntil = t + 1.0;
            s.madeAt = t;
            const uint32_t from = rd<uint32_t>(rdram, obj + kObjSheet);
            if (inRam(from) && s.sheet >= kSheetBase) // (a party slot's is the game's own)
                std::memcpy(rdram + s.sheet, rdram + from, kSheetSize);
            wr<uint32_t>(rdram, obj + kObjSheet, s.sheet);
        }
        const uint32_t flags = rd<uint32_t>(rdram, obj + kObjFlags);
        if (!(flags & kNoAct))
            wr<uint32_t>(rdram, obj + kObjFlags, flags | kNoAct);
        // A form's look whose files never came (a room with no room for
        // them): the avatar has no model (+0x7AC) and is not drawn. Their own
        // look instead, for this room.
        if (!s.plainLook && t - s.madeAt > 4.0 && rd<uint32_t>(rdram, obj + kObjModelData) == 0 &&
            kh2partner::formBaseLook(rdram, s.entry))
        {
            std::printf("[avatars] player %u's avatar (look %04X) has no model here: their own look instead\n",
                        r.playerId + 1, s.entry);
            std::fflush(stdout);
            s.plainLook = true;
        }
        // Their name for the character in their party place (the party
        // gauge, item targets, menus: names.h).
        if (!r.name.empty() && st.now.world < 0x20 && sheetIndex <= 2)
        {
            const uint8_t raw = rd<uint8_t>(rdram, kh2state::addr::Save + 0x3534 + uint32_t(st.now.world) * 4u + sheetIndex);
            const uint8_t character = raw & 0x7F;
            if (character > 0 && character < 16 && !(raw & 0x80))
                names[character] = r.name;
            // A world's own companion there (bit 7: Ping is 0x83, not her
            // character number; or Mulan, 3, who is Ping in her disguise):
            // the world companions' names (characters 3-15, Ping is 14) are
            // theirs; only one of them is in a party.
            if ((raw & 0x80) || (character >= 3 && character < 16))
                for (int c = 3; c <= 15; ++c)
                    names[c] = r.name;
        }
        // Their place in the party: the member in slot (index + 1), hidden
        // and frozen where the avatar is, on the avatar's sheet. The party
        // gauge shows it, items and Cure can be used on it, and what they
        // give goes to its player as a heal.
        {
            const uint32_t slot = sheetIndex - 1; // 0 or 1 (party slots 1 and 2)
            const uint32_t proxy = slot < 2 ? rd<uint32_t>(rdram, kFriendTable + slot * 4) : 0;
            const uint32_t was = s.proxy;
            s.proxy = proxy && live(rdram, proxy) && proxy != obj ? proxy : 0;
            if (was && was != s.proxy && live(rdram, was))
                show(rdram, was); // no longer anyone's place: seen again
            if (s.proxy)
            {
                const uint32_t pf = rd<uint32_t>(rdram, s.proxy + kObjFlags);
                if (!(pf & kNoAct))
                    wr<uint32_t>(rdram, s.proxy + kObjFlags, pf | kNoAct);
                hide(rdram, s.proxy, t);
                if (std::getenv("KH2_AVATAR_LOG"))
                {
                    static int n = 0;
                    if (++n % 120 == 1)
                    {
                        float pp[3];
                        std::memcpy(pp, rdram + s.proxy + 0x540, 12);
                        std::printf("[avatars] proxy %08X (flags %08X model %08X parts %08X) at %.0f,%.0f,%.0f; near it:\n", s.proxy,
                                    rd<uint32_t>(rdram, s.proxy + kObjFlags),
                                    rd<uint32_t>(rdram, s.proxy + kObjModel),
                                    inRam(rd<uint32_t>(rdram, s.proxy + kObjModel))
                                        ? rd<uint32_t>(rdram, rd<uint32_t>(rdram, s.proxy + kObjModel) + kModelParts)
                                        : 0u,
                                    pp[0], pp[1], pp[2]);
                        for (const kh2state::ActorInfo &a : kh2state::actors(rdram))
                        {
                            const float dx = a.pos[0] - pp[0], dz = a.pos[2] - pp[2];
                            if (dx * dx + dz * dz < 200.0f * 200.0f)
                                std::printf("[avatars]   %08X type %u %.32s (owner+570 %08X)\n", a.address,
                                            unsigned(a.type), a.name, rd<uint32_t>(rdram, a.address + kWeaponOwner));
                        }
                        std::fflush(stdout);
                    }
                }
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
            const bool quiet = t < s.quietUntil;
            const int32_t orb = takeOrbHp(s.sheet); // (an orb picked up here: not theirs)
            if (s.writtenHp >= 0 && hpNow > s.writtenHp + orb && !quiet)
                h.hp = hpNow - s.writtenHp - orb;
            else if (orb > 0 && s.writtenHp >= 0 && hpNow > s.writtenHp)
            {
                std::printf("[avatars] an orb gave player %u's place %d HP here: theirs is their own\n",
                            r.playerId + 1, hpNow - s.writtenHp);
                std::fflush(stdout);
            }
            // (MP: the game recharges a party slot's MP by itself, in small
            // steps; only an Ether-sized rise is ours.)
            if (s.writtenMp >= 0 && mpNow >= s.writtenMp + 10 && !quiet)
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
                removeAvatar(rdram, it->second.object);
            if (it->second.proxy && live(rdram, it->second.proxy))
                show(rdram, it->second.proxy);
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
    // The command/drive HUD (*0x003485EC) keeps the player it shows at +0x34,
    // picked up again while an avatar is being made: without this the drive
    // gauge loses its label and follows the avatar.
    if (g_ourPlayer)
    {
        const uint32_t hud = rd<uint32_t>(rdram, kHud);
        // (Always ours: an avatar gone since, or one being made, left there
        // too; the client's drive gauge read an avatar's sheet, 0.)
        if (hud >= 0x00100000u && hud < 0x02000000u && live(rdram, g_ourPlayer) && !g_states.empty() && rd<uint32_t>(rdram, hud + 0x34) != 0 &&
            rd<uint32_t>(rdram, hud + 0x34) != g_ourPlayer)
        {
            static int logged = 0;
            if (logged++ < 10)
            {
                std::printf("[avatars] the HUD showed %08X: back to our player %08X\n",
                            rd<uint32_t>(rdram, hud + 0x34), g_ourPlayer);
                std::fflush(stdout);
            }
            wr<uint32_t>(rdram, hud + 0x34, g_ourPlayer);
        }
    }
    {
        uint32_t j = 0;
        for (auto &[id, st2] : g_states)
            if (j < kMaxAvatars)
            {
                const uint32_t faceProxy = st2.object && st2.prepared ? st2.proxy : 0u;
                const uint32_t faceAvatar = st2.object && st2.prepared ? st2.object : 0u;
                const uint32_t wasProxy = g_faceProxy[j].exchange(faceProxy);
                const uint32_t wasAvatar = g_faceAvatar[j].exchange(faceAvatar);
                if (faceAvatar && (faceAvatar != wasAvatar || faceProxy != wasProxy))
                    refreshFace(rdram, faceProxy);
                g_usedSheets[j++].store(st2.object || st2.proxy ? st2.sheet : 0u);
            }
        for (; j < kMaxAvatars; ++j)
        {
            g_usedSheets[j].store(0);
            g_faceProxy[j].store(0);
            g_faceAvatar[j].store(0);
        }
    }
    {
        uint32_t i = 0;
        for (auto &[id, st2] : g_states)
            if (st2.object && i < kMaxAvatars)
                g_avatarObjs[i++].store(st2.object);
        for (; i < kMaxAvatars; ++i)
            g_avatarObjs[i].store(0);
    }
    kh2puppets::setOverrides(overrides, 2);
    kh2puppets::setMotionGuards(guards, 1);
    g_names = names;
    return heals;
}

namespace
{
void removeAvatar(uint8_t *rdram, uint32_t obj)
{
    {
        const uint32_t i = g_leavingNext.fetch_add(1) % 4;
        g_leavingAt[i].store(nowMs());
        g_leaving[i].store(obj);
    }
    std::vector<uint32_t> keep(std::begin(kPlayerRefs), std::end(kPlayerRefs));
    const uint32_t hud = rd<uint32_t>(rdram, kHud);
    if (inRam(hud))
        keep.push_back(hud + 0x34);
    kh2puppets::removeKeeping(obj, std::move(keep), [](uint32_t o) {
        const int64_t t = nowMs();
        for (int i = 0; i < 8; ++i)
            if (g_torn[i].load() == o && t - g_tornAt[i].load() < 2000)
                return true;
        return false;
    });
}
} // namespace

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
    {
        if (s.object)
            removeAvatar(rdram, s.object);
        if (s.proxy && live(rdram, s.proxy))
            show(rdram, s.proxy); // the party member is theirs again
    }
    g_states.clear();
    for (auto &a : g_avatarObjs)
        a.store(0);
    kh2puppets::setOverrides({}, 2);
    kh2puppets::setMotionGuards({}, 1);
}
} // namespace kh2avatars
