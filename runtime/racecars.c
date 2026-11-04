/* racecars.c -- the other players' cars in a race between players (the game's
 * LAN races: free roam's multiplayer races and the LAN menu's), sent and shown
 * the way free roam sends and shows friends' cars.
 *
 * Why (2026-11-04): players saw the other cars in races wobble, jump, sit in
 * the wrong place and bump like walls, worse than friends' cars in free roam.
 * The game's own car states (CARSPAM, racenet.c) come 20 or 30 times a
 * second at most (14-16 from a phone at 16-27 fps), carry the sender's race
 * clock, which a joiner estimates from the creator's answers (11 ms off on
 * average, 116 ms at most in a live log: 6 m at 50 m/s), and racenet.c puts
 * each car where its newest state carries it to now, every step: 135-195 ms
 * of guessing for players 60-120 ms from the server.  Free roam's ghosts
 * (Ghost mode) are never guessed: drawn 100 ms in the past between states
 * that came, they are always smooth.
 *
 * What this does:
 *  - every game sends its own car to every other racer each frame, at most
 *    HZ a second, as free roam's car state (roam_core.c rc_state: place,
 *    velocity, orientation, spin, the controls, gear, rpm), stamped by a
 *    clock that runs with the world's steps and is steered to the wall clock
 *    (clock_frame: what the car moved by and the stamps agree, also in a
 *    world slower than real time, and a long frame is taken up over the next
 *    half second, never at once);
 *  - "N2RS" on the horn's port (racehorn.c), straight to each racer: the
 *    creator to its joiners, a joiner to the creator and to the others on the
 *    creator's list (racenet.c's N2PL).  Each also echoes the newest stamp it
 *    had from that racer and how long it held it: each pair knows its round
 *    trip without the game's race clock;
 *  - each other car runs through free roam's interpolator (rc_interp: the
 *    clock offset from the least delay, a jitter buffer, Hermite between
 *    states, corrections faded): far ones are drawn GHOST_US in the past
 *    between states that came (no guess, as free roam's ghosts), near ones
 *    where their owner has them now (half the least round trip carried on
 *    along the curve the car drives, as free roam's solid cars), the delay
 *    sliding between the two with the distance (NEAR_M .. FAR_M) at most
 *    SLEW of the step: a car that comes near is shown a little faster for a
 *    while, never jumps.  Each step it is put where it is at the step's
 *    start with the velocity and spin it moves at over the step (the game's
 *    physics moves it on after the blend);
 *  - (off since 2026-11-05, NFSU2_RACE_DRIVE=1 a test) a car within DRIVE_M
 *    runs on the game's own physics, its owner's controls in it, its velocity
 *    and yaw rate steered each step toward where the states say (DRIVE_TAU,
 *    DRIVE_TAU_R), its height, roll and pitch the physics' own: it meets walls
 *    and the player's car as a car does, and bumps both ways (a car put there
 *    every step was an immovable one to bumps the game does not call hits).
 *    Live, the cars it drove bounced, stood on their tails and flipped in
 *    every kind of race (players' reports and screenshots, 2026-11-04, the
 *    first night it was out): the rig measured place and heading only, never
 *    height, roll or pitch;
 *  - after a hit here (the game stamps one, racenet.c's sub_005f8d40) the car
 *    runs on its own physics until a state its owner sent after the hit has
 *    come, then glides back to where the states say; so one within reach
 *    drawn STALL_EXT_US past its newest state (a stalled link), with its
 *    owner's last controls.  One farther off is carried on along its way, up
 *    to CAP_FAR_US.  A correction the interpolator would snap (over 5 m) glides
 *    too;
 *  - a car of an older game (none of these come for it) keeps racenet.c's
 *    way, and an older game drops these (an unknown message on the port).
 *
 * N2RS, version 1 (little-endian):
 *   0 "N2RS", 4 version 1, 5 flags (1: states with the extension), 6 the
 *   sender's race slot, 7 n states (1..NSTATES, newest first), 8 epoch (a
 *   change: its car was put elsewhere, snap), 9 clock epoch (a change: its
 *   stamp clock was set anew, re-sync), 10 status (roam_core.h RC_ST_*), 11 0,
 *   12 its wall clock now (u32 µs), 16 the newest such stamp it had from the
 *   addressee (0: none), 20 how long it held that one (u32 µs), 24 the states
 *   (rc_state_put).
 * Who may speak for whom (as racenet.c's N2CS): only an address on the
 * race's peers (racenet_race_peers), and each address for one slot only, the
 * first it named in this race, which no other address may name.
 *
 * Test knobs (read only with NFSU2_INPUT_SCRIPT set, racenet.c's test_env):
 * NFSU2_RACE_CARS=0 off (the game's and racenet.c's way), NFSU2_RACE_CARS_HZ,
 * NFSU2_RACE_NEAR_M / _FAR_M, NFSU2_RACE_GHOST_MS, NFSU2_RACE_LEAD_MS,
 * NFSU2_RACE_DRIVE=1 (near cars driven, above; 0 the default: all put), NFSU2_RACE_DRIVE_M /
 * _DRIVE_TAU_MS / _DRIVE_TAUR_MS, NFSU2_RACECARS_TRACE=1 (lines on what
 * moved a car: "[rctrace]").
 *
 * Measured on the rig (web/tools/lan-pair.mjs, three games, every player's
 * way to the server 70 +- 15 ms with 500 ms stalls, the AI driving with a
 * swerve every 5-9 s; tools/race_acc.py): see the commit.
 */
#include "cpu.h"
#include "roam_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define G_FLOW       0x008654a4u   /* 6: in the world */
#define G_NETMODE    0x00850078u   /* 8 Online, 0x200 LAN */
#define G_WORLD      0x00890080u   /* World*: +0 the cars' list */
#define G_PLAYER1    SPLIT_PLAYERS_INDEX   /* Player*: +4 car */
#define G_WORLDDT    0x00865154u   /* float: this step's world dt (s) */
#define G_RACE       0x00890118u   /* the race coordinator*: +0 state */
#define RC_RACING    4u
#define G_NETPLAYERS 0x0089cf50u   /* the race's players by slot: 6 pointers, name at +0x12 (a car's slot: +0xA) */
#define CAR_SLOT     0x0au         /* a car: int16, its player's race slot (0x5F9840: the players by slot) */
#define CAR_CTL      0x30u         /* a car: its input (+0x20C throttle, +0x210 handbrake, +0x214 brake, u16 +0x1E0 steer) */
#define CAR_POS      0x60u
#define CAR_VEL      0x70u
#define CAR_ROWS     0x90u         /* the orientation's rows: forward, left (+0xA0), up (+0xB0) */
#define CAR_SPIN     0x110u
#define CAR_SPEED    0x42cu
#define CAR_GEAR     0x434u
#define CAR_RPM      0x440u
#define CTL_CAR      0x10u         /* a remote car's controller: its world car */
#define CTL_PREV_V   0x140u
#define CTL_HIT      0x184u        /*   world time (1/4000 s) of the last hit it saw */
#define CTL_BLEND    0xc0u         /*   the blend's state: +0x10 place, +0x20 velocity, +0x30 orientation, +0x40 spin */
#define FN_APPLY     0x005f8810u   /* the blend's state into the car (thiscall: the state, the car) */
#define RET_APPLY    0x005f8cf7u
#define FN_DRIVEN    0x005a2a60u   /* a car of kind 3 driven by its states */
#define RET_DRIVEN   0x005f8d0cu
#define HORN_PORT    3659u

