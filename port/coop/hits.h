// Hits: what the game's damage handler sees (YS::BTLOBJ::_OVR_exec_damage,
// 0x00186148, obj->vtable exec_damage(obj, DAMAGE*)). KH2_DAMAGE_LOG=1 prints
// each DAMAGE the field's objects take; the start of co-op hit replication.
#pragma once

class PS2Runtime;

namespace kh2hits
{
bool install(PS2Runtime &runtime);
} // namespace kh2hits
