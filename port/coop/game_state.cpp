#include "game_state.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace kh2state
{
namespace
{
template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
} // namespace

bool readCamera(const uint8_t *ram, Camera &out)
{
    out = Camera{};
    std::memcpy(out.view, ram + addr::CameraView, sizeof(out.view));
    float p00, p11;
    std::memcpy(&p00, ram + addr::CameraProj, 4);
    std::memcpy(&p11, ram + addr::CameraProj + 0x14, 4);
    // Measured against screenshots: the picture's half-width covers x/z =
    // 1 / (0.47 * p00) (and likewise y), i.e. the stored scales are about
    // twice the visible frustum's (a guard band for clipping, presumably).
    constexpr float kVisible = 0.47f;
    out.scaleX = p00 * kVisible;
    out.scaleY = p11 * kVisible;
    const float w = out.view[15];
    out.valid = std::isfinite(p00) && std::isfinite(p11) && p00 > 0.1f && p00 < 20.0f && p11 > 0.1f &&
                p11 < 20.0f && std::fabs(w - 1.0f) < 1e-3f;
    for (float v : out.view)
        out.valid = out.valid && std::isfinite(v);
    return out.valid;
}

bool project(const Camera &cam, const float p[3], float &x, float &y, float &depth)
{
    if (!cam.valid)
        return false;
    const float *m = cam.view;
    const float vx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    const float vy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    const float vz = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
    if (vz < 10.0f)
        return false;
    x = vx / vz * cam.scaleX;
    y = vy / vz * cam.scaleY;
    depth = vz;
    return true;
}

State read(const uint8_t *ram)
{
    State s;
    s.now.world = rd<uint8_t>(ram, addr::Now);
    s.now.room = rd<uint8_t>(ram, addr::Now + 1);
    s.now.door = rd<uint16_t>(ram, addr::Now + 2);
    s.now.map = rd<uint16_t>(ram, addr::Now + 4);
    s.now.battle = rd<uint16_t>(ram, addr::Now + 6);
    s.now.event = rd<uint16_t>(ram, addr::Now + 8);
    for (int i = 0; i < 3; ++i)
    {
        const uint32_t b = addr::Slot1 - addr::SlotStride * uint32_t(i); // companions below the player
        Unit &u = s.party[i];
        u.hp = rd<int32_t>(ram, b + addr::SlotHp);
        u.maxHp = rd<int32_t>(ram, b + addr::SlotMaxHp);
        u.mp = rd<int32_t>(ram, b + addr::SlotMp);
        u.maxMp = rd<int32_t>(ram, b + addr::SlotMaxMp);
        u.drive = rd<uint8_t>(ram, b + addr::SlotDrive);
        u.maxDrive = rd<uint8_t>(ram, b + addr::SlotMaxDrive);
    }
    const uint32_t actor = rd<uint32_t>(ram, addr::PlayerPtr);
    if (actor >= 0x00100000u && actor < 0x02000000u - 0x1000u && rd<float>(ram, actor + addr::ActorPos + 12) == 1.0f)
    {
        s.player.valid = true;
        s.player.address = actor;
        for (int i = 0; i < 3; ++i)
            s.player.pos[i] = rd<float>(ram, actor + addr::ActorPos + 4u * uint32_t(i));
        s.player.facing = rd<float>(ram, actor + addr::ActorFacing);
        s.player.motion = rd<uint32_t>(ram, actor + addr::ObjMotionIndex);
        const uint32_t entry = rd<uint32_t>(ram, actor + addr::ObjEntry);
        if (entry >= 0x00100000u && entry < 0x02000000u)
            s.player.entryId = rd<uint32_t>(ram, entry);
    }
    s.control = rd<uint32_t>(ram, addr::Cntrl);
    s.battle = rd<uint32_t>(ram, addr::BtlTyp);
    s.gameSpeed = rd<float>(ram, addr::GamSpd);
    s.reaction = rd<uint16_t>(ram, addr::React);
    s.cutsceneTime = rd<uint32_t>(ram, addr::CutNow);
    s.cutsceneLength = rd<uint32_t>(ram, addr::CutLen);
    return s;
}

std::vector<ActorInfo> actors(const uint8_t *ram, size_t max)
{
    std::vector<ActorInfo> out;
    auto valid = [](uint32_t a) { return a >= 0x00100000u && a < 0x02000000u - 0x1000u; };
    uint32_t obj = rd<uint32_t>(ram, addr::ObjListHead);
    while (valid(obj) && out.size() < max)
    {
        ActorInfo a;
        a.address = obj;
        const uint32_t entry = rd<uint32_t>(ram, obj + addr::ObjEntry);
        if (valid(entry))
        {
            a.entryId = rd<uint32_t>(ram, entry);
            a.type = rd<uint8_t>(ram, entry + 4);
            std::memcpy(a.name, ram + ((entry + 8) & 0x01FFFFFFu), 32);
            a.name[32] = 0;
            for (char &c : a.name)
                if (c && (c < 32 || c > 126))
                    c = '?';
        }
        for (int i = 0; i < 3; ++i)
            a.pos[i] = rd<float>(ram, obj + addr::ObjPos + 4u * uint32_t(i));
        a.yaw = rd<float>(ram, obj + addr::ObjDir + 12);
        out.push_back(a);
        obj = rd<uint32_t>(ram, obj + addr::ObjNext);
    }
    return out;
}

const char *actorTypeName(uint8_t type)
{
    // 00objentry.bin object types (OpenKH's naming).
    static const char *const kNames[] = {"player", "party", "npc", "boss", "enemy", "weapon", "placeholder",
                                         "savepoint", "neutral", "out-of-party", "chest", "moogle", "giant boss",
                                         "?13", "pause dummy", "npc (no collision)", "?16", "?17", "?18", "?19",
                                         "?20", "?21", "prize"};
    return type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[type] : "?";
}

const char *worldName(uint8_t world)
{
    static const char *const kNames[] = {
        "World ZZ",           "End of Sea",        "Twilight Town", "Destiny Islands",  "Hollow Bastion",
        "Beast's Castle",     "Olympus Coliseum",  "Agrabah",       "Land of Dragons",  "100 Acre Wood",
        "Pride Lands",        "Atlantica",         "Disney Castle", "Timeless River",   "Halloween Town",
        "World Map",          "Port Royal",        "Space Paranoids", "The World That Never Was",
    };
    return world < sizeof(kNames) / sizeof(kNames[0]) ? kNames[world] : "?";
}

std::string describe(const Location &loc)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s %02x-%02x", worldName(loc.world), loc.world, loc.room);
    return buf;
}
} // namespace kh2state
