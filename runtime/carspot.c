/* carspot.c -- the career's kept spot moved to the new career's start when
 * it is no place on the map (issue 55).
 *
 * Leaving free roam (the pause menu's Quit, the garage) the game keeps in the
 * career where the player's car is: Career (0x85AD40) +0x8750 the place,
 * +0x8760 the heading, +0x8744 set (0x513B30, 0x513950).  The profile saves
 * them (0x514530), Resume Career and the crib's Return to Explore Mode set
 * +0x8744 again (0x495A30, 0x4AF3A0), and the world's load puts the car there
 * (0x5404A0, 0x540780) without a look at the numbers.  A car once left at NaN
 * in free roam (roam_game.c car_rescue) had its spot kept so, and every later
 * load put the car at NaN again: a black screen under the HUD, after any
 * restart.  Every frame (roam.c's main-loop hook, free-roam multiplayer on or
 * not), kept or not (Return to Explore Mode sets it and loads the world in
 * the same frame): a spot that is no place becomes the new career's start,
 * the airport's car park, which every career can reach, and the next save
 * keeps that. */
#include "cpu.h"
#include <stdio.h>

#define G_CAREER        0x0085ad40u   /* Career */
#define C_SPOT          0x8750u       /* float[3]: the kept place */
#define C_SPOT_HEADING  0x8760u       /* u16 */
#define PLACE_MAX       100000.f      /* farther out than this (m): no place on the map */
static const float START[3] = { 1695.25f, -883.64f, 24.11f };   /* the new career's start (roam_game.c SPAWN_*) */
#define START_HEADING   0xf840u

long autotest_frame(void);

void carspot_frame(void){
    float p[3]; int bad = 0;
    for (uint32_t i = 0; i < 3u; i++){
        p[i] = MRF32(G_CAREER + C_SPOT + 4u * i);
        if (!__builtin_isfinite(p[i]) || __builtin_fabsf(p[i]) > PLACE_MAX) bad = 1;
    }
    if (!bad) return;
    for (uint32_t i = 0; i < 3u; i++) MRF32(G_CAREER + C_SPOT + 4u * i) = START[i];
    MR16(G_CAREER + C_SPOT_HEADING) = START_HEADING;
    fprintf(stderr, "[carspot] f=%ld the career's kept spot is no place (%.1f %.1f %.1f, %s): the new career's start instead\n",
            autotest_frame(), (double)p[0], (double)p[1], (double)p[2], MR8(G_CAREER + 0x8744u) ? "kept" : "not kept now");
}
