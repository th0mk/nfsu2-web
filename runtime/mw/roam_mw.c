/* roam_mw.c -- Most Wanted's free roam with friends: the game side (TRACKER M10-04 to M10-07, M10-25).  Off unless
 * NFSU2_ROAM=1.  The wire, the session and the hub are Underground 2's (runtime/roam_core.h, roam_web.c,
 * web/server-rs/crates/roam); this file is what runtime/roam_game.c is for Underground 2, for Most Wanted, and much
 * smaller: friends' cars made, posed every frame and taken away again.  Their looks, sounds, names, minimap arrows,
 * collisions shared by weight, races and the rest of FEATURES 8.3 are later rows of M10.
 *
 * Every frame, at the game's per-frame anchor (game.h on_frame: MainLoop's 0x663EC6, after the update and before
 * the render, on the game's thread):
 *  - a sync clock: the simulation's time (the sim's steps x its step, [0x9203B0] +0x28 and +0x20), pulled to the host
 *    clock as roam_game.c's is, stamps the player's states and is the clock friends' cars are drawn by;
 *  - in free roam (GameFlow 6 and IsRoaming 0x56DC00, or no activity and no race at all: the free roam a career saved
 *    there resumes into answers 0 to IsRoaming, its activity unset) the player's car goes to the session: its rigid
 *    body's place, orientation, velocities and speed, turned from the simulation's frame into the world's (below);
 *    its look (the vehicle's name and attribute key) goes as a car record when it changes;
 *  - each friend in free roam within NEAR_M whose state and record came gets a car of our own (up to
 *    NFSU2_ROAM_MAX_CARS, 6: the pools, M10-05) when the record's vehicle key is one of the career's player cars
 *    (PLAYER_CARS; the name shown is the table's, not the record's): a record of any other vehicle, the police's
 *    cars and helicopter, traffic, a boss's, gets none, with one line, and takes a car already shown away; the
 *    nearest first, one a frame: a PVehicle made the way the game
 *    makes its cop and traffic cars (PVehicle::Construct 0x689820 through the "PVehicle" factory, a VehicleParams
 *    of driver class DRIVER_NONE (4): AIVehicleEmpty for a brain, no input of its own, no IPerpetrator, so the
 *    police, the pursuit, the roadblocks and the traffic have no AI of it to look at; GAME.md has the list, M10-04),
 *    and its pose is written every frame from the core's interpolation through the car's IRigidBody (SetPosition,
 *    SetOrientation by matrix, SetLinearVelocity, SetAngularVelocity), its steering, gas and brake into its
 *    IInput's controls, which its own suspension and lights read (the wheels are not yet seen on a screen); the
 *    physics still runs between two poses (solid cars: the game's own collisions), and a pose a frame holds it;
 *  - a car whose friend left, went into an event, stopped sending, went past FAR_M or out of the hub's list goes
 *    again (ISimable::Kill), and so do all of them when the player leaves free roam; a car the game took away
 *    itself (a world reload) is noticed by the rigid bodies' list and forgotten.  A car the game makes none of is
 *    asked for again every RETRY_US, with one line saying which of Construct's checks refused it (mw_roam_hook).
 *
 * The frames.  The simulation's (where `where` and `teleport` work, UMath) is left-handed with Y up; the world's
 * (the renderer's, the maps', the wire's: roam_core.h wants Z up for the hub's map) is right-handed with Z up, and
 * world = (sim z, -sim x, sim y) (GAME.md 13.8, checked on the car lot's zone).  That is a reflection (det -1), so
 * an orientation goes as its matrix, M R M^T, and an angular velocity, a pseudo-vector, picks up the sign:
 * w_world = -M w_sim.  The rigid body's matrix (its data +0x70) has the car's right, up and forward vectors as
 * rows; the wire's quaternion is rc_m2q of forward, left and up in the world.
 *
 * The wire's steer is MW's steering control (-1 .. 1, left positive), not the wheels' angle as Underground 2 sends it:
 * only MW peers read MW states (the game byte), and the receiver gives it to its car's input as it came.
 *
 * Script commands (tests/mw/scripts/roam_*.txt; test only, the guest calls queued for the anchor):
 *   roam mp status                  the session, each friend and each car of ours
 *   roam mp waitpeers N [TMO]       wait for N friends established (at most TMO frames)
 *   roam mp waitcars N [TMO]        wait for N friends' cars shown
 *   roam mp waitmoved N M [TMO]     wait until N friends' cars have each moved M metres since they appeared
 *   roam mp drop                    friends' cars away now (ISimable::Kill, as for a friend gone far), to be made
 *                                   again from the next frame as any new friend's (a car taken away and made again)
 *   roam probe                      the player's car: rigid body, matrix, controls, driver class; the pools
 *   roam spawn [NAME]               a car of our own of that vehicle (the player's without one) 12 m ahead, posed
 *                                   there every frame as a friend's would be (no network: the pools probe, M10-05)
 *   roam unspawn                    those cars away again
 *   roam pools                      the counts the friends' cars use up (rigid bodies, vehicles, those of driver
 *                                   class 4 as ours are, the car loader's memory pool): one [roampool] line
 * NFSU2_ROAM_POSELOG=<file> writes UG2's pose log (L rows the player's car, R rows friends' cars as shown, world
 * frame) for tools/roam_compare.py; NFSU2_ROAM_MAX_CARS caps the cars (6, DEF_CARS). */
#include "cpu.h"
#include "game.h"
#include "roam_core.h"
#include "roam_plat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t com_scratch(uint32_t n);
void dispatch_indirect(CPU *c, uint32_t addr);

/* ---- the game's objects (speed.exe v1.3) ---- */
#define G_PLAYERS        0x0092d87cu     /* IPlayer *[]; count at G_NPLAYERS; IPlayer +4: its ISimable */
#define G_NPLAYERS       0x0092d884u
#define FN_IS_ROAMING    0x0056dc00u     /* GRaceStatus' activity is free roam's (career_mw.c) */
#define G_SIM            0x009203b0u     /* the sim: +0x20 its step (s, float), +0x28 the steps run */
#define FN_QUERY         0x005d59f0u     /* thiscall on [ISimable + 4] (id): an interface of the object */
#define IID_IVEHICLE     0x004040e0u
#define IID_IINPUT       0x00403f90u     /* AIVehicle keeps it at +0xE0 (0x415033) */
#define FN_CONSTRUCT     0x00689820u     /* PVehicle's factory function ("PVehicle", 0x8875B0): cdecl (Sim::Param by
                                          * value: type, name, the params, 0) -> ISimable* (PVehicle + 0x2C) or 0 */
#define FN_VP_TYPE       0x004040f0u     /* cdecl (UCrc32 *out): the type id of VehicleParams */
#define FN_ATTRIB_KEY    0x00454640u     /* cdecl (const char *): Attrib's key of a name (the pvehicle collection's) */
#define VP_NAME          0xa6b47facu     /* the name every VehicleParams the game makes carries */
#define DRIVER_NONE      4u              /* AIVehicleEmpty (the table at 0x8F8890) */
#define G_RB_COUNT       0x009377c8u     /* RigidBody's count: their table 0x9376C8 has 64 rows (the ctor 0x6B59F0) */
#define RB_MAX           64u
#define G_VEHLIST        0x0092d0dcu     /* the vehicles' IRigidBody pointers: +0 the array, +8 the count (cap 160) */
#define G_SIMLIST        0x0090d664u     /* every vehicle (PVehicle's ctor adds it, 0x68933A via 0x423750; AITarget::Acquire
                                          * 0x423860 takes only these): +0 the array, +8 the count */
#define G_POOLS          0x0090df10u     /* bMemoryPool *[16]: +0 name, +0x24 allocations, +0x2C holds, +0x30 allocated (pursuit_mw.c) */
/* ISimable (PVehicle + 0x2C) */
#define SIM_KILL         0x08u
#define SIM_RB           0x54u           /* GetRigidBody */
/* IRigidBody (RBVehicle + 0x48, vtable 0x8AC880): its data at [[this + 0x30]] */
#define RB_GETPOS        0x20u
#define RB_GETVEL        0x24u
#define RB_GETANG        0x28u
#define RB_GETSPEED      0x2cu           /* float on the x87 stack */
#define RB_SETPOS        0x5cu
#define RB_SETVEL        0x60u
#define RB_SETANG        0x64u
#define RB_SETMAT        0x70u           /* (Matrix4 *): rows right, up, forward (the 4th ignored) */
#define RBD_QUAT         0x00u
#define RBD_POS          0x10u
#define RBD_VEL          0x20u
#define RBD_ANG          0x30u
#define RBD_RIGHT        0x70u           /* GetRightVector (vtable +0x38) */
#define RBD_UP           0x80u           /* +0x3C */
#define RBD_FWD          0x90u           /* +0x34 */
/* IVehicle (PVehicle + 0xAC, vtable 0x8AA828) */
#define VEH_ACTIVE       0x20u           /* bool [+0xB1] (0 for the player's own car too: not identified) */
#define VEH_NAME         0x4cu           /* const char * */
#define VEH_CLASS        0x58u           /* the driver class, [PVehicle + 0x140] */
/* IInput (PInput + 0x48, vtable 0x8AB5C8; InputPlayer's 0x8AC670): its controls at +8; of those +4 the steering (-1 full
 * left: the setter at vtable +0x14), +0x14 the gas (+0x18), +0x18 the brake (+0x1C; 1 while the car stands without gas),
 * seen in roam_probe.txt runs (LEFT and UP held) */
