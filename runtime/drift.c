/* drift.c -- drift mode in free roam: X turns on the game's own drift-event
 * handling, its drift camera and its drift scoring while the player drives
 * around the city, and off again.
 *
 * Every test the game makes of "is this a drift event?" is the race's drift
 * byte ([0x89E7D9], race parameters +0x39) or the dword [0x890104], which
 * nothing in the game sets (drag has [0x890108], Street X [0x89010C]): a
 * developers' switch, read as the byte is in the helper 0x401910 and about a
 * hundred inlined copies.  Set, the car's physics take their drift-event
 * branches every step (the drift assist 0x5AC260, the steering's drift
 * curve: docs/port/drift-fidelity.md in the native repo, 2.2), the car's
 * physics are made as a drift event's when they are made again (below), the
 * camera key has a fifth view, the drift camera
 * (PlayerSettings::ScrollDriveCam 0x502540), and the race
 * coordinator's tick of each racer runs the drift tick (0x6016A0 ->
 * 0x5F7A00), whose first call makes the racer's DriftCar.  Free roam's
 * coordinator has one racer, the player, with a DriftScore and a style
 * monitor already (the style messages of free roam's mode: a jump's HANG
 * TIME).
 *
 * What free roam lacks for the scoring, added while drift mode is on:
 *  - the drift event's styles: a DriftScore holds a style for each entry of
 *    the style table ([0x864624], 39 of 0x30 bytes) whose mode mask (+7)
 *    has the bit of the event's mode, made by its constructor (0x56EBE0):
 *    free roam's have bit 1, a drift event's bit 4, among them STYLE_DRIFT
 *    (entry 0) and the "Good drift!" messages' (entries 8-14, read by
 *    0x56CCD0).  The empty slots of those are filled once per DriftScore as
 *    the constructor fills one (a style from the styles' pool [0x864628],
 *    0x440E50, reset by 0x56D650); the DriftScore frees them with its own.
 *  - a route: free roam has none (the car's route distance
 *    [[car +0x1C] +0x14] is -1), and the point rate (0x56C940) is 0 while
 *    that is below the DriftCar's best (+0x30, 0 when it is made).  Its best
 *    follows it down.
 * The rest is the game's: the rate, the multiplier and its "Gain 2X with"
 * points, banking after 0.3 s without a drift, a wall hit losing the drift.
 * The page shows it (web/src/app/drift.ts): free roam's HUD (HUD_SingleRace)
 * shows none of it (the drift event's panel and DRIFT popup are HUD_Drift's
 * objects; the HUD object has its drift part, [HUD +0x2C], all the same, in
 * the prologue and in a career).  Three of the drift event's routines are
 * skipped while the switch is ours (the overrides at the end):
 *  - the style points' drift branch (0x4C85D0, from their update 0x4DA900):
 *    the drift part's popup, then the panel's numbers into the style points'
 *    objects (+0x3C .. +0x54).  Their constructor (0x4F7AD0) looks +0x3C up
 *    only with the switch on and otherwise leaves what the heap had there;
 *    free roam's HUD is made with it off, so the first number went through
 *    that (0 in the tests; for players a stale pointer, out of bounds in
 *    0x537B30 at their first X);
 *  - the drift messages ("Good drift!", "Too slow!": FEngHud::
 *    ShowNiftyDriftyMessage 0x4AC120, the drift part's +0x74);
 *  - the drift leaderboard (DriftManager 0x56DDD0, each frame: the scores of
 *    the world's AI cars, each a racer of the event; free roam's, its tuners
 *    and friends' cars, are none: it read through their racer entries and
 *    wrote their scores through leaderboard entries it did not find, both 0).
 * With the guest's first 4 KB filled with 0xDEADBEEF, drift mode then ran
 * through drifts, the pause menu and friends' cars about (the leaderboard
 * had trapped).  Free roam's style popup (the popup list's title and
 * points, the style points' +0x38 and +0x34) is the list's own (0x4DA710),
 * which only runs while the switch is off; while it is on they came up all
 * the same, with the objects' own text ("HANGY TIMEY!", "+ 25"), and stayed,
 * so they are kept hidden then.
 *
 * The car's physics (its ride's block from +0x10) are made from the ride's
 * parts and dyno setup (+0x398) when the car is, by 0x5B4FC0 (through
 * 0x639CE0): with the switch or a drift event's byte set, from the car
 * type's drift parameters (set 3; a drag's 2, Street X's 10).  A race's
 * setup puts the dyno setup of its kind in the ride first: the career car's
 * record (+0x360) keeps 5 of 0x29 bytes (0 circuit and sprint, 1 drag, 2
 * drift, 3 and 4 the others), and the record's 0x516AC0 copies one (career
 * races 0x525ED0 with the kind's: a drift event's 2, 0x53F830; free roam's
 * 0x53FEB0 always 0).  Free roam's car is made with neither, and nothing
 * makes it again: drift mode had none of a drift event's physics.  So when
 * the switch comes on, the ride takes the record's Drift setup (if it has
 * other values) and the car is made again (the car's own 0x5F6C20, as the
 * dyno's tuning screens have it done: 0x50FDD0); when it goes off, the setup
 * the ride had and the car made again once more.  Measured (a stage 3
 * career, ride +0x10 .. +0x580): X made no value differ before; now 3 with
 * the Drift setup left as it came and 90 with one of its own, each as a
 * drift event (S3_DRIFT_9) has it, the 4 left a race's own; X off gave every
 * value back.  Friends' games are sent the ride's own setup meanwhile
 * (drift_kept_tuning, roam_game.c's car look): another look would have the
 * car loaded again in each.
 *
 * The nitrous stays as it was: a drift event has none (set 3's 0x5A7750 sets
 * the shot's length, the physics' [car +0x20] +0x2A0 in seconds, to 0), and
 * 0x5F6C20 fills the car's nitrous (+0x40, 0x591CD0) again from that length
 * whenever the shot it has (+0x414 in 1/4000 s, times 0.00025f) does not
 * compare equal to it, which it hardly ever does (0.00025f is a little under
 * 1/4000).
 * Made again for X, the car had no nitrous in drift mode, and a full shot
 * back after it (X twice, a pause: free nitrous).  Players asked for it in
 * drift mode, where it went with the drifts ("Bring back nitro in drift
 * mode"), so the shot's length goes back after the car is made again for X,
 * and the nitrous's state (+0x414 .. +0x420) after each time.
 *
 * The camera: on, the drift camera (view 4) as the camera key reaches it
 * (Player 0x5F2FA0: PlayerSettings view, [0x83AA64 + 0x28 n] +0x10, then
 * the camera for it); off, the view the player had, if it is still the
 * drift camera.  A view 4 outside a drift event is the game's own trouble
 * too: free roam's setup puts it back to 3 (0x540599).
 *
 * The music: a drift or drag event has none.  The music manager's tick
 * (0x482180) plays the EA Trax songs in the world only when the race is no
 * drift (0x401910) and no drag event (0x4018F0), and switches to its other
 * player otherwise; the in-game mix (0x471970) turns the music's channel all
 * the way down for both (its attenuation, [this +0x3C] and [0x7ED344], 0x7FFF).
 * Both read the switch, so drift mode silenced the music, and the second X
 * started a new song.  While the switch is ours neither sees it (drift_hook):
 * free roam's song plays on.
 *
 * Only in free roam itself: the world, free roam's race (kind 0, its one
 * racer, racing: 6 is paused), no career race, not Online or LAN.  A menu,
 * the map, a message or a race card over the HUD switches the handling off
 * until it is gone (a race set up from a race card never sees it); so do an
 * outrun (kind 7) and leaving free roam, which give the camera back too.
 * Drift mode stays chosen and comes back in free roam.  X switches it in
 * free roam's world (paused too: it comes on when the driving does), and
 * anywhere else does nothing but tell the page.
 *
 * The page (web/src/app/drift.ts) reads the status table (status_web.c):
 *   drift        bits: 1 drift mode chosen, 2 on now (the panel shows),
 *                4 the last X was outside free roam
 *   drift_key    X presses so far (the page's note on each)
 *   drift_x      the multiplier (1-5)       drift_gain   its next level's points (0 at 5)
 *   drift_total  points banked in drift mode since the game started
 *   drift_best   the most of one drift
 *   drift_st     STYLE_DRIFT's state: 0 a drift that counts, 1 starting, 2 none, 3 banked, 4 lost (a wall)
 *   drift_pend   its points so far (the page counts them up, as the drift event's HUD does) */
