/* engage.c -- Engage at a marker in the career's free roam (a shop or garage door, a race start, a cover shoot):
 * mended when the game would refuse it for good, and logged when it refuses.
 *
 * Players reported the prompt up at a marker (the top left's icon) but Enter doing nothing, at shops, race starts and
 * a timed cover shoot's target, the last one a career stuck before stage 5 (2026-11).  The prompt and Engage are two
 * tests of the game's: the prompt (0x5EAFB0 -> 0x532A50) asks only whether the car is in the marker's trigger, Engage
 * (Player::HandleAction 0x602BD0, case 16, at 0x602D99) also wants the career not engaging already (Career +0x7138,
 * a byte: set there, cleared only by the engage manager's handler 0x606730 and a career's load 0x532830) and the
 * showcase preview's byte 0x85EF6C clear (its sub-modes set it, its Back 0x5295B0 clears it).  Left set, every
 * marker after says yes and does nothing until the next load.
 *
 * Each frame of the career's free roam with the HUD on top and the engage manager free ([[0x8900D8]] 4): either byte
 * set that long (STUCK frames) is cleared, as the game leaves them with nothing engaged, and logged.  And an Engage
 * pressed at a marker that brings no screen up within a second logs what Engage tests, for the next report. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

const char *autotest_top_fng(void);
long autotest_frame(void);

#define G_GAMEMODE      0x00850078u   /* 1 the career */
#define G_RACETYPE      0x0089e7b0u   /* 0 free roam */
#define CAREER          0x0085ad40u
#define C_ENGAGING      0x7138u       /* byte: an Engage under way */
#define G_PREVIEW       0x0085ef6cu   /* byte: the showcase preview's sub-mode on */
#define G_ENGAGE_MGR    0x008900d8u   /* -> the engage manager: +0 its state (4 free) */
#define ENGAGE_FREE     4u
#define G_ACTIONS       0x0086eaf0u   /* the actions held this frame, a bit each, 0 = held */
#define ACT_ENGAGE      15u
#define PL_MARKER       0x9acu        /* Player: the marker Engage would enter */
#define STUCK           120           /* frames */
#define NO_SCREEN       60            /* frames after an Engage at a marker */

static int ok4(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u); }

void engage_frame(CPU *c){
    (void)c;
    static long stuck_eng, stuck_prev, pressed_at = -1;
    static int was_held;
    static uint32_t snap[6];
    long f = autotest_frame();
    const char *top = autotest_top_fng();
    uint32_t mgr = MR32(G_ENGAGE_MGR), pl = MR32(SPLIT_PLAYERS_INDEX);
    int roam = MR32(G_GAMEMODE) == 1u && MR32(G_RACETYPE) == 0u && top && !strcasecmp(top, "HUD_SingleRace.fng");
    int idle = roam && ok4(mgr) && MR32(mgr) == ENGAGE_FREE;
    /* a byte left set with nothing engaged */
    if (idle && MR8(CAREER + C_ENGAGING)){
        if (!stuck_eng) stuck_eng = f;
        else if (f - stuck_eng >= STUCK){
            MR8(CAREER + C_ENGAGING) = 0; stuck_eng = 0;
            fprintf(stderr, "[engage] f=%ld the career was left engaging with nothing engaged (Career +0x7138): cleared, Enter at a marker works again\n", f);
        }
    } else stuck_eng = 0;
    if (idle && MR8(G_PREVIEW)){
        if (!stuck_prev) stuck_prev = f;
        else if (f - stuck_prev >= STUCK){
            MR8(G_PREVIEW) = 0; stuck_prev = 0;
            fprintf(stderr, "[engage] f=%ld the showcase preview's mode was left on in free roam (0x85EF6C): cleared, Enter at a marker works again\n", f);
        }
    } else stuck_prev = 0;
    /* an Engage at a marker that brought nothing up */
    int held = !(MR32(G_ACTIONS) & (1u << ACT_ENGAGE));
    uint32_t marker = ok4(pl + PL_MARKER) ? MR32(pl + PL_MARKER) : 0u;
    if (held && !was_held && roam && marker && pressed_at < 0){
        pressed_at = f;
        snap[0] = marker; snap[1] = MR8(CAREER + C_ENGAGING); snap[2] = MR8(G_PREVIEW);
        snap[3] = ok4(mgr) ? MR32(mgr) : 0xffffffffu; snap[4] = ok4(mgr + 0x4cu) ? MR32(mgr + 0x4cu) : 0xffffffffu;
        snap[5] = MR32(G_GAMEMODE);
    }
    was_held = held;
    if (pressed_at >= 0 && f - pressed_at >= NO_SCREEN){
        if (roam)                                     /* still the HUD: nothing came up */
            fprintf(stderr, "[engage] f=%ld Enter at marker %08x brought nothing up: engaging %u, preview %u, engage manager %u (kind %u), mode %u\n",
                    f, snap[0], snap[1], snap[2], snap[3], snap[4], snap[5]);
        pressed_at = -1;
    }
}
