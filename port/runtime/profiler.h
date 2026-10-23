// --profile[=MS]: sampling profiler for host time. See profiler.cpp.
#pragma once

namespace kh2prof
{
void start(unsigned intervalMs);
// Stops sampling and prints the hottest functions. Safe to call more than once.
void stopAndReport();
// Samples are only kept while recording (on by default).
void setRecording(bool on);
} // namespace kh2prof
