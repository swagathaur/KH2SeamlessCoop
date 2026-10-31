#include "unlock.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace kh2unlock
{
namespace
{
// The save in memory (0x0032BB30): item counts at +0x3580 (a byte each, by
// the item's index), flag items (drive forms, abilities) as bits at +0x36C0.
constexpr uint32_t kSave = 0x0032BB30;
constexpr uint32_t kSaveCounts = kSave + 0x3580;
constexpr uint32_t kSaveFlags = kSave + 0x36C0;
// The item table (03system "item"): *0x01C6CB90 -> u32 ?, u32 count, records
// of 0x18 (u16 id, +2 type, +3 bit 0: a flag item, +0x12 its index), as the
// count lookup (FUN_001A3570) reads it.
constexpr uint32_t kItemTablePtr = 0x01C6CB90;
// Spells (FUN_001A6658): *0x01C6CB28 -> u32 ?, u32 count, records of 0x38
// (byte 0: slot 0..5, +0x24: the slot's element item; owned count = level).
constexpr uint32_t kMagicDataPtr = 0x01C6CB28;
// YS::ITEM::FormToItem: u16 item per form, form 1 (Valor) on, at 0x0036D458.
constexpr uint32_t kFormItems = 0x0036D458;
// The player's sheet (party slot 1): drive at +0x1B1, its maximum at +0x1B2.
constexpr uint32_t kSlot1 = 0x01C6C750;

const char *const kSpellNames[6] = {"Fire", "Blizzard", "Thunder", "Cure", "Magnet", "Reflect"};
// (Anti Form too: its item (0x1E) is in the table after Final Form, and once
// owned the Drive menu offers it like the others.)
constexpr int kForms = 6;
const char *const kFormNames[kForms] = {"Valor Form", "Wisdom Form", "Limit Form", "Master Form", "Final Form", "Anti Form"};

template <typename T> T rd(const uint8_t *ram, uint32_t a)
{
    T v;
    std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
    return v;
}
bool inRam(uint32_t a) { return a >= 0x00100000u && a < 0x02000000u; }

// The item table's record for `id` (0: none).
uint32_t itemRecord(const uint8_t *ram, uint16_t id)
{
    const uint32_t table = rd<uint32_t>(ram, kItemTablePtr);
    if (!inRam(table) || !id)
        return 0;
    const uint32_t n = rd<uint32_t>(ram, table + 4);
    for (uint32_t i = 0; i < n && i < 4096; ++i)
        if (rd<uint16_t>(ram, table + 8 + i * 0x18) == id)
            return table + 8 + i * 0x18;
    return 0;
}
bool isFlag(const uint8_t *ram, uint32_t rec) { return (ram[rec + 3] & 1) || ram[rec + 2] == 0x13; }

int owned(const uint8_t *ram, uint16_t id)
{
    const uint32_t rec = itemRecord(ram, id);
    if (!rec)
        return -1;
    const uint16_t index = rd<uint16_t>(ram, rec + 0x12);
    if (isFlag(ram, rec))
        return (ram[kSaveFlags + (index >> 3)] >> (index & 7)) & 1;
    return ram[kSaveCounts + index];
}

// Item `id` up to `count` (a flag item: its bit set). False: no such item.
bool give(uint8_t *ram, uint16_t id, int count)
{
    const uint32_t rec = itemRecord(ram, id);
    if (!rec)
        return false;
    const uint16_t index = rd<uint16_t>(ram, rec + 0x12);
    if (isFlag(ram, rec))
        ram[kSaveFlags + (index >> 3)] |= uint8_t(1u << (index & 7));
    else if (ram[kSaveCounts + index] < count)
        ram[kSaveCounts + index] = uint8_t(count);
    return true;
}

// A spell slot's element item (0: the magic data is not loaded).
uint16_t spellItem(const uint8_t *ram, int slot)
{
    const uint32_t magic = rd<uint32_t>(ram, kMagicDataPtr);
    if (!inRam(magic))
        return 0;
    const uint32_t count = rd<uint32_t>(ram, magic + 4);
    for (uint32_t i = 0; i < count && i < 256; ++i)
    {
        const uint32_t rec = magic + 8 + i * 0x38;
        if (ram[rec] == slot)
            if (const uint16_t item = rd<uint16_t>(ram, rec + 0x24))
                return item;
    }
    return 0;
}
uint16_t formItem(const uint8_t *ram, int index) { return rd<uint16_t>(ram, kFormItems + (index + 1) * 2); }

void setDrive(uint8_t *ram)
{
    ram[kSlot1 + 0x1B2] = 9;
    if (ram[kSlot1 + 0x1B1] < 9)
        ram[kSlot1 + 0x1B1] = 9;
}

std::mutex g_mutex;
std::vector<int> g_spellRequests, g_formRequests; // under g_mutex
std::atomic<bool> g_allRequested{false}, g_driveRequested{false};

void say(const std::string &s)
{
    std::printf("[unlock] %s\n", s.c_str());
    std::fflush(stdout);
}
} // namespace

std::vector<Entry> spells(const uint8_t *ram)
{
    std::vector<Entry> out;
    for (int slot = 0; slot < 6; ++slot)
    {
        const uint16_t item = spellItem(ram, slot);
        const int have = item ? owned(ram, item) : -1;
        if (have < 0)
            return {};
        out.push_back({kSpellNames[slot], have, 3});
    }
    return out;
}

std::vector<Entry> forms(const uint8_t *ram)
{
    std::vector<Entry> out;
    for (int i = 0; i < kForms; ++i)
    {
        const int have = owned(ram, formItem(ram, i));
        if (have < 0)
            return {};
        out.push_back({kFormNames[i], have, 1});
    }
    return out;
}

void giveSpell(int slot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_spellRequests.push_back(slot);
}
void giveForm(int index)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_formRequests.push_back(index);
}
void giveAll() { g_allRequested = true; }
void fillDrive() { g_driveRequested = true; }

