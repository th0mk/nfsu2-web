/* bf_hshifter.c -- Bayview Forever's H-shifter and clutch: the game's own
 * cut "manual with clutch" gearbox, driven by a wheel's H-shifter and clutch
 * pedal (BayviewForever.asi 0.2.9, unit hshifter; DRAFT for the port).
 *
 * The game's gearbox (SPEED2.EXE v1.2 NA, verified by disassembly):
 *   RealDriver (the human driver object; ecx of everything below):
 *     +0x1E4  the wanted gear (0 R, 1 N, 2 1st .. ; -1 automatic), read every
 *             tick by the drivetrain (0x5ABDA9 -> Drivetrain_RequestGear,
 *             0x5ABDB5 writes the result back)
 *     +0x1E0  steering (word), +0x20C throttle out, +0x210 ?, +0x214 brake out
 *             (setters 0x5EB910 / 0x5EB900 / 0x5EB8F0)
 *     +0x2A8  the car; +0x2AC the input port (-1: none, 0x41E2C4 skips)
 *     +0x2B4  byte: start hold (the countdown: 0x41E37E sets wanted = N and
 *             calls 0x5EB8F0(1.0))
 *     +0x2C0  the shift mode: 0 automatic, 1 manual, 2 (other), 3 manual with
 *             clutch, 4 manual with clutch while a shift into/out of N or R
 *             runs on its own (0x41E3DE sets 4; 0x5ECD70 ends it, back to 3)
 *     +0x2DC  brake pedal in, +0x2D4 throttle pedal in, +0x2F0 byte: the
 *             brake is reversing (0x407B18), +0x2F4 clutch pedal (1.0 =
 *             engaged, 0 = pressed; pushed to Drivetrain_SetClutch 0x594910
 *             every frame in mode 3, 0x41E413)
 *   Car: +0x20 its tuning (+0x1C8 the number of forward gears: the top gear
 *        is +0x1C8 + 1 in the gear numbers above, 0x407FF5), +0x28 1 = a
 *        human's, +0x34 -> +0x4C the Drivetrain, +0x42C/+0x430 speed (m/s),
 *        +0x434 the gear it is in.
 *   Drivetrain: +0x20 the engine (+0x80 float power factor: 0x5A141E sets it
 *        to 1.0 every step in mode 3, a grind sets 0 at 0x5A163C), +0x38 the
 *        target gear, +0x3C the gear, +0x44 the clutch state (1: disengaged,
 *        Drivetrain_RequestGear needs it, 0x5AAC49; 3 engaging), +0x48 a
 *        shift pending, +0x60 the shift mode copy.
 *   0x41E2C0  RealDriver update (thiscall, 1 arg dt): MapPedals 0x407A30,
 *             steering 0x41DFC0, start hold, mode 3/4 bookkeeping, clutch,
 *             then 0x407400 (its two CALLs 0x41E429 and 0x41E466).
 *   0x407280  RealDriver_SetShiftMode(mode) (vtable 0x784FFC): 3 -> wanted =
 *             car gear, mode 3; 1 -> wanted = car gear, mode 1; 0 -> wanted
 *             -1, mode 0; else mode 2; a human car's drivetrain +0x60 = mode.
 *   0x407A30  RealDriver_MapPedals(dt): the pedals into throttle/brake; a
 *             brake held at a standstill sets +0x2F0 and wanted = R
 *             (0x407CBE), the throttle in R asks vtable+0x30 (1st gear).
 *   0x5A1400  Drivetrain_UpdateClutchState (vtable 0x7A2524, ecx only, ret).
 *   [0x89E7D8] byte: a drag race.  [0x86518C] WorldTimer, 1/4000 s.
 *
 * What the mod does (and this file, the same way):
 *   - Manual in the options (SetShiftMode(1)) becomes 3, manual with clutch
 *     (ClutchMode=1); in a drag race only with DragRaces=1 (BF needs its
 *     DragSteering on for drags, which keeps the normal driving controls).
 *   - Every RealDriver update of player 1 (before 0x407400): the lever's
 *     position -> wanted gear (clamped to the car's top gear), once the lever
 *     has been seen in a gear; the clutch pedal -> +0x2F4 (with the dead
 *     zones), once the pedal has been seen pressed past 60 %.  In mode 3, a
 *     move of the lever starts a "shift": the clutch is held pressed for the
 *     game (0) and the old gear kept wanted until the drivetrain's clutch
 *     state is 1, then the new gear is asked for; the pedal's deepest point
 *     during the shift is kept.  When the gear is in: pedal deepest point <
 *     ClutchThreshold, not into neutral, BogPower < 100 and a pedal ever
 *     seen -> the bog starts.  A shift not done in 1.5 s gives the clutch
 *     back to the pedal.  Mode 4 and (ClutchMode=0) mode 1: the lever's gear
 *     is wanted directly.  The start hold: nothing but the pedal.
 *   - The bog (after the game's UpdateClutchState of that drivetrain):
 *     engine power factor x BogPower for BogHold, then linearly back to 1
 *     over BogRecover (WorldTimer seconds).
 *   - BrakeOnlyBrakes: MapPedals of player 1 with the shifter in use (mode 3
 *     or 4, or 1 with ClutchMode=0): the game's MapPedals runs with the brake
 *     pedal at 0 and +0x2F0 cleared, so it neither reverses nor asks for R;
 *     the brake pedal and the wanted gear are put back, and the brake output
 *     (+0x214) = the brake pedal, unless the game left its "hold" state
 *     (throttle out 0 and brake out 0.5).
 *
 * Where the shifter and the clutch come from: the page (web/src/input/
 * wheel.ts) knows the wheel's shifter buttons (WheelMap.gears) and clutch
 * axis (ModelAxes.clutch); it sends the lever's position and the clutch
 * travel with the pad (PadState.shifter, PadState.clutch: spec_hw.md).
 * Nothing here talks to a device: the ini's [HShifter.Device] Name,
 * [HShifter.Shifter] buttons and [HShifter.Clutch] Axis/UpValue/DownValue
 * are the page's.
 *
 * Settings (NFSU2_BF_<SECTION>_<KEY>; the ini's defaults):
 *   NFSU2_BF_FEATURES_HSHIFTER=0               the whole feature
 *   NFSU2_BF_HSHIFTER_GEARBOX_CLUTCHMODE=1     1 manual with clutch, 0 the game's manual
 *   NFSU2_BF_HSHIFTER_GEARBOX_DRAGRACES=1
 *   NFSU2_BF_HSHIFTER_GEARBOX_BRAKEONLYBRAKES=1
 *   NFSU2_BF_HSHIFTER_CLUTCH_TOPDEADZONE=5     % 0..45
 *   NFSU2_BF_HSHIFTER_CLUTCH_BOTTOMDEADZONE=10 % 0..45
 *   NFSU2_BF_HSHIFTER_SHIFTING_CLUTCHTHRESHOLD=50  % 1..100 (0 counts as 1)
 *   NFSU2_BF_HSHIFTER_SHIFTING_BOGPOWER=0      % 0..100 (100 = no bog)
 *   NFSU2_BF_HSHIFTER_SHIFTING_BOGHOLD=400     ms 0..5000
 *   NFSU2_BF_HSHIFTER_SHIFTING_BOGRECOVER=1000 ms 0..10000
 *   NFSU2_BF_HSHIFTER_LOG_GEARBOX=0            log shifts */
