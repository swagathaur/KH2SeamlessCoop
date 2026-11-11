// sceCdSearchFile from the disc image's ISO9660 directory. See cd_iso.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace kh2cd
{
// Index the image and bind sceCdSearchFile. Call after loadELF().
bool install(PS2Runtime &runtime, const std::string &isoPath);

// A file of KH2.IMG by the game's name for it (as found in KH2.IDX, or in
// `subIndex`, an index inside KH2.IMG such as "000gumi.idx"), decompressed.
// Reads the disc image directly (any thread); false if it is not there.
bool readGameFile(const std::string &name, std::vector<uint8_t> &out, const std::string &subIndex = "");
} // namespace kh2cd
