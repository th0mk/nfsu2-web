/* roam.c -- free-roam multiplayer RESEARCH PROBES (v1.2 NA port).
 *
 * Test-only.  Nothing here acts unless an input-script line ("roam ...") or
 * an NFSU2_ROAM_* variable asks for it.  It is the probing aid behind the
 * free-roam multiplayer's game facts: list the world's cars, log who
 * creates them, record the player's car, and drive a second car (one of the
 * parked free-roam opponent cars) from host code, with any model and
 * customization.  It is not the multiplayer feature.
 *
 * Guest calls run in the game's main loop, at an instruction hook before the
 * second debug no-op call of GameLoop (0x581475), on the guest thread between
 * two frames.  The whole CPU state is saved and restored around each call.
 *
 * Script commands (NFSU2_INPUT_SCRIPT lines "<when> roam <sub> ..."):
 *   roam call FN [ecx=X] [edx=X] [ARG...]  call a guest function, log eax
 *   roam peek X [N] / memdump X LEN FILE    read guest memory
 *   roam snapshot FILE             every committed guest page and the CPU at
 *                                  the main-loop hook (tests/asi_harness.c)
 *   roam wait X VAL [tmo]          wait until dword X == VAL
 *   roam waitroam [tmo]            wait until career free roam is settled
 *   roam teleport X Y Z [ANGLE]    the game's own hot-position jump (0x600E10)
 *   roam shopgo NAME               jump onto a career shop's marker
 *   roam racego                    jump onto the first available career race's marker
 *   roam fe2roam [tmo]             press through the front end to free roam (saved career)
 *   roam mp ...                    the multiplayer feature's commands (roam_game.c)
 *   roam asi ...                   the virtual Windows friend's (roam_asipeer.c)
 *   roam cars / carlog N           log the world's cars (every N frames)
 *   roam types / loader            log the car type table / CarLoader entries
 *   roam watch X                   log the guest functions that write X
 *   roam smsauto on|off            answer in-game SMS pop-ups
 *   roam trace on FILE|off         per-frame player state and clocks (CSV;
 *                                  NFSU2_ROAM_TRACECTL=FILE adds the controls)
 *   roam rec on|off / rec save FILE  record the player's car at 20 Hz
 *   roam rewind                    jump the player back to the first sample
 *   roam ghost on K setpos|direct|replay [nocol]
 *                                  activate the K-th parked opponent car and
 *                                  drive it: on a circle (NFSU2_ROAM_CIRCLE=
 *                                  "ahead,left,radius") through Car::SetPosition
 *                                  or by writing the pose, or replaying the
 *                                  recording (NFSU2_ROAM_GHOSTCSV=FILE traces
 *                                  it); nocol: no collision body
 *   roam ghost ride player|copyK|TYPE  give it the player's ride (no loader
 *                                  reference: test only), a copy of racer
 *                                  record K's ride, or the stock ride of car
 *                                  type TYPE, streamed in first
 *   roam ghost off                 park it again
 *   roam spawn                     EXPERIMENT: CreateCar an extra AI-range car
 *                                  of our own next to the player
 * X/ARG/FN are expressions: numbers, "*X" (guest dword at X), "(X)", "+",
 * "-", and "*" between two operands, e.g. "*(*0x890080+0x1c)+0x60".
 * NFSU2_ROAM_PROBE=1 logs car creation, render-info rebuilds, CarLoader
 * purges and every driver/physics change of the ghost car.
 */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);
const char *autotest_top_fng(void);
void mprot_arm(uint32_t);
void autotest_tap(unsigned dik, long frames);

/* ---- guest globals (v1.2 NA) ---------------------------------------------- */
#define G_FLOW          0x008654a4u   /* TheGameFlowManager state (6 = in game) */
#define G_WORLD         0x00890080u   /* World*: +0/+4 car list (bTList), +0x10 count */
#define G_PLAYER1       0x008900acu   /* PlayersByNumber[0] (Player*), +4 = car */
#define G_RACE          0x00890118u   /* race coordinator: +0 state, +0x24 racers */
#define G_CAREER_RACE   0x00863470u   /* Career 0x85AD40 +0x8730: race being run (0 = roam) */
#define G_HOTPOS        0x00890140u
#define G_HOTPOS_ANGLE  0x008900d0u
#define G_HOTPOS_JUMP   0x008900cdu
#define G_HOTPOS_VALID  0x008900ceu
#define G_CARTYPES      0x008a1cccu   /* CarTypeInfo[], 0x890 bytes, +0 name */
#define HOOK_MAINLOOP   0x00581475u

/* ---- expressions ---------------------------------------------------------- */
static const char *xp;
static uint32_t x_expr(void);
static void x_ws(void){ while (*xp == ' ' || *xp == '\t') xp++; }
static uint32_t rd32(uint32_t a){ return mem_is_committed(a) && mem_is_committed(a + 3) ? MR32(a) : 0u; }
static uint32_t x_unary(void){
    x_ws();
    if (*xp == '*'){ xp++; return rd32(x_unary()); }
    if (*xp == '('){ xp++; uint32_t v = x_expr(); x_ws(); if (*xp == ')') xp++; return v; }
    char *end; uint32_t v = (uint32_t)strtoul(xp, &end, 0); xp = end; return v;
}
static uint32_t x_term(void){
    uint32_t v = x_unary();
    for (;;){ x_ws(); if (*xp == '*'){ xp++; v *= x_unary(); } else return v; }
}
static uint32_t x_expr(void){
    uint32_t v = x_term();
    for (;;){ x_ws();
        if (*xp == '+'){ xp++; v += x_term(); }
        else if (*xp == '-'){ xp++; v -= x_term(); }
        else return v; }
}
static uint32_t eval(const char *s){ xp = s; return x_expr(); }

/* ---- guest calls ---------------------------------------------------------- */
/* Call guest function fn (any convention: esp and every register are restored
 * afterwards).  Only from the main-loop hook. */