#include "cpu.h"
#include "gamepad.h"
int bf_dragsteer_on(void);                  /* bf_dragsteer.c */
#include "splitscreen_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define G_DRAG        0x0089e7d8u   /* byte: a drag race */
#define G_TICKS       0x0086518cu   /* WorldTimer, 1/4000 s */
#define DRV_WANTED    0x1e4u
#define DRV_THROTTLE  0x20cu        /* throttle out */
#define DRV_BRAKE_OUT 0x214u
#define DRV_CAR       0x2a8u
#define DRV_HOLD      0x2b4u        /* byte: start hold */
#define DRV_MODE      0x2c0u
#define DRV_BRAKE_IN  0x2dcu
#define DRV_REVERSING 0x2f0u        /* byte */
#define DRV_CLUTCH    0x2f4u        /* float 1 engaged .. 0 pressed */
#define CAR_TUNING    0x20u         /* -> +0x1C8 forward gears */
#define CAR_GEAR      0x434u
#define CAR_DT        0x34u         /* -> +0x4C drivetrain */
#define DT_ENGINE     0x20u         /* -> +0x80 power factor */
#define DT_GEAR       0x3cu
#define DT_CLUTCHST   0x44u

void sub_00407280_orig(CPU *c);
void sub_00407400_orig(CPU *c);
void sub_00407a30_orig(CPU *c);
void sub_005a1400_orig(CPU *c);

