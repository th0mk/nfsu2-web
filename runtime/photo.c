/* photo.c -- photo mode: the world stands still, the HUD goes, and the
 * camera comes loose from the car to take a picture with F12 (the page's
 * screenshot).  F12 opens it where it can (web/src/app/photo.ts: the page
 * keeps the keys, the mouse, the controller and the touch screen from the
 * game while it is on and sends what they do here as IEV_PHOTO events), and
 * so does the pause menu's Photo Mode.  Two cameras: orbit (the first) turns
 * about a point at the car, which moves up to ORBIT_PAN_M from it; free flies
 * up to FREE_M from the car.  Both zoom (the field of view) and tilt.  In free
 * roam with friends their names over their cars can show (off at first), and
 * their cars stand where they were (roam_game_hold) as everything else does.
 * With the page's Freeze time off the world runs on instead (traffic, the
 * racers, friends' cars; the camera follows the player's car, which gets no
 * input and coasts), the sound with it, and each frame is the picture as it
 * is (GW_PHOTO_NOW: no frames added up, so no smooth edges or lens blur).
 * Every car keeps its full model however far the camera is, and the cars can
 * be hidden: the others, or all (below, "the cars in the picture").
 *
 * The picture: once the camera stands still, each frame is drawn from a
 * slightly different spot, and the page averages them (GwPhoto, gpu_proto.h):
 * SAMPLES_SHARP frames shifted within a pixel (smooth edges), or, with the
 * lens's blur, SAMPLES_BLUR frames with the eye on a disc as wide as the blur
 * asks and the view sheared so that what is at the focus distance stays put
 * (sharp there, blurred nearer and farther, as a camera lens does).  The focus
 * follows the car (the eye's distance to it) until the page sets one.  The
 * shifts go in where the view is made (overrides.c sub_005c7ec0, photo_view).
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
 *    game runs again.  savegame.c's Save goes in through the same two
 *    patches (after Return to Explore Mode; its Accept before ours).
 *
 * The page (status_web.c) reads:
 *   photo       bits: 1 photo mode can open now, 2 it is on, 4 the free camera,
 *               8 the names on, 16 friends' names can show here, 32 the focus
 *               follows the car; 256 time stands, 512 other players' cars can be here (free roam with them)
 *   photo_fov   the field of view, degrees     photo_roll  the tilt, degrees
 *   photo_n     how many times it opened       photo_focus the focus distance, m
 *   photo_path  the camera path's points, + 16 while it plays
 *   photo_target the kind of car the camera is on (KIND_*: 0 the player's, 1 a friend's, 2 a crew-mate's, 3 another
 *               player's, 4 traffic, 5 a racer's) */
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
int roam_game_online_here(void);                         /* ...free roam with other players */
int roam_game_names(CPU *c, uint32_t *obj, int max);
uint32_t language_add_text(const char *name, const char *text);

void roam_game_hold(int on);                             /* roam_game.c: friends' cars held where they are */
int roam_game_slot_kind(uint32_t car, const char **name);/* ...and what player's a car is (-1 none of theirs) */
void horn_mute(int on);                                  /* horn.c: every horn quiet */
int lightkey_state(uint32_t car);                        /* lightkey.c: a car's lights (1 off) */

/* status_web.c in the browser; the Mac has no page */
__attribute__((weak)) int host_status_slot(const char *name){ (void)name; return -1; }
__attribute__((weak)) void host_status_set_n(const int *slot, const double *value, int n){ (void)slot; (void)value; (void)n; }
/* gpu_web.c in the browser (web/gpu_proto.h GwPhoto: GW_PHOTO_OFF 0, LIVE 1, ADD 2, DONE 3); the Mac averages nothing */
__attribute__((weak)) void gw_photo(uint32_t state, uint32_t n, uint32_t target, uint32_t gen){ (void)state; (void)n; (void)target; (void)gen; }
__attribute__((weak)) uint32_t gw_photo_got(uint32_t gen){ (void)gen; return 0; }
__attribute__((weak)) void gw_size(unsigned *w, unsigned *h){ *w = 0; *h = 0; }

#define HOOK_RET       0x00581475u   /* the main-loop hook's return address, as guest calls see it */
#define G_FLOW         0x008654a4u   /* 6: in the world */
#define G_RACE         0x00890118u   /* the race status: +0 4 racing, 6 paused */
#define G_RACEKIND     0x0089e7b0u   /* 0 free roam */
#define G_NET_ONLINE   0x0089cf69u   /* byte: an online session */
#define G_NET_CLIENT   0x008669f4u   /* > 0 in a network race */
#define G_NET_HOST     0x00866a14u
#define G_PLAYER1      SPLIT_PLAYERS_INDEX   /* Player*: +4 its car, +0x9A4 its HUD, +0x238 its camera mover */
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
#define NAMES_MAX      128           /* the names over friends' cars and their flags (roam_game_names) */
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
#define ORBIT_PAN_M    50.f          /* how far that point moves from it, sideways */
#define ORBIT_UP_M     30.f          /* and up */
#define DIST_MIN       1.2f
#define DIST_MAX       60.f          /* (nothing keeps the camera out of walls: in a street the houses can be in the way) */
#define FREE_M         100.f         /* the free camera from the car's centre */
#define EYE_MIN_Z      -0.3f         /* the eye above the car's position, at least */
#define MOVE_MPS       5.f
#define TURN_DPS       100.f
#define FOV_MIN        15.f
#define FOV_MAX        110.f
#define ROLL_MAX       45.f
#define RATE_DPS       30.f          /* the zoom and tilt keys */
#define DEG            0.017453292f
#define PI             3.14159265f
#define SNAP_M         0.001f        /* the smoothed eye this near where it goes: there (a still camera is still) */
#define SAMPLES_SHARP  16            /* the frames of a still picture */
#define SAMPLES_BLUR   160           /*   with the lens's blur, at most */
#define BLUR_FAR       0.03f         /* the most blur: what is far away over a disc of 1.5 % of the view's width (radius;
                                        the blur's square times this) */
#define BLUR_PX_AREA   12.f          /* a frame for each this many pixels of that disc */
#define G_HEADLIGHTS   0x00870cc4u   /* g_CarHeadlightEnable: off while photo mode hides every car (no lit road with no car) */
#define G_MOTION_BLUR  0x00870cdcu   /* g_MotionBlurEnable: off while photo mode is on (the world stands; the car's
                                        speed would still streak the picture's edges and blend in the frame before) */
#define FOCUS_MIN      0.5f
#define FOCUS_MAX      500.f
#define NCAM           11            /* the camera's values that glide: yaw, pitch, dist, off[3], eye[3], fov, roll */
#define GLIDE_FAST     30.f          /* the glide's spring at the least Smoothing (it settles in about 0.15 s) */
#define GLIDE_SLOW     2.f           /*   and at the most (about 2.5 s) */
#define GLIDE_SNAP     0.002f        /* the glide this near where it goes and this slow: there (a still camera is still) */
#define PATH_MAX       8             /* the camera path's points */
#define PATH_S         10.f          /* its time, s, unless the page says */
#define PATH_S_MAX     120.f
#define SPIN_MAX       90.f          /* turning by itself, degrees a second at most */
#define G_WORLD        0x00890080u   /* World*: its car list (car +0 the next) has the World as its head and end */
#define G_RAIN         0x00832eb4u   /* Rain* (eView 1 +0x64): +0x1F0 its strength, +0x5098/+0x509C the clouds */
#define CAR_DRIVER     0x24u         /* 1 the player, 3 an AI racer, 4 traffic, 0 a free-roam player's (roam_game.c) */
#define DRIVER_RACER   3u
#define DRIVER_TRAFFIC 4u
#define CAR_HL_ON      0x93du        /* byte: its headlights are on (lightkey.c: the world's step sets it) */
#define CAR_ACTIVE     0x550u
#define CAR_BLINKOUT   0x998u
#define PARK_Z         (-123456.f)   /* Car::Deactivate's parking spot */
#define TARGET_FAR_M   150.f         /* the camera on another car: no farther than this from the player's */
#define DARK_MAX       128           /* other cars whose headlights photo mode turned off */

/* ---- what the page sends (the input thread) ----------------------------------- */
enum { PH_HELLO = 0, PH_OPEN = 1, PH_HOLD = 2, PH_MOVE = 3, PH_TURN = 4, PH_DRAG = 5, PH_PAN = 6, PH_DOLLY = 7,
       PH_FOV = 8, PH_ROLL = 9, PH_MODE = 10, PH_RESET = 11, PH_NAMES = 12, PH_FOCUS = 13, PH_BLUR = 14, PH_CARS = 15, PH_FREEZE = 16,
       PH_SPEED = 17, PH_SMOOTH = 18, PH_LEVEL = 19, PH_SPIN = 20, PH_PATH = 21, PH_SHOW = 22, PH_LIGHTS = 23, PH_HORNS = 24,
       PH_WEATHER = 25, PH_TARGET = 26, PH_SHUTTER = 27, PH_SIZE = 28, PH_SHIFT = 29, PH_LENS = 30 };
/* PH_PATH's: the camera's path */
enum { PATH_CLEAR = 0, PATH_ADD = 1, PATH_PLAY = 2, PATH_STOP = 3 };
/* PH_HOLD's bits: keys and buttons held */
enum { HB_FWD = 1, HB_BACK = 2, HB_LEFT = 4, HB_RIGHT = 8, HB_UP = 16, HB_DOWN = 32, HB_TL = 64, HB_TR = 128,
       HB_TU = 256, HB_TD = 512, HB_ZIN = 4096, HB_ZOUT = 8192, HB_RL = 16384, HB_RR = 32768, HB_FAST = 65536 };
