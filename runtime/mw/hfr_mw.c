/* hfr_mw.c -- Most Wanted's simulation at a step other than 1/60 s (TRACKER M7-21, M7-22): the fixes per
 * subsystem.  framerate_mw.c keeps the game's step, 1/60; its research switch NFSMW_WORLD_HFR=1 (or
 * NFSMW_SIM_HZ) steps a world at the page's rate; this file keeps what was written for 60 steps a second
 * working at 120 and 240.  At the game's own step it changes nothing (unless the stopped-traffic option below is
 * set).
 *
 * The scheduler's tasks (GAME.md 6.3).  SimSystem::AddTask 0x6ED0F0 (thiscall on [0x9885E0]: schedule, rate,
 * handler, start offset, mode) makes a task of 0x28 bytes (0x6ECCD0): +0 rate (at most 1), +4 handler, +8 its
 * accumulator (minus the start offset at first), +0x18 mode (low 4 bits; bit 4 holds it), +0x1C the time a
 * mode-1 task has gathered, +0x20 handle.  Each step the schedule (0x6ED070, from the step 0x6F6E22) runs every
 * task through 0x6ECD90 (thiscall, ret 8) with [esp+4] = the step's time (the step times the time scale) and
 * [esp+8] = the time of the steps run since the last frame's first step on a frame's first step, 0 on its other
 * steps (0x6F6DD7: the frame counter [0x982B78], counted at each frame's draw 0x6E764B, against the last seen):
 *   mode 0 (0x6ECDBB): the task adds its rate to the accumulator and, at 1 or more, runs its handler (vtable +4,
 *     (handle, [esp+4] / rate)) and takes 1 off.  So its rate is a share of the STEPS.
 *   mode 1 (0x6ECDFA): only when [esp+8] > 0, it adds its rate to the accumulator and [esp+8] to +0x1C and, at 1
 *     or more, runs its handler with +0x1C (the time since it last ran), zeroes +0x1C and takes 1 off.  So its
 *     rate is a share of the FRAMES that step; with one step a frame (60 at 60, a world at the page's rate) the
 *     same as of the steps.
 * AIVehicleTraffic's 0.1 (0x43B0F4, mode 1 through 0x43A9A0) thinks every 10 steps, 1/6 s at 60 and 1/12 s at
 * 120, with dT 1/6 s and 1/12 s; the racers' 0.5 (0x43ADA0's callers, mode 0) every 2 steps.  Code that is not
 * linear in dT then changes with the step.  The case seen (M7-21):
 * AIActionTraffic's speed 0x411820 (current speed, dT) caps the target at current + 2 dT (0x411A2E), and the AI
 * driver's pedals 0x408430 brake whenever the target is under 0.5 m/s (0x408582).  A car stopped (speed 0) gets
 * a target of 0.333 at 60 and of 0.167 at 120, both under 0.5, so it brakes for good: the traffic that stops
 * and never starts again above about 40 Hz (WidescreenFixesPack #1218; 2 dT = 0.5 at 40).  Seen in a lockstep
 * race at 120: a traffic car at 0.0000 m/s with target 0.1667 every think, 11 s to the run's end.
 * The fix: a task of mode 0 or 1 whose rate is under 1 keeps its 60 Hz cadence in time, rate x 60 / S at S
 * steps a second, so it thinks as often and with the same dT as at 60: a mode-0 task (the racers) by the steps,
 * a mode-1 task (the traffic 0x43B0F1, AITrafficManager 0x441A2D, 0x442EC0, 0x6B9E06) by the steps its run
 * covers: [esp+8] / [esp+4] (the steps' time since the last frame's run over one step's time, both under the
 * same time scale, 0x6F6DA0..0x6F6E22), one in a world at the page's rate that keeps up, its +0x1C gathering
 * their time: the traffic every 1/6 s with dT 1/6 at 120 and 240.  When the page falls behind its rate (frames
 * of 2 or 3 steps: a race at 120 on a machine that holds 70-85 frames a second, M7-32) the task still runs by
 * the time of the steps, every 1/6 s: scaled by the frames alone (the first form of this fix, NFSMW_HFR_TASKS=2)
 * it ran at 70-85 / 120 of that, as the game's own does at 60 only on a machine under 60 frames a second.  At
 * most 1 a frame (a mode-1 task runs once a frame).  A start offset counts in runs of the task, so it keeps its
 * time too.  A task of rate 1 (every step or frame: the physics, the player's AI) runs as before.  The rate is
 * scaled only for the call and put back after it, so SetRate and the game's own reads see the game's values.
 *
 *
 * The same stop at 60 (the game's own step): there the traffic thinks with dT 1/6, so a stopped car's target is
 * 0.333, still under the driver's 0.5: in a lockstep race at 60 a traffic car sat at 0.0000 m/s with target
 * 0.3333 and BRAKE for 16 s to the run's end.  Only at 40 frames a second or fewer (2 dT >= 0.5) does a stopped
 * car get going again; the cadence fix keeps 120 and 240 as 60, stops and all.  So the stop is the game's own at
 * its own rate, and changing it changes the game's traffic at 60 too: an option, off by default.
 * NFSMW_TRAFFIC_RESTART=1: a car within 0.05 m/s of 0 whose cap (current + 2 dT) is under 0.5 gets dT 0.275 s
 * for the call, so its cap is current + 0.55, over 0.5: wanting 0.5 or more, its target is over 0.5 and the
 * driver gives it gas.  Wanting under 0.5 (a car ahead, a dead end) it still brakes, toward the wanted speed
 * instead of the cap; on 0x411820's path that skips the cap (0x411A2C) dT is not read and the target is the
 * same either way.  dT is read nowhere else in 0x411820 (checked over its stack slots); a moving car, or one
 * whose cap already reaches 0.5, keeps the game's.  It does not do what it is for: in a lockstep race at 120
 * (traffic at its 60 Hz cadence) a car at 0.0000 m/s got target 0.55 at each of 62 thinks, 10 s, and did not
 * move, so the stop needs more than the target (TRACKER M7-21).
 *
 * Options (test only): NFSMW_HFR_AILOG=1 logs AIActionTraffic's speed for a car under 0.6 m/s ("[hfrai]",
 * BRAKE when the target is under the driver's 0.5), =2 for every car (the first 4000); NFSMW_HFR_TASKS=0 turns the cadence fix off,
 * =2 scales a mode-1 task by the frames only (the first form, for A/Bs);
 * NFSMW_TRAFFIC_RESTART=1 the stopped-traffic restart on (above). */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

