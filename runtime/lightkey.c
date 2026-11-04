/* lightkey.c -- the headlights on a key: G tapped turns them off, or back on (the game's own lights, as they were);
 * held (HOLD_S) it is high beam, brighter than the game's own and reaching farther, for as long as it is held, and as
 * they were again when it is let go (off again, if they were off: a flash).  On a controller the right stick straight
 * up does the same (the look's stick: the left one when the game steers with the right one, look.c): flicked up and
 * back, on or off; held up, high beam (straight up turns the look by nothing).  Only
 * while the player drives (the drive camera, the HUD on top, the race running: free roam, races, Online and LAN);
 * not paused, in menus, photo mode, a replay or a cutscene.  Friends in free roam see it (roam_game.c: the state's
 * RC_BIT_DARK, its extension's RC_OPEN_HIBEAM), on their screens as on ours.  The key is the player's (the start
 * page's Controls, Site: settings.ini key_lights, NFSU2_KEY_LIGHTS, or off: dinput_kbd.c dikbd_site_key), G by
 * default, the game's "Debug Camera Slow" (nothing without the developers' camera); a player who put one of the
 * game's controls on it (Options, Controls) keeps the key for that: it does nothing here then (dikbd_ours).
 *
 * How the game lights a car (SPEED2.EXE):
 *  - car +0x93D (byte): its headlights are on.  0x5F6190 (from the car's update 0x604740, World::DoTimestep) sets it
 *    each step, 1 but for a flicker of its own (+0x925); 0x604260 sets it 1 at the car's start.  Its readers all
 *    draw: 0x615630 (the flares: the two headlight ones, light hashes 0x9DB90133 and 0xD09091C6, only with it), 0x615490
 *    (each car's headlight glow, 0x60D5D0), 0x60F550 (the light trails' colour: 0 without it).  So "off" is that byte
 *    0 from the main-loop hook on (after the world's step, before the frame is drawn: lightkey_frame), for our car
 *    and each friend's, put back to 1 for a friend's car once its lights are on again (our car's the game's next
 *    step gives back).
 *  - the flares: 0x615630 (thiscall, the car's render info, ret 0x18; its car at +0) takes each light of the car's
 *    list (+0x3F0, the light's hash at +8), its strength (a headlight's 1 + [0x7A6680], 2, capped at 1.0: the
 *    immediate at 0x61594C that ExtraOptions' HighBeamBrightness writes, the same cap for every light) times its
 *    last argument, and draws it with 0x490920 (cdecl: view, light, matrix, strength, ...), which sizes the flare
 *    from the light's least to its most size by the strength, and lights it by it: past 1 bigger and brighter.  High
 *    beam: each headlight flare's strength times K.hi_flare (photo.c's override of 0x615630 names the car,
 *    lightkey_flares; the override of 0x490920 below).
 *  - the road: the player's car's projector (view 9; headlights.c) with HEADLIGHTCOLOR, the colour of the player's
 *    car ([[car +0x554] +0x380], 0x60F510) as the effect constant (ID3DXEffect::SetVector with the handle at +0x574 of
 *    the shader [0x870910] +0x1C, per lit draw: 0x5BDAA0).  d3dx_hle.c's SetVector scales it by
 *    lightkey_road_scale(): 0 off; for high beam headlights.c lights the road from our car once more (a light of the
 *    friends' kind, aimed higher and brighter), the game's own then 0.  Friends' lights (headlights.c) ask
 *    lightkey_state for each car: none when off, brighter and aimed higher on high beam.
 *
 * The game's own look stays as it was while the lights are on.  NFSU2_LIGHTS_LOG=1 logs each press.  Tests: the
 * script command "lights ..." (lightkey_script), tests/web/scripts/lightkey_*.txt, tests/scripts/roam_lights_*.txt. */
#include "cpu.h"
#include "kbd.h"
#include "gamepad.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

long autotest_frame(void);
unsigned dikbd_site_key(const char *env, unsigned dflt);   /* dinput_kbd.c: the player's key for one of ours, 0 off */
int dikbd_ours(unsigned dik, const char *what);           /* ...0 while one of the game's controls is on it */
const char *dik_name(unsigned dik);                       /* dikeys.c */

