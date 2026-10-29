/* framerate_mw.c -- Most Wanted's frame rate: the host's slot pacer in place of the game's frame limiter,
 * and the simulation's steps counted from the pacer's slots.
 *
 * The game's loop (GAME.md 6.3).  WinMain's loop (0x666606) calls MainLoop 0x663D30 as soon as its QPC
 * ticker (0x45CE40, ms) has moved by more than 0.25 ms: no cap there.  MainLoop:
 *   0x661280(dt s)  the frame's real time: carry + dt -> ticks of 1/3600 s (cap 4/60 s), RealTimer units
 *                   of 1/4000 s with a carry in the other domain; [0x9259BC] = ticks / 3600, the frame's
 *                   real dt.  RealTimer [0x925B14] takes it at MainLoop's end (0x65C6D0), with
 *                   RealTimeFrames (1/60 units, at least 1 a frame).
 *   0x642E60        the Sim timer (thiscall, ecx = [0x9203B0], [esp+4] = the byte [0x925854]: 4 steps),
 *                   limiter and fixed-step accumulator in one:
 *                     dt = (RealTimer - [+0x14]) / 4000; [+0x14] = RealTimer
 *                     if dt < P ([+0x20] = 1/60, the constructor's argument pushed at 0x666100):
 *                         SleepEx(trunc((P - dt) * 1000) ms)          (0x64CF00, called from 0x642ED2)
 *                     n = trunc(dt / P + [+0x1C]), [+0x1C] = the rest; 0x642DB0(n) runs n steps of P
 *                   (at most 6, 2 while [0x9203AC] is set, 1 while [+0x24] is; mode 5 unlimited; its count
 *                   in [+0x28]) and it returns n * P, the world's time step (0x64A680), n before those
 *                   limits.  0x661280's cap keeps n at 5 at most: dt is 4/60 s and a carry.
 * RealTimer there still holds the frame before's time, so the sleep answers the frame before the last:
 * frame times d alternate around d(k+1) = w + P - d(k-1) (w the work), on average (P + w) / 2, and every
 * sleep is in whole ms on the browser's timers (Windows Chrome's workers wake on its 15.6 ms timer).
 * That is the cap: 102-108 fps headless on the main menu (NFSU2_FPS=off; w 2.2-3.3 ms a frame), 85-91 fps
 * headless in races (w 5.5-7 ms) and 67-71 fps in Windows Chrome, menus and races.
 * At exactly 1/60 a frame the accumulator sits on its boundary (RealTimer moves 67, 67, 66 units: 0.99
 * + 0.01 steps), and a float rounding decides between 1, 1, 1 and 2, 1, 0 steps: only the clock's own
 * jitter keeps the original off it.
 *
 * Here (NFSU2_FPS, as UG2's runtime/framerate.c: auto = the display's rate, >= 119 Hz 120, >= 239 Hz 240,
 * <= 31 Hz 30, else 60):
 *   - MainLoop's 0x661280 (not MiniMainLoop's: the loading screens sleep 8 ms a pass themselves) waits for
 *     the next slot of the content period on the grid locked to the page's display clock (the rAF vsync
 *     feed, gpu_web.c), sleeping with the guest lock released (win32_ext.c guest_sleep_frame_ns: the
 *     learned spin margin and the timer probe), and the frame's dt is its slots x the period, exactly;
 *     a frame stands for the game's own cap at most (4/60 s: [0x8F8304] x [0x8970F0], or x [0x892B64] =
 *     0.02 s while [0x8F82F0] is 0; 4 slots at 60, 8 at 120): after a hitch the slots beyond it are
 *     dropped with the missed ones, so the steps the slots owe stay within what the original steps and
 *     no debt carries over (UG2's framerate.c keeps the game's 50 ms clamp the same way);
 *   - the frame's real time exact (as UG2's framerate.c F1 ticks fix and its RealTimer fix): 0x661280's
 *     carry is 0 on a paced frame, so its ticks are slots x 3600 / R exactly (30 a slot at 120, 15 at 240)
 *     and [0x9259BC] = ticks / 3600; its carry in the other domain (1/4000 s units) had made the ticks
 *     30, 30, 31 at 120 (1.1 % fast), 15, 15, 14 at 240 (2.2 % slow) and 60, 60, 59 at 60 (0.6 % slow),
 *     which RealTimeFrames and every reader of [0x9259BC] took.  RealTimer then moves by
 *     round(ticks x 4000 / 3600) with the fraction carried here (33, 33, 34 at 120): without that carry
 *     it would run 1 % slow at 120 (33 units a frame);
 *   - the step's own sleep is skipped (0x64CF00 from 0x642ED7);
 *   - the step count follows the slots exactly: the slots so far x S / R steps (S the sim rate 1/P, R the
 *     content rate), given to the original through its carry, so 60 is 1 step a frame, 120 is 0, 1, 0, 1,
 *     30 is 2 a frame; the game's own +0x18 total, +0x14 time, 0x642DB0 call and return are its own.
 * A world (a race, free roam: GameFlow 6) steps at the page's rate: P = 1/R for R above 60 (1/60 at 60 and
 * 30), set when the front end is left for the world's loading and kept until the front end comes back, so a
 * frame is one step at 60, 120 and 240 (two at 30), as UG2's races.  The front end keeps the game's 1/60 and
 * the camera fix below.  A world at a step under 1/60 needs the fixes of runtime/mw/hfr_mw.c (the scheduler's
 * tasks at their 60 Hz cadence: the traffic that stopped for good above about 40 Hz, WidescreenFixesPack
 * #1218).  In a world the content rate is at most its steps a second: a frame that did not step would show
 * the one before (its cameras move only with the steps, 0x64A680), so a rate the page takes up after the
 * world began waits for the next world.
 *
 * Two clocks that counted frames, fixed for content rates above 60 (the original runs them at 60 at most
 * with vsync on; this port's limiter ran them at 70-110 before this file):
 *   RealTimeFrames [0x8F82F4] (1/60 s units, 21 readers: cameras, effects): 0x65C6D0 counts
 *     n = trunc(60 (dt + carry)) with the carry, then makes n at least 1: one a frame above 60, twice as
 *     fast at 120.  The forced 1 goes again (as UG2's framerate.c F5): the game's own exact 60 Hz count.
 *   The front end's cameras (0x64A680, GameFlow 3): a frame that steps gets the steps' time
 *     ([0x925E9C], 0x480E40 and 0x480C10), a frame that does not step gets 0.01 s, a fixed time a frame:
 *     at 120 (0, 1, 0, 1 steps) the cameras ran 1.6 times as fast.  Here a frame that does not step gives
 *     them its own time (its slots / R), and the next step's time is that much less: the cameras move
 *     every frame, on the wall clock.
 *
 * Options:
 *   NFSU2_FPS = auto | 30 | 60 | 120 | 240 | uncapped | off    (aliases native = auto, max = uncapped,
 *               vanilla = game = off) off: the game's own limiter, as before this file; uncapped: no pacer,
 *               no game limiter (benchmarks), the steps from the game's accumulator
 *   NFSU2_HFR_LOCKSTEP = 1     test only: every frame is exactly one slot of game time and steps, whatever
 *                              the wall clock did (NFSU2_HFR_LOCKSTEP_WALL=<fps> paces the wall at that rate)
 *   NFSMW_SIM_HZ = <hz>        research only (M7-21): a world's steps a second (the page's rate, at least
 *                              60, otherwise); 0 or unset = that
 *   NFSMW_RACE_HFR = 1         research only (M7-21): in a world too the page's rate when it is above the
 *                              world's steps (frames that do not step show the one before)
 *   NFSU2_HFR_LOG = 1          one [mwpace] line a second: frames, slots, missed, the steps 0x642DB0 ran a
 *                              frame (its [+0x28] count), frames cut to the cap, sleep, work
 */
