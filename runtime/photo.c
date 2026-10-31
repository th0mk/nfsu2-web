/* photo.c -- photo mode: the world stands still, the HUD goes, and the
 * camera comes loose from the car to take a picture with F12 (the page's
 * screenshot).  F12 opens it where it can (web/src/app/photo.ts: the page
 * keeps the keys, the mouse, the controller and the touch screen from the
 * game while it is on and sends what they do here as IEV_PHOTO events), and
 * so does the pause menu's Photo Mode.  Two cameras: orbit (the first) turns
 * about a point at the car, which moves up to ORBIT_PAN_M from it; free flies
 * up to FREE_M from the car.  Both zoom (the field of view) and tilt.  In free
 * roam with friends their names over their cars can show (off at first).
 *
 * Only in the world while the player drives (a race, free roam, an outrun:
 * the race status racing, the HUD the top package, the player's own drive
 * camera), and not in a race over the network (Online, LAN, free roam's
 * races between players), whose world cannot stand.  In free roam with other
 * players the world stands for this player only, as the pause menu's does:
 * the others see the car paused and pass through it (roam_game.c's sync
 * clock: a world dt of 0), and their cars keep driving here.  Anything over
 * the HUD (a message on the phone) ends it.
 *
 * The game (SPEED2.EXE v1.2 NA):
 *  - [0x890084] is the developers' world freeze, which nothing in the game
 *    sets: World::IsPaused (0x5EA320) says paused, so the main loop
 *    (0x5811C0) takes its paused branch (a world step of 0 from 0x601A60(0),
 *    no World::DoTimestep: the cars, the smoke and the race clock stand)
 *    while the cameras keep running on real time ([0x865198]; 0x44EBB0 puts
 *    the drive camera on it for this flag).
 *  - Player 0x5F1FD0 (thiscall: type, 0) makes the player's camera mover
 *    (Player +0x238, its type at +0xC: 1 the drive cameras).  Type 2 is the
 *    developers' DebugWorldCameraMover (constructor 0x455B10, update
 *    0x44D420): every update it looks from [0x7EC4D0] (the eye) at
 *    [0x8286A0], after its keys' moves (+0x2C .. +0x3F: the numpad, the race
 *    key map's Debug Camera actions), a car it follows ([0x7EB5F4], -1 none)
 *    and a pending move ([0x8286B0]), with a field of view of 71.5 degrees
 *    (camera +0xC4, a binary angle).  Here the eye and the point are written
 *    every frame before its update (the main-loop hook), the rest zeroed; the
 *    field of view and the tilt go in where the view is made (overrides.c
 *    sub_005c7ec0).  Player 0x5FA820 (0) gives the player's own camera back.
 *  - The front end draws the HUD, the names over friends' cars and any
 *    screen in 0x5378C0 (from the frame's render 0x5CBF40): skipped while
 *    photo mode is on.  With the names on it runs, every object of the
 *    packages on (the package manager's lists at +0xE4 and +0xF4, records
 *    chained through +4 with the name at +0xC; FEPackage +0x4C the first
 *    object, each +4 the next; a group, type +0x18 5, has its own at +0x64)
 *    but the names hidden for it (+0x1C bit 1 and 0x400000, as FEngHide
 *    0x50CA00 sets them), and those hidden ones shown after (as FEngShow
 *    0x50CA50: bit 1 off, 0x400000 on).
 *  - The sound manager ([0x82B884]) plays at the volumes its 0x469050
 *    (thiscall: volumes*) points it to, the Audio options' (0x83AA28: +0
 *    master, +4 speech, +8 menu music, +0xC game music, +0x10 effects,
 *    +0x14 engine, +0x18 tires).  The world standing still keeps every
 *    sound going, the engine at its revs, so while photo mode is on it
 *    points to a copy of them without effects, engine and tires: the music
 *    and speech play on, and the player's options are never touched.
 *  - The pause menu's items (free roam 0x4C2900, races 0x4C2BD0) have a
 *    Replay (0x4C2640: a video camera icon 0x84D1D864, "Replay"; Online and
 *    LAN games have none), greyed out (+0x20) until the replay recorder
 *    ([0x8650B8]: +0x50EC on, +0xB8 > 0) holds some driving; it plays that
 *    back (UI_ReplayControl.fng).  Where photo mode can open (the page has
 *    it, the player's drive camera, no network race) a Photo Mode item goes
 *    in after it, through the menus' AddOption (0x50DAB0, called through
 *    their vtables only): Replay's kind of item (0x51F670, vtable 0x7954F0)
 *    with our label and the icon INGAME_ICON_VOIP_ON (0x69CD72D1), which
 *    nothing in the PC game draws and texrep_web.c draws as a still camera.
 *    Their action (0x4C26B0, called through the vtable only) on Photo
 *    Mode's Accept asks for photo mode, then does what the menu's Resume
 *    does (free roam 0x4C2510, a race 0x4C24C0).  Photo mode opens once the
 *    game runs again.
 *
 * The page (status_web.c) reads:
 *   photo       bits: 1 photo mode can open now, 2 it is on, 4 the free camera,
 *               8 the names on, 16 friends' names can show here
 *   photo_fov   the field of view, degrees     photo_roll  the tilt, degrees
 *   photo_n     how many times it opened */