enum { CARS_ALL = 0, CARS_MINE = 1, CARS_NONE = 2, CARS_BUDDIES = 3 };    /* the cars shown (3: the player's, its
                                                                            friends' and crew-mates'), as SHOW_* */
/* the kinds of cars, and PH_SHOW's bits for them (the page's Cars boxes) */
enum { KIND_MINE = 0, KIND_FRIEND = 1, KIND_CREW = 2, KIND_PLAYER = 3, KIND_TRAFFIC = 4, KIND_RACER = 5 };
enum { SHOW_MINE = 1, SHOW_FRIENDS = 2, SHOW_CREWS = 4, SHOW_PLAYERS = 8, SHOW_TRAFFIC = 16, SHOW_RACERS = 32, SHOW_ALL = 63,
       SHOW_PEOPLE = SHOW_FRIENDS | SHOW_CREWS | SHOW_PLAYERS };
/* PH_TARGET's: the camera on the player's car, or on the car nearest the middle of the picture (another each time) */
enum { TARGET_MINE = 0, TARGET_MIDDLE = 1 };
#define NONE_SET INT32_MIN
static struct {
    atomic_int page, open, hold, move, lift, turn, dx, dy, px, py, dolly, fov, roll, mode, reset, names, focus, blur, cars, freeze;
    atomic_int speed, smooth, level, spin, path, path_ms;
    atomic_int show, lights, horns, weather, target;             /* the scene's: kinds of cars shown, their lights, horns,
                                                                    the weather; the car the camera is on */
    atomic_int shutter, size, shift_x, shift_y, lens;            /* the picture's: shutter time, size, lens shift, lens */
} I = { .fov = NONE_SET, .roll = NONE_SET, .mode = -1, .names = -1, .focus = NONE_SET, .blur = NONE_SET, .cars = -1, .freeze = -1,
        .speed = NONE_SET, .smooth = NONE_SET, .level = -1, .spin = NONE_SET, .path = -1,
        .show = -1, .lights = -1, .horns = -1, .weather = NONE_SET, .target = -1,
        .shutter = -1, .size = -1, .shift_x = NONE_SET, .shift_y = NONE_SET, .lens = -1 };

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
    case PH_FOCUS: atomic_store(&I.focus, a); break;                              /* cm; -1 the car */
    case PH_BLUR:  atomic_store(&I.blur, a); break;                               /* 0 .. 1000 */
    case PH_CARS:  atomic_store(&I.cars, a); break;                               /* 0 all, 1 only the player's, 2 none */
    case PH_FREEZE: atomic_store(&I.freeze, a); break;                            /* time: 0 runs on, 1 stands */
    case PH_SPEED: atomic_store(&I.speed, a); break;                              /* the keys' and sticks' speed, % */
    case PH_SMOOTH: atomic_store(&I.smooth, a); break;                            /* 0 .. 1000 */
    case PH_LEVEL: atomic_store(&I.level, a); break;                              /* the free camera keeps its height: 0, 1 */
    case PH_SPIN:  atomic_store(&I.spin, a); break;                               /* turning by itself, 1/10 degree a second */
    case PH_PATH:  atomic_store(&I.path_ms, b); atomic_store(&I.path, a); break;  /* PATH_*; play: its time, ms */
    case PH_SHOW:  atomic_store(&I.show, a); break;                               /* SHOW_* bits: the kinds of cars shown */
    case PH_LIGHTS: atomic_store(&I.lights, a); break;                            /* other cars' lights: 0 off, 1 on */
    case PH_HORNS: atomic_store(&I.horns, a); break;                              /* horns heard: 0, 1 */
    case PH_WEATHER: atomic_store(&I.weather, a); break;                          /* -1 as it is, 0 dry, 1 rain */
    case PH_TARGET: atomic_store(&I.target, a); break;                            /* TARGET_*: the car the camera is on */
    case PH_SHUTTER: atomic_store(&I.shutter, a); break;                          /* the shutter's time, ms (0: none) */
    case PH_SIZE:  atomic_store(&I.size, a); break;                               /* the picture: 1 the screen's, 2 .. 4 times */
    case PH_SHIFT: atomic_store(&I.shift_x, a); atomic_store(&I.shift_y, b); break; /* lens shift, 1/1000 of the view's half */
    case PH_LENS:  atomic_store(&I.lens, a); break;                               /* 0 normal, 1 fisheye */
    default: break;
    }
}

#define NSLOTS 7
static struct {
    int on, free, names, drawn, patched, slots, slot[NSLOTS];
    int drive_on;                                /* time runs on while photo mode is on (the page's Freeze time off) */
    int game_lod;                                /* tests (photo fulldetail 0): the cars' models as the game picks them */
    long n, pause_at;
    double t, val[NSLOTS];
    uint32_t freeze_was, label, quiet, after;    /* after: the Replay item Photo Mode goes in behind */
    uint32_t least[2];                           /* the Car detail option's least models, while photo mode has 0 there */
    uint32_t mb_was, hl_was;                     /* the Motion Blur and Headlights options, while photo mode changes them */
    int speed_blur;                              /* tests (photo speedblur 1): the game's motion blur stays on */
    const char *lang;
    float yaw, pitch, dist, off[3], eye[3];      /* the view's direction; orbit: from its point; free: from the car's centre */
    float yaw0, pitch0, dist0, off0[3], fov0, fov, roll;
    float se[3], sl[3];                          /* the eye and the point looked at, smoothed, world */
    float focus, blur;                           /* the focus distance (m; focus_auto: the car's), the lens's blur 0 .. 1 */
    int focus_auto;
    float speed, smooth, spin;                   /* the keys' and sticks' speed (0: 1, as it was), the glide 0 .. 1, turning
                                                    by itself (degrees a second) */
    int level;                                   /* the free camera keeps its height (forward is along the ground) */
    float cv[NCAM], cvel[NCAM];                  /* the camera as it glides there (cam_pack's order), and its speeds */
    int snap;                                    /* the glide jumps there (a new mode: its values mean other things) */
    int jump;                                    /* the eye and the point jump there too (the camera on another car) */
    int show;                                    /* SHOW_* the kinds of cars shown (SHOW_ALL at first: see photo_frame) */
    int lights, horns, weather;                  /* other cars' headlights; horns heard; -1 the game's weather, 0 dry, 1 rain */
    int shown_set;                               /* show, lights and horns given their first values */
    uint32_t target;                             /* the car the camera is on (0: the player's) */
    int target_kind;                             /* its KIND_* */
    uint32_t dark[DARK_MAX]; int ndark;          /* the cars whose headlights photo mode turned off */
    float rain_was[3]; int rain_saved;           /* the rain's strength and clouds when photo mode came on */
    int path_n, path_play;                       /* the path's points; it plays */
    float path_e[PATH_MAX][3], path_l[PATH_MAX][3], path_fov[PATH_MAX], path_roll[PATH_MAX];   /* from the car's centre */
    float path_t, path_s;                        /* how far it played, s, of its time */
    int acc, acc_n;                              /* the still picture's frames the page has, of acc_n */
    uint32_t gen, said_gen;                      /* the still picture's number (1 .. 0xFFFF); the last one logged */
    float sig[13];                               /* what the picture shows, the last frame (a change starts it again) */
    float jit[5];                                /* this frame's shift: within the pixel (x, y, as parts of the view's
                                                    half), far away's move over the lens (x, y, parts of the view's
                                                    half width), the focus distance (m) */
} P;
/* the car renders of this frame in photo mode, and those only it gave the full model; the frame before's */
static int cars_drawn, cars_full, cars_drawn1, cars_full1;

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
    atomic_store(&I.mode, -1); atomic_store(&I.reset, 0); atomic_store(&I.focus, NONE_SET);
}

/* ---- the cars: what kind each is, the car the camera is on, the other cars' headlights ------------------------- */
static int car_live(uint32_t car){              /* a car of the world's in play: active, not parked, not blinking out */
    return ok(car, CAR_BLINKOUT) && MR8(car + CAR_ACTIVE) && !MR8(car + CAR_BLINKOUT) && fabsf(MRF32(car + 0x68u) - PARK_Z) >= 0.001f;
}
static int car_kind(uint32_t car, uint32_t mine){
    if (car == mine) return KIND_MINE;
    uint32_t d = MR32(car + CAR_DRIVER);
    if (d == DRIVER_TRAFFIC) return KIND_TRAFFIC;
    if (d == DRIVER_RACER) return KIND_RACER;
    int k = roam_game_slot_kind(car, 0);
    return k == 1 ? KIND_FRIEND : k == 2 ? KIND_CREW : k == 0 ? KIND_PLAYER : KIND_TRAFFIC;   /* (anything else: as traffic) */
}
static int kind_shown(int kind){
    static const int bit[] = { SHOW_MINE, SHOW_FRIENDS, SHOW_CREWS, SHOW_PLAYERS, SHOW_TRAFFIC, SHOW_RACERS };
    return (P.show & bit[kind]) != 0;
}
/* the world's cars, each in play (at most 256: the list is the game's) */
static int world_cars(uint32_t *out, int max){
    uint32_t w = rd32(G_WORLD);
    int n = 0, guard = 0;
    if (!w) return 0;
    for (uint32_t car = rd32(w); car && car != w && guard < 256 && n < max; car = rd32(car), guard++){
        if (!ok(car, CAR_BLINKOUT)) break;
        if (car_live(car)) out[n++] = car;
    }
    return n;
}
static float dist_of(uint32_t a, uint32_t b){
    float pa[3], pb[3];
    rd3(a + 0x60u, pa); rd3(b + 0x60u, pb);
    return sqrtf((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2]));
}
/* the camera on another car (0: the player's): the orbit about it at the same distance and angle, the free camera
 * where it is (from then on going with that car); no glide over the way between them */