/* ---- settings ---------------------------------------------------------------- */
static struct {
    int on, clutch_mode, drag, brake_only, log;
    float top_dz, bot_dz, threshold, bog_power, bog_hold, bog_recover;   /* fractions, seconds */
} C;
static int envi(const char *n, int d, int lo, int hi){
    const char *e = getenv(n);
    int v = e && *e ? atoi(e) : d;
    return v < lo ? lo : v > hi ? hi : v;
}
static void cfg(void){
    static int done;
    if (done) return;
    done = 1;
    C.on          = gamepad_env_flag("NFSU2_BF_FEATURES_HSHIFTER", 0);
    C.clutch_mode = gamepad_env_flag("NFSU2_BF_HSHIFTER_GEARBOX_CLUTCHMODE", 1);
    C.drag        = gamepad_env_flag("NFSU2_BF_HSHIFTER_GEARBOX_DRAGRACES", 1);
    C.brake_only  = gamepad_env_flag("NFSU2_BF_HSHIFTER_GEARBOX_BRAKEONLYBRAKES", 1);
    C.log         = gamepad_env_flag("NFSU2_BF_HSHIFTER_LOG_GEARBOX", 0);
    C.top_dz      = envi("NFSU2_BF_HSHIFTER_CLUTCH_TOPDEADZONE", 5, 0, 45) / 100.f;
    C.bot_dz      = envi("NFSU2_BF_HSHIFTER_CLUTCH_BOTTOMDEADZONE", 10, 0, 45) / 100.f;
    int th        = envi("NFSU2_BF_HSHIFTER_SHIFTING_CLUTCHTHRESHOLD", 50, -1000, 100);
    C.threshold   = (th <= 0 ? 1 : th) / 100.f;                       /* the mod: <= 0 -> 1 */
    C.bog_power   = envi("NFSU2_BF_HSHIFTER_SHIFTING_BOGPOWER", 0, 0, 100) / 100.f;
    C.bog_hold    = envi("NFSU2_BF_HSHIFTER_SHIFTING_BOGHOLD", 400, 0, 5000) / 1000.f;
    C.bog_recover = envi("NFSU2_BF_HSHIFTER_SHIFTING_BOGRECOVER", 1000, 0, 10000) / 1000.f;
    if (C.on)
        fprintf(stderr, "[hshifter] on: %s, drags %d, brake only brakes %d; clutch dead zones %.0f/%.0f %%, "
                "clean from %.0f %%, missed: %.0f %% power for %.0f ms, back over %.0f ms\n",
                C.clutch_mode ? "manual with clutch (mode 3)" : "manual (mode 1)", C.drag, C.brake_only,
                C.top_dz * 100, C.bot_dz * 100, C.threshold * 100, C.bog_power * 100,
                C.bog_hold * 1000, C.bog_recover * 1000);
}

/* ---- state ------------------------------------------------------------------------ */
static int g_shifter_seen, g_clutch_seen;
static struct { int on, from, to, timed_out; float deepest; uint64_t t0_ms; } g_shift;
static struct { uint32_t dt; float t0; int logged; } g_bog;
static int g_hold_was = -1;

static uint64_t now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}
static float world_s(void){ return (float)(int32_t)MR32(G_TICKS) * 0.00025f; }
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static const char *gear_name(int g){
    static const char *n[] = {"R", "N", "1", "2", "3", "4", "5", "6", "7", "8"};
    return g == -1 ? "auto" : g >= 0 && g <= 9 ? n[g] : "?";
}

/* sub_712cf490: player 1's driver (the mod: player count <= 1, or the car is
 * the first player's) */
static int is_player1(uint32_t drv){
    uint32_t car = MR32(drv + DRV_CAR);
    int32_t n = (int32_t)MR32(SPLIT_PLAYERS_COUNT);
    if (n <= 1) return 1;
    uint32_t p = MR32(SPLIT_PLAYERS_ORDER);
    return p && ok(p + 4u, 4) && MR32(p + 4u) == car;
}

/* the lever's gear in the game's numbers (0 R, 1 N, 2 1st ..), the clutch's
 * engagement for +0x2F4 and its travel (0 up .. 1 down); 0: no controller */
