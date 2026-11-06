/* bf_dragsteer.c -- Bayview Forever's Real Drag Steering: a drag race keeps the
 * normal driving controls (full steering) instead of the game's slot-car lane
 * changes.
 *
 * The game (SPEED2.EXE v1.2 NA, as read for this file):
 *  - Every Player (the list [0x8900B4], SPLIT_PLAYERS_ORDER here) is updated each
 *    frame by the player update (the code from 0x602A58, reached from 0x60A240).
 *    It loads EDI = 3 (PLAYER_MODE_DRAG_RACING) at 0x602A5D and, for a player
 *    with a car ([Player +4]) whose car controller type ([car +0x24]: 1 the normal
 *    human controller, 2 the drag one) is 2, switches back to normal (0x5F3080)
 *    when the player's mode ([Player +0x22C]) is no longer 3; otherwise, when the
 *    mode is 3, it calls Player_SwitchToDragController (0x5F3010, thiscall Player,
 *    no arguments, plain RET; its only caller is 0x602A98).
 *  - 0x5F3010: unless the car's controller is type 2 already, 0x5EBA30(2) on the
 *    car makes a new controller of type 2 ([car +0x2C], [car +0x24] = 2); then
 *    the car's input object ([car +0x30]) gets the player's port (vtable +0x10,
 *    [Player +8]), +0x1C(0) and the gearbox (+0x34: 1 in a drag race with
 *    [0x864FAC] = 0, else [Player +0x20], the player's Transmission choice), and
 *    its +0x14.  The type-2 controller is the lane steering: left/right change
 *    lanes, the car steers itself.  0x5F3080 is the same with type 1.
 *  - Force feedback: 0x4064BF asks the player's option (0x5EB420), then
 *    RaceOptions_IsDragRace (0x4018F0, [0x89E7A0 +0x38] = [0x89E7D8]) and JNE
 *    0x40692A at 0x4064D8 skips every road-feel effect in a drag (only a centring
 *    spring).  The mod NOPs that JNE when [DragSteering] ForceFeedback = 1.  The
 *    port has no force feedback (dinput_pad.c is the only controller the game
 *    sees, a gamepad with no effects): that option is read for the page and the
 *    log only.
 *
 * What the mod does (BayviewForever.asi 0x712C6770): with [Features]
 * DragSteering = 1 and [DragSteering] NormalSteering = 1 it rewrites the CALL at
 * 0x602A98 (E8 73 05 FF FF -> CALL 0x712C6680, a bare RET): the switch never
 * happens, so the car keeps controller type 1 for the whole drag.  The switch
 * back (0x5F3080) only runs for a type-2 controller, so it never runs either.
 * Here: 0x5F3010 overridden, a plain return while the setting is on (its one
 * caller, so the same as the mod's patch).
 *
 * What stays the drag race's own: the race (the drag byte [0x89E7D8], its HUD,
 * its physics set: drift.c's notes, set 2), the AI's lanes, the music
 * (drift.c race_music).  Untested in the port (see the spec): whether anything
 * drag-specific (perfect shift / launch, the engine's heat) lives in the type-2
 * controller, and which gearbox the car has in a drag when the switch never
 * sets +0x34 (the car's attach sets it; status_web.c says manual in a drag).
 *
 * Every local player is affected (the update runs per Player: split screen and
 * co-op's player 2 too).  In a network drag (LAN / Online) each game decides
 * for its own car only: nothing of the controller type is sent (racecars.c and
 * the game's car states carry place, velocity and the controls), so a player
 * with the setting on steers freely while the others change lanes; no desync,
 * but not the same race.
 *
 * Settings (the page writes them; the Mac app passes the ini's own):
 *   NFSU2_BF_FEATURES_DRAGSTEERING        0/1, default 1  ([Features] DragSteering)
 *   NFSU2_BF_DRAGSTEERING_NORMALSTEERING  0/1, default 1  ([DragSteering] NormalSteering)
 *   NFSU2_BF_DRAGSTEERING_FORCEFEEDBACK   0/1, default 1  (no force feedback here: logged only)
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

long autotest_frame(void);

#define G_DRAGRACE  0x0089e7d8u   /* byte: a drag event */
#define PL_CAR      0x04u         /* Player: its car */
#define PL_MODE     0x22cu        /* Player: its mode (3 drag racing) */
#define CAR_CTRL    0x24u         /* car: its controller's type (1 normal, 2 drag lanes) */

static int g_on = -1;
static int env_on(const char *name, int def){
    const char *e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
}
static void dragsteer_init(void){
    if (g_on >= 0) return;
    int feat = env_on("NFSU2_BF_FEATURES_DRAGSTEERING", 0);
    int normal = env_on("NFSU2_BF_DRAGSTEERING_NORMALSTEERING", 1);
    int ffb = env_on("NFSU2_BF_DRAGSTEERING_FORCEFEEDBACK", 1);
    g_on = feat && normal;
    fprintf(stderr, "[bf-drag] Real Drag Steering %s (DragSteering=%d NormalSteering=%d; ForceFeedback=%d: no force feedback in this port)\n",
            g_on ? "on: drag races keep the normal steering" : "off: the game's lane steering", feat, normal, ffb);
}
/* for bf_hshifter.c ([HShifter.Gearbox] DragRaces needs the normal controls in a drag) */
int bf_dragsteer_on(void){ dragsteer_init(); return g_on; }

/* thiscall Player: Player_SwitchToDragController (plain RET) */
void sub_005f3010_orig(CPU *c);
void sub_005f3010(CPU *c){
    dragsteer_init();
    if (!g_on){ sub_005f3010_orig(c); return; }
    static uint32_t told[6]; static unsigned nt; /* the Players told of (a line each, once per Player) */
    uint32_t pl = c->ecx; unsigned k = 0;
    while (k < 6u && told[k] != pl) k++;
    if (k == 6u){
        told[nt++ % 6u] = pl;
        uint32_t car = mem_is_committed(pl + PL_CAR + 3u) ? MR32(pl + PL_CAR) : 0u;
        fprintf(stderr, "[bf-drag] f=%ld drag race (byte %u): player 0x%08x mode %u keeps the normal controller (car 0x%08x type %u)\n",
                autotest_frame(), MR8(G_DRAGRACE), pl, mem_is_committed(pl + PL_MODE + 3u) ? MR32(pl + PL_MODE) : 0u, car,
                car && mem_is_committed(car + CAR_CTRL + 3u) ? MR32(car + CAR_CTRL) : 0u);
    }
    POP32(c->eip);                               /* the mod's CALL to a bare RET */
}
