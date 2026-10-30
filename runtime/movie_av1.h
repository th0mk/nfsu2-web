/* movie_av1.h -- the movies in AV1 (tools/movies_av1.py), played by the game's own movie player: the part every
 * game shares (runtime/movie_av1.c).  A game's own file hooks its player's three decoder calls and hands each one
 * over here: Underground 2's runtime/movie.c, Most Wanted's runtime/mw/movie_mw.c. */
#ifndef MOVIE_AV1_H
#define MOVIE_AV1_H
#include <stdint.h>

/* The decoder's decode call (decoder, data, size, w, h): 1 when the chunk is an AV1 one, decoded here (the hook
 * returns 0 to the player and the decoder never sees it), 0 when the game's VP6 decoder takes it. */
int movie_av1_decode(uint32_t dec, uint32_t data, uint32_t size, uint32_t w, uint32_t h);
/* The decoder's planes call (decoder, &yv12): 1 when the decoder stands for an AV1 movie and the planes are written
 * into yv12 here (the hook returns 0), 0 when the VP6 decoder fills them: the hook calls it, then
 * movie_av1_vp6_planes with the same two. */
int movie_av1_planes(uint32_t dec, uint32_t yv12);
void movie_av1_vp6_planes(uint32_t dec, uint32_t yv12);
/* The decoder's free call (&decoder), before the game's own runs: an AV1 movie's end; catchups is the game's count
 * of jumps to the next key frame, for the log line. */
void movie_av1_free(uint32_t slot, uint32_t catchups);

#endif
