// Stall watchdog: if the host loop stops calling beat() for a while, print a
// stack trace of every thread (once per stall). See watchdog.cpp.
#pragma once

namespace kh2watch
{
void start(unsigned seconds);
void beat();
void stop();
// Print every other thread's stack now (e.g. at a chosen guest frame).
void dumpNow();
// On a crash (an unhandled exception, std::terminate, abort), write a report
// with every thread's stack and a minidump to `dir` (default "crashes").
void installCrashHandler(const char *dir = nullptr);
} // namespace kh2watch
