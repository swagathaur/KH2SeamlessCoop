#pragma once

class PS2Runtime;

// Keeps the menus' image-slot allocator (FUN_00236D98) inside its table.
namespace kh2imageslots
{
bool install(PS2Runtime &runtime);
}