std::string apply(uint8_t *ram)
{
    int spellsDone = 0, formsDone = 0;
    for (int slot = 0; slot < 6; ++slot)
        if (const uint16_t item = spellItem(ram, slot))
            spellsDone += give(ram, item, 3) ? 1 : 0;
    for (int i = 0; i < kForms; ++i)
        formsDone += give(ram, formItem(ram, i), 1) ? 1 : 0;
    if (!spellsDone && !formsDone)
        return {};
    setDrive(ram);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d spell(s) at level 3, %d drive form(s), drive 9/9", spellsDone, formsDone);
    return buf;
}

void tick(uint8_t *ram, uint64_t guestFrame)
{
    std::vector<int> spellReq, formReq;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        spellReq.swap(g_spellRequests);
        formReq.swap(g_formRequests);
    }
    for (int slot : spellReq)
        if (slot >= 0 && slot < 6)
            if (const uint16_t item = spellItem(ram, slot))
            {
                const int have = owned(ram, item);
                if (have >= 0 && have < 3 && give(ram, item, have + 1))
                    say(std::string(kSpellNames[slot]) + " is now level " + std::to_string(have + 1));
            }
    for (int i : formReq)
        if (i >= 0 && i < kForms && give(ram, formItem(ram, i), 1))
            say(std::string(kFormNames[i]) + " learned");
    if (g_driveRequested.exchange(false))
    {
        setDrive(ram);
        say("drive 9/9");
    }
    static const bool always = [] {
        const char *e = std::getenv("KH2_UNLOCK_ALL");
        return e && *e == '1';
    }();
    if (g_allRequested.exchange(false) || (always && guestFrame % 60 == 0))
    {
        static std::string last;
        const std::string did = apply(ram);
        if (!did.empty() && did != last)
        {
            last = did;
            say(did);
        }
    }
}
} // namespace kh2unlock
