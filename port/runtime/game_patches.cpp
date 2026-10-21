// Switchable game patches from config/game_patches.toml (values from PCSX2's
// patch database; authors are credited there).
//
// Code patches arrive as recompiled copies of the affected game functions
// (tools/game_patches.py) and are swapped in with replaceFunction; their
// addresses are listed in config/hle_functions.txt so tail jumps reach them
// too. Data patches are memory writes repeated every frame, like PCSX2's
// "patch=1" lines, so they survive the game re-initialising the values.

#include "game_patches.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

#include "game_patch_table.h"
#include "ps2_runtime.h"

namespace kh2patches
{
namespace
{
std::vector<const GamePatch *> g_enabled;

const GamePatch *find(const std::string &name)
{
    for (const GamePatch *p = kGamePatches; p->name; ++p)
        if (name == p->name)
            return p;
    return nullptr;
}
} // namespace

bool enable(PS2Runtime &runtime, const std::string &names)
{
    bool ok = true;
    std::stringstream ss(names);
    for (std::string name; std::getline(ss, name, ',');)
    {
        if (name.empty())
            continue;
        const GamePatch *p = find(name);
        if (!p)
        {
            std::fprintf(stderr, "[patches] unknown patch '%s'%s\n", name.c_str(),
                         kGamePatches[0].name ? "" : " (run tools/game_patches.py to build them)");
            ok = false;
            continue;
        }
        for (const CodeVariant *c = p->code; c && c->address; ++c)
        {
            if (!runtime.replaceFunction(c->address, c->function))
            {
                std::fprintf(stderr, "[patches] %s: could not replace 0x%08X\n", p->name, c->address);
                ok = false;
            }
        }
        g_enabled.push_back(p);
        std::printf("[patches] %s: %s (by %s)\n", p->name, p->description, p->author);
    }
    return ok;
}

void tick(uint8_t *rdram)
{
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