static void set_target(uint32_t car, uint32_t mine){
    if (car == mine) car = 0;
    if (car == P.target) return;
    float cc[3];
    centre(car ? car : mine, cc);
    P.target = car; P.target_kind = car ? car_kind(car, mine) : KIND_MINE;
    memset(P.off, 0, sizeof P.off);
    for (int i = 0; i < 3; i++) P.eye[i] = P.se[i] - cc[i];
    P.snap = 1; P.jump = 1; P.focus_auto = 1;
    static const char *const kn[] = { "the player's", "a friend's", "a crew-mate's", "a player's", "traffic", "a racer's" };
    fprintf(stderr, "[photo] f=%ld the camera on %s car\n", autotest_frame(), kn[P.target_kind]);
}
/* the car nearest the middle of the picture, within 35 degrees of it (another than the one the camera is on; the
 * player's too); 0: none */
static uint32_t middle_car(uint32_t mine){
    uint32_t cars[256];
    int n = world_cars(cars, 256);
    float a[3] = { P.sl[0] - P.se[0], P.sl[1] - P.se[1], P.sl[2] - P.se[2] }, al = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (al < 1e-3f) return 0;
    uint32_t best = 0; float bc = cosf(35.f * DEG);
    for (int i = 0; i < n; i++){
        uint32_t car = cars[i];
        if (car == (P.target ? P.target : mine) || !kind_shown(car_kind(car, mine))) continue;
        if (car != mine && dist_of(car, mine) > TARGET_FAR_M) continue;
        float c[3], v[3], vl = 0.f, dot = 0.f;
        centre(car, c);
        for (int k = 0; k < 3; k++){ v[k] = c[k] - P.se[k]; vl += v[k] * v[k]; dot += v[k] * a[k]; }
        vl = sqrtf(vl);
        if (vl < 1.f || vl > 250.f) continue;
        float cs = dot / (vl * al);
        if (cs > bc){ bc = cs; best = car; }
    }
    return best;
}
/* the camera's car still there (in the world's list, in play, near the player's) */
static int target_there(uint32_t mine){
    uint32_t cars[256];
    int n = world_cars(cars, 256);
    for (int i = 0; i < n; i++) if (cars[i] == P.target) return dist_of(P.target, mine) <= TARGET_FAR_M;
    return 0;
}
/* the other cars' headlights off (their byte after the world's step, before the frame is drawn, as lightkey.c does), or
 * back on: the cars this turned off, but a friend's whose own lights are off (lightkey.c) */
static void lights_restore(void){
    for (int i = 0; i < P.ndark; i++){
        uint32_t car = P.dark[i];
        if (car_live(car) && ok(car, CAR_HL_ON) && lightkey_state(car) != 1) MR8(car + CAR_HL_ON) = 1;
    }
    P.ndark = 0;
}
static void lights_off(uint32_t mine){
    uint32_t cars[256];
    int n = world_cars(cars, 256);
    for (int i = 0; i < n; i++){
        uint32_t car = cars[i];
        if (car == mine || !ok(car, CAR_HL_ON) || !MR8(car + CAR_HL_ON)) continue;
        MR8(car + CAR_HL_ON) = 0;
        int k = 0;
        while (k < P.ndark && P.dark[k] != car) k++;
        if (k == P.ndark && P.ndark < DARK_MAX) P.dark[P.ndark++] = car;
    }
}
/* rain.c's (overrides.c sub_0061f120): the weather photo mode asks for, -1 the game's; whether its world stands */
int photo_weather(void){ return P.on ? P.weather : -1; }
/* headlights.c: a car whose headlights photo mode turned off casts no light on the road either */
int photo_dark(uint32_t car){ return P.on && !P.lights && car != car_of(player()); }
int photo_stands(void){ return P.on && !P.drive_on; }

#define G_LEAST_LOD_A  0x008a1c10u   /* the least of a car's models its render may draw (the Car detail option's: */
#define G_LEAST_LOD_B  0x008a1c14u   /*   0x5D2850 writes 1 for Low, which never draws the full model) */

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
    MR32(G_FREEZE) = P.drive_on ? P.freeze_was : 1u;
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
    if (!P.drive_on) hush(c);
    P.least[0] = MR32(G_LEAST_LOD_A); P.least[1] = MR32(G_LEAST_LOD_B);
    MR32(G_LEAST_LOD_A) = 0; MR32(G_LEAST_LOD_B) = 0;
    P.mb_was = MR32(G_MOTION_BLUR); P.hl_was = MR32(G_HEADLIGHTS);
    if (!P.speed_blur) MR32(G_MOTION_BLUR) = 0;
    roam_game_hold(!P.drive_on);
    horn_mute(!P.horns);
    uint32_t rn = rd32(G_RAIN);                     /* the rain as it is: back so after a photo's own weather */
    P.rain_saved = ok(rn, 0x50a3u);
    if (P.rain_saved){ P.rain_was[0] = MRF32(rn + 0x1f0u); P.rain_was[1] = MRF32(rn + 0x5098u); P.rain_was[2] = MRF32(rn + 0x509cu); }
    P.target = 0; P.target_kind = KIND_MINE; P.path_play = 0;
    P.focus_auto = 1; P.acc = 0; P.acc_n = 0;
    P.on = 1; P.n++; P.drawn = 0;
    fprintf(stderr, "[photo] f=%ld on: the world %s, the orbit at %.1f m, field of view %.1f\n", autotest_frame(),
            P.drive_on ? "runs on" : "stands", P.dist, P.fov);
}
static void leave(CPU *c, const char *why){
    uint32_t pl = player();
    if (MR32(G_FLOW) == 6u && pl && mover_type(pl) == MOVER_DEBUG){ uint32_t z = 0; gcall(c, FN_OWN_CAMERA, pl, 1, &z); }
    MR32(G_FREEZE) = P.freeze_was;
    volumes(c, G_VOLUMES);
    MR32(G_LEAST_LOD_A) = P.least[0]; MR32(G_LEAST_LOD_B) = P.least[1];
    MR32(G_MOTION_BLUR) = P.mb_was; MR32(G_HEADLIGHTS) = P.hl_was;
    roam_game_hold(0);
    lights_restore(); horn_mute(0);
    uint32_t rn = rd32(G_RAIN);
    if (P.weather >= 0 && P.rain_saved && ok(rn, 0x50a3u)){
        MRF32(rn + 0x1f0u) = P.rain_was[0]; MRF32(rn + 0x5098u) = P.rain_was[1]; MRF32(rn + 0x509cu) = P.rain_was[2]; }
    P.target = 0; P.path_play = 0;
    memset(P.jit, 0, sizeof P.jit);
    gw_photo(0, 0, 0, 0);
    P.on = 0; P.pause_at = 0;
    fprintf(stderr, "[photo] f=%ld off (%s)\n", autotest_frame(), why);
}

/* the camera's values that glide, in one row (glide) */
static void cam_pack(float *v){
    v[0] = P.yaw; v[1] = P.pitch; v[2] = P.dist; memcpy(v + 3, P.off, 12); memcpy(v + 6, P.eye, 12); v[9] = P.fov; v[10] = P.roll;
}
static float wrap_pi(float a){ while (a > PI) a -= 2.f * PI; while (a < -PI) a += 2.f * PI; return a; }

/* the camera glides to where the input put it: a spring that neither overshoots nor jerks (critically damped), so a
 * key, a stick or a drag starts it softly and it comes to a stop softly; Smoothing 0: there at once, as it was */
static void glide(float dt){
    float t[NCAM];
    cam_pack(t);
    if (P.smooth <= 0.f || P.snap){ memcpy(P.cv, t, sizeof t); memset(P.cvel, 0, sizeof P.cvel); P.snap = 0; return; }
    float w = GLIDE_FAST * powf(GLIDE_SLOW / GLIDE_FAST, P.smooth);
    t[0] = P.cv[0] + wrap_pi(t[0] - P.cv[0]);                       /* the yaw the near way round */
    int n = (int)ceilf(dt * 240.f);
    float h = n > 0 ? dt / (float)n : 0.f, far = 0.f;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < NCAM; i++){
            P.cvel[i] += (w * w * (t[i] - P.cv[i]) - 2.f * w * P.cvel[i]) * h;
            P.cv[i] += P.cvel[i] * h;
        }
    for (int i = 0; i < NCAM; i++) far = fmaxf(far, fmaxf(fabsf(t[i] - P.cv[i]), fabsf(P.cvel[i]) * 0.1f));
    if (far < GLIDE_SNAP){ cam_pack(P.cv); memset(P.cvel, 0, sizeof P.cvel); }
    P.cv[0] = wrap_pi(P.cv[0]);
}

/* ---- the camera's path: points the page marks (where the camera looks from and at, from the car's centre, its zoom
 * and tilt), played through in the time the page gives, along a Catmull-Rom curve, slowly at its start and end ---- */
