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
} // namespace kh2
