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
 * drift.c's drift mode (X) puts view 4 on and gives the one before back. */
#include "cpu.h"

#define VIEW_LAST  4u
#define PS_VIEW    0x10u

/* ScrollDriveCam: as the game's, with the last view 4 */
void sub_00502540_orig(CPU *c);
void sub_00502540(CPU *c){
    uint32_t ps = c->ecx, step = MR32(c->esp + 4u), v = MR32(ps + PS_VIEW);
    if (step == 1u){ v++; if ((int32_t)v > (int32_t)VIEW_LAST) v = 0; MR32(ps + PS_VIEW) = v; }
    else if (step == 0xffffffffu){ v--; MR32(ps + PS_VIEW) = (int32_t)v < 0 ? VIEW_LAST : v; }
    else MR32(ps + PS_VIEW) = v;
    c->eax = v;
    c->edx = VIEW_LAST;
    POP32(c->eip);
    c->esp += 4u;
}