long autotest_frame(void);

#define A_SIMOBJ   0x009203b0u   /* the Sim timer (framerate_mw.c) */
#define SIM_STEP   0x20u         /*   its step P, s */
#define TASK_RATE  0x00u
#define TASK_MODE  0x18u

static int env_int(const char *k, int dflt){ const char *e = getenv(k); return e && *e ? atoi(e) : dflt; }

/* the steps a second the world runs at, 60 when it is the game's 1/60 or cannot be read */
static double sim_hz(void){
    uint32_t sim = MR32(A_SIMOBJ);
    float P = sim ? MRF32(sim + SIM_STEP) : 0.f;
    return P > 0.0005f && P < 0.1f ? 1.0 / (double)P : 60.0;
}

/* a task's run 0x6ECD90 (thiscall, ecx = the task, [esp+4] = the step's time, [esp+8] = a frame's steps' time
 * on its first step, else 0; ret 8): modes 0 and 1 scaled (above), any other mode as the game's */
void sub_006ecd90_orig(CPU *c);
void sub_006ecd90(CPU *c){
    static int on = -1;
    if (on < 0) on = env_int("NFSMW_HFR_TASKS", 1);
    uint32_t t = c->ecx;
    double S = on ? sim_hz() : 60.0;
    uint32_t mode = t ? MR32(t + TASK_MODE) & 0xfu : 0xfu;
    if (S <= 60.5 || mode > 1u){ sub_006ecd90_orig(c); return; }
    float rate = MRF32(t + TASK_RATE);
    if (!(rate > 0.f && rate < 1.f)){ sub_006ecd90_orig(c); return; }
    double k = 60.0 / S;
    if (mode == 1u && on == 1){       /* the steps this run covers (above); mode 1 runs only when [esp+8] > 0 */
        float step = MRF32(c->esp + 4u), b = MRF32(c->esp + 8u);
        double n = step > 0.f && b > 0.f ? floor((double)b / (double)step + 0.5) : 1.0;
        k *= n < 1.0 ? 1.0 : n > 8.0 ? 8.0 : n;
    }
    double sc = (double)rate * k;
    float scaled = (float)(sc < 1.0 ? sc : 1.0);
    MRF32(t + TASK_RATE) = scaled;
    sub_006ecd90_orig(c);
    /* the handler may have set a new rate (SetRate) or removed the task: put the game's value back only over
     * ours */
    if (MRF32(t + TASK_RATE) == scaled) MRF32(t + TASK_RATE) = rate;
}