#include "cpu.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void dispatch_indirect(CPU *c, uint32_t addr);
void dispatch_patch(uint32_t a, void (*f)(CPU *));
long autotest_frame(void);
const char *language_name(void);
int roam_game_names_here(void);                          /* roam_game.c: the names over friends' cars */
int roam_game_names(CPU *c, uint32_t *obj, int max);
uint32_t language_add_text(const char *name, const char *text);

/* status_web.c in the browser; the Mac has no page */
__attribute__((weak)) int host_status_slot(const char *name){ (void)name; return -1; }
__attribute__((weak)) void host_status_set_n(const int *slot, const double *value, int n){ (void)slot; (void)value; (void)n; }

#define HOOK_RET       0x00581475u   /* the main-loop hook's return address, as guest calls see it */
#define G_FLOW         0x008654a4u   /* 6: in the world */
#define G_RACE         0x00890118u   /* the race status: +0 4 racing, 6 paused */
#define G_RACEKIND     0x0089e7b0u   /* 0 free roam */
#define G_NET_ONLINE   0x0089cf69u   /* byte: an online session */
#define G_NET_CLIENT   0x008669f4u   /* > 0 in a network race */
#define G_NET_HOST     0x00866a14u
#define G_PLAYER1      0x008900acu   /* Player*: +4 its car, +0x9A4 its HUD, +0x238 its camera mover */
#define G_FENG         0x008384c4u   /* +8 the package manager: +0xE4 the top package, +0xC its name */
#define G_VIEW1        0x00832e50u   /* eView 1: +0x40 its camera (+0x40 position, +0x50 forward, +0xC4 the field of view) */
#define G_FREEZE       0x00890084u
#define G_EYE          0x007ec4d0u
#define G_LOOK         0x008286a0u
#define G_FOLLOW       0x007eb5f4u
#define G_NUDGE        0x008286b0u
#define FN_SET_MOVER   0x005f1fd0u
#define FN_OWN_CAMERA  0x005fa820u
#define FN_REPLAY_ACT  0x004c26b0u
#define FN_ADD_ITEM    0x0050dab0u   /* thiscall (item; ret 4): a menu's AddOption, its vtable's +0x18 */
#define FN_ITEM        0x0051f670u   /* thiscall (icon, label, 0; ret 0xC): a menu item's constructor */
#define VT_REPLAY      0x007954f0u   /* the Replay item's vtable */
#define ITEM_BYTES     0x48u
#define ICON_PHOTO     0x69cd72d1u   /* INGAME_ICON_VOIP_ON, unused: texrep_web.c draws a still camera there */
#define FN_NEW         0x00575620u   /* cdecl (size): operator new */
#define FN_FE_PACKAGE  0x0052cef0u   /* cdecl (package name) -> FEPackage* */
#define PK_FIRST       0x4cu         /* an FEPackage's first object */
#define OB_NEXT        4u            /* an FE object's next, */
#define OB_TYPE        0x18u         /*   its type (5 a group), */
#define OB_FLAGS       0x1cu         /*   its flags (bit 1 hidden, 0x400000 changed: the renderer looks again), */
#define OB_CHILD       0x64u         /*   a group's first */
#define NAMES_MAX      64
#define HIDDEN_MAX     4096
#define FN_VOLUMES     0x00469050u
#define G_SOUND        0x0082b884u
#define G_VOLUMES      0x0083aa28u
#define VOL_BYTES      0x40u
#define PL_CAR         4u
#define PL_HUD         0x9a4u
#define PL_MOVER       0x238u
#define MV_TYPE        0xcu
#define MV_KEYS        0x2cu         /* its keys' moves, 20 bytes */
#define CAM_POS        0x40u
#define CAM_FWD        0x50u
#define CAM_FOV        0xc4u
#define IT_GREY        0x20u         /* a pause menu item: greyed out */
#define IT_LABEL       0x2cu         /* its label's hash */
#define MSG_ACCEPT     0x0c407210u
#define MOVER_DRIVE    1u
#define MOVER_DEBUG    2u

#define CENTRE_Z       0.5f          /* the point the orbit starts at: above the car's position, m */
#define ORBIT_PAN_M    25.f          /* how far that point moves from it, sideways */
#define ORBIT_UP_M     15.f          /* and up */
#define DIST_MIN       1.2f
#define DIST_MAX       30.f          /* (nothing keeps the camera out of walls: in a street the houses can be in the way) */
#define FREE_M         50.f          /* the free camera from the car's centre */
#define EYE_MIN_Z      -0.3f         /* the eye above the car's position, at least */
#define MOVE_MPS       5.f
#define TURN_DPS       100.f
#define FOV_MIN        15.f
#define FOV_MAX        110.f
#define ROLL_MAX       45.f
#define RATE_DPS       30.f          /* the zoom and tilt keys */
#define DEG            0.017453292f
#define PI             3.14159265f

/* ---- what the page sends (the input thread) ----------------------------------- */
enum { PH_HELLO = 0, PH_OPEN = 1, PH_HOLD = 2, PH_MOVE = 3, PH_TURN = 4, PH_DRAG = 5, PH_PAN = 6, PH_DOLLY = 7,
       PH_FOV = 8, PH_ROLL = 9, PH_MODE = 10, PH_RESET = 11, PH_NAMES = 12 };
