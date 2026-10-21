// sceGsResetGraph wrapper that fills libgraph's GParam. See gs_env.cpp.
#pragma once

class PS2Runtime;

namespace kh2gs
{
// Call after loadELF() (it reads sceGsGetGParam's code to find the struct).
bool installEnv(PS2Runtime &runtime);
} // namespace kh2gs