#include "cpu.h"
#include "game.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>

void guest_sleep_frame_ns(int64_t ns);  /* win32_ext.c: sleeps with the guest lock released, retakes it first */
int  metal_display_hz(void);            /* gpu_web.c: the page's measured refresh rate, 0 = not yet */

#define A_REALTIMER   0x00925b14u   /* RealTimer, 1/4000 s (0x65C6D0 adds the frame's real dt) */
#define A_REALDT      0x009259bcu   /* the frame's real dt, s (0x661280) */
#define A_TICKS       0x0092595cu   /*   the same in ticks of 1/3600 s */
#define A_TICK_CARRY  0x00925964u   /*   0x661280's carry, s (the rest of the 1/4000 s units) */
#define A_SIMOBJ      0x009203b0u   /* the Sim timer object (constructor 0x6482D0 through 0x6484E0) */
#define SIM_LAST      0x14u         /*   RealTimer at its last step */
#define SIM_CARRY     0x1cu         /*   the accumulator's rest, in steps */
#define SIM_STEP      0x20u         /*   the step P, s */
#define SIM_RUNS      0x28u         /*   the steps 0x642DB0 ran so far */
#define A_CAP_MUL     0x008f8304u   /* 4.0: a frame's time is at most this many units (0x6612D3) */
#define A_CAP_60      0x008f82f0u   /*   nonzero: the unit is 1/60 s ([0x8970F0]), else [0x892B64] (0.02 s) */
#define A_SIXTIETH    0x008970f0u
#define A_FIFTIETH    0x00892b64u
#define A_SIMMODE     0x009b0fa0u   /* 5: the step's clock is 0x735840's (0x6277D0), not RealTimer */
#define A_SKIP4       0x00925854u   /* the step's argument: 4 steps, no sleep */
#define A_FLOW        0x00925e90u   /* GameFlowManager's state: 3 front end, 6 racing */
#define FLOW_FE       3u
#define FLOW_RACING   6u          /* a world: a race, free roam (the racing state) */
#define A_RTF_CARRY   0x00925960u   /* RealTimeFrames' carry, s (0x65C6D0) */
#define A_RTF         0x008f82f4u   /* RealTimeFrames, 1/60 s units */
#define A_RTF_COPY    0x008f82f8u   /*   its copy */
#define A_RTF_TICK    0x008f82fcu   /*   this frame's */
#define A_CAM_DT      0x00925e9cu   /* the steps' time the front end's cameras get (0x64A680) */
#define RET_CAMA_STEP 0x0064a759u   /* 0x480E40 called with [0x925E9C] (a frame that stepped) */
#define RET_CAMB_STEP 0x0064a764u   /* 0x480C10 likewise */
#define RET_CAMA_ZERO 0x0064a6a5u   /* 0x480E40 called with 0.01 (the front end, no step) */
#define RET_CAMB_ZERO 0x0064a6afu   /* 0x480C10 likewise */
#define RET_MAIN_RTF  0x00663ef2u   /* MainLoop's call of 0x65C6D0 returns here */
#define RET_MAINLOOP  0x00663d53u   /* MainLoop's call of 0x661280 returns here (MiniMainLoop's: 0x663CCB) */
#define RET_STEPSLEEP 0x00642ed7u   /* the step's call of the sleep 0x64CF00 returns here */

