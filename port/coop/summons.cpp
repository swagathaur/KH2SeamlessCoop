#include "summons.h"

#include <cstdio>
#include <cstdlib>

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace kh2summons
{
namespace
{
// FUN_001C9AB8: may we summon? True if FUN_001BC010 says so, or if both
// friends (YS::FRIEND::Get(0), Get(1)) are there and can make way
// (YS::FRIEND::can_summon_sacrifice, 0x0017E3B8). The summon command's state
// (FUN_001C9B38, command type 0x48) is 2, shown as "?", when it is false.
constexpr uint32_t kCanSummon = 0x001C9AB8;
PS2Runtime::RecompiledFunction g_canSummon = nullptr;

void canSummonHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == kCanSummon)
    {
        SET_GPR_U32(ctx, 2, 1);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    g_canSummon(rdram, ctx, runtime);
}

// kn::ObjObserver's destructor (its vtable's slot 1) starts at 0x00135F70
// with "lui $t7, 0x34"; the function map starts it one instruction later
// (FUN_00135F74, which goes on from $t7). A call to the real start had no
// function and was skipped (a null call, from 0x0010C894 as a party member
// let go of its observers when a summon sent it away): the observer stayed
// linked in the game's list of them (0x00347D08) after its memory was freed.
constexpr uint32_t kObserverDtor = 0x00135F70, kObserverDtorBody = 0x00135F74;
PS2Runtime::RecompiledFunction g_observerDtorBody = nullptr;
void observerDtor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    SET_GPR_U32(ctx, 15, 0x00340000u);
    ctx->pc = kObserverDtorBody;
    g_observerDtorBody(rdram, ctx, runtime);
}
} // namespace

bool install(PS2Runtime &runtime)
{
    if (!runtime.hasFunction(kObserverDtor))
    {
        g_observerDtorBody = runtime.lookupFunction(kObserverDtorBody);
        if (!g_observerDtorBody || !runtime.registerFunction(kObserverDtor, &observerDtor))
            std::fprintf(stderr, "[summons] could not add kn::ObjObserver's destructor\n");
    }
    if (const char *e = std::getenv("KH2_SUMMON_PARTY"); e && *e == '1')
        return false; // (testing: the game's own rule, two friends)
    g_canSummon = runtime.lookupFunction(kCanSummon);
    if (!g_canSummon || !runtime.replaceFunction(kCanSummon, &canSummonHook))
    {
        g_canSummon = nullptr;
        std::fprintf(stderr, "[summons] could not hook the party check\n");
        return false;
    }
    return true;
}
} // namespace kh2summons