#define IN_CTL           0x08u
#define CTL_STEER        0x04u
#define CTL_GAS          0x14u
#define CTL_BRAKE        0x18u

/* PVehicle::Construct's checks, in order (0x689820): each hook is the instruction after a check's call, where its answer
 * is in eax or on the stack.  While roam_mw.c makes a car (g_cons.on) the last one reached, with its answer, says
 * which check refused a car the game made none of ("[roam] ... the game made none at ..."). */
#define HOOK_SITE(va)    (va##u)
#define H_CONS_ATTRIB    HOOK_SITE(0x006898c1)   /* the vehicle's attributes: [esp+0x18] its collection, 0 = none */
#define H_CONS_CUSTOM    HOOK_SITE(0x00689955)   /* a customization of its preset taken (al) */
#define H_CONS_SHAPE     HOOK_SITE(0x00689a0e)   /* its entry in the 0x8003B900 chunks' list (0x92C5B8, 0x681340): eax, +4 >= 1 */
#define H_CONS_PARTS     HOOK_SITE(0x00689a5f)   /* 0x669080's flag, [esp+0x8c] & 1 */
#define H_CONS_SIM       HOOK_SITE(0x00689b0c)   /* the car loader's budget, 0x687710 (al): the memory a new model takes (the table
                                          * 0x904448: Racing 6,397 KB, read by 0x739F80) against [0x9B2C64] less what the
                                          * PVehicles' models hold (0x683C80), and a count of 20 ([0x92CD24]); past them
                                          * it would evict vehicles (none of ours: no cache), else 0.  Six new racing
                                          * models fit beside the player's in crowd runs */
#define H_CONS_ROOM      HOOK_SITE(0x00689b76)   /* the rigid bodies' room (0x6ED260, al) */
#define H_CONS_MADE      HOOK_SITE(0x00689bf4)   /* the PVehicle made (eax) */
static struct { int on, step; uint32_t val, val2; } g_cons;
static const char *const cons_step[] = { "the start", "the attributes", "the preset's customization", "the shape (0x8003B900 list)",
                                         "0x669080", "the car loader's budget (0x687710)", "the rigid bodies' room", "the PVehicle's construction" };
int mw_roam_hook(CPU *c, uint32_t va){
    int st = va == H_CONS_ATTRIB ? 1 : va == H_CONS_CUSTOM ? 2 : va == H_CONS_SHAPE ? 3 : va == H_CONS_PARTS ? 4 : va == H_CONS_SIM ? 5
           : va == H_CONS_ROOM ? 6 : va == H_CONS_MADE ? 7 : 0;
    if (!st) return 0;
    if (!g_cons.on) return 1;
    g_cons.step = st;
    switch (st){
    case 1: g_cons.val = MR32(c->esp + 0x18u); g_cons.val2 = 0; break;
    case 3: g_cons.val = c->eax; g_cons.val2 = c->eax && mem_is_committed(c->eax + 7u) ? MR32(c->eax + 4u) : 0u; break;
    case 4: g_cons.val = MR8(c->esp + 0x8cu) & 1u; g_cons.val2 = 0; break;
    default: g_cons.val = c->eax & (st == 7 ? 0xffffffffu : 0xffu); g_cons.val2 = 0; break;
    }
    return 1;
}

#define MAX_CARS         32
#define DEF_CARS         6               /* the game's car loader takes six friends' cars beside the player's (its budget,
                                          * H_CONS_SIM: 6,397 KB reserved a racing car of the pool's 50,000 KB; crowd runs
                                          * of 16 and 32 friends, distinct models or all one, made 6): the first friend
                                          * count (TRACKER M10-05) until the pool grows */
#define VISIBLE_US       2000000         /* a friend's snapshots older than this: not shown */
#define GONE_US          10000000        /* its car goes when not shown this long */
#define RETRY_US         2000000         /* a car the game would not make: asked again after this */
#define NEAR_M           300.f           /* a friend's car is made within this of the player's car (no need for farther ones:
                                          * each holds a rigid body of the 64 and its model the car loader's memory) */
#define FAR_M            450.f           /* and taken away past this, after FAR_US */
#define FAR_US           5000000

/* The career's player cars by their pvehicle names (GAME.md 18; runtime/mw/bot_mw.c car_name without its "speedtest",
 * web/tools/roam-crowd.mjs's Most Wanted list): the only vehicles a friend's car record may name.  Their keys are the
 * game's own (FN_ATTRIB_KEY), worked out once in free roam. */
static const char *const PLAYER_CARS[] = { "911turbo", "997s", "a3", "a4", "bmwm3gtr", "bmwm3gtre46", "camaro", "carreragt",
    "caymans", "clio", "clk500", "cobaltss", "corvette", "corvettec6r", "cts", "db9", "eclipsegt", "elise", "fordgt",
    "gallardo", "gti", "gto", "imprezawrx", "is300", "lancerevo8", "monaro", "murcielago", "mustanggt", "punto", "rx7",
    "rx8", "sl500", "sl65", "slr", "supra", "tt", "viper" };
#define N_PLAYER_CARS    ((int)(sizeof PLAYER_CARS / sizeof PLAYER_CARS[0]))
static uint32_t g_pc_key[N_PLAYER_CARS];
static int g_pc_ready;

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static float rdf(uint32_t a){ uint32_t v = rd(a); float f; memcpy(&f, &v, 4); return f; }
static void wrf(uint32_t a, float f){ memcpy(g_mem + a, &f, 4); }
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args){
    if (!fn) return 0;
    uint32_t keep = c->ecx; c->ecx = self;
    uint32_t r = call_guest(c, fn, n, args);
    c->ecx = keep;
    return r;
}
static float gcall_f(CPU *c, uint32_t fn, uint32_t self){
    if (!fn) return 0.0f;
    CPU save = *c;
    c->ecx = self;
    PUSH32(0xDEADBEEFu);
    dispatch_indirect(c, fn);
    float f = (float)ST(0);
    *c = save;
    return f;
}
static uint32_t vcall(CPU *c, uint32_t obj, uint32_t slot, int n, const uint32_t *args){
    uint32_t vt = rd(obj);
    return vt ? gcall(c, rd(vt + slot), obj, n, args) : 0u;
}
static uint32_t query(CPU *c, uint32_t sim, uint32_t id){
    uint32_t o = rd(sim + 4u);
    return o ? gcall(c, FN_QUERY, o, 1, &id) : 0u;
}

/* ---- the frames ---- */
static void s2w(const float *s, float *w){ float x = s[0], y = s[1], z = s[2]; w[0] = z; w[1] = -x; w[2] = y; }
static void w2s(const float *w, float *s){ float x = w[0], y = w[1], z = w[2]; s[0] = -y; s[1] = z; s[2] = x; }
static void s2w_axial(const float *s, float *w){ float x = s[0], y = s[1], z = s[2]; w[0] = -z; w[1] = x; w[2] = -y; }
static void w2s_axial(const float *w, float *s){ float x = w[0], y = w[1], z = w[2]; s[0] = y; s[1] = -z; s[2] = -x; }
static void cross(const float *a, const float *b, float *o){
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}

/* ---- state ---- */
typedef struct {
    int      st;                 /* 0 free, 1 shown */
    int      peer;               /* session table index */
    uint32_t peer_id;
    char     name[RC_NAME_MAX + 1], car[RC_CFG_TYPE_NAME + 1];
    uint32_t key;                /* the vehicle's attribute key */
    uint32_t sim, rb, in, veh;   /* ISimable, IRigidBody, IInput, IVehicle of our car */
    int64_t  seen_us;            /* last frame its friend was shown */
    float    p0[3], moved;       /* where it appeared; the most it went from there (m) */
    float    last[3], drift;     /* the place written last frame (sim frame); the most the game moved it between two writes */
    int64_t  far_us;             /* since when it is past FAR_M (0: it is not) */
    float    ctl_last[3];        /* the steering, gas and brake written last frame */
    long     ctl_changed;        /* frames the game had changed them since (its own input clears them: then they do not hold) */
    long     frames;             /* frames posed */
    uint32_t cfg_gen;
} Car;

