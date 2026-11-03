/* look.c -- the controller's right stick looks around while driving: the
 * view turns to where the stick points (right looks right, a stick half way
 * out turns half as far) and swings back behind the car once the stick is
 * let go.  Straight back is the game's own Look Back, as the stick's button
 * (NFS Heat's way): the view snaps to the rear and back again.  The PC game
 * has nothing of the kind: only Look Back and the camera's change (the
 * consoles' right stick turned the front end's car viewer only:
 * dinput_pad.c sub_004a5d90).  Everywhere the player drives with the drive
 * camera: free roam, the career's and Quick Race's races, races between
 * players (Online, LAN, free roam's).  Not in photo mode, the replay, a
 * cutscene or the crash and jump slow motion (other camera movers: photo
 * mode's is the developers' debug camera), nor with a screen over the HUD or
 * the game paused: then the view is straight behind again at once.  Where
 * quick chat opens (free roam with others, races between players) the
 * stick's click opens it, and the page keeps the stick from the game while
 * its picker is open (web/src/app/quickchat.ts): the view swings back then.
 * A wheel's and an RC radio's right stick is always at rest
 * (web/src/input/wheel.ts).
 *
 * How: the drive camera puts the camera where it wants it each frame, in the
 * main loop's update (0x5811C0, before the sound's 0x483B40); the frame is
 * drawn after (0x5CE8A0 .. Present in 0x5D2850).  The camera itself is
 * turned for that: from the main view's setup (overrides.c sub_005c7ec0,
 * view 1, the first of the frame) until after Present (career.c's main-loop
 * hook, the empty call returning to LOOK_END_RET), when its bytes go back as
 * the game made them.  Everything drawn reads the turned one: the views and
 * their culling planes, and the car renderer (0x620AE0), which takes the
 * eye from the view's camera (+0x40) for which of a car's parts it draws
 * (from behind it leaves out the front wheels, looking back the rear ones)
 * and for the car's reflections.  Turning only the view's matrix, as the
 * look did first, drew the cars as seen from behind the car: the wheels on
 * the side looked at missing and the reflections wrong (players' reports,
 * 2026-11-03).  The sound and the drive camera's next frame keep the game's
 * camera; free roam's names and lines over the cars, placed in the HUD's
 * update before the frame is drawn, ask look_eye for the view's eye and
 * direction as drawn (roam_game.c view_eye).  The road's reflection (view
 * 4) turns its own camera the same way, if it has one.
 * The turn: the world about a vertical line (M' = A M, A the turn about the
 * line), which is the camera going round the line the other way, its eye,
 * forward and the point it looks at with it.  The line goes through the
 * car's centre (car +0x60), so a chase camera goes round the car and looks
 * past it; with the eye in or on the car (the bumper and hood cameras:
 * within IN_CAR_M of its centre) it goes through the eye, a turn of the
 * head.  The game's world has z up.  Each frame the turn moves towards the
 * stick's (TAU_S), back at rest the short way round; a stick pointing back
 * (past BACK_RAD either side) keeps to the side it came from.
 * Straight back (within SNAP_IN of it, past SNAP_R out) the stick holds the
 * pad's button bound to Look Back (dinput_pad.c fill_state, look_back_held)
 * and the camera is the game's; it lets go outside SNAP_OUT or below
 * SNAP_R_OFF.  From there to the side the turn goes on from straight back;
 * let go, the view is behind the car at once, as after the button.  With no
 * button bound to Look Back the stick turns the view round to the rear.
 *
 * NFSU2_PAD_LOG=1 logs each look's start and end
 * (tests/scripts/pad_look.txt; roam_look_a.txt in free roam). */
#include "cpu.h"
#include "gamepad.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define G_FLOW       0x008654a4u   /* 6: in the world */
#define G_RACE       0x00890118u   /* the race status: +0 4 racing (free roam too), 6 paused */
#define G_PLAYER1    0x008900acu   /* Player*: +4 its car, +0x9A4 its HUD, +0x238 its camera mover */
#define G_FENG       0x008384c4u   /* +8 the package manager: +0xE4 the top package, +0xC its name */
#define PL_CAR       4u
#define PL_HUD       0x9a4u
#define PL_MOVER     0x238u
#define MV_TYPE      0xcu
#define MOVER_DRIVE  1u
#define CAR_POS      0x60u         /* the car's centre, world */
#define CAM_POS      0x40u         /* the camera's eye, world */
#define CAM_FWD      0x50u         /*   its forward */
#define CAM_TARGET   0x60u         /*   the point it looks at */
#define CAM_TURNED   0x70u         /* its bytes the turn changes: the matrix, those three */
#define NCAM         2             /* the cameras turned in a frame: view 1's, view 4's if its own */