/* PH_HOLD's bits: keys and buttons held */
enum { HB_FWD = 1, HB_BACK = 2, HB_LEFT = 4, HB_RIGHT = 8, HB_UP = 16, HB_DOWN = 32, HB_TL = 64, HB_TR = 128,
       HB_TU = 256, HB_TD = 512, HB_ZIN = 4096, HB_ZOUT = 8192, HB_RL = 16384, HB_RR = 32768, HB_FAST = 65536 };
#define NONE_SET INT32_MIN
static struct {
    atomic_int page, open, hold, move, lift, turn, dx, dy, px, py, dolly, fov, roll, mode, reset, names;
} I = { .fov = NONE_SET, .roll = NONE_SET, .mode = -1, .names = -1 };

/* the page's events (runtime/web/input_web.c, IEV_PHOTO) and the test script's */
void photo_input(int cmd, int a, int b){
    switch (cmd){
    case PH_HELLO: atomic_store(&I.page, 1); break;
    case PH_OPEN:  atomic_store(&I.open, a ? 1 : 2); break;
    case PH_HOLD:  atomic_store(&I.hold, a); break;
    case PH_MOVE:  atomic_store(&I.move, a); atomic_store(&I.lift, b); break;   /* x, y as int16 pairs; up, closer */
    case PH_TURN:  atomic_store(&I.turn, a); break;
    case PH_DRAG:  atomic_fetch_add(&I.dx, a); atomic_fetch_add(&I.dy, b); break;   /* 1/10000 of the view's height */
    case PH_PAN:   atomic_fetch_add(&I.px, a); atomic_fetch_add(&I.py, b); break;
    case PH_DOLLY: atomic_fetch_add(&I.dolly, a); break;                          /* 1/1000 of a wheel notch */
    case PH_FOV:   atomic_store(&I.fov, a); break;                                /* 1/100 degree */
    case PH_ROLL:  atomic_store(&I.roll, a); break;
    case PH_MODE:  atomic_store(&I.mode, a); break;                               /* 0 orbit, 1 free, 2 the other */
    case PH_RESET: atomic_store(&I.reset, 1); break;
    case PH_NAMES: atomic_store(&I.names, a); break;                              /* 0 off, 1 on, 2 the other */
    default: break;
    }
}