static struct {
    int inited, on, net, test, max_cars;
    int in_roam, parked;
    double sync;                 /* the sync clock (us) */
    int64_t last_host;
    uint32_t last_steps;
    long frame0;
    uint32_t key;                /* the player's vehicle's key, name */
    char car[RC_CFG_TYPE_NAME + 1];
    int64_t cfg_next;
    uint32_t scratch;            /* guest memory of ours: a VehicleParams, its vectors, a matrix */
    uint32_t vp_type;
    FILE *poselog;
    unsigned creates, create_fail, kills, lost, joins, leaves;
    int64_t retry_us[RC_TABLE];
    uint32_t retry_id[RC_TABLE];
    int retry_step[RC_TABLE];    /* the check that refused that friend's car last (logged when it changes) */
    unsigned fail_at[8];         /* refusals by Construct's check */
    unsigned refused;            /* friends' car records naming no player car */
    uint32_t bad_id[RC_TABLE], bad_gen[RC_TABLE];   /* the record of that friend's logged last as naming none (its generation) */
    uint32_t local_id;
} R;
static rc_session g_s;
static Car g_car[MAX_CARS];
static Car g_test[8];            /* roam spawn's cars */
static int g_ntest;
static char g_name[RC_NAME_MAX + 1];

static const char *opt(const char *k){ return getenv(k); }
static int env_int(const char *k, int def, int lo, int hi){
    const char *v = opt(k);
    if (!v || !*v) return def;
    int x = atoi(v);
    return x < lo ? lo : x > hi ? hi : x;
}

/* ---- the game's objects, read ---- */
static uint32_t player_sim(CPU *c){
    if (!rd(G_NPLAYERS)) return 0;
    uint32_t pl = rd(rd(G_PLAYERS));
    if (!pl || !rd(pl)) return 0;
    return vcall(c, pl, 4u, 0, 0);
}
#define G_RACESTATUS     0x0091e000u     /* GRaceStatus: +0x198A (byte) a race is on, +0x1968 its activity */
static int roaming(CPU *c){
    if (game_flow() != 6u) return 0;
    if (gcall(c, FN_IS_ROAMING, 0, 0, 0) & 0xffu) return 1;
    uint32_t rs = rd(G_RACESTATUS);
    return rs && !rd(rs + 0x1968u) && !MR8(rs + 0x198au);   /* no activity at all and no race: the world as it loaded */
}
/* a vehicle of the game's list (its IRigidBody: the list holds those, seen in roam_probe.txt runs) */
static int vehicle_listed(uint32_t rb){
    uint32_t b = rd(G_VEHLIST), n = rd(G_VEHLIST + 8u);
    for (uint32_t i = 0; rb && b && i < n && i < 256u; i++) if (rd(b + 4u * i) == rb) return 1;
    return 0;
}
/* the rigid body's data block, 0 when it is not there */
static uint32_t rb_data(uint32_t rb){
    uint32_t pp = rd(rb + 0x30u), d = rd(pp);
    return ok(d, 0xa0) ? d : 0u;
}

/* the player's car as a wire state (world frame); 0 when there is none */
static int local_state(CPU *c, rc_state *o, uint32_t *sim_out){
    uint32_t sim = player_sim(c), rb = sim ? vcall(c, sim, SIM_RB, 0, 0) : 0u, d = rb ? rb_data(rb) : 0u;
    if (!d) return 0;
    if (sim_out) *sim_out = sim;
    float p[3], v[3], w[3], r[3], u[3], f[3];
    for (int k = 0; k < 3; k++){
        p[k] = rdf(d + RBD_POS + 4u * k); v[k] = rdf(d + RBD_VEL + 4u * k); w[k] = rdf(d + RBD_ANG + 4u * k);
        r[k] = rdf(d + RBD_RIGHT + 4u * k); u[k] = rdf(d + RBD_UP + 4u * k); f[k] = rdf(d + RBD_FWD + 4u * k);
    }
    for (int k = 0; k < 3; k++) if (!isfinite(p[k]) || !isfinite(v[k]) || !isfinite(f[k]) || !isfinite(u[k])) return 0;
    memset(o, 0, sizeof *o);
    s2w(p, o->p); s2w(v, o->v); s2w_axial(w, o->w);
    float fw[3], uw[3], lw[3];
    s2w(f, fw); s2w(u, uw); cross(uw, fw, lw);
    rc_m2q(fw, lw, uw, o->q);
    o->speed = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
    (void)r;
    /* the controls the car takes (its IInput): steering, gas, brake */
    uint32_t in = query(c, sim, IID_IINPUT), ctl = in ? in + IN_CTL : 0u;
    if (ok(ctl, 0x24)){
        float gas = rdf(ctl + CTL_GAS), brake = rdf(ctl + CTL_BRAKE), steer = -rdf(ctl + CTL_STEER);   /* the wire: left positive */
        o->throttle = isfinite(gas) ? (gas < 0 ? 0 : gas > 1 ? 1 : gas) : 0;
        o->brake = isfinite(brake) ? (brake < 0 ? 0 : brake > 1 ? 1 : brake) : 0;
        o->steer = isfinite(steer) ? (steer < -1 ? -1 : steer > 1 ? 1 : steer) : 0;
        if (o->brake > 0.05f) o->bits |= RC_BIT_BRAKE;
    }
    return 1;
}

/* the player's vehicle's name and key (IVehicle +0x4C) */
static int local_car(CPU *c, uint32_t sim, char *name, uint32_t *key){
    uint32_t veh = query(c, sim, IID_IVEHICLE), s = veh ? vcall(c, veh, VEH_NAME, 0, 0) : 0u;
    if (!ok(s, 1)) return 0;
    int n = 0;
    for (; n < RC_CFG_TYPE_NAME && ok(s + (uint32_t)n, 1) && MR8(s + (uint32_t)n); n++) name[n] = (char)MR8(s + (uint32_t)n);
    name[n] = 0;
    if (!n) return 0;
    uint32_t arg = s;
    *key = call_guest(c, FN_ATTRIB_KEY, 1, &arg);
    return 1;
}

/* Attrib's key of a pvehicle name (the game's own, FN_ATTRIB_KEY), the name copied into our guest memory; 0 for a name
 * longer than 31 */
static uint32_t attrib_key(CPU *c, const char *name){
    uint32_t s = R.scratch ? R.scratch : (R.scratch = com_scratch(0x200u)), n = (uint32_t)strlen(name);
    if (n > 0x1fu) return 0;
    memset(g_mem + s + 0x1c0u, 0, 0x20u); memcpy(g_mem + s + 0x1c0u, name, n);
    uint32_t a = s + 0x1c0u;
    return call_guest(c, FN_ATTRIB_KEY, 1, &a);
}
static void player_car_keys(CPU *c){
    for (int i = 0; i < N_PLAYER_CARS; i++) g_pc_key[i] = attrib_key(c, PLAYER_CARS[i]);
    g_pc_ready = 1;
}
/* the player car of a vehicle key (its index in PLAYER_CARS), -1 for any other vehicle */
static int player_car(uint32_t key){
    for (int i = 0; key && g_pc_ready && i < N_PLAYER_CARS; i++) if (g_pc_key[i] == key) return i;
    return -1;
}

