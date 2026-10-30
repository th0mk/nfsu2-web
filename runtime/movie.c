/* movie.c -- Underground 2's movies in AV1 (tools/movies_av1.py): the game's
 * own movie player reads them, and the browser decodes their pictures
 * (runtime/movie_av1.c, every game's part, says how).
 *
 * The movie player (RealCore's VP6 codec object, vtable 0x7c35b8: its
 * DecodeFrame 0x700aff, frame 0x700a2f) keeps the On2 VP6 decoder statically
 * linked in SPEED2.EXE ("ON2.COM VERSION VP60D 6.1.0.5") behind three calls:
 *   0x702e47(decoder, data, size, w, h)  cdecl: decode a video chunk's payload
 *            (also for the frames skipped on the way to the one shown, 0x700c31)
 *   0x70288e(decoder, &yv12)             cdecl: post-process, then the
 *            picture's planes; the frame (0x700a2f) keeps the buffer's address
 *            and reads the planes from it as one block of (w+96)*(h+96)*3/2
 *   0x702e5e(&decoder)                   cdecl: free the decoder
 * The decoder is made by 0x7024d7, from the header's size.
 * One line a movie in the log: "[movie] AV1 768x512: 1867 frames, ..." */
#include "cpu.h"
#include "movie_av1.h"

/* the player's jumps to the next key frame when it fell 16 frames behind (0x700bc9), counted since its codec was made */
#define CATCHUPS  0x008b231cu

/* 0x702e47(decoder, data, size, w, h): decode one video chunk's payload */
void sub_00702e47_orig(CPU *c);
void sub_00702e47(CPU *c){
    if (!movie_av1_decode(MR32(c->esp + 4u), MR32(c->esp + 8u), MR32(c->esp + 12u), MR32(c->esp + 16u), MR32(c->esp + 20u))){
        sub_00702e47_orig(c); return; }
    c->eax = 0;
    POP32(c->eip);
}

/* 0x70288e(decoder, &yv12): the decoded picture's planes */
void sub_0070288e_orig(CPU *c);
void sub_0070288e(CPU *c){
    uint32_t dec = MR32(c->esp + 4u), o = MR32(c->esp + 8u);
    if (!movie_av1_planes(dec, o)){
        sub_0070288e_orig(c);
        movie_av1_vp6_planes(dec, o);
        return;
    }
    c->eax = 0;
    POP32(c->eip);
}

/* 0x702e5e(&decoder): the movie's end (or its codec's) */
void sub_00702e5e_orig(CPU *c);
void sub_00702e5e(CPU *c){
    movie_av1_free(MR32(c->esp + 4u), MR32(CATCHUPS));
    sub_00702e5e_orig(c);
}
