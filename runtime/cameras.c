/* cameras.c -- the drift camera (the drift races' own, high behind the car)
 * in every drive, after the four views the camera key (C, a controller's Y,
 * the touch screen's camera button) goes through.  Nothing else changes: the
 * views keep their order and their look, Look Back and the right stick's
 * look work with it as with the others, and a profile keeps it when picked,
 * as it keeps the others.
 *
 * The game (v1.2 NA; the addresses as ExtraOptions' EnableHiddenCameraModes
 * names them, ExOptsTeam/NFSU2ExOpts, GPL-3.0: only the facts are used here):
 *   PlayerSettings (0x83AA64, 0x28 each) +0x10 the view.
 *   0x502540 ScrollDriveCam (thiscall settings, step +1 / -1, ret 4): the
 *     next or previous view, wrapping; the last is 3 (four views), or 4 in a
 *     drift event ([0x89E7D9]), with the drift-event switch on ([0x890104],
 *     drift.c's drift mode) or on the drift tracks 0x104E .. 0x1053
 *     ([0x89E7A0]).  Here it is 4 everywhere.
 *   0x502500 GetPOVTypeFromPlayerCamera (cdecl view): the view's camera kind,
 *     0 -> 2, 1 -> 4, 2 -> 1, 3 -> 0, 4 -> 5 (the drift camera).  The game has
 *     a sixth kind, 3, which ExtraOptions puts on a view 5 ("driver camera"):
 *     left out here, it looks some 35 degrees left of the car's heading from
 *     low in it, without the car (tried 2026-11-03).
 * drift.c's drift mode (X) puts view 4 on and gives the one before back.
 *
 * The player's view after a drift event.  A drift event's setup puts the
 * drift camera on whatever the player had (each human racer's, 0x525ED0, in
 * a drift race; a race's from its record, 0x5322E0, for a race of kind 5; the
 * dyno's test run in its Drift mode, 0x526970 -> 0x4FF070), and every other
 * setup turns a view 4 into 3: the same ones for the other races (the dyno's
 * other modes: 0x4FEF80, 0x4FEFF0, 0x4FF0E0, 0x4FF160) and free roam's
 * (0x5404A0, 0x540780; player 2's view too), view 4 being a drift event's
 * only in the game.  So after a drift race the player drove with view 3 and
 * not the one they had picked (a player's report; measured: a career drift
 * race from view 1, 3 in free roam after it; a Quick Race drift race from
 * view 1, 3 in the next race), and the drift camera picked with the camera
 * key, or drift mode's, was 3 at the next race.
 * Here a setup that turns the drift camera into 3, or leaves it after a
 * drift event, gives the view the player had back: the one they had last
 * outside a drift event, a setup and drift mode's drift camera (picked with
 * the camera key or in the options, or a profile's).  A profile's view 4 is
 * taken for a drift event's (a career saved after a drift race, before free
 * roam's setup, has it: one of the test saves) and goes to 3 as in the game;
 * the drift camera is the player's own only as the camera key picks it.
 * Each setup is seen: 0x525ED0, 0x526970 (garage.c), 0x540780 and the loads'
 * 0x581820 (coop.c) through their overrides, which tell cameras_setup, and
 * free roam's 0x5404A0 (tail jumps reach it) and 0x53FEB0 and the career's
 * races (0x5322E0) through 0x4FE9F0, the race's settings, which each calls
 * (0x5404A0 after the views).  The views are looked at once a frame: the
 * world's camera is made from its view only when the world is (the Player,
 * 0x5F2A93), frames after the setup (150 after free roam's).  The drift event
 * keeps its drift camera, and a view picked in it stays, as in the game. */
#include "cpu.h"
#include "guest.h"
#include "game.h"
#include <stdio.h>

#define VIEW_LAST  4u                /* the drift camera */
#define VIEW_TO    3u                /* what the setups make of it */
#define VIEW_NONE  0xffffffffu       /* the player's own view not known */

long autotest_frame(void);
uint32_t drift_camera_view(void);   /* drift.c: the view its drift camera is in, 0 none */

static struct {
    int setup;                       /* a race's setup since the last frame */
    int known;
    uint32_t seen[2], own[2];        /* each player's view at the last frame, and the player's own (VIEW_NONE) */
    uint32_t keyed[2];               /* the view the camera key put there, while it stays (VIEW_NONE) */
} V = { .keyed = { VIEW_NONE, VIEW_NONE } };

/* ScrollDriveCam: as the game's, with the last view 4 */
void sub_00502540_orig(CPU *c);
void sub_00502540(CPU *c){
    uint32_t ps = c->ecx, step = MR32(c->esp + 4u), v = MR32(ps + PS_VIEW);
    if (step == 1u){ v++; if ((int32_t)v > (int32_t)VIEW_LAST) v = 0; MR32(ps + PS_VIEW) = v; }
    else if (step == 0xffffffffu){ v--; MR32(ps + PS_VIEW) = (int32_t)v < 0 ? VIEW_LAST : v; }
    else MR32(ps + PS_VIEW) = v;
    for (uint32_t n = 0; n < 2u; n++) if (ps == G_PSETTINGS + PS_SIZE * n) V.keyed[n] = MR32(ps + PS_VIEW);
    c->eax = v;
    c->edx = VIEW_LAST;
    POP32(c->eip);
    c->esp += 4u;
}

/* a race's setup (the human racers 0x525ED0 and the dyno's test run 0x526970: garage.c; free roam's 0x540780 and the
 * loads' 0x581820: coop.c) */
void cameras_setup(void){ V.setup = 1; }

/* the race's settings, at free roam's setups (0x53FEB0, 0x5404A0) and the career's races (0x5322E0) */
void sub_004fe9f0_orig(CPU *c);
void sub_004fe9f0(CPU *c){
    V.setup = 1;
    sub_004fe9f0_orig(c);
}

/* each frame: the views as the player picks them, and the player's back where a setup put the drift camera to 3 */
void cameras_frame(CPU *c){
    (void)c;
    int setup = V.setup, drift = MR8(G_DRIFTRACE) != 0;
    uint32_t ours = drift_camera_view();
    V.setup = 0;
    for (uint32_t n = 0; n < 2u; n++){
        uint32_t at = G_PSETTINGS + PS_SIZE * n + PS_VIEW, v = MR32(at);
        if (!V.known){ V.seen[n] = v; V.own[n] = v < VIEW_LAST ? v : VIEW_NONE; continue; }
        if (setup && !drift && V.seen[n] == VIEW_LAST && (v == VIEW_TO || v == VIEW_LAST) && V.own[n] <= VIEW_LAST && v != V.own[n]){
            fprintf(stderr, "[cameras] f=%ld player %u's view %u again (a race's setup had view %u after the drift camera)\n",
                    autotest_frame(), n + 1u, V.own[n], v);
            v = V.own[n];
            MR32(at) = v;
        } else if (!setup && !drift && at != ours){
            /* the drift camera only as the camera key picked it (a profile's or a poke's is no one's) */
            if (v < VIEW_LAST || V.keyed[n] == v) V.own[n] = v;
            else if (v != V.seen[n]) V.own[n] = VIEW_NONE;
        }
        if (v != VIEW_LAST) V.keyed[n] = VIEW_NONE;
        V.seen[n] = v;
    }
    V.known = 1;
}
