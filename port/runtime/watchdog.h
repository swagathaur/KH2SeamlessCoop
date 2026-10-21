// Stall watchdog: if the host loop stops calling beat() for a while, print a
// stack trace of every thread (once per stall). See watchdog.cpp.
#pragma once

namespace kh2watch
{
void start(unsigned seconds);
void beat();
void stop();
} // namespace kh2watch
