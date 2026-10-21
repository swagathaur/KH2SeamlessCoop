// Switchable game patches from config/game_patches.toml (values from PCSX2's
// patch database; authors are credited there).
//
// Code patches arrive as recompiled copies of the affected game functions
// (tools/game_patches.py) and are swapped in with replaceFunction; their
// addresses are listed in config/hle_functions.txt so tail jumps reach them
// too. Data patches are memory writes repeated every frame, like PCSX2's
// "patch=1" lines, so they survive the game re-initialising the values.

#include "game_patches.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>
#include <map>
#include <mutex>

#include "game_patch_table.h"
#include "ps2_runtime.h"

namespace kh2patches
{
namespace
{
std::vector<const GamePatch *> g_enabled;
std::mutex g_mutex; // enable/disable (presenter thread) vs tick
std::map<uint32_t, PS2Runtime::RecompiledFunction> g_originalCode; // address -> translated original
std::map<uint32_t, uint32_t> g_originalData;                       // address -> word before the first write

const GamePatch *find(const std::string &name)
{
    for (const GamePatch *p = kGamePatches; p->name; ++p)
        if (name == p->name)
            return p;
    return nullptr;
}
} // namespace

bool setEnabled(PS2Runtime &runtime, const std::string &name, bool on)
{
    // Patches taken out (config/game_patches.toml says why); old settings
    // files may still name them.
    if (name == "no-subtitles")
    {
        if (on)
            std::fprintf(stderr, "[patches] no-subtitles is no longer available (it broke cutscenes); ignored\n");
        return true;
    }
    const GamePatch *p = find(name);
    if (!p)
    {
        std::fprintf(stderr, "[patches] unknown patch '%s'%s\n", name.c_str(),
                     kGamePatches[0].name ? "" : " (run tools/game_patches.py to build them)");
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = std::find(g_enabled.begin(), g_enabled.end(), p);
    if (on == (it != g_enabled.end()))
        return true;
    bool ok = true;
    // Debugging: KH2_PATCH_SKIP_CODE / KH2_PATCH_SKIP_DATA leave out one half.
    static const bool skipCode = std::getenv("KH2_PATCH_SKIP_CODE") != nullptr;
    static const bool skipData = std::getenv("KH2_PATCH_SKIP_DATA") != nullptr;
    // KH2_PATCH_ONLY_FN=addr,addr,...: swap in only these functions (hex).
    static const std::vector<uint32_t> onlyFns = [] {
        std::vector<uint32_t> v;
        if (const char *e = std::getenv("KH2_PATCH_ONLY_FN"))
        {
            std::stringstream ss(e);
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty())
                    v.push_back(static_cast<uint32_t>(std::stoul(t, nullptr, 16)));
        }
        return v;
    }();
    for (const CodeVariant *c = p->code; c && c->address && !skipCode; ++c)
    {
        if (!onlyFns.empty() && std::find(onlyFns.begin(), onlyFns.end(), c->address) == onlyFns.end())
            continue;
        if (on && !g_originalCode.count(c->address))
            g_originalCode[c->address] = runtime.lookupFunction(c->address);
        PS2Runtime::RecompiledFunction fn = on ? c->function : g_originalCode[c->address];
        if (!fn || !runtime.replaceFunction(c->address, fn))
        {
            std::fprintf(stderr, "[patches] %s: could not replace 0x%08X\n", p->name, c->address);
            ok = false;
        }
    }
    uint8_t *rdram = runtime.memory().getRDRAM();
    for (const DataWrite *d = p->data; d && d->address && !skipData; ++d)
    {
        uint8_t *dst = rdram + (d->address & PS2_RAM_MASK & ~3u);
        if (on && !g_originalData.count(d->address & ~3u))
        {
            uint32_t word;
            std::memcpy(&word, dst, 4);
            g_originalData[d->address & ~3u] = word;
        }
        if (!on)
        {
            const uint32_t word = g_originalData[d->address & ~3u];
            std::memcpy(dst, &word, 4);
        }
    }
    if (on)
    {
        g_enabled.push_back(p);
        std::printf("[patches] %s: %s (by %s)\n", p->name, p->description, p->author);
    }
    else
    {
        g_enabled.erase(it);
        std::printf("[patches] %s off\n", p->name);
    }
    return ok;
}

bool isEnabled(const std::string &name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const GamePatch *p : g_enabled)
        if (name == p->name)
            return true;
    return false;
}

std::vector<Info> available()
{
    std::vector<Info> out;
    for (const GamePatch *p = kGamePatches; p->name; ++p)
        out.push_back({p->name, p->description});
    return out;
}

bool enable(PS2Runtime &runtime, const std::string &names)
{
    bool ok = true;
    std::stringstream ss(names);
    for (std::string name; std::getline(ss, name, ',');)
        if (!name.empty())
            ok &= setEnabled(runtime, name, true);
    return ok;
}

void tick(uint8_t *rdram)
{
    static const bool skipData = std::getenv("KH2_PATCH_SKIP_DATA") != nullptr;
    if (skipData)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const GamePatch *p : g_enabled)
        for (const DataWrite *d = p->data; d && d->address; ++d)
        {
            uint8_t *dst = rdram + (d->address & PS2_RAM_MASK);
            if (d->bits == 32)
                std::memcpy(dst, &d->value, 4);
            else if (d->bits == 16)
            {
                const uint16_t v = static_cast<uint16_t>(d->value);
                std::memcpy(dst, &v, 2);
            }
            else
                *dst = static_cast<uint8_t>(d->value);
        }
}

float aspect()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    float a = 0.0f;
    for (const GamePatch *p : g_enabled)
        if (p->aspect > a)
            a = p->aspect;
    return a;
}

void list()
{
    if (!kGamePatches[0].name)
    {
        std::printf("  (no game patches built; run tools/game_patches.py)\n");
        return;
    }
    for (const GamePatch *p = kGamePatches; p->name; ++p)
        std::printf("  %-22s %s (by %s)\n", p->name, p->description, p->author);
}
} // namespace kh2patches
