/* movie_mw.c -- Most Wanted's movies in AV1 (tools/movies_av1.py) through the game's own movie player, as
 * Underground 2's (runtime/movie_av1.c says how; a movie in another language: game_mw.c's file_alt).
 *
 * The player is RealCore 6.24's VP6 codec object (GAME.md 8.2), the same code as Underground 2's at other
 * addresses (Q-07, from the disassembly): its DecodeFrame 0x7fa092 (thiscall, ret 4; slot 2 of the codec vtable
 * 0x8c2850) makes the decoder on the first frame with 0x7fbbe7(&decoder at codec+4, w, h), w and h from the MVhd
 * header (+0xc, +0xe), and hands each video chunk to
 *   0x7fc55d(decoder, data, size, w, h)   cdecl: the payload past the chunk's 8-byte header (callers 0x7fa011, the
 *            frame shown, and 0x7fa1c4, the frames decoded on the way to it)
 *   0x7fbf9e(decoder, &yv12)              cdecl: the planes, 11 dwords at [ebp-0x2c] of the frame call 0x7f9fc2,
 *            which keeps the 10th (the buffer) and reads one block of (w+0x60)*(h+0x60)*3/2 bytes from it (a 48-pixel
 *            border, as Underground 2's); its EMMS is at 0x7fa025
 *   0x7fc574(&decoder)                    cdecl: the decoder freed (from 0x7f9f9c)
 * Only calls reach the three, no jump.  The movies are 1024x512 at 29.97 fps (GAME.md 8.1).
 * One line a movie in the log: "[movie] AV1 1024x512: 92 frames, ..." */
#include "cpu.h"
#include "movie_av1.h"

/* the player's jumps to the next key frame when it fell 16 frames behind (0x7fa15c), counted since its codec was made */
#define CATCHUPS  0x009bfe1cu

/* 0x7fc55d(decoder, data, size, w, h): decode one video chunk's payload */
void sub_007fc55d_orig(CPU *c);
void sub_007fc55d(CPU *c){
    if (!movie_av1_decode(MR32(c->esp + 4u), MR32(c->esp + 8u), MR32(c->esp + 12u), MR32(c->esp + 16u), MR32(c->esp + 20u))){
        sub_007fc55d_orig(c); return; }
    c->eax = 0;
    POP32(c->eip);
}

/* 0x7fbf9e(decoder, &yv12): the decoded picture's planes */
void sub_007fbf9e_orig(CPU *c);
void sub_007fbf9e(CPU *c){
    uint32_t dec = MR32(c->esp + 4u), o = MR32(c->esp + 8u);
    if (!movie_av1_planes(dec, o)){
        sub_007fbf9e_orig(c);
        movie_av1_vp6_planes(dec, o);
        return;
    }
    c->eax = 0;
    POP32(c->eip);
}

/* 0x7fc574(&decoder): the movie's end (or its codec's) */
void sub_007fc574_orig(CPU *c);
void sub_007fc574(CPU *c){
    movie_av1_free(MR32(c->esp + 4u), MR32(CATCHUPS));
    sub_007fc574_orig(c);
}
