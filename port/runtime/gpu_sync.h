// The game's "wait until VIF1 / VU1 / GIF are idle" (FUN_00134030).
//
// Before the frame's model and map chains go out, KH2 spins until D1_CHCR.STR,
// VIF1_STAT (FQC, VPS), VU1 (VPU_STAT bit 8) and GIF_STAT.APATH all read idle.
// With the asynchronous DMA worker (PS2X_ASYNC_DMA, patch 0033/0034) those
// reads block until the worker has drained everything, which serializes the
// EE behind VU1 and the GS again.
//
// Nothing here needs that wait: the worker takes a snapshot of each transfer
// when it is kicked and runs them in order, and any later read or write of
// state the worker owns (VIF/GIF registers, VU1 memory, GS privileged
// registers) still waits for it. So the function is replaced by one that
// returns 0 at once. KH2_GPU_SYNC=1 keeps the original.
#pragma once

class PS2Runtime;

namespace kh2gpusync
{
bool install(PS2Runtime &runtime);
} // namespace kh2gpusync
