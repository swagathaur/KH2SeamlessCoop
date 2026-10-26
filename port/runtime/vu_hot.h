#pragma once
// VU1 programs compiled while the game runs (vu_hot.cpp).
#include <filesystem>

namespace kh2vuhot
{
// Starts watching the recordings (PS2_VU_PROGRAM_PROFILE) when the build
// tools are there; exeDir holds kh2recomp.lib and vu-static-entries.txt.
void start(const std::filesystem::path &exeDir);
void stop();
} // namespace kh2vuhot
