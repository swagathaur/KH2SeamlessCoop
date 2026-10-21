// End-of-stream signal for ovl_movie's PSS playback.
//
// The runtime's libmpeg (bound to ovl_movie, see config/overlays/movie.stubs.toml)
// learns that a movie's data has run out from the CD streaming API (sceCdSt*),
// which is how Dragon Quest VIII feeds it. KH2 streams the PSS with ordinary
// sceCdRead calls instead, so after the last frame sceMpegGetPicture waited
// forever on a white screen and the game never left the opening movie.
//
// ovl_movie keeps the bytes still to read in a u64 at 0x01F294E0 and a
// "read in flight" byte at 0x01F294E8. Polled once per frame while the movie
// overlay is resident: 0 -> nonzero starts a stream, nonzero -> 0 with no read
// in flight is the end of it.

#include "movie_eof.h"

#include <cstdio>
#include <cstring>

#include "overlay_manager.h"
#include "ps2_runtime.h"

namespace ps2_stubs
{
void notifyMpegCdStreamStart(PS2Runtime *runtime);
void notifyMpegCdStreamEof(PS2Runtime *runtime);
} // namespace ps2_stubs

namespace kh2movie
{
namespace
{
constexpr uint32_t kRemainingBytes = 0x01F294E0u; // u64
constexpr uint32_t kReadBusy = 0x01F294E8u;       // u8
bool g_streaming = false;
unsigned g_streams = 0;
} // namespace

void poll(PS2Runtime &runtime)
{
    if (std::strcmp(kh2ovl::activeName(), "movie") != 0)
    {
        g_streaming = false;
        return;
    }
    const uint8_t *rdram = runtime.memory().getRDRAM();
    uint64_t remaining;
    std::memcpy(&remaining, rdram + kRemainingBytes, sizeof(remaining));
    const bool busy = rdram[kReadBusy] != 0;
    if (!g_streaming && remaining != 0)
    {
        g_streaming = true;
        ps2_stubs::notifyMpegCdStreamStart(&runtime);
        std::printf("[kh2-movie] stream %u started, %llu bytes\n", ++g_streams, (unsigned long long)remaining);
    }
    else if (g_streaming && remaining == 0 && !busy)
    {
        g_streaming = false;
        ps2_stubs::notifyMpegCdStreamEof(&runtime);
        std::printf("[kh2-movie] stream %u fully read: end of stream\n", g_streams);
    }
}
} // namespace kh2movie
