/* savegame.c -- the pause menu's Save: in career free roam the career saved
 * where the car stands, without a trip to the garage.
 *
 * The game saves the career after races, on leaving a shop, after a cover
 * shoot and on leaving the crib (Return to Explore Mode, with the autosave on
 * and something changed); driving around in between saves nothing, and the
 * place a save keeps for the car is where it last left free roam.  Career free
 * roam's pause menu (not an outrun's) now has Save second, after Return to
 * Explore Mode: the car's place and heading go where leaving free roam keeps
 * them, and the game's own save screen runs over the pause menu, as the crib
 * and the tuning test run's pause menu start it, whatever the Save option
 * (Autosave, Prompt, Off) says: the player asked for it (with Prompt and Off
 * the save screen asks first, as it does for the game's own saves).  Back on
 * the pause menu after it; Resume Career and the crib's Return to Explore Mode later
 * put the car where it was saved.  Greyed out while the car moves (STILL_MPS
 * or faster when the menu comes up), with no profile loaded, in a new
 * career's prologue (no car of its own yet: a state the game never saves),
 * in a free-roam race lobby (its trip out of free roam could come in the
 * middle of the save) and while a save runs.
 *
 * The game (SPEED2.EXE v1.2 NA):
 *  - Career free roam's pause menu (0x4C2900) starts with Return to Explore
 *    Mode (MU_PAUSE_EXPLORE 0xC3E5BCDC, class vtable 0x7954D8), added through
 *    the menu's AddOption (0x50DAB0, patched by photo.c, which calls
 *    savegame_item after each add).  Save goes in after it: an item of
 *    Replay's kind (0x51F670, vtable 0x7954F0, +0x20 greyed out) with the
 *    game's own label MU_PAUSE_SAVE_GAME (0x37D13AE5, "Save" in every
 *    language, which the PC game never shows) and the crib menu's own Save
 *    icon: that is a front-end texture (0x8ADA6FD4) the world does not have,
 *    so texrep_web.c copies it into INGAME_ICON_VOIP_MUTE (0x12F47B2F), which
 *    nothing in the PC game draws, and the item names that.  Its action is Replay's
 *    (0x4C26B0, thiscall: package, message, 3 more; ret 0x14), whose patch in
 *    photo.c asks savegame_accept first.
 *  - The career keeps the car's place for the world's next load at Career
 *    (0x85AD40) +0x8750 (float[3]) and +0x8760 (u16, the heading), +0x8744 1
 *    to put it there, as 0x513B30 takes them from the car (+0x60, +0x40C)
 *    when the pause menu's Quit leaves free roam; the profile saves the three
 *    (0x514530).  Only the world's loads read the flag (0x5404A0, 0x540780):
 *    it goes back to what it was once the save is over.
 *  - The save: 0x4A3100 (cdecl: the package that asks, the package to tell,
 *    the job, 4 more) with the job 0x151, as the crib's Return to Explore
 *    Mode (0x4AF383) and the test run's pause menu (0x5648DB) call it:
 *    MC_Main.fng over that package; its end (0x4A31D0) tells the package
 *    0x251BF83E and takes MC_Main off, and the job's word ([0x836514]) is 0
 *    again (0x4A3272; not after a job with 0x2000, so here MC_Main up is
 *    what says a save runs).  The save (0x4E8D30, state 5) asks "Are you
 *    sure you want to save the profile named %s?" (0x4A3C10, the job's
 *    0x4000000) unless the autosave is on ([0x844AD9]); a game's first save
 *    asks "Do you want to overwrite previously saved game?": the memory-card
 *    library (the save screen's manager [0x836498] +0x5C) asks it (its state
 *    0xD) while its flag 0x100 (+0x6C) is off, and state 6 turns that off
 *    again (0x4E8F17) while the manager's +6 is 0.  Its answer becomes the
 *    autosave: 0x4C6500 writes it to [0x844AD9] and +6, so a No there turns
 *    Autosave off, and the profile is saved so at the next save.  With
 *    Autosave on, Save sets the two first as that Yes leaves them (+6 1, the
 *    flag on through the game's own 0x6FBFCC): the player asked for the
 *    save, and it is written at once with no question; the save screen then
 *    says "Save complete." (Continue, its only button).
 *    With Prompt or Off the save screen asks as for the game's own saves
 *    (are you sure, overwrite, "Save complete.").
 *  - mprace.c's trip out of free roam (the pause menu's Quit) waits while
 *    savegame_busy(), the player's input theirs.
 *
 * Log lines start "[save]"; tests/scripts/pause_save.txt checks it in the
 * game. */