static float path_w(int k){                     /* a stretch's share of the time: how far the camera goes on it */
    float a = 0.f, b = 0.f;
    for (int i = 0; i < 3; i++){
        float de = P.path_e[k + 1][i] - P.path_e[k][i], dl = P.path_l[k + 1][i] - P.path_l[k][i];
        a += de * de; b += dl * dl;
    }
    return sqrtf(a) + 0.5f * sqrtf(b) + 0.2f;
}
static float cr(float p0, float p1, float p2, float p3, float u){
    return 0.5f * (2.f * p1 + (p2 - p0) * u + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * u * u + (3.f * p1 - p0 - 3.f * p2 + p3) * u * u * u);
}
/* where the path is at x (0 .. 1 of its length): eye, point, zoom, tilt */
static void path_at(float x, float *e, float *l, float *fov, float *roll){
    int n = P.path_n;
    float total = 0.f, s;
    for (int k = 0; k + 1 < n; k++) total += path_w(k);
    s = x * total;
    int k = 0;
    for (; k + 2 < n && s > path_w(k); k++) s -= path_w(k);
    float u = clampf(s / path_w(k), 0.f, 1.f);
    int a = k > 0 ? k - 1 : 0, b = k, c = k + 1, d = k + 2 < n ? k + 2 : n - 1;
    for (int i = 0; i < 3; i++){
        e[i] = cr(P.path_e[a][i], P.path_e[b][i], P.path_e[c][i], P.path_e[d][i], u);
        l[i] = cr(P.path_l[a][i], P.path_l[b][i], P.path_l[c][i], P.path_l[d][i], u);
    }
    *fov = clampf(cr(P.path_fov[a], P.path_fov[b], P.path_fov[c], P.path_fov[d], u), FOV_MIN, FOV_MAX);
    *roll = clampf(cr(P.path_roll[a], P.path_roll[b], P.path_roll[c], P.path_roll[d], u), -ROLL_MAX, ROLL_MAX);
}
/* the path stops (played to its end, or asked): the free camera where it is, so the input goes on from there */
static void path_stop(const float *cc){
    if (!P.path_play) return;
    P.path_play = 0;
    float v[3] = { P.sl[0] - P.se[0], P.sl[1] - P.se[1], P.sl[2] - P.se[2] }, l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    P.free = 1;
    for (int i = 0; i < 3; i++) P.eye[i] = P.se[i] - cc[i];
    if (l > 1e-3f){ P.yaw = atan2f(v[1], v[0]); P.pitch = clampf(asinf(clampf(v[2] / l, -1.f, 1.f)), -85.f * DEG, 85.f * DEG); }
    P.snap = 1;
    fprintf(stderr, "[photo] f=%ld the path stopped\n", autotest_frame());
}
static void path_input(const float *cc){
    int pa = atomic_exchange(&I.path, -1), ms = atomic_load(&I.path_ms);
    if (pa == PATH_CLEAR){ path_stop(cc); P.path_n = 0; fprintf(stderr, "[photo] f=%ld the path cleared\n", autotest_frame()); }
    else if (pa == PATH_ADD && !P.path_play && P.path_n < PATH_MAX){
        int k = P.path_n++;
        for (int i = 0; i < 3; i++){ P.path_e[k][i] = P.se[i] - cc[i]; P.path_l[k][i] = P.sl[i] - cc[i]; }
        P.path_fov[k] = P.cv[9]; P.path_roll[k] = P.cv[10];
        fprintf(stderr, "[photo] f=%ld path point %d\n", autotest_frame(), P.path_n);
    }
    else if (pa == PATH_PLAY && P.path_n >= 2){
        P.path_play = 1; P.path_t = 0.f; P.path_s = ms > 0 ? clampf((float)ms / 1000.f, 1.f, PATH_S_MAX) : PATH_S;
        fprintf(stderr, "[photo] f=%ld the path plays: %d points in %.1f s\n", autotest_frame(), P.path_n, P.path_s);
    }
    else if (pa == PATH_STOP) path_stop(cc);
}

static int capture_view(const float *cc);         /* (below: a picture being taken, the camera waits) */
/* the camera, one frame: what the page sends, then where it looks from and at */
static void steer(uint32_t car, uint32_t mine, float dt){
    int hold = atomic_load(&I.hold), mv = atomic_load(&I.move), lift = atomic_load(&I.lift), tn = atomic_load(&I.turn);
    float spd = P.speed > 0.f ? P.speed : 1.f, fast = (hold & HB_FAST ? 3.f : 1.f) * spd;
    float mx = (float)(!!(hold & HB_RIGHT) - !!(hold & HB_LEFT)) + s16(mv, 0);
    float my = (float)(!!(hold & HB_FWD) - !!(hold & HB_BACK)) + s16(mv, 1);
    float mz = (float)(!!(hold & HB_UP) - !!(hold & HB_DOWN)) + s16(lift, 0);
    float dolly = s16(lift, 1) * 3.f * spd * dt + (float)atomic_exchange(&I.dolly, 0) / 1000.f;
    float ty = (float)(!!(hold & HB_TL) - !!(hold & HB_TR)) - s16(tn, 0), tp = (float)(!!(hold & HB_TU) - !!(hold & HB_TD)) - s16(tn, 1);
    float ddx = (float)atomic_exchange(&I.dx, 0) / 10000.f, ddy = (float)atomic_exchange(&I.dy, 0) / 10000.f;
    float pdx = (float)atomic_exchange(&I.px, 0) / 10000.f, pdy = (float)atomic_exchange(&I.py, 0) / 10000.f;
    int fs = atomic_exchange(&I.fov, NONE_SET), rs = atomic_exchange(&I.roll, NONE_SET), md = atomic_exchange(&I.mode, -1);
    int rst = atomic_exchange(&I.reset, 0);
    float cc[3], d[3], e[3], l[3];
    if (rst && P.target){ set_target(0, mine); car = mine; }         /* Reset: the player's car again */
    centre(car, cc);
    path_input(cc);
    if (capture_view(cc)) goto lens;                                  /* a picture being taken: the camera waits */
    if (P.path_play && (rst || md >= 0)) path_stop(cc);                /* Reset or the other camera: the path stops */
    if (P.path_play){
        /* the path: the input waits (what came is dropped), the camera goes along it */
        P.path_t += dt;
        float x = clampf(P.path_t / P.path_s, 0.f, 1.f), fov, roll, pe[3], pl[3];
        path_at(x * x * (3.f - 2.f * x), pe, pl, &fov, &roll);
        for (int i = 0; i < 3; i++){ P.se[i] = cc[i] + pe[i]; P.sl[i] = cc[i] + pl[i]; }
        P.fov = P.cv[9] = fov; P.roll = P.cv[10] = roll;
        if (x >= 1.f) path_stop(cc);
        goto lens;
    }
    if (rst){
        P.free = 0; memcpy(P.off, P.off0, sizeof P.off);
        P.yaw = P.yaw0; P.pitch = P.pitch0; P.dist = P.dist0; P.fov = P.fov0; P.roll = 0; P.focus_auto = 1;
    }
    if (md >= 0 && (md == 2 ? !P.free : md) != P.free){
        dir(P.yaw, P.pitch, d);
        if (!P.free){                                   /* free from where the orbit looks from */
            for (int i = 0; i < 3; i++) P.eye[i] = P.se[i] - cc[i];
        } else {                                        /* an orbit about the point as far ahead as the car */
            float ee[3], v[3];
            for (int i = 0; i < 3; i++){ ee[i] = cc[i] + P.eye[i]; v[i] = cc[i] - ee[i]; }
            float ll = clampf(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), DIST_MIN, DIST_MAX), p[3];
            for (int i = 0; i < 3; i++) p[i] = ee[i] + d[i] * ll;
            for (int i = 0; i < 3; i++) P.off[i] = p[i] - cc[i];
            P.dist = ll;
        }
        P.free = !P.free; P.snap = 1;
        fprintf(stderr, "[photo] f=%ld the %s camera\n", autotest_frame(), P.free ? "free" : "orbit");
    }
    /* turning: the keys and the stick at TURN_DPS (times Speed), a drag across the view's height about 140 degrees;
     * and by itself (the page's Turn: about the car in the orbit, round where it stands in the free camera) */
    P.yaw += ty * TURN_DPS * DEG * fast * dt - ddx * 2.4f + P.spin * DEG * dt;
    P.pitch += tp * TURN_DPS * DEG * fast * dt - ddy * 2.4f;
    P.pitch = P.free ? clampf(P.pitch, -85.f * DEG, 85.f * DEG) : clampf(P.pitch, -85.f * DEG, 20.f * DEG);
    P.yaw = wrap_pi(P.yaw);
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
        const float *fw = P.level ? f : d;               /* Keep height: forward along the ground */
        for (int i = 0; i < 3; i++) P.eye[i] += (fw[i] * my + r[i] * mx) * step + d[i] * dolly * 0.5f - r[i] * pdx * 5.f;
        P.eye[2] += mz * step + pdy * 5.f;
        float ll = sqrtf(P.eye[0] * P.eye[0] + P.eye[1] * P.eye[1] + P.eye[2] * P.eye[2]);
        if (ll > FREE_M) for (int i = 0; i < 3; i++) P.eye[i] *= FREE_M / ll;
    }
    float zr = (float)(!!(hold & HB_ZOUT) - !!(hold & HB_ZIN)), rr = (float)(!!(hold & HB_RR) - !!(hold & HB_RL));
    P.fov = clampf(fs != NONE_SET ? (float)fs / 100.f : P.fov + zr * RATE_DPS * spd * dt, FOV_MIN, FOV_MAX);
    P.roll = clampf(rs != NONE_SET ? (float)rs / 100.f : P.roll + rr * RATE_DPS * spd * dt, -ROLL_MAX, ROLL_MAX);
    /* where from and at, as the camera glides there (Smoothing): the eye kept over the car's ground, then smoothed a
     * little more (a jerky drag or a key reads as a glide) */
    glide(dt);
    {
        float gd[3], *g = P.cv;
        dir(g[0], g[1], gd);
        if (!P.free){
            for (int i = 0; i < 3; i++){ l[i] = cc[i] + g[3 + i]; e[i] = l[i] - gd[i] * g[2]; }
        } else {
            for (int i = 0; i < 3; i++){ e[i] = cc[i] + g[6 + i]; l[i] = e[i] + gd[i] * 10.f; }
        }
    }
    float zmin = cc[2] - CENTRE_Z + EYE_MIN_Z;
    if (e[2] < zmin){
        l[2] += zmin - e[2]; e[2] = zmin;
        if (P.free && P.eye[2] < zmin - cc[2]){ P.eye[2] = zmin - cc[2]; P.cv[8] = fmaxf(P.cv[8], P.eye[2]); }
    }
    if (P.jump){ memcpy(P.se, e, sizeof P.se); memcpy(P.sl, l, sizeof P.sl); P.jump = 0; }
    float k = 1.f - expf(-dt * 14.f), far = 0.f;
    for (int i = 0; i < 3; i++){
        P.se[i] += (e[i] - P.se[i]) * k; P.sl[i] += (l[i] - P.sl[i]) * k;
        far = fmaxf(far, fmaxf(fabsf(e[i] - P.se[i]), fabsf(l[i] - P.sl[i])));
    }
    if (far < SNAP_M){ memcpy(P.se, e, sizeof P.se); memcpy(P.sl, l, sizeof P.sl); }
