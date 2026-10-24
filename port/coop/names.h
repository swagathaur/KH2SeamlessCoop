// Player names in the game's own text: a party member another player stands
// in for (partners.h) is called by that player's name wherever the game shows
// party members' names (the camp menu, item and status screens, level-up
// windows, ...).
//
// The game turns a message id into its text with 0x00192048 (the search of
// the loaded message files). Party members' names are asked for with
// YS::OBJPART::GetMessage (0x001B77A8, part = character + 1; the camp menu's
// Tz::PartyInfo and the level-up windows), which tail-jumps there, so both
// are hooked. The name ids are in the table at 0x0036DAD0,
// one per character (u16, by save character index + 1: 0x851F Sora, 0x8520
// Donald, 0x8521 Goofy, ...). For those ids the hook answers with the player's
// name, written into free guest memory in the game's text encoding (A-Z 0x2E-
// 0x47, a-z 0x9A-0xB3, 0-9 0x21-0x2A, space 0x01, 0 ends it).
#pragma once

#include <cstdint>
#include <map>
#include <string>

class PS2Runtime;

namespace kh2names
{
bool install(PS2Runtime &runtime);
// Character (save record index: 0 Sora, 1 Donald, 2 Goofy, ...) -> the name
// to show for it. Replaces the previous set; empty: the game's own names.
void set(const std::map<int, std::string> &byCharacter);
// The game's text encoding of `name` (letters, digits and spaces; others are
// left out), at most `max` characters.
std::string encode(const std::string &name, size_t max = 12);
} // namespace kh2names
