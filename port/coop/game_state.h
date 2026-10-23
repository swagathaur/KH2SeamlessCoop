// Typed reads of KH2 Final Mix's own state: where the player is, party stats,
// battle and control status. Addresses are for this ELF (SLPM_666.75, CRC
// F266B00B) and come from the KH2FM modding community's PCSX2 scripts
// (KH2FM-Mods-Num/GoA-ROM-Edition, "F266B00B GoA ROM.lua"), cross-checked at
// run time (docs/findings.md). The co-op layer builds on these.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kh2state
{
namespace addr
{
constexpr uint32_t Now = 0x0032BAE0;     // current location: world u8, room u8, door u16, map u16, battle u16, event u16
constexpr uint32_t Save = 0x0032BB30;    // the save data being played
constexpr uint32_t SavedLoc = 0x01D5A970;
constexpr uint32_t Music = 0x00347D34;
constexpr uint32_t Pause = 0x00347E08;   // 2 = pausing disabled
constexpr uint32_t React = 0x01C5FF4E;   // reaction command shown (u16)
constexpr uint32_t Cntrl = 0x01D48DB8;   // control state (3 = in a cutscene)
constexpr uint32_t CamTyp = 0x00348750;
constexpr uint32_t GamSpd = 0x00349E0C;  // game speed (float)
constexpr uint32_t CutNow = 0x0035DE20;  // cutscene timer
constexpr uint32_t CutLen = 0x0035DE28;  // cutscene length
constexpr uint32_t CutSkp = 0x0035DE08;
constexpr uint32_t BtlTyp = 0x01C61958;  // 0 out of battle, 1 battle, 2 forced battle
constexpr uint32_t BtlEnd = 0x01D490C0;
constexpr uint32_t TxtBox = 0x01D48D54;  // last text box shown
constexpr uint32_t Slot1 = 0x01C6C750;   // party unit stats, slot 1 (the player); companion n at Slot1 - n * SlotStride
constexpr uint32_t SlotStride = 0x268;
constexpr uint32_t Menu1 = 0x01C5FF18;   // command menu state
constexpr uint32_t Obj0Ptr = 0x01D5BA10; // -> 00objentry.bin
constexpr uint32_t Sys3Ptr = 0x01C61AF8; // -> 03system.bin
constexpr uint32_t Btl0Ptr = 0x01C61AFC; // -> 00battle.bin
constexpr uint32_t ArdPtr = 0x0034ECF4;  // -> current room's .ard
// The player's actor (a YS::PLAYER): this static holds it while the player
// exists; the object starts with the class's vtable pointer. Found by
// snapshot diffing (docs/findings.md, "Player actor").
constexpr uint32_t PlayerPtr = 0x0034E948;    // what YS::OBJ::GetPlayer() returns
constexpr uint32_t PlayerVtable = 0x00352410; // Roxas/Sora's YS::PLAYER (forms may differ)
// Live objects: a list from this head, linked through +0x8B4 (the game's own
// iterator 0x001B9BC8 skips objects whose is_leave() is set). +8 points at the
// object's 00objentry.bin record: id u32, type u8 at +4, model name at +8.
constexpr uint32_t ObjListHead = 0x01D48DA0;
constexpr uint32_t ObjNext = 0x8B4;
constexpr uint32_t ObjEntry = 0x8;
constexpr uint32_t ObjPos = 0x540;      // x, y, z, 1: what the world matrix is built from
constexpr uint32_t ObjDir = 0x550;      // sin(yaw), 0, cos(yaw), yaw
constexpr uint32_t ObjMotion = 0x140;   // YS::MOTION; its +0x14 (obj +0x154) is the motion index
constexpr uint32_t ObjMotionIndex = 0x154; // 0 idle, 2 run, 3 jump, 5 attack, ...
// FUN_00193608(obj): rebuild the object's world matrix from ObjPos/ObjDir;
// YS::OBJ::calc_motion calls it every frame before posing the skeleton.
constexpr uint32_t ObjBuildMatrix = 0x00193608;
// FUN_00104F58(const LOCATION* to, int fade, int, u8, callback): the game's map
// jump (YS::TEST_MAPJUMP and the script traps call it). LOCATION is world u8,
// room u8, door u16, map/battle/event u16; 0xFFFF there means "what the save
// says for that room". 0x0032BB20 is non-zero while a jump is under way.
constexpr uint32_t MapJump = 0x00104F58;
constexpr uint32_t MapJumpTask = 0x0032BB20;
constexpr uint32_t ActorMatrix = 0x40;  // 4x4 floats, rows; translation row at +0x70
constexpr uint32_t ActorPos = 0x70;     // x, y, z, 1 (y up, the floor at the feet)
constexpr uint32_t ActorFacing = 0x55C; // radians; matrix yaw + pi (mirrored at +0x66C)
// Offsets in a unit slot.
constexpr uint32_t SlotHp = 0x000, SlotMaxHp = 0x004, SlotMp = 0x180, SlotMaxMp = 0x184;
constexpr uint32_t SlotDrivePct = 0x1B0, SlotDrive = 0x1B1, SlotMaxDrive = 0x1B2;
// The field camera: a 4x4 view matrix (row vectors: v * M; up is -y), then
// the eye, the point looked at and the up vector (x, y, z, 1 each). Just
// before it, a projection whose x and y scales are at +0x00 and +0x14
// (docs/findings.md, "Camera").
constexpr uint32_t CameraView = 0x003A7F80;
constexpr uint32_t CameraEye = 0x003A7FC0;
constexpr uint32_t CameraTarget = 0x003A7FD0;
constexpr uint32_t CameraProj = 0x003A7EF0;
} // namespace addr

struct Location
{
    uint8_t world = 0, room = 0;
    uint16_t door = 0, map = 0, battle = 0, event = 0;
};

struct Unit
{
    int32_t hp = 0, maxHp = 0, mp = 0, maxMp = 0;
    uint8_t drive = 0, maxDrive = 0;
};

struct Actor
{
    bool valid = false;
    uint32_t address = 0;
    float pos[3] = {0, 0, 0};
    float facing = 0;
    uint32_t entryId = 0; // objentry id (0x5A = Roxas)
    uint32_t motion = 0;  // motion index
};

struct ActorInfo
{
    uint32_t address = 0;
    uint32_t entryId = 0;
    uint8_t type = 0;   // 0 player, 1 party, 2 NPC, 4 enemy, 5 weapon, 15 NPC (no collision), 22 prize...
    char name[33] = {};
    float pos[3] = {0, 0, 0};
    float yaw = 0;
};

struct State
{
    Actor player;
    Location now;
    Unit party[3];
    uint32_t control = 0;
    uint32_t battle = 0;
    float gameSpeed = 0;
    uint16_t reaction = 0;
    uint32_t cutsceneTime = 0, cutsceneLength = 0;
};

struct Camera
{
    bool valid = false;
    float view[16] = {};
    float scaleX = 0, scaleY = 0; // screen half-extents per unit of x/z, y/z
};
// Where a world point is on the game's picture: x, y in -1..1 across the
// picture (y down), and its depth in front of the camera. False when it is
// behind the camera or the camera is unknown.
bool readCamera(const uint8_t *rdram, Camera &out);
bool project(const Camera &cam, const float world[3], float &x, float &y, float &depth);

// rdram: the guest's 32 MB.
State read(const uint8_t *rdram);
// The live objects, in list order (at most `max`).
std::vector<ActorInfo> actors(const uint8_t *rdram, size_t max = 128);
const char *actorTypeName(uint8_t type);
const char *worldName(uint8_t world);
// "Twilight Town 02-05" style.
std::string describe(const Location &loc);
} // namespace kh2state