/* ---- our cars ---- */
/* a car of vehicle key, at pos facing fwd (simulation frame), driver class DRIVER_NONE; 0 = the game made none */
static uint32_t make_car(CPU *c, uint32_t key, const float *pos, const float *fwd){
    if (!R.scratch) R.scratch = com_scratch(0x200u);
    uint32_t P = R.scratch, dir = P + 0x40u, at = P + 0x50u;
    if (!R.vp_type){ uint32_t out = P; call_guest(c, FN_VP_TYPE, 1, &out); R.vp_type = rd(P); }
    memset(g_mem + P, 0, 0x60u);
    for (int k = 0; k < 3; k++){ wrf(dir + 4u * k, fwd[k]); wrf(at + 4u * k, pos[k]); }
    MR32(P + 0x00u) = R.vp_type;        /* Sim::Param: type, name, the params themselves */
    MR32(P + 0x04u) = VP_NAME;
    MR32(P + 0x08u) = P;
    MR32(P + 0x10u) = DRIVER_NONE;      /* VehicleParams: driver class, the vehicle's key, the start's direction and */
    MR32(P + 0x14u) = key;              /* place; no customization (the vehicle's own), no cache, no performance to */
    MR32(P + 0x18u) = dir;              /* match, no flags (2 would set it on the ground at the place) */
    MR32(P + 0x1cu) = at;
    uint32_t arg[4] = { R.vp_type, VP_NAME, P, 0 };
    g_cons.on = 1; g_cons.step = 0; g_cons.val = g_cons.val2 = 0;
    uint32_t sim = call_guest(c, FN_CONSTRUCT, 4, arg);
    g_cons.on = 0;
    return sim;
}
/* why the game made none: the last of Construct's checks it reached and its answer */
static const char *cons_why(char *b, int n){
    snprintf(b, (size_t)n, "at %s (0x%x%s)", cons_step[g_cons.step], g_cons.val, g_cons.step == 3 ? (g_cons.val ? ", count low" : ", none") : "");
    return b;
}
static int car_bind(CPU *c, Car *k, uint32_t sim){
    k->sim = sim;
    k->rb = vcall(c, sim, SIM_RB, 0, 0);
    k->in = query(c, sim, IID_IINPUT);
    k->veh = query(c, sim, IID_IVEHICLE);
    return k->rb && rb_data(k->rb) && k->veh;
}
static void car_kill(CPU *c, Car *k, const char *why){
    if (k->sim && vehicle_listed(k->rb)){ vcall(c, k->sim, SIM_KILL, 0, 0); R.kills++; }
    fprintf(stderr, "[roam] f=%ld %s's car (%s) away: %s, %ld frames shown, moved %.1f m\n", autotest_frame(), k->name, k->car,
            why, k->frames, (double)k->moved);
    memset(k, 0, sizeof *k);
}
/* the pose (world frame) written into the car: place, orientation, velocities; steering, gas and brake */
static void car_pose(CPU *c, Car *k, const float *pw, const float *qw, const float *vw, const float *ww,
                     float steer, float gas, float brake){
    float ps[3], vs[3], ws[3], fw[3], lw[3], uw[3], f[3], l[3], u[3], r[3];
    w2s(pw, ps); w2s(vw, vs); w2s_axial(ww, ws);
    /* how far the simulation moved the car since the pose written last frame (its own physics between our writes) */
    uint32_t d = rb_data(k->rb);
    if (d && k->frames){
        float e2 = 0;
        for (int i = 0; i < 3; i++){ float e = rdf(d + RBD_POS + 4u * (uint32_t)i) - k->last[i]; e2 += e * e; }
        float e = sqrtf(e2);
        if (e > k->drift) k->drift = e;
    }
    rc_q2m(qw, fw, lw, uw);
    w2s(fw, f); w2s(uw, u); w2s(lw, l);
    for (int i = 0; i < 3; i++) r[i] = -l[i];
    uint32_t m = R.scratch + 0x100u, vp = R.scratch + 0x140u, vv = R.scratch + 0x150u, vw2 = R.scratch + 0x160u;
    for (int i = 0; i < 3; i++){
        wrf(m + 4u * i, r[i]); wrf(m + 0x10u + 4u * i, u[i]); wrf(m + 0x20u + 4u * i, f[i]); wrf(m + 0x30u + 4u * i, ps[i]);
        wrf(vp + 4u * i, ps[i]); wrf(vv + 4u * i, vs[i]); wrf(vw2 + 4u * i, ws[i]);
    }
    wrf(m + 0xcu, 0); wrf(m + 0x1cu, 0); wrf(m + 0x2cu, 0); wrf(m + 0x3cu, 1.0f);
    uint32_t a;
    a = m;   vcall(c, k->rb, RB_SETMAT, 1, &a);
    a = vp;  vcall(c, k->rb, RB_SETPOS, 1, &a);
    a = vv;  vcall(c, k->rb, RB_SETVEL, 1, &a);
    a = vw2; vcall(c, k->rb, RB_SETANG, 1, &a);
    uint32_t ctl = k->in ? k->in + IN_CTL : 0u;
    if (ok(ctl, 0x24)){
        if (k->frames && (rdf(ctl + CTL_STEER) != k->ctl_last[0] || rdf(ctl + CTL_GAS) != k->ctl_last[1] || rdf(ctl + CTL_BRAKE) != k->ctl_last[2]))
            k->ctl_changed++;
        k->ctl_last[0] = -steer; k->ctl_last[1] = gas; k->ctl_last[2] = brake;
        wrf(ctl + CTL_BRAKE, brake); wrf(ctl + CTL_STEER, -steer); wrf(ctl + CTL_GAS, gas);
    }
    memcpy(k->last, ps, sizeof k->last);
    k->frames++;
}

/* ---- the session ---- */
static void init_once(void){
    R.inited = 1;
    R.on = env_int("NFSU2_ROAM", 0, 0, 1);
    if (!R.on) return;
    R.test = roam_plat_test_mode();
    R.max_cars = env_int("NFSU2_ROAM_MAX_CARS", DEF_CARS, 0, MAX_CARS);
    int port = env_int("NFSU2_ROAM_PORT", RC_DEFAULT_PORT, 1024, 65535);
    int maxp = env_int("NFSU2_ROAM_MAX_PLAYERS", RC_MAX_PLAYERS, 2, RC_MAX_PLAYERS);
    const char *nm = opt("NFSU2_ROAM_NAME");
    if (nm && *nm) rc_name_clean(g_name, nm, 64); else snprintf(g_name, sizeof g_name, "Player");
    const char *rm = opt("NFSU2_ROAM_ROOM");
    rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = roam_plat_random(); cf.port = (uint16_t)port; cf.room = rc_room_hash(rm ? rm : "");
    snprintf(cf.name, sizeof cf.name, "%s", g_name);
    cf.platform = RC_PLAT_MAC;
    cf.test_mode = R.test; cf.max_players = maxp; cf.rate_hz = env_int("NFSU2_ROAM_RATE", 20, 10, 30);
    cf.discovery = env_int("NFSU2_ROAM_DISCOVERY", 1, 0, 1) && !R.test;
    cf.game = RC_GAME_MW;               /* the HELLO says Most Wanted: an MW hub takes it, a UG2 hub and UG2 peers do not */
    rc_io io = { 0, 0, 0, 0 };
    rc_session_init(&g_s, &cf, &io);
    roam_plat_cfg pc = { (uint16_t)port, R.test, cf.discovery, opt("NFSU2_ROAM_IMPAIR") };
    if (roam_plat_start(&pc, &g_s) != 0){ fprintf(stderr, "[roam] free-roam multiplayer could not start: off\n"); R.on = 0; return; }
    R.net = 1;
    R.local_id = cf.self_id;
    uint32_t pip[RC_MAX_PEERS]; uint16_t ppt[RC_MAX_PEERS];
    int np = roam_plat_parse_peers(opt("NFSU2_ROAM_PEERS"), R.test, (uint16_t)port, pip, ppt, RC_MAX_PEERS);
    roam_plat_lock();
    for (int i = 0; i < np; i++) rc_add_peer(&g_s, pip[i], ppt[i], 1, roam_plat_now_us());
    roam_plat_unlock();
    const char *pl = opt("NFSU2_ROAM_POSELOG");
    if (pl && *pl){
        R.poselog = fopen(pl, "w");
        if (R.poselog){ static char buf[1 << 20]; setvbuf(R.poselog, buf, _IOFBF, sizeof buf);
                        fprintf(R.poselog, "kind,host_us,sync_us,id,x,y,z,speed,mode,err,fx,fy,fz,uz,rx,ry,rz\n"); }
    }
    R.frame0 = autotest_frame();
    fprintf(stderr, "[roam] free-roam multiplayer ON (Most Wanted): id %08x, port %d, %d configured peer(s), room \"%s\", up to %d "
            "players, %d friends' cars, %d Hz%s\n", cf.self_id, port, np, rm ? rm : "", maxp, R.max_cars, cf.rate_hz,
            R.test ? ", test harness (loopback only)" : "");
}