#define HZ           60            /* our car's states a second at most */
#define NSTATES      1             /* states a message (newest first: over a WebSocket nothing is lost) */
#define NCARS        8
#define NPEERS       8
#define STALE_US     1500000       /* no state for this long: the car goes back to racenet.c's way */
#define STALL_EXT_US 1200000       /* a car within reach drawn this far past its newest state: on its own physics */
#define CAP_US       1500000       /* carried on this far past its newest state at most (within reach) */
#define CAP_FAR_US   1500000       /* (farther off: then it waits) */
#define GHOST_US     100000        /* a far car's delay (free roam's ghosts': NFSU2_ROAM_DELAY) */
#define NEAR_M       40.f          /* a car this near: where its owner has it now */
#define FAR_M        120.f         /* this far: GHOST_US in the past */
#define SLEW         0.10          /* the delay changes at most this much of the step */
#define ONEWAY_MAX   400000        /* µs: the most a near car is carried on for the way (200 drew players 250 ms
                                      apart 45 ms behind where they were) */
#define SNAP_M       25.f          /* a car back from its own physics this far from its states: put there */
#define GLIDE_V      25.f          /* m/s: else it glides back about this fast, in a time constant ... */
#define GLIDE_TAU_MIN 0.08f        /* ... between these (s) */
#define GLIDE_TAU_MAX 0.35f
#define JUMP_M       15.f          /* our car this far off its own move in a frame: put elsewhere (a reset) */
/* driven (NFSU2_RACE_DRIVE=1, a test, off by default: live, driven cars bounced and flipped): a car this near runs on the game's own physics, its velocity and yaw rate
 * set each step toward where the states say (the error over DRIVE_TAU / DRIVE_TAU_R), its height, roll and pitch
 * the physics' own; an error over DRIVE_SNAP_M is put right at once */
#define DRIVE_M      60.f
#define DRIVE_TAU    0.20f
#define DRIVE_TAU_R  0.15f
#define DRIVE_SNAP_M 6.f
#define DRIVE_VMAX   12.f          /* m/s: the most the correction adds to the velocity */
#define NRTT         8
#define BIG_M        5.f           /* a car put this far from where it was: a log line (BIG_LINES a race at most) */
#define BIG_LINES    12
#define K_TAU        0.5           /* s: our stamp clock goes to the wall clock with this time constant ... */
#define K_TAU_I      2.0           /*   and learns how fast the world drifts over this */
#define K_RMIN       (-0.3)        /*   at most this much slower than the world's step ... */
#define K_RMAX       0.5           /*   or faster */
#define K_RSLEW      0.5           /*   its rate changing at most this much a second */

int vlan_rt_send(uint16_t port, uint32_t ip_h, const void *p, int n);
int racenet_race_peers(uint32_t *ip_h, int max);   /* racenet.c: the other racers' addresses */

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static void rd3(uint32_t a, float *o){ o[0] = MRF32(a); o[1] = MRF32(a + 4u); o[2] = MRF32(a + 8u); }
static int64_t now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)ts.tv_nsec / 1000;
}
static float clampf(float x, float lo, float hi){ return x < lo ? lo : x > hi ? hi : x; }
static int finite_n(const float *a, int n){ for (int i = 0; i < n; i++) if (!__builtin_isfinite(a[i])) return 0; return 1; }
static float len3(const float *a){ return __builtin_sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static void rq_mul(const float *a, const float *b, float *o){   /* o = a * b, (x, y, z, w) */
    float x = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    float y = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    float z = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    float w = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
static void rq_conj(const float *a, float *o){ o[0] = -a[0]; o[1] = -a[1]; o[2] = -a[2]; o[3] = a[3]; }
static void rq_norm(float *q){
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n > 1e-12f) || !__builtin_isfinite(n)){ q[0] = q[1] = q[2] = 0.f; q[3] = 1.f; return; }
    float k = 1.f / __builtin_sqrtf(n);
    for (int i = 0; i < 4; i++) q[i] *= k;
}
static void rq_nlerp(const float *a, const float *b, float t, float *o){
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3], s = d < 0.f ? -1.f : 1.f;
    for (int i = 0; i < 4; i++) o[i] = (1.f - t) * a[i] + t * s * b[i];
    rq_norm(o);
}
static const char *test_env(const char *k){
    const char *s = getenv("NFSU2_INPUT_SCRIPT");
    return s && *s ? getenv(k) : 0;
}
static double knob(const char *k, double def, double lo, double hi){
    const char *e = test_env(k);
    double v = e && *e ? atof(e) : def;
    return v >= lo && v <= hi ? v : def;
}
static struct { int init, on, hz; float near_m, far_m; int64_t ghost_us, lead_us; int drive; float drive_m, drive_tau, drive_tau_r; int trace; } O;
static void opts(void){
    if (O.init) return;
    O.init = 1;
    const char *e = getenv("NFSU2_RACE_SMOOTH");                  /* racenet.c's: the game's own way, all of it */
    O.on = !(e && e[0] == '0') && knob("NFSU2_RACE_CARS", 1, 0, 1) != 0;
    O.hz = (int)knob("NFSU2_RACE_CARS_HZ", HZ, 1, 240);
    O.near_m = (float)knob("NFSU2_RACE_NEAR_M", NEAR_M, 0, 10000);
    O.far_m = (float)knob("NFSU2_RACE_FAR_M", FAR_M, 0, 10000);
    if (O.far_m < O.near_m + 1.f) O.far_m = O.near_m + 1.f;
    O.ghost_us = (int64_t)(knob("NFSU2_RACE_GHOST_MS", GHOST_US / 1000, -500, 1000) * 1000.0);
    O.lead_us = (int64_t)(knob("NFSU2_RACE_LEAD_MS", 0, -500, 500) * 1000.0);
    O.drive = (int)knob("NFSU2_RACE_DRIVE", 0, 0, 1);
    O.trace = (int)knob("NFSU2_RACECARS_TRACE", 0, 0, 1);
    O.drive_m = (float)knob("NFSU2_RACE_DRIVE_M", DRIVE_M, 0, 10000);
    O.drive_tau = (float)knob("NFSU2_RACE_DRIVE_TAU_MS", DRIVE_TAU * 1000.f, 20, 2000) / 1000.f;
    O.drive_tau_r = (float)knob("NFSU2_RACE_DRIVE_TAUR_MS", DRIVE_TAU_R * 1000.f, 20, 2000) / 1000.f;
}
int racecars_on(void){ opts(); return O.on; }

