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
//
// --skip-movies does what the player's own skip does: ovl_movie keeps its
// playback state in a word at 0x01F29470; setting it to 1 (0x01F1B5F0 is the
// game's setter) makes the decoder thread stop, which then sets 3 (finished)
// and the player shuts its threads down. Zeroing the bytes-remaining count instead left the
// decoder waiting for pictures forever after the opening.

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
constexpr uint32_t kPlayState = 0x01F29470u;      // u32: 1 = stop requested, 3 = decoder finished
bool g_streaming = false;
bool g_skip = false;
bool g_skipOnce = false;
unsigned g_streams = 0;
} // namespace

#if defined(__ANDROID__)
// No MPEG decoder in the Android build yet (no FFmpeg): movies would be a black
// screen, so they are always skipped there.
void setSkip(bool) { g_skip = true; }
#else
void setSkip(bool skip) { g_skip = skip; }
#endif
bool skipping() { return g_skip; }
bool playing() { return g_streaming; }
void skipCurrent()
{
    if (g_streaming)
        g_skipOnce = true;
}

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
    static unsigned s_polls = 0;
    if (g_streaming && (++s_polls % 2000u) == 0u)
        std::printf("[kh2-movie] streaming: remaining=%lld busy=%d state=%u\n", (long long)remaining, (int)busy,
                    *reinterpret_cast<const uint32_t *>(rdram + kPlayState));
    static bool s_stopSent = false;
    if (!g_streaming)
    {
        s_stopSent = false;
        g_skipOnce = false;
    }
    if ((g_skip || g_skipOnce) && g_streaming && !s_stopSent)
    {
        uint32_t state;
        std::memcpy(&state, rdram + kPlayState, sizeof(state));
        if (state != 1u && state != 3u)
        {
            s_stopSent = true;
            const uint32_t stop = 1u;
            std::memcpy(runtime.memory().getRDRAM() + kPlayState, &stop, sizeof(stop));
            std::printf("[kh2-movie] stream %u: stop requested (%s)\n", g_streams, g_skip ? "--skip-movies" : "skipped");
        }
    }
    if (!g_streaming && remaining != 0)
    {
        g_streaming = true;
        ps2_stubs::notifyMpegCdStreamStart(&runtime);
        std::printf("[kh2-movie] stream %u started, %llu bytes%s\n", ++g_streams, (unsigned long long)remaining,
                    g_skip ? " (skipping)" : "");
    }
    else if (g_streaming && remaining == 0 && !busy)
    {
        g_streaming = false;
        ps2_stubs::notifyMpegCdStreamEof(&runtime);
        std::printf("[kh2-movie] stream %u fully read: end of stream\n", g_streams);
    }
}
} // namespace kh2movie