/* the sync clock: the simulation's time, pulled to the host clock (roam_game.c's rule) */
static int64_t clock_step(int in_roam, int64_t host){
    uint32_t sim = rd(G_SIM), steps = sim ? rd(sim + 0x28u) : 0u;
    float P = sim ? rdf(sim + 0x20u) : 0.f;
    double wdt = (steps >= R.last_steps && steps - R.last_steps < 1000u && isfinite(P) && P > 0.f && P < 1.f)
                 ? (double)(steps - R.last_steps) * P : 0.0;
    R.last_steps = steps;
    int64_t hdt = host - R.last_host, frame_dt = 0;
    if (hdt < 0 || hdt > 1000000) hdt = 0;
    if (!in_roam){
        R.sync = (double)host;
        if (R.in_roam){ roam_plat_lock(); rc_bump_clock(&g_s); roam_plat_unlock(); }
    } else if (!R.in_roam){
        R.sync = (double)host;
        roam_plat_lock(); rc_bump_epoch(&g_s); rc_bump_clock(&g_s); roam_plat_unlock();
        R.cfg_next = 0;
    } else if (wdt <= 0.0){
        if (hdt > 0){ R.sync += (double)hdt; frame_dt = hdt; }
    } else {
        double before = R.sync;
        R.sync += wdt * 1e6;
        double err = (double)host - R.sync;
        if (fabs(err) > 1000000.0){ R.sync = (double)host; roam_plat_lock(); rc_bump_clock(&g_s); roam_plat_unlock(); }
        else { double k = (double)hdt / 250000.0; R.sync += err * (k < 1.0 ? k : 1.0); if (R.sync < before) R.sync = before; }
        frame_dt = (int64_t)(R.sync - before);
    }
    R.in_roam = in_roam;
    R.last_host = host;
    return frame_dt;
}

/* friends' cars: one each for the friends in free roam, the nearest first while there are more than cars */
typedef struct { int idx; uint32_t id; char name[RC_NAME_MAX + 1]; int visible, have_pose; rc_pose pose; float dist;
                 uint32_t key, cfg_gen; char car[RC_CFG_TYPE_NAME + 1];
                 int bad; uint32_t bad_key; } View;   /* bad: its record names no player car (bad_key; car: the name it gave) */
static View g_v[RC_TABLE];
static int g_nv;

/* How far behind its sender's clock a friend's car is drawn (rc_interp_eval's delay), roam_game.c's rule for solid
 * cars (friends' cars here are solid: the game collides them): where the friend is now, the newest state carried on by
 * the one-way delay (half the smallest of the last 8 round trips, at most 150 ms), so a contact is at the same spot on
 * both screens; a change is slewed at 5 % of the frame's time, so the car never jumps.  With a fixed 100 ms in the
 * past instead, a pair's run (pair1) showed the car 100 ms behind its friend, 2.2 m at 24 m/s. */
#define RD_PINGS 8
static struct { uint32_t id; int32_t rtt[RD_PINGS], last; int n, h, init; int64_t d; } g_rd[RC_TABLE];
static int64_t render_delay(int i, const rc_peer *p, int64_t frame_dt){
    if (g_rd[i].id != p->id){ memset(&g_rd[i], 0, sizeof g_rd[i]); g_rd[i].id = p->id; }
    if (p->rtt_us > 0 && p->rtt_us != g_rd[i].last){
        g_rd[i].last = p->rtt_us; g_rd[i].rtt[g_rd[i].h] = p->rtt_us; g_rd[i].h = (g_rd[i].h + 1) % RD_PINGS;
        if (g_rd[i].n < RD_PINGS) g_rd[i].n++;
    }
    int32_t m = 0;
    for (int k = 0; k < g_rd[i].n; k++) if (k == 0 || g_rd[i].rtt[k] < m) m = g_rd[i].rtt[k];
    int64_t want = -(int64_t)(m / 2 < 150000 ? m / 2 : 150000);
    if (!g_rd[i].init){ g_rd[i].d = want; g_rd[i].init = 1; }
    else {
        int64_t lim = (frame_dt > 1000000 ? 1000000 : frame_dt < 0 ? 0 : frame_dt) / 20 + 1, d = want - g_rd[i].d;
        g_rd[i].d += d > lim ? lim : d < -lim ? -lim : d;
    }
    return g_rd[i].d;
}
static float g_lp[3];
static int g_lp_ok;

static void cars_frame(CPU *c, int in_roam, int64_t host, int64_t frame_dt){
    /* cars the game took away (a world reload): forgotten */
    for (int k = 0; k < MAX_CARS; k++)
        if (g_car[k].st && !vehicle_listed(g_car[k].rb)){
            fprintf(stderr, "[roam] f=%ld %s's car is gone from the game's vehicles: forgotten\n", autotest_frame(), g_car[k].name);
            R.lost++; memset(&g_car[k], 0, sizeof g_car[k]);
        }
    for (int k = 0; k < g_ntest; k++) if (g_test[k].st && !vehicle_listed(g_test[k].rb)) memset(&g_test[k], 0, sizeof g_test[k]);
    if (!in_roam){
        for (int k = 0; k < MAX_CARS; k++) if (g_car[k].st) car_kill(c, &g_car[k], "not in free roam");
        return;
    }
    if (!g_pc_ready) player_car_keys(c);
    /* what the session has */
    g_nv = 0;
    roam_plat_lock();
    for (int i = 0; i < RC_TABLE; i++){
        rc_peer *p = &g_s.peer[i];
        if (p->state != RC_PS_EST) continue;
        View *v = &g_v[g_nv++];
        memset(v, 0, sizeof *v);
        v->idx = i; v->id = p->id; snprintf(v->name, sizeof v->name, "%s", p->name);
        v->visible = !p->lost && p->mo.n > 0 && rc_peer_listed(&g_s, i) &&
                     (p->status == RC_ST_PAUSED || (p->status == RC_ST_ROAM && host - p->last_snap_rx < VISIBLE_US));
        if (p->cfg_len){
            static rc_carcfg cc;
            if (rc_carcfg_decode(p->cfg, p->cfg_len, &cc) == RC_OK){
                int pc = player_car(cc.type_hash);
                v->cfg_gen = p->cfg_gen;
                if (pc >= 0){ v->key = cc.type_hash; snprintf(v->car, sizeof v->car, "%s", PLAYER_CARS[pc]); }
                else { v->bad = 1; v->bad_key = cc.type_hash; snprintf(v->car, sizeof v->car, "%s", cc.type_name); }
            }
        }
        int mine = -1;
        for (int k = 0; k < MAX_CARS; k++) if (g_car[k].st && g_car[k].peer == i && g_car[k].peer_id == p->id) mine = k;
        if (v->visible){
            if (mine >= 0) v->have_pose = rc_interp_eval(&p->mo, (int64_t)R.sync, frame_dt, render_delay(i, p, frame_dt), 250000, &v->pose);
            else v->have_pose = rc_interp_peek(&p->mo, p->mo.t[p->mo.n - 1], 0, &v->pose);
        }
        float dx = v->pose.p[0] - g_lp[0], dy = v->pose.p[1] - g_lp[1];
        v->dist = v->have_pose ? sqrtf(dx * dx + dy * dy) : 1e9f;
    }
    roam_plat_unlock();
    /* a record naming no player car: said once a record */
    for (int i = 0; i < g_nv; i++){
        View *v = &g_v[i];
        if (!v->bad || (R.bad_id[v->idx] == v->id && R.bad_gen[v->idx] == v->cfg_gen)) continue;
        R.bad_id[v->idx] = v->id; R.bad_gen[v->idx] = v->cfg_gen; R.refused++;
        fprintf(stderr, "[roam] f=%ld %s's car record names a vehicle that is no player car (\"%s\", key %08x): no car for it\n",
                autotest_frame(), v->name, v->car, v->bad_key);
    }
    /* the cars we have: posed, or away */
    for (int k = 0; k < MAX_CARS; k++){
        Car *car = &g_car[k];
        if (!car->st) continue;
        View *v = 0;
        for (int i = 0; i < g_nv; i++) if (g_v[i].idx == car->peer && g_v[i].id == car->peer_id) v = &g_v[i];
        if (!v){ car_kill(c, car, "left"); continue; }
        if (v->bad){ car_kill(c, car, "no player car in its record"); continue; }
        if (v->key && v->key != car->key){ car_kill(c, car, "another car"); continue; }
        if (!v->visible || !v->have_pose){
            if (host - car->seen_us > GONE_US) car_kill(c, car, "not seen for 10 s");
            continue;
        }
        if (g_lp_ok && v->dist > FAR_M){
            if (!car->far_us) car->far_us = host;
            else if (host - car->far_us > FAR_US){ car_kill(c, car, "far away"); continue; }
        } else car->far_us = 0;
        car->seen_us = host;
        car_pose(c, car, v->pose.p, v->pose.q, v->pose.v, v->pose.w, v->pose.steer, v->pose.throttle, v->pose.brake);
        float dx = v->pose.p[0] - car->p0[0], dy = v->pose.p[1] - car->p0[1], dz = v->pose.p[2] - car->p0[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > car->moved) car->moved = d;
        if (R.poselog){
            float f[3], l[3], u[3]; rc_q2m(v->pose.q, f, l, u);
            fprintf(R.poselog, "R,%lld,%.0f,%08x,%.4f,%.4f,%.4f,%.3f,%d,0,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", (long long)host, R.sync,
                    car->peer_id, (double)v->pose.p[0], (double)v->pose.p[1], (double)v->pose.p[2], (double)v->pose.speed, v->pose.mode,
                    (double)f[0], (double)f[1], (double)f[2], (double)u[2], (double)v->pose.p[0], (double)v->pose.p[1], (double)v->pose.p[2]);
        }
    }
    /* a new car: the nearest friend without one, one a frame */
    int n = 0;
    for (int k = 0; k < MAX_CARS; k++) n += g_car[k].st != 0;
    if (n >= R.max_cars) return;
    View *best = 0;
    for (int i = 0; i < g_nv; i++){
        View *v = &g_v[i];
        if (!v->visible || !v->have_pose || !v->key || !g_lp_ok || v->dist > NEAR_M) continue;
        int has = 0;
        for (int k = 0; k < MAX_CARS; k++) if (g_car[k].st && g_car[k].peer == v->idx && g_car[k].peer_id == v->id) has = 1;
        if (has) continue;
        if (R.retry_id[v->idx] == v->id && host < R.retry_us[v->idx]) continue;
        if (!best || v->dist < best->dist) best = v;
    }
    if (!best) return;
    int slot = -1;
    for (int k = 0; k < MAX_CARS && slot < 0; k++) if (!g_car[k].st) slot = k;
    if (slot < 0) return;
    float ps[3], fw[3], lw[3], uw[3], fs[3];
    w2s(best->pose.p, ps);
    rc_q2m(best->pose.q, fw, lw, uw); w2s(fw, fs);
    uint32_t sim = make_car(c, best->key, ps, fs);
    Car *car = &g_car[slot];
    if (!sim || !car_bind(c, car, sim)){
        R.create_fail++;
        int st = sim ? 7 : g_cons.step;
        R.fail_at[st & 7]++;
        /* one line per friend and refusing check (asked again every RETRY_US, quietly while the answer stays) */
        if (R.retry_id[best->idx] != best->id || R.retry_step[best->idx] != st){
            char why[96];
            fprintf(stderr, "[roam] f=%ld %s's car (%s, key %08x), %.1f m away: the game made none %s (sim 0x%08x; rigid bodies %u of %u)\n",
                    autotest_frame(), best->name, best->car, best->key, (double)best->dist, cons_why(why, sizeof why), sim, rd(G_RB_COUNT), RB_MAX);
        }
        R.retry_id[best->idx] = best->id; R.retry_us[best->idx] = host + RETRY_US; R.retry_step[best->idx] = st;
        if (sim) vcall(c, sim, SIM_KILL, 0, 0);
        memset(car, 0, sizeof *car);
        return;
    }
    car->st = 1; car->peer = best->idx; car->peer_id = best->id; car->key = best->key; car->seen_us = host;
    car->cfg_gen = best->cfg_gen;
    snprintf(car->name, sizeof car->name, "%s", best->name); snprintf(car->car, sizeof car->car, "%s", best->car);
    memcpy(car->p0, best->pose.p, sizeof car->p0);
    R.creates++;
    car_pose(c, car, best->pose.p, best->pose.q, best->pose.v, best->pose.w, best->pose.steer, best->pose.throttle, best->pose.brake);
    fprintf(stderr, "[roam] f=%ld %s's car: %s (key %08x), ISimable 0x%08x, driver class %u, active %u, %.1f m away; rigid bodies %u of %u, "
            "vehicles %u\n", autotest_frame(), car->name, car->car, car->key, sim, vcall(c, car->veh, VEH_CLASS, 0, 0) ,
            vcall(c, car->veh, VEH_ACTIVE, 0, 0) & 0xffu, (double)best->dist, rd(G_RB_COUNT), RB_MAX, rd(G_VEHLIST + 8u));
}