static uint32_t guest_call(CPU *c, uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 512u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx; c->edx = edx;
    PUSH32(HOOK_MAINLOOP);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

#define QMAX 32
static char g_q[QMAX][200]; static int g_qh, g_qt;
static void queue(const char *line){
    if ((g_qt + 1) % QMAX == g_qh){ fprintf(stderr, "[roam] job queue full, dropped: %s\n", line); return; }
    snprintf(g_q[g_qt], sizeof g_q[g_qt], "%s", line); g_qt = (g_qt + 1) % QMAX;
}

/* ---- world inspection ------------------------------------------------------- */
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static const char *cstr(uint32_t a){
    static char b[4][48]; static int k; char *o = b[k++ & 3];
    if (!ok(a, 32)){ snprintf(o, 48, "?"); return o; }
    int i = 0; for (; i < 40 && MR8(a + i) >= 0x20 && MR8(a + i) < 0x7f; i++) o[i] = (char)MR8(a + i);
    o[i] = 0; return o;
}
static uint32_t player_car(void){ uint32_t p = rd32(G_PLAYER1); return p ? rd32(p + 4u) : 0; }
static int roaming(void){
    uint32_t race = rd32(G_RACE);
    return rd32(G_FLOW) == 6u && ok(race, 0xc00) && MR32(race) == 4u && MR32(race + 0x24u) == 1u &&
           !rd32(G_CAREER_RACE);
}
/* free roam now (runtime/web/input_web.c: the touch controls' free-roam buttons) */
int roam_exploring(void){ return roaming(); }
/* One line per car of the world list (World +0 head; the list node is the
 * car's first two dwords). */
static void log_cars(const char *why){
    uint32_t w = rd32(G_WORLD);
    if (!ok(w, 0x20)){ fprintf(stderr, "[roam] f=%ld cars (%s): no world\n", autotest_frame(), why); return; }
    uint32_t me = player_car();
    fprintf(stderr, "[roam] f=%ld cars (%s): world 0x%08x count %u, player car 0x%08x\n",
            autotest_frame(), why, w, MR32(w + 0x10u), me);
    uint32_t e = MR32(w);
    for (int k = 0; e && e != w && k < 64 && ok(e, 0x9e0); k++, e = MR32(e)){
        uint32_t ti = MR32(e + 0xcu), rec = MR32(e + 0x14u), brain = MR32(e + 0x2cu), phys = MR32(e + 0x34u);
        fprintf(stderr, "[roam]   car 0x%08x%s #%d racer %d type %s drv %u (brain vt 0x%08x) phys %u (vt 0x%08x)"
                " rec 0x%08x kind %u pos %.1f %.1f %.1f hdg 0x%04x vel %.1f %.1f %.1f +5c0 %u +918 0x%08x\n",
                e, e == me ? " (player)" : "", (int16_t)MR16(e + 8u), (int16_t)MR16(e + 0xau),
                ok(ti, 16) ? cstr(ti) : "?", MR32(e + 0x24u), ok(brain, 4) ? MR32(brain) : 0, MR32(e + 0x28u),
                ok(phys, 4) ? MR32(phys) : 0, rec, ok(rec, 8) ? MR32(rec + 4u) : 0,
                (double)MRF32(e + 0x60u), (double)MRF32(e + 0x64u), (double)MRF32(e + 0x68u), MR16(e + 0x40cu),
                (double)MRF32(e + 0x70u), (double)MRF32(e + 0x74u), (double)MRF32(e + 0x78u),
                MR8(e + 0x5c0u), MR32(e + 0x918u));
    }
}

/* ---- creation probes (NFSU2_ROAM_PROBE=1) ----------------------------------
 * Log who creates and destroys cars, with the guest return address. */
static int g_probe = -1;
static int probe(void){ if (g_probe < 0){ const char *e = getenv("NFSU2_ROAM_PROBE"); g_probe = e && atoi(e); } return g_probe; }
void sub_00606480_orig(CPU *c);   /* CreateCar(ecx = setup, edx = racer record, driver type, ...) */
void sub_00606480(CPU *c){
    uint32_t ret = MR32(c->esp), ecx = c->ecx, edx = c->edx, a1 = MR32(c->esp + 4u), a2 = MR32(c->esp + 8u);
    sub_00606480_orig(c);
    if (probe())
        fprintf(stderr, "[roam] f=%ld CreateCar from 0x%08x ecx 0x%08x rec 0x%08x (kind %u) drv %u a2 0x%08x -> car 0x%08x\n",
                autotest_frame(), ret, ecx, edx, ok(edx, 8) ? MR32(edx + 4u) : 0, a1, a2, c->eax);
}
void sub_00604260_orig(CPU *c);   /* Car::Car(rec, pos*, u16 heading, float) */
void sub_00604260(CPU *c){
    uint32_t ret = MR32(c->esp), self = c->ecx, rec = MR32(c->esp + 4u), pos = MR32(c->esp + 8u),
             hd = MR32(c->esp + 12u), f4 = MR32(c->esp + 16u);
    sub_00604260_orig(c);
    if (probe()){
        float x = ok(pos, 12) ? MRF32(pos) : 0, y = ok(pos, 12) ? MRF32(pos + 4u) : 0, z = ok(pos, 12) ? MRF32(pos + 8u) : 0, fv;
        memcpy(&fv, &f4, 4);
        fprintf(stderr, "[roam] f=%ld Car::Car 0x%08x from 0x%08x rec 0x%08x (kind %u, cartype %u) pos %.1f %.1f %.1f hdg 0x%04x a4 %g\n",
                autotest_frame(), self, ret, rec, ok(rec, 0x20) ? MR32(rec + 4u) : 0, ok(rec, 0x20) ? MR32(rec + 0x10u) : 0,
                (double)x, (double)y, (double)z, hd & 0xffffu, (double)fv);
    }
}


/* ---- ghost-car prototype ---------------------------------------------------
 * "roam ghost on K [setpos|direct]": take the K-th parked free-roam opponent
 * (the random-encounter cars: World +0x14 .. +0x18 index range of the car
 * array at World +0x1C), activate it (Car::Activate 0x5F6D50), make it inert
 * (driver type 0 = no brain, physics mode 0 = no integration) and move it
 * along a circle next to the player from host code every frame:
 *   setpos  Car::SetPosition (0x5FBD60: pos*, speed, u16 heading, flag), the
 *           game's own placement (matrix from heading, snapped to the ground)
 *   direct  write the pose fields (+0x60 position, +0x70 velocity, +0x90
 *           orientation rows, +0x110 angular velocity, +0x40C heading,
 *           +0x42C speed), then the section/collider update 0x5F6CD0
 * "roam ghost off": Car::Deactivate (0x5F6E20) parks it again. */
#define FN_CAR_ACTIVATE    0x005f6d50u
#define FN_CAR_DEACTIVATE  0x005f6e20u
#define FN_CAR_SETDRIVER   0x005eba30u   /* thiscall (type) */
#define FN_CAR_SETPHYSICS  0x005f46a0u   /* thiscall (mode) */
#define FN_CAR_SETPOSITION 0x005fbd60u   /* thiscall (pos*, speed, u16 heading, flag) */
#define FN_CAR_SECTION     0x005f6cd0u   /* thiscall () */
#define G_WORLDTIMER       0x0086518cu   /* int, 1/4000 s */
static struct { int on, direct, nocol, replay; uint32_t car; float cx, cy, cz, r, w; uint32_t t0; long remode;
                float lx, ly, lz; uint32_t lwt; long lf; double jsum, jmax; long jn; } G;

/* ---- local state recorder (what a client would send) ------------------------
 * "roam rec on|off": sample the player's car every 50 ms of world time (20 Hz,
 * a plausible network send rate): position +0x60, velocity +0x70, orientation
 * rows +0x90 (forward) / +0xA0 (left) / +0xB0 (up), angular velocity +0x110,
 * speed +0x42C, gear +0x434, engine rpm +0x440.  "roam rewind" jumps the
 * player back to the first sample (hot-position jump). */
typedef struct { uint32_t wt; float p[3], v[3], f[3], l[3], u[3], w[3], spd, rpm; int gear; } Sample;
#define RECN 8192
static Sample g_rec[RECN]; static int g_nrec, g_recon; static uint32_t g_rec_next;
static void rd3(uint32_t a, float *o){ o[0] = MRF32(a); o[1] = MRF32(a + 4u); o[2] = MRF32(a + 8u); }
/* "roam trace on FILE|off": every frame (main-loop hook), the player's car
 * state and the clocks, to see which clock the car's motion follows. */
double hfr_sim_ms(void);
static FILE *g_trace;
static void trace_tick(void){
    uint32_t car = player_car();
    if (!g_trace || !car) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (getenv("NFSU2_ROAM_TRACECTL")){   /* the controls block (car +0x30 -> 0x220 bytes) as raw dwords */
        static FILE *cf; if (!cf) cf = fopen(getenv("NFSU2_ROAM_TRACECTL"), "w");
        uint32_t ctl = MR32(car + 0x30u);
        if (cf && ok(ctl, 0x220)){ fprintf(cf, "%ld,%.4f", autotest_frame(), (double)MRF32(car + 0x118u));
            for (uint32_t o = 0; o < 0x220u; o += 4) fprintf(cf, ",%08x", MR32(ctl + o));
            for (uint32_t o = 0x400; o < 0x600u; o += 4) fprintf(cf, ",%08x", MR32(car + o));
            fputc('\n', cf); }
    }
    fprintf(g_trace, "%ld,%u,%u,%.4f,%.6f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", autotest_frame(),
            MR32(G_WORLDTIMER), MR32(0x008651acu), hfr_sim_ms(), (double)ts.tv_sec + ts.tv_nsec / 1e9,
            (double)MRF32(0x00865154u), (double)MRF32(car + 0x60u), (double)MRF32(car + 0x64u), (double)MRF32(car + 0x68u),
            (double)MRF32(car + 0x70u), (double)MRF32(car + 0x74u), (double)MRF32(car + 0x78u),
            (double)MRF32(car + 0x50u), (double)MRF32(car + 0x54u), (double)MRF32(car + 0x58u), (double)MRF32(car + 0x42cu),
            (double)MRF32(car + 0x440u));
}
/* Simulation clock for samples.  At the main-loop hook the car's position
 * already includes this frame's world dt ([0x865154]) while WorldTimer
 * (0x86518C) does not yet (run roam-22: displacement / (v * wdt) = 0.99..1.01
 * per frame, against 0.5..2.0 with WorldTimer differences), so samples are
 * stamped with the sum of world dt, in WorldTimer units (1/4000 s). */
#define G_WORLDDT 0x00865154u
static double g_simt;
static uint32_t sim_now(void){ return (uint32_t)(int64_t)(g_simt * 4000.0 + 0.5); }
static void rec_tick(void){
    uint32_t car = player_car();
    if (!g_recon || !car || g_nrec >= RECN) return;
    uint32_t wt = sim_now();
    if (g_nrec && (int32_t)(wt - g_rec_next) < 0) return;
    g_rec_next = wt + 200u;                                    /* 4000 units/s */
    Sample *s = &g_rec[g_nrec++];
    s->wt = wt; rd3(car + 0x60u, s->p); rd3(car + 0x70u, s->v); rd3(car + 0x90u, s->f); rd3(car + 0xa0u, s->l);
    rd3(car + 0xb0u, s->u); rd3(car + 0x110u, s->w); s->spd = MRF32(car + 0x42cu); s->rpm = MRF32(car + 0x440u);
    s->gear = (int)MR32(car + 0x434u);
}
/* rotation rows (f, l, u) <-> quaternion */
static void m2q(const float *f, const float *l, const float *u, float *q){
    float tr = f[0] + l[1] + u[2];
    if (tr > 0){ float s = sqrtf(tr + 1.f) * 2.f; q[3] = .25f * s; q[0] = (l[2] - u[1]) / s; q[1] = (u[0] - f[2]) / s; q[2] = (f[1] - l[0]) / s; }
    else if (f[0] > l[1] && f[0] > u[2]){ float s = sqrtf(1.f + f[0] - l[1] - u[2]) * 2.f; q[3] = (l[2] - u[1]) / s; q[0] = .25f * s; q[1] = (l[0] + f[1]) / s; q[2] = (u[0] + f[2]) / s; }
    else if (l[1] > u[2]){ float s = sqrtf(1.f + l[1] - f[0] - u[2]) * 2.f; q[3] = (u[0] - f[2]) / s; q[0] = (l[0] + f[1]) / s; q[1] = .25f * s; q[2] = (u[1] + l[2]) / s; }
    else { float s = sqrtf(1.f + u[2] - f[0] - l[1]) * 2.f; q[3] = (f[1] - l[0]) / s; q[0] = (u[0] + f[2]) / s; q[1] = (u[1] + l[2]) / s; q[2] = .25f * s; }
}
static void q2m(const float *q, float *f, float *l, float *u){
    float x = q[0], y = q[1], z = q[2], w = q[3];
    f[0] = 1 - 2*(y*y + z*z); f[1] = 2*(x*y + z*w);     f[2] = 2*(x*z - y*w);
    l[0] = 2*(x*y - z*w);     l[1] = 1 - 2*(x*x + z*z); l[2] = 2*(y*z + x*w);
    u[0] = 2*(x*z + y*w);     u[1] = 2*(y*z - x*w);     u[2] = 1 - 2*(x*x + y*y);
}
static void slerp(const float *a, const float *b0, float t, float *o){
    float b[4] = { b0[0], b0[1], b0[2], b0[3] }, d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    if (d < 0){ d = -d; for (int i = 0; i < 4; i++) b[i] = -b[i]; }
    float k0 = 1 - t, k1 = t;
    if (d < 0.9995f){ float th = acosf(d), sn = sinf(th); k0 = sinf((1 - t) * th) / sn; k1 = sinf(t * th) / sn; }
    float n = 0; for (int i = 0; i < 4; i++){ o[i] = k0 * a[i] + k1 * b[i]; n += o[i] * o[i]; }
    n = 1.f / sqrtf(n); for (int i = 0; i < 4; i++) o[i] *= n;
}
/* Pose at world time t (units of 1/4000 s) from the recording: cubic Hermite
 * position (with the sampled velocities), slerped orientation, linear rest. */
typedef struct { float p[3], v[3], f[3], l[3], u[3], w[3], spd, rpm; int gear; } Pose;
static int rec_pose(uint32_t t, Pose *o){
    if (g_nrec < 2) return 0;
    int k = 0;
    if ((int32_t)(t - g_rec[0].wt) <= 0) k = 0;
    else { while (k + 2 < g_nrec && (int32_t)(g_rec[k + 1].wt - t) <= 0) k++; }
    const Sample *a = &g_rec[k], *b = &g_rec[k + 1];
    float dt = (float)(int32_t)(b->wt - a->wt) / 4000.f, u = dt > 0 ? (float)(int32_t)(t - a->wt) / 4000.f / dt : 0;
    if (u < 0) u = 0; if (u > 1) u = 1;
    float h00 = 2*u*u*u - 3*u*u + 1, h10 = u*u*u - 2*u*u + u, h01 = -2*u*u*u + 3*u*u, h11 = u*u*u - u*u;
    for (int i = 0; i < 3; i++){
        o->p[i] = h00 * a->p[i] + h10 * dt * a->v[i] + h01 * b->p[i] + h11 * dt * b->v[i];
        o->v[i] = a->v[i] + u * (b->v[i] - a->v[i]); o->w[i] = a->w[i] + u * (b->w[i] - a->w[i]); }
    float qa[4], qb[4], q[4]; m2q(a->f, a->l, a->u, qa); m2q(b->f, b->l, b->u, qb); slerp(qa, qb, u, q); q2m(q, o->f, o->l, o->u);
    o->spd = a->spd + u * (b->spd - a->spd); o->rpm = a->rpm + u * (b->rpm - a->rpm); o->gear = u < .5f ? a->gear : b->gear;
    return 1;
}
static void rewind_player(void){
    if (!g_nrec){ fprintf(stderr, "[roam] rewind: nothing recorded\n"); return; }
    const Sample *s = &g_rec[0];
    uint16_t hd = (uint16_t)(int)(atan2f(s->f[1], s->f[0]) / (2.f * 3.14159265f) * 65536.f);
    MRF32(G_HOTPOS) = s->p[0]; MRF32(G_HOTPOS + 4) = s->p[1]; MRF32(G_HOTPOS + 8) = s->p[2] + 0.5f;
    MR16(G_HOTPOS_ANGLE) = hd; MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
    fprintf(stderr, "[roam] f=%ld rewind: player back to %.1f %.1f %.1f heading 0x%04x (%d samples, %.1f s)\n", autotest_frame(),
            (double)s->p[0], (double)s->p[1], (double)s->p[2], hd, g_nrec, (double)(int32_t)(g_rec[g_nrec - 1].wt - s->wt) / 4000.0);
}
#define FN_BODY_DISABLE    0x005930f0u   /* DynamicCollisionBody (car +0x3C): ++[+0x17] */
static uint32_t opponent_car(int k){
    uint32_t w = rd32(G_WORLD);
    if (!ok(w, 0x80)) return 0;
    int first = (int)MR32(w + 0x14u), end = (int)MR32(w + 0x18u);
    if (first < 0 || end > 22 || first + k >= end) return 0;
    return rd32(w + 0x1cu + 4u * (uint32_t)(first + k));
}
/* The free-roam RandomEncounterManager (outrun challengers) owns these cars:
 * race coordinator [0x890118] +0xC40, state machine 0x42EDF0 (0 none, 1 poll
 * career, 2 loading opponents, 3 trying to engage, 4 racing, 5 deactivate;
 * any value above 5 makes Update return at once).  It treats the slot cars'
 * brains as AI racer brains and crashes on our inert car (0x42A860 ->
 * 0x41C100 in run roam-18), so it is switched off while a ghost exists. */
#define RC_ENCOUNTER_MGR 0xc40u
static uint32_t encounter_mgr(void){ uint32_t r = rd32(G_RACE); return ok(r, 0xc44) ? r + RC_ENCOUNTER_MGR : 0; }
static void ghost_on(CPU *c, int k, int direct, int nocol, int replay){
    uint32_t car = opponent_car(k), me = player_car(), a, em = encounter_mgr();
    if (!car || !me){ fprintf(stderr, "[roam] f=%ld ghost: no opponent car %d / no player\n", autotest_frame(), k); return; }
    if (em){ fprintf(stderr, "[roam] f=%ld ghost: encounter manager state %u -> 6 (off)\n", autotest_frame(), MR32(em)); MR32(em) = 6; }
    guest_call(c, FN_CAR_ACTIVATE, car, 0, 0, 0);
    a = 0; guest_call(c, FN_CAR_SETDRIVER, car, 0, 1, &a);
    a = 0; guest_call(c, FN_CAR_SETPHYSICS, car, 0, 1, &a);
    if (nocol && ok(MR32(car + 0x3cu), 0x20)) guest_call(c, FN_BODY_DISABLE, MR32(car + 0x3cu), 0, 0, 0);
    /* circle centre 17 m ahead of the player (in view of the chase camera),
     * radius 7 m, 0.6 rad/s */
    float ah = 17.f, lf = 0.f, rr = 7.f;   /* NFSU2_ROAM_CIRCLE="ahead,left,radius" (m) */
    { const char *e = getenv("NFSU2_ROAM_CIRCLE"); if (e) sscanf(e, "%f,%f,%f", &ah, &lf, &rr); }
    G.cx = MRF32(me + 0x60u) + ah * MRF32(me + 0x90u) + lf * MRF32(me + 0xa0u);
    G.cy = MRF32(me + 0x64u) + ah * MRF32(me + 0x94u) + lf * MRF32(me + 0xa4u);
    G.cz = MRF32(me + 0x68u);
    G.r = rr; G.w = rr > 15.f ? 4.f / rr : 0.6f; G.t0 = replay ? sim_now() : MR32(G_WORLDTIMER); G.car = car; G.direct = direct || replay; G.nocol = nocol;
    G.replay = replay; G.lf = -1; G.jsum = G.jmax = 0; G.jn = 0; G.on = 1;
    fprintf(stderr, "[roam] f=%ld ghost on: car 0x%08x (%s) mode %s%s, circle at %.1f %.1f %.1f\n", autotest_frame(),
            car, cstr(MR32(car + 0xcu)), replay ? "replay" : direct ? "direct" : "setpos", nocol ? " nocol" : "",
            (double)G.cx, (double)G.cy, (double)G.cz);
}
/* Model and customization: Car::SetRideInfo (0x601110, thiscall(RideInfo*))
 * copies a RideInfo into the car's racer record and rebuilds the collision
 * body, the CarRenderInfo and the physics data (what the traffic manager does
 * to re-skin a traffic car).  The ride's model must be resident: a stock
 * ride is built in guest memory (RideInfo::Init 0x610270 (type, 1, 0, 0),
 * stock parts 0x637040 (0)), streamed with CarLoader::Load (0x63BC70 on the
 * loader at 0x8A3288, (ride, 0, 0) -> handle) + Service (0x63BCD0 (0, 0)),
 * and applied once CarLoader's entry for the handle reports loaded (0x610910;
 * entries are on the list at loader +0x58, handle at +0x123C). */
#define FN_MALLOC          0x00575620u   /* cdecl (size) */
#define FN_RIDE_INIT       0x00610270u   /* thiscall (type, 1, 0, 0) */
#define FN_RIDE_STOCKPARTS 0x00637040u   /* thiscall (0) */
#define FN_RIDE_SKINS      0x0061c280u   /* thiscall (n): DUMMY_SKIN<n> etc. */
#define FN_CAR_SETRIDE     0x00601110u   /* thiscall (RideInfo*) */
#define G_CARLOADER        0x008a3288u
#define FN_LOADER_LOAD     0x0063bc70u   /* thiscall (ride, 0, 0) -> handle */
#define FN_LOADER_SERVICE  0x0063bcd0u   /* thiscall (0, 0) */
#define FN_LOADER_LOADED   0x00610910u   /* (entry) -> 1 when every part is in */
#define FN_LOADER_WAIT     0x0061ca50u   /* thiscall (handle): blocks until loaded */
static struct { uint32_t ride, handle; long since; } R;
static uint32_t loader_entry(uint32_t h){
    uint32_t head = G_CARLOADER + 0x58u, e = rd32(head);
    for (int k = 0; e && e != head && k < 64; k++, e = rd32(e)) if (rd32(e + 0x123cu) == h) return e;
    return 0;
}
static void ghost_ride(CPU *c, const char *what){
    if (!G.on){ fprintf(stderr, "[roam] ghost ride: no ghost\n"); return; }
    if (!strcasecmp(what, "player")){
        uint32_t me = player_car(), ride = me ? MR32(me + 0x10u) : 0;
        if (!ride) return;
        guest_call(c, FN_CAR_SETRIDE, G.car, 0, 1, &ride);
        fprintf(stderr, "[roam] f=%ld ghost ride: the player's ride (%s), render info 0x%08x\n", autotest_frame(),
                cstr(MR32(G.car + 0xcu)), MR32(G.car + 0x554u));
        return;
    }
    uint32_t type = (uint32_t)atoi(what), sz = 0x900u;
    uint32_t ride = guest_call(c, FN_MALLOC, 0, 0, 1, &sz);
    if (!ride){ fprintf(stderr, "[roam] ghost ride: malloc failed\n"); return; }
    for (uint32_t i = 0; i < sz; i += 4) MR32(ride + i) = 0;
    if (!strncasecmp(what, "copy", 4)){       /* "copyK": racer record K's ride, own skin slot */
        uint32_t src = 0x890170u + (uint32_t)atoi(what + 4) * 0x940u;
        memcpy(g_mem + ride, g_mem + src, 0x8f0);
        uint32_t slot = (MR32(G.car + 0x14u) - 0x890160u) / 0x940u + 1u;
        guest_call(c, FN_RIDE_SKINS, ride, 0, 1, &slot);
        uint32_t a3[3] = { ride, 0, 0 };
        R.handle = guest_call(c, FN_LOADER_LOAD, G_CARLOADER, 0, 3, a3);
        uint32_t a2[2] = { 0, 0 }; guest_call(c, FN_LOADER_SERVICE, G_CARLOADER, 0, 2, a2);
        R.ride = ride; R.since = autotest_frame();
        fprintf(stderr, "[roam] f=%ld ghost ride: copy of racer %d's ride (%s), skin slot %u, handle 0x%08x, player render info 0x%08x\n",
                autotest_frame(), atoi(what + 4), cstr(MR32(G_CARTYPES) + MR32(src) * 0x890u), slot, R.handle, MR32(player_car() + 0x554u));
        return;
    }
    uint32_t a4[4] = { type, 1, 0, 0 }; guest_call(c, FN_RIDE_INIT, ride, 0, 4, a4);
    /* the racer's own composited skin / wheel slots: DUMMY_SKIN<n>,
     * DUMMY_WHEEL<n>, DUMMY_SPINNER<n> (0x61C280), n = racer record index + 1
     * (player 1, the three free-roam opponents 2..4); a ride without them
     * (or with another racer's) composites into the wrong skin texture */
    uint32_t slot = (MR32(G.car + 0x14u) - 0x890160u) / 0x940u + 1u;
    if (getenv("NFSU2_ROAM_NOSKIN") == 0) guest_call(c, FN_RIDE_SKINS, ride, 0, 1, &slot);
    uint32_t z = 0; guest_call(c, FN_RIDE_STOCKPARTS, ride, 0, 1, &z);
    uint32_t a3[3] = { ride, 0, 0 };
    R.handle = guest_call(c, FN_LOADER_LOAD, G_CARLOADER, 0, 3, a3);
    uint32_t a2[2] = { 0, 0 }; guest_call(c, FN_LOADER_SERVICE, G_CARLOADER, 0, 2, a2);
    R.ride = ride; R.since = autotest_frame();
    fprintf(stderr, "[roam] f=%ld ghost ride: stock %s (type %u, skin slot %u) loading, handle 0x%08x entry 0x%08x\n",
            autotest_frame(), cstr(MR32(G_CARTYPES) + type * 0x890u), type, slot, R.handle, loader_entry(R.handle));
}
static void ride_tick(CPU *c){
    if (!R.ride || !G.on) return;
    uint32_t e = loader_entry(R.handle);
    uint32_t done = e ? guest_call(c, FN_LOADER_LOADED, G_CARLOADER, 0, 1, &e) : 0;
    if (!done){
        if (autotest_frame() - R.since > 1200){ fprintf(stderr, "[roam] f=%ld ghost ride: not loaded after 10 s\n", autotest_frame()); R.ride = 0; }
        return; }
    guest_call(c, FN_LOADER_WAIT, G_CARLOADER, 0, 1, &R.handle);
    if (getenv("NFSU2_ROAM_NOAPPLY")){ fprintf(stderr, "[roam] f=%ld ghost ride: loaded, not applied\n", autotest_frame()); R.ride = 0; return; }
    guest_call(c, FN_CAR_SETRIDE, G.car, 0, 1, &R.ride);
    fprintf(stderr, "[roam] f=%ld ghost ride: loaded after %ld frames, car now %s, render info 0x%08x, player's 0x%08x\n", autotest_frame(),
            autotest_frame() - R.since, cstr(MR32(G.car + 0xcu)), MR32(G.car + 0x554u), MR32(player_car() + 0x554u));
    R.ride = 0;
}
/* CarLoader entries (list at loader +0x58): handle, refcount, car type */
static void log_loader(const char *why){
    uint32_t head = G_CARLOADER + 0x58u, e = rd32(head); int n = 0;
    fprintf(stderr, "[roam] f=%ld loader (%s): +0x1c %u +0x20 %u entries:", autotest_frame(), why,
            MR32(G_CARLOADER + 0x1cu), MR32(G_CARLOADER + 0x20u));
    for (; e && e != head && n < 40; n++, e = rd32(e))
        fprintf(stderr, " [h%u ref%d %s]", rd32(e + 0x123cu), (int)rd32(e + 8u), cstr(rd32(G_CARTYPES) + rd32(e + 0x10u) * 0x890u));
    fprintf(stderr, " (%d)\n", n);
}
/* "roam spawn": an extra AI-range car of our own (EXPERIMENT).  A copy of
 * opponent record 0 (0x890AA0) in guest memory with racer number 103+n, name
 * "REMOTE<n>" and its own skin slot (DUMMY_SKIN 5+n), its ride loaded through
 * the CarLoader, then CreateCar (0x606480: ecx = grid entry {vec3, +0x10 u16
 * heading}, edx = record, driver 0, no start object) next to the player and
 * World::IndexCars (0x5F0D20).  The new car is then opponent K = 3 + n. */
#define FN_CREATECAR   0x00606480u
#define FN_INDEXCARS   0x005f0d20u
static struct { uint32_t rec, handle; int n, pending; long since; } SP;
static void spawn_start(CPU *c){
    uint32_t me = player_car(), sz = 0x940u;
    if (!me || SP.pending){ fprintf(stderr, "[roam] spawn: no player / busy\n"); return; }
    uint32_t rec = guest_call(c, FN_MALLOC, 0, 0, 1, &sz);
    if (!rec) return;
    memcpy(g_mem + rec, g_mem + 0x890aa0u, 0x940);
    MR8(rec + 1u) = (uint8_t)(103 + SP.n); MR8(rec + 0x900u) = 0;
    snprintf((char *)(g_mem + rec + 0x90cu), 16, "REMOTE%d", SP.n);
    uint32_t slot = 5u + (uint32_t)SP.n;
    guest_call(c, FN_RIDE_SKINS, rec + 0x10u, 0, 1, &slot);
    uint32_t a3[3] = { rec + 0x10u, 0, 0 };
    SP.handle = guest_call(c, FN_LOADER_LOAD, G_CARLOADER, 0, 3, a3);
    uint32_t a2[2] = { 0, 0 }; guest_call(c, FN_LOADER_SERVICE, G_CARLOADER, 0, 2, a2);
    SP.rec = rec; SP.pending = 1; SP.since = autotest_frame();
    fprintf(stderr, "[roam] f=%ld spawn: record 0x%08x (%s, skin slot %u), loading handle 0x%08x\n", autotest_frame(), rec,
            cstr(MR32(G_CARTYPES) + MR32(rec + 0x10u) * 0x890u), slot, SP.handle);
}
static void spawn_tick(CPU *c){
    if (!SP.pending) return;
    uint32_t e = loader_entry(SP.handle), me = player_car(), w = rd32(G_WORLD);
    uint32_t done = e ? guest_call(c, FN_LOADER_LOADED, G_CARLOADER, 0, 1, &e) : 0;
    if (!done){ if (autotest_frame() - SP.since > 1200){ fprintf(stderr, "[roam] spawn: not loaded\n"); SP.pending = 0; } return; }
    guest_call(c, FN_LOADER_WAIT, G_CARLOADER, 0, 1, &SP.handle);
    if (!me || !ok(w, 0x80)){ SP.pending = 0; return; }
    uint32_t grid = (c->esp - 256u) & ~15u;
    for (uint32_t i = 0; i < 0x20u; i += 4) MR32(grid + i) = 0;
    MRF32(grid) = MRF32(me + 0x60u) + 8.f * MRF32(me + 0xa0u); MRF32(grid + 4) = MRF32(me + 0x64u) + 8.f * MRF32(me + 0xa4u);
    MRF32(grid + 8) = MRF32(me + 0x68u) + 0.5f; MR16(grid + 0x10u) = MR16(me + 0x40cu);
    uint32_t n0 = MR32(w + 0x10u), a2[2] = { 0, 0 };
    guest_call(c, FN_CREATECAR, grid, SP.rec, 2, a2);
    guest_call(c, FN_INDEXCARS, w, 0, 0, 0);
    fprintf(stderr, "[roam] f=%ld spawn: CreateCar + IndexCars: cars %u -> %u, AI range [%u, %u)\n", autotest_frame(),
            n0, MR32(w + 0x10u), MR32(w + 0x14u), MR32(w + 0x18u));
    SP.n++; SP.pending = 0;
}
static void log_types(void){
    uint32_t t = rd32(G_CARTYPES);
    fprintf(stderr, "[roam] f=%ld car types:", autotest_frame());
    for (uint32_t i = 0; t && i < 64 && ok(t + i * 0x890u, 0x100); i++){
        const char *n = cstr(t + i * 0x890u);
        if (!n[0] || n[0] == '?') break;
        fprintf(stderr, " %u=%s", i, n);
    }
    fputc('\n', stderr);
}
static void ghost_off(CPU *c){
    if (!G.on) return;
    if (G.nocol && ok(MR32(G.car + 0x3cu), 0x20)) MR8(MR32(G.car + 0x3cu) + 0x17u) = 0;
    guest_call(c, FN_CAR_DEACTIVATE, G.car, 0, 0, 0);
    { uint32_t em = encounter_mgr(); if (em && MR32(em) == 6) MR32(em) = 0; }   /* re-initialises itself */
    fprintf(stderr, "[roam] f=%ld ghost off: car 0x%08x parked (physics mode reset %ld times), encounters back on\n",
            autotest_frame(), G.car, G.remode);
    G.remode = 0;
    G.on = 0;
}
static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static void ghost_tick(CPU *c){
    if (!G.on) return;
    if (!roaming() || !ok(G.car, 0x9e0)){ fprintf(stderr, "[roam] f=%ld ghost: left free roam, off\n", autotest_frame()); G.on = 0; return; }
    { uint32_t em = encounter_mgr(); if (em && MR32(em) != 6){
        fprintf(stderr, "[roam] f=%ld ghost: encounter manager state %u again -> 6\n", autotest_frame(), MR32(em)); MR32(em) = 6; } }
    float t = (float)(int32_t)(MR32(G_WORLDTIMER) - G.t0) / 4000.f, a = G.w * t;
    float x = G.cx + G.r * cosf(a), y = G.cy + G.r * sinf(a), z = G.cz;
    float fx = -sinf(a), fy = cosf(a), v = G.r * G.w;          /* counter-clockwise tangent */
    uint16_t hd = (uint16_t)(int)(atan2f(fy, fx) / (2.f * 3.14159265f) * 65536.f);
    uint32_t car = G.car;
    /* the brain's wake-up (null brain vf +0x38 = 0x404890) switches an
     * inert car to physics mode 3 when something touches it: put it back */
    if (MR32(car + 0x28u) != 0){ uint32_t z = 0; guest_call(c, FN_CAR_SETPHYSICS, car, 0, 1, &z); G.remode++; }
    if (!G.direct){
        uint32_t pv = (c->esp - 256u) & ~15u;                    /* above guest_call's frame */
        MRF32(pv) = x; MRF32(pv + 4) = y; MRF32(pv + 8) = z + 0.3f;
        uint32_t arg[4] = { pv, fbits(v), hd, 0 };
        guest_call(c, FN_CAR_SETPOSITION, car, 0, 4, arg);
    } else if (G.replay){
        /* the recorded path from its start, at the recording's own pace */
        Pose P; uint32_t now = sim_now();
        if (!rec_pose(g_rec[0].wt + (now - G.t0), &P)) return;
        for (int i = 0; i < 3; i++){
            MRF32(car + 0x60u + 4u * (uint32_t)i) = P.p[i]; MRF32(car + 0x70u + 4u * (uint32_t)i) = P.v[i];
            MRF32(car + 0x110u + 4u * (uint32_t)i) = P.w[i];
            MRF32(car + 0x90u + 4u * (uint32_t)i) = P.f[i]; MRF32(car + 0xa0u + 4u * (uint32_t)i) = P.l[i];
            MRF32(car + 0xb0u + 4u * (uint32_t)i) = P.u[i];
            MRF32(car + 0xd0u + 4u * (uint32_t)i) = P.f[i]; MRF32(car + 0xe0u + 4u * (uint32_t)i) = P.l[i];
            MRF32(car + 0xf0u + 4u * (uint32_t)i) = P.u[i]; }
        MR16(car + 0x40cu) = (uint16_t)(int)(atan2f(P.f[1], P.f[0]) / (2.f * 3.14159265f) * 65536.f);
        MRF32(car + 0x42cu) = P.spd; MRF32(car + 0x430u) = P.spd; MRF32(car + 0x440u) = P.rpm; MR32(car + 0x434u) = (uint32_t)P.gear;
        /* wheel spin: speed / tyre radius per wheel (RidePhysicsInfo +0x20 +0x30k), as SetPosition does
         * (whether the renderer uses these or +0x4E8/+0x4F8 is UNVERIFIED) */
        uint32_t ph = MR32(car + 0x20u);
        if (ok(ph, 0xc0)) for (uint32_t k = 0; k < 4; k++){ float r = MRF32(ph + 0x20u + 0x30u * k); if (r > 0.05f) MRF32(car + 0x330u + 4u * k) = P.spd / r; }
        guest_call(c, FN_CAR_SECTION, car, 0, 0, 0);
        /* smoothness: per-frame displacement against the interpolated speed
         * (m; a pop shows as a large difference), and an optional per-frame
         * trace NFSU2_ROAM_GHOSTCSV=<file> */
        long f = autotest_frame();
        static FILE *trace; static int trace_tried;
        if (!trace_tried){ trace_tried = 1; const char *e = getenv("NFSU2_ROAM_GHOSTCSV"); if (e && *e) trace = fopen(e, "w");
            if (trace) fprintf(trace, "frame,wt,px,py,pz,vx,vy,vz,fx,fy,fz\n"); }
        if (trace) fprintf(trace, "%ld,%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f\n", f, now, P.p[0], P.p[1], P.p[2],
                           P.v[0], P.v[1], P.v[2], P.f[0], P.f[1], P.f[2]);
        if (G.lf >= 0 && now != G.lwt){
            float dx = P.p[0] - G.lx, dy = P.p[1] - G.ly, dz = P.p[2] - G.lz;
            double d = sqrt((double)(dx*dx + dy*dy + dz*dz)), dt = (double)(int32_t)(now - G.lwt) / 4000.0;
            double vv = sqrt((double)(P.v[0]*P.v[0] + P.v[1]*P.v[1] + P.v[2]*P.v[2])), err = fabs(d - vv * dt);
            G.jsum += err; G.jn++; if (err > G.jmax) G.jmax = err;
            if (G.jn % 240 == 0){ uint32_t me = player_car();
                float ddx = me ? MRF32(me + 0x60u) - P.p[0] : 0, ddy = me ? MRF32(me + 0x64u) - P.p[1] : 0;
                fprintf(stderr, "[roam] f=%ld ghost: %.0f m from the player, on loaded track (+0x5C0) %u, section +0x5CC 0x%08x, render info 0x%08x\n",
                        f, sqrt((double)(ddx*ddx + ddy*ddy)), MR8(car + 0x5c0u), MR32(car + 0x5ccu), MR32(car + 0x554u)); }
            if (G.jn % 240 == 0) fprintf(stderr, "[roam] f=%ld ghost replay: pos %.1f %.1f %.1f, speed %.2f m/s, frame step %.3f m"
                                         " (|step - v*dt| mean %.4f m, max %.4f m over %ld frames)\n", f, (double)P.p[0], (double)P.p[1],
                                         (double)P.p[2], vv, d, G.jsum / (double)G.jn, G.jmax, G.jn);
            if (trace) fflush(trace);
        }
        G.lx = P.p[0]; G.ly = P.p[1]; G.lz = P.p[2]; G.lwt = now; G.lf = f;
    } else {
        MRF32(car + 0x60u) = x; MRF32(car + 0x64u) = y; MRF32(car + 0x68u) = z;
        MRF32(car + 0x70u) = fx * v; MRF32(car + 0x74u) = fy * v; MRF32(car + 0x78u) = 0;
        float m[16] = { fx, fy, 0, 0,  -fy, fx, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        for (int i = 0; i < 16; i++){ MRF32(car + 0x90u + 4u * (uint32_t)i) = m[i]; MRF32(car + 0xd0u + 4u * (uint32_t)i) = m[i]; }
        MRF32(car + 0x110u) = 0; MRF32(car + 0x114u) = 0; MRF32(car + 0x118u) = G.w;
        MR16(car + 0x40cu) = hd; MRF32(car + 0x42cu) = v; MRF32(car + 0x430u) = v;
        guest_call(c, FN_CAR_SECTION, car, 0, 0, 0);
    }
}

/* Who changes the ghost car's driver type or physics mode (NFSU2_ROAM_PROBE);
 * a friend's car (free-roam multiplayer) is not woken into physics when
 * a car comes near it (roam_game.c, roam_game_inert). */
void sub_005f46a0_orig(CPU *c);   /* Car::SetPhysicsMode(this, mode) */
int roam_game_inert(uint32_t car, uint32_t mode, uint32_t from);
void sub_005f46a0(CPU *c){
    if (roam_game_inert(c->ecx, MR32(c->esp + 4u), MR32(c->esp))){ POP32(c->eip); c->esp += 4u; return; }   /* thiscall, ret 4 */
    if (G.on && c->ecx == G.car && probe())
        fprintf(stderr, "[roam] f=%ld ghost SetPhysicsMode(%u) from 0x%08x (was %u)\n", autotest_frame(),
                MR32(c->esp + 4u), MR32(c->esp), MR32(G.car + 0x28u));
    sub_005f46a0_orig(c);
}
/* A touching pair of collision bodies (0x5B2610, thiscall, from the
 * collision step 0x5FFA50 only): a hit between the player's car and a
 * friend's car is shared between the two (roam_game.c, roam_game_contact) */
void sub_005b2610_orig(CPU *c);
void roam_game_contact(CPU *c, void (*orig)(CPU *));
void sub_005b2610(CPU *c){ roam_game_contact(c, sub_005b2610_orig); }
/* The race's end, for the random encounters (0x42EFB0): new goals for the AI
 * range's cars that are out, through their brains; friends' cars have none
 * to give up and are left out (roam_game.c, roam_game_goals) */
void sub_0042cd60_orig(CPU *c);
void roam_game_goals(CPU *c, void (*orig)(CPU *));
void sub_0042cd60(CPU *c){ roam_game_goals(c, sub_0042cd60_orig); }
/* GetTextureInfo(hash, default, ...) (cdecl): friends' cars past the game's
 * six paint slots composite into textures of our own, DUMMY_SKIN7 and up
 * (roam_game.c, roam_game_texture); every other name is the game's */
void sub_004901d0_orig(CPU *c);
uint32_t roam_game_texture(uint32_t hash);
void sub_004901d0(CPU *c){
    uint32_t ti = roam_game_texture(MR32(c->esp + 4u));
    if (ti){ MR8(ti + 0x53u) = 1; c->eax = ti; POP32(c->eip); return; }     /* as the game marks a texture it hands out */
    sub_004901d0_orig(c);
}
/* bSlotPool's constructor (cdecl size, count, name, flags): friends' cars
 * need more of a few pools than the game makes, the car loader's rides and
 * the minimap's icons (roam_game.c, roam_game_pool) */
void sub_00440df0_orig(CPU *c);
uint32_t roam_game_pool(uint32_t name, uint32_t count);
void sub_00440df0(CPU *c){
    MR32(c->esp + 8u) = roam_game_pool(MR32(c->esp + 12u), MR32(c->esp + 8u));
    sub_00440df0_orig(c);
}
/* CarLoader: give back the memory a track load added to the Cars pool
 * (thiscall, no arguments), only once it is free again; and a track load's
 * addition to it (thiscall, the track's size), only when the pool is smaller
 * (roam_game.c, roam_game_loader_release, roam_game_pool_grow) */
void sub_0063a6f0_orig(CPU *c);
int roam_game_loader_release(CPU *c);
void sub_0063a6f0(CPU *c){
    if (roam_game_loader_release(c)) sub_0063a6f0_orig(c);
    else POP32(c->eip);
}
void sub_00610750_orig(CPU *c);
int roam_game_pool_grow(uint32_t size);
void sub_00610750(CPU *c){
    if (roam_game_pool_grow(MR32(c->esp + 4u))) sub_00610750_orig(c);
    else { POP32(c->eip); c->esp += 4u; }
}
void sub_005eba30_orig(CPU *c);   /* Car::SetDriverType(this, type) */
void sub_005eba30(CPU *c){
    if (G.on && c->ecx == G.car && probe())
        fprintf(stderr, "[roam] f=%ld ghost SetDriverType(%u) from 0x%08x (was %u)\n", autotest_frame(),
                MR32(c->esp + 4u), MR32(c->esp), MR32(G.car + 0x24u));
    sub_005eba30_orig(c);
}

void sub_005f4650_orig(CPU *c);   /* World::RebuildCarRenderInfos(car type or -1) */
void sub_005f4650(CPU *c){
    if (probe()) fprintf(stderr, "[roam] f=%ld RebuildCarRenderInfos(%d) from 0x%08x, player render info 0x%08x\n",
                         autotest_frame(), (int)MR32(c->esp + 4u), MR32(c->esp), player_car() ? MR32(player_car() + 0x554u) : 0);
    sub_005f4650_orig(c);
}
void sub_0063a1b0_orig(CPU *c);   /* CarLoader: unload unused entries (purge) */
int roam_game_purge(CPU *c, uint32_t ret);
void sub_0063a1b0(CPU *c){
    if (probe()) fprintf(stderr, "[roam] f=%ld CarLoader purge(%u) from 0x%08x\n", autotest_frame(), MR32(c->esp + 4u), MR32(c->esp));
    if (MR8(c->esp + 4u) && roam_game_purge(c, MR32(c->esp))){ c->eax = 1; POP32(c->eip); c->esp += 4u; return; }
    sub_0063a1b0_orig(c);
}

/* "roam shopgo NAME": jump onto a career shop's marker (as career.c's shopgo;
 * shops [0x8383F0] x [0x8383F4], 0xA0 bytes, +0 name, +0x3C marker hash;
 * markers PathsData 0x88F298 +0x438 size, +0x43C blob of records with +4 x,
 * +8 y, +0xC/+0x10 heading, +0x14 z, +0x30 hash, +0x42 record size). */
static uint32_t marker_by_hash(uint32_t h){
    uint32_t m = rd32(0x88f298u + 0x43cu), end = m + rd32(0x88f298u + 0x438u);
    for (int k = 0; m && m < end && k < 4096; k++){
        if (MR32(m + 0x30u) == h) return m;
        int16_t sz = (int16_t)MR16(m + 0x42u); if (sz <= 0) break; m += (uint32_t)sz;
    }
    return 0;
}
static void shop_go(const char *nm){
    uint32_t t = rd32(0x8383f0u), n = rd32(0x8383f4u), sh = 0;
    for (uint32_t i = 0; t && i < n && !sh; i++) if (!strcasecmp(cstr(t + i * 0xa0u), nm)) sh = t + i * 0xa0u;
    uint32_t m = sh ? marker_by_hash(MR32(sh + 0x3cu)) : 0;
    if (!m){ fprintf(stderr, "[roam] f=%ld shopgo %s: not found\n", autotest_frame(), nm); return; }
    double a = atan2((double)MRF32(m + 0x10u), (double)MRF32(m + 0xcu));
    MRF32(G_HOTPOS) = MRF32(m + 4u); MRF32(G_HOTPOS + 4) = MRF32(m + 8u); MRF32(G_HOTPOS + 8) = MRF32(m + 0x14u) + 0.5f;
    MR16(G_HOTPOS_ANGLE) = (uint16_t)(int)(a / (2.0 * 3.14159265358979) * 65536.0); MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
    fprintf(stderr, "[roam] f=%ld shopgo %s: %.1f %.1f %.1f\n", autotest_frame(), nm, (double)MRF32(m + 4u), (double)MRF32(m + 8u), (double)MRF32(m + 0x14u));
}
/* "roam racego": jump onto the start marker of the first available career
 * race (Career 0x85AD40 +0x71C8: 64 CareerRace*, +0x28 marker hash, +0
 * name), as career.c's careergo does */
static void race_go(void){
    for (int i = 0; i < 64; i++){
        uint32_t r = rd32(0x85ad40u + 0x71c8u + 4u * (uint32_t)i);
        uint32_t m = ok(r, 0x40) ? marker_by_hash(MR32(r + 0x28u)) : 0;
        if (!m) continue;
        double a = atan2((double)MRF32(m + 0x10u), (double)MRF32(m + 0xcu));
        MRF32(G_HOTPOS) = MRF32(m + 4u); MRF32(G_HOTPOS + 4) = MRF32(m + 8u); MRF32(G_HOTPOS + 8) = MRF32(m + 0x14u) + 0.5f;
        MR16(G_HOTPOS_ANGLE) = (uint16_t)(int)(a / (2.0 * 3.14159265358979) * 65536.0); MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
        fprintf(stderr, "[roam] f=%ld racego %s: %.1f %.1f %.1f\n", autotest_frame(), cstr(rd32(r)), (double)MRF32(m + 4u),
                (double)MRF32(m + 8u), (double)MRF32(m + 0x14u));
        return;
    }
    fprintf(stderr, "[roam] f=%ld racego: no available race\n", autotest_frame());
}
static uint32_t g_world_seen; static int g_active;   /* any roam command seen */
static void world_watch(void){
    uint32_t w = rd32(G_WORLD);
    if (!g_active && !probe()){ g_world_seen = w; return; }
    if (w != g_world_seen){
        fprintf(stderr, "[roam] f=%ld world 0x%08x -> 0x%08x (flow %u, cars %u)\n", autotest_frame(), g_world_seen, w,
                rd32(G_FLOW), ok(w, 0x20) ? MR32(w + 0x10u) : 0);
        g_world_seen = w; }
}

/* ---- guest memory snapshot (tests/asi_harness.c runs the Windows plugin on it) ---
 * "NFSUSNP1", then u32: version 1, eip (the hook), eax ecx edx ebx esp ebp esi
 * edi, fs_base, fcw, n; then n x { u32 block (address >> 14), 16 KB }. */
extern uint8_t g_cbits[1u << 15];
static void snapshot(CPU *c, const char *path){
    char p[200] = {0}; sscanf(path, "%199s", p);
    FILE *o = fopen(p, "wb");
    if (!o){ fprintf(stderr, "[roam] snapshot: cannot write %s\n", p); return; }
    uint32_t n = 0;
    for (uint32_t b = 0; b < (1u << 18); b++) if ((g_cbits[b >> 3] >> (b & 7)) & 1) n++;
    uint32_t hdr[14] = { 1u, HOOK_MAINLOOP, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, c->fs_base,
                         (uint32_t)c->fcw, n, 0 };
    fwrite("NFSUSNP1", 1, 8, o); fwrite(hdr, 4, 13, o);
    for (uint32_t b = 0; b < (1u << 18); b++){
        if (!((g_cbits[b >> 3] >> (b & 7)) & 1)) continue;
        fwrite(&b, 4, 1, o); fwrite(g_mem + ((uint64_t)b << 14), 1, 0x4000, o);
    }
    fclose(o);
    fprintf(stderr, "[roam] f=%ld snapshot: %u blocks (%u MB), esp 0x%08x -> %s\n", autotest_frame(), n, n >> 6, c->esp, p);
}

/* ---- main-loop hook ----------------------------------------------------------- */
static long g_carlog_every, g_carlog_next;
static int g_smsauto; static long g_sms_next;   /* "roam smsauto on": answer in-game SMS pop-ups */
static void run_job(CPU *c, const char *line){
    char sub[32] = {0}; int used = 0;
    sscanf(line, "%31s %n", sub, &used);
    const char *rest = line + used;
    if (!strcasecmp(sub, "call")){
        char tok[16][64]; int nt = 0; const char *p = rest;
        while (nt < 16 && *p){
            while (*p == ' ') p++;
            if (!*p) break;
            int k = 0; while (*p && *p != ' ' && k < 63) tok[nt][k++] = *p++;
            tok[nt++][k] = 0;
        }
        if (nt < 1) return;
        uint32_t fn = eval(tok[0]), ecx = 0, edx = 0, arg[15]; int na = 0;
        for (int i = 1; i < nt; i++){
            if (!strncasecmp(tok[i], "ecx=", 4)) ecx = eval(tok[i] + 4);
            else if (!strncasecmp(tok[i], "edx=", 4)) edx = eval(tok[i] + 4);
            else arg[na++] = eval(tok[i]);
        }
        uint32_t r = guest_call(c, fn, ecx, edx, na, arg);
        fprintf(stderr, "[roam] f=%ld call 0x%08x(ecx=0x%08x edx=0x%08x, %d args) -> eax=0x%08x (%d)\n",
                autotest_frame(), fn, ecx, edx, na, r, (int)r);
    } else if (!strcasecmp(sub, "cars")) log_cars("script");
    else if (!strcasecmp(sub, "types")) log_types();
    else if (!strcasecmp(sub, "loader")) log_loader("script");
    else if (!strcasecmp(sub, "rewind")) rewind_player();
    else if (!strcasecmp(sub, "spawn")) spawn_start(c);
    else if (!strcasecmp(sub, "snapshot")) snapshot(c, rest);
    else if (!strcasecmp(sub, "ghost")){
        char a1[16] = {0}, a2[16] = {0}, a3[16] = {0};
        sscanf(rest, "%15s %15s %15s", a1, a2, a3);
        char a4[16] = {0}; sscanf(rest, "%*s %*s %*s %15s", a4);
        if (!strcasecmp(a1, "on")) ghost_on(c, atoi(a2), !strcasecmp(a3, "direct"), !strcasecmp(a4, "nocol"), !strcasecmp(a3, "replay"));
        else if (!strcasecmp(a1, "ride")) ghost_ride(c, a2);
        else ghost_off(c);
    }
}
static void roam_frame(CPU *c){
    while (g_qh != g_qt){ char line[200]; snprintf(line, sizeof line, "%s", g_q[g_qh]); g_qh = (g_qh + 1) % QMAX; run_job(c, line); }
    long f = autotest_frame();
    if (rd32(G_FLOW) == 6u) g_simt += (double)MRF32(G_WORLDDT);
    if (g_smsauto && f >= g_sms_next){
        const char *t = autotest_top_fng();
        if (t && !strcasecmp(t, "UI_EngageMessageDialog.fng")){
            autotest_tap(0x1cu, 6); g_sms_next = f + 60;
            fprintf(stderr, "[roam] f=%ld smsauto: RETURN on %s\n", f, t); }
        else if (t && (!strcasecmp(t, "UI_SMS_Mailbox.fng") || !strcasecmp(t, "UI_SMS_Message.fng"))){
            autotest_tap(0x01u, 6); g_sms_next = f + 60;          /* ESCAPE: back to the game */
            fprintf(stderr, "[roam] f=%ld smsauto: ESCAPE on %s\n", f, t); }
        else if (t && strcasecmp(t, "HUD_SingleRace.fng") && g_sms_next < f - 600){
            fprintf(stderr, "[roam] f=%ld smsauto: top package %s\n", f, t); g_sms_next = f + 600; }
    }
    world_watch();
    trace_tick();
    rec_tick();
    ghost_tick(c);
    ride_tick(c);
    spawn_tick(c);
    if (g_carlog_every > 0 && f >= g_carlog_next){ g_carlog_next = f + g_carlog_every; log_cars("periodic"); }
}
void roam_game_frame(CPU *c);                 /* roam_game.c: the multiplayer feature */
void racehorn_frame(CPU *c);                  /* racehorn.c: horns in races between players */
void racenet_frame(CPU *c);                   /* racenet.c: how the other players' cars came in, at a race's end */
void racenet_prestep(CPU *c);                 /*   and the cars' own places back before the world's step */
void mprace_frame(CPU *c);                    /* mprace.c: a multiplayer race's trip from free roam and back */
void optslider_frame(CPU *c);                 /* optslider.c: our option sliders' copied parts (the pause menu's Horn Volume) */
void drift_frame(CPU *c);                     /* drift.c: drift mode in free roam (F2) */
void photo_frame(CPU *c);                     /* photo.c: photo mode (F12, the pause menu's Photo Mode) */
void language_texts_frame(CPU *c);            /* language.c: texts of our own in the game's language table */
void sms_frame(CPU *c);                       /* sms.c: a text message of ours on the game's phone */
void reads_frame(CPU *c);                     /* overrides.c: the streamer's reads and Loading screens, for the log */
void roam_game_hud(CPU *c);                 /* its names over friends' cars */
int roam_game_cmd(CPU *c, const char *args, long *wait_start);
/* the "virtual Windows friend" (roam_asipeer.c, only in out/nfsu2_asipeer):
 * the Windows plugin under Unicorn instead of roam_game.c */
__attribute__((weak)) int roam_asipeer_frame(CPU *c){ (void)c; return 0; }
__attribute__((weak)) int roam_asipeer_cmd(CPU *c, const char *args, long *wait_start){
    (void)c; (void)args; (void)wait_start;
    fprintf(stderr, "[roam] asi: not in this build (ASIPEER=1 ./link.sh orig makes out/nfsu2_asipeer)\n"); return 1; }
int roam_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x00581475): reads_frame(c); language_texts_frame(c); sms_frame(c); mprace_frame(c); if (!roam_asipeer_frame(c)) roam_game_frame(c); racehorn_frame(c); racenet_frame(c); optslider_frame(c); drift_frame(c); photo_frame(c); roam_frame(c); return 1;
    /* Player::Update's HUD step (0x600590), behind its call of the HUD
     * update (0x4EC5A0), where the game places its own LAN race labels */
    case HOOK_SITE(0x006005e0): roam_game_hud(c); return 1;
    /* the main loop's call of World::DoTimestep (0x609CE0), its only one */
    case HOOK_SITE(0x00581407): racenet_prestep(c); return 1;
    }
    return 0;
}

