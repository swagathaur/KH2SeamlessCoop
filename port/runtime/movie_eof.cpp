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
// in flight is the end of it. With --skip-movies the remaining count is
// zeroed as soon as a stream starts, which ends the movie almost at once.

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
bool g_skip = false;
unsigned g_streams = 0;
} // namespace

void setSkip(bool skip) { g_skip = skip; }

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
    if (g_skip && g_streaming && !busy && remaining != 0)
    {
        // A read that was in flight when the count was zeroed subtracts from it
        // on completion (leaving it negative); zero it again once it lands.
        remaining = 0;
        std::memcpy(runtime.memory().getRDRAM() + kRemainingBytes, &remaining, sizeof(remaining));
    }
    if (!g_streaming && remaining != 0)
    {
        g_streaming = true;
        ps2_stubs::notifyMpegCdStreamStart(&runtime);
        std::printf("[kh2-movie] stream %u started, %llu bytes%s\n", ++g_streams, (unsigned long long)remaining,
                    g_skip ? " (skipping)" : "");
        if (g_skip)
        {
            // --skip-movies: pretend the file has been read; the player ends
            // the movie once the runtime's libmpeg reports the stream over.
            const uint64_t zero = 0;
            std::memcpy(runtime.memory().getRDRAM() + kRemainingBytes, &zero, sizeof(zero));
        }
    }
    else if (g_streaming && remaining == 0 && !busy)
    {
        g_streaming = false;
        ps2_stubs::notifyMpegCdStreamEof(&runtime);
        std::printf("[kh2-movie] stream %u fully read: end of stream\n", g_streams);
    }
}
} // namespace kh2movie