lens:;
    /* the lens: the page's focus and blur; the focus follows the car until the page sets one (Reset: the car again) */
    int fo = atomic_exchange(&I.focus, NONE_SET), bl = atomic_exchange(&I.blur, NONE_SET);
    if (fo != NONE_SET){ P.focus_auto = fo < 0; if (fo >= 0) P.focus = clampf((float)fo / 100.f, FOCUS_MIN, FOCUS_MAX); }
    if (bl != NONE_SET) P.blur = clampf((float)bl / 1000.f, 0.f, 1.f);
    /* the focus that follows: what the lens keeps sharp is a plane across the view at that depth, so the orbit's
     * point (the car, unless the orbit was moved off it), on the view's axis, or in the free camera the car's depth
     * along the view (behind the camera: as it was) */
    if (P.focus_auto){
        float a[3] = { P.sl[0] - P.se[0], P.sl[1] - P.se[1], P.sl[2] - P.se[2] }, al = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        float t = (P.free || P.path_play) && al > 1e-3f ? ((cc[0] - P.se[0]) * a[0] + (cc[1] - P.se[1]) * a[1] + (cc[2] - P.se[2]) * a[2]) / al : al;
        if (t > FOCUS_MIN * 0.5f) P.focus = clampf(t, FOCUS_MIN, FOCUS_MAX);
    }
}

/* the radical inverse of i in base b: Halton's points, evenly over 0 .. 1 */
static float halton(int i, int b){
    float f = 1.f, r = 0.f;
    for (; i > 0; i /= b){ f /= (float)b; r += f * (float)(i % b); }
    return r;
}
/* ---- the picture taken (Take photo): bigger than the screen, in tiles, or over a shutter's time -----------------
 * The page asks for a picture (GwPhoto want, at Take photo).  With Size above the screen's (2 .. 4 tiles a side) the
 * game draws the picture again tile by tile, each a part of the view (overrides.c: the projection made N times as big
 * and moved so that tile fills the screen), each tile's frames added up as a still picture's are; the page reads each
 * tile out as it is whole and puts them together.  With a Shutter time the world goes on by that time over the
 * picture's frames: each frame a step of the world after the one before (the main loop's world step, 0x601A60, made
 * that long once the page has the frame before), the camera on the car in the orbit (the car sharp, the street
 * streaked) and where it stood in the free camera (the lights' trails).  Both need time to stand (Freeze time); the
 * camera waits while they are taken.  The fisheye is the page's (the frame drawn wider, bent round in its look); the
 * lens shift moves the picture's middle off the view's (the projection sheared, the game drawing wider so that what
 * comes in is there). */
#define TILES_MAX      4
#define TILE_SAMPLES_BLUR 64         /* a tile's frames with the lens's blur, at most (4 x 4 tiles: 1024) */
#define SHUTTER_MS_MAX 1000
#define SHUTTER_RATE   240.f         /* the shutter's frames for a second of its time */
#define SHUTTER_N_MIN  24
#define SHUTTER_N_MAX  240
#define FISHEYE_WIDER  1.18f         /* the field of view the fisheye is drawn with, times the zoom's (110 -> 130) */
#define SHIFT_MAX      0.6f          /* lens shift, a part of the picture's half width or height */
enum { PIC_SIZE_OK = 1, PIC_SHUTTER_OK = 2, PIC_FISHEYE_OK = 4, PIC_SHIFT_OK = 8, PIC_TAKING = 16 };
static struct {
    int size, shutter_ms, lens;          /* the page's: tiles a side (1: the screen's), the shutter's time, the lens */
    float shift[2];                      /* lens shift: x right, y up, parts of the picture's half width and height */
    int mode;                            /* GW_CAP_*: what is being taken */
    uint32_t want;                       /* the last picture asked for (1 .. 0xFFFF: the page tells it is done by it) */
    uint32_t page_want;                  /* the page's last ask the game saw */
    int take;                            /* tests (photo take): a picture asked for, as the page asks (4 tiles a side) */
    int n, tile;                         /* tiles: a side, the one being drawn */
    int samples, i;                      /* shutter: its frames, the one drawn now */
    float step;                          /* shutter: the world's step between its frames, s */
    int advance, stepped, stepped_now;   /* shutter: the next world step goes on; one went on (this frame's) */
    float cc0[3];                        /* the car's centre where it started (the free camera stays there) */
    uint32_t gen;                        /* the picture's number, as the still picture's (P.gen) */
    double t0;
    int status, slot;                    /* photo_pic as published */
} S = { .size = 1, .status = -1, .slot = -2 };

/* gpu_web.c (web/gpu_proto.h GwPhoto's tile, fov, lens and want); the Mac has no page */
__attribute__((weak)) void gw_photo_more(uint32_t tile, float fov, uint32_t lens){ (void)tile; (void)fov; (void)lens; }
__attribute__((weak)) uint32_t gw_photo_want(void){ return 0; }
#define GW_CAP_NONE    0u
#define GW_CAP_TILES   1u
#define GW_CAP_SHUTTER 2u

/* what can be used now (the page greys out the rest): Size with time standing, the plain lens and no shutter; the
 * Shutter with time standing and the screen's size; the fisheye at the screen's size; lens shift with the plain lens */
static int pic_can(void){
    int st = !P.drive_on;
    return (st && !S.lens && !S.shutter_ms ? PIC_SIZE_OK : 0) | (st && S.size <= 1 ? PIC_SHUTTER_OK : 0) |
           (S.size <= 1 ? PIC_FISHEYE_OK : 0) | (!S.lens ? PIC_SHIFT_OK : 0) | (S.mode ? PIC_TAKING : 0);
}
static void pic_publish(void){
    int v = pic_can();
    if (S.slot == -2) S.slot = host_status_slot("photo_pic");
    if (v == S.status || S.slot < 0) return;
    S.status = v;
    double d = v;
    host_status_set_n(&S.slot, &d, 1);
}

/* the page's picture settings, each frame (a change starts the still picture again) */
static void pic_input(void){
    int sh = atomic_exchange(&I.shutter, -1), sz = atomic_exchange(&I.size, -1), ln = atomic_exchange(&I.lens, -1);
    int sx = atomic_exchange(&I.shift_x, NONE_SET), sy = atomic_exchange(&I.shift_y, NONE_SET);
    int again = 0;
    if (sh >= 0 && sh != S.shutter_ms){ S.shutter_ms = sh > SHUTTER_MS_MAX ? SHUTTER_MS_MAX : sh; }
    if (sz >= 1 && sz != S.size){ S.size = sz > TILES_MAX ? TILES_MAX : sz; }
    if (ln >= 0 && (ln != 0) != S.lens){ S.lens = ln != 0; again = 1; }
    if (sx != NONE_SET || sy != NONE_SET){
        float x = sx != NONE_SET ? clampf((float)sx / 1000.f, -SHIFT_MAX, SHIFT_MAX) : S.shift[0];
        float y = sy != NONE_SET ? clampf((float)sy / 1000.f, -SHIFT_MAX, SHIFT_MAX) : S.shift[1];
        if (x != S.shift[0] || y != S.shift[1]){ S.shift[0] = x; S.shift[1] = y; again = 1; }
    }
    if (sh >= 0 || sz >= 1 || ln >= 0 || again)
        fprintf(stderr, "[photo] f=%ld picture: size %d, shutter %d ms, lens %s, shift %.2f %.2f\n", autotest_frame(),
                S.size, S.shutter_ms, S.lens ? "fisheye" : "plain", (double)S.shift[0], (double)S.shift[1]);
    if (again) P.acc_n = 0;                          /* the still picture again */
}

/* the camera while a picture is taken: where it was (the glide's), the input waiting; the orbit on the car as it goes
 * on, the free camera where it stood; at once, no smoothing (each frame of the picture where it is) */