static int in_race(void){
    uint32_t w = rd32(G_WORLD), rc = rd32(G_RACE);
    return rd32(G_FLOW) == 6u && (rd32(G_NETMODE) & 0x208u) && ok(w, 0x80) && rd32(rc) == RC_RACING;
}
static uint32_t my_car(void){ uint32_t pl = rd32(G_PLAYER1); return pl ? rd32(pl + 4u) : 0; }
/* the car is on the world's list */
static int in_world(uint32_t w, uint32_t car){
    uint32_t x = rd32(w);
    for (int k = 0; x && x != w && k < 64 && ok(x, 0x9e0); k++, x = rd32(x)) if (x == car) return 1;
    return 0;
}
/* the world's car of a race slot (not ours) */
static uint32_t car_of_slot(int slot){
    uint32_t w = rd32(G_WORLD), me = my_car(), x = rd32(w);
    for (int k = 0; x && x != w && k < 64 && ok(x, 0x9e0); k++, x = rd32(x))
        if (x != me && (int16_t)MR16(x + CAR_SLOT) == slot) return x;
    return 0;
}
static void car_name(uint32_t car, char *o){
    int i = ok(car, 0x9e0) ? (int16_t)MR16(car + CAR_SLOT) : -1;
    uint32_t pl = i >= 0 && i < 6 ? rd32(G_NETPLAYERS + 4u * (uint32_t)i) : 0;
    int n = 0;
    if (ok(pl, 0x12 + 16))
        for (; n < 15; n++){ uint8_t ch = MR8(pl + 0x12u + (uint32_t)n); if (ch < 0x20 || ch > 0x7e) break; o[n] = (char)ch; }
    o[n] = 0;
}

/* ---- our stamp clock (free roam's g_sync, roam_game.c) ---- */
static struct {
    int on; double s, vscale, irate, rate; int64_t host;   /* the clock (µs), world seconds per stamp second, its drift
                                                        learnt, its rate now (+ = faster than the world's steps),
                                                        the wall clock then */
    uint8_t clock, epoch;                         /* its epoch (set anew), our car's (put elsewhere) */
    int paused;
    float lp[3], lv[3]; int lp_ok;                /* our car last frame */
    double sent; uint32_t nsent, ndg;             /* the last state sent (stamp), how many */
    uint32_t snaps;
} K;
static void clock_frame(int racing){
    int64_t host = now_us(), hdt = host - K.host;
    if (hdt < 0 || hdt > 1000000) hdt = 0;
    float wdt = MRF32(G_WORLDDT);
    if (!__builtin_isfinite(wdt) || wdt < 0.f || wdt > 1.f) wdt = 0.f;
    if (!racing){ K.on = 0; K.host = host; return; }
    if (!K.on){ K.on = 1; K.s = (double)host; K.vscale = 1.0; K.irate = 0; K.rate = 0; K.clock++; K.paused = 0; K.lp_ok = 0; }
    else if (wdt <= 0.f){ K.paused++; K.s += (double)hdt; }
    else {
        /* the world's step, sped up or slowed down by what it is off the wall clock (a PI: K_TAU, and what it
         * keeps drifting by over K_TAU_I), at most K_RMIN .. K_RMAX of the step.  Not pulled at once as free
         * roam's clock is: a frame of 300 ms whose world step the game cut to 50 put that clock 250 ms on in one
         * frame, so the car's states said it stood still for that long, and every other car drawn from them
         * stepped (one frame in nine on a busy machine).  A world steadily slower than real time (the game's
         * step cut below 20 fps) is followed (K_RMAX: down to 2/3 of real time) */
        double e = ((double)host - (K.s + wdt * 1e6)) * 1e-6;
        if (__builtin_fabs(e) > 1.0){ K.s = (double)host; K.clock++; K.snaps++; K.irate = 0; K.rate = 0; K.vscale = 1.0; }
        else {
            /* the rate moves at most K_RSLEW a second: a frame's wall time swings by a pacer's step (16.7 ms at 30
             * fps) against its world step, and a rate that followed each frame's error moved every car drawn by
             * the clock 3 % back and forth a frame (5 cm at 50 m/s, a phone's screen) */
            K.irate += e * wdt / (K_TAU * K_TAU_I);
            K.irate = K.irate < K_RMIN ? K_RMIN : K.irate > K_RMAX ? K_RMAX : K.irate;
            double want = e / K_TAU + K.irate, lim = K_RSLEW * wdt;
            want = want < K_RMIN ? K_RMIN : want > K_RMAX ? K_RMAX : want;
            K.rate += want - K.rate > lim ? lim : want - K.rate < -lim ? -lim : want - K.rate;
            K.s += wdt * 1e6 * (1.0 + K.rate);
            K.vscale = 1.0 / (1.0 + K.rate);
        }
        K.paused = 0;
    }
    K.host = host;
}

/* ---- the other racers ---- */
typedef struct {
    uint32_t ip; int slot;                        /* its address, the slot it speaks for (-1: none yet) */
    uint32_t car;                                 /* its world car (by the slot) */
    char name[16];
    rc_interp mo;
    int64_t rx_at;                                /* our wall clock at its newest state */
    uint32_t their_tx; int64_t their_at;          /* its newest wall stamp, when it came (our echo) */
    int32_t rtt[NRTT]; int rn, rh;                /* round trips (µs) */
    int64_t delay; int dinit;                     /* the delay it is drawn with (µs; < 0: carried on) */
    int64_t last_eval; int evals;                 /* the stamp time of the last step it was drawn for */
    uint32_t hit; int own; double own_from;       /* the game's last hit stamp; on its own physics since (stamp) */
    int stalled;
    float gp[3], gq[4], gtau; int glide;          /* the glide back from its own physics: the offset, its time constant */
    float sp[3], sv[3], sq[4], sw[3];             /* the car before the game's blend this step (racecars_pre) */
    int driving; uint32_t drive_steps, drive_snaps;   /* on its own physics, steered (NFSU2_RACE_DRIVE) */
    uint32_t vfit;                                /* steps whose velocity was the drawn car's over the step */
    uint32_t bigs;                                /* steps it was put over BIG_M from where it was */
    float tp[3], tm[3]; float th; int thave;      /* NFSU2_RACECARS_TRACE: the place drawn last step, its move, the step */
    float putp[3], putv[3]; int puts;             /*   where we put it this pass, its velocity then, how often */
    float dist;
    /* for the log */
    uint32_t n, g250, g500; int64_t gap_max;
    uint32_t steps, own_hit, own_stall, glides, snaps, near_steps, far_steps;
    float glide_max;
    double age_sum; uint32_t age_n; float age_max;
} Rc;
static Rc g_rc[NCARS];
static uint32_t g_world;
static struct { uint32_t refused, bad, sent_err; } S;

