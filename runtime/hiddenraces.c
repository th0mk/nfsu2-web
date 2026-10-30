/* hiddenraces.c -- the career's hidden races on the map like the others, as
 * MaxHwoy's Show Hidden Races Mod (nfsmods.xyz/mod/1583) has them with its
 * GlobalB.lzc.  show_hidden_races = on (NFSU2_SHOW_HIDDEN_RACES=1); off by
 * default, the original game.
 *
 * GLOBAL\GlobalB's career chunk (0x80034A10, its first one only, read by
 * 0x512C00: the tables stay where the file was loaded) holds the races
 * ([0x8383E0] x [0x8383E4], 0x88 bytes: +0x37 stage, +0x7F hidden) and the
 * 6 stages ([0x83841C] x [0x838418], 0x50 bytes: +0x30..+0x34 the most
 * circuits, sprints, drags, drifts and Street X races open at once, 1 to 4,
 * read by 0x527AA0; +0x41 the most hidden races open at once, -1 any).  33
 * races are hidden: STAGE_0_CIRCUIT_0 and S2_H_* to S5_H_* (4, 8, 10 and 10).
 * The career's open races (Career +0x71C8, made by 0x527D30) are a random
 * pick of each kind up to the stage's cap (0x527BB0, which skips hidden
 * ones), then every hidden race of the stage not won yet (up to +0x41).  The
 * map and the radar leave a hidden one out (0x532C20), so only its marker on
 * the street shows it, once you drive near; a timer now and then points at
 * one (0x525650 by 0x500970), and the first look at one says it was found
 * (0x512F70).
 *
 * The mod (v1.1; oldman777's Binary script, nfsmods.xyz/mod/5048, is the
 * same) clears +0x7F on all 33 and sets each stage's 5 caps to 8, so the
 * races once hidden join the pick without taking others off the map.  Here
 * the same in memory, once the tables are loaded (a profile keeps none of it:
 * with the setting off again a career is as it was; races open stay open). */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define G_RACES         0x008383e0u   /* the races, RACE_SZ each, [G_NRACES] of them */
#define G_NRACES        0x008383e4u
#define G_STAGES        0x0083841cu   /* the stages, STAGE_SZ each, [G_NSTAGES] of them */
#define G_NSTAGES       0x00838418u
#define RACE_SZ         0x88u
#define STAGE_SZ        0x50u
#define R_HIDDEN        0x7fu         /* race: 1 hidden */
#define S_CAPS          0x30u         /* stage: 5 caps, a signed byte each */
#define CAP             8

static int g_on = -1;
static uint32_t g_done;               /* the race table changed */

static int committed(uint32_t a, uint32_t n){ return a && n && mem_is_committed(a) && mem_is_committed(a + n - 1u); }

/* every frame (roam.c's main-loop hook): once, when the tables are there */
void hiddenraces_frame(void){
    if (g_on < 0){       /* the page writes 1 or 0; the Mac app passes the line's own on/off */
        const char *e = getenv("NFSU2_SHOW_HIDDEN_RACES");
        g_on = e && *e && *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
    }
    if (!g_on) return;
    uint32_t races = MR32(G_RACES), nr = MR32(G_NRACES), stages = MR32(G_STAGES), ns = MR32(G_NSTAGES);
    if (!races || races == g_done || nr > 1024u || ns > 16u || !committed(races, nr * RACE_SZ) || !committed(stages, ns * STAGE_SZ))
        return;
    int n = 0, caps = 0;
    for (uint32_t i = 0; i < nr; i++)
        if (MR8(races + i * RACE_SZ + R_HIDDEN)){ MR8(races + i * RACE_SZ + R_HIDDEN) = 0; n++; }
    for (uint32_t s = 0; s < ns; s++)
        for (uint32_t k = 0; k < 5u; k++){
            uint32_t a = stages + s * STAGE_SZ + S_CAPS + k;
            int8_t v = (int8_t)MR8(a);                    /* -1: no cap */
            if (v >= 0 && v < CAP){ MR8(a) = CAP; caps++; }
        }
    g_done = races;
    fprintf(stderr, "[career] show hidden races: %d of %u races out of hiding, %d of the %u stages' caps raised to %d\n",
            n, nr, caps, ns, CAP);
}