static struct {
    int on, free, names, drawn, patched, slots, slot[4];
    long n, pause_at;
    double t, val[4];
    uint32_t freeze_was, label, quiet, after;    /* after: the Replay item Photo Mode goes in behind */
    const char *lang;
    float yaw, pitch, dist, off[3], eye[3];      /* the view's direction; orbit: from its point; free: from the car's centre */
    float yaw0, pitch0, dist0, off0[3], fov0, fov, roll;
    float se[3], sl[3];                          /* the eye and the point looked at, smoothed, world */
} P;

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void rd3(uint32_t a, float *v){ for (int i = 0; i < 3; i++) v[i] = MRF32(a + 4u * (uint32_t)i); }
static void wr3(uint32_t a, const float *v){ for (int i = 0; i < 3; i++) MRF32(a + 4u * (uint32_t)i) = v[i]; }
static void cstr(uint32_t a, char *o, int n){
    int i = 0;
    for (; a && i + 1 < n && ok(a + (uint32_t)i, 0) && MR8(a + (uint32_t)i); i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
static float clampf(float v, float lo, float hi){ return v < lo ? lo : v > hi ? hi : v; }
static float s16(int packed, int hi){ return (float)(int16_t)(hi ? (uint32_t)packed >> 16 : (uint32_t)packed & 0xffffu) / 32767.f; }
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

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

/* the sound at the volumes at v (the options' own, or the quiet copy) */
static void volumes(CPU *c, uint32_t v){
    uint32_t snd = rd32(G_SOUND);
    if (snd && v) gcall(c, FN_VOLUMES, snd, 1, &v);
}
static void hush(CPU *c){
    if (!P.quiet){ uint32_t n = VOL_BYTES; P.quiet = gcall(c, FN_NEW, 0, 1, &n); }
    if (!ok(P.quiet, VOL_BYTES - 1u)){ P.quiet = 0; return; }
    memcpy(g_mem + P.quiet, g_mem + G_VOLUMES, VOL_BYTES);
    for (uint32_t o = 0x10u; o <= 0x18u; o += 4u) MRF32(P.quiet + o) = 0.f;
    volumes(c, P.quiet);
}

static uint32_t player(void){ uint32_t p = rd32(G_PLAYER1); return ok(p, PL_HUD + 4u) ? p : 0; }
static uint32_t car_of(uint32_t pl){ uint32_t c = pl ? rd32(pl + PL_CAR) : 0; return ok(c, 0x70) ? c : 0; }
static uint32_t mover_type(uint32_t pl){ uint32_t m = pl ? rd32(pl + PL_MOVER) : 0; return ok(m, MV_KEYS + 20u) ? MR32(m + MV_TYPE) : 0; }
static uint32_t view1_camera(void){ uint32_t cam = rd32(G_VIEW1 + 0x40u); return ok(cam, CAM_FOV + 2u) ? cam : 0; }
static int network_race(void){
    return MR8(G_NET_ONLINE) || (int32_t)MR32(G_NET_CLIENT) > 0 || (int32_t)MR32(G_NET_HOST) > 0;
}
/* the HUD is the top package: no screen over it */
static int hud_on_top(uint32_t pl){
    uint32_t hud = rd32(pl + PL_HUD), fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0, top = mg ? rd32(mg + 0xe4u) : 0;
    char a[40], b[40];
    cstr(top ? rd32(top + 0xcu) : 0, a, sizeof a); cstr(hud ? rd32(hud + 0x10u) : 0, b, sizeof b);
    return a[0] && !strcasecmp(a, b);
}
/* why photo mode cannot be on now (0: it can); on: it is, with our camera */
static const char *not_now(int on){
    uint32_t pl = player(), r = rd32(G_RACE);
    if (MR32(G_FLOW) != 6u || !pl || !car_of(pl)) return "not in the world";
    if (network_race()) return "a race over the network";
    if (!ok(r, 3) || MR32(r) != 4u) return "not driving";
    if (!hud_on_top(pl)) return "a screen over the HUD";
    if (mover_type(pl) != (on ? MOVER_DEBUG : MOVER_DRIVE)) return on ? "the game changed the camera" : "not the drive camera";
    return 0;
}
static void centre(uint32_t car, float *v){ rd3(car + 0x60u, v); v[2] += CENTRE_Z; }
static void dir(float yaw, float pitch, float *d){ d[0] = cosf(pitch) * cosf(yaw); d[1] = cosf(pitch) * sinf(yaw); d[2] = sinf(pitch); }

/* the orbit about point p, its view as the eye e has it */
static void orbit_from(const float *p, const float *e){
    float v[3] = { p[0] - e[0], p[1] - e[1], p[2] - e[2] }, l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l < 0.01f){ v[0] = 1; v[1] = 0; v[2] = 0; l = 1; }
    P.yaw = atan2f(v[1], v[0]);
    P.pitch = clampf(asinf(clampf(v[2] / l, -1.f, 1.f)), -85.f * DEG, 20.f * DEG);
    P.dist = clampf(l, DIST_MIN, DIST_MAX);
}
/* the point an orbit turns about, as the drive camera looks from e along f: as far ahead as the car is */
static void aim_point(const float *cc, const float *e, const float *f, float *p){
    float v[3] = { cc[0] - e[0], cc[1] - e[1], cc[2] - e[2] }, along = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
    along = clampf(along, DIST_MIN, DIST_MAX);
    for (int i = 0; i < 3; i++) p[i] = e[i] + f[i] * along;
    float o[3] = { p[0] - cc[0], p[1] - cc[1], p[2] - cc[2] }, h = sqrtf(o[0] * o[0] + o[1] * o[1]);
    if (h > ORBIT_PAN_M){ o[0] *= ORBIT_PAN_M / h; o[1] *= ORBIT_PAN_M / h; }
    o[2] = clampf(o[2], -CENTRE_Z, ORBIT_UP_M);
    for (int i = 0; i < 3; i++) p[i] = cc[i] + o[i];
}
static void clear_input(void){
    atomic_store(&I.dx, 0); atomic_store(&I.dy, 0); atomic_store(&I.px, 0); atomic_store(&I.py, 0);
    atomic_store(&I.dolly, 0); atomic_store(&I.fov, NONE_SET); atomic_store(&I.roll, NONE_SET);
    atomic_store(&I.mode, -1); atomic_store(&I.reset, 0);
}

static void enter(CPU *c){
    uint32_t pl = player(), car = car_of(pl), cam = view1_camera();
    float cc[3], ce[3], cf[3], p[3];
    centre(car, cc);
    if (cam){ rd3(cam + CAM_POS, ce); rd3(cam + CAM_FWD, cf); }
    else { ce[0] = cc[0] - 5.f; ce[1] = cc[1]; ce[2] = cc[2] + 1.5f; cf[0] = 1.f; cf[1] = 0; cf[2] = -0.2f; }
    float fl = sqrtf(cf[0] * cf[0] + cf[1] * cf[1] + cf[2] * cf[2]);
    if (!(fl > 0.5f && fl < 2.f)){ cf[0] = cc[0] - ce[0]; cf[1] = cc[1] - ce[1]; cf[2] = cc[2] - ce[2]; fl = sqrtf(cf[0] * cf[0] + cf[1] * cf[1] + cf[2] * cf[2]) + 1e-6f; }
    for (int i = 0; i < 3; i++) cf[i] /= fl;
    P.fov0 = cam ? clampf((float)MR16(cam + CAM_FOV) * 360.f / 65536.f, FOV_MIN, FOV_MAX) : 71.5f;
    P.freeze_was = MR32(G_FREEZE);
    MR32(G_FREEZE) = 1u;
    uint32_t arg[2] = { MOVER_DEBUG, 0u };
    gcall(c, FN_SET_MOVER, pl, 2, arg);
    if (mover_type(pl) != MOVER_DEBUG){
        MR32(G_FREEZE) = P.freeze_was;
        fprintf(stderr, "[photo] f=%ld no free camera (mover type %u): not opened\n", autotest_frame(), mover_type(pl));
        return;
    }
    MR32(G_FOLLOW) = 0xffffffffu;
    aim_point(cc, ce, cf, p);                       /* the first frame as the drive camera had it */
    for (int i = 0; i < 3; i++) P.off[i] = P.off0[i] = p[i] - cc[i];
    orbit_from(p, ce);
    P.yaw0 = P.yaw; P.pitch0 = P.pitch; P.dist0 = P.dist;
    P.fov = P.fov0; P.roll = 0; P.free = 0;
    memcpy(P.se, ce, sizeof P.se); memcpy(P.sl, p, sizeof P.sl);
    clear_input();
    hush(c);
    P.on = 1; P.n++; P.drawn = 0;
    fprintf(stderr, "[photo] f=%ld on: the world stands, the orbit at %.1f m, field of view %.1f\n", autotest_frame(), P.dist, P.fov);
}
static void leave(CPU *c, const char *why){
    uint32_t pl = player();
    if (MR32(G_FLOW) == 6u && pl && mover_type(pl) == MOVER_DEBUG){ uint32_t z = 0; gcall(c, FN_OWN_CAMERA, pl, 1, &z); }
    MR32(G_FREEZE) = P.freeze_was;
    volumes(c, G_VOLUMES);
    P.on = 0; P.pause_at = 0;
    fprintf(stderr, "[photo] f=%ld off (%s)\n", autotest_frame(), why);
}

/* the camera, one frame: what the page sends, then where it looks from and at */
static void steer(uint32_t car, float dt){
    int hold = atomic_load(&I.hold), mv = atomic_load(&I.move), lift = atomic_load(&I.lift), tn = atomic_load(&I.turn);
    float fast = hold & HB_FAST ? 3.f : 1.f;
    float mx = (float)(!!(hold & HB_RIGHT) - !!(hold & HB_LEFT)) + s16(mv, 0);
    float my = (float)(!!(hold & HB_FWD) - !!(hold & HB_BACK)) + s16(mv, 1);
    float mz = (float)(!!(hold & HB_UP) - !!(hold & HB_DOWN)) + s16(lift, 0);
    float dolly = s16(lift, 1) * 3.f * dt + (float)atomic_exchange(&I.dolly, 0) / 1000.f;
    float ty = (float)(!!(hold & HB_TL) - !!(hold & HB_TR)) - s16(tn, 0), tp = (float)(!!(hold & HB_TU) - !!(hold & HB_TD)) - s16(tn, 1);
    float ddx = (float)atomic_exchange(&I.dx, 0) / 10000.f, ddy = (float)atomic_exchange(&I.dy, 0) / 10000.f;
    float pdx = (float)atomic_exchange(&I.px, 0) / 10000.f, pdy = (float)atomic_exchange(&I.py, 0) / 10000.f;
    int fs = atomic_exchange(&I.fov, NONE_SET), rs = atomic_exchange(&I.roll, NONE_SET), md = atomic_exchange(&I.mode, -1);
    float cc[3], d[3];
    centre(car, cc);
    if (atomic_exchange(&I.reset, 0)){
        P.free = 0; memcpy(P.off, P.off0, sizeof P.off);
        P.yaw = P.yaw0; P.pitch = P.pitch0; P.dist = P.dist0; P.fov = P.fov0; P.roll = 0;
    }
    if (md >= 0 && (md == 2 ? !P.free : md) != P.free){
        dir(P.yaw, P.pitch, d);
        if (!P.free){                                   /* free from where the orbit looks from */
            for (int i = 0; i < 3; i++) P.eye[i] = P.se[i] - cc[i];
        } else {                                        /* an orbit about the point as far ahead as the car */
            float e[3], v[3];
            for (int i = 0; i < 3; i++){ e[i] = cc[i] + P.eye[i]; v[i] = cc[i] - e[i]; }
            float l = clampf(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), DIST_MIN, DIST_MAX), p[3];
            for (int i = 0; i < 3; i++) p[i] = e[i] + d[i] * l;
            for (int i = 0; i < 3; i++) P.off[i] = p[i] - cc[i];
            P.dist = l;
        }
        P.free = !P.free;
        fprintf(stderr, "[photo] f=%ld the %s camera\n", autotest_frame(), P.free ? "free" : "orbit");
    }
    /* turning: the keys and the stick at TURN_DPS, a drag across the view's height about 140 degrees */
    P.yaw += ty * TURN_DPS * DEG * fast * dt - ddx * 2.4f;
    P.pitch += tp * TURN_DPS * DEG * fast * dt - ddy * 2.4f;
    P.pitch = P.free ? clampf(P.pitch, -85.f * DEG, 85.f * DEG) : clampf(P.pitch, -85.f * DEG, 20.f * DEG);
    if (P.yaw > PI) P.yaw -= 2.f * PI; else if (P.yaw < -PI) P.yaw += 2.f * PI;
    dir(P.yaw, P.pitch, d);
    float f[3] = { cosf(P.yaw), sinf(P.yaw), 0 }, r[3] = { sinf(P.yaw), -cosf(P.yaw), 0 };
    float step = MOVE_MPS * fast * dt;
    if (!P.free){
        float k = P.dist * 0.5f;                         /* a two-finger drag: the point under the fingers */
        for (int i = 0; i < 3; i++) P.off[i] += (r[i] * mx + f[i] * my) * step - r[i] * pdx * k;
        P.off[2] += mz * step + pdy * k;
        float h = sqrtf(P.off[0] * P.off[0] + P.off[1] * P.off[1]);
        if (h > ORBIT_PAN_M){ P.off[0] *= ORBIT_PAN_M / h; P.off[1] *= ORBIT_PAN_M / h; }
        P.off[2] = clampf(P.off[2], -CENTRE_Z, ORBIT_UP_M);
        P.dist = clampf(P.dist * expf(-dolly * 0.12f), DIST_MIN, DIST_MAX);
    } else {
        for (int i = 0; i < 3; i++) P.eye[i] += (d[i] * my + r[i] * mx) * step + d[i] * dolly * 0.5f - r[i] * pdx * 5.f;
        P.eye[2] += mz * step + pdy * 5.f;
        float l = sqrtf(P.eye[0] * P.eye[0] + P.eye[1] * P.eye[1] + P.eye[2] * P.eye[2]);
        if (l > FREE_M) for (int i = 0; i < 3; i++) P.eye[i] *= FREE_M / l;
    }
    float zr = (float)(!!(hold & HB_ZOUT) - !!(hold & HB_ZIN)), rr = (float)(!!(hold & HB_RR) - !!(hold & HB_RL));
    P.fov = clampf(fs != NONE_SET ? (float)fs / 100.f : P.fov + zr * RATE_DPS * dt, FOV_MIN, FOV_MAX);
    P.roll = clampf(rs != NONE_SET ? (float)rs / 100.f : P.roll + rr * RATE_DPS * dt, -ROLL_MAX, ROLL_MAX);
    /* where from and at: the eye kept over the car's ground, then smoothed (a jerky drag or a key reads as a glide) */
    float e[3], l[3];
    if (!P.free){
        for (int i = 0; i < 3; i++){ l[i] = cc[i] + P.off[i]; e[i] = l[i] - d[i] * P.dist; }
    } else {
        for (int i = 0; i < 3; i++){ e[i] = cc[i] + P.eye[i]; l[i] = e[i] + d[i] * 10.f; }
    }
    float zmin = cc[2] - CENTRE_Z + EYE_MIN_Z;
    if (e[2] < zmin){ l[2] += zmin - e[2]; e[2] = zmin; if (P.free) P.eye[2] = zmin - cc[2]; }
    float k = 1.f - expf(-dt * 14.f);
    for (int i = 0; i < 3; i++){ P.se[i] += (e[i] - P.se[i]) * k; P.sl[i] += (l[i] - P.sl[i]) * k; }
}