static Rc *rc_of_ip(uint32_t ip, int add){
    for (int i = 0; i < NCARS; i++) if (g_rc[i].ip == ip) return &g_rc[i];
    if (!add) return 0;
    for (int i = 0; i < NCARS; i++) if (!g_rc[i].ip){
        memset(&g_rc[i], 0, sizeof g_rc[i]);
        g_rc[i].ip = ip; g_rc[i].slot = -1; g_rc[i].gq[3] = 1.f;
        rc_interp_reset(&g_rc[i].mo);
        return &g_rc[i];
    }
    return 0;
}
/* the racer whose car this is, while its states come */
static Rc *rc_of_car(uint32_t car){
    if (!car) return 0;
    int64_t t = now_us();
    for (int i = 0; i < NCARS; i++){
        Rc *r = &g_rc[i];
        if (!r->ip || r->slot < 0 || !r->mo.n) continue;
        if (!r->car || !in_world(rd32(G_WORLD), r->car)) r->car = car_of_slot(r->slot);
        if (r->car == car) return t - r->rx_at <= STALE_US ? r : 0;
    }
    return 0;
}
int racecars_fed(uint32_t car){ return racecars_on() && in_race() && rc_of_car(car) != 0; }
static int32_t rtt_min(const Rc *r){
    int32_t m = 0;
    for (int k = 0; k < r->rn; k++) if (k == 0 || r->rtt[k] < m) m = r->rtt[k];
    return m;
}

/* ---- our car to the others (every frame, after the world's step) ---- */
static int own_state(rc_state *s, uint32_t car){
    memset(s, 0, sizeof *s);
    float f[3], l[3], u[3];
    s->t_us = (uint32_t)(uint64_t)(int64_t)K.s;
    rd3(car + CAR_POS, s->p); rd3(car + CAR_VEL, s->v); rd3(car + CAR_SPIN, s->w);
    rd3(car + CAR_ROWS, f); rd3(car + CAR_ROWS + 0x10u, l); rd3(car + CAR_ROWS + 0x20u, u);
    rc_m2q(f, l, u, s->q);
    s->speed = MRF32(car + CAR_SPEED); s->rpm = MRF32(car + CAR_RPM); s->gear = (int8_t)MR32(car + CAR_GEAR);
    uint32_t ctl = rd32(car + CAR_CTL);
    if (ok(ctl, 0x220)){
        s->throttle = MRF32(ctl + 0x20cu); s->brake = MRF32(ctl + 0x214u);
        s->steer = (float)(int16_t)MR16(ctl + 0x1e0u) / 32767.f;
        if (s->brake > 0.1f) s->bits |= RC_BIT_BRAKE;
        if (MRF32(ctl + 0x210u) > 0.1f) s->bits |= RC_BIT_HANDBRAKE;
    }
    if (s->speed < -0.5f) s->bits |= RC_BIT_REVERSE;
    if (!finite_n(s->p, 3) || !finite_n(s->v, 3) || !finite_n(s->q, 4)) return 0;
    if (K.paused > 1){ memset(s->v, 0, sizeof s->v); memset(s->w, 0, sizeof s->w); s->speed = 0; }
    /* velocities per stamp second (the stamp clock may run a little faster than the world) */
    for (int i = 0; i < 3; i++){ s->v[i] = (float)(s->v[i] * K.vscale); s->w[i] = (float)(s->w[i] * K.vscale); }
    if (!finite_n(s->w, 3)) memset(s->w, 0, sizeof s->w);
    if (!__builtin_isfinite(s->speed)) s->speed = 0;
    if (!__builtin_isfinite(s->rpm)) s->rpm = 0;
    if (!__builtin_isfinite(s->throttle)) s->throttle = 0;
    if (!__builtin_isfinite(s->brake)) s->brake = 0;
    if (!__builtin_isfinite(s->steer)) s->steer = 0;
    return 1;
}
static void send_frame(void){
    uint32_t car = my_car();
    if (!ok(car, 0x9e0)) return;
    float wdt = MRF32(G_WORLDDT);
    float p[3]; rd3(car + CAR_POS, p);
    /* put elsewhere (the game's reset): a new epoch, the others snap instead of gliding */
    if (K.lp_ok && wdt > 0.f && wdt < 0.25f && finite_n(p, 3)){
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = p[i] - (K.lp[i] + K.lv[i] * wdt);
        if (len3(d) > JUMP_M) K.epoch++;
    }
    memcpy(K.lp, p, sizeof p); rd3(car + CAR_VEL, K.lv); K.lp_ok = finite_n(K.lp, 3) && finite_n(K.lv, 3);
    if (K.s - K.sent < 1e6 / O.hz - 2000.0 && K.sent > 0) return;
    rc_state s;
    if (!own_state(&s, car)) return;
    uint32_t peers[NPEERS];
    int np = racenet_race_peers(peers, NPEERS);
    if (np <= 0) return;
    K.sent = K.s; K.nsent++;
    int slot = (int16_t)MR16(car + CAR_SLOT);              /* as the others find our car (the game's G_MYSLOT is not the creator's own) */
    if (slot < 0 || slot >= 6) return;
    int64_t t = now_us();
    for (int k = 0; k < np; k++){
        uint8_t m[24 + NSTATES * (RC_STATE_BYTES + RC_STATEX_BYTES)];
        rc_w w; rc_w_init(&w, m, sizeof m);
        rc_putb(&w, "N2RS", 4); rc_put8(&w, 1); rc_put8(&w, 0); rc_put8(&w, (uint32_t)slot); rc_put8(&w, 1);
        rc_put8(&w, K.epoch); rc_put8(&w, K.clock); rc_put8(&w, K.paused > 1 ? RC_ST_PAUSED : RC_ST_ROAM); rc_put8(&w, 0);
        rc_put32(&w, (uint32_t)t);
        Rc *r = rc_of_ip(peers[k], 0);
        int fresh = r && r->their_at && t - r->their_at < 2000000;
        rc_put32(&w, fresh ? r->their_tx : 0u);
        rc_put32(&w, fresh ? (uint32_t)(t - r->their_at) : 0u);
        rc_state_put(&w, &s, 0);
        if (w.err) continue;
        if (vlan_rt_send(HORN_PORT, peers[k], m, (int)w.n)) S.sent_err++;
        else K.ndg++;
    }
}