#include "cpu.h"
#include <math.h>
#include <stdio.h>
#include <strings.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);
int career_fe_active(const char *name);
int mprace_car_ok(void);              /* mprace.c: the career has a car of its own (not a new one's prologue) */
int roam_game_in_lobby(void);         /* roam_game.c: in a free-roam race lobby (its trip can start any moment) */

#define HOOK_RET       0x00581475u   /* the main-loop hook's return address, as guest calls see it */
#define G_GAMEMODE     0x00850078u   /* 1 the career */
#define G_RACEKIND     0x0089e7b0u   /* 0 free roam, 7 an outrun */
#define G_PLAYER1      0x008900acu   /* Player*: +4 its car */
#define G_PROFILE      0x0083a9e0u   /* the profile's name, 32 bytes; +0x20 1: a profile is loaded */
#define G_MC_JOB       0x00836514u   /* the save screen's job: 0 none */
#define G_MC_MANAGER   0x00836498u   /* the save screen's manager: +6 the overwrite question answered Yes this game, */
#define MGR_YES        6u
#define MGR_LIB        0x5cu         /* ... +0x5C the memory-card library ([it] the library: +0x6C its flags) */
#define FN_LIB_FLAGS   0x006fbfccu   /* thiscall (the +0x5C) (on, bits; ret 8): the library's flags on or off */
#define LIB_ASKED      0x100u        /* ... set: the overwrite question answered Yes (not asked again) */
#define G_AUTOSAVE     0x00844ad9u   /* byte: Save at Autosave */
#define G_CAREER       0x0085ad40u
#define C_KEEP         0x8744u       /* byte: the world's next load puts the car at the place below */
#define C_PLACE        0x8750u       /* float[3] */
#define C_HEADING      0x8760u       /* u16 */
#define CAR_PLACE      0x60u
#define CAR_VELOCITY   0x70u
#define CAR_HEADING    0x40cu
#define FN_NEW         0x00575620u   /* cdecl (size): operator new */
#define FN_ITEM        0x0051f670u   /* thiscall (icon, label, 0; ret 0xC): a menu item's constructor */
#define FN_ADD_ITEM    0x0050dab0u   /* thiscall (item; ret 4): a menu's AddOption */
#define FN_MC_JOB      0x004a3100u   /* cdecl (package, package to tell, job, 0, 0, 0, 0) */
#define VT_EXPLORE     0x007954d8u   /* Return to Explore Mode's class */
#define VT_REPLAY      0x007954f0u
#define LBL_EXPLORE    0xc3e5bcdcu   /* MU_PAUSE_EXPLORE "Return to Explore Mode" */
#define LBL_SAVE       0x37d13ae5u   /* MU_PAUSE_SAVE_GAME "Save" */
#define ICON_SAVE      0x12f47b2fu   /* INGAME_ICON_VOIP_MUTE, unused: texrep_web.c copies the crib menu's Save icon in */
#define ITEM_BYTES     0x48u
#define IT_GREY        0x20u
#define IT_LABEL       0x2cu
#define MSG_ACCEPT     0x0c407210u
#define MC_JOB_SAVE    0x151u        /* the crib's: save the profile */
#define STILL_MPS      1.0f          /* standing still: slower than this (3.6 km/h) */
#define PLACE_MAX      100000.f      /* farther out than this (m): no place on the map (carspot.c) */
#define WAIT_FRAMES    14400         /* a save not over by then (2 minutes at 120 fps): the flag back anyway */
#define MC_PACKAGE     "MC_Main.fng"

static struct {
    int pending, keep_was, writes_at;
    long at;
} S;
static int g_writes;                 /* profiles the game wrote (fileio.c) */

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }

/* a guest function with ecx and stack arguments, the CPU as it was after */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(HOOK_RET);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

static uint32_t player_car(void){ uint32_t p = rd32(G_PLAYER1), car = p ? rd32(p + 4u) : 0; return ok(car, CAR_HEADING + 2u) ? car : 0; }

/* why Save cannot run now (0: it can) */
static const char *not_now(void){
    if (MR32(G_GAMEMODE) != 1u || MR32(G_RACEKIND) != 0u) return "not career free roam";
    if (!MR8(G_PROFILE + 0x20u)) return "no profile loaded";
    if (!mprace_car_ok()) return "no car of the career's own yet";
    if (roam_game_in_lobby()) return "in a race lobby";
    if (S.pending || career_fe_active(MC_PACKAGE)) return "a save runs";
    uint32_t car = player_car();
    if (!car) return "no car";
    float v2 = 0.f;
    for (uint32_t i = 0; i < 3u; i++){
        float p = MRF32(car + CAR_PLACE + 4u * i), v = MRF32(car + CAR_VELOCITY + 4u * i);
        if (!isfinite(p) || fabsf(p) > PLACE_MAX || !isfinite(v)) return "no place on the map";
        v2 += v * v;
    }
    if (!(v2 < STILL_MPS * STILL_MPS)) return "moving";
    return 0;
}

