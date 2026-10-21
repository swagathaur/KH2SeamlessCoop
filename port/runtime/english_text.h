// English text in the Japanese executable.
//
// KH2FM's code is the international one: REGION::Get (0x00105C30) returns
// the region found from SYSTEM.CNF (1 Japan, 2 US, 3 UK, ... 8 Final Mix),
// and text code asks "Japanese?" as Get() == 1 || Get() == 8. The answer
// picks the font's cell size (FUN_001AAA08: 18x18 / 24x24 for the Japanese
// fonts, 18x24 / 24x32 for the international ones, whose glyphs have
// ascenders and descenders) and how names and numbers are formatted into
// messages. With the US files in place of msg/jp/* (file_override), those
// checks have to answer "no".
//
// So REGION::Get is replaced and answers 2 (US) to the text call sites
// listed in english_text.cpp, and the real region to everything else: the
// pad code (Final Mix's button layout and right-stick camera), the save
// file's product code, voices and the other Final Mix features.
#pragma once

class PS2Runtime;

namespace kh2english
{
bool install(PS2Runtime &runtime);
bool active();
} // namespace kh2english