#define M_OFF      (-1)
#define M_UNCAPPED (-2)
#define M_AUTO     (-3)

static int g_mode = M_AUTO, g_lockstep = 0, g_lock_wall = 0, g_log = 0, g_race_hfr = 0;
static double g_sim_hz = 0;
static _Atomic int g_rate_now = 0;

static uint64_t mono_ns(void){ return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* the page's rate (NFSU2_DISPLAY_HZ is its measurement at Play, until the renderer measures rAF), or the
 * autotest's "displayhz" */
static _Atomic int g_hz_forced = 0;
static int display_hz(void){ int f = atomic_load(&g_hz_forced); return f > 0 ? f : metal_display_hz(); }
void hfr_set_display_hz(int hz){ atomic_store(&g_hz_forced, hz > 0 ? hz : 0); }
static int rate_for_hz(int hz){
    if (hz >= 239) return 240;
    if (hz >= 119) return 120;
    if (hz > 0 && hz <= 31) return 30;
    return 60;
}
static void resolve_once(void){
    const char *e = getenv("NFSU2_FPS");
    if (!e || !*e || !strcasecmp(e, "auto") || !strcasecmp(e, "native")) g_mode = M_AUTO;
    else if (!strcasecmp(e, "off") || !strcasecmp(e, "vanilla") || !strcasecmp(e, "game")) g_mode = M_OFF;
    else if (!strcasecmp(e, "uncapped") || !strcasecmp(e, "max")) g_mode = M_UNCAPPED;
    else {
        int v = atoi(e);
        if (v == 30 || v == 60 || v == 120 || v == 240) g_mode = v;
        else { fprintf(stderr, "[mwpace] NFSU2_FPS=%s not supported (30|60|120|240|auto|uncapped|off): auto\n", e);
               g_mode = M_AUTO; }
    }
    e = getenv("NFSU2_HFR_LOCKSTEP"); g_lockstep = e && *e == '1';
    e = getenv("NFSU2_HFR_LOCKSTEP_WALL"); g_lock_wall = e ? atoi(e) : 0;
    e = getenv("NFSMW_SIM_HZ"); g_sim_hz = e ? atof(e) : 0.0;
    if (!(g_sim_hz >= 10.0 && g_sim_hz <= 1000.0)) g_sim_hz = 0.0;
    e = getenv("NFSU2_HFR_LOG"); g_log = e ? atoi(e) : 0;
    e = getenv("NFSMW_RACE_HFR"); g_race_hfr = e && *e == '1';
}
static void resolve(void){ static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, resolve_once); }
static int mode_rate(void){
    if (g_mode == M_AUTO) return rate_for_hz(display_hz());
    if (g_mode == M_UNCAPPED || g_mode == M_OFF) return 0;
    return g_mode;
}
static const char *mode_name(void){
    return g_mode == M_OFF ? "off" : g_mode == M_UNCAPPED ? "uncapped" : g_mode == M_AUTO ? "auto" : "fixed";
}