static int capture_view(const float *cc){
    if (!S.mode) return 0;
    float gd[3], e[3], l[3], *g = P.cv;
    dir(g[0], g[1], gd);
    if (!P.free){ for (int i = 0; i < 3; i++){ l[i] = cc[i] + g[3 + i]; e[i] = l[i] - gd[i] * g[2]; } }
    else { for (int i = 0; i < 3; i++){ e[i] = S.cc0[i] + g[6 + i]; l[i] = e[i] + gd[i] * 10.f; } }
    memcpy(P.se, e, sizeof P.se); memcpy(P.sl, l, sizeof P.sl);
    return 1;
}

/* drifttest.c's 0x601A60: the world's step this frame (a shutter's frame: its time, once the page has the frame
 * before); 0: as the game has it */
int photo_world_step(double *st0){
    if (S.mode != (int)GW_CAP_SHUTTER || !S.advance || !(*st0 > 0.0)) return 0;
    *st0 = (double)S.step; S.advance = 0; S.stepped = 1;
    return 1;
}
/* framerate.c's particle step (smoke, sparks) while a shutter's picture is taken: the world's step of this frame, or
 * none; -1: as it is */
float photo_particle_step(void){
    if (S.mode != (int)GW_CAP_SHUTTER) return -1.f;
    return S.stepped_now ? S.step : 0.f;
}

/* this frame's shift within the pixel (Halton 2, 3) and over the lens (Halton 5, 7), for frame i of a picture */
static void jitter(int i, unsigned w, unsigned h, float coc){
    memset(P.jit, 0, sizeof P.jit);
    P.jit[0] = (halton(i, 2) - 0.5f) * 2.f / (float)w;
    P.jit[1] = (halton(i, 3) - 0.5f) * 2.f / (float)h;
    if (coc > 0.f){
        float r = coc * sqrtf(halton(i, 5)), th = 2.f * PI * halton(i, 7);
        P.jit[2] = r * cosf(th); P.jit[3] = r * sinf(th); P.jit[4] = P.focus;
    }
}

/* a picture asked for: taken in tiles, over the shutter's time, or the still picture as it is (the page's) */
static void capture_start(uint32_t want, const float *cc, unsigned w, unsigned h){
    int most = (int)(want >> 16 & 0xfu), shut = S.shutter_ms > 0 && !P.drive_on && S.size <= 1;
    int n = !P.drive_on && !S.lens && !S.shutter_ms ? (S.size < most ? S.size : most) : 1;
    S.mode = 0;
    if (!w || !h || (!shut && n <= 1)) return;
    S.gen = P.gen = P.gen % 0xfffeu + 1u;
    S.t0 = now_s(); memcpy(S.cc0, cc, sizeof S.cc0);
    P.acc = 0; P.acc_n = 0;                          /* the still picture starts again after */
    if (shut){
        S.mode = GW_CAP_SHUTTER;
        S.samples = (int)clampf(ceilf((float)S.shutter_ms / 1000.f * SHUTTER_RATE), SHUTTER_N_MIN, SHUTTER_N_MAX);
        S.step = (float)S.shutter_ms / 1000.f / (float)S.samples;
        S.i = 0; S.advance = 0; S.stepped = S.stepped_now = 0;
        fprintf(stderr, "[photo] f=%ld taking the picture over %d ms: %d frames, the world %.2f ms on between them\n",
                autotest_frame(), S.shutter_ms, S.samples, (double)S.step * 1e3);
    } else {
        S.mode = GW_CAP_TILES; S.n = n; S.tile = -1;
        fprintf(stderr, "[photo] f=%ld taking the picture in %d x %d tiles of %ux%u: %ux%u\n", autotest_frame(), n, n, w, h,
                w * (unsigned)n, h * (unsigned)n);
    }
}
static void capture_end(const char *how){
    fprintf(stderr, "[photo] f=%ld the picture %s after %.1f s\n", autotest_frame(), how, now_s() - S.t0);
    if (S.mode == (int)GW_CAP_SHUTTER && P.on && !P.drive_on) MR32(G_FREEZE) = 1u;   /* (off: leave() gave the game its own) */
    S.mode = 0; S.advance = 0;
    memset(P.jit, 0, sizeof P.jit);
}

/* a picture being taken, one frame (picture()'s): 1 while it is */
static int capture(unsigned w, unsigned h, float coc){
    if (S.mode == (int)GW_CAP_TILES){
        int nt = S.n * S.n;
        if (S.tile < 0 || (int)gw_photo_got(S.gen) >= P.acc_n){
            if (S.tile >= 0) fprintf(stderr, "[photo] f=%ld tile %d of %d: %d frames\n", autotest_frame(), S.tile + 1, nt, P.acc_n);
            if (++S.tile >= nt){ capture_end("taken in tiles"); return 0; }
            /* each tile its own picture (gen): a tile's blur is N times as wide in its pixels, its frames fewer */
            S.gen = P.gen = P.gen % 0xfffeu + 1u;
            float r_px = coc * (float)w * 0.5f * (float)S.n;
            P.acc_n = (int)clampf(ceilf(PI * r_px * r_px / BLUR_PX_AREA), SAMPLES_SHARP, TILE_SAMPLES_BLUR);
            P.acc = 0;
        } else P.acc = (int)gw_photo_got(S.gen);
        int i = P.acc + 1;
        jitter(i, w, h, coc);
        gw_photo(2, (uint32_t)i, (uint32_t)P.acc_n, S.gen);
        return 1;
    }
    if (S.mode == (int)GW_CAP_SHUTTER){
        int got = (int)gw_photo_got(S.gen);
        P.acc_n = S.samples;
        if (got >= S.samples){
            P.acc = got;
            gw_photo(3, (uint32_t)got, (uint32_t)S.samples, S.gen);
            capture_end("taken over the shutter's time");
            /* the still picture (the world as the shutter left it) comes after: the page has kept this one by then */
            return 1;
        }
        if (S.i == 0) S.i = 1;                       /* the world as it was */
        else if (S.stepped_now) S.i++;               /* it went on a step: the next frame */
        else if (got >= S.i){ S.advance = 1; MR32(G_FREEZE) = 0u; }   /* the page has it: on a step, the next frame */
        jitter(S.i, w, h, coc);
        gw_photo(2, (uint32_t)S.i, (uint32_t)S.samples, S.gen);     /* (a frame again: the page adds it once) */
        return 1;
    }
    return 0;
}

/* The picture, one frame: what it shows (the smoothed eye and the point, the zoom, the tilt, the lens, the cars, the
 * names); the same as the last frame's, the next of its still frames (this frame's shift), else the frame as it is.
 * The next is the one after those the page has (gw_photo_got: it may not encode every frame), the picture's own
 * number (gen) telling its frames from an earlier picture's. */
static void picture(void){
    unsigned w = 0, h = 0;
    gw_size(&w, &h);
    float s[13] = { P.se[0], P.se[1], P.se[2], P.sl[0], P.sl[1], P.sl[2], P.fov, P.roll,
                    P.blur > 0.f ? P.focus : 0.f, P.blur, (float)(P.show | P.names << 6 | P.game_lod << 8 | P.speed_blur << 9 | P.drive_on << 10 |
                                                                      !P.lights << 11 | (P.weather + 1) << 12),
                    (float)w, (float)h };
    int same = P.acc_n > 0;
    for (int i = 0; i < 13 && same; i++) same = fabsf(s[i] - P.sig[i]) <= (i < 6 ? 1e-4f : 1e-3f);
    memcpy(P.sig, s, sizeof s);
    memset(P.jit, 0, sizeof P.jit);
    /* the lens: what is far away blurs over a disc this wide (a radius, as a part of the view's half width), and the
     * frames as many as keep its points about BLUR_PX_AREA pixels apart (SAMPLES_SHARP at least, SAMPLES_BLUR most) */
    float coc = BLUR_FAR * P.blur * P.blur, r_px = coc * (float)w * 0.5f;
    /* a picture asked for (Take photo): taken in tiles or over the shutter's time, or the still picture as it is */
    uint32_t want = gw_photo_want();
    if (!S.mode && (((want & 0xffffu) && want != S.page_want) || S.take)){      /* (one at a time: the next waits) */
        float cc[3] = { 0, 0, 0 };
        uint32_t car = P.target ? P.target : car_of(player());          /* (the car the camera is on) */
        if (car) centre(car, cc);
        if (S.take){ want = (0x8000u | (S.want + 1u)) | (uint32_t)TILES_MAX << 16; S.take = 0; }
        else S.page_want = want;
        S.want = want & 0xffffu;
        capture_start(want, cc, w, h);
    }
    int was = S.mode, taking = capture(w, h, coc);
    gw_photo_more((S.mode == (int)GW_CAP_TILES ? (uint32_t)(S.tile & 0xff) | (uint32_t)S.n << 8 : 1u << 8) |
                  (uint32_t)S.mode << 12 | S.want << 16, P.fov * (S.lens ? FISHEYE_WIDER : 1.f), (uint32_t)S.lens);
    if (taking){
        /* the shutter's picture taken: it stays the still picture until the camera or what it shows changes */
        if (was == (int)GW_CAP_SHUTTER && !S.mode){ memcpy(P.sig, s, sizeof s); P.gen = S.gen; }
        return;
    }
    if (was == (int)GW_CAP_TILES) P.acc_n = 0;     /* the tiles taken: the still picture of the whole view again */
    if (P.drive_on){ P.acc = P.acc_n = 0; gw_photo(4, 1, 1, P.gen); return; }   /* GW_PHOTO_NOW */
    if (!same || !w || !h){
        P.acc = 0; P.gen = P.gen % 0xfffeu + 1u;
        P.acc_n = (int)clampf(ceilf(PI * r_px * r_px / BLUR_PX_AREA), SAMPLES_SHARP, SAMPLES_BLUR);
        gw_photo(1, 0, 0, P.gen);
        return;
    }
    P.acc = (int)gw_photo_got(P.gen);
    if (P.acc >= P.acc_n){
        if (P.said_gen != P.gen){
            P.said_gen = P.gen;
            fprintf(stderr, "[photo] f=%ld picture %u: %d frames%s; car renders %d, %d of them with the full model for photo mode%s\n",
                    autotest_frame(), P.gen, P.acc, coc > 0.f ? " (the lens's blur)" : "", cars_drawn1, cars_full1,
                    P.game_lod ? " (off: the game's models)" : "");
        }
        gw_photo(3, (uint32_t)P.acc, (uint32_t)P.acc_n, P.gen);
        return;
    }
    int i = P.acc + 1;
    /* within the pixel: a box over it (Halton 2, 3), as parts of the view's half width and height; the eye over the
     * lens's disc, evenly (Halton 5, 7: any first frames spread over all of it, so the picture as it comes in is
     * already blurred as it will be): where far away lands, in the view's half widths (overrides.c makes the eye's
     * move of it: times the focus distance over the projection's x scale) */
    jitter(i, w, h, coc);
    gw_photo(2, (uint32_t)i, (uint32_t)P.acc_n, P.gen);
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
        { "turkish", "Foto Modu" }, { "hungarian", "Fot\xf3 m\xf3" "d" }, { "czech", "Fotorezim" },
        { "slovak", "Foto re\x9eim" },
    };
    const char *l = language_name(), *s = "Photo Mode";
    if (l == P.lang) return;
    P.lang = l;
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(l, T[i].lang)) s = T[i].text;
    P.label = language_add_text("PHOTO_MODE_N2", s);
}