/* ---- front end: from the title to free roam with a saved career -------------
 * "roam fe2roam [tmo]" presses through the screens a saved career goes
 * through (title, "Load complete.", main menu -> Career, Resume Career, the
 * crib -> Return to Explore Mode, SMS pop-ups) until the car is in free roam.
 * The screen logic is career.c's bot (feat/career), cut down to this path.
 * Active FEng packages: cFEng [0x8384C4] +8 -> manager, lists at +0xE4 and
 * +0xF4 of FNGInfo records chained through +4, name at +0xC; overlays such as
 * MC_Main.fng (memory-card prompts) are on them while GetTopPackage still
 * names the screen below. */
static uint32_t fe_mgr(void){
    uint32_t fe = rd32(0x8384c4u);
    if (!fe || !ok(fe, 11)) return 0;
    uint32_t mg = MR32(fe + 8u);
    return ok(mg, 0xf7) ? mg : 0;
}
static int fe_active(const char *want){
    uint32_t mg = fe_mgr();
    if (!mg) return 0;
    for (uint32_t head = 0xe4u; head <= 0xf4u; head += 0x10u){
        uint32_t p = MR32(mg + head);
        for (int k = 0; p && k < 64 && ok(p, 0x10); k++, p = MR32(p + 4u)){
            uint32_t n = MR32(p + 0xcu);
            if (!ok(n, 47)) continue;
            const char *q = (const char *)(g_mem + n); int i = 0;
            while (i < 47 && q[i] >= 0x20 && q[i] < 0x7f) i++;
            if (i > 4 && !q[i] && (want[0] == '*' ? strstr(q, want + 1) != 0 : !strcasecmp(q, want))) return 1;
        }
    }
    return 0;
}
static struct { char keys[16]; int nkeys; long next, top_since, last_poke, roam_since; char top[64]; } FE;
static void fe_keys(const char *k){ snprintf(FE.keys, sizeof FE.keys, "%s", k); FE.nkeys = (int)strlen(FE.keys); }
static int fe2roam(long f, long *wait_start, long tmo){
    if (*wait_start < 0){ *wait_start = f; memset(&FE, 0, sizeof FE); FE.roam_since = -1; }
    const char *top = autotest_top_fng(); if (!top) top = "";
    if (strcmp(top, FE.top)){ snprintf(FE.top, sizeof FE.top, "%s", top); FE.top_since = f; FE.last_poke = f;
        fprintf(stderr, "[roam] f=%ld fe2roam: screen %s (flow %u)\n", f, top, rd32(G_FLOW)); }
    int in = roaming() && !strcasecmp(top, "HUD_SingleRace.fng") && !fe_active("UI_EngageMessageDialog.fng");
    if (!in) FE.roam_since = -1; else if (FE.roam_since < 0) FE.roam_since = f;
    if (in && f - FE.roam_since >= 240){ fprintf(stderr, "[roam] f=%ld fe2roam: in free roam after %ld frames\n", f, f - *wait_start); *wait_start = -1; return 1; }
    if (tmo && f - *wait_start >= tmo){ fprintf(stderr, "[roam] f=%ld fe2roam TIMEOUT on %s\n", f, top); *wait_start = -1; return 1; }
    if (f < FE.next) return 0;
    if (FE.nkeys){                      /* one key every 20 frames */
        char k = FE.keys[0]; memmove(FE.keys, FE.keys + 1, sizeof FE.keys - 1); FE.nkeys--;
        autotest_tap(k == 'L' ? 0xcbu : k == 'G' ? 0xcdu : 0x1cu, 6);
        FE.next = f + (FE.nkeys ? 20 : 90); return 0;
    }
    if (fe_active("*GenericDialog")){ fe_keys("LR"); FE.next = f + 30; return 0; }
    if (fe_active("MC_Main.fng")){ fe_keys("LR"); FE.next = f + 60; return 0; }   /* memory-card prompts: the left button (yes / OK) */
    if (!strcasecmp(top, "UI_EngageMessageDialog.fng") || !strcasecmp(top, "MC_Background.fng") ||
        !strcasecmp(top, "UI_SMS_Mailbox.fng") || !strcasecmp(top, "UI_SMS_Message.fng")){ fe_keys("R"); FE.next = f + 150; return 0; }
    if (rd32(G_FLOW) == 3u){
        if (fe_active("UI_CareerCrib.fng")){ fe_keys("R"); FE.next = f + 60; return 0; }      /* Return to Explore Mode */
        if (fe_active("UI_Main.fng")){ fe_keys("LR"); FE.next = f + 240; return 0; }          /* main menu: Career */
        if (fe_active("UI_StartCareer.fng")){ fe_keys("R"); FE.next = f + 240; return 0; }    /* Resume Career */
    }
    if (!strcasecmp(top, "HUD_SingleRace.fng") && fe_active("UI_EngageMessageDialog.fng")){ fe_keys("R"); FE.next = f + 60; return 0; }
    if (!strcasecmp(top, "PC_Loading.fng") || !strcasecmp(top, "HUD_World_Loading.fng") || !strcasecmp(top, "HUD_SingleRace.fng")) return 0;
    if (f - FE.last_poke >= 600){ FE.last_poke = f; fprintf(stderr, "[roam] f=%ld fe2roam: %s for %ld frames: RETURN\n", f, top, f - FE.top_since); fe_keys("R"); }
    return 0;
}