/* the camera mover's: the eye and the point, nothing of its own */
static void place(uint32_t pl){
    uint32_t m = rd32(pl + PL_MOVER);
    memset(g_mem + m + MV_KEYS, 0, 20);
    MR32(G_FOLLOW) = 0xffffffffu;
    MR32(G_NUDGE) = 0; MR32(G_NUDGE + 4u) = 0; MR32(G_NUDGE + 8u) = 0;
    wr3(G_EYE, P.se); wr3(G_LOOK, P.sl);
}

/* the pause menu's label, in the game's language (the Latin-script ones as their language files write theirs, in
 * Latin-1; Polish and Turkish without their own letters; English for the others, whose fonts draw ASCII as it is) */
static void label(void){
    static const struct { const char *lang, *text; } T[] = {
        { "french", "Mode photo" }, { "german", "Fotomodus" }, { "italian", "Modalit\xe0 foto" },
        { "spanish", "Modo foto" }, { "dutch", "Fotomodus" }, { "swedish", "Fotol\xe4ge" },
        { "danish", "Fototilstand" }, { "portuguese", "Modo Foto" }, { "polish", "Tryb foto" },
        { "turkish", "Foto Modu" }, { "hungarian", "Fot\xf3 m\xf3" "d" },
    };
    const char *l = language_name(), *s = "Photo Mode";
    if (l == P.lang) return;
    P.lang = l;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(l, T[i].lang)) s = T[i].text;
    P.label = language_add_text("PHOTO_MODE_N2", s);
}

