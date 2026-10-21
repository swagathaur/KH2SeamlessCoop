// --trace=ADDR[,ADDR...]: log and count calls to guest functions. See trace.cpp.
#pragma once

#include <string>

class PS2Runtime;

namespace kh2trace
{
// Wrap each listed function start. Call after loadELF() and after any other
// replaceFunction() so the wrapper calls the final implementation.
void install(PS2Runtime &runtime, const std::string &spec);
// One-line summary of call counts since the previous call (empty if none traced).
std::string summary();
} // namespace kh2trace