/* ---- the test cars of `roam spawn`: posed where they were made, every frame ---- */
typedef struct { float p[3], q[4]; } TestPose;
static TestPose g_tpose[8];
static void test_frame(CPU *c){
    for (int k = 0; k < g_ntest; k++){
        if (!g_test[k].st) continue;
        float z[3] = { 0, 0, 0 };
        car_pose(c, &g_test[k], g_tpose[k].p, g_tpose[k].q, z, z, 0, 0, 0);
    }
}

/* ---- the queued script work (guest calls at the anchor) ---- */
enum { Q_NONE, Q_PROBE, Q_SPAWN, Q_UNSPAWN, Q_POOLS, Q_STATUS, Q_DROP };
static struct { int op; char arg[64]; } Q;

/* the game's vehicles of driver class DRIVER_NONE, as ours are (the list at G_SIMLIST holds their ISimables): a car we
 * took away and the game kept would stay in it */
static unsigned class4_vehicles(CPU *c){
    uint32_t b = rd(G_SIMLIST), n = rd(G_SIMLIST + 8u);
    unsigned k = 0;
    for (uint32_t i = 0; b && i < n && i < 256u; i++){
        uint32_t s = rd(b + 4u * i), veh = s ? query(c, s, IID_IVEHICLE) : 0u;
        if (veh && vcall(c, veh, VEH_CLASS, 0, 0) == DRIVER_NONE) k++;
    }
    return k;
}
static void pools_line(CPU *c, const char *why){
    int ours = 0;
    for (int k = 0; k < MAX_CARS; k++) ours += g_car[k].st != 0;
    for (int k = 0; k < g_ntest; k++) ours += g_test[k].st != 0;
    char b[512]; int o = 0;
    o += snprintf(b + o, sizeof b - (size_t)o, "[roampool] f=%ld %s: our cars %d, rigid bodies %u of %u, rigid-body list %u, vehicles %u"
                  " (driver class 4: %u)", autotest_frame(), why, ours, rd(G_RB_COUNT), RB_MAX, rd(G_VEHLIST + 8u), rd(G_SIMLIST + 8u),
                  class4_vehicles(c));
    for (uint32_t i = 0; i < 16u && o < (int)sizeof b - 64; i++){
        uint32_t pp = rd(G_POOLS + 4u * i), nm = pp ? rd(pp) : 0u;
        if (!pp || !ok(nm, 4)) continue;
        char n[24] = {0};
        for (int j = 0; j < 23 && ok(nm + (uint32_t)j, 1) && MR8(nm + (uint32_t)j); j++) n[j] = (char)MR8(nm + (uint32_t)j);
        if (!strcmp(n, "Cars") || !strcmp(n, "Main Pool"))
            o += snprintf(b + o, sizeof b - (size_t)o, ", %s %u of %u KB in %u", n, rd(pp + 0x30u) >> 10, rd(pp + 0x2cu) >> 10, rd(pp + 0x24u));
    }
    fprintf(stderr, "%s\n", b);
}
static void probe(CPU *c){
    long f = autotest_frame();
    uint32_t sim = player_sim(c), rb = sim ? vcall(c, sim, SIM_RB, 0, 0) : 0u, d = rb ? rb_data(rb) : 0u;
    if (!d){ fprintf(stderr, "[roam] f=%ld probe: no car\n", f); return; }
    fprintf(stderr, "[roam] f=%ld probe: ISimable 0x%08x, IRigidBody 0x%08x (vtable 0x%08x), data 0x%08x\n", f, sim, rb, rd(rb), d);
    fprintf(stderr, "[roam]   quat %.4f %.4f %.4f %.4f pos %.2f %.2f %.2f vel %.2f %.2f %.2f ang %.3f %.3f %.3f speed %.2f\n",
            (double)rdf(d), (double)rdf(d + 4u), (double)rdf(d + 8u), (double)rdf(d + 12u),
            (double)rdf(d + 0x10u), (double)rdf(d + 0x14u), (double)rdf(d + 0x18u),
            (double)rdf(d + 0x20u), (double)rdf(d + 0x24u), (double)rdf(d + 0x28u),
            (double)rdf(d + 0x30u), (double)rdf(d + 0x34u), (double)rdf(d + 0x38u), (double)gcall_f(c, rd(rd(rb) + RB_GETSPEED), rb));
    for (uint32_t r = 0; r < 4u; r++)
        fprintf(stderr, "[roam]   matrix row %u: %.4f %.4f %.4f %.4f\n", r, (double)rdf(d + 0x70u + 0x10u * r), (double)rdf(d + 0x74u + 0x10u * r),
                (double)rdf(d + 0x78u + 0x10u * r), (double)rdf(d + 0x7cu + 0x10u * r));
    uint32_t in = query(c, sim, IID_IINPUT), veh = query(c, sim, IID_IVEHICLE);
    if (ok(in + IN_CTL, 0x28)){
        char b[256]; int o = 0;
        for (uint32_t i = 0; i < 0x24u; i += 4u) o += snprintf(b + o, sizeof b - (size_t)o, " +%02x=%.3f", i, (double)rdf(in + IN_CTL + i));
        fprintf(stderr, "[roam]   IInput 0x%08x (vtable 0x%08x) controls:%s bytes +20 %u +21 %u\n", in, rd(in), b,
                MR8(in + IN_CTL + 0x20u), MR8(in + IN_CTL + 0x21u));
    } else fprintf(stderr, "[roam]   no IInput\n");
    char name[RC_CFG_TYPE_NAME + 1]; uint32_t key = 0;
    if (veh) fprintf(stderr, "[roam]   IVehicle 0x%08x: %s (key %08x), driver class %u, active %u; vehicles %u, racers %u\n", veh,
                     local_car(c, sim, name, &key) ? name : "?", key, vcall(c, veh, VEH_CLASS, 0, 0), vcall(c, veh, VEH_ACTIVE, 0, 0) & 0xffu,
                     rd(G_VEHLIST + 8u), rd(G_SIMLIST + 8u));
    uint32_t vb = rd(G_VEHLIST), vn = rd(G_VEHLIST + 8u);
    char b[1024]; int o = 0;
    for (uint32_t i = 0; vb && i < vn && i < 40u && o < (int)sizeof b - 40; i++){
        uint32_t v = rd(vb + 4u * i);
        o += snprintf(b + o, sizeof b - (size_t)o, " %08x", v);
    }
    fprintf(stderr, "[roam]   the vehicles' IRigidBody pointers:%s\n", b);
    uint32_t rs = rd(G_RACESTATUS);
    fprintf(stderr, "[roam]   flow %u, IsRoaming %u, race status activity 0x%08x, racing %u\n", game_flow(),
            gcall(c, FN_IS_ROAMING, 0, 0, 0) & 0xffu, rs ? rd(rs + 0x1968u) : 0u, rs ? MR8(rs + 0x198au) : 0u);
    for (int k = 0; k < g_ntest; k++){
        Car *t = &g_test[k]; uint32_t td = t->st ? rb_data(t->rb) : 0u;
        if (!td) continue;
        float ps[3] = { rdf(td + RBD_POS), rdf(td + RBD_POS + 4u), rdf(td + RBD_POS + 8u) }, pw[3]; s2w(ps, pw);
        fprintf(stderr, "[roam]   test car %d (%s): at %.2f %.2f %.2f (world), up row %.3f %.3f %.3f, %ld frames posed, the game moved it "
                "at most %.3f m between two poses, driver class %u, listed %d\n", k, t->car, (double)pw[0], (double)pw[1], (double)pw[2],
                (double)rdf(td + RBD_UP), (double)rdf(td + RBD_UP + 4u), (double)rdf(td + RBD_UP + 8u), t->frames, (double)t->drift,
                vcall(c, t->veh, VEH_CLASS, 0, 0), vehicle_listed(t->rb));
    }
    pools_line(c, "probe");
}
static void spawn(CPU *c, const char *nm){
    long f = autotest_frame();
    if (g_ntest >= 8){ fprintf(stderr, "[roam] f=%ld spawn: 8 already\n", f); return; }
    uint32_t sim = player_sim(c), rb = sim ? vcall(c, sim, SIM_RB, 0, 0) : 0u, d = rb ? rb_data(rb) : 0u;
    if (!d){ fprintf(stderr, "[roam] f=%ld spawn: no car\n", f); return; }
    char name[RC_CFG_TYPE_NAME + 1]; uint32_t key = 0;
    if (nm && *nm){                     /* any vehicle (the probe's ROAM_SPAWN: a cop's car too), not only PLAYER_CARS */
        snprintf(name, sizeof name, "%s", nm);
        key = attrib_key(c, name);
    } else if (!local_car(c, sim, name, &key)){ fprintf(stderr, "[roam] f=%ld spawn: no vehicle name\n", f); return; }
    rc_state ls;
    if (!local_state(c, &ls, 0)) return;
    float fw[3], lw[3], uw[3]; rc_q2m(ls.q, fw, lw, uw);
    float pw[3], ahead = 12.0f + 7.0f * (float)g_ntest;
    for (int i = 0; i < 3; i++) pw[i] = ls.p[i] + ahead * fw[i];
    pw[2] += 0.3f;
    float ps[3], fs[3]; w2s(pw, ps); w2s(fw, fs);
    uint32_t before = rd(G_RB_COUNT);
    uint32_t s = make_car(c, key, ps, fs);
    Car *k = &g_test[g_ntest];
    memset(k, 0, sizeof *k);
    if (!s || !car_bind(c, k, s)){
        char why[96];
        fprintf(stderr, "[roam] f=%ld spawn %s (key %08x): the game made none %s (0x%08x), rigid bodies %u\n", f, name, key,
                cons_why(why, sizeof why), s, rd(G_RB_COUNT));
        R.create_fail++;
        return;
    }
    k->st = 1; snprintf(k->name, sizeof k->name, "test%d", g_ntest); snprintf(k->car, sizeof k->car, "%s", name); k->key = key;
    memcpy(g_tpose[g_ntest].p, pw, sizeof pw); memcpy(g_tpose[g_ntest].q, ls.q, sizeof ls.q);
    g_ntest++; R.creates++;
    fprintf(stderr, "[roam] f=%ld spawn %s (key %08x): ISimable 0x%08x, IRigidBody 0x%08x, IInput 0x%08x, IVehicle 0x%08x, driver class %u, "
            "active %u, listed %d; rigid bodies %u -> %u\n", f, name, key, s, k->rb, k->in, k->veh, vcall(c, k->veh, VEH_CLASS, 0, 0),
            vcall(c, k->veh, VEH_ACTIVE, 0, 0) & 0xffu, vehicle_listed(k->rb), before, rd(G_RB_COUNT));
}
static void status(CPU *c){
    long f = autotest_frame();
    roam_plat_lock();
    fprintf(stderr, "[roam] f=%ld mp status: %s, %d friend(s), my car %s (key %08x), my record rev %u\n", f, rc_status_name(g_s.status),
            rc_established(&g_s), R.car, R.key, g_s.cfg_rev);
    for (int i = 0; i < RC_TABLE; i++){
        const rc_peer *p = &g_s.peer[i]; char b[16];
        if (p->state == RC_PS_FREE) continue;
        fprintf(stderr, "[roam]   peer %d %s:%u id %08x state %d %s (%s) record rev %u%s, %d states, rtt %.1f ms\n", i, roam_ip_str(p->ip, b),
                p->port, p->id, p->state, p->name, rc_status_name(p->status), p->cfg_rev, p->cfg_len ? "" : " (none yet)", p->mo.n,
                p->rtt_us / 1000.0);
    }
    roam_plat_unlock();
    for (int k = 0; k < MAX_CARS; k++){
        Car *car = &g_car[k];
        if (!car->st) continue;
        uint32_t d = rb_data(car->rb);
        float ps[3] = { d ? rdf(d + 0x10u) : 0, d ? rdf(d + 0x14u) : 0, d ? rdf(d + 0x18u) : 0 }, pw[3];
        s2w(ps, pw);
        uint32_t ctl = car->in ? car->in + IN_CTL : 0u;
        int hc = ok(ctl, 0x24);
        fprintf(stderr, "[roam]   car %d: %s's %s, %ld frames posed, at %.1f %.1f %.1f (world), moved %.1f m, speed %.1f m/s, driver class %u, "
                "listed %d, its controls steering %.2f gas %.2f brake %.2f (changed by the game in %ld frames), the game moved it at "
                "most %.3f m between two poses\n",
                k, car->name, car->car, car->frames, (double)pw[0], (double)pw[1], (double)pw[2], (double)car->moved,
                (double)gcall_f(c, rd(rd(car->rb) + RB_GETSPEED), car->rb), vcall(c, car->veh, VEH_CLASS, 0, 0), vehicle_listed(car->rb),
                hc ? (double)rdf(ctl + CTL_STEER) : 0.0, hc ? (double)rdf(ctl + CTL_GAS) : 0.0, hc ? (double)rdf(ctl + CTL_BRAKE) : 0.0,
                car->ctl_changed, (double)car->drift);
    }
    pools_line(c, "status");
}