/* ---- script commands (autotest.c) ------------------------------------------ */
/* line = "<sub> <args>".  Returns 1 when done, 0 to be called again next frame. */
int roam_cmd(CPU *c, const char *line, long *wait_start){
    char sub[32] = {0}, a1[64] = {0}, a2[64] = {0}, a3[200] = {0}; int used = 0;
    sscanf(line, "%31s %n", sub, &used);
    const char *rest = line + used;
    sscanf(rest, "%63s %63s %199s", a1, a2, a3);
    long f = autotest_frame();
    if (!strcasecmp(sub, "mp")) return roam_game_cmd(c, rest, wait_start);
    if (!strcasecmp(sub, "asi")) return roam_asipeer_cmd(c, rest, wait_start);
    g_active = 1;
    if (!strcasecmp(sub, "call") || !strcasecmp(sub, "cars") || !strcasecmp(sub, "ghost") ||
        !strcasecmp(sub, "types") || !strcasecmp(sub, "loader") || !strcasecmp(sub, "rewind") ||
        !strcasecmp(sub, "spawn") || !strcasecmp(sub, "snapshot")){ queue(line); return 1; }
    if (!strcasecmp(sub, "trace")){
        if (g_trace){ fclose(g_trace); g_trace = 0; }
        if (!strcasecmp(a1, "on") && (g_trace = fopen(a2, "w")))
            fprintf(g_trace, "frame,worldtimer,realtimer,sim_ms,host_s,wdt,px,py,pz,vx,vy,vz,cx,cy,cz,speed,rpm\n");
        fprintf(stderr, "[roam] f=%ld trace %s\n", f, g_trace ? a2 : "off");
        return 1; }
    if (!strcasecmp(sub, "shopgo")){ shop_go(a1); return 1; }
    if (!strcasecmp(sub, "racego")){ race_go(); return 1; }
    if (!strcasecmp(sub, "smsauto")){ g_smsauto = !strcasecmp(a1, "on"); return 1; }
    if (!strcasecmp(sub, "rec") && !strcasecmp(a1, "save")){     /* roam rec save FILE: CSV of the samples */
        FILE *o = fopen(a2, "w");
        if (!o){ fprintf(stderr, "[roam] rec save: cannot write %s\n", a2); return 1; }
        fprintf(o, "wt,px,py,pz,vx,vy,vz,fx,fy,fz,lx,ly,lz,ux,uy,uz,wx,wy,wz,speed,rpm,gear\n");
        for (int i = 0; i < g_nrec; i++){ const Sample *q = &g_rec[i];
            fprintf(o, "%u,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.3f,%.1f,%d\n",
                    q->wt, q->p[0], q->p[1], q->p[2], q->v[0], q->v[1], q->v[2], q->f[0], q->f[1], q->f[2], q->l[0], q->l[1], q->l[2],
                    q->u[0], q->u[1], q->u[2], q->w[0], q->w[1], q->w[2], q->spd, q->rpm, q->gear); }
        fclose(o);
        fprintf(stderr, "[roam] f=%ld rec save: %d samples -> %s\n", f, g_nrec, a2);
        return 1; }
    if (!strcasecmp(sub, "rec")){
        g_recon = !strcasecmp(a1, "on");
        if (g_recon){ g_nrec = 0; }
        fprintf(stderr, "[roam] f=%ld rec %s (%d samples)\n", f, g_recon ? "on" : "off", g_nrec);
        return 1; }
    if (!strcasecmp(sub, "carlog")){ g_carlog_every = atol(a1); g_carlog_next = f; return 1; }
    if (!strcasecmp(sub, "peek")){
        uint32_t a = eval(a1), n = a2[0] ? eval(a2) : 1;
        for (uint32_t i = 0; i < n && i < 64; i++){
            uint32_t q = a + 4u * i;
            if (!ok(q, 3)){ fprintf(stderr, "[roam] f=%ld peek [0x%08x] = <unmapped>\n", f, q); continue; }
            uint32_t v = MR32(q); float fv; memcpy(&fv, &v, 4);
            fprintf(stderr, "[roam] f=%ld peek [0x%08x] = 0x%08x (%d, %g)\n", f, q, v, (int)v, (double)fv);
        }
        return 1;
    }
    if (!strcasecmp(sub, "memdump")){
        uint32_t a = eval(a1), n = eval(a2), w = 0;
        FILE *o = fopen(a3, "wb");
        if (!o){ fprintf(stderr, "[roam] memdump: cannot write %s\n", a3); return 1; }
        for (; w < n && mem_is_committed(a + w); w++) fputc(MR8(a + w), o);
        fclose(o);
        fprintf(stderr, "[roam] f=%ld memdump 0x%08x %u bytes -> %s\n", f, a, w, a3);
        return 1;
    }
    if (!strcasecmp(sub, "wait")){
        if (*wait_start < 0) *wait_start = f;
        uint32_t v = eval(a1), want = eval(a2); long tmo = a3[0] ? atol(a3) : 0;
        if (v != want){
            if (tmo && f - *wait_start >= tmo){
                fprintf(stderr, "[roam] f=%ld wait %s==%s TIMEOUT (0x%x)\n", f, a1, a2, v); *wait_start = -1; return 1; }
            return 0; }
        fprintf(stderr, "[roam] f=%ld wait %s==%s after %ld frames\n", f, a1, a2, f - *wait_start);
        *wait_start = -1; return 1;
    }
    if (!strcasecmp(sub, "fe2roam")) return fe2roam(f, wait_start, a1[0] ? atol(a1) : 0);
    if (!strcasecmp(sub, "waitroam")){
        static long since = -1;
        long tmo = a1[0] ? atol(a1) : 0; const char *t = autotest_top_fng();
        if (*wait_start < 0){ *wait_start = f; since = -1; }
        int in = roaming() && t && !strcasecmp(t, "HUD_SingleRace.fng");
        if (!in) since = -1; else if (since < 0) since = f;
        if (in && f - since >= 240){
            fprintf(stderr, "[roam] f=%ld waitroam satisfied after %ld frames\n", f, f - *wait_start);
            *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ fprintf(stderr, "[roam] f=%ld waitroam TIMEOUT\n", f); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(sub, "teleport")){
        /* X Y Z [ANGLE]: numbers, or expressions starting with '*' / '(' that
         * give the float's bits (e.g. a car's +0x60) */
        char t[4][96] = {{0}}; float v[3] = {0}; unsigned ang = 0;
        int n = sscanf(rest, "%95s %95s %95s %95s", t[0], t[1], t[2], t[3]);
        if (n < 3){ fprintf(stderr, "[roam] teleport: X Y Z [ANGLE]\n"); return 1; }
        for (int i = 0; i < 3; i++){
            if (t[i][0] == '*' || t[i][0] == '('){ uint32_t b = eval(t[i]); memcpy(&v[i], &b, 4); }
            else v[i] = (float)atof(t[i]); }
        if (n > 3) ang = eval(t[3]);
        MRF32(G_HOTPOS) = v[0]; MRF32(G_HOTPOS + 4) = v[1]; MRF32(G_HOTPOS + 8) = v[2] + 0.5f;
        MR16(G_HOTPOS_ANGLE) = (uint16_t)ang; MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
        fprintf(stderr, "[roam] f=%ld teleport %.1f %.1f %.1f angle 0x%04x\n", f, (double)v[0], (double)v[1], (double)v[2], ang & 0xffffu);
        return 1;
    }
    if (!strcasecmp(sub, "watch")){ mprot_arm(eval(a1)); return 1; }
    fprintf(stderr, "[roam] f=%ld unknown subcommand '%s'\n", f, sub);
    return 1;
}