/* core's questions (runtime/web/null_game.c answers them for a game without a pacer) */
int hfr_mode_off(void){ resolve(); return g_mode == M_OFF; }
int hfr_rate_now(void){ return atomic_load(&g_rate_now); }
/* the presenter replaces a frame the page has not taken yet: uncapped, or a content rate above the
 * display's (gpu_web.c handoff) */
int hfr_mailbox(void){
    resolve();
    if (g_mode == M_UNCAPPED) return 1;
    int r = atomic_load(&g_rate_now), hz = display_hz();
    return r > 0 && hz > 0 && r > hz + 1;
}
/* the virtual clock's smallest step a Present (win32.c vclock_tick): 4 ms was a 250 fps cap */
uint64_t hfr_vclock_floor_us(void){
    resolve();
    if (g_mode == M_UNCAPPED) return 500u;
    if (g_mode != M_OFF && atomic_load(&g_rate_now) > 200) return 1000u;
    return 4000u;
}

/* ---- the display clock: the page's rAF times (gpu_web.c vsync_feed), as UG2's framerate.c ---- */
static _Atomic uint64_t g_vs_ts = 0, g_vs_period = 0;
static double g_vs_t0 = 0, g_vs_nom = 0;
void hfr_note_vsync(double ts, double dur){
    if (!(ts > 0) || !(dur > 0.001 && dur < 0.1)) return;
    if (!g_vs_t0 || fabs(dur - g_vs_nom) > 0.02 * g_vs_nom){ g_vs_t0 = ts; g_vs_nom = dur; atomic_store(&g_vs_period, 0); }
    double k = floor((ts - g_vs_t0) / g_vs_nom + 0.5);
    if (k >= 240){                   /* >= 2 s of baseline: the measured period */
        double p = (ts - g_vs_t0) / k;
        if (fabs(p / g_vs_nom - 1.0) < 0.002) atomic_store(&g_vs_period, (uint64_t)llround(p * 1e9));
    }
    atomic_store(&g_vs_ts, (uint64_t)llround(ts * 1e9));
}
/* the display clock ticked in the last 250 ms: the page draws (win32_ext.c spins the pacer's sleep only
 * then) */
int hfr_display_live(void){
    uint64_t tv = atomic_load(&g_vs_ts);
    return tv && (int64_t)(mono_ns() - tv) < 250000000ll;
}

