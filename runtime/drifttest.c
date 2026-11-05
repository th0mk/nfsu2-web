/* drifttest.c -- test helpers for the drift fidelity work.  Nothing here is
 * active unless asked for.
 *
 *   NFSU2_FIXED_DT = <hz>
 *       The world time step the game simulates (the value 0x601A60 returns
 *       into [0x865154], which World::DoTimestep, the car physics, the drift
 *       scoring and the WorldTimer use) becomes exactly 1/hz whenever the
 *       game asks for a step > 0.  The simulation is then independent of how
 *       long a frame really took: a slow test binary (the oracle) or a loaded
 *       machine replays the same physics, and 60 vs 120 Hz stepping can be
 *       compared with identical input timing in game time.  The render rate
 *       and real-dt consumers (camera, audio, HUD timers) are not touched.
 *       A comma list (e.g. 60,20) cycles through the rates frame by frame: a
 *       reproducible uneven frame time, like a slow device's.
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

void sub_00601a60_orig(CPU *);
/* GRaceStatus world-time scaler (thiscall, 1 float arg, float in ST0):
 * time scale [0x7A5730] and the online clock correction. */
void sub_00601a60(CPU *c){
    static float step[16]; static int nstep = -1, k = 0;
    if (nstep < 0){
        nstep = 0;
        const char *e = getenv("NFSU2_FIXED_DT");
        for (const char *p = e ? e : ""; *p && nstep < 16; ){
            const char *q = p;
            double hz = strtod(q, (char **)&p);
            if (p == q) break;                          /* not a number */
            if (hz > 0.0) step[nstep++] = (float)(1.0 / hz);
            while (*p == ',' || *p == ' ') p++;
        }
        if (nstep) fprintf(stderr, "[drifttest] fixed world step: %d rate(s) from %s\n", nstep, e);
    }
    sub_00601a60_orig(c);
    if (nstep && ST(0) > 0.0){ ST(0) = (double)step[k]; k = (k + 1) % nstep; }
    /* photo mode's shutter (photo.c): each of its picture's frames a step of the world of its own length */
    int photo_world_step(double *st0);
    double st = ST(0);
    if (photo_world_step(&st)) ST(0) = st;
}
