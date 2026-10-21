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
    w.u8(s.flags);
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
    s.flags = r.u8();
    return r.ok();
}
} // namespace kh2net