/* the status, when it changes */
static void publish(int can){
    static const char *const k[4] = { "photo", "photo_fov", "photo_roll", "photo_n" };
    if (!P.slots){ P.slots = 1; for (int i = 0; i < 4; i++){ P.slot[i] = host_status_slot(k[i]); P.val[i] = -1; } }
    double v[4] = { (double)((can && !P.on ? 1 : 0) | (P.on ? 2 : 0) | (P.on && P.free ? 4 : 0) | (P.names ? 8 : 0) |
                             (P.on && roam_game_names_here() ? 16 : 0)),
                    floor(P.fov * 10.f + 0.5f) / 10.0, floor(P.roll * 10.f + 0.5f) / 10.0, (double)P.n };
    if (!memcmp(v, P.val, sizeof v)) return;
    memcpy(P.val, v, sizeof v);
    host_status_set_n(P.slot, v, 4);
}

static void act(CPU *c);
static void add_item(CPU *c);
void photo_frame(CPU *c){
    double t = now_s();
    float dt = P.t > 0 ? (float)(t - P.t) : 0.f;
    P.t = t;
    if (dt < 0.f) dt = 0.f; else if (dt > 0.1f) dt = 0.1f;
    if (!P.patched){ P.patched = 1; dispatch_patch(FN_REPLAY_ACT, act); dispatch_patch(FN_ADD_ITEM, add_item); }
    label();
    int req = atomic_exchange(&I.open, 0);
    const char *why = not_now(P.on);
    if (P.on && (why || req == 2)) leave(c, req == 2 ? "closed" : why);
    else if (!P.on && !why && (req == 1 || P.pause_at)) enter(c);
    if (P.pause_at && (P.on || autotest_frame() - P.pause_at > 180)){
        if (!P.on) fprintf(stderr, "[photo] f=%ld the pause menu's Photo Mode: not opened (%s)\n", autotest_frame(), why ? why : "?");
        P.pause_at = 0;
    }
    int nm = atomic_exchange(&I.names, -1);
    if (nm >= 0 && (nm == 2 ? !P.names : nm) != P.names){
        P.names = !P.names;
        fprintf(stderr, "[photo] f=%ld names %s\n", autotest_frame(), P.names ? "on" : "off");
    }
    uint32_t pl = player();
    if (P.on){ steer(car_of(pl), dt); place(pl); }
    publish(!why && atomic_load(&I.page));
}