/* the status, when it changes */
static void publish(int can){
    static const char *const k[NSLOTS] = { "photo", "photo_fov", "photo_roll", "photo_n", "photo_focus", "photo_path", "photo_target" };
    if (!P.slots){ P.slots = 1; for (int i = 0; i < NSLOTS; i++){ P.slot[i] = host_status_slot(k[i]); P.val[i] = -1; } }
    double v[NSLOTS] = { (double)((can && !P.on ? 1 : 0) | (P.on ? 2 : 0) | (P.on && P.free ? 4 : 0) | (P.names ? 8 : 0) |
                                  (P.on && roam_game_names_here() ? 16 : 0) | (P.focus_auto ? 32 : 0) |
                                  (P.drive_on ? 0 : 256) | (P.on && roam_game_online_here() ? 512 : 0)),   /* (256: time stands) */
                         floor(P.fov * 10.f + 0.5f) / 10.0, floor(P.roll * 10.f + 0.5f) / 10.0, (double)P.n,
                         floor(P.focus * 10.f + 0.5f) / 10.0, (double)(P.path_n | (P.path_play ? 16 : 0)),
                         (double)(P.on && P.target ? P.target_kind : KIND_MINE) };
    if (!memcmp(v, P.val, sizeof v)) return;
    memcpy(P.val, v, sizeof v);
    host_status_set_n(P.slot, v, NSLOTS);
}

static void act(CPU *c);
static void add_item(CPU *c);
void photo_frame(CPU *c){
    cars_drawn1 = cars_drawn; cars_full1 = cars_full; cars_drawn = cars_full = 0;
    double t = now_s();
    float dt = P.t > 0 ? (float)(t - P.t) : 0.f;
    P.t = t;
    if (dt < 0.f) dt = 0.f; else if (dt > 0.1f) dt = 0.1f;
    if (!P.patched){ P.patched = 1; dispatch_patch(FN_REPLAY_ACT, act); dispatch_patch(FN_ADD_ITEM, add_item); }
    if (!P.shown_set){ P.shown_set = 1; P.show = SHOW_ALL; P.lights = 1; P.horns = 1; P.weather = -1; }
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
    int fz = atomic_exchange(&I.freeze, -1);
    if (fz >= 0 && (fz == 0) != P.drive_on){
        P.drive_on = !fz;
        if (P.on){
            roam_game_hold(!P.drive_on);
            MR32(G_FREEZE) = P.drive_on ? P.freeze_was : 1u;
            if (P.drive_on) volumes(c, G_VOLUMES); else hush(c);
        }
        fprintf(stderr, "[photo] f=%ld time %s\n", autotest_frame(), P.drive_on ? "runs on" : "stands");
    }
    int sp = atomic_exchange(&I.speed, NONE_SET), sm = atomic_exchange(&I.smooth, NONE_SET), lv = atomic_exchange(&I.level, -1);
    int sn = atomic_exchange(&I.spin, NONE_SET);
    if (sp != NONE_SET) P.speed = clampf((float)sp / 100.f, 0.05f, 10.f);
    if (sm != NONE_SET) P.smooth = clampf((float)sm / 1000.f, 0.f, 1.f);
    if (lv >= 0) P.level = lv != 0;
    if (sn != NONE_SET) P.spin = clampf((float)sn / 10.f, -SPIN_MAX, SPIN_MAX);
    int cs = atomic_exchange(&I.cars, -1), sh = atomic_exchange(&I.show, -1);   /* (the Cars list's: as kinds) */
    if (cs >= CARS_ALL && cs <= CARS_BUDDIES){
        static const int as[] = { SHOW_ALL, SHOW_MINE, 0, SHOW_MINE | SHOW_FRIENDS | SHOW_CREWS };
        sh = as[cs];
    }
    if (sh >= 0 && (sh & SHOW_ALL) != P.show){
        P.show = sh & SHOW_ALL;
        fprintf(stderr, "[photo] f=%ld cars shown:%s%s%s%s%s%s%s\n", autotest_frame(), P.show ? "" : " none",
                P.show & SHOW_MINE ? " the player's" : "", P.show & SHOW_FRIENDS ? " friends'" : "", P.show & SHOW_CREWS ? " crew-mates'" : "",
                P.show & SHOW_PLAYERS ? " players'" : "", P.show & SHOW_TRAFFIC ? " traffic" : "", P.show & SHOW_RACERS ? " racers" : "");
    }
    int li = atomic_exchange(&I.lights, -1), ho = atomic_exchange(&I.horns, -1), we = atomic_exchange(&I.weather, NONE_SET);
    if (li >= 0 && (li != 0) != P.lights){
        P.lights = li != 0;
        if (P.lights) lights_restore();
        fprintf(stderr, "[photo] f=%ld other cars' headlights %s\n", autotest_frame(), P.lights ? "on" : "off");
    }
    if (ho >= 0 && (ho != 0) != P.horns){
        P.horns = ho != 0;
        if (P.on) horn_mute(!P.horns);
        fprintf(stderr, "[photo] f=%ld horns %s\n", autotest_frame(), P.horns ? "heard" : "quiet");
    }
    if (we != NONE_SET && (we < 0 ? -1 : we > 0) != P.weather){
        P.weather = we < 0 ? -1 : we > 0;
        fprintf(stderr, "[photo] f=%ld weather: %s\n", autotest_frame(), P.weather < 0 ? "as it is" : P.weather ? "rain" : "dry");
    }
    pic_input();
    S.stepped_now = S.stepped; S.stepped = 0;
    if (S.mode && (!P.on || P.drive_on)) capture_end("not taken (photo mode or Freeze time changed)");
    if (S.mode == (int)GW_CAP_SHUTTER) MR32(G_FREEZE) = 1u;    /* a shutter's frame: the world waits (capture() goes on) */
    uint32_t pl = player(), mine = car_of(pl);
    int tg = atomic_exchange(&I.target, -1);
    if (P.on){
        if (tg == TARGET_MINE) set_target(0, mine);
        else if (tg == TARGET_MIDDLE){
            uint32_t m = middle_car(mine);
            if (m) set_target(m, mine); else fprintf(stderr, "[photo] f=%ld no car in the middle of the picture\n", autotest_frame());
        }
        if (P.target && (!target_there(mine) || !kind_shown(P.target_kind))){
            fprintf(stderr, "[photo] f=%ld the camera's car is gone: the player's again\n", autotest_frame());
            set_target(0, mine);
        }
        if (!P.lights) lights_off(mine);
        MR32(G_HEADLIGHTS) = P.show & SHOW_MINE ? P.hl_was : 0u;
        steer(P.target ? P.target : mine, mine, dt); place(pl); picture();
    }
    pic_publish();
    publish(!why && atomic_load(&I.page));
}

/* overrides.c sub_005c7ec0: while photo mode is on, the view's field of view (a binary angle) and tilt (radians), and
 * this frame's shift (picture): j[0], j[1] within the pixel, as parts of the view's half width and height; j[2], j[3]
 * the eye along the view's x and y, m; j[4] the distance that stays put (0: none); j[5], j[6] the lens shift (where
 * the picture's middle goes: right, up, parts of its half width and height), j[7] how much wider the game draws for
 * it (the projection made that much bigger again, so what the shift brings in is drawn); j[8] tiles a side, j[9],
 * j[10] the tile drawn (from the left, from the top) */