/* AIActionTraffic's target speed (0x411820, thiscall (current speed, dT), the target on st0; ret 8): the
 * stopped car's cap with NFSMW_TRAFFIC_RESTART=1 (above) */
void sub_00411820_orig(CPU *c);
void sub_00411820(CPU *c){
    static int log = -1, fix = -1; static long n = 0;
    if (log < 0){ log = env_int("NFSMW_HFR_AILOG", 0); fix = env_int("NFSMW_TRAFFIC_RESTART", 0) != 0; }
    float cur = MRF32(c->esp + 4u), dt = MRF32(c->esp + 8u);
    uint32_t self = c->ecx;
    if (fix && cur > -0.05f && cur < 0.05f && cur + 2.f * dt < 0.5f)
        MRF32(c->esp + 8u) = 0.275f;                                       /* the callee's own argument slot */
    sub_00411820_orig(c);
    if (log && (log > 1 || cur < 0.6f) && n < 4000){
        n++;
        fprintf(stderr, "[hfrai] f=%ld traffic %08x speed %.4f dT %.5f target %.4f%s\n", autotest_frame(), self, cur, dt,
                (double)ST(0), ST(0) < 0.5 ? " BRAKE" : "");
    }
}

/* The NOS trail (M7-21, M7-32).  The cars' flares are drawn by the loop over the cars' render objects at 0x744E46;
 * each object (edi there) keeps a ring of 3 samples of a flare's matrix (+0x1190, 64 bytes each) and position
 * (+0x1250, 16 bytes each), the newest at the index +0x1280: in the views of id 1 and 2 (0x744F0C) each draw moves
 * the index on and writes the newest (0x744F14..0x744F79).  When the object at [esp+0x10] has +0x117C set and the
 * car's velocity (the vector at [esi+0x38]) is above [0x895074], the trail is drawn from 0x744FBF on: three
 * segments between the ring's samples (oldest -> middle, newest -> oldest, middle -> newest), [0x903694] flares on
 * each (0x745040).  So the trail covers the last two drawn frames: 2/60 s of the car's travel at 60, 2/120 s at
 * 120, half the length (WidescreenFixesPack's FixNOSTrailLength scales the segments by SimRate / 60 instead).
 * Here, as UG2's framerate.c F3: while the content rate is above 60 the newest stays the game's (the head on the
 * car) and, for the draw, the two older samples become this car's samples at least 1/60 s and 2/60 s older (a
 * history of its newest samples on the frames' real time [0x9259BC]): the trail of 60.  Until a car has 2/60 s of
 * samples (a trail that has just begun) its oldest stands in.  At 60 the picks would be the game's own samples;
 * at 60 or below nothing is written.  Not shown by a run yet: in six whole races and two runs with the nitrous
 * key held (tests/mw/scripts/hfr_laps.txt, hfr_nos.txt; M7-32) no car drew a trail: +0x117C stayed 0 on every
 * car.  It is the byte +0x71 of the car's render update (written at 0x756518; what sets it, the nitrous or a
 * setting, is not found).  So the fix is off until a run shows the trail: NFSMW_NOS_TRAIL=1 turns it on (with
 * the world above 60).  NFSMW_HFR_TRAIL=1 (test only) logs each car's trail every 0.25 s while it is drawn
 * ("[hfrtrail]"): the velocity's length, the span newest -> oldest as the game wrote it and as drawn, in m and in
 * s of travel at that speed; =2 also the trail's gate (below). */
