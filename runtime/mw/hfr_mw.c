/* hfr_mw.c -- Most Wanted's simulation at a step other than 1/60 s (TRACKER M7-21, M7-22): the fixes per
 * subsystem.  framerate_mw.c sets the step (the page's rate in a world, 1/60 in the front end); this file keeps
 * what was written for 60 steps a second working at 120 and 240.
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
 * a mode-1 task (the traffic 0x43B0F1, AITrafficManager 0x441A2D, 0x442EC0, 0x6B9E06) by the frames, one step a
 * frame in a world at the page's rate (framerate_mw.c keeps a world's content rate at its steps), its +0x1C
 * gathering their time: the traffic every 1/6 s with dT 1/6 at 120 and 240.  A frame that runs two steps (the
 * page fell behind) is one mode-1 run with both steps' time, as at 60.  A start offset counts in runs of the
 * task, so it keeps its time too.  A task of rate 1 (every step or frame: the physics, the player) runs as
 * before.  The rate is
 * scaled only for the call and put back after it, so SetRate and the game's own reads see the game's values.
 *
 *
 * The same stop at 60 (the game's own step): there the traffic thinks with dT 1/6, so a stopped car's target is
 * 0.333, still under the driver's 0.5: in a lockstep race at 60 a traffic car sat at 0.0000 m/s with target
 * 0.3333 and BRAKE for 16 s to the run's end.  Only at 40 steps a second or fewer (2 dT >= 0.5) does a stopped
 * car get going again; the cadence fix keeps 120 and 240 as 60, stops and all.  So the stop itself is
 * the game's bug at its own rate (Carbon's traffic does not stop for good at any rate, per WidescreenFixesPack
 * #1218), fixed here at every rate: below 0.5 m/s AIActionTraffic's cap counts dT as at least 0.25 s (its
 * value at 40 Hz), so a stopped car whose wanted speed is 0.5 or more gets 0.5 and the driver gives it gas; a
 * car that wants under 0.5 (a car ahead, a dead end) still brakes.  dT is read nowhere else in 0x411820
 * (checked over its stack slots), and a car at 0.5 m/s or more keeps the game's cap.
 *
 * Options (test only): NFSMW_HFR_AILOG=1 logs AIActionTraffic's speed for a car under 0.6 m/s ("[hfrai]",
 * BRAKE when the target is under the driver's 0.5); NFSMW_HFR_TASKS=0 turns the cadence fix off and
 * NFSMW_TRAFFIC_RESTART=0 the stop fix (A/B). */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>

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
    if (on < 0) on = env_int("NFSMW_HFR_TASKS", 1) != 0;
    uint32_t t = c->ecx;
    double S = on ? sim_hz() : 60.0;
    if (S <= 60.5 || !t || (MR32(t + TASK_MODE) & 0xfu) > 1u){ sub_006ecd90_orig(c); return; }
    float rate = MRF32(t + TASK_RATE);
    if (!(rate > 0.f && rate < 1.f)){ sub_006ecd90_orig(c); return; }
    float scaled = (float)((double)rate * 60.0 / S);
    MRF32(t + TASK_RATE) = scaled;
    sub_006ecd90_orig(c);
    /* the handler may have set a new rate (SetRate) or removed the task: put the game's value back only over
     * ours */
    if (MRF32(t + TASK_RATE) == scaled) MRF32(t + TASK_RATE) = rate;
}

/* AIActionTraffic's target speed (0x411820, thiscall (current speed, dT), the target on st0; ret 8): the
 * stopped car's cap (above) */
void sub_00411820_orig(CPU *c);
void sub_00411820(CPU *c){
    static int log = -1, fix = -1; static long n = 0;
    if (log < 0){ log = env_int("NFSMW_HFR_AILOG", 0); fix = env_int("NFSMW_TRAFFIC_RESTART", 1) != 0; }
    float cur = MRF32(c->esp + 4u), dt = MRF32(c->esp + 8u);
    uint32_t self = c->ecx;
    if (fix && cur < 0.5f && dt < 0.25f) MRF32(c->esp + 8u) = 0.25f;     /* the callee's own argument slot */
    sub_00411820_orig(c);
    if (log && cur < 0.6f && n < 4000){
        n++;
        fprintf(stderr, "[hfrai] f=%ld traffic %08x speed %.4f dT %.5f target %.4f%s\n", autotest_frame(), self, cur, dt,
                (double)ST(0), ST(0) < 0.5 ? " BRAKE" : "");
    }
}
