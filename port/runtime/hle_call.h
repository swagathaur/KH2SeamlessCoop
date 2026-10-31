// Calling the implementation a replaced guest function had before.
//
// tools/recomp.py gives stub wrappers and the functions in
// config/hle_functions.txt a forwarder, so a tail jump (which the recompiler
// emits as a direct C++ call) still reaches a replacement installed with
// PS2Runtime::replaceFunction. A replacement that wraps the original must call
// it through callOriginal(): the flag tells the forwarder to run the original
// body instead of forwarding back into the replacement.
#pragma once

#include "ps2_runtime.h"

extern thread_local bool kh2_callingOriginal; // read by the generated forwarders

namespace kh2
{
inline void callOriginal(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx,
                         PS2Runtime *runtime)
{
    kh2_callingOriginal = true;
    original(rdram, ctx, runtime);
    kh2_callingOriginal = false;
}

// The original, called from the hook of the function at `entry`; true once
// it has returned for good. A guest function can be paused part way (the EE
// scheduler switches threads at its checkpoints): the original then comes
// back early with the pc where it is to go on, and finishes in later calls
// through the hook (with that pc, its registers no longer the arguments).
// What a hook does with the result (v0) or after the call belongs only after
// a call that came in at the entry has returned (pc at its return address,
// sp as it came in): on a paused one v0 is not the result yet, and writing
// it changes the paused function's own register.
inline bool callOriginalReturned(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx,
                                 PS2Runtime *runtime, uint32_t entry)
{
    const bool fromEntry = ctx->pc == entry;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    callOriginal(original, rdram, ctx, runtime);
    return fromEntry && ctx->pc == ra && static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) == sp;
}
} // namespace kh2
