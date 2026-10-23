#include "hits.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle_call.h"
#include "ps2_runtime.h"

namespace kh2hits
{
namespace
{
constexpr uint32_t kExecDamage = 0x00186148; // YS::BTLOBJ::_OVR_exec_damage(DAMAGE*)
PS2Runtime::RecompiledFunction g_execDamage = nullptr;
int g_logged = 0;

void execDamageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4), dmg = getRegU32(ctx, 5);
    if (g_logged < 400 && obj < 0x02000000u && dmg < 0x02000000u)
    {
        ++g_logged;
        uint32_t entry = 0, rec = 0;
        std::memcpy(&rec, rdram + ((obj + 8) & 0x01FFFFFFu), 4);
        if (rec && rec < 0x02000000u)
            std::memcpy(&entry, rdram + (rec & 0x01FFFFFFu), 4);
        std::printf("[hits] obj %08X (%04X) damage %08X:", obj, entry & 0xFFFF, dmg);
        for (int i = 0; i < 0x60; i += 4)
        {
            uint32_t w;
            std::memcpy(&w, rdram + ((dmg + i) & 0x01FFFFFFu), 4);
            std::printf(" %08X", w);
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    kh2::callOriginal(g_execDamage, rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    const char *e = std::getenv("KH2_DAMAGE_LOG");
    if (!e || *e != '1')
        return false;
    g_execDamage = runtime.lookupFunction(kExecDamage);
    if (!g_execDamage || !runtime.replaceFunction(kExecDamage, &execDamageHook))
        return false;
    std::printf("[hits] logging damage\n");
    return true;
}
} // namespace kh2hits