static int read_controls(int *wanted, float *engage, float *travel){
    PadState s;
    if (!gamepad_read(&s) || !s.wheel) return 0;
    /* lever: 0 none, 1 neutral, 2..7 1st to 6th, 8 reverse (gamepad.h); the game's: 0 R, 1 N, 2 1st .. */
    *wanted = s.lever == 0 ? -1 : s.lever == 8 ? 0 : s.lever;
    if (s.lever > 1 && !g_shifter_seen){
        g_shifter_seen = 1;
        fprintf(stderr, "[hshifter] shifter seen (gear %s): the lever sets the wanted gear\n", gear_name(*wanted));
    }
    float p = !s.has_clutch ? 0.f : s.clutch < 0.f ? 0.f : s.clutch > 1.f ? 1.f : s.clutch;
    if (s.has_clutch && p > 0.6f && !g_clutch_seen){
        g_clutch_seen = 1;
        fprintf(stderr, "[hshifter] clutch pedal seen: it now drives the game's clutch\n");
    }
    /* the dead zones: up to TopDeadzone fully engaged, past 1 - BottomDeadzone fully pressed */
    float e = 1.f;
    if (p > C.top_dz) e = p >= 1.f - C.bot_dz ? 0.f : 1.f - (p - C.top_dz) / (1.f - C.top_dz - C.bot_dz);
    *engage = e; *travel = p;
    return 1;
}
static int clamp_top(uint32_t car, int wanted){
    uint32_t t = MR32(car + CAR_TUNING);
    if (t && ok(t + 0x1c8u, 4)){ int top = (int)MR32(t + 0x1c8u) + 1; if (wanted > top) wanted = top; }
    return wanted;
}

/* ---- RealDriver_SetShiftMode (vtable 0x784FFC) ------------------------------------ */
void sub_00407280(CPU *c){
    cfg();
    uint32_t mode = MR32(c->esp + 4u);
    PadState ps;
    /* the port: only with a wheel whose shifter the page knows (a controller's player keeps the game's Manual) */
    int lever = gamepad_read(&ps) && ps.wheel && ps.lever > 0;
    if (C.on && mode == 1u && C.clutch_mode && lever){
        int drag = MR8(G_DRAG) == 1;
        if (!drag || (C.drag && bf_dragsteer_on())) MR32(c->esp + 4u) = 3u;   /* drags: with the normal steering only */
        if (C.log) fprintf(stderr, "[hshifter] shift mode 1%s for driver 0x%08x -> %u\n",
                           drag ? " (drag race)" : "", c->ecx, MR32(c->esp + 4u));
    }
    g_shift.on = 0;
    sub_00407280_orig(c);
}