/* ---- the slot pacer (UG2's framerate.c pace): a frame that is early sleeps to the start of the slot
 * after its predecessor's; a late one takes the last boundary it passed, the missed slots dropped; with
 * the vsync clock the boundaries sit half a refresh after the vsyncs and the period is k measured
 * refreshes.  Returns the slots the frame stands for (its game time / period). */
static int64_t g_last_slot = 0;
static uint64_t g_lock_ok = 0;
static int64_t snap(int64_t t, int64_t origin, int64_t g){
    int64_t d = t - origin;
    int64_t m = d >= 0 ? (d + g / 2) / g : -((-d + g / 2) / g);
    return origin + m * g;
}
static int pace(int rate, uint64_t *slept, uint64_t *missed){
    int64_t now = (int64_t)mono_ns();
    *slept = 0; *missed = 0;
    double nominal = 1e9 / rate;
    int64_t P = (int64_t)llround(nominal), g = P, origin = 0;
    uint64_t pv = atomic_load(&g_vs_period), tv = atomic_load(&g_vs_ts);
    if (pv && tv){
        double k = floor(nominal / (double)pv + 0.5);
        if (k >= 1 && fabs(k * (double)pv - nominal) < 0.01 * nominal){
            P = (int64_t)llround(k * (double)pv); g = (int64_t)pv; origin = (int64_t)tv + g / 2;
            g_lock_ok++;
        }
    }
    if (!g_last_slot || now - g_last_slot > 4 * 1000000000ll){     /* the first frame, or after a load */
        int64_t s0 = snap(now, origin, g); if (s0 > now) s0 -= g;
        g_last_slot = s0;
        return 1;
    }
    int64_t target = snap(g_last_slot + P, origin, g);
    if (target <= g_last_slot) target += g;
    if (now < target){
        guest_sleep_frame_ns(target - now);
        *slept = (uint64_t)(target - now);
        int n = (int)((double)(target - g_last_slot) / (double)P + 0.5);
        g_last_slot = target;
        return n < 1 ? 1 : n;
    }
    int64_t n = (now - g_last_slot) / P; if (n < 1) n = 1;
    *missed = (uint64_t)(n - 1);
    g_last_slot = snap(g_last_slot + n * P, origin, g);
    if (g_last_slot > now) g_last_slot -= g;
    return (int)n;
}

/* ---- the frame: the slots it stands for, and the steps they owe ---- */
static int      g_paced = 0;        /* this frame went through the pacer: the step sleeps not, counts slots */
static int      g_rate_f = 0;       /* its content rate */
static int      g_frame_slots = 1;  /* the slots of this frame */
static uint64_t g_slots_tot = 0;    /* slots since the rate last changed */
static uint64_t g_steps_tot = 0;    /* steps owed for them: floor(slots x S / R) */
static double   g_sim_hz_f = 60.0;  /* S of the frame */
/* statistics: this second and all; the steps 0x642DB0 ran a frame 0..3, 4+; frames cut to the game's cap
 * (their slots beyond it count as missed); frames whose steps 0x642DB0 limited below the steps owed */
typedef struct { uint64_t frames, slots, missed, sleep_ns, work_ns, steps[5], cut, short_run; } Stat;
static Stat g_sec, g_tot;
static uint64_t g_t_open = 0, g_sec_t0 = 0, g_rtf_undone = 0;

static void stat_close(uint64_t now){
    if (!g_sec_t0) g_sec_t0 = now;
    if (now - g_sec_t0 < 1000000000ull) return;
    if (g_log && g_sec.frames)
        fprintf(stderr, "[mwpace] rate %d flow %u: %llu frames, %llu slots, %llu missed, steps run 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu, "
                "%llu cut to the cap, %llu short, sleep %.2f ms/f, work %.2f ms/f\n", atomic_load(&g_rate_now), MR32(A_FLOW),
                (unsigned long long)g_sec.frames, (unsigned long long)g_sec.slots, (unsigned long long)g_sec.missed,
                (unsigned long long)g_sec.steps[0], (unsigned long long)g_sec.steps[1], (unsigned long long)g_sec.steps[2],
                (unsigned long long)g_sec.steps[3], (unsigned long long)g_sec.steps[4],
                (unsigned long long)g_sec.cut, (unsigned long long)g_sec.short_run,
                g_sec.sleep_ns / 1e6 / (double)g_sec.frames, g_sec.work_ns / 1e6 / (double)g_sec.frames);
    g_tot.frames += g_sec.frames; g_tot.slots += g_sec.slots; g_tot.missed += g_sec.missed;
    g_tot.sleep_ns += g_sec.sleep_ns; g_tot.work_ns += g_sec.work_ns;
    g_tot.cut += g_sec.cut; g_tot.short_run += g_sec.short_run;
    for (int i = 0; i < 5; i++) g_tot.steps[i] += g_sec.steps[i];
    memset(&g_sec, 0, sizeof g_sec);
    g_sec_t0 = now;
}