#include "cpu.h"
#include "kbd.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <string.h>
#include <strings.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);

/* status_web.c in the browser; the Mac has no page */
__attribute__((weak)) int host_status_slot(const char *name){ (void)name; return -1; }
__attribute__((weak)) void host_status_set_n(const int *slot, const double *value, int n){ (void)slot; (void)value; (void)n; }

#define DIK_X          0x2du         /* in none of the game's key maps (its F2 is the map) */
#define HOOK_RET       0x00581475u   /* the main-loop hook's return address, as guest calls see it */
#define G_FLOW         0x008654a4u   /* 6: in the world */
#define G_GAMEMODE     0x00850078u   /* 1 career, 8 Online, 0x200 LAN */
#define G_RACE         0x00890118u   /* the race coordinator: +0 state (4 racing, 6 paused), +0x24 racers, +0xBD0 the racers */
#define G_RACEKIND     0x0089e7b0u   /* 0 free roam, 7 an outrun */
#define G_DRIFTRACE    0x0089e7d9u   /* byte: a drift event */
#define G_FORCEDRIFT   0x00890104u   /* the drift-event switch */
#define G_CAREER_RACE  0x00863470u   /* the career race being run (0 = free roam) */
#define G_PLAYER1      0x008900acu   /* Player* of player 1, then 2: +4 its car, +0x10 its number, +0x9A4 its HUD */
#define G_PSET         0x0083aa64u   /* PlayerSettings, 0x28 each: +0x10 the camera view */
#define G_FENG         0x008384c4u   /* +8 the package manager: +0xE4 the top package, +0xC its name */
#define G_STYLES       0x00864624u   /* the style table: 0x30 each, +7 the modes' mask */
#define G_STYLEPOOL    0x00864628u
#define G_CAREER_CAR   0x00863480u   /* the career car's handle (CAREER_SLOT_n) */
#define G_FECARS       0x0083ad90u   /* player 1's front-end cars (garage.c's CARS0): the career car's record */
#define FN_POOL_ALLOC  0x00440e50u   /* cdecl (pool) */
#define FN_CAR_TUNED   0x005f6c20u   /* thiscall car (1): its physics again, from its ride's tuning and parts */
#define FN_STYLE_INIT  0x0056d650u   /* thiscall style */
#define FN_CAMERA_KEY  0x005f2fa0u   /* thiscall Player (step): the next camera view */
#define FN_FE_HIDE     0x0050ca00u   /* cdecl (FE object, 0: none) */
#define HUD_STYLES     0x4cu         /* HUD: its style points (0x4DA900): +0x34, +0x38 the popup list's title and points */
#define HUD_DRIFT      0x2cu         /* HUD: its drift part (0x4DA590) */
#define N_STYLES       39
#define MODE_DRIFT     4u
#define RC_DRIFTSCORE  0x1d4u        /* racer: DriftScore* (a style* per entry), DriftCar*, the style monitor */
#define RC_DRIFTCAR    0x1d8u
#define DC_CAR         0x08u         /* DriftCar: its car */
#define DC_LEVEL       0x1cu         /* the multiplier */
#define DC_GAIN        0x20u         /* the next level's points */
#define DC_BEST        0x30u         /* the farthest route distance */
#define ST_STATE       0x14u         /* a style: its state (drift_st) */
#define ST_PEND        0x24u         /* the drift's points so far (float) */
#define ST_TOTAL       0x2cu         /* points banked (float) */
#define PL_HUD         0x9a4u
#define VIEW_DRIFT     4u
#define CAR_RIDE       0x10u         /* car: its ride (RideInfo) */
#define CAR_PHYS       0x20u         /* car: its physics' values (+0x2A0 the nitrous shot's length, s) */
#define PH_NOS_LEN     0x2a0u
#define CAR_NOS_STATE  0x454u        /* car: its nitrous's state (+0x40 +0x414: the shot, +0x418, left, on since), 4 dwords */
#define RIDE_TUNING    0x398u        /* ride: the dyno setup it drives with */
#define REC_SETUPS     0x360u        /* a car record: its 5 dyno setups, SETUP_SZ bytes each */
#define SETUP_SZ       0x29u
#define SETUP_DRIFT    2u            /* 0 circuit and sprint, 1 drag, 2 drift, 3 and 4 the others */

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void cstr(uint32_t a, char *o, int n){
    int i = 0;
    for (; a && i + 1 < n && ok(a + (uint32_t)i, 0) && MR8(a + (uint32_t)i); i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
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

static struct {
    int want, on, cam, note;          /* chosen, the switch set now, our camera, the last X outside free roam */
    uint32_t saved;                   /* the view before the drift camera */
    uint32_t style;                   /* STYLE_DRIFT's style, and its total when last seen */
    float last;
    double total, best;
    uint32_t keys, kseq; int kinit;
    int slot[8]; double val[8]; int slots;
    uint32_t tuned; int swapped;      /* the car whose physics are the switch's; the Drift setup in its ride */
    uint8_t kept[SETUP_SZ];           /* ... and the setup the ride had */
} D;

/* X presses since the last frame, from the keyboard's events (a tap between frames counts) */
static int x_presses(void){
    uint32_t head = kbd_ev_head();
    if (!D.kinit){ D.kinit = 1; D.kseq = head; return 0; }
    int n = 0;
    for (; D.kseq != head; D.kseq++){
        KbdEv e;
        int r = kbd_ev_get(D.kseq, &e);
        if (r == 0) break;
        if (r > 0 && e.dik == DIK_X && e.down) n++;
    }
    return n;
}
static uint32_t race(void){ uint32_t r = rd32(G_RACE); return ok(r, 0xbd4) ? r : 0; }
static uint32_t racer(void){ uint32_t r = race(), p = r ? rd32(r + 0xbd0u) : 0; return ok(p, RC_DRIFTCAR + 4u) ? p : 0; }
/* free roam's world: its own race, paused or not */
static int roam_world(void){
    uint32_t r = race(), pl = rd32(G_PLAYER1);
    return rd32(G_FLOW) == 6u && r && MR32(r + 0x24u) == 1u && (MR32(r) == 4u || MR32(r) == 6u) && !rd32(G_CAREER_RACE) &&
           rd32(G_RACEKIND) == 0u && !(rd32(G_GAMEMODE) & 0x208u) && !MR8(G_DRIFTRACE) && pl && rd32(pl + 4u);
}
/* the HUD is the top package: no screen over it */
static int hud_on_top(void){
    uint32_t pl = rd32(G_PLAYER1), hud = pl ? rd32(pl + PL_HUD) : 0, fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0,
             top = mg ? rd32(mg + 0xe4u) : 0;
    char a[40], b[40];
    cstr(top ? rd32(top + 0xcu) : 0, a, sizeof a); cstr(hud ? rd32(hud + 0x10u) : 0, b, sizeof b);
    return a[0] && !strcasecmp(a, b);
}
static uint32_t view_addr(void){
    uint32_t pl = rd32(G_PLAYER1), n = pl ? rd32(pl + 0x10u) : 9u;
    return n < 2u ? G_PSET + 0x28u * n + 0x10u : 0;
}

/* the drift event's styles that the racer's DriftScore lacks (looked for every frame: a new free roam's
 * DriftScore may be where the last one was) */
static void add_styles(CPU *c, uint32_t rc){
    uint32_t ds = rd32(rc + RC_DRIFTSCORE), tab = rd32(G_STYLES), pool = rd32(G_STYLEPOOL);
    if (!ok(ds, 4u * N_STYLES) || !ok(tab, 0x30u * N_STYLES) || !pool) return;
    int added = 0;
    for (uint32_t i = 0; i < N_STYLES; i++){
        if (!(MR8(tab + 0x30u * i + 7u) & MODE_DRIFT) || MR32(ds + 4u * i)) continue;
        uint32_t s = gcall(c, FN_POOL_ALLOC, 0, 1, &pool);
        if (!ok(s, 0x3c)){ fprintf(stderr, "[drift] f=%ld no style from the pool (entry %u)\n", autotest_frame(), i); return; }
        MR32(s + 0x34u) = 0; MR32(s + 0x38u) = 0; MR8(s + 0x10u) = 0; MR8(s + 0x18u) = 1;
        MR32(s + 0xcu) = i; MR32(s + 8u) = tab + 0x30u * i;
        gcall(c, FN_STYLE_INIT, s, 0, 0);
        MR32(ds + 4u * i) = s;
        MR8(s + 0x10u) = 1;
        added++;
    }
    if (!added) return;
    fprintf(stderr, "[drift] f=%ld the drift event's styles: %d added to free roam's DriftScore 0x%08x\n", autotest_frame(), added, ds);
}
/* free roam has no route: the DriftCar's best route distance follows the car's down */
static void no_route(uint32_t rc){
    uint32_t dc = rd32(rc + RC_DRIFTCAR), car = dc && ok(dc, DC_BEST + 4u) ? rd32(dc + DC_CAR) : 0, rt = car ? rd32(car + 0x1cu) : 0;
    if (!ok(rt, 0x18)) return;
    float d = MRF32(rt + 0x14u);
    if (MRF32(dc + DC_BEST) > d) MRF32(dc + DC_BEST) = d;
}
/* the points of drifts banked since the last frame */
static void count(uint32_t rc){
    uint32_t ds = rd32(rc + RC_DRIFTSCORE), st = ok(ds, 3) ? rd32(ds) : 0;
    if (!ok(st, ST_TOTAL + 4u)) return;
    float tot = MRF32(st + ST_TOTAL);
    if (st != D.style){ D.style = st; D.last = tot; return; }
    if (tot >= D.last + 1.f){
        double got = (double)(int)(tot - D.last);
        D.total += got;
        if (got > D.best) D.best = got;
        fprintf(stderr, "[drift] f=%ld a drift of %.0f (total %.0f, best %.0f)\n", autotest_frame(), got, D.total, D.best);
    }
    D.last = tot;
}

/* free roam's style popup, which stays up while the switch is on (see the top) */
static void hide_popup(CPU *c){
    uint32_t pl = rd32(G_PLAYER1), hud = pl ? rd32(pl + PL_HUD) : 0, el = hud ? rd32(hud + HUD_STYLES) : 0;
    if (!ok(el, 0x3c)) return;
    for (uint32_t o = 0x34u; o <= 0x38u; o += 4u){
        uint32_t obj = rd32(el + o);
        if (ok(obj, 0x20) && !(MR32(obj + 0x1cu) & 1u)) gcall(c, FN_FE_HIDE, 0, 1, &obj);
    }
}

static void camera_on(CPU *c){
    uint32_t va = view_addr(), pl = rd32(G_PLAYER1);
    if (!va || !pl) return;
    uint32_t v = MR32(va);
    if (v != VIEW_DRIFT){
        D.saved = v;
        MR32(va) = VIEW_DRIFT - 1u;
        uint32_t one = 1;
        gcall(c, FN_CAMERA_KEY, pl, 1, &one);
    } else if (D.saved > 3u) D.saved = 3u;
    D.cam = MR32(va) == VIEW_DRIFT;
    if (!D.cam){ MR32(va) = v; fprintf(stderr, "[drift] f=%ld no drift camera (view %u)\n", autotest_frame(), v); }
}
/* live: the world's camera too (the camera key's way), else only the view the next one is made for */
static void camera_off(CPU *c, int live){
    uint32_t va = view_addr(), pl = rd32(G_PLAYER1), back = D.saved <= 3u ? D.saved : 3u;
    D.cam = 0;
    if (!va || MR32(va) != VIEW_DRIFT || MR8(G_DRIFTRACE)) return;
    if (live && pl){
        MR32(va) = back - 1u;                      /* the camera key's step wraps a -1 to 0 */
        uint32_t one = 1;
        gcall(c, FN_CAMERA_KEY, pl, 1, &one);
    } else MR32(va) = back;
}

uint32_t garage_find(uint32_t db, uint32_t h);   /* garage.c: a front-end car record by handle (slots 6-10 too) */

/* The car made again (0x5F6C20) with its nitrous as it was: the shot's length (a drift event's is 0) and what is left
 * of it (see the top) */
static void car_tuned(CPU *c, uint32_t car){
    uint32_t ph = rd32(car + CAR_PHYS), nos[4], one = 1;
    int keep = ok(ph, PH_NOS_LEN + 3u) && ok(car + CAR_NOS_STATE, 15u);
    float len = keep ? MRF32(ph + PH_NOS_LEN) : 0.f;
    for (uint32_t i = 0; keep && i < 4u; i++) nos[i] = MR32(car + CAR_NOS_STATE + 4u * i);
    gcall(c, FN_CAR_TUNED, car, 1, &one);
    if (!keep || rd32(car + CAR_PHYS) != ph) return;
    MRF32(ph + PH_NOS_LEN) = len;
    for (uint32_t i = 0; i < 4u; i++) MR32(car + CAR_NOS_STATE + 4u * i) = nos[i];
}

/* The car's physics made again with the switch on: a drift event's handling (parameter set 3) and the car's Drift
 * setup from the dyno in its ride (see the top) */
static void physics_in(CPU *c){
    uint32_t car = rd32(rd32(G_PLAYER1) + 4u), ride = car ? rd32(car + CAR_RIDE) : 0,
             rec = MR32(G_GAMEMODE) == 1u ? garage_find(G_FECARS, rd32(G_CAREER_CAR)) : 0;
    if (!ok(ride, RIDE_TUNING + SETUP_SZ)) return;
    uint32_t setup = ok(rec, REC_SETUPS + 5u * SETUP_SZ) ? rec + REC_SETUPS + SETUP_DRIFT * SETUP_SZ : 0;
    int differ = 0;
    for (uint32_t i = 0; setup && i < SETUP_SZ; i++) differ += MR8(setup + i) != MR8(ride + RIDE_TUNING + i);
    if (differ){
        for (uint32_t i = 0; i < SETUP_SZ; i++){ D.kept[i] = MR8(ride + RIDE_TUNING + i); MR8(ride + RIDE_TUNING + i) = MR8(setup + i); }
        D.swapped = 1;
    }
    D.tuned = car;
    car_tuned(c, car);
    fprintf(stderr, "[drift] f=%ld the car's physics for a drift event%s (%d of %u values from the dyno's Drift setup), its nitrous kept\n",
            autotest_frame(), !setup ? ", no Drift setup of its own" : "", differ, SETUP_SZ);
}
/* ... and as they were, while the car is still the one (out of the world: the next car is made new) */
static void physics_out(CPU *c){
    uint32_t car = D.tuned, ride = car ? rd32(car + CAR_RIDE) : 0;
    int same = car && rd32(rd32(G_PLAYER1) + 4u) == car && ok(ride, RIDE_TUNING + SETUP_SZ);
    if (same && D.swapped) for (uint32_t i = 0; i < SETUP_SZ; i++) MR8(ride + RIDE_TUNING + i) = D.kept[i];
    D.tuned = 0; D.swapped = 0;
    if (!car) return;
    if (!same || rd32(G_FLOW) != 6u){ fprintf(stderr, "[drift] f=%ld the car is gone: no physics to give back\n", autotest_frame()); return; }
    car_tuned(c, car);
    fprintf(stderr, "[drift] f=%ld the car's own physics back\n", autotest_frame());
}
/* the dyno setup friends' games show the car with (roam_game.c): the ride's own while the Drift setup is in, so X is
 * no new look (a car loaded again in every friend's game) */
int drift_kept_tuning(uint8_t *out){
    if (!D.swapped) return 0;
    memcpy(out, D.kept, SETUP_SZ);
    return 1;
}

/* the status, every frame it changes (a drift's points: every frame of it), all in one consistent section */
static void publish(void){
    static const char *const k[8] = { "drift", "drift_key", "drift_x", "drift_gain", "drift_total", "drift_best", "drift_st", "drift_pend" };
    if (!D.slots){ D.slots = 1; for (int i = 0; i < 8; i++){ D.slot[i] = host_status_slot(k[i]); D.val[i] = -1; } }
    uint32_t dc = 0, st = 0, rc = D.on ? racer() : 0, ds = rc ? rd32(rc + RC_DRIFTSCORE) : 0;
    if (rc) dc = rd32(rc + RC_DRIFTCAR);
    if (ok(ds, 3)) st = rd32(ds);
    if (!ok(st, ST_TOTAL + 4u)) st = 0;
    int lvl = dc && ok(dc, DC_GAIN + 4u) ? (int)MR32(dc + DC_LEVEL) : 1;
    if (lvl < 1 || lvl > 5) lvl = 1;
    float pend = st ? MRF32(st + ST_PEND) : 0.f;
    double v[8] = { (double)(D.want | (D.on ? 2 : 0) | (D.note ? 4 : 0)), (double)D.keys, (double)lvl,
                    lvl < 5 && dc ? (double)(int32_t)MR32(dc + DC_GAIN) : 0.0, D.total, D.best,
                    st ? (double)MR32(st + ST_STATE) : 2.0, pend > 0.f && pend < 1e9f ? (double)(int)pend : 0.0 };
    int changed = 0;
    for (int i = 0; i < 8; i++) changed |= v[i] != D.val[i];
    if (!changed) return;
    memcpy(D.val, v, sizeof v);
    host_status_set_n(D.slot, v, 8);
}

void drift_frame(CPU *c){
    int world = roam_world(), top = world && hud_on_top(), drive = top && MR32(race()) == 4u;
    for (int n = x_presses(); n > 0; n--){
        D.keys++;
        D.note = !world;
        if (world){ D.want = !D.want; fprintf(stderr, "[drift] f=%ld drift mode %s (X)\n", autotest_frame(), D.want ? "on" : "off"); }
        else fprintf(stderr, "[drift] f=%ld X outside free roam: nothing\n", autotest_frame());
    }
    int on = D.want && drive;
    if (on != D.on){
        D.on = on;
        MR32(G_FORCEDRIFT) = (uint32_t)on;
        fprintf(stderr, "[drift] f=%ld drift handling %s%s\n", autotest_frame(), on ? "on" : "off",
                on ? "" : !D.want ? "" : !world ? " (not in free roam)" : !top ? " (a screen over the HUD)" : " (paused)");
        if (on) physics_in(c);
        else physics_out(c);
    }
    if (on){
        uint32_t rc = racer();
        if (rc){ add_styles(c, rc); no_route(rc); count(rc); }
        hide_popup(c);
        if (!D.cam) camera_on(c);
    } else if (D.cam && (!D.want || !world)) camera_off(c, rd32(G_FLOW) == 6u && hud_on_top());
    publish();
}

/* The music's two tests of the switch, while it is ours: no drift event (free roam's songs, at their volume). */
int drift_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x00482279):                  /* the music manager: test al, al (0x401910, a drift event?) */
        if (D.on && !MR8(G_DRIFTRACE)) c->eax &= ~0xffu;
        return 1;
    case HOOK_SITE(0x00471b95):                  /* the in-game mix: test eax, eax (eax = [0x890104]) */
        if (D.on && !MR8(G_DRIFTRACE)) c->eax = 0;
        return 1;
    }
    return 0;
}

/* The drift event's leaderboard while the switch is ours (free roam's AI cars are no racers): skipped. */
void sub_0056ddd0_orig(CPU *c);
void sub_0056ddd0(CPU *c){
    if (!D.on || MR8(G_DRIFTRACE)){ sub_0056ddd0_orig(c); return; }
    POP32(c->eip);
}
/* The drift event's HUD while the switch is ours (free roam's shows none of it, the page does): skipped.  Both thiscall
 * with one argument. */
void sub_004c85d0_orig(CPU *c);
void sub_004c85d0(CPU *c){                       /* the style points' drift branch (this: the style points) */
    if (!D.on || MR8(G_DRIFTRACE)){ sub_004c85d0_orig(c); return; }
    POP32(c->eip); c->esp += 4u;
}
void sub_004ac120_orig(CPU *c);
void sub_004ac120(CPU *c){                       /* FEngHud::ShowNiftyDriftyMessage (this: the HUD) */
    if ((!D.on || MR8(G_DRIFTRACE)) && rd32(c->ecx + HUD_DRIFT)){ sub_004ac120_orig(c); return; }
    POP32(c->eip); c->esp += 4u;
}
