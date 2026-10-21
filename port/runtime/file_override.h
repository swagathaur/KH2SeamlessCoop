// Replacing game files without touching the disc image: files in an override
// folder (laid out by the game's own names, e.g. msg/jp/sys.bar) are given
// CD sectors of their own past the end of the disc, and their entries in
// the game's file indexes in RAM are pointed at them.
//
// KH2 looks a name up (FUN_001AE460, which searches the three index tables
// whose pointers are at 0x01C6CFB0..B8: KH2.IDX and the current world's
// 000<world>.idx) and reads the entry's sectors with sceCdRead: +0 CRC-32 of
// the name, +4 CRC-16 (u16, checked when bit 15 of +6 is set), +6 sector
// count - 1 (bits 0-13), compressed (bit 14), +8 LBN, +C size. Each frame the
// entries of overridden names are found by hash and rewritten (a world's
// index is reloaded when the world changes).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class PS2Runtime;

namespace kh2files
{
// Use the files under these folders (later folders win). Returns how many.
size_t install(const std::vector<std::filesystem::path> &folders);
// Once per host loop iteration.
void tick(uint8_t *rdram);
size_t count();

// The game's name hashes (exposed for tests).
uint32_t hash32(const std::string &name);
uint16_t hash16(const std::string &name);
} // namespace kh2files