#define HOOK_SITE(va) (va##u)
#define SITE_TRAIL  HOOK_SITE(0x00744fbf)  /* mov dword [esp+0x10], 3: the trail's three segments begin */
#define SITE_TRAIL_GATE HOOK_SITE(0x00744fad)  /* mov edx, [esp+0x10]: the car is fast enough; +0x117C next */
#define TRAIL_MAT   0x1190u
#define TRAIL_POS   0x1250u
#define TRAIL_IDX   0x1280u
#define A_FRAMES    0x00982b78u            /* the frame counter (++ at each frame's draw, 0x6E764B) */
#define A_REALDT    0x009259bcu            /* the frame's real dt, s (0x661280) */
#define TR_CARS     16
#define TR_HIST     32
int hfr_rate_now(void);                    /* framerate_mw.c: the content rate now */
typedef struct { uint32_t ring, frame; int n, head, idx; double logged;
                 double t[TR_HIST]; uint32_t m[TR_HIST][16], p[TR_HIST][4]; } Trail;
static Trail g_tr[TR_CARS];
static double g_tr_time = 0;
static uint32_t g_tr_frame = 0;
static Trail *trail_of(uint32_t ring){
    int fr = -1;
    for (int i = 0; i < TR_CARS; i++){
        if (g_tr[i].ring == ring) return &g_tr[i];
        if (!g_tr[i].ring && fr < 0) fr = i;
    }
    if (fr < 0) fr = (int)((ring >> 4) % TR_CARS);
    memset(&g_tr[fr], 0, sizeof g_tr[fr]);
    g_tr[fr].ring = ring; g_tr[fr].idx = -1;
    return &g_tr[fr];
}
/* the newest sample at time t or before, else the oldest kept */
static int trail_pick(const Trail *h, double t){
    for (int k = 0; k < h->n; k++){
        int i = (h->head - k + TR_HIST) % TR_HIST;
        if (h->t[i] <= t + 1e-4) return i;
    }
    return (h->head - (h->n - 1) + TR_HIST) % TR_HIST;
}
static double trail_span(uint32_t a, uint32_t b){
    double dx = MRF32(a) - MRF32(b), dy = MRF32(a + 4u) - MRF32(b + 4u), dz = MRF32(a + 8u) - MRF32(b + 8u);
    return sqrt(dx * dx + dy * dy + dz * dz);
}
static void trail(CPU *c){
    static int fix = -1, tlog = 0;
    if (fix < 0){ fix = env_int("NFSMW_NOS_TRAIL", 0) != 0; tlog = env_int("NFSMW_HFR_TRAIL", 0); }
    uint32_t r = c->edi;
    if (!r || !mem_is_committed(r + TRAIL_IDX + 3u) || !mem_is_committed(r + TRAIL_MAT)) return;
    int idx = (int)MR32(r + TRAIL_IDX);
    if (idx < 0 || idx > 2) return;
    uint32_t fr = MR32(A_FRAMES);
    if (fr != g_tr_frame){            /* the first trail of a frame: the clock moves by the frame's real time */
        float dt = MRF32(A_REALDT);
        g_tr_time += dt > 0.f && dt < 0.25f ? dt : 0.0;
        g_tr_frame = fr;
    }
    Trail *h = trail_of(r);
    uint32_t pn = r + TRAIL_POS + (uint32_t)idx * 16u, mn = r + TRAIL_MAT + (uint32_t)idx * 64u;
    if (idx != h->idx){               /* the ring moved: its newest sample into the history */
        if (!(h->n && h->frame == fr)){             /* (moved twice in a frame: the later sample replaces) */
            if (h->n && fr - h->frame != 1u) h->n = 0;   /* frames without this car's trail: stale */
            h->head = (h->head + 1) % TR_HIST; if (h->n < TR_HIST) h->n++;
        }
        for (int k = 0; k < 16; k++) h->m[h->head][k] = MR32(mn + 4u * (uint32_t)k);
        for (int k = 0; k < 4; k++) h->p[h->head][k] = MR32(pn + 4u * (uint32_t)k);
        h->t[h->head] = g_tr_time; h->frame = fr; h->idx = idx;
    }
    uint32_t po = r + TRAIL_POS + (uint32_t)((idx + 1) % 3) * 16u;   /* the oldest */
    double game = tlog ? trail_span(pn, po) : 0.0;
    int rate = hfr_rate_now();
    if (fix && rate > 60 && h->n > 1){
        for (int back = 1; back <= 2; back++){
            int i = trail_pick(h, g_tr_time - back / 60.0);
            uint32_t slot = (uint32_t)((idx + 3 - back) % 3);
            for (int k = 0; k < 16; k++) MR32(r + TRAIL_MAT + slot * 64u + 4u * (uint32_t)k) = h->m[i][k];
            for (int k = 0; k < 4; k++) MR32(r + TRAIL_POS + slot * 16u + 4u * (uint32_t)k) = h->p[i][k];
        }
    }
    if (tlog && g_tr_time - h->logged >= 0.25){
        h->logged = g_tr_time;
        uint32_t v = c->esi;
        double sp = v && mem_is_committed(v + 11u)
                  ? sqrt((double)MRF32(v) * MRF32(v) + (double)MRF32(v + 4u) * MRF32(v + 4u) + (double)MRF32(v + 8u) * MRF32(v + 8u)) : 0.0;
        double drawn = trail_span(pn, po);
        fprintf(stderr, "[hfrtrail] f=%ld car %08x v %.2f span %.3f m (%.4f s) drawn %.3f m (%.4f s) dt %.5f rate %d hist %d\n",
                autotest_frame(), r, sp, game, sp > 0.5 ? game / sp : 0.0, drawn, sp > 0.5 ? drawn / sp : 0.0,
                (double)MRF32(A_REALDT), rate, h->n);
    }
}

