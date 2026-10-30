// Testing helper: spells and drive forms learned on demand, from the F1 menu
// (one at a time), with KH2_UNLOCK_ALL=1 (all of them, kept), or the
// harness's "unlock". Saving in game afterwards keeps them in the save file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kh2unlock
{
struct Entry
{
    std::string name;
    int have = 0; // spell level (element items owned), or 1: form learned
    int max = 0;  // 3 for a spell, 1 for a form
};
// What the save has now (empty before the game's tables are loaded).
std::vector<Entry> spells(const uint8_t *rdram); // Fire, Blizzard, Thunder, Cure, Magnet, Reflect
std::vector<Entry> forms(const uint8_t *rdram);  // Valor, Wisdom, Limit, Master, Final

// Requests (any thread; done on the game's thread by tick()).
void giveSpell(int slot); // one more of its element item: the next level
void giveForm(int index); // 0..4 as forms()
void giveAll();           // every spell at level 3, every form, drive 9/9
void fillDrive();         // drive gauge 9/9

// Every spell at level 3, every form, drive 9/9 now (game thread); what it did.
std::string apply(uint8_t *rdram);
// Each frame on the game's thread: the requests, and KH2_UNLOCK_ALL=1.
void tick(uint8_t *rdram, uint64_t guestFrame);
} // namespace kh2unlock