/* ---- an N2RS from the horn's port (racehorn.c, the game thread) ---- */
void racecars_heard(const uint8_t *m, int n, uint32_t from){
    if (!racecars_on() || n < 24 || m[4] != 1 || !in_race()) return;
    uint32_t peers[NPEERS];
    int np = racenet_race_peers(peers, NPEERS), known = 0;
    for (int k = 0; k < np; k++) known |= peers[k] == from;
    int slot = m[6], ns = m[7];
    uint32_t me = my_car();
    if (!known || slot >= 6 || !ok(me, 0x9e0) || slot == (int16_t)MR16(me + CAR_SLOT) || ns < 1 || ns > 3){ S.refused++; return; }
    /* one slot an address, one address a slot, for this race */
    for (int i = 0; i < NCARS; i++) if (g_rc[i].ip && g_rc[i].ip != from && g_rc[i].slot == slot){ S.refused++; return; }
    Rc *r = rc_of_ip(from, 1);
    if (!r){ S.refused++; return; }
    if (r->slot >= 0 && r->slot != slot){ S.refused++; return; }
    rc_r rd; rc_r_init(&rd, m + 24, (uint32_t)n - 24u);
    rc_state st[3];
    for (int k = 0; k < ns; k++) if (rc_state_get(&rd, &st[k], m[5] & 1) != RC_OK){ S.bad++; return; }
    if (r->slot < 0){ r->slot = slot; r->car = car_of_slot(slot); if (r->car) car_name(r->car, r->name); }
    int64_t t = now_us();
    uint32_t tx = (uint32_t)m[12] | (uint32_t)m[13] << 8 | (uint32_t)m[14] << 16 | (uint32_t)m[15] << 24;
    uint32_t echo = (uint32_t)m[16] | (uint32_t)m[17] << 8 | (uint32_t)m[18] << 16 | (uint32_t)m[19] << 24;
    uint32_t held = (uint32_t)m[20] | (uint32_t)m[21] << 8 | (uint32_t)m[22] << 16 | (uint32_t)m[23] << 24;
    if (echo){
        int32_t rtt = (int32_t)((uint32_t)t - echo) - (int32_t)held;
        if (rtt >= 0 && rtt < 5000000){ r->rtt[r->rh] = rtt; r->rh = (r->rh + 1) % NRTT; if (r->rn < NRTT) r->rn++; }
    }
    r->their_tx = tx; r->their_at = t;
    if (r->n){
        int64_t g = t - r->rx_at;
        if (g > r->gap_max) r->gap_max = g;
        r->g250 += g > 250000; r->g500 += g > 500000;
    }
    r->n++; r->rx_at = t;
    /* the stamp clock's time now, for the interpolator's offset (the newest state only) */
    for (int k = ns - 1; k >= 0; k--) rc_interp_insert(&r->mo, &st[k], m[8], m[9], m[10], (int64_t)K.s + (t - K.host), k == 0);
}

