// libpad2 (DualShock 2) high-level replacement for KH2 Final Mix. See pad2_hle.cpp.
#pragma once

class PS2Runtime;

namespace kh2pad2
{
// Bind libpad2 entry points to host-input handlers. Call after loadELF().
bool install(PS2Runtime &runtime);
} // namespace kh2pad2