/* photo.c's AddOption patch, after the game's add: after Return to Explore Mode, Save */
void savegame_item(CPU *c, uint32_t menu, uint32_t it){
    if (!ok(it, IT_LABEL + 4u) || MR32(it) != VT_EXPLORE || MR32(it + IT_LABEL) != LBL_EXPLORE) return;
    if (MR32(G_GAMEMODE) != 1u || MR32(G_RACEKIND) != 0u) return;
    uint32_t n = ITEM_BYTES, sv = gcall(c, FN_NEW, 0, 1, &n);
    if (!ok(sv, ITEM_BYTES)) return;
    uint32_t arg[3] = { ICON_SAVE, LBL_SAVE, 0 };
    gcall(c, FN_ITEM, sv, 3, arg);
    MR32(sv) = VT_REPLAY;
    const char *why = not_now();
    MR8(sv + IT_GREY) = why ? 1 : 0;
    gcall(c, FN_ADD_ITEM, menu, 1, &sv);
    fprintf(stderr, "[save] f=%ld the pause menu's Save%s%s%s\n", autotest_frame(), why ? " (greyed out: " : "", why ? why : "", why ? ")" : "");
}

/* photo.c's patch of Replay's action, first: Save's Accept saves; 1 when it was Save's (the call returned) */
int savegame_accept(CPU *c){
    uint32_t it = c->ecx, pkg = MR32(c->esp + 4u), msg = MR32(c->esp + 8u);
    if (!ok(it, IT_LABEL + 4u) || MR32(it + IT_LABEL) != LBL_SAVE || MR32(it) != VT_REPLAY) return 0;
    if (msg == MSG_ACCEPT && !MR8(it + IT_GREY)){
        const char *why = not_now();
        if (why) fprintf(stderr, "[save] f=%ld Save: not now (%s)\n", autotest_frame(), why);
        else {
            uint32_t car = player_car(), k = G_CAREER;
            S.keep_was = MR8(k + C_KEEP);
            for (uint32_t i = 0; i < 3u; i++) MRF32(k + C_PLACE + 4u * i) = MRF32(car + CAR_PLACE + 4u * i);
            MR16(k + C_HEADING) = MR16(car + CAR_HEADING);
            MR8(k + C_KEEP) = 1;
            uint32_t mgr = rd32(G_MC_MANAGER), lib = ok(mgr, MGR_LIB + 3u) ? rd32(mgr + MGR_LIB) : 0;
            if (MR8(G_AUTOSAVE) && lib){                     /* no "overwrite?", whose No turns Autosave off */
                MR8(mgr + MGR_YES) = 1;
                uint32_t on[2] = { 1u, LIB_ASKED };
                gcall(c, FN_LIB_FLAGS, lib, 2, on);
            }
            uint32_t arg[7] = { pkg, pkg, MC_JOB_SAVE, 0, 0, 0, 0 };
            S.pending = 1; S.at = autotest_frame(); S.writes_at = g_writes;
            fprintf(stderr, "[save] f=%ld Save: the career at %.1f %.1f %.1f (heading %04x), the save screen\n", S.at,
                    (double)MRF32(k + C_PLACE), (double)MRF32(k + C_PLACE + 4u), (double)MRF32(k + C_PLACE + 8u), MR16(k + C_HEADING));
            gcall(c, FN_MC_JOB, 0, 7, arg);
        }
    }
    POP32(c->eip);                   /* thiscall, 5 arguments */
    c->esp += 0x14u;
    return 1;
}

/* fileio.c: the game wrote a profile */
void savegame_profile_written(void){ g_writes++; }

/* 1 while Save's save runs, or any save screen is up (mprace.c waits) */
int savegame_busy(void){ return S.pending || career_fe_active(MC_PACKAGE); }

/* the main-loop hook: the save over (its job done, MC_Main off), the career's flag as it was */
void savegame_frame(void){
    if (!S.pending) return;
    long f = autotest_frame();
    int up = MR32(G_MC_JOB) || career_fe_active(MC_PACKAGE);   /* MC_Main comes up through the front end's queue */
    if ((up || f - S.at < 10) && f - S.at < WAIT_FRAMES) return;
    MR8(G_CAREER + C_KEEP) = (uint8_t)S.keep_was;
    S.pending = 0;
    int w = g_writes - S.writes_at;
    fprintf(stderr, "[save] f=%ld Save: %s after %ld frames (%s)\n", f, up ? "the save screen still up" : "over", f - S.at,
            w ? "the profile written" : "nothing written");
}