/* the most slots a frame stands for: the game's cap on a frame's time (0x6612D3), in whole slots */
static int cap_slots(int rate){
    double unit = MR32(A_CAP_60) ? (double)MRF32(A_SIXTIETH) : (double)MRF32(A_FIFTIETH);
    double cap = (double)MRF32(A_CAP_MUL) * unit;
    if (!(cap > 0.0 && cap < 1.0)) cap = 4.0 / 60.0;
    int k = (int)floor(cap * (double)rate + 1e-6);
    return k < 1 ? 1 : k;
}

/* The world's step (above): 1/60 in the front end; leaving it (the loading of a world) the page's rate, at
 * least 60 (NFSMW_SIM_HZ when set), latched until the world is left for the front end again, so the world's
 * tasks and objects never see the step change under them.  A rate the page takes up later (a display measured
 * at 120 after the world began) waits for the next world; until then the world runs at its own steps. */
static double g_world_hz = 60.0;
static void world_step(int rate){
    static int seen_fe = 0;            /* the boot (before the first front end) keeps the game's step */
    uint32_t flow = MR32(A_FLOW), sim = MR32(A_SIMOBJ);
    if (flow == FLOW_FE){ g_world_hz = 60.0; seen_fe = 1; }
    else if (flow != FLOW_RACING && seen_fe) g_world_hz = g_sim_hz > 0 ? g_sim_hz : rate > 60 ? (double)rate : 60.0;
    if (sim) MRF32(sim + SIM_STEP) = (float)(1.0 / g_world_hz);
}

/* MainLoop's real time 0x661280 (cdecl, [esp+4] = the frame's dt in s, from WinMain's QPC ticker) */
void sub_00661280_orig(CPU *c);
void sub_00661280(CPU *c){
    resolve();
    g_paced = 0;
    if (g_mode == M_OFF || MR32(c->esp) != RET_MAINLOOP){ sub_00661280_orig(c); return; }
    uint64_t t_in = mono_ns();
    int rate = mode_rate();
    world_step(rate);
    /* in a world at most its steps a second (above): a frame that does not step would show the one before */
    if (rate > g_world_hz && !g_race_hfr && MR32(A_FLOW) != FLOW_FE) rate = (int)g_world_hz;
    if (rate != atomic_load(&g_rate_now)){ g_slots_tot = 0; g_steps_tot = 0; }
    atomic_store(&g_rate_now, rate);
    if (rate > 0){
        uint64_t slept = 0, missed = 0;
        int slots = pace(g_lockstep && g_lock_wall > 0 ? g_lock_wall : rate, &slept, &missed);
        if (g_lockstep){ slots = 1; missed = 0; }
        int most = cap_slots(rate);
        if (slots > most){ missed += (uint64_t)(slots - most); slots = most; g_sec.cut++; }
        MRF32(c->esp + 4u) = (float)((double)slots / rate);
        MRF32(A_TICK_CARRY) = 0.f;   /* the ticks exact: slots x 3600 / R (above) */
        g_paced = 1; g_rate_f = rate;
        g_frame_slots = slots; g_slots_tot += (uint64_t)slots;
        g_sec.frames++; g_sec.slots += (uint64_t)slots; g_sec.missed += missed; g_sec.sleep_ns += slept;
        if (g_t_open) g_sec.work_ns += t_in - g_t_open;
    } else if (g_mode == M_UNCAPPED){
        g_paced = 2;                 /* no pacer, no limiter: the game's accumulator */
        g_sec.frames++;
        if (g_t_open) g_sec.work_ns += t_in - g_t_open;
    }
    uint64_t now = mono_ns();
    stat_close(now);
    g_t_open = now;
    sub_00661280_orig(c);
}