/* ---- every frame ---- */
static void queued(CPU *c, int in_roam){
    if (in_roam) test_frame(c);
    switch (Q.op){
    case Q_PROBE: probe(c); break;
    case Q_SPAWN: spawn(c, Q.arg); break;
    case Q_UNSPAWN:
        for (int k = 0; k < g_ntest; k++) if (g_test[k].st) car_kill(c, &g_test[k], "unspawn");
        g_ntest = 0; break;
    case Q_POOLS: pools_line(c, "pools"); break;
    case Q_STATUS: if (R.on) status(c); break;
    case Q_DROP:
        for (int k = 0; k < MAX_CARS; k++) if (g_car[k].st) car_kill(c, &g_car[k], "dropped (roam mp drop)");
        break;
    default: break;
    }
    Q.op = Q_NONE;
}
void roam_mw_frame(CPU *c){
    if (!R.inited) init_once();
    if (!R.on && !Q.op && !g_ntest) return;
    int in_roam = roaming(c);
    if (!R.on){ queued(c, in_roam); return; }
    int64_t host = roam_plat_now_us();
    int64_t frame_dt = clock_step(in_roam, host);
    rc_state ls; uint32_t sim = 0;
    int have = in_roam && local_state(c, &ls, &sim);
    if (have){ ls.t_us = (uint32_t)(int64_t)R.sync; memcpy(g_lp, ls.p, sizeof g_lp); g_lp_ok = 1; }
    /* the car's record: its vehicle's name and key, every 5 s while in free roam (sent when it changed) */
    uint8_t rec[RC_CFG_MAX]; int recn = 0;
    if (have && host >= R.cfg_next){
        R.cfg_next = host + 5000000;
        char name[RC_CFG_TYPE_NAME + 1]; uint32_t key = 0;
        if (local_car(c, sim, name, &key)){
            if (key != R.key || strcmp(name, R.car)){
                fprintf(stderr, "[roam] f=%ld my car: %s (key %08x)\n", autotest_frame(), name, key);
                R.key = key; snprintf(R.car, sizeof R.car, "%s", name);
            }
            rc_carcfg cc; memset(&cc, 0, sizeof cc);
            cc.type_hash = key; snprintf(cc.type_name, sizeof cc.type_name, "%s", name);
            recn = rc_carcfg_encode(&cc, rec, sizeof rec);
        }
    }
    roam_plat_lock();
    uint8_t st = in_roam ? (R.parked ? RC_ST_PAUSED : RC_ST_ROAM) : game_flow() == 6u ? RC_ST_EVENT : game_flow() == 4u || game_flow() == 5u
                 ? RC_ST_LOADING : RC_ST_MENUS;
    rc_set_status(&g_s, st);
    if (recn > 0) rc_set_cfg(&g_s, rec, recn, host);
    if (have) rc_push_state(&g_s, &ls, host);
    roam_plat_unlock();
    if (have && R.poselog){
        float f[3], l[3], u[3]; rc_q2m(ls.q, f, l, u);
        fprintf(R.poselog, "L,%lld,%.0f,%08x,%.4f,%.4f,%.4f,%.3f,0,0,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", (long long)host, R.sync, R.local_id,
                (double)ls.p[0], (double)ls.p[1], (double)ls.p[2], (double)ls.speed, (double)f[0], (double)f[1], (double)f[2], (double)u[2],
                (double)ls.p[0], (double)ls.p[1], (double)ls.p[2]);
    }
    cars_frame(c, in_roam, host, frame_dt);
    queued(c, in_roam);
}

