/* whine.h -- each local player's gearbox whine, as ropi's Underground 2
 * Transmission Whine has it (whine.c).  gearbox_whine = on
 * (NFSU2_GEARBOX_WHINE=1); off by default, the original game. */
#ifndef WHINE_H
#define WHINE_H
#include <stdint.h>

/* Every frame on the game thread (roam.c's main-loop hook): reads the local
 * players' cars and sets their whines' pitches and volumes. */
void whine_frame(void);
/* Adds the whines to an interleaved stereo float mix.  Mixer thread. */
void whine_mix(float *acc, unsigned frames, unsigned rate);
#endif