/* the Sim timer's step 0x642E60 (thiscall, ecx = [0x9203B0], [esp+4] = the 4-step byte; ret 4) */
void sub_00642e60_orig(CPU *c);
void sub_00642e60(CPU *c){
    uint32_t sim = c->ecx;
    if (!g_paced || !sim || sim != MR32(A_SIMOBJ)){ sub_00642e60_orig(c); return; }
    float P = MRF32(sim + SIM_STEP);
    int skip4 = MR8(c->esp + 4u) != 0;
    long owed = -1;
    if (g_paced == 1 && P > 0.f){
        /* the steps the slots owe: floor(slots x S / R), S = 1/P (60 = the game's 1/60 within float) */
        double S = 1.0 / (double)P;
        double sr = floor(S + 0.5); if (fabs(S - sr) < 1e-3 * S) S = sr;
        if (S != g_sim_hz_f){ g_sim_hz_f = S; g_slots_tot = (uint64_t)g_frame_slots; g_steps_tot = 0; }
        uint64_t want = (uint64_t)floor((double)g_slots_tot * S / (double)g_rate_f + 1e-9);
        long due = (long)(want - g_steps_tot);
        g_steps_tot = want;          /* steps a frame does not take here are dropped: no debt */
        if (MR32(A_SIMMODE) != 5u && !skip4){
            /* the original computes n = trunc(dt / P + carry) with dt from RealTimer as it does: a carry
             * that puts dt / P + carry at owed + 0.5 makes n = owed, whatever its rounding */
            int32_t d = (int32_t)(MR32(A_REALTIMER) - MR32(sim + SIM_LAST));
            double dt = (double)d * 0.00025;
            owed = due;
            if (d > 0) MRF32(sim + SIM_CARRY) = (float)((double)owed + 0.5 - dt / (double)P);
            else owed = 0;           /* dt <= 0: the original steps not (and keeps its carry) */
        }
    }
    uint32_t runs0 = MR32(sim + SIM_RUNS);
    sub_00642e60_orig(c);
    if (owed >= 0) MRF32(sim + SIM_CARRY) = 0.f;     /* no rest of its own: the slots keep it */
    uint32_t ran = MR32(sim + SIM_RUNS) - runs0;     /* what 0x642DB0 ran, after its own limits */
    g_sec.steps[ran < 4u ? ran : 4u]++;
    if (owed > (long)ran) g_sec.short_run++;
}

/* the sleep 0x64CF00 (cdecl ms; SleepEx(ms, TRUE) through 0x7EC298): the step's limiter sleep is the
 * pacer's now */
void sub_0064cf00_orig(CPU *c);
void sub_0064cf00(CPU *c){
    if (g_paced && MR32(c->esp) == RET_STEPSLEEP){
        c->eax = 0;                  /* SleepEx's answer: the time went by */
        c->eip = MR32(c->esp); c->esp += 4u;
        return;
    }
    sub_0064cf00_orig(c);
}

/* RealTimeFrames and RealTimer 0x65C6D0 (MainLoop's last call; MiniMainLoop's too): without the forced
 * minimum of one RealTimeFrame a frame above 60, and RealTimer by the frame's ticks exactly (above) */