#define DIK_G        0x22u         /* the key's default */
#define G_FLOW       0x008654a4u   /* 6: in the world */
#define G_RACE       0x00890118u   /* the race status: +0 4 racing (free roam too), 6 paused */
#define G_PLAYER1    SPLIT_PLAYERS_INDEX   /* Player*: +4 its car, +0x9A4 its HUD, +0x238 its camera mover */
#define G_FENG       0x008384c4u   /* +8 the package manager: +0xE4 the top package, +0xC its name */
#define G_FREEZE     0x00890084u   /* the world stands (photo mode) */
#define G_TXT_ON     0x0083a9c0u   /* byte: the game's text entry takes the typing ... */
#define G_TXT_REC    0x0083a9c4u   /* ... for this record (0: none) */
#define PL_CAR       4u
#define PL_HUD       0x9a4u
#define PL_MOVER     0x238u
#define MV_TYPE      0xcu
#define MOVER_DRIVE  1u
#define CAR_HL_ON    0x93du        /* byte: the car's headlights are on */
#define HASH_HL_A    0x9db90133u   /* the two headlight flares (0x615630's lights gated by CAR_HL_ON) */
#define HASH_HL_B    0xd09091c6u
#define HOLD_S       0.2           /* held this long: high beam (a shorter press: on or off) */
#define LK_SLOTS     64            /* roam_game.c's slots (NSLOT 48) */
/* the look's stick (look.c): up within UP_RAD of straight up and past UP_R from at rest (under REST_R); the look turns
 * the view by the stick's angle, so held there the view turns 17 degrees at most */
#define REST_R       0.35f
#define UP_R         0.75f
#define UP_RAD       0.30f
#define SIDE_RAD     0.60f         /* ...turned this far off straight up on its way: a look round, not the lights */

enum { LK_NORMAL = 0, LK_OFF = 1, LK_HIGH = 2 };

static struct {
    int off;                          /* tapped off */
    uint32_t kseq; int kinit, gdown;  /* the key's events */
    int kheld; double kt0;            /* the key down since kt0 (a press of ours: none of the game's controls is on it) */
    int pst; double pt0;              /* the stick: 0 at rest or elsewhere, 1 up from rest since pt0, 2 not ours till rest */
    int kfl, pfl;                     /* this press of the key, of the stick, has shown high beam: no tap when it ends (a press
                                         is a tap by that, not by when a frame sees it end: at a low frame rate too) */
    int flash;                        /* held: high beam */
    uint32_t car;                     /* our car, as last seen */
    int dark;                         /* we put its +0x93D to 0 */
    uint32_t fcar[LK_SLOTS]; uint8_t fst[LK_SLOTS], fdark[LK_SLOTS];
    int fresh;                        /* roam_game.c said this frame which friends' cars it shows (lightkey_car) */
    uint32_t flare_ri;                /* 0x615630 draws this render info's flares now */
    float flare_k;                    /* ...its headlight flares' strength times this */
    int test;                         /* the script's "lights state ...": -1 none, else our state */
    int last;                         /* our state, as last logged */
    unsigned long taps, holds;
    unsigned long fl_cars, fl_hl, fl_scaled, fl_cars_hi, rd_set;   /* (counts for "lights report") */
} K = { .test = -1, .last = -1 };

/* high beam (option D of the rendered variants, the one picked): the flares' strength times hi_flare, the road light times hi_gain and
 * aimed hi_pitch degrees higher (headlights.c) */
