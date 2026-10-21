// sceCdCallback and read-completion callbacks. See cd_callback.cpp.
#pragma once

#include <string>

class PS2Runtime;

namespace kh2cd
{
bool installCallbacks(PS2Runtime &runtime);
// " cd: reads=N failed=N" for the status line.
std::string stats();
} // namespace kh2cd
