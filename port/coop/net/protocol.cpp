#include "protocol.h"

namespace kh2net
{
void writePlayerState(ByteWriter &w, const PlayerState &s)
{
    w.u8(s.origin);
    w.u32(s.tick);
    w.u8(s.world);
    w.u8(s.room);
    w.u16(s.door);
    for (float p : s.pos)
        w.f32(p);
    w.f32(s.rotY);
    w.u16(s.model);
    w.u16(s.anim);
    w.f32(s.animTime);
    w.i32(s.hp);
    w.i32(s.maxHp);
    w.i32(s.mp);
    w.i32(s.maxMp);
    w.u16(s.weapon);
    w.u8(s.flags);
}

void writeEnemyStates(ByteWriter &w, const EnemyStates &s)
{
    w.u8(s.origin);
    w.u32(s.tick);
    w.u8(s.world);
    w.u8(s.room);
    const size_t n = s.enemies.size() < kMaxEnemies ? s.enemies.size() : kMaxEnemies;
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i)
    {
        const EnemyState &e = s.enemies[i];
        w.u16(static_cast<uint16_t>(e.serial));
        w.u16(e.entry);
        for (float p : e.pos)
            w.f32(p);
        w.f32(e.yaw);
        w.u16(e.motion);
        w.i32(e.hp);
        w.i32(e.maxHp);
    }
}

bool readEnemyStates(ByteReader &r, EnemyStates &s)
{
    s.origin = r.u8();
    s.tick = r.u32();
    s.world = r.u8();
    s.room = r.u8();
    const uint8_t n = r.u8();
    if (n > kMaxEnemies)
        return false;
    s.enemies.resize(n);
    for (EnemyState &e : s.enemies)
    {
        e.serial = static_cast<int16_t>(r.u16());
        e.entry = r.u16();
        for (float &p : e.pos)
            p = r.f32();
        e.yaw = r.f32();
        e.motion = r.u16();
        e.hp = r.i32();
        e.maxHp = r.i32();
    }
    return r.ok();
}

bool readPlayerState(ByteReader &r, PlayerState &s)
{
    s.origin = r.u8();
    s.tick = r.u32();
    s.world = r.u8();
    s.room = r.u8();
    s.door = r.u16();
    for (float &p : s.pos)
        p = r.f32();
    s.rotY = r.f32();
    s.model = r.u16();
    s.anim = r.u16();
    s.animTime = r.f32();
    s.hp = r.i32();
    s.maxHp = r.i32();
    s.mp = r.i32();
    s.maxMp = r.i32();
    s.weapon = r.u16();
    s.flags = r.u8();
    return r.ok();
}
} // namespace kh2net