/* overrides.c sub_005c7ec0: while photo mode is on, the view's field of view (a binary angle) and tilt (radians) */
int photo_view(uint16_t *fov, float *roll){
    if (!P.on) return 0;
    *fov = (uint16_t)lrintf(P.fov * 65536.f / 360.f);
    *roll = P.roll * DEG;
    return 1;
}

/* the objects in the list at first hidden but those in keep and the groups that hold one (a group's own, which the
 * renderer draws by their own flags, with it, as FEngHide does); hid: the ones it hid */
static uint32_t hid[HIDDEN_MAX];
static int nhid;
static int holds(uint32_t o, const uint32_t *keep, int nk, int depth){
    for (int i = 0; i < nk; i++) if (o == keep[i]) return 1;
    if (depth >= 8 || MR32(o + OB_TYPE) != 5u) return 0;
    int n = 0;
    for (uint32_t it = rd32(o + OB_CHILD); it && ok(it, OB_CHILD + 4u) && n < 4096; it = rd32(it + OB_NEXT), n++)
        if (holds(it, keep, nk, depth + 1)) return 1;
    return 0;
}
static void hide_but(uint32_t first, const uint32_t *keep, int nk, int depth){
    int n = 0;
    for (uint32_t o = rd32(first); o && ok(o, OB_CHILD + 4u) && n < 4096; o = rd32(o + OB_NEXT), n++){
        int group = MR32(o + OB_TYPE) == 5u && depth < 8;
        if (holds(o, keep, nk, depth)){
            if (group) hide_but(o + OB_CHILD, keep, nk, depth + 1);
            continue;
        }
        if (!(MR32(o + OB_FLAGS) & 1u) && nhid < HIDDEN_MAX){ MR32(o + OB_FLAGS) |= 0x400001u; hid[nhid++] = o; }
        if (group) hide_but(o + OB_CHILD, keep, 0, depth + 1);
    }
}

/* The front end's frame (cdecl, no arguments): not while photo mode is on, but for the names over friends' cars
 * when they are on. */
void sub_005378c0_orig(CPU *c);
void sub_005378c0(CPU *c){
    if (!P.on){ sub_005378c0_orig(c); return; }
    uint32_t keep[NAMES_MAX], fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0;
    int nk = P.names && ok(mg, 0xf7) ? roam_game_names(c, keep, NAMES_MAX) : 0;
    if (!nk){ POP32(c->eip); return; }
    nhid = 0;
    char seen[160] = ""; int np = 0;
    for (uint32_t head = 0xe4u; head <= 0xf4u; head += 0x10u)
        for (uint32_t r = rd32(mg + head); r && ok(r, 0x10) && np < 64; r = rd32(r + 4u), np++){
            uint32_t name = rd32(r + 0xcu), pk = name ? gcall(c, FN_FE_PACKAGE, 0, 1, &name) : 0;
            if (!ok(pk, PK_FIRST + 4u)) continue;
            hide_but(pk + PK_FIRST, keep, nk, 0);
            if (P.drawn) continue;
            char b[40]; cstr(name, b, sizeof b);
            snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "%s%s", seen[0] ? ", " : "", b);
        }
    if (!P.drawn){ P.drawn = 1; fprintf(stderr, "[photo] f=%ld names: %d, the rest of %s hidden (%d)\n", autotest_frame(), nk, seen, nhid); }
    sub_005378c0_orig(c);
    for (int i = 0; i < nhid; i++) MR32(hid[i] + OB_FLAGS) = (MR32(hid[i] + OB_FLAGS) & ~1u) | 0x400000u;
}

/* The pause menu's Replay (thiscall: icon, label, 0; ret 0xC), as the game makes it; Photo Mode goes in after it
 * where photo mode can open after the pause. */
