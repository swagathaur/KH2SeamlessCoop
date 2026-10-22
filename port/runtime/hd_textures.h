// High-resolution texture replacement, with the PC release's remastered
// textures (tools/egs_hd_extract.py writes them and a manifest).
//
// The GS has no idea what a texture "is", so the replacement keys on the
// data: every completed upload to GS memory (a host -> local transfer) is
// hashed over its first 256 bytes (FNV-1a 64); a hit in the manifest marks
// that piece of GS memory (base pointer, buffer width, format, rectangle) as
// holding the image. When a draw then samples a texture at that base pointer
// in that format, inside that rectangle, paraLLEl-GS gets the high-resolution
// image instead of its own decoding of GS memory (patches/parallel-gs:
// TextureReplacer), mapped onto the same texel rectangle. Any later upload
// over the memory forgets the mark.
//
// The remastered images are full colour, so a texture drawn with different
// palettes shows the palette the PC artists painted; the alpha is rescaled to
// the GS's 0x80 = opaque.
#pragma once

#include <cstddef>
#include <filesystem>

namespace Vulkan
{
class Device;
}
namespace ParallelGS
{
class TextureReplacer;
}

namespace kh2hd
{
// Reads <folder>/manifest.txt. Returns how many images it lists.
size_t load(const std::filesystem::path &folder);
// The replacer for paraLLEl-GS, once a device is known (nullptr if nothing
// was loaded).
ParallelGS::TextureReplacer *attach(Vulkan::Device *device);
bool loaded();
bool enabled();
void setEnabled(bool on);
// Log uploads that matched and textures that were replaced (first time each).
void setLog(bool on);
// Forget every image (the device is going away).
void detach();
} // namespace kh2hd