int photo_view(uint16_t *fov, float *roll, float *j){
    if (!P.on) return 0;
    float f = P.fov * (S.lens ? FISHEYE_WIDER : 1.f), sx = S.lens ? 0.f : S.shift[0], sy = S.lens ? 0.f : S.shift[1];
    float k = 1.f + fmaxf(fabsf(sx), fabsf(sy));
    if (k > 1.f) f = 2.f * atanf(tanf(f * 0.5f * DEG) * k) / DEG;
    *fov = (uint16_t)lrintf(f * 65536.f / 360.f);
    *roll = P.roll * DEG;
    memcpy(j, P.jit, sizeof P.jit);
    int n = S.mode == (int)GW_CAP_TILES && S.tile >= 0 ? S.n : 1;
    j[5] = sx; j[6] = sy; j[7] = k; j[8] = (float)n;
    j[9] = (float)(n > 1 ? S.tile % n : 0); j[10] = (float)(n > 1 ? S.tile / n : 0);
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
    if (S.mode == (int)GW_CAP_TILES){ POP32(c->eip); return; }       /* tiles: the names would be where the whole view has them */
    uint32_t keep[NAMES_MAX], fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0;
    int nk = P.names && (P.show & SHOW_PEOPLE) == SHOW_PEOPLE && ok(mg, 0xf7) ? roam_game_names(c, keep, NAMES_MAX) : 0;   /* (players' cars hidden: no names) */
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
 * icon and label (and savegame.c's Save after Return to Explore Mode). */
void sub_0050dab0(CPU *c);
void savegame_item(CPU *c, uint32_t menu, uint32_t it);
int savegame_accept(CPU *c);
static void add_item(CPU *c){
    uint32_t menu = c->ecx, it = MR32(c->esp + 4u);
    sub_0050dab0(c);
    savegame_item(c, menu, it);
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
 * the game as the menu's Resume does; Save's is savegame.c's; Replay's as the game has it. */
void sub_004c26b0(CPU *c);
void sub_004c2510(CPU *c);
void sub_004c24c0(CPU *c);
static void act(CPU *c){
    if (savegame_accept(c)) return;
    uint32_t it = c->ecx, msg = MR32(c->esp + 8u);
    if (msg != MSG_ACCEPT || !ok(it, IT_LABEL + 4u) || MR32(it + IT_LABEL) != P.label || MR8(it + IT_GREY)){ sub_004c26b0(c); return; }
    P.pause_at = autotest_frame();
    fprintf(stderr, "[photo] f=%ld the pause menu's Photo Mode\n", autotest_frame());
    if (MR32(G_RACEKIND) == 0u) sub_004c2510(c); else sub_004c24c0(c);
}

/* ---- the cars in the picture ---------------------------------------------------------------------------------
 * A car's size on screen (0x48ACB0, thiscall: position, radius; ret 8: [view+0xC] * radius / (distance - radius),
 * 0 behind the camera) picks which of its four models the car's render (0x620AE0) draws: the table at 0x7A66DC
 * {40, 30, 20, 0}, the full model from 40 (about 40 m at the game's field of view), the roughest under 20.  While
 * photo mode is on the render's own call gets at least SIZE_FULL, so every car keeps its full model however far or
 * wide the camera is (the flares' (0x615630) and 0x61D150's calls keep the game's size).  The least model the render
 * may draw comes from the Car detail option ([0x8A1C10] and [0x8A1C14], set by 0x5D2850: 1 with Low, which never
 * draws the full model): 0 while photo mode is on.
 * The cars photo mode hides are not drawn at all: neither render (0x620AE0, whose override is showoff.c's and asks
 * photo_hides first, as its 0x615A10's does, the car's lights and neon; and 0x6162E0, which the car drawing
 * 0x6316D0 calls instead; both thiscall on the car's render info, car +0x554; ret 0x30) nor its flares (0x615630,
 * ret 0x18). */
#define RET_CAR_SIZE   0x00620c7eu   /* the car render's call of 0x48ACB0 returns here */
#define SIZE_FULL      1000
#define CAR_RENDER     0x554u
int photo_hides(uint32_t ri){
    if (!P.on || P.show == SHOW_ALL) return 0;
    if (!P.show) return 1;
    uint32_t mine = car_of(player()), car = ok(ri, 3) ? MR32(ri) : 0;   /* (a render info's car at its +0) */
    if (mine && ri == rd32(mine + CAR_RENDER)) return !(P.show & SHOW_MINE);
    return !ok(car, CAR_BLINKOUT) ? 0 : !kind_shown(car_kind(car, mine));
}
#define SIZE_MODEL_A   40            /* the table's first: the full model from this size */
void sub_0048acb0_orig(CPU *c);
void sub_0048acb0(CPU *c){
    uint32_t ret = MR32(c->esp);
    sub_0048acb0_orig(c);
    if (!P.on || ret != RET_CAR_SIZE || (int32_t)c->eax <= 0) return;
    cars_drawn++;
    if ((int32_t)c->eax < SIZE_MODEL_A) cars_full++;
    if (!P.game_lod && (int32_t)c->eax < SIZE_FULL) c->eax = SIZE_FULL;
}
void sub_006162e0_orig(CPU *c);
void sub_006162e0(CPU *c){
    if (photo_hides(c->ecx)){ POP32(c->eip); c->esp += 0x30u; return; }
    sub_006162e0_orig(c);
}
void sub_00615630_orig(CPU *c);
void lightkey_flares(uint32_t ri);               /* lightkey.c: whose flares these are (high beam) */
void sub_00615630(CPU *c){
    if (photo_hides(c->ecx)){ POP32(c->eip); c->esp += 0x18u; return; }
    lightkey_flares(c->ecx);
    sub_00615630_orig(c);
    lightkey_flares(0);
}

/* the test script's "photo ..." (autotest.c): hello, on, off, orbit, free, other (Tab), reset, names [0|1], hold MASK,
 * move X Y [UP [CLOSER]], turn X Y (-1..1), drag DX DY, pan DX DY (view heights), dolly N (wheel notches), fov DEG,
 * roll DEG, focus M (0: the car), blur 0..1, cars 0|1|2|3 (all, only the player's, none, friends and crews), freeze 0|1 (time runs on, stands),
 * speed X (times the keys' and sticks'), smooth 0..1, level 0|1, spin DEG/S, path add|play [S]|stop|clear,
 * show MASK (SHOW_*), lights 0|1, horns 0|1, weather -1|0|1, target 0|1 (the player's car, the car in the middle),
 * size N (tiles a side), shutter MS, lens 0|1 (plain, fisheye), shift X Y (lens shift, -0.6 .. 0.6), take (a picture
 * asked for as Take photo asks: in tiles, over the shutter's time; the page puts tiles together as take_<n>.png) */
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
    else if (!strcasecmp(w, "focus")) photo_input(PH_FOCUS, a > 0 ? (int)lrintf(a * 100.f) : -1, 0);
    else if (!strcasecmp(w, "blur")) photo_input(PH_BLUR, (int)lrintf(clampf(a, 0, 1) * 1000.f), 0);
    else if (!strcasecmp(w, "cars")) photo_input(PH_CARS, (int)a, 0);
    else if (!strcasecmp(w, "freeze")) photo_input(PH_FREEZE, n > 1 ? (int)a : 1, 0);
    else if (!strcasecmp(w, "speed")) photo_input(PH_SPEED, (int)lrintf(a * 100.f), 0);
    else if (!strcasecmp(w, "smooth")) photo_input(PH_SMOOTH, (int)lrintf(clampf(a, 0, 1) * 1000.f), 0);
    else if (!strcasecmp(w, "level")) photo_input(PH_LEVEL, n > 1 ? (int)a : 1, 0);
    else if (!strcasecmp(w, "spin")) photo_input(PH_SPIN, (int)lrintf(a * 10.f), 0);
    else if (!strcasecmp(w, "path")){
        char v[16] = {0};
        if (sscanf(s, "%*s %15s %f", v, &a) < 1) return 0;
        int pa = !strcasecmp(v, "add") ? PATH_ADD : !strcasecmp(v, "play") ? PATH_PLAY : !strcasecmp(v, "stop") ? PATH_STOP :
                 !strcasecmp(v, "clear") ? PATH_CLEAR : -1;
        if (pa < 0) return 0;
        photo_input(PH_PATH, pa, pa == PATH_PLAY ? (int)lrintf(a * 1000.f) : 0);
    }
    else if (!strcasecmp(w, "show")) photo_input(PH_SHOW, (int)a, 0);
    else if (!strcasecmp(w, "lights")) photo_input(PH_LIGHTS, n > 1 ? (int)a : 1, 0);
    else if (!strcasecmp(w, "horns")) photo_input(PH_HORNS, n > 1 ? (int)a : 1, 0);
    else if (!strcasecmp(w, "weather")) photo_input(PH_WEATHER, (int)a, 0);
    else if (!strcasecmp(w, "target")) photo_input(PH_TARGET, (int)a, 0);
    else if (!strcasecmp(w, "size")) photo_input(PH_SIZE, (int)a, 0);
    else if (!strcasecmp(w, "take")) S.take = 1;
    else if (!strcasecmp(w, "shutter")) photo_input(PH_SHUTTER, (int)a, 0);
    else if (!strcasecmp(w, "lens")) photo_input(PH_LENS, (int)a, 0);
    else if (!strcasecmp(w, "shift")) photo_input(PH_SHIFT, (int)lrintf(a * 1000.f), (int)lrintf(b * 1000.f));
    else if (!strcasecmp(w, "fulldetail")) P.game_lod = n > 1 && a == 0;     /* tests: 0 the game's models (A/B) */
    else if (!strcasecmp(w, "speedblur")){                                    /* tests: 1 the game's motion blur on (A/B) */
        P.speed_blur = n > 1 && a != 0;
        if (P.on) MR32(G_MOTION_BLUR) = P.speed_blur ? P.mb_was : 0u;
    }
    else return 0;
#undef PACK
    return 1;
}