/* ---- the RealDriver update's tail (0x41E429 / 0x41E466 CALL 0x407400) -------------- */
static void driver_update(uint32_t drv){
    if (!ok(drv, 0x300) || !is_player1(drv)) return;
    uint32_t car = MR32(drv + DRV_CAR);
    if (!ok(car, 0x438)) return;
    int wanted; float e, p;
    if (!read_controls(&wanted, &e, &p)) return;            /* the mod: device lost -> nothing */
    int mode = (int)MR32(drv + DRV_MODE);
    int hold = MR8(drv + DRV_HOLD) != 0;
    if (hold != g_hold_was){ g_hold_was = hold; if (C.log) fprintf(stderr, "[hshifter] start hold %s\n", hold ? "on" : "off"); }
    if (hold){                                            /* the game keeps neutral */
        g_shift.on = 0;
        if (mode == 3 && g_clutch_seen) MRF32(drv + DRV_CLUTCH) = e;
        return;
    }
    uint32_t dt = MR32(car + CAR_DT);
    dt = dt && ok(dt + 0x4cu, 4) ? MR32(dt + 0x4cu) : 0u;
    if (dt && !ok(dt, 0x64)) dt = 0u;
    if (mode == 3 && dt && g_shifter_seen){
        wanted = clamp_top(car, wanted);
        int cur = (int)MR32(dt + DT_GEAR);
        float out = g_clutch_seen ? e : 1.f;
        if (cur == wanted){
            if (g_shift.on){                              /* the gear is in */
                g_shift.on = 0;
                int clean = g_shift.deepest >= C.threshold;
                int bog = g_clutch_seen && !clean && wanted != 1 && C.bog_power < 1.f && C.bog_hold + C.bog_recover > 0.f;
                if (bog){ g_bog.dt = dt; g_bog.t0 = world_s(); g_bog.logged = 0; }
                if (C.log)
                    fprintf(stderr, "[hshifter] shift %s -> %s in %llu ms: clutch pedal %d %%%s\n",
                            gear_name(g_shift.from), gear_name(wanted), (unsigned long long)(now_ms() - g_shift.t0_ms),
                            (int)(g_shift.deepest * 100.f + 0.5f),
                            bog ? " -> missed clutch, bog" : !clean && g_clutch_seen ? " (into neutral: no penalty)" : "");
            }
        } else {
            if (!g_shift.on || g_shift.to != wanted){     /* a new shift */
                g_shift.on = 1; g_shift.from = cur; g_shift.to = wanted;
                g_shift.deepest = 0.f; g_shift.t0_ms = now_ms(); g_shift.timed_out = 0;
            }
            if (p > g_shift.deepest) g_shift.deepest = p;
            if (!g_shift.timed_out && now_ms() - g_shift.t0_ms > 1500u){
                g_shift.timed_out = 1;
                if (C.log) fprintf(stderr, "[hshifter] shift %s -> %s not done in 1500 ms: the clutch is the pedal's again\n",
                                   gear_name(g_shift.from), gear_name(wanted));
            }
            if (!g_shift.timed_out){
                /* press the clutch for the game; ask for the new gear once it is out */
                out = 0.f;
                if ((int)MR32(dt + DT_CLUTCHST) != 1) wanted = cur;
            }
        }
        MR32(drv + DRV_WANTED) = (uint32_t)wanted;
        MRF32(drv + DRV_CLUTCH) = out;
        return;
    }
    if (mode == 3 || mode == 4 || (mode == 1 && !C.clutch_mode)){
        if (mode != 1 && g_clutch_seen) MRF32(drv + DRV_CLUTCH) = e;
        if (g_shifter_seen) MR32(drv + DRV_WANTED) = (uint32_t)clamp_top(car, wanted);
    }
}
void sub_00407400(CPU *c){
    cfg();
    if (C.on) driver_update(c->ecx);
    sub_00407400_orig(c);
}

/* ---- RealDriver_MapPedals (CALL at 0x41E332) --------------------------------------- */
void sub_00407a30(CPU *c){
    cfg();
    uint32_t drv = c->ecx;
    int mode = ok(drv, 0x300) ? (int)MR32(drv + DRV_MODE) : -1;
    if (!C.on || !C.brake_only || !g_shifter_seen || !(mode == 3 || mode == 4 || (mode == 1 && !C.clutch_mode)) ||
        !is_player1(drv)){
        sub_00407a30_orig(c);
        return;
    }
    uint32_t brake = MR32(drv + DRV_BRAKE_IN), wanted = MR32(drv + DRV_WANTED);
    MR8(drv + DRV_REVERSING) = 0;
    MR32(drv + DRV_BRAKE_IN) = 0;
    sub_00407a30_orig(c);                                   /* thiscall, ret 4: pops its own argument */
    MR32(drv + DRV_BRAKE_IN) = brake;
    MR32(drv + DRV_WANTED) = wanted;
    if (!(MRF32(drv + DRV_THROTTLE) == 0.f && MRF32(drv + DRV_BRAKE_OUT) == 0.5f))
        MR32(drv + DRV_BRAKE_OUT) = brake;
}

/* ---- Drivetrain_UpdateClutchState (vtable 0x7A2524): the bog ------------------------ */
void sub_005a1400(CPU *c){
    uint32_t dt = c->ecx;
    sub_005a1400_orig(c);
    if (!g_bog.dt || g_bog.dt != dt) return;
    float t = world_s() - g_bog.t0, f;
    if (t < 0.f){ g_bog.dt = 0; return; }
    if (t < C.bog_hold) f = C.bog_power;
    else if (C.bog_recover <= 0.f || t >= C.bog_hold + C.bog_recover){ g_bog.dt = 0; return; }
    else f = C.bog_power + (1.f - C.bog_power) * (t - C.bog_hold) / C.bog_recover;
    if (f >= 1.f){ g_bog.dt = 0; return; }
    uint32_t eng = MR32(dt + DT_ENGINE);
    if (!ok(eng + 0x80u, 4)) return;
    if (!g_bog.logged && C.log){ g_bog.logged = 1; fprintf(stderr, "[hshifter] bog: power factor %.2f x %.2f\n", MRF32(eng + 0x80u), f); }
    MRF32(eng + 0x80u) *= f;
}