#define STICK_ON     0.25f         /* the stick starts a look past this far out (after the dead zone) */
#define STICK_FULL   0.85f         /* and turns the whole way from this far */
#define TAU_S        0.09f         /* the turn's time constant towards the stick's */
#define BACK_RAD     2.4f          /* a stick pointing back past this (rad, ~137 degrees) keeps the turn's side */
#define SNAP_IN      0.44f         /* within this of straight back (rad, ~25 degrees) the stick holds Look Back, */
#define SNAP_OUT     0.61f         /*   and lets go outside this (~35 degrees), */
#define SNAP_R       0.5f          /*   from this far out, */
#define SNAP_R_OFF   0.4f          /*   down to this */
#define IN_CAR_M     3.f           /* the eye this near the car's centre (m, flat): the bumper and hood cameras */
#define REST_RAD     0.0005f       /* a turn this small is none */
#define PI_F         3.14159265f

long autotest_frame(void);         /* autotest.c: the game's Present count */

static struct {
    float yaw;                     /* the turn now, rad (+ looks right) */
    float px, py;                  /* the line it turns about, this frame */
    int on;                        /* turned this frame (views 1 and 4) */
    int looking;                   /* the stick out (for the log) */
    int back;                      /* the stick straight back: it holds Look Back */
    double t;                      /* the last frame's clock */
    int n;                         /* the cameras turned now (until look_frame_end), in Present count frame */
    long frame;
    uint32_t cam[NCAM];
    uint8_t keep[NCAM][CAM_TURNED];    /* their bytes as the game made them */
    uint8_t made[NCAM][CAM_TURNED];    /* and as turned */
} L;

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void cstr(uint32_t a, char *o, int n){
    int i = 0;
    for (; a && i + 1 < n && ok(a + (uint32_t)i, 0) && MR8(a + (uint32_t)i); i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float wrap(float a){
    while (a > PI_F) a -= 2.f * PI_F;
    while (a < -PI_F) a += 2.f * PI_F;
    return a;
}
static int pad_log(void){
    static int l = -1;
    if (l < 0) l = gamepad_env_flag("NFSU2_PAD_LOG", 0);
    return l;
}

/* the player's car while it drives with the drive camera and the HUD on top (0: no look now) */
static uint32_t driving_car(void){
    uint32_t pl = rd32(G_PLAYER1), r = rd32(G_RACE);
    if (MR32(G_FLOW) != 6u || !ok(pl, PL_MOVER + 4u) || !ok(r, 3) || MR32(r) != 4u) return 0;
    uint32_t car = rd32(pl + PL_CAR), mv = rd32(pl + PL_MOVER);
    if (!ok(car, CAR_POS + 12u) || !ok(mv, MV_TYPE + 4u) || MR32(mv + MV_TYPE) != MOVER_DRIVE) return 0;
    uint32_t hud = rd32(pl + PL_HUD), fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0, top = mg ? rd32(mg + 0xe4u) : 0;
    char a[40], b[40];
    cstr(top ? rd32(top + 0xcu) : 0, a, sizeof a); cstr(hud ? rd32(hud + 0x10u) : 0, b, sizeof b);
    return a[0] && !strcasecmp(a, b) ? car : 0;
}

/* how far the stick at x, y points from straight back (rad, 0 .. pi) */
static float from_back(float x, float y){ return PI_F - fabsf(atan2f(x, -y)); }

/* the turn the stick asks for now (rad, + right; 0 at rest), its back side kept as the turn now has it; *back: it
 * points straight back, where it holds Look Back (where a button has it) instead of turning the view */
static float stick_yaw(int can, int *back){
    PadState s;
    *back = 0;
    if (!can || !gamepad_read(&s) || s.wheel) return 0.f;
    float x = s.axis[PAD_AXIS_RX], y = s.axis[PAD_AXIS_RY], r = hypotf(x, y);
    int pad_look_back_bound(void);                   /* dinput_pad.c: a button of the pad's is Look Back */
    if (pad_look_back_bound()){
        float off = from_back(x, y);
        if (L.back ? off < SNAP_OUT && r > SNAP_R_OFF : off < SNAP_IN && r > SNAP_R){ *back = 1; return 0.f; }
        if (off < (L.back ? SNAP_OUT : SNAP_IN)) return 0.f;   /* on its way out there or back in: no turn */
    }
    if (r < STICK_ON) return 0.f;
    float k = fminf(1.f, (r - STICK_ON) / (STICK_FULL - STICK_ON));
    float dir = atan2f(x, -y);                       /* up 0, right +pi/2, down pi, left -pi/2 */
    int side = fabsf(L.yaw) > 0.05f ? (L.yaw > 0.f ? 1 : -1) : (x >= 0.f ? 1 : -1);
    if (fabsf(dir) > BACK_RAD && (dir > 0.f ? 1 : -1) != side) dir += side * 2.f * PI_F;
    return dir * k;
}

/* dinput_pad.c fill_state, as the game reads the pad: whether the stick (in s) holds the button bound to Look Back */
int look_back_held(const PadState *s){
    if (!L.back || !s || s->wheel) return 0;
    float x = s->axis[PAD_AXIS_RX], y = s->axis[PAD_AXIS_RY];
    return hypotf(x, y) > SNAP_R_OFF && from_back(x, y) < SNAP_OUT && driving_car() != 0;
}

/* p (a point, or a direction with pt 0) turned as the view is: clockwise seen from above about the line */
static void turn_xy(float *p, int pt){
    float c = cosf(L.yaw), s = sinf(L.yaw), ox = pt ? L.px : 0.f, oy = pt ? L.py : 0.f, x = p[0] - ox, y = p[1] - oy;
    p[0] = ox + x * c + y * s; p[1] = oy - x * s + y * c;
}

/* the camera's matrix at cam (rows; the translation in the last; points as rows times it) turned: the world turned
 * about the line first, anticlockwise seen from above by the look's angle, so the camera goes round it clockwise and
 * looks right (the game's world: x, y, z up as a right hand has them) */
static void turn_matrix(uint32_t cam){
    float c = cosf(L.yaw), s = -sinf(L.yaw), *M = (float *)(g_mem + cam);
    /* A: rows (c, -s, 0), (s, c, 0), (0, 0, 1) (the world by -s's angle, anticlockwise), translation p - p R */
    float tx = L.px - (L.px * c + L.py * s), ty = L.py - (-L.px * s + L.py * c);
    float r0[4], r1[4];
    for (int k = 0; k < 4; k++){
        r0[k] = c * M[k] - s * M[4 + k];
        r1[k] = s * M[k] + c * M[4 + k];
    }
    for (int k = 0; k < 4; k++){
        M[12 + k] += tx * M[k] + ty * M[4 + k];
        M[k] = r0[k]; M[4 + k] = r1[k];
    }
}

/* the camera at cam turned for the rest of the frame (once), its bytes kept for look_frame_end */
static void turn_camera(uint32_t cam){
    if (!ok(cam, CAM_TURNED) || L.n >= NCAM) return;
    for (int i = 0; i < L.n; i++) if (L.cam[i] == cam) return;
    memcpy(L.keep[L.n], g_mem + cam, CAM_TURNED);
    turn_matrix(cam);
    turn_xy((float *)(g_mem + cam + CAM_POS), 1);
    turn_xy((float *)(g_mem + cam + CAM_FWD), 0);
    turn_xy((float *)(g_mem + cam + CAM_TARGET), 1);
    memcpy(L.made[L.n], g_mem + cam, CAM_TURNED);
    L.cam[L.n++] = cam;
    L.frame = autotest_frame();
}

/* career.c's main-loop hook after Present: the cameras back as the game made them (unless something wrote them since) */
void look_frame_end(void){
    for (int i = 0; i < L.n; i++){
        uint32_t cam = L.cam[i];
        if (!ok(cam, CAM_TURNED)) continue;
        if (!memcmp(g_mem + cam, L.made[i], CAM_TURNED)) memcpy(g_mem + cam, L.keep[i], CAM_TURNED);
        else if (pad_log()) fprintf(stderr, "[pad] the look's camera 0x%08x was written while turned: left as it is\n", cam);
    }
    L.n = 0;
}

/* overrides.c sub_005c7ec0, before the view is made, for views 1 (the main view: once a frame, first) and 4 (its road
 * reflection) and their cameras: the camera turned now for the frame, as the stick asks */
void look_view(uint32_t id, uint32_t cam){
    if (id != 1u){
        static int said;
        if (pad_log() && L.on && !said){
            said = 1;
            fprintf(stderr, "[pad] look: view %u's camera 0x%08x, %s\n", id, cam, L.n && L.cam[0] == cam ? "view 1's" : "its own");
        }
        if (L.on) turn_camera(cam);                  /* a camera of its own (view 1's is turned already) */
        return;
    }
    for (int i = 0; i < L.n; i++) if (L.cam[i] == cam) return;   /* made again this frame: turned already */
    if (L.n && L.frame != autotest_frame()) look_frame_end();    /* a frame that ended without the hook */
    double t = now_s();
    float dt = L.t > 0. ? (float)(t - L.t) : 0.f;
    L.t = t;
    if (dt > 0.25f) dt = 0.25f;
    uint32_t car = driving_car();
    int back = 0;
    float want = stick_yaw(car != 0, &back);
    if (pad_log() && back != L.back)
        fprintf(stderr, back ? "[pad] the right stick holds Look Back\n" : "[pad] the right stick lets go of Look Back\n");
    if (pad_log() && !back && !L.back && (want != 0.f) != L.looking){
        if (want != 0.f) fprintf(stderr, "[pad] the right stick looks round the car\n");
        else fprintf(stderr, "[pad] the right stick let go (the view turned %.0f degrees)%s\n",
                     (double)(L.yaw * 57.29578f), car ? "" : ": no look now");
    }
    L.looking = want != 0.f;
    if (back){                                       /* the game's own view to the rear: the turn waits there */
        if (!L.back) L.yaw = L.yaw < -0.05f ? -PI_F : PI_F;
        L.back = 1;
        L.on = 0;
        return;
    }
    if (L.back){
        L.back = 0;
        if (want == 0.f) L.yaw = 0.f;                /* let go from straight back: behind the car at once */
    }
    float d = want - L.yaw;
    if (d > PI_F) d -= 2.f * PI_F; else if (d < -PI_F) d += 2.f * PI_F;
    L.yaw = wrap(L.yaw + d * (1.f - expf(-dt / TAU_S)));
    if (!car || (want == 0.f && fabsf(L.yaw) < REST_RAD)){
        if (!car) L.yaw = 0.f;                       /* no look now: straight behind at once (a screen, a replay) */
        else if (fabsf(L.yaw) < REST_RAD) L.yaw = 0.f;
        L.on = 0;
        return;
    }
    float cx = MRF32(car + CAR_POS), cy = MRF32(car + CAR_POS + 4u);
    if (ok(cam, CAM_POS + 12u)){
        float ex = MRF32(cam + CAM_POS), ey = MRF32(cam + CAM_POS + 4u);
        if (hypotf(ex - cx, ey - cy) < IN_CAR_M){ cx = ex; cy = ey; }
    }
    L.px = cx; L.py = cy;
    L.on = 1;
    turn_camera(cam);
    static int said;
    if (pad_log() && !said && L.n){
        const float *k = (const float *)L.keep[0];
        said = 1;
        fprintf(stderr, "[pad] look: camera 0x%08x eye (%.2f %.2f %.2f) forward (%.3f %.3f %.3f) +0x60 (%.2f %.2f %.2f)"
                " car (%.2f %.2f %.2f)\n", cam, (double)k[16], (double)k[17], (double)k[18], (double)k[20],
                (double)k[21], (double)k[22], (double)k[24], (double)k[25], (double)k[26], (double)MRF32(car + CAR_POS),
                (double)MRF32(car + CAR_POS + 4u), (double)MRF32(car + CAR_POS + 8u));
    }
}

/* the main view's eye and forward e, f (the camera's, as read while it is not turned: the HUD's update) turned as the
 * frame is drawn: what the view looks from and along (roam_game.c's names over the cars and lines over them: what is
 * in front) */
void look_eye(float *e, float *f){
    if (!L.on || L.n) return;                        /* turned now: the camera says so itself */
    turn_xy(e, 1);
    turn_xy(f, 0);
}