static double g_rt_carry = 0;       /* RealTimer's fraction, 1/4000 s units */
void sub_0065c6d0_orig(CPU *c);
void sub_0065c6d0(CPU *c){
    int undo = 0, paced = g_paced == 1 && MR32(c->esp) == RET_MAIN_RTF;
    uint32_t timer0 = MR32(A_REALTIMER), ticks = MR32(A_TICKS);
    if (paced && g_rate_f > 60){
        /* the original's count: trunc((dt + carry) x 60), dt = [0x9259BC] */
        double x = ((double)MRF32(A_REALDT) + (double)MRF32(A_RTF_CARRY)) * 60.0;
        undo = x < 1.0;
    }
    sub_0065c6d0_orig(c);
    if (undo && MR32(A_RTF_TICK) == 1u){
        MR32(A_RTF) -= 1u; MR32(A_RTF_COPY) -= 1u; MR32(A_RTF_TICK) = 0u;
        g_rtf_undone++;
    }
    if (paced && (int32_t)ticks > 0){
        /* the original adds round(dt x 4000), dt = ticks / 3600 */
        double ex = (double)ticks * (4000.0 / 3600.0) + g_rt_carry, u = floor(ex + 0.5);
        g_rt_carry = ex - u;
        MR32(A_REALTIMER) = timer0 + (uint32_t)(int32_t)u;
    }
}

/* The front end's cameras 0x480E40 and 0x480C10 (cdecl, [esp+4] = their time step, s), both called from
 * 0x64A680 one after the other with the same time: on the wall clock frame by frame (above) */
static double g_cam_ahead = 0;      /* time the cameras got on frames that did not step, since the last step */
static float  g_cam_dt = 0;         /* this frame's, for the second of the pair */
static uint64_t g_cam_zero = 0, g_cam_step = 0;
static int cam_frame(CPU *c, uint32_t ret_zero, uint32_t ret_step, int first){
    uint32_t r = MR32(c->esp);
    if (g_paced != 1 || (r != ret_zero && r != ret_step) || MR32(A_FLOW) != FLOW_FE) return 0;
    if (first){
        if (r == ret_zero){
            double f = (double)g_frame_slots / (double)g_rate_f;
            g_cam_dt = (float)f; g_cam_ahead += f; g_cam_zero++;
        } else {
            double s = (double)MRF32(c->esp + 4u) - g_cam_ahead;
            g_cam_dt = (float)(s > 0 ? s : 0); g_cam_ahead = 0; g_cam_step++;
        }
    }
    MRF32(c->esp + 4u) = g_cam_dt;
    return 1;
}
void sub_00480e40_orig(CPU *c);
void sub_00480e40(CPU *c){ cam_frame(c, RET_CAMA_ZERO, RET_CAMA_STEP, 1); sub_00480e40_orig(c); }
void sub_00480c10_orig(CPU *c);
void sub_00480c10(CPU *c){ cam_frame(c, RET_CAMB_ZERO, RET_CAMB_STEP, 0); sub_00480c10_orig(c); }

/* exit report (main_web.c) */
void hfr_report(void){
    resolve();
    stat_close(mono_ns() + 1000000000ull);
    if (!g_tot.frames) return;
    fprintf(stderr, "\n=== frame rate (Most Wanted): mode %s, display %d Hz, content rate %d, world steps %.0f/s%s%s ===\n",
            mode_name(), display_hz(), atomic_load(&g_rate_now), g_world_hz, g_lockstep ? ", LOCKSTEP" : "", g_sim_hz > 0 ? ", sim rate set" : "");
    fprintf(stderr, "  %llu frames, %llu slots, %llu missed, %llu on the vsync grid; steps run a frame 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu; "
            "sleep %.2f ms/f, work %.2f ms/f\n",
            (unsigned long long)g_tot.frames, (unsigned long long)g_tot.slots, (unsigned long long)g_tot.missed,
            (unsigned long long)g_lock_ok, (unsigned long long)g_tot.steps[0], (unsigned long long)g_tot.steps[1],
            (unsigned long long)g_tot.steps[2], (unsigned long long)g_tot.steps[3], (unsigned long long)g_tot.steps[4],
            g_tot.sleep_ns / 1e6 / (double)g_tot.frames, g_tot.work_ns / 1e6 / (double)g_tot.frames);
    fprintf(stderr, "  %llu frames cut to the game's cap on a frame's time; %llu frames 0x642DB0 ran fewer steps than owed\n",
            (unsigned long long)g_tot.cut, (unsigned long long)g_tot.short_run);
    fprintf(stderr, "  RealTimeFrames' forced one undone %llu times; front-end camera frames: %llu without a step, %llu with\n",
            (unsigned long long)g_rtf_undone, (unsigned long long)g_cam_zero, (unsigned long long)g_cam_step);
}
