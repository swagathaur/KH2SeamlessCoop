// --dma-log=FRAME,COUNT: decode GIF-channel DMA chains. See dma_log.cpp.
#pragma once

#include <string>

class PS2Runtime;

namespace kh2dma
{
// Wrap sceDmaSend. Call before kh2trace::install so a trace can wrap this too.
void install(PS2Runtime &runtime, const std::string &spec);
} // namespace kh2dma