/* ---- the car drawn (racenet.c's override of the game's blend, after it, each world step) ---- */
static void call_game(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg, uint32_t ret){
    CPU save = *c;
    c->esp = (c->esp - 512u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(ret);
    dispatch_indirect(c, fn);
    *c = save;
}
/* the car put where p, v, q, w say, as the game's blend puts it (0x5F8C98..0x5F8D07) */
static void put_car(CPU *c, uint32_t ctl, uint32_t car, const float *p, const float *v, const float *q, const float *w){
    uint32_t b = ctl + CTL_BLEND;
    for (uint32_t k = 0; k < 3; k++){ MRF32(b + 0x10u + 4u * k) = p[k]; MRF32(b + 0x20u + 4u * k) = v[k]; MRF32(b + 0x40u + 4u * k) = w[k]; }
    for (uint32_t k = 0; k < 4; k++) MRF32(b + 0x30u + 4u * k) = q[k];
    uint32_t a = car;
    call_game(c, FN_APPLY, b, 1, &a, RET_APPLY);
}
static void car_now(uint32_t car, float *p, float *v, float *q, float *w){
    float f[3], l[3], u[3];
    rd3(car + CAR_POS, p); rd3(car + CAR_VEL, v); rd3(car + CAR_SPIN, w);
    rd3(car + CAR_ROWS, f); rd3(car + CAR_ROWS + 0x10u, l); rd3(car + CAR_ROWS + 0x20u, u);
    rc_m2q(f, l, u, q);
}
static void controls(uint32_t car, const rc_pose *o){
    uint32_t in = rd32(car + CAR_CTL);
    if (!ok(in, 0x218u)) return;
    MRF32(in + 0x20cu) = clampf(o->throttle, 0.f, 1.f); MRF32(in + 0x214u) = clampf(o->brake, 0.f, 1.f);
    MRF32(in + 0x210u) = o->bits & RC_BIT_HANDBRAKE ? 1.f : 0.f;
    MR16(in + 0x1e0u) = (uint16_t)(int16_t)(clampf(o->steer, -1.f, 1.f) * 32767.f);
}
/* racenet.c's sub_005f8af0, before the game's blend: 1 when the car is ours
 * to draw (its states come), its place, velocity, orientation and spin kept
 * (put back while it runs on its own) */
int racecars_pre(uint32_t car){
    if (!racecars_on() || !in_race() || !ok(car, 0x9e0)) return 0;
    Rc *r = rc_of_car(car);
    if (!r) return 0;
    car_now(car, r->sp, r->sv, r->sq, r->sw);
    return 1;
}
/* thiscall (the controller) from racenet.c's sub_005f8af0, after the game's
 * blend (when racecars_pre said 1): 1 when the car was drawn here, 0 for
 * racenet.c's way */
int racecars_step(CPU *c, uint32_t ctl, uint32_t car){
    if (!racecars_on() || !in_race()) return 0;
    Rc *r = rc_of_car(car);
    if (!r || !ok(ctl + CTL_BLEND, 0x50u) || !ok(car, 0x9e0)) return 0;
    const float *sp = r->sp, *sv = r->sv, *sq = r->sq, *sw = r->sw;
    if (!r->name[0]) car_name(car, r->name);
    float h = MRF32(G_WORLDDT);
    if (!(h > 0.f && h < 0.25f)) h = 0.f;
    /* the step's start on our stamp clock: the game moves the car by the velocity put here after this (its
     * physics comes after the blend), to the step's end.  Put at the end (a step ahead), every frame moved by twice
     * its step less the one before: a 50 ms step after a 16.7 ms one jumped 1 m at 30 m/s, a 16.7 ms after a 33 ms
     * one stood (the rig, on a busy machine) */
    double te = K.s + (double)O.lead_us;
    int64_t fdt = r->evals ? (int64_t)(te - (double)r->last_eval) : 0;
    if (fdt < 0) fdt = 0;
    r->last_eval = (int64_t)te; r->evals++;
    /* the delay by the distance from our car */
    uint32_t me = my_car();
    float mp[3] = {0}, cp[3];
    if (ok(me, 0x9e0)) rd3(me + CAR_POS, mp);
    rd3(car + CAR_POS, cp);
    float d[3] = { cp[0] - mp[0], cp[1] - mp[1], cp[2] - mp[2] }, dist = len3(d);
    if (!__builtin_isfinite(dist)) dist = 0.f;
    r->dist = dist;
    int32_t way = rtt_min(r) / 2;
    if (way > ONEWAY_MAX) way = ONEWAY_MAX;
    float u = clampf((dist - O.near_m) / (O.far_m - O.near_m), 0.f, 1.f);
    int64_t want = (int64_t)((1.f - u) * (float)(-way) + u * (float)O.ghost_us);
    if (!r->dinit){ r->delay = want; r->dinit = 1; }
    int64_t delay0 = r->delay;
    {
        int64_t lim = (int64_t)((double)fdt * SLEW) + 1, dd = want - r->delay;
        r->delay += dd > lim ? lim : dd < -lim ? -lim : dd;
    }
    if (u < 0.5f) r->near_steps++; else r->far_steps++;
    /* the newest state's age by our clock (its stamp + the offset - the way) */
    const rc_interp *mo = &r->mo;
    double newest = mo->n && mo->off_init ? (double)mo->t[mo->n - 1] + (double)mo->off_r - (double)way : te;
    double age = te - newest;
    if (mo->n && mo->off_init){ r->age_sum += age; r->age_n++; if ((float)age > r->age_max) r->age_max = (float)age; }
    /* a hit here: on its own physics until a state from after it */
    uint32_t hit = rd32(ctl + CTL_HIT);
    if (r->evals == 1) r->hit = hit;                  /* (one from before we drew it is not ours) */
    if (hit && hit != r->hit){ r->hit = hit; if (!r->own){ r->own = 1; r->own_from = te; r->own_hit++; } }
    /* a stall: a car is carried on along its way (a driven one follows that way on its own physics: a wall still
     * stops it); only one within reach drawn STALL_EXT_US past its newest state runs on its own physics with its
     * owner's last controls (as the game ran one with no prediction left).  At 400 ms that was 26-30 times a car
     * a race on the rig's bad link (220 ms a way, 600 ms stalls), each ending in a glide back of up to 23 m; carried
     * on, as racenet.c's way does, the error stayed under 5 m.  A far car's physics with no states wandered off
     * (3 m a frame on the rig): never on its own */
    int reach = dist < O.drive_m;
    double ext = mo->n && mo->off_init ? te - (double)mo->off_r - (double)r->delay - (double)mo->t[mo->n - 1] : 0.0;
    int stall = reach && ext > (double)STALL_EXT_US;
    if (stall && !r->own){ r->own = 1; r->own_from = te; r->own_stall++; r->stalled = 1; }
    if (r->own && !stall && newest >= r->own_from){
        /* back: glide from where the car is to where the states say.  The interpolator starts its own correction
         * anew (what it held from before the car ran on its own faded at another rate than the glide: the two
         * pulled the car a metre a step apart, 2-11 m "jumps" on the rig) */
        r->own = 0; r->stalled = 0; r->glide = 1; r->mo.shown = 0;
    }
    if (r->own){
        /* the game's blend may have moved it: back to its own physics' place */
        float p[3], v[3], q[4], w[3];
        car_now(car, p, v, q, w);
        int moved = 0;
        for (int i = 0; i < 3; i++) moved |= p[i] != sp[i];
        if (moved) put_car(c, ctl, car, sp, sv, sq, sw);
        rc_pose last; rc_interp_peek(&r->mo, mo->n ? mo->t[mo->n - 1] : 0, 0, &last);
        controls(car, &last);
        for (uint32_t k = 0; k < 3; k++) MRF32(ctl + CTL_PREV_V + 4u * k) = MRF32(car + CAR_VEL + 4u * k);
        return 1;
    }
    rc_pose o;
    int64_t off0 = r->mo.off_r;
    uint64_t sn0 = r->mo.n_snap;
    if (!rc_interp_eval(&r->mo, (int64_t)te, fdt, r->delay, reach ? CAP_US : CAP_FAR_US, &o)) return 0;
    /* a correction too big for the interpolator's fade (over 5 m, or its clock offset set anew): it put the car
     * there at once; glide from where it was instead */
    if (r->mo.n_snap != sn0 && r->steps) r->glide = 1;
    if (r->glide){
        r->glide = 0;
        float ic[4], e;
        for (int i = 0; i < 3; i++) r->gp[i] = sp[i] - o.p[i];
        rq_conj(o.q, ic); rq_mul(sq, ic, r->gq); rq_norm(r->gq);
        e = len3(r->gp);
        if (!finite_n(r->gp, 3) || e > SNAP_M){ memset(r->gp, 0, sizeof r->gp); r->gq[0] = r->gq[1] = r->gq[2] = 0.f; r->gq[3] = 1.f; r->snaps++; }
        else { r->gtau = clampf(e / GLIDE_V, GLIDE_TAU_MIN, GLIDE_TAU_MAX); r->glides++; if (e > r->glide_max) r->glide_max = e; }
    }
    if (h > 0.f){
        static const float id[4] = { 0.f, 0.f, 0.f, 1.f };
        float k = __builtin_expf(-h / (r->gtau > 0.01f ? r->gtau : GLIDE_TAU_MIN)), t4[4];
        for (int i = 0; i < 3; i++) r->gp[i] *= k;
        rq_nlerp(id, r->gq, k, t4); memcpy(r->gq, t4, sizeof t4);
    }
    float op[3], oq[4], ov[3], ow[3], vs = (float)(K.vscale > 0.05 ? K.vscale : 1.0);
    for (int i = 0; i < 3; i++){ op[i] = o.p[i] + r->gp[i]; ov[i] = o.v[i] / vs; ow[i] = o.w[i] / vs; }
    rq_mul(r->gq, o.q, oq); rq_norm(oq);
    if (!finite_n(op, 3) || !finite_n(oq, 4) || !finite_n(ov, 3) || !finite_n(ow, 3)) return 0;
    /* The velocity and spin the game moves the car by after this, to the step's end: how far the drawn car goes
     * over this step (the interpolator at the next step's time, the delay and the offset sliding on as they did
     * this step, the corrections fading on).  The states' own velocity is not that while the delay or the offset
     * slide (up to 15 %) or a correction fades, and on uneven steps (16.7 / 33 ms) the difference was 10-16 cm of
     * wobble a frame on the rig, more where the drawing changed from between states to carried on */
    if (h > 0.f){
        double dte = (double)h * 1e6 / vs;
        int64_t rt = (int64_t)te - r->mo.off_r - r->delay;
        int64_t rt2 = rt + (int64_t)dte - (r->mo.off_r - off0) - (r->delay - delay0);
        rc_pose n2;
        if (rc_interp_peek(&r->mo, rt2, CAP_US, &n2)){
            static const float id[4] = { 0.f, 0.f, 0.f, 1.f };
            float ke = rc_expneg((float)dte / 100000.f), kg = __builtin_expf(-h / (r->gtau > 0.01f ? r->gtau : GLIDE_TAU_MIN));
            float p2[3], v2[3], eq2[4], gq2[4], q1[4], q2[4], ic[4], dq[4];
            for (int i = 0; i < 3; i++){ p2[i] = n2.p[i] + r->mo.err_p[i] * ke + r->gp[i] * kg; v2[i] = (p2[i] - op[i]) / h; }
            rq_nlerp(id, r->mo.err_q, ke, eq2); rq_nlerp(id, r->gq, kg, gq2);
            rq_mul(eq2, n2.q, q1); rq_mul(gq2, q1, q2); rq_norm(q2);
            rq_conj(oq, ic); rq_mul(q2, ic, dq); rq_norm(dq);
            if (dq[3] < 0.f) for (int i = 0; i < 4; i++) dq[i] = -dq[i];
            float vn = __builtin_sqrtf(dq[0] * dq[0] + dq[1] * dq[1] + dq[2] * dq[2]), ang = 2.f * __builtin_atan2f(vn, dq[3]);
            if (finite_n(v2, 3) && len3(v2) < 150.f){ memcpy(ov, v2, sizeof v2); r->vfit++; }
            if (vn > 1e-7f && __builtin_isfinite(ang) && ang / h < 20.f) for (int i = 0; i < 3; i++) ow[i] = dq[i] / vn * ang / h;
            else if (vn <= 1e-7f) memset(ow, 0, sizeof ow);
        }
    }
    /* driven (a test, above): on its own physics, its velocity and yaw rate toward the states' */
    int drive = O.drive && dist < (r->driving ? O.drive_m + 20.f : O.drive_m);
    if (drive){
        float e[3] = { op[0] - sp[0], op[1] - sp[1], op[2] - sp[2] };
        if (len3(e) < DRIVE_SNAP_M){
            float vc[3], wc[3], k = 1.f / O.drive_tau, cx = e[0] * k, cy = e[1] * k, cm = __builtin_sqrtf(cx * cx + cy * cy);
            if (cm > DRIVE_VMAX){ cx *= DRIVE_VMAX / cm; cy *= DRIVE_VMAX / cm; }
            vc[0] = ov[0] + cx; vc[1] = ov[1] + cy; vc[2] = sv[2];
            float ic[4], qe[4];
            rq_conj(sq, ic); rq_mul(oq, ic, qe); rq_norm(qe);       /* the turn from the car to the states' (world) */
            if (qe[3] < 0.f) for (int i = 0; i < 4; i++) qe[i] = -qe[i];
            float vn = __builtin_sqrtf(qe[0] * qe[0] + qe[1] * qe[1] + qe[2] * qe[2]);
            float ang = 2.f * __builtin_atan2f(vn, qe[3]), rz = vn > 1e-6f ? qe[2] / vn * ang : 0.f;
            wc[0] = sw[0]; wc[1] = sw[1]; wc[2] = ow[2] + rz / O.drive_tau_r;
            if (finite_n(vc, 3) && finite_n(wc, 3)){
                put_car(c, ctl, car, sp, vc, sq, wc);
                r->driving = 1; r->drive_steps++;
                goto drawn;
            }
        }
        r->drive_snaps++;
    } else if (r->driving){
        /* out of reach: from where the physics left it, a glide to the states' place */
        r->driving = 0;
        float ic[4];
        for (int i = 0; i < 3; i++) r->gp[i] = sp[i] - o.p[i];
        rq_conj(o.q, ic); rq_mul(sq, ic, r->gq); rq_norm(r->gq);
        float e = len3(r->gp);
        if (!finite_n(r->gp, 3) || e > SNAP_M){ memset(r->gp, 0, sizeof r->gp); r->gq[0] = r->gq[1] = r->gq[2] = 0.f; r->gq[3] = 1.f; r->snaps++; }
        else { r->gtau = clampf(e / GLIDE_V, GLIDE_TAU_MIN, GLIDE_TAU_MAX); r->glides++; if (e > r->glide_max) r->glide_max = e; }
        for (int i = 0; i < 3; i++) op[i] = o.p[i] + r->gp[i];
        rq_mul(r->gq, o.q, oq); rq_norm(oq);
    }
    put_car(c, ctl, car, op, ov, oq, ow);
drawn:
    rd3(car + CAR_POS, r->putp); rd3(car + CAR_VEL, r->putv); r->puts++;
    {
        /* a car put over BIG_M from where it was at the step's start: why (always, a few lines a race) */
        float d[3] = { r->putp[0] - sp[0], r->putp[1] - sp[1], r->putp[2] - sp[2] };
        if (len3(d) > BIG_M && r->bigs++ < BIG_LINES)
            fprintf(stderr, "[racecars] %s put %.1f m from where it was: mode %d, delay %.0f ms, %.0f ms past its newest state (%d "
                    "states, epoch %u), glide %.1f m, driving %d, own %d, interpolator's correction %.1f m, %.0f m away\n",
                    r->name, (double)len3(d), o.mode, (double)r->delay / 1e3, ext / 1e3, r->mo.n, (unsigned)r->mo.epoch,
                    (double)len3(r->gp), r->driving, r->own, (double)len3(r->mo.err_p), (double)dist);
    }
    if (O.trace){
        /* a step whose move differs from the step before's (scaled by the steps) by over 0.3 m: what made it */
        float np[3]; rd3(car + CAR_POS, np);
        if (r->thave && h > 0.f && r->th > 0.f){
            float dv[3], k = h / r->th;
            for (int i = 0; i < 3; i++) dv[i] = (np[i] - r->tp[i]) - r->tm[i] * k;
            if (len3(dv) > 0.3f)
                fprintf(stderr, "[rctrace] %s: step %.1f ms (last %.1f), te %.1f ms on (fdt), delay %.1f ms, mode %d, off %.1f ms, "
                        "newest %.1f ms old, %d states; moved %.2f m off the last step's move; drive %d glide %.2f m\n",
                        r->name, (double)h * 1e3, (double)r->th * 1e3, (double)fdt / 1e3, (double)r->delay / 1e3, o.mode,
                        (double)r->mo.off_r / 1e3, age / 1e3, r->mo.n, (double)len3(dv), r->driving, (double)len3(r->gp));
        }
        if (r->steps % 300 == 0 && r->mo.n)
            fprintf(stderr, "[rctrace] %s every 300: %.0f m away, delay %.1f ms, mode %d, drawn %.1f ms past its newest state, "
                    "%d states over %.0f ms, newest %.1f ms old, way %.1f ms, vscale %.3f\n", r->name, (double)dist, (double)r->delay / 1e3, o.mode,
                    ((double)r->last_eval - (double)r->mo.off_r - (double)r->delay - (double)r->mo.t[r->mo.n - 1]) / 1e3, r->mo.n,
                    (double)(r->mo.t[r->mo.n - 1] - r->mo.t[0]) / 1e3, age / 1e3, (double)way / 1e3, K.vscale);
        if (h > 0.f){ for (int i = 0; i < 3; i++){ r->tm[i] = r->thave ? np[i] - r->tp[i] : 0.f; } r->th = r->thave ? h : 0.f; }
        memcpy(r->tp, np, sizeof np); r->thave = 1;
    }
    controls(car, &o);
    uint32_t drv = rd32(car + 0x28u) == 3u ? rd32(car + 0x34u) : 0, x = drv ? rd32(drv + 0x24u) : 0;
    if (ok(x, 0x139u)){ MR8(x + 0x138u) = 1; call_game(c, FN_DRIVEN, drv, 0, 0, RET_DRIVEN); }
    for (uint32_t k = 0; k < 3; k++) MRF32(ctl + CTL_PREV_V + 4u * k) = MRF32(car + CAR_VEL + 4u * k);
    r->steps++;
    return 1;
}

/* every frame, on the game thread (racenet.c's racenet_frame, after the world's step) */
void racecars_frame(CPU *c){
    (void)c;
    opts();
    int racing = O.on && in_race();
    uint32_t w = rd32(G_WORLD);
    if (!racing || w != g_world){
        if (!racing){ memset(g_rc, 0, sizeof g_rc); S.refused = S.bad = S.sent_err = 0; }
        g_world = racing ? w : 0;
    }
    clock_frame(racing);
    if (racing) send_frame();
    /* NFSU2_RACECARS_TRACE: a car moved between our put (the world's step) and here (before the frame is drawn) */
    if (racing && O.trace) for (int i = 0; i < NCARS; i++){
        Rc *r = &g_rc[i];
        if (!r->ip || !r->car || !ok(r->car, 0x9e0) || !r->mo.n) continue;
        float p[3], d[3], h = MRF32(G_WORLDDT); rd3(r->car + CAR_POS, p);
        for (int k = 0; k < 3; k++) d[k] = p[k] - r->putp[k] - r->putv[k] * h;
        if (!r->puts && racecars_fed(r->car))
            fprintf(stderr, "[rctrace] %s: not put this pass (own %d, newest %.0f ms ago)\n", r->name, r->own, (double)(now_us() - r->rx_at) / 1e3);
        else if (r->puts && (len3(d) > 0.2f || r->puts != 1))
            fprintf(stderr, "[rctrace] %s after the step: %.3f m (%.3f %.3f %.3f) off where we put it + its velocity x the step (%.1f ms), "
                    "%d puts, own %d\n", r->name, (double)len3(d), (double)d[0], (double)d[1], (double)d[2], (double)h * 1e3, r->puts, r->own);
        r->puts = 0;
    }
}

/* the lines at the race's end (racenet.c's end) */
void racecars_log(double secs){
    if (!racecars_on()) return;
    fprintf(stderr, "[racecars]   our car: %u states to the others (%.1f/s), %u datagrams, %u not sent; %u refused, %u bad; "
            "stamp clock set anew %u times\n", K.nsent, secs > 0 ? K.nsent / secs : 0., K.ndg, S.sent_err, S.refused, S.bad, K.snaps);
    for (int i = 0; i < NCARS; i++){
        Rc *r = &g_rc[i];
        if (!r->ip) continue;
        char a[16]; snprintf(a, sizeof a, "%u.%u.%u.%u", r->ip >> 24, r->ip >> 16 & 255u, r->ip >> 8 & 255u, r->ip & 255u);
        uint32_t st = r->near_steps + r->far_steps;
        fprintf(stderr, "[racecars]   car %s (slot %d, %s): %u states (%.1f/s), longest wait %.0f ms, waits over 250/500 ms %u/%u; "
                "least round trip %.0f ms; its states %.0f ms old on average (at most %.0f); drawn %u steps, %.0f %% near; "
                "between states %llu, carried on %llu, held %llu; corrections over 5 m %llu (largest %.2f m); on its own after "
                "%u hits, %u stalls; %u glides back (largest %.1f m), %u snaps; driven %u steps, %u put right\n",
                r->name[0] ? r->name : "?", r->slot, a, r->n, secs > 0 ? r->n / secs : 0., (double)r->gap_max / 1000.0, r->g250, r->g500,
                (double)rtt_min(r) / 1000.0, r->age_n ? r->age_sum / r->age_n / 1000.0 : 0., (double)r->age_max / 1000.0,
                r->steps, st ? 100.0 * r->near_steps / st : 0., (unsigned long long)r->mo.n_interp, (unsigned long long)r->mo.n_extrap,
                (unsigned long long)r->mo.n_hold, (unsigned long long)r->mo.n_snap, (double)r->mo.err_max, r->own_hit, r->own_stall,
                r->glides, (double)r->glide_max, r->snaps, r->drive_steps, r->drive_snaps);
    }
}