void sub_004c2640_orig(CPU *c);
void sub_004c2640(CPU *c){
    uint32_t it = c->ecx, pl = player();
    sub_004c2640_orig(c);
    const char *no = !atomic_load(&I.page) ? "no page for it" : !P.label || !ok(it, IT_LABEL + 4u) ? "no label" :
                     network_race() ? "a race over the network" : !car_of(pl) ? "no car" :
                     mover_type(pl) != MOVER_DRIVE ? "not the drive camera" : 0;
    fprintf(stderr, "[photo] f=%ld the pause menu's Replay%s, Photo Mode %s%s (camera %u)\n", autotest_frame(),
            ok(it, IT_GREY) && MR8(it + IT_GREY) ? " (greyed out)" : "", no ? "not: " : "after it", no ? no : "", mover_type(pl));
    P.after = no ? 0u : it;
}
/* A menu's AddOption (thiscall: item; ret 4): after that Replay, Photo Mode, an item of Replay's kind with its own
 * icon and label. */
void sub_0050dab0(CPU *c);
static void add_item(CPU *c){
    uint32_t menu = c->ecx, it = MR32(c->esp + 4u);
    sub_0050dab0(c);
    if (!it || it != P.after) return;
    P.after = 0;
    uint32_t n = ITEM_BYTES, ph = gcall(c, FN_NEW, 0, 1, &n);
    if (!ok(ph, ITEM_BYTES)) return;
    uint32_t arg[3] = { ICON_PHOTO, P.label, 0 };
    gcall(c, FN_ITEM, ph, 3, arg);
    MR32(ph) = VT_REPLAY;
    gcall(c, FN_ADD_ITEM, menu, 1, &ph);
}
/* Their action (thiscall: package, message, ...; ret 0x14): Photo Mode's Accept asks for photo mode, then resumes
 * the game as the menu's Resume does; Replay's as the game has it. */
void sub_004c26b0(CPU *c);
void sub_004c2510(CPU *c);
void sub_004c24c0(CPU *c);
static void act(CPU *c){
    uint32_t it = c->ecx, msg = MR32(c->esp + 8u);
    if (msg != MSG_ACCEPT || !ok(it, IT_LABEL + 4u) || MR32(it + IT_LABEL) != P.label || MR8(it + IT_GREY)){ sub_004c26b0(c); return; }
    P.pause_at = autotest_frame();
    fprintf(stderr, "[photo] f=%ld the pause menu's Photo Mode\n", autotest_frame());
    if (MR32(G_RACEKIND) == 0u) sub_004c2510(c); else sub_004c24c0(c);
}

/* the test script's "photo ..." (autotest.c): hello, on, off, orbit, free, other (Tab), reset, names [0|1], hold MASK,
 * move X Y [UP [CLOSER]], turn X Y (-1..1), drag DX DY, pan DX DY (view heights), dolly N (wheel notches), fov DEG,
 * roll DEG */
int photo_script(const char *s){
    char w[16] = {0}; float a = 0, b = 0, z = 0, k = 0;
    int n = sscanf(s, "%15s %f %f %f %f", w, &a, &b, &z, &k);
#define PACK(x, y) ((int)(((uint32_t)(int16_t)lrintf(clampf(x, -1, 1) * 32767.f) & 0xffffu) | ((uint32_t)(int16_t)lrintf(clampf(y, -1, 1) * 32767.f) << 16)))
    if (n < 1) return 0;
    if (!strcasecmp(w, "hello")) photo_input(PH_HELLO, 0, 0);
    else if (!strcasecmp(w, "on")){ photo_input(PH_HELLO, 0, 0); photo_input(PH_OPEN, 1, 0); }
    else if (!strcasecmp(w, "off")) photo_input(PH_OPEN, 0, 0);
    else if (!strcasecmp(w, "orbit")) photo_input(PH_MODE, 0, 0);
    else if (!strcasecmp(w, "free")) photo_input(PH_MODE, 1, 0);
    else if (!strcasecmp(w, "other")) photo_input(PH_MODE, 2, 0);
    else if (!strcasecmp(w, "reset")) photo_input(PH_RESET, 0, 0);
    else if (!strcasecmp(w, "names")) photo_input(PH_NAMES, n > 1 ? (int)a : 2, 0);
    else if (!strcasecmp(w, "hold")) photo_input(PH_HOLD, (int)a, 0);
    else if (!strcasecmp(w, "move")) photo_input(PH_MOVE, PACK(a, b), PACK(n > 3 ? z : 0, n > 4 ? k : 0));
    else if (!strcasecmp(w, "turn")) photo_input(PH_TURN, PACK(a, b), 0);
    else if (!strcasecmp(w, "drag")) photo_input(PH_DRAG, (int)lrintf(a * 10000.f), (int)lrintf(b * 10000.f));
    else if (!strcasecmp(w, "pan")) photo_input(PH_PAN, (int)lrintf(a * 10000.f), (int)lrintf(b * 10000.f));
    else if (!strcasecmp(w, "dolly")) photo_input(PH_DOLLY, (int)lrintf(a * 1000.f), 0);
    else if (!strcasecmp(w, "fov")) photo_input(PH_FOV, (int)lrintf(a * 100.f), 0);
    else if (!strcasecmp(w, "roll")) photo_input(PH_ROLL, (int)lrintf(a * 100.f), 0);
    else return 0;
#undef PACK
    return 1;
}
