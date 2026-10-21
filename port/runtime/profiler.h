// --profile[=MS]: sampling profiler for host time. See profiler.cpp.
#pragma once

namespace kh2prof
{
void start(unsigned intervalMs);
// Stops sampling and prints the hottest functions. Safe to call more than once.
void stopAndReport();
} // namespace kh2prof
