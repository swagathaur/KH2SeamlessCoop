// KH2 Final Mix EE code overlays share one guest arena. Calls into the arena
// dispatch through the function table of whichever overlay is resident.
// Design follows DQ8Recomp's overlay manager (GPL-3.0); KH2 overlays have no
// header, so the resident image is identified by the `j <entry>` word every
// overlay carries at arena offset +0x14 (each overlay's differs).
#pragma once

#include <cstddef>
#include <cstdint>

#include "ps2_runtime.h"

namespace kh2ovl
{
inline constexpr uint32_t kArenaStart = 0x01F18000u; // main ELF `overlay` section
inline constexpr uint32_t kArenaEnd = 0x01FC0000u;   // zeroed by FUN_00168548 before a load
inline constexpr uint32_t kSignatureAddress = kArenaStart + 0x14u;

struct OverlayInfo
{
    const char *name;
    uint32_t signature;   // word at kSignatureAddress when this overlay is resident
    uint32_t imageSize;
    PS2Runtime::FunctionRegion &(*region)();
};

// Generated into generated/overlays/overlay_registry.cpp.
extern const OverlayInfo kOverlays[];
extern const size_t kOverlayCount;

// Install the dispatch resolver. Call after PS2Runtime::loadELF().
void install(PS2Runtime &runtime);

// Name of the overlay currently resident ("none" if no known image), and how
// many times the resident overlay has changed since install().
const char *activeName();
uint32_t switchCount();
} // namespace kh2ovl