/* NFSMW_HFR_TRAIL=2 (test only): at the trail's gate (0x744FAD: the car above the speed, before the +0x117C test)
 * one "[hfrtrail] gate" line a car every 2 s of frames: the +0x117C word, the velocity's length and the trail draws
 * so far */
static long g_trail_draws = 0;
static void trail_gate(CPU *c){
    static int tlog = -1; static struct { uint32_t car, frame; } seen[TR_CARS];
    if (tlog < 0) tlog = env_int("NFSMW_HFR_TRAIL", 0);
    if (tlog < 2) return;
    uint32_t car = c->edi, fr = MR32(A_FRAMES), o = MR32(c->esp + 0x10u), v = c->esi;
    int i = 0;
    while (i < TR_CARS && seen[i].car && seen[i].car != car) i++;
    if (i == TR_CARS) i = (int)((car >> 4) % TR_CARS);
    if (seen[i].car == car && fr - seen[i].frame < 240u) return;
    seen[i].car = car; seen[i].frame = fr;
    double sp = v && mem_is_committed(v + 11u)
              ? sqrt((double)MRF32(v) * MRF32(v) + (double)MRF32(v + 4u) * MRF32(v + 4u) + (double)MRF32(v + 8u) * MRF32(v + 8u)) : -1.0;
    fprintf(stderr, "[hfrtrail] gate f=%ld car %08x obj %08x +117c %08x v %.2f (gate %.2f) draws %ld\n", autotest_frame(), car, o,
            o && mem_is_committed(o + 0x117fu) ? MR32(o + 0x117cu) : 0xffffffffu, sp, (double)MRF32(0x00895074u), g_trail_draws);
}

/* the instruction hooks of this file (debug_mw.c mw_debug_hook) */
int mw_hfr_hook(CPU *c, uint32_t va){
    if (va == SITE_TRAIL_GATE){ trail_gate(c); return 1; }
    if (va != SITE_TRAIL) return 0;
    g_trail_draws++;
    trail(c);
    return 1;
}