static float g_hi_flare = 3.0f, g_hi_gain = 5.0f, g_hi_pitch = 5.0f;

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void cstr(uint32_t a, char *o, int n){
    int i = 0;
    for (; a && i + 1 < n && ok(a + (uint32_t)i, 0) && MR8(a + (uint32_t)i); i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static int lg(void){ static int on = -1; if (on < 0){ const char *e = getenv("NFSU2_LIGHTS_LOG"); on = e && *e == '1'; } return on; }

static uint32_t my_car(void){ uint32_t pl = rd32(G_PLAYER1); return pl ? rd32(pl + PL_CAR) : 0u; }
/* the player drives: the world runs (not paused), the drive camera, the HUD on top, no typing, not photo mode */
static int driving(void){
    uint32_t pl = rd32(G_PLAYER1), r = rd32(G_RACE);
    if (rd32(G_FLOW) != 6u || !ok(pl, PL_MOVER + 4u) || !ok(r, 3) || MR32(r) != 4u || rd32(G_FREEZE)) return 0;
    uint32_t car = rd32(pl + PL_CAR), mv = rd32(pl + PL_MOVER);
    if (!ok(car, 0x9e0) || !ok(mv, MV_TYPE + 4u) || MR32(mv + MV_TYPE) != MOVER_DRIVE) return 0;
    if (mem_is_committed(G_TXT_REC + 3u) && MR8(G_TXT_ON) && MR32(G_TXT_REC)) return 0;
    uint32_t hud = rd32(pl + PL_HUD), fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0, top = mg ? rd32(mg + 0xe4u) : 0;
    char a[40], b[40];
    cstr(top ? rd32(top + 0xcu) : 0, a, sizeof a); cstr(hud ? rd32(hud + 0x10u) : 0, b, sizeof b);
    return a[0] && !strcasecmp(a, b);
}

/* our lights now: high beam while held, else off when tapped off */
int lightkey_mine(void){
    if (K.test >= 0) return K.test;
    return K.flash ? LK_HIGH : K.off ? LK_OFF : LK_NORMAL;
}
/* a car's lights: ours, a friend's (as its player has them), any other car's as the game has them */
int lightkey_state(uint32_t car){
    if (!car) return LK_NORMAL;
    if (car == K.car) return lightkey_mine();
    for (int k = 0; k < LK_SLOTS; k++) if (K.fcar[k] == car) return K.fst[k];
    return LK_NORMAL;
}
/* roam_game.c, each frame: slot k's car (0: none) and its player's lights */
void lightkey_car(int k, uint32_t car, int st){
    if (k < 0 || k >= LK_SLOTS) return;
    K.fresh = 1;
    if (K.fcar[k] != car){
        /* the slot's car before is gone or another slot's now: never written again from here.  A car new to the slot
         * with its lights off may be one we turned off in another slot: on again below unless its player has them off */
        K.fcar[k] = car; K.fst[k] = LK_NORMAL;
        K.fdark[k] = ok(car, 0x9e0) && !MR8(car + CAR_HL_ON);
    }
    if (!car) return;
    st = st == LK_OFF || st == LK_HIGH ? st : LK_NORMAL;
    if (st != K.fst[k] && lg())
        fprintf(stderr, "[lights] f=%ld slot %d: the friend's lights %s\n", autotest_frame(), k,
                st == LK_OFF ? "off" : st == LK_HIGH ? "on high beam" : "on");
    K.fst[k] = (uint8_t)st;
}

static const char *st_name(int st){ return st == LK_OFF ? "off" : st == LK_HIGH ? "high beam" : "on"; }
static void press_tap(const char *how){
    K.off = !K.off; K.taps++;
    fprintf(stderr, "[lights] f=%ld %s: lights %s\n", autotest_frame(), how, K.off ? "off" : "on");
}

/* the key's presses since the last frame (G unless NFSU2_KEY_LIGHTS; none off): a press let go within HOLD_S turns
 * the lights off or on, one held longer is high beam until it is let go (a tap between two frames counts).  None while
 * the player has one of the game's controls on the key (dikbd_ours: the log says so once) */
static unsigned g_dik = 0x100u;
static void key_input(int can, double t){
    if (g_dik == 0x100u) g_dik = dikbd_site_key("NFSU2_KEY_LIGHTS", DIK_G);
    uint32_t head = kbd_ev_head();
    if (!K.kinit){ K.kinit = 1; K.kseq = head; }
    for (; K.kseq != head; K.kseq++){
        KbdEv e;
        int r = kbd_ev_get(K.kseq, &e);
        if (r == 0) break;
        if (r < 0 || !g_dik || e.dik != g_dik) continue;
        int down = e.down != 0;
        if (down == K.gdown) continue;                                  /* (a repeat) */
        K.gdown = down;
        if (down){
            if (!can || !dikbd_ours(g_dik, "the headlight key")) continue;   /* (the game's control on it: theirs) */
            K.kheld = 1; K.kt0 = t;
        } else if (K.kheld){
            K.kheld = 0;
            if (!K.kfl && can) press_tap(dik_name(g_dik) ? dik_name(g_dik) : "the key");
            K.kfl = 0;
        }
    }
    if (!can){ K.kheld = 0; K.kfl = 0; }
}

/* the look's stick straight up (the right stick; the left one when the game's controls read the right one and none of
 * the left, none when they read both: dinput_pad.c pad_look_stick): flicked up and back to rest within HOLD_S, on or
 * off; held up, high beam.  A stick the game steers or brakes with never is (the quick chat's picker keeps the stick
 * from the game, and from here, while it is open) */
int pad_look_stick(void);
static int stick_input(int can, double t){
    PadState s;
    int st = pad_look_stick();
    if (!can || st < 0 || !gamepad_read(&s) || s.wheel){ K.pst = 0; K.pfl = 0; return 0; }
    float x = s.axis[st], y = s.axis[st + 1], r = hypotf(x, y), dir = fabsf(atan2f(x, -y));
    if (!(r == r)) r = 0.f;
    int held = 0;
    if (r < REST_R){
        if (K.pst == 1 && !K.pfl) press_tap(st == PAD_AXIS_LX ? "the left stick up" : "the right stick up");
        K.pst = 0; K.pfl = 0;
    } else if (K.pst == 0){
        if (r >= UP_R) K.pst = dir < UP_RAD ? 1 : 2, K.pt0 = t;           /* up from rest: ours; elsewhere a look */
    } else if (K.pst == 1){
        if (dir > SIDE_RAD){ K.pst = 2; K.pfl = 0; }                  /* turned on to the side: a look round */
        else if ((held = t - K.pt0 >= HOLD_S)) K.pfl = 1;
    }
    return held;
}

static int g_self_lit;              /* headlights.c lit the road from our car this view render (our high beam) */
/* each frame, from the main-loop hook: after the world's step, before the frame is drawn */
void lightkey_frame(CPU *c){
    (void)c;
    double t = now_s();
    int can = driving();
    g_self_lit = 0;                                                     /* (headlights.c says so again as it draws) */
    key_input(can, t);
    int sheld = stick_input(can, t);
    int kheld = K.kheld && t - K.kt0 >= HOLD_S;
    if (kheld) K.kfl = 1;
    int flash = can && (kheld || sheld);
    if (flash && !K.flash){ K.holds++; fprintf(stderr, "[lights] f=%ld %s held: high beam\n", autotest_frame(),
                                       kheld ? (dik_name(g_dik) ? dik_name(g_dik) : "the key") : "the right stick up"); }
    else if (!flash && K.flash) fprintf(stderr, "[lights] f=%ld let go: lights %s\n", autotest_frame(), K.off ? "off" : "on");
    K.flash = flash;
    uint32_t car = my_car();
    if (car != K.car){ K.car = car; K.dark = 0; }
    int st = lightkey_mine();
    if (st != K.last && lg()) fprintf(stderr, "[lights] f=%ld my lights %s\n", autotest_frame(), st_name(st));
    K.last = st;
    if (ok(car, 0x9e0)){
        if (st == LK_OFF){ MR8(car + CAR_HL_ON) = 0; K.dark = 1; }
        else if (K.dark){ MR8(car + CAR_HL_ON) = 1; K.dark = 0; }    /* (the game's next step sets its own) */
    }
    if (!K.fresh){                                                      /* no free roam this frame: no friends' cars */
        memset(K.fcar, 0, sizeof K.fcar); memset(K.fst, 0, sizeof K.fst); memset(K.fdark, 0, sizeof K.fdark);
    }
    K.fresh = 0;
    for (int k = 0; k < LK_SLOTS; k++){
        uint32_t fc = K.fcar[k];
        if (!fc || !ok(fc, 0x9e0)) continue;
        if (K.fst[k] == LK_OFF){ MR8(fc + CAR_HL_ON) = 0; K.fdark[k] = 1; }
        else if (K.fdark[k]){ MR8(fc + CAR_HL_ON) = 1; K.fdark[k] = 0; }
    }
}

/* ---- the flares ---------------------------------------------------------------------------------------------------
 * photo.c's override of 0x615630, around the game's: the car whose flares it draws now (its render info; 0: done) */
void lightkey_flares(uint32_t ri){
    K.flare_ri = ri; K.flare_k = 1.f;
    if (!ri || !ok(ri, 3)) return;
    K.fl_cars++;
    if (lightkey_state(MR32(ri)) == LK_HIGH){ K.flare_k = g_hi_flare; K.fl_cars_hi++; }
}
void sub_00490920_orig(CPU *c);
void sub_00490920(CPU *c){                       /* a light's flare (cdecl: view, light, matrix, strength, ...) */
    if (K.flare_ri && K.flare_k != 1.f){
        uint32_t light = MR32(c->esp + 8u), h = ok(light, 0xb) ? MR32(light + 8u) : 0u;
        if (h == HASH_HL_A || h == HASH_HL_B){ MRF32(c->esp + 0x10u) *= K.flare_k; K.fl_scaled++; }
    }
    if (K.flare_ri){ uint32_t light = MR32(c->esp + 8u), h = ok(light, 0xb) ? MR32(light + 8u) : 0u; K.fl_hl += h == HASH_HL_A || h == HASH_HL_B; }
    sub_00490920_orig(c);
}

/* ---- the road -------------------------------------------------------------------------------------------------- */
float lightkey_hi_gain(void){ return g_hi_gain; }
static int g_noself;                /* test: our high beam as the game's own light, brighter, not aimed higher */
int lightkey_self_ok(void){ return !g_noself; }
float lightkey_hi_pitch(void){ return g_hi_pitch; }
/* headlights.c: whether it lit the road from our car this view render (our high beam, its own light) */
void lightkey_self_lit(int on){ g_self_lit = on; }
/* d3dx_hle.c's SetVector: the game's own light on the road from our car (HEADLIGHTCOLOR) times this */
#define G_SHADER     0x00870910u   /* the shader being drawn with: +0x1C its parameters' handles */
#define SH_HLCOLOR   0x574u        /* ...HEADLIGHTCOLOR's */
uint32_t split_render_player(void);   /* splitscreen.c: the Player* whose view split screen draws now, 0 none */
int lightkey_road_vector(uint32_t handle, uint32_t vec){
    int st = lightkey_mine();
    if (st == LK_NORMAL || rd32(G_FLOW) != 6u) return 0;
    uint32_t rp = split_render_player();
    if (rp && rp != rd32(G_PLAYER1)) return 0;                          /* (another split-screen player's own light) */
    uint32_t sh = rd32(G_SHADER), p = sh ? rd32(sh + 0x1cu) : 0;
    if (!p || !ok(p + SH_HLCOLOR, 3) || MR32(p + SH_HLCOLOR) != handle || !ok(vec, 15)) return 0;
    float k = st == LK_OFF ? 0.f : g_self_lit ? 0.f : g_hi_gain;
    for (uint32_t i = 0; i < 3; i++) MRF32(vec + 4u * i) *= k;
    K.rd_set++;
    return 1;
}

/* the test script's "lights ...": state off|on|high|game (a state of ours, or the game's keys again), hi FLARE GAIN
 * PITCH (high beam's look, for the variants), report */
int lightkey_script(const char *s){
    char w[16] = {0}, a[16] = {0}; float x = 0, y = 0, z = 0;
    int n = sscanf(s, "%15s %15s", w, a);
    if (n < 1) return 0;
    if (!strcasecmp(w, "state") && n == 2){
        K.test = !strcasecmp(a, "off") ? LK_OFF : !strcasecmp(a, "high") ? LK_HIGH : !strcasecmp(a, "on") ? LK_NORMAL : -1;
        return 1;
    }
    if (!strcasecmp(w, "hi") && sscanf(s, "%*s %f %f %f", &x, &y, &z) == 3){ g_hi_flare = x; g_hi_gain = y; g_hi_pitch = z; return 1; }
    if (!strcasecmp(w, "noself") && n == 2){ g_noself = atoi(a) != 0; return 1; }
    if (!strcasecmp(w, "report")){
        fprintf(stderr, "[lights] f=%ld mine %s (off %d, flash %d), taps %lu, holds %lu, car 0x%08x +0x93d %u; high beam: flares x%.2f, road x%.2f, %.1f deg up\n",
                autotest_frame(), st_name(lightkey_mine()), K.off, K.flash, K.taps, K.holds, K.car,
                ok(K.car, 0x9e0) ? MR8(K.car + CAR_HL_ON) : 0u, (double)g_hi_flare, (double)g_hi_gain, (double)g_hi_pitch);
        fprintf(stderr, "[lights]   flares: %lu cars' drawn (%lu on high beam), %lu headlight flares, %lu of them brighter; our road light set %lu times\n",
                K.fl_cars, K.fl_cars_hi, K.fl_hl, K.fl_scaled, K.rd_set);
        for (int k = -1; k < LK_SLOTS; k++){            /* (and the fields 0x615630 draws the headlight flares by) */
            uint32_t c = k < 0 ? K.car : K.fcar[k];
            if (!c || !ok(c, 0x9e0)) continue;
            uint32_t ri = rd32(c + 0x554u), lt = rd32(c + 0x8e4u);
            fprintf(stderr, "[lights]   %s %d car 0x%08x: %s, +0x93d %u, its render info's car 0x%08x, light type %d\n",
                    k < 0 ? "mine" : "slot", k, c, st_name(k < 0 ? lightkey_mine() : K.fst[k]), MR8(c + CAR_HL_ON),
                    ri && ok(ri, 3) ? MR32(ri) : 0u, lt && ok(lt, 4) ? (int)(int8_t)MR8(lt + 4u) : -1);
        }
        return 1;
    }
    return 0;
}
