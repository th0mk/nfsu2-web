/* Most Wanted's state for the page (runtime/game.h StatusOut, published by runtime/web/status_web.c at every
 * Present): "drive" for the touch controls (web/src/app/touch.ts), the render check and the copy of the log, "pause"
 * for the pause hint, "rate" for the render check.  UG2's are game_ug2.c's; the words mean the same here:
 *   drive  1 while the player drives in a race (from the countdown to the line: once the race's first racer, the
 *          player, has finished, the game drives), 2 while the player drives outside one (free roam, a Challenge
 *          Series pursuit), 0 elsewhere; | 8 at a race's start (the HUD up, the race's timer not running yet: the
 *          countdown; seen 185 frames in status_race.txt), | 16 while the cops chase the player (Most Wanted's own: UG2 leaves it 0).  (| 4, a manual
 *          gearbox, is not set: where Most Wanted keeps the player's Transmission choice is not known yet.)
 *   pause  1 while the game's pause menu (Pause_Main.fng) is the package that takes input: not its options or a
 *          question over it
 *   rate   the frame rate the game paces to now (framerate_mw.c)
 * Driving is GameFlow 6 with a HUD package active (HUD_SingleRace.fng in races, free roam and pursuits) and no package
 * taking input (FEPackage +0x28, fe_mw.c mw_fe_input_top): the pause menu, the results, the post-race and engage
 * screens and the loading screens take input or are no HUD, and while the HUD's package is not in the active list
 * (a race's opening camera, the story's cutscenes, the world's loading) nobody drives.  Seen in the logs of
 * career_start.txt and pursuit_rap.txt: the race's start camera (Scene_GenericStart) runs with no HUD, which comes in
 * some 190 frames before GO, the countdown; a cutscene in free roam (IntroNis05) with InGameBackground.fng and no
 * HUD; the top package itself is no help (EA_TRAX.fng, the song's name, stays on top while the player drives).
 * The race and the chase need the game's own calls (the player's interfaces: career_mw.c says how), so the frame
 * anchor samples them every SAMPLE passes (mw_status_frame, game_mw.c mw_on_frame) and the Present reads what it
 * kept; the rest are reads of memory at the Present. */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
long metal_frames(void);
int hfr_rate_now(void);                                     /* framerate_mw.c */
const char *mw_fe_input_top(uint32_t *focus);               /* fe_mw.c */
const char *mw_fe_find(const char *prefix);                 /* fe_mw.c */

#define G_RACESTATUS     0x0091e000u     /* GRaceStatus (career_mw.c): +0x198A (byte) actively racing */
#define RS_ACTIVE        0x198au
#define RS_GO            0x16cu          /* the first racer's timer flag (GRacerInfo +0x154): the race's GO */
#define RS_DONE          0x39u           /* the first racer's finished byte (GRacerInfo +0x21; the racers from +0x18) */
#define G_PLAYERS        0x0092d87cu     /* IPlayer *[]; count at G_NPLAYERS; IPlayer method +4: its ISimable */
#define G_NPLAYERS       0x0092d884u
#define FN_QUERY         0x005d59f0u     /* an object's interface by id (thiscall (id)) */
#define IID_AIVEH        0x00403700u     /* the vehicle's AI interface: +0x70 the pursuit object, 0 without a chase */
#define AIVEH_PURSUIT    0x70u
#define SAMPLE           8u              /* frame anchor passes between two samples */

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args){
    if (!fn) return 0;
    uint32_t keep = c->ecx; c->ecx = self;
    uint32_t r = call_guest(c, fn, n, args);
    c->ecx = keep;
    return r;
}

static int g_pursuit;                   /* the last sample: a chase is on */

/* the frame anchor (game_mw.c mw_on_frame): the chase, every SAMPLE passes while the game is in a world */
void mw_status_frame(CPU *c){
    static unsigned n;
    if (n++ % SAMPLE) return;
    if (game_flow() != 6u || !rd(G_NPLAYERS)){ g_pursuit = 0; return; }
    uint32_t pl = rd(rd(G_PLAYERS)), vt = rd(pl);
    uint32_t sim = vt ? gcall(c, rd(vt + 4u), pl, 0, 0) : 0u, obj = sim ? rd(sim + 4u) : 0u, id = IID_AIVEH;
    uint32_t ai = obj ? gcall(c, FN_QUERY, obj, 1, &id) : 0u;
    g_pursuit = ai && rd(ai + AIVEH_PURSUIT) ? 1 : 0;
}

static int drive_state(int *pause){
    unsigned flow = game_flow();
    uint32_t focus = 0;
    const char *in = mw_fe_input_top(&focus), *hud = mw_fe_find("HUD_");
    uint32_t rs = rd(G_RACESTATUS);
    int race = rs && mem_is_committed(rs + RS_ACTIVE) ? MR8(rs + RS_ACTIVE) != 0 : 0;
    int go = race && mem_is_committed(rs + RS_GO) ? MR8(rs + RS_GO) != 0 : 0;
    int done = race && mem_is_committed(rs + RS_DONE) ? MR8(rs + RS_DONE) != 0 : 0;
    int v = flow == 6u && hud && !in && !done ? (race ? 1 : 2) : 0;
    if (v == 1 && !go) v |= 8;
    if (v && g_pursuit) v |= 16;
    *pause = in && !strcasecmp(in, "Pause_Main.fng");
    /* a line at each change (in a world: of the package that takes input too), for a player's log */
    static unsigned l_flow = ~0u; static int l_v = -1, l_p = -1, l_race = -1, l_go = -1, l_done = -1, l_hud = -1;
    static char l_in[48];
    if (flow != l_flow || v != l_v || *pause != l_p || race != l_race || go != l_go || done != l_done || !!hud != l_hud ||
        (flow == 6u && strcmp(in ? in : "-", l_in))){
        l_flow = flow; l_v = v; l_p = *pause; l_race = race; l_go = go; l_done = done; l_hud = !!hud;
        snprintf(l_in, sizeof l_in, "%s", in ? in : "-");
        fprintf(stderr, "[drive] f=%ld flow=%u hud=%s input=%s race=%d go=%d done=%d pursuit=%d -> %d pause=%d\n", metal_frames(),
                flow, hud ? hud : "-", l_in, race, go, done, g_pursuit, v, *pause);
    }
    return v;
}

/* game.h status: at every Present */
void mw_status(StatusOut *s){
    s->drive = drive_state(&s->pause);
    s->rate = hfr_rate_now();
}
