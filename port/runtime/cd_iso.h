// sceCdSearchFile from the disc image's ISO9660 directory. See cd_iso.cpp.
#pragma once

#include <string>

class PS2Runtime;

namespace kh2cd
{
// Index the image and bind sceCdSearchFile. Call after loadELF().
bool install(PS2Runtime &runtime, const std::string &isoPath);
} // namespace kh2cd
