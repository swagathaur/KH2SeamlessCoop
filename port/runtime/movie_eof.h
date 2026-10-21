// End-of-stream signal for ovl_movie's PSS playback. See movie_eof.cpp.
#pragma once

class PS2Runtime;

namespace kh2movie
{
// --skip-movies: end each movie as soon as it starts streaming.
void setSkip(bool skip);
bool skipping();
// A movie is streaming now.
bool playing();
// End the movie that is playing (as --skip-movies would have).
void skipCurrent();
// Call once per presented frame.
void poll(PS2Runtime &runtime);
} // namespace kh2movie