/* ---- what the core's other files ask of the game side ---- */
void roam_game_event(int ev, int peer, const char *text){
    if (ev == RC_EV_JOIN) __atomic_fetch_add(&R.joins, 1, __ATOMIC_RELAXED);
    if (ev == RC_EV_LEAVE) __atomic_fetch_add(&R.leaves, 1, __ATOMIC_RELAXED);
    fprintf(stderr, "[roam] %s%s (peer %d)\n", ev == RC_EV_JOIN ? "JOIN: " : ev == RC_EV_LEAVE ? "LEAVE: " : "", text, peer);
}
int roam_game_racing(void){ return 0; }
void roam_game_parked(int on){
    R.parked = on;
    if (!R.on || !R.net) return;
    roam_plat_lock();
    if (on && g_s.status == RC_ST_ROAM) rc_set_status(&g_s, RC_ST_PAUSED);
    roam_plat_unlock();
}
void roam_game_report(void){
    if (!R.on || !R.net) return;
    roam_plat_stats ps; roam_plat_get_stats(&ps);
    fprintf(stderr, "\n  [roam] free-roam multiplayer (Most Wanted): joins %u, leaves %u, cars made %u, the game made none %u (by check:"
            " attributes %u, preset %u, shape %u, 0x669080 %u, 0x687710 %u, room %u, other %u), taken away %u, lost with the world %u, "
            "records of no player car %u\n",
            R.joins, R.leaves, R.creates, R.create_fail, R.fail_at[1], R.fail_at[2], R.fail_at[3], R.fail_at[4], R.fail_at[5], R.fail_at[6],
            R.fail_at[0] + R.fail_at[7], R.kills, R.lost, R.refused);
    fprintf(stderr, "  [roam] datagrams sent %llu (%llu bytes), received %llu (%llu bytes), blocked %llu, send errors %llu\n",
            (unsigned long long)ps.sent, (unsigned long long)ps.sent_bytes, (unsigned long long)ps.recv, (unsigned long long)ps.recv_bytes,
            (unsigned long long)ps.blocked, (unsigned long long)ps.send_err);
    if (R.poselog) fflush(R.poselog);
    roam_plat_stop(0);
}

/* ---- script commands ("roam ..."; autotest.c hands them over) ---- */
static int count_shown(void){ int n = 0; for (int k = 0; k < MAX_CARS; k++) n += g_car[k].st && g_car[k].frames > 0; return n; }
int roam_cmd(CPU *c, const char *line, long *wait_start){
    (void)c;
    char sub[32] = {0}, a1[64] = {0}, a2[64] = {0}, a3[64] = {0}; int used = 0;
    sscanf(line, "%31s %n", sub, &used);
    long f = autotest_frame();
    if (!strcasecmp(sub, "mp")){
        char w[32] = {0}; int u2 = 0;
        sscanf(line + used, "%31s %n", w, &u2);
        sscanf(line + used + u2, "%63s %63s %63s", a1, a2, a3);
        if (!R.on){ fprintf(stderr, "[roam] f=%ld mp %s: multiplayer is off (NFSU2_ROAM=1)\n", f, w); return 1; }
        if (!strcasecmp(w, "status")){ if (Q.op) return 0; Q.op = Q_STATUS; return 1; }
        if (!strcasecmp(w, "drop")){ if (Q.op) return 0; Q.op = Q_DROP; return 1; }
        if (!strcasecmp(w, "waitpeers") || !strcasecmp(w, "waitcars") || !strcasecmp(w, "waitmoved")){
            int want = atoi(a1);
            float metres = !strcasecmp(w, "waitmoved") ? (float)atof(a2) : 0.f;
            long tmo = atol(!strcasecmp(w, "waitmoved") ? a3 : a2);
            if (*wait_start < 0) *wait_start = f;
            int have = 0;
            if (!strcasecmp(w, "waitpeers")){ roam_plat_lock(); have = rc_established(&g_s); roam_plat_unlock(); }
            else if (!strcasecmp(w, "waitcars")) have = count_shown();
            else for (int k = 0; k < MAX_CARS; k++) have += g_car[k].st && g_car[k].moved >= metres;
            if (have >= want){ fprintf(stderr, "[roam] f=%ld mp %s %d satisfied after %ld frames\n", f, w, want, f - *wait_start); *wait_start = -1; return 1; }
            if (tmo && f - *wait_start >= tmo){ fprintf(stderr, "[roam] f=%ld mp %s %d TIMEOUT (have %d)\n", f, w, want, have); *wait_start = -1; return 1; }
            return 0;
        }
        fprintf(stderr, "[roam] f=%ld mp %s: not known\n", f, w);
        return 1;
    }
    sscanf(line + used, "%63s", a1);
    int op = !strcasecmp(sub, "probe") ? Q_PROBE : !strcasecmp(sub, "spawn") ? Q_SPAWN : !strcasecmp(sub, "unspawn") ? Q_UNSPAWN
           : !strcasecmp(sub, "pools") ? Q_POOLS : Q_NONE;
    if (op == Q_NONE){ fprintf(stderr, "[roam] f=%ld %s: not known\n", f, sub); return 1; }
    if (!R.inited) init_once();
    if (Q.op) return 0;                 /* the one before has not run yet */
    Q.op = op; snprintf(Q.arg, sizeof Q.arg, "%s", a1);
    return 1;
}
