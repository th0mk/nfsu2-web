/* framerate.c -- frame-rate modes, the host frame pacer, and the fixes that
 * make the game's frame-count-driven subsystems correct above 60 fps (and
 * at 30).
 *
 * The game runs one simulation step and one render per main-loop iteration
 * (0x5811C0): dt is variable and the frame rate is the simulation rate.  Car
 * physics substeps (<= 4.4 ms, 0x5B7347) make the handling the same at 30, 60,
 * 120 and 240 fps as long as dt is exactly 1/rate, so the work here is
 *   (1) pace the frames on a fixed slot grid and give the game exact ticks,
 *   (2) fix the handful of subsystems that count frames instead of time.
 *
 * Options (all read once):
 *   NFSU2_FPS = auto | 30 | 60 | 120 | 240 | uncapped | off
 *       auto (default): the window's screen maximumFramesPerSecond mapped to
 *       the best supported content rate (>= 239 Hz -> 240, >= 119 Hz -> 120,
 *       up to 31 Hz -> 30, otherwise 60).  30 Hz is a browser that halves its
 *       animation frames (Safari in Low Power Mode, or on a Mac nobody has
 *       touched for a minute; Chrome's Energy Saver): it shows only every
 *       other frame of 60, so the game makes the half it shows.  The game's
 *       own 30 fps requests in NIS cutscenes are kept (SetFrameRate
 *       0x576CB0); its online/LAN cap 0x5FF67C is not
 *       (sub_005ff490 below).
 *       30: for a machine that cannot hold 60 (a phone): steady frames at
 *       half the rate, the game otherwise as at 60 (the fixes below).
 *       uncapped: no pacer, mailbox presenter, display sync off.  Benchmark
 *       only: dt follows the machine, so the physics substep varies.
 *       off: the game's own QPC limiter at 1/120 s (plus SPINSLEEP), no fix,
 *       i.e. the timing of the port before this file existed.
 *       Aliases: native = auto, vanilla = game = off, max = uncapped.
 *   NFSU2_HFR_FIX = default | all | none | comma list of
 *       ticks,timer,particles,trail,audio,rtf,rain,spinners; a '-' prefix
 *       removes one ("default,-audio").  default = all when the mode's
 *       content rate is above 60, none at 60 (60 stays the vanilla
 *       reference), and ticks,particles,rain,spinners at 30 (rtf and audio
 *       do nothing there; timer and trail stay the game's own at that
 *       rate).  With auto the set follows the rate when the window moves to
 *       a screen with another refresh rate.
 *   NFSU2_LAN_TIMING = pc | exact
 *       pc (default): in a network race (World::IsOnline) ticks and timer
 *       are off, so the clocks run as on an unmodified Windows PC at the
 *       same frame rate (fixes_follow_rate), and 240 plays at 120, the
 *       fastest such a PC runs (content_rate); exact: the fix set as above.
 *   NFSU2_NOS_TRAIL_LEN = x     NOS/streak trail length factor (default 1)
 *   NFSU2_HFR_LOG = 1 | 2       one [hfr] line per second of racing (2: of
 *                               every frame, menus too) and the exit report
 *   NFSU2_HFR_TRACE = <file>    per-frame CSV while racing (GameFlow 6),
 *                               read by tools/hfr_compare.py
 *   NFSU2_DISPLAY_HZ = <hz>     override the detected refresh rate (tests);
 *                               on the web the page's measurement at Play,
 *                               the rate until the renderer measures it
 *   NFSU2_PRESENT_MINDUR = 0    no presentDrawable:afterMinimumDuration: (A/B)
 *   NFSU2_NO_VSYNC_CLOCK = 1    (metal.m) no CADisplayLink: the slot grid uses
 *                               the nominal period and an arbitrary phase
 *   NOVSYNC = 1                 (metal.m) display sync off; pacing unchanged
 *   PACE60, SPINSLEEP           old limiter helpers, only used with off
 *
 * Fixes (each one independent):
 *   ticks      exact host ticks: 1/3600 s tick base instead of 1/1800, the
 *              tick count primed from the pacer's slot count (F1)
 *   timer      RealTimer/WorldTimer (1/4000 s units) without the per-frame
 *              rounding: RealTimer carries the fraction, WorldTimer follows
 *              the game's own WorldTimeFrames clock (replays restore it)
 *   particles  "Acid" effects step = min(real dt, 1/60) instead of 1/60; at
 *              30 the real dt (F2)
 *   trail      NOS/streak trail sampled every frame, history resampled to
 *              1/60 s spacing (F3)
 *   audio      EAXSound::Update dt carried across its 15 ms gate (F4)
 *   rtf        RealTimeFrames without the forced minimum of 1 (F5)
 *   rain       Rain::Update on every frame, its per-call steps per time; at
 *              30 twice a frame (F6)
 *   spinners   the front end's spinners turn by time, not a step per frame
 *              (F7)
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

void guest_sleep_frame_ns(int64_t ns);  /* win32_ext.c: sleeps with the GIL released, retakes it first */
int  metal_display_hz(void);            /* metal.m: window's screen max fps, 0 = unknown */

enum { FX_TICKS = 1, FX_TIMER = 2, FX_PARTICLES = 4, FX_TRAIL = 8, FX_AUDIO = 16, FX_RTF = 32,
       FX_RAIN = 64, FX_SPIN = 128, FX_ALL = 255, FX_N = 8 };
static const char *const g_fx_name[FX_N] = { "ticks", "timer", "particles", "trail", "audio", "rtf", "rain", "spinners" };

#define M_OFF      (-1)
#define M_UNCAPPED (-2)
#define M_AUTO     (-3)

/* ---- game addresses (SPEED2.EXE v1.2 NA) ------------------------------ */
#define A_FLOW        0x008654a4u   /* GameFlowManager state, 6 = racing */
#define A_CAP         0x00865558u   /* minimum frame time (game's request) */
#define A_CAP_DEF     0x007fb710u   /* default cap, 1/120 */
#define A_PLAYBACK    0x008650d0u   /* JoyLog playback active */
#define A_CARRY       0x00865148u   /* FrameTimeUpdate carry (1/4000 domain) */
#define A_TICKS       0x00865140u   /* ticks this frame */
#define A_REALDT      0x00865198u   /* real dt (s) = ticks * [A_TICK_S] */
#define A_WORLDDT     0x00865154u   /* world dt (s), consumed by 0x57A0E0 */
#define A_TICK_HZ     0x007a061cu   /* 1800.0 */
#define A_TICK_4K     0x007a0aa8u   /* 4000/1800 */
#define A_TICK_S      0x007a0618u   /* 1/1800 */
#define A_RT_CARRY    0x00865144u   /* RealTimeFrames carry */
#define A_RTF         0x007fb708u   /* RealTimeFrames (60 Hz units) */
#define A_RTF_COPY    0x007fb704u
#define A_RTF_TICK    0x007fb70cu   /* RealTimeFrames this tick */
#define A_SIXTY       0x00787590u   /* 60.0 */
#define A_REALTIMER   0x008651acu   /* RealTimer, 1/4000 s */
#define A_WORLDTIMER  0x0086518cu   /* WorldTimer, 1/4000 s */
#define A_WORLD_S     0x007fb718u   /* WorldTimer in seconds (float) */
#define A_WTF         0x007fb720u   /* WorldTimeFrames */
#define A_WTF_REST    0x00865158u   /* WorldTimeFrames remainder (s) */
#define A_FRAME60     0x007875bcu   /* 1/60 */
#define A_U4000       0x00784268u   /* 1/4000 */
#define A_FRAMECNT    0x00870818u   /* eFrameCounter (render) */
#define A_STREAK_FC   0x008a44ecu   /* UpdateCarStreakingFlares: last frame */
#define A_CARLIST     0x00890080u   /* car list head (sentinel) */
#define A_RACERS      0x008900b4u   /* racer infos, [+4] = car */
#define A_NRACERS     0x008900bcu
#define A_SND_ON      0x007faaa8u
#define A_SND_PAUSE   0x0082b8dcu
#define A_ONLINE      0x0089cf69u   /* World+0x21 (byte): online session */
#define A_NET_CLIENT  0x008669f4u   /* client race state, > 0 in a network race */
#define A_NET_HOST    0x00866a14u   /* host race state, > 0 in a network race */

#define TICK_HZ   3600              /* tick base with the ticks fix */
#define TICK_MAX  180               /* 50 ms = 3 x [0x7875BC], the game's max dt */

/* ---- options ----------------------------------------------------------- */
static int    g_mode = M_AUTO;      /* M_OFF, M_UNCAPPED, M_AUTO or a fixed rate */
static int    g_fix = 0;            /* fixes in effect (fixes_follow_rate) */
static int    g_fix_hi = 0, g_fix_lo = 0, g_fix_30 = 0;   /* NFSU2_HFR_FIX above 60 fps, at 60, at 30 */
static double g_trail_len = 1.0;
static int    g_log = 0;
static FILE  *g_trace = 0;
static int    g_resolved = 0;
static int    g_lan_pc = 1;         /* NFSU2_LAN_TIMING: 1 = pc, 0 = exact */
static double g_rt_carry = 0;       /* timer fix: RealTimer's fraction */

/* mach_absolute_time in ns: the clock of CADisplayLink / presentedTime */
static uint64_t mono_ns(void){ return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
/* NFSU2_DISPLAY_HZ, or the autotest "displayhz" command (a screen change).
 * On the web NFSU2_DISPLAY_HZ is no override: the page sets it to what it
 * measured at Play, and metal_display_hz (gpu_web.c) gives it until the
 * renderer has measured the rate from requestAnimationFrame, then that rate as
 * it changes.  As an override it had auto stay at the rate of the start, on a
 * 30 Hz phase of Safari's or a move to another screen. */
static _Atomic int g_hz_forced = -1;
static int display_hz(void){
    int f = atomic_load(&g_hz_forced);
#ifdef __wasm__
    if (f < 0) f = 0;
#else
    if (f < 0){ const char *e = getenv("NFSU2_DISPLAY_HZ"); f = e && *e ? atoi(e) : 0;
                int un = -1; atomic_compare_exchange_strong(&g_hz_forced, &un, f); f = atomic_load(&g_hz_forced); }
#endif
    return f > 0 ? f : metal_display_hz();
}
void hfr_set_display_hz(int hz){ atomic_store(&g_hz_forced, hz > 0 ? hz : 0); }
/* best supported content rate for a refresh rate */
static int rate_for_hz(int hz){
    if (hz >= 239) return 240;
    if (hz >= 119) return 120;
    if (hz > 0 && hz <= 31) return 30;
    return 60;
}
static void parse_mode(void){
    const char *e = getenv("NFSU2_FPS");
    if (!e || !*e || !strcasecmp(e, "auto") || !strcasecmp(e, "native")) g_mode = M_AUTO;
    else if (!strcasecmp(e, "off") || !strcasecmp(e, "vanilla") || !strcasecmp(e, "game")) g_mode = M_OFF;
    else if (!strcasecmp(e, "uncapped") || !strcasecmp(e, "max")) g_mode = M_UNCAPPED;
    else {
        int v = atoi(e);
        if (v == 30 || v == 60 || v == 120 || v == 240) g_mode = v;
        else { fprintf(stderr, "[hfr] NFSU2_FPS=%s not supported (30|60|120|240|auto|uncapped|off): auto\n", e);
               g_mode = M_AUTO; }
    }
}
/* content rate of the mode itself (before the game's 30 fps requests);
 * 0 = uncapped, 120 = the game's own cap when off */
static int mode_rate(void){
    if (g_mode == M_AUTO) return rate_for_hz(display_hz());
    if (g_mode == M_UNCAPPED) return 0;
    if (g_mode == M_OFF) return 120;
    return g_mode;
}
static const char *mode_name(void){
    return g_mode == M_OFF ? "off" : g_mode == M_UNCAPPED ? "uncapped" : g_mode == M_AUTO ? "auto" : "fixed";
}
/* the fix set for a mode rate above 60 (or uncapped) when def = FX_ALL,
 * for 60 when def = 0, for 30 when def = FX_30 */
#define FX_30 (FX_TICKS | FX_PARTICLES | FX_RAIN | FX_SPIN)
static int parse_fix(int def, int warn){
    const char *e = getenv("NFSU2_HFR_FIX");
    if (!e || !*e) return def;
    int f = 0;
    char buf[256]; snprintf(buf, sizeof buf, "%s", e);
    for (char *t = strtok(buf, ", "); t; t = strtok(0, ", ")){
        int neg = *t == '-'; if (neg || *t == '+') t++;
        int bit = 0;
        if (!strcasecmp(t, "all")) bit = FX_ALL;
        else if (!strcasecmp(t, "none")) { f = 0; continue; }
        else if (!strcasecmp(t, "default")) bit = def;
        else for (int i = 0; i < FX_N; i++) if (!strcasecmp(t, g_fx_name[i])) bit = 1 << i;
        if (!bit && strcasecmp(t, "default")){
            if (warn) fprintf(stderr, "[hfr] NFSU2_HFR_FIX: unknown fix '%s'\n", t);
            continue; }
        f = neg ? (f & ~bit) : (f | bit);
    }
    return f;
}
static void fix_str(int f, char *out, size_t n){
    out[0] = 0;
    for (int i = 0; i < FX_N; i++) if (f & (1 << i)){
        size_t l = strlen(out); snprintf(out + l, n - l, "%s%s", l ? "," : "", g_fx_name[i]); }
    if (!out[0]) snprintf(out, n, "none");
}
static int fix_for_rate(int r){ return r > 60 || r == 0 ? g_fix_hi : r <= 30 ? g_fix_30 : g_fix_lo; }

/* Tick base 1/3600 s: 30, 60, 120 and 240 fps are 120, 60, 30 and 15 whole
 * ticks, so every supported rate gets dt = exactly 1/rate and the car
 * substep h = 1/240 s.  The three constants are read only by 0x57EAD0 and
 * 0x576CE0 (replay ticks -> s).  The JoyLog 8-bit tick field still fits
 * (180 at the 50 ms maximum).  1800 Hz (vanilla) cannot express 240 fps. */
static void set_tick_base(int on){
    static int saved = 0;
    static float van[3];            /* the game's own 1/1800 base */
    if (!saved){ saved = 1; van[0] = MRF32(A_TICK_HZ); van[1] = MRF32(A_TICK_4K); van[2] = MRF32(A_TICK_S); }
    MRF32(A_TICK_HZ) = on ? (float)TICK_HZ : van[0];
    MRF32(A_TICK_4K) = on ? (float)(4000.0 / TICK_HZ) : van[1];
    MRF32(A_TICK_S)  = on ? (float)(1.0 / TICK_HZ) : van[2];
}

static void resolve_once(void){
    parse_mode();
    g_fix_hi = g_mode == M_OFF ? 0 : parse_fix(FX_ALL, 1);
    g_fix_lo = g_mode == M_OFF ? 0 : parse_fix(0, 0);
    g_fix_30 = g_mode == M_OFF ? 0 : parse_fix(FX_30, 0);
    g_fix = fix_for_rate(mode_rate());
    { const char *e = getenv("NFSU2_NOS_TRAIL_LEN"); if (e && *e) g_trail_len = atof(e);
      if (!(g_trail_len > 0.05 && g_trail_len < 20.0)) g_trail_len = 1.0; }
    { const char *e = getenv("NFSU2_HFR_LOG"); g_log = e && *e ? atoi(e) : 0; }
    { const char *e = getenv("NFSU2_LAN_TIMING");
      if (e && *e){
          if (!strcasecmp(e, "exact")) g_lan_pc = 0;
          else if (strcasecmp(e, "pc")) fprintf(stderr, "[hfr] NFSU2_LAN_TIMING=%s is not pc|exact: pc\n", e); } }
    { const char *e = getenv("NFSU2_HFR_TRACE");
      if (e && *e){ g_trace = fopen(e, "w");
          if (g_trace) fprintf(g_trace, "frame,wall_s,sim_s,real_s,slots,ticks,realdt,worlddt,"
                                        "x,y,z,speed,dist,realtimer,worldtimer,rtf,wtf,"
                                        "au_upd,au_dt,part_s,trail_n,work_ms,sleep_ms,"
                                        "c0x,c0y,c0z,c0v,c1x,c1y,c1z,c1v,c2x,c2y,c2z,c2v,c3x,c3y,c3z,c3v,streak0,trail0_m,"
                                        "fx_acid,fx_world\n"); } }
    char fs[96]; fix_str(g_fix, fs, sizeof fs);
    fprintf(stderr, "[hfr] mode %s, display %d Hz, content rate %d, fixes: %s\n",
            mode_name(), display_hz(), mode_rate(), fs);
    g_resolved = 1;
}
/* first use may come from the main thread (layer setup) or the guest */
static void resolve(void){
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, resolve_once);
}

int hfr_mode_off(void){ resolve(); return g_mode == M_OFF; }
int hfr_uncapped(void){ resolve(); return g_mode == M_UNCAPPED; }

/* ---- current content rate --------------------------------------------- */
/* The game's own request in [0x865558]: 1/120 by default (= "the mode's
 * rate"), 1/30 in NIS.  Requests are honoured only
 * downwards: the default 1/120 must not cap 240.
 * A network race on the game's own clocks (NFSU2_LAN_TIMING=pc) runs at
 * most 120: no unmodified PC gets past its limiter's 1/120, and 1/240 is
 * 7.5 ticks of 1/1800 s, which the game's quantiser makes 8 a frame.
 * Measured at 240: simulation 1.06 x the wall clock, WorldTimer 1.011 per
 * unit of world dt (0.7 % less car physics per race second than a PC at
 * 60, 1.6 % less than one at 120), and a 240 host ran a LAN race about
 * 7 % fast for every player. */
static int net_race(void);
static _Atomic int g_rate_now = 0;
static int content_rate(void){
    int m = mode_rate();
    if (m > 120 && g_lan_pc && net_race()) m = 120;
    float cap = MRF32(A_CAP);
    if (cap > 0.f && cap != MRF32(A_CAP_DEF)){
        int req = (int)lround(1.0 / (double)cap);
        if (req >= 10 && req < m) m = req;
    }
    return m;
}
/* fps = 30, or auto on a 30 Hz display, outside the game's own 30 fps
 * requests (a cutscene runs at 30 in every mode and keeps the effects it has
 * there at 60): a frame is two of 60, so effects stepped once a frame step
 * for both (F2, F6) */
static int at_30(void){
    float cap = MRF32(A_CAP);
    return mode_rate() == 30 && !(cap > 0.f && cap != MRF32(A_CAP_DEF));
}
/* frame-rate counter (fpscounter.m): the mode's name, its own rate (0 =
 * uncapped) and the content rate of the last frame (the game's 30 fps
 * requests in cutscenes included; 0 = uncapped) */
const char *hfr_mode_label(void){ resolve(); return mode_name(); }
int hfr_mode_rate(void){ resolve(); return mode_rate(); }
int hfr_rate_now(void){ return atomic_load(&g_rate_now); }
/* metal_present: replace undisplayed frames instead of waiting for the
 * presenter.  Uncapped, and a content rate above the display's (240 on this
 * 120 Hz panel): the simulation keeps its rate, the display shows what it
 * can. */
int hfr_mailbox(void){
    if (g_mode == M_UNCAPPED) return 1;
    int r = atomic_load(&g_rate_now), hz = display_hz();
    return r > 0 && hz > 0 && r > hz + 1;
}
/* presentDrawable:afterMinimumDuration: for the main thread, 0 = none.
 * Half a refresh less than the content period: a frame never lands in the
 * slot right after its predecessor, and a refresh rate a hair above
 * nominal cannot push it one slot too far. */
double hfr_present_min_duration(void){
    static int off = -1;
    if (off < 0){ const char *e = getenv("NFSU2_PRESENT_MINDUR"); off = e && *e == '0'; }
    if (off) return 0.0;
    int r = atomic_load(&g_rate_now);
    int hz = display_hz();
    if (r <= 0 || hz <= 0) return 0.0;
    double d = 1.0 / r - 0.5 / hz;
    return d > 0.0005 ? d : 0.0;
}
/* virtual clock (win32.c vclock_tick): smallest per-Present step.  4 ms was
 * a 250 fps cap; 240 needs a lower floor or game time would run fast on
 * every frame that comes in under 4 ms. */
uint64_t hfr_vclock_floor_us(void){
    if (g_mode == M_UNCAPPED) return 500u;
    if (g_mode != M_OFF && atomic_load(&g_rate_now) > 200) return 1000u;
    return 4000u;
}

/* ---- statistics --------------------------------------------------------- */
typedef struct {
    uint64_t frames, slots, missed, sleep_ns, work_ns;
    double   sim_s, real_s, wall_s;
    uint64_t rtf, wtf, rtimer, wtimer;
    uint64_t au_upd; double au_dt;
    double   part_s;
    uint64_t trail_samples;
} Stat;
static Stat g_tot, g_sec;            /* counted frames (racing, or all with LOG=2): total, this second */
static uint64_t g_sec_t0;
static double   g_sim_ms = 0;        /* world time since start, ms (autotest "wms") */
#define WH_BINS 256
static uint64_t g_wh[WH_BINS];       /* guest work time per counted frame, 0.25 ms bins; the last one
                                        holds everything from 63.75 ms up */
static uint64_t g_sleep_all_ns = 0;  /* pacer sleep, all frames (perflog) */
static double   g_race_ms = -1;      /* world time at which GameFlow last became 6 */
double hfr_sim_ms(void){ return g_sim_ms; }
double hfr_race_start_ms(void){ return MR32(A_FLOW) == 6u ? g_race_ms : -1; }
uint64_t hfr_paced_sleep_us(void){ return g_sleep_all_ns / 1000u; }

/* Interval histograms: presentedTime (what the display showed; metal.m
 * addPresentedHandler, any thread) and the main thread's present calls. */
typedef struct {
    double   last, sum, sq;
    uint64_t hist[200];             /* 0.25 ms bins up to 50 ms */
    uint64_t n, over, zero, in, longer, shorter;
    int      rate;                  /* content rate the previous sample belonged to */
} IvHist;
static pthread_mutex_t g_pl = PTHREAD_MUTEX_INITIALIZER;
static IvHist g_ph, g_pc;
static void iv_note(IvHist *h, double t){
    pthread_mutex_lock(&g_pl);
    if (!(t > 0)){ h->zero++; pthread_mutex_unlock(&g_pl); return; }
    int r = atomic_load(&g_rate_now);
    if (h->last > 0 && t > h->last){
        double d = (t - h->last) * 1e3;
        int b = (int)(d * 4.0);
        if (b < 200) h->hist[b]++; else h->over++;
        h->n++; h->sum += d; h->sq += d * d;
        if (r > 0 && r == h->rate){
            double want = 1e3 / r;
            if (fabs(d - want) <= 0.5) h->in++;
            else if (d > want) h->longer++;
            else h->shorter++;
        }
    }
    h->rate = r; h->last = t;
    pthread_mutex_unlock(&g_pl);
}
void hfr_note_presented(double t){ iv_note(&g_ph, t); }
void hfr_note_present_call(void){ iv_note(&g_pc, mono_ns() / 1e9); }
static void iv_report(const char *what, IvHist *h){
    pthread_mutex_lock(&g_pl);
    if (h->n){
        double mean = h->sum / h->n, sd = sqrt(fmax(0.0, h->sq / h->n - mean * mean));
        double p[3] = {0.5, 0.95, 0.99}, v[3] = {0, 0, 0}; uint64_t acc = 0; int k = 0;
        for (int i = 0; i < 200 && k < 3; i++){ acc += h->hist[i];
            while (k < 3 && acc >= (uint64_t)(p[k] * h->n)){ v[k] = (i + 1) * 0.25; k++; } }
        fprintf(stderr, "  %s: %llu intervals, mean %.3f ms, sd %.3f ms, p50 <= %.2f, p95 <= %.2f, p99 <= %.2f ms, >50 ms %llu\n",
                what, (unsigned long long)h->n, mean, sd, v[0], v[1], v[2], (unsigned long long)h->over);
        uint64_t m = h->in + h->longer + h->shorter;
        if (m) fprintf(stderr, "    at a steady content rate: %.2f %% within +-0.5 ms of the period, %llu longer, %llu shorter\n",
                       100.0 * h->in / m, (unsigned long long)h->longer, (unsigned long long)h->shorter);
        fprintf(stderr, "    histogram (ms: count, bins >= 0.1 %%):");
        for (int i = 0; i < 200; i++) if (h->hist[i] * 1000 >= h->n)
            fprintf(stderr, " %.2f-%.2f:%llu", i * 0.25, (i + 1) * 0.25, (unsigned long long)h->hist[i]);
        fprintf(stderr, "\n");
    }
    if (h->zero) fprintf(stderr, "  %s: %llu samples without a time (drawable not shown)\n",
                         what, (unsigned long long)h->zero);
    pthread_mutex_unlock(&g_pl);
}

/* ---- vsync clock (metal.m CADisplayLink) --------------------------------
 * Last vsync time and the refresh period measured over a long baseline
 * (whole refreshes counted against the nominal duration), both in ns of
 * mach_absolute_time.  A change of the nominal duration (other screen,
 * ProMotion stepping down) restarts the measurement. */
static _Atomic uint64_t g_vs_ts = 0, g_vs_period = 0;
static double g_vs_t0 = 0, g_vs_nom = 0;
void hfr_note_vsync(double ts, double dur){
    if (!(ts > 0) || !(dur > 0.001 && dur < 0.1)) return;
    if (!g_vs_t0 || fabs(dur - g_vs_nom) > 0.02 * g_vs_nom){
        g_vs_t0 = ts; g_vs_nom = dur; atomic_store(&g_vs_period, 0);
    }
    double k = floor((ts - g_vs_t0) / g_vs_nom + 0.5);
    if (k >= 240){                   /* >= 2 s of baseline */
        double p = (ts - g_vs_t0) / k;
        if (fabs(p / g_vs_nom - 1.0) < 0.002) atomic_store(&g_vs_period, (uint64_t)llround(p * 1e9));
    }
    atomic_store(&g_vs_ts, (uint64_t)llround(ts * 1e9));
}
/* The display clock ticked in the last 250 ms: the page is drawing (a hidden
 * tab gets no requestAnimationFrame).  win32_ext.c spins the end of the
 * pacer's sleep only then. */
int hfr_display_live(void){
    uint64_t tv = atomic_load(&g_vs_ts);
    return tv && (int64_t)(mono_ns() - tv) < 250000000ll;
}

/* ---- the pacer ----------------------------------------------------------
 * Slots of the content period (MW kit SlotPacer semantics): a frame that is
 * early sleeps to the start of the slot after its predecessor's and takes
 * that slot; a late frame takes the last slot boundary it passed and the
 * missed slots are dropped (no burst to catch up).  The game's dt is the
 * number of slots times the period, so game time follows the wall clock to
 * within one slot whatever the frame times are.
 * With the vsync clock the boundaries sit half a refresh after the vsyncs
 * and the period is k measured refreshes (k = display Hz / content rate):
 * production matches the display exactly, and a frame released by the
 * presenter around a vsync lands mid-slot instead of on a boundary. */
static int64_t g_last_slot = 0;     /* start of the previous frame's slot, ns */
static uint64_t g_lock_n = 0, g_lock_ok = 0;
static int64_t snap(int64_t t, int64_t origin, int64_t g){   /* nearest grid point */
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
    g_lock_n++;
    if (pv && tv){
        double k = floor(nominal / (double)pv + 0.5);
        if (k >= 1 && fabs(k * (double)pv - nominal) < 0.01 * nominal){
            P = (int64_t)llround(k * (double)pv); g = (int64_t)pv; origin = (int64_t)tv + g / 2;
            g_lock_ok++;
        }
    }
    if (!g_last_slot || now - g_last_slot > 4 * 1000000000ll){     /* first frame, or after a load */
        int64_t s0 = snap(now, origin, g); if (s0 > now) s0 -= g;
        g_last_slot = s0;
        return 1;
    }
    int64_t target = snap(g_last_slot + P, origin, g);
    if (target <= g_last_slot) target += g;
    if (now < target){
        guest_sleep_frame_ns((int64_t)(target - now));
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

static void stat_add(Stat *d, const Stat *s){
    d->frames += s->frames; d->slots += s->slots; d->missed += s->missed;
    d->sleep_ns += s->sleep_ns; d->work_ns += s->work_ns;
    d->sim_s += s->sim_s; d->real_s += s->real_s; d->wall_s += s->wall_s;
    d->rtf += s->rtf; d->wtf += s->wtf; d->rtimer += s->rtimer; d->wtimer += s->wtimer;
    d->au_upd += s->au_upd; d->au_dt += s->au_dt; d->part_s += s->part_s;
    d->trail_samples += s->trail_samples;
}
static void stat_line(const char *tag, const Stat *s){
    double w = s->wall_s > 0 ? s->wall_s : 1e-9;
    fprintf(stderr, "[hfr] %s wall=%.2fs fps=%.2f slots/f=%.3f miss=%llu sim/wall=%.4f real/wall=%.4f "
            "realtimer/real=%.4f worldtimer/sim=%.4f rtf=%.2f/s wtf=%.2f/s audio=%.1fupd/s,%.3fs/s "
            "particles=%.3fs/s trail=%.1f/s work=%.2fms/f sleep=%.2fms/f\n",
            tag, s->wall_s, s->frames / w, s->frames ? (double)s->slots / s->frames : 0.0,
            (unsigned long long)s->missed, s->sim_s / w, s->real_s / w,
            s->real_s > 0 ? s->rtimer / 4000.0 / s->real_s : 0.0,
            s->sim_s > 0 ? s->wtimer / 4000.0 / s->sim_s : 0.0,
            s->rtf / w, s->wtf / w, s->au_upd / w, s->au_dt / w, s->part_s / w,
            s->trail_samples / w, s->frames ? s->work_ns / 1e6 / s->frames : 0.0,
            s->frames ? s->sleep_ns / 1e6 / s->frames : 0.0);
}

/* The frame being measured runs from one FrameTimeUpdate's pacer wake-up to
 * the next: the work of one main-loop iteration plus the pacer's sleep. */
static struct {
    uint64_t t, work;               /* start (after the pacer), work time */
    int      slots;
    uint32_t rt0, wt0, rtf0, wtf0;
    double   wdt, au_dt, part;
    uint64_t au_upd, trail;
} g_f;
static double   g_trace_real = 0, g_trace_sim = 0;
static uint64_t g_trace_t0 = 0, g_trace_n = 0;

static void trace_row(const Stat *f, uint64_t sleep){
    if (!g_trace_t0) g_trace_t0 = g_f.t;
    g_trace_real += f->real_s; g_trace_sim += f->sim_s;
    uint32_t car = 0;
    if (MR32(A_NRACERS) >= 1u){ uint32_t ri = MR32(A_RACERS);
        if (ri && mem_is_committed(ri + 7)) car = MR32(ri + 4); }
    float x = 0, y = 0, z = 0, sp = 0, di = 0;
    if (car && mem_is_committed(car + 0x8ff)){
        x = MRF32(car + 0x60); y = MRF32(car + 0x64); z = MRF32(car + 0x68);
        sp = MRF32(car + 0x430); di = MRF32(car + 0x8fc); }
    fprintf(g_trace, "%llu,%.6f,%.6f,%.6f,%d,%u,%.7g,%.7g,%.4f,%.4f,%.4f,%.4f,%.3f,%u,%u,%u,%u,"
            "%llu,%.5f,%.5f,%llu,%.3f,%.3f",
            (unsigned long long)++g_trace_n, (g_f.t - g_trace_t0) / 1e9, g_trace_sim, g_trace_real,
            g_f.slots, MR32(A_TICKS), (double)MRF32(A_REALDT), g_f.wdt,
            x, y, z, sp, di, MR32(A_REALTIMER), MR32(A_WORLDTIMER), MR32(A_RTF), MR32(A_WTF),
            (unsigned long long)g_f.au_upd, g_f.au_dt, g_f.part, (unsigned long long)g_f.trail,
            g_f.work / 1e6, sleep / 1e6);
    /* every car of the World (0x609CE0: [world+0x1C+4i], i < [world+0x10]):
     * the AI drives whole laps by itself, a long deterministic physics test */
    uint32_t w = MR32(A_CARLIST), nw = w && mem_is_committed(w + 0x2f) ? MR32(w + 0x10) : 0;
    for (uint32_t i = 0; i < 4; i++){
        uint32_t cr = i < nw ? MR32(w + 0x1c + 4 * i) : 0;
        if (cr && mem_is_committed(cr + 0x433))
            fprintf(g_trace, ",%.4f,%.4f,%.4f,%.4f", MRF32(cr + 0x60), MRF32(cr + 0x64), MRF32(cr + 0x68),
                    MRF32(cr + 0x430));
        else fprintf(g_trace, ",,,,");
    }
    /* car+0x42C > 0 gates the streak/NOS trail drawing (0x61B04E) */
    { uint32_t c0 = nw ? MR32(w + 0x1c) : 0;
      fprintf(g_trace, ",%.4f", c0 && mem_is_committed(c0 + 0x42f) ? (double)MRF32(c0 + 0x42c) : 0.0);
      /* the player's streak trail: distance newest -> oldest ring entry (F3) */
      double span = 0;
      uint32_t r = c0 && mem_is_committed(c0 + 0x557) ? MR32(c0 + 0x554) : 0;
      if (r && mem_is_committed(r + 0x2183)){
          uint32_t i = MR32(r + 0x2180);
          if (i < 3){
              uint32_t a = r + 0x2150 + i * 16u, b = r + 0x2150 + ((i + 1) % 3) * 16u;
              double dx = MRF32(a) - MRF32(b), dy = MRF32(a + 4) - MRF32(b + 4), dz = MRF32(a + 8) - MRF32(b + 8);
              span = sqrt(dx * dx + dy * dy + dz * dz);
          } }
      fprintf(g_trace, ",%.4f", span); }
    /* live particle groups in the two systems: 0x835860 (stepped by
     * eDisplayFrame, F2) and 0x8357E0 (stepped with world dt, 0x60A181);
     * 0x492E10 walks the groups from [sys] with the system as sentinel */
    for (int k = 0; k < 2; k++){
        uint32_t sys = k ? 0x008357e0u : 0x00835860u, head = sys, e = MR32(head);
        int n = 0;
        while (e && e != head && n < 100000 && mem_is_committed(e + 3)){ n++; e = MR32(e); }
        fprintf(g_trace, ",%d", n);
    }
    fputc('\n', g_trace);
}
/* closes the previous frame at `now` (after the pacer); its sleep and the
 * slots it skipped were just measured by pace() */
static void frame_close(uint64_t now, uint64_t sleep, uint64_t missed){
    if (!g_f.t) return;
    int racing = MR32(A_FLOW) == 6u;
    if (racing || g_log == 2){
        Stat f; memset(&f, 0, sizeof f);
        f.frames = 1; f.slots = (uint64_t)g_f.slots; f.missed = missed;
        f.sleep_ns = sleep; f.work_ns = g_f.work;
        f.wall_s = (now - g_f.t) / 1e9;
        f.real_s = MRF32(A_REALDT);
        f.sim_s  = g_f.wdt;
        f.rtimer = MR32(A_REALTIMER) - g_f.rt0; f.wtimer = MR32(A_WORLDTIMER) - g_f.wt0;
        f.rtf = MR32(A_RTF) - g_f.rtf0; f.wtf = MR32(A_WTF) - g_f.wtf0;
        f.au_upd = g_f.au_upd; f.au_dt = g_f.au_dt; f.part_s = g_f.part; f.trail_samples = g_f.trail;
        stat_add(&g_sec, &f);
        { uint64_t b = g_f.work / 250000u; g_wh[b < WH_BINS - 1 ? b : WH_BINS - 1]++; }
        if (g_trace && racing) trace_row(&f, sleep);
    }
    if (!g_sec_t0) g_sec_t0 = now;
    if (now - g_sec_t0 >= 1000000000ull){
        if (g_sec.frames){
            if (g_log) stat_line("1s", &g_sec);
            stat_add(&g_tot, &g_sec);
        }
        memset(&g_sec, 0, sizeof g_sec);
        g_sec_t0 = now;
    }
}
static void frame_open(uint64_t now, int slots){
    static uint32_t flow_prev = 0;
    uint32_t flow = MR32(A_FLOW);
    if (flow == 6u && flow_prev != 6u) g_race_ms = g_sim_ms;
    flow_prev = flow;
    memset(&g_f, 0, sizeof g_f);
    g_f.t = now; g_f.slots = slots;
    g_f.rt0 = MR32(A_REALTIMER); g_f.wt0 = MR32(A_WORLDTIMER);
    g_f.rtf0 = MR32(A_RTF); g_f.wtf0 = MR32(A_WTF);
}

/* ---- F1: FrameTimeUpdate 0x57EAD0 (limiter, clamp, tick quantizer) -----
 * cdecl, [esp+4] = work time since the previous limiter (s).  The original
 * busy-waits until [0x865558] has passed, adds the wait to dt (overshoot is
 * discarded), clamps to [cap, 50 ms] and quantises to 1/1800 s ticks with a
 * mixed-domain carry: +1.33 % game speed when the limiter binds at 1/120,
 * 15/16/14-tick jitter on a steady display (research §1.2, review B.1).
 *
 * Here the host pacer does the waiting (GIL released) and the original runs
 * with its limiter disarmed:
 *   ticks fix: dt = T/3600 with T the exact slot count in ticks, carry 0,
 *     cap 0.  The original then computes ticks = T itself, so the JoyLog
 *     channel-3 write at 0x57EC5F records the same ticks the game uses
 *     (review B.4).
 *   without:   dt = slots x period, cap = period: the vanilla quantizer in
 *     its limiter-bound state (exactly 30 ticks per frame at 60).
 * JoyLog playback ([0x8650D0]) keeps its recorded tick stream. */
/* ---- The game's frame wait loop: sleep instead of spinning ----
 * sub_0057ead0 (0x57eb4e..0x57eb7e), when the frame time dt is below target:
 *     start = GetTicks(); target = (X - dt) * k;
 *     do { elapsed = Diff(start, GetTicks()); } while (elapsed < target);
 * GetTicks = sub_0043bdf0 = QPC >> [0x8284e4]; Diff = sub_0043be20 =
 * (now - start) * 2^S * [0x828028] seconds.  The loop only looks at the
 * clock; at 120 fps ~19 thousand QPCs per frame and ALL of the game
 * thread's idle time (8.2 ms of CPU per frame, 98%) burned here: every ms
 * saved in D3DX went here, the power draw did not change.
 * On a QPC from the loop (return 0x43bdfe, its caller 0x57eb69) the time
 * left is computed with the game's own formula and we sleep for (left -
 * margin); the final margin is still waited out spinning.  The exit
 * condition and the clock are the same, so the game's timing does not
 * change, only the CPU goes idle.  The sleep uses kevent NOTE_CRITICAL
 * (guest_sleep_ns): nanosleep woke 447 us late on average, kevent 41 us
 * (98% <= 200 us).
 * SPINSLEEP=<margin us> (default 200), SPINSLEEP=0 disables it.
 * Stack: after POP32 [esp+0xc] = 0x57eb69, E = esp+0x14 (0057ead0's local
 * base), target float [E+0xc] = [esp+0x20], start = EBX. */
static int g_spin_margin = -1;
static uint64_t g_spin_n, g_spin_us, g_spin_late[7];   /* late wake-up: <=25,50,100,200,400,800,>800 us */
static void spin_report(void){
    fprintf(stderr, "\n=== SPINSLEEP: the frame wait loop slept %llu times, %.2f s in total (margin %d us) ===\n",
            (unsigned long long)g_spin_n, g_spin_us / 1e6, g_spin_margin);
    fprintf(stderr, "  late wake-ups: <=25us %llu  <=50 %llu  <=100 %llu  <=200 %llu  <=400 %llu  <=800 %llu  >800 %llu\n",
            (unsigned long long)g_spin_late[0], (unsigned long long)g_spin_late[1], (unsigned long long)g_spin_late[2],
            (unsigned long long)g_spin_late[3], (unsigned long long)g_spin_late[4], (unsigned long long)g_spin_late[5],
            (unsigned long long)g_spin_late[6]);
    extern uint64_t g_sleep_late_ns, g_sleep_gil_ns, g_sleep_n;
    if (g_sleep_n) fprintf(stderr, "  average: nanosleep lateness %.0f us, lock wait on wake-up %.0f us\n",
                           g_sleep_late_ns / 1e3 / g_sleep_n, g_sleep_gil_ns / 1e3 / g_sleep_n);
}
uint64_t spin_slept_us(void){ return g_spin_us; }   /* perflog: frame limiter sleep */
uint64_t host_qpc(void);                /* win32.c */
/* every QueryPerformanceCounter of the exe (game.h on_qpc, win32.c), after
 * its return address was taken */
void spinsleep_qpc(CPU *c){
    if (c->eip != 0x43bdfeu || MR32(c->esp + 0xcu) != 0x57eb69u) return;
    if (g_spin_margin < 0){ const char *e = getenv("SPINSLEEP"); g_spin_margin = e ? atoi(e) : 200;
                            if (g_spin_margin && getenv("QPCWHO")) atexit(spin_report); }
    if (!g_spin_margin) return;
    /* the game's units: F = [0x828028] per QPC tick (measured 0.001 = ms,
     * k = [0x784550] = 1000), X = [0x865558] = 1/120 s frame target */
    uint32_t S = MR32(0x8284e4u) & 31u;
    float F = MRF32(0x828028u), target = MRF32(c->esp + 0x20u);
    if (!(F > 0.0f) || !(target > 0.0f)) return;
    uint64_t now = host_qpc();
    int32_t d = (int32_t)((uint32_t)(now >> S) - c->ebx); if (d < 0) d = 0;
    double left = (double)target - (double)d * (double)(1u << S) * (double)F;
    double us = left / (double)F - (double)g_spin_margin;      /* QPC tick = us (QPF 1 MHz) */
    if (us < 50.0 || us > 100000.0) return;                  /* short: spin; >0.1 s: unexpected */
    void guest_sleep_ns(int64_t ns);
    guest_sleep_ns((int64_t)(us * 1000.0));
    g_spin_n++; g_spin_us += (uint64_t)us;
    { double late = (double)(host_qpc() - now) - us;         /* us slept beyond what was asked */
      int b = late <= 25 ? 0 : late <= 50 ? 1 : late <= 100 ? 2 : late <= 200 ? 3 : late <= 400 ? 4 : late <= 800 ? 5 : 6;
      g_spin_late[b]++; }
}
void sub_0057ead0_orig(CPU *c);
static uint64_t hfr_spin_slept_us(void){ return spin_slept_us(); }
static double   g_tick_acc = 0;
/* A network race: the World constructor's test for the 30 fps cap
 * (0x5FF652) and World::IsOnline (0x5F00F0). */
static int net_race(void){
    return MR8(A_ONLINE) || (int32_t)MR32(A_NET_CLIENT) > 0 || (int32_t)MR32(A_NET_HOST) > 0;
}
/* The World constructor (thiscall, returns this) asks a network race for 30
 * fps (0x5FF67C: SetFrameRate [0x7A572C]), and only the race intro's NIS end
 * (0x437A00) sets the default 1/120 again.  A race without an intro ran at
 * 30 to the finish, and six cars (runtime/netrace.c) get none: measured
 * online at auto 120, two cars ran 30 for the intro and then 120, six cars
 * 30 throughout.  The request is undone in every mode, off included: the
 * intro still runs at 30 (its own request, 0x435FA3), the race at the
 * mode's rate. */
void sub_005ff490_orig(CPU *c);
void sub_005ff490(CPU *c){
    float cap = MRF32(A_CAP);
    sub_005ff490_orig(c);
    MRF32(A_CAP) = cap;
}
/* The fix set follows the mode's rate class at a frame boundary: auto
 * changes the content rate when the window moves to a screen with another
 * refresh rate, and a game that started on a 60 Hz screen needs the fixes
 * once it runs at 120 (and is vanilla again back at 60).
 *
 * Network races (NFSU2_LAN_TIMING=pc, the default) run without ticks and
 * timer.  Every client slaves its WorldTimer to the race host's (0x5891B0,
 * 0x601A60), so how much car physics a machine simulates per race second is
 * its own world dt per WorldTimer unit.  An unmodified PC advances WorldTimer
 * by round(dt x 4000) with no carry: 1.0046 per unit of world dt at 60 fps,
 * 0.9950 at the limiter-bound 1/120 (measured);
 * the timer fix makes it 1.0000, which matches no PC: a Mac at 120 would get
 * about 0.5 % more simulated time per race second than a 60 fps PC, and 0.5 %
 * less than a PC at its default 1/120.  Without the two fixes the Mac runs
 * the game's own quantiser and timers at its frame rate, as a PC at that rate
 * does (equal frame rates: equal race; unequal ones: the game's own
 * difference, as between two PCs).  The other fixes only change effects,
 * sound and trails. */
static void fixes_follow_rate(void){
    static int first = 1, net_prev = 0;
    int r = mode_rate(), fx = fix_for_rate(r);
    int net = g_lan_pc && net_race();
    if (net) fx &= ~(FX_TICKS | FX_TIMER);
    if (!first && net != net_prev){
        char a[96], b[96]; fix_str(g_fix, a, sizeof a); fix_str(fx, b, sizeof b);
        fprintf(stderr, "[hfr] network race %s: content rate %d, fixes %s -> %s%s\n", net ? "begins" : "over",
                net && r > 120 ? 120 : r, a, b,
                net ? " (the game's own clocks, as on an unmodified PC at this rate)" : "");
    } else if (!first && fx != g_fix){
        char a[96], b[96]; fix_str(g_fix, a, sizeof a); fix_str(fx, b, sizeof b);
        fprintf(stderr, "[hfr] display %d Hz: content rate %d, fixes %s -> %s\n", display_hz(), r, a, b);
    }
    net_prev = net;
    if (!first && fx == g_fix) return;
    if (first || ((fx ^ g_fix) & FX_TICKS)){ set_tick_base(fx & FX_TICKS); g_tick_acc = 0; }
    if ((fx ^ g_fix) & FX_TIMER) g_rt_carry = 0;
    g_fix = fx; first = 0;
}
static void fe_spinners(void);          /* F7 */
void sub_0057ead0(CPU *c){
    resolve();
    if (g_mode == M_OFF){
        /* vanilla timing: the game's own limiter inside the original; only
         * the bookkeeping (log, trace, report) around it */
        uint64_t t_in = mono_ns();
        if (g_f.t) g_f.work = t_in - g_f.t;
        uint64_t sl0 = hfr_spin_slept_us();
        sub_0057ead0_orig(c);
        uint64_t now = mono_ns();
        frame_close(now, (hfr_spin_slept_us() - sl0) * 1000u, 0);
        frame_open(now, 1);
        return;
    }
    fixes_follow_rate();
    uint64_t t_in = mono_ns();
    if (g_f.t) g_f.work = t_in - g_f.t;

    int rate = g_mode == M_UNCAPPED ? 0 : content_rate();
    atomic_store(&g_rate_now, rate);
    int slots = 1; double frame_s; uint64_t slept = 0, missed = 0;
    if (rate > 0){
        /* Test only (image A/B captures):
         * NFSU2_HFR_LOCKSTEP=1 gives every frame exactly one slot of game
         * time whatever the wall clock did, so two runs of one script show
         * the same world at the same frame; NFSU2_HFR_LOCKSTEP_WALL=<fps>
         * also paces the wall clock at that rate (slow configurations then
         * reach timed events in the same state). */
        static int ls = -1, lsw = 0;
        if (ls < 0){ const char *e = getenv("NFSU2_HFR_LOCKSTEP"); ls = e && *e == '1';
                     e = getenv("NFSU2_HFR_LOCKSTEP_WALL"); lsw = e ? atoi(e) : 0;
                     if (ls) fprintf(stderr, "[hfr] LOCKSTEP test pacing (wall %d fps)\n", lsw > 0 ? lsw : rate); }
        slots = pace(ls && lsw > 0 ? lsw : rate, &slept, &missed);
        if (ls){ slots = 1; missed = 0; }
        frame_s = (double)slots / rate;
    } else frame_s = g_f.t ? (t_in - g_f.t) / 1e9 : 1.0 / 120;
    g_sleep_all_ns += slept;
    uint64_t now = mono_ns();
    frame_close(now, slept, missed);
    frame_open(now, slots);

    if (MR32(A_PLAYBACK)){ sub_0057ead0_orig(c); fe_spinners(); return; }
    float cap = MRF32(A_CAP);
    if (g_fix & FX_TICKS){
        g_tick_acc += frame_s * TICK_HZ;
        double t = floor(g_tick_acc + 0.5);
        g_tick_acc -= t;
        if (t > TICK_MAX){ t = TICK_MAX; g_tick_acc = 0; }   /* the game's 50 ms max dt: slow down, no catch-up */
        if (t < 0) t = 0;
        MRF32(c->esp + 4u) = (float)(t / TICK_HZ);
        MRF32(A_CARRY) = 0.f;
        MRF32(A_CAP) = 0.f;
        sub_0057ead0_orig(c);
        MRF32(A_CARRY) = 0.f;
    } else {
        double p = rate > 0 ? 1.0 / rate : 0.0;
        MRF32(c->esp + 4u) = (float)frame_s;
        MRF32(A_CAP) = (float)(p < frame_s ? p : frame_s);
        sub_0057ead0_orig(c);
    }
    MRF32(A_CAP) = cap;
    fe_spinners();
}

/* ---- F5 + timer: RealTimer advance 0x57A050 ------------------------------
 * n = trunc(60 (dt + carry)), carry -= n/60, then "if (n == 0) n = 1"
 * (0x57A084): one RealTimeFrame per rendered frame below 60 Hz frame
 * times, 2x at 120.  The carry was already computed with n = 0, so undoing
 * the forced 1 leaves the game's own exact 60 Hz count (= the EB 05 byte
 * patch).  RealTimer += round(dt x 4000) has no carry: 15/1800 s -> 33 units
 * (-1 %); the timer fix carries the fraction. */
void sub_0057a050_orig(CPU *c);
void sub_0057a050(CPU *c){
    float dt = MRF32(A_REALDT), carry0 = MRF32(A_RT_CARRY);
    uint32_t frames0 = MR32(A_RTF), timer0 = MR32(A_REALTIMER);
    sub_0057a050_orig(c);
    if (g_fix & FX_RTF){
        int n0 = (int)(((double)dt + (double)carry0) * (double)MRF32(A_SIXTY));
        if (n0 == 0 && MR32(A_RTF_TICK) == 1u){
            MR32(A_RTF_TICK) = 0; MR32(A_RTF) = frames0; MR32(A_RTF_COPY) = frames0;
        }
    }
    if (g_fix & FX_TIMER){
        double ex = (double)dt * 4000.0 + g_rt_carry;
        double u = floor(ex + 0.5);
        g_rt_carry = ex - u;
        MR32(A_REALTIMER) = timer0 + (uint32_t)(int32_t)u;
    }
}

/* ---- timer: WorldTimer advance 0x57A0E0 (skipped while world dt == 0) --
 * WorldTimer += round(dt x 4000) has no carry either: 15/3600 s -> 17 units
 * (+2 %).  The same function keeps an exact world clock of its own:
 * WorldTimeFrames ([0x7FB720], 60ths) and their remainder ([0x865158], s),
 * n = trunc(60 (dt + remainder)).  The timer fix moves WorldTimer by that
 * clock's change, rounded: no fraction of ours to carry.  A replay needs it
 * so: it restores the race from a snapshot (the "WorldTime" block, 0x57BBA0,
 * holds WorldTimer, WorldTimeFrames and the remainder, not a carry of ours),
 * drives it again and checks every frame a checksum that includes WorldTimer
 * (0x5FA770); with a carry of ours the timer ticked 17 16 17 where the race
 * had ticked 16 17 17, and the game left the replay after a few frames. */
static double world_clock(void){
    return (double)(int32_t)MR32(A_WTF) * (double)MRF32(A_FRAME60) + (double)MRF32(A_WTF_REST);
}
void sub_0057a0e0_orig(CPU *c);
void sub_0057a0e0(CPU *c){
    float wdt = MRF32(A_WORLDDT);
    uint32_t w0 = MR32(A_WORLDTIMER);
    double x0 = world_clock();
    sub_0057a0e0_orig(c);
    if (wdt == 0.f) return;
    g_f.wdt += wdt; g_sim_ms += wdt * 1e3;
    if (g_fix & FX_TIMER){
        double u = floor(world_clock() * 4000.0 + 0.5) - floor(x0 * 4000.0 + 0.5);
        MR32(A_WORLDTIMER) = w0 + (uint32_t)(int32_t)u;
        MRF32(A_WORLD_S) = (float)((double)(int32_t)MR32(A_WORLDTIMER) * (double)MRF32(A_U4000));
    }
}

/* ---- F2: "Acid" particle update 0x492E10 --------------------------------
 * __thiscall(ecx = effect system, [esp+4] = step).  eDisplayFrame pushes a
 * constant 1/60 for the system at 0x835860 (0x5D2C0D) once per rendered
 * frame: smoke, sparks and flames age 2x at 120.  The step becomes the real
 * frame dt, but never more than the 1/60 of the original: at or below 60
 * fps (and in 30 fps NIS) the argument is untouched, bit for bit.  At fps
 * = 30 they would age at half speed: there the step is the real dt up to
 * 1/30 (the emitters' countdowns take it as time too). */
void sub_00492e10_orig(CPU *c);
void sub_00492e10(CPU *c){
    if (MR32(c->esp) == 0x005d2c1cu && c->ecx == 0x00835860u){
        float k = MRF32(c->esp + 4u);
        if (g_fix & FX_PARTICLES){
            float dt = MRF32(A_REALDT), hi = at_30() ? 2.f * k : k;
            if (dt < k) MRF32(c->esp + 4u) = dt > 0.f ? dt : 0.f;
            else MRF32(c->esp + 4u) = dt < hi ? dt : hi;
        }
        g_f.part += MRF32(c->esp + 4u);
    }
    sub_00492e10_orig(c);
}

/* ---- F3: UpdateCarStreakingFlares 0x61AD80 --------------------------------
 * cdecl(eView), once per rendered frame (gated on [0x870818] at
 * [0x8A44EC]), it pushes every visible car's matrix and position into a
 * 3-entry ring (index car+0x2180, matrices +0x2090, positions +0x2150);
 * the renderer (0x61B094) draws the trail between the ring entries.  At
 * 120 fps the ring spans 2/120 s instead of 2/60: half the length.
 * Sampling at 60 Hz instead (upstream WFP) halves the length error but the
 * trail head then lags the car on every other frame.  Here the game still
 * samples every frame (the head stays on the car) and the two older
 * entries are replaced by this car's samples at least 1/60 s and 2/60 s
 * older (x NFSU2_NOS_TRAIL_LEN).  At 30 and 60 fps those are exactly the
 * game's own entries. */
void sub_0061ad80_orig(CPU *c);
#define TR_CARS 16
#define TR_HIST 24
#define TR_GAP  0.1                 /* s without a sample: the history is stale */
typedef struct { uint32_t ring; int n, head; double t[TR_HIST];
                 uint8_t m[TR_HIST][64]; uint8_t p[TR_HIST][16]; } TrailHist;
static TrailHist g_tr[TR_CARS];
static double    g_tr_time = 0;
static TrailHist *trail_of(uint32_t ring){
    int free_i = -1;
    for (int i = 0; i < TR_CARS; i++){
        if (g_tr[i].ring == ring) return &g_tr[i];
        if (!g_tr[i].ring && free_i < 0) free_i = i;
    }
    if (free_i < 0){ free_i = (int)(ring >> 4) % TR_CARS; }
    memset(&g_tr[free_i], 0, sizeof g_tr[free_i]);
    g_tr[free_i].ring = ring;
    return &g_tr[free_i];
}
/* newest history entry at least `age` seconds older than time t */
static int trail_pick(const TrailHist *h, double t){
    for (int k = 0; k < h->n; k++){
        int i = (h->head - k + TR_HIST) % TR_HIST;
        if (h->t[i] <= t + 1e-4) return i;
    }
    return -1;
}
void sub_0061ad80(CPU *c){
    static int on = 0;
    if (!(g_fix & FX_TRAIL)){ on = 0; sub_0061ad80_orig(c); return; }
    if (!on){ on = 1; memset(g_tr, 0, sizeof g_tr); }      /* (re)enabled: no history from before */
    if (MR32(A_FRAMECNT) == MR32(A_STREAK_FC)){ sub_0061ad80_orig(c); return; }
    /* first call this rendered frame: the trail clock advances by the real
     * frame dt (split screen calls once per eView: only the first counts) */
    float dt = MRF32(A_REALDT);
    g_tr_time += dt > 0.f && dt < 0.25f ? dt : 0.0;
    uint32_t rings[TR_CARS]; int idx0[TR_CARS], nc = 0;
    uint32_t head = MR32(A_CARLIST);
    if (head && mem_is_committed(head + 3)){
        for (uint32_t e = MR32(head); e && e != head && nc < TR_CARS && mem_is_committed(e + 0x557); e = MR32(e)){
            uint32_t r = MR32(e + 0x554);
            if (!r || !mem_is_committed(r + 0x2183)) continue;
            rings[nc] = r; idx0[nc] = (int)MR32(r + 0x2180); nc++;
        }
    }
    sub_0061ad80_orig(c);
    double back = g_trail_len / 60.0;
    for (int i = 0; i < nc; i++){
        uint32_t r = rings[i];
        int idx = (int)MR32(r + 0x2180);
        if (idx == idx0[i] || idx < 0 || idx > 2) continue;      /* not sampled this frame */
        g_f.trail++;
        TrailHist *h = trail_of(r);
        /* A car that was not sampled for a while (culled, off screen) has a
         * history that ends where it was then, hundreds of metres away in
         * free roam traffic: start afresh, so the game's own entries are used
         * until 1/60 s of new samples exists. */
        if (h->n && g_tr_time - h->t[h->head] > TR_GAP) h->n = 0;
        h->head = (h->head + 1) % TR_HIST; if (h->n < TR_HIST) h->n++;
        h->t[h->head] = g_tr_time;
        memcpy(h->m[h->head], g_mem + r + 0x2090 + (uint32_t)idx * 64u, 64);
        memcpy(h->p[h->head], g_mem + r + 0x2150 + (uint32_t)idx * 16u, 16);
        int prev = (idx + 2) % 3, old = (idx + 1) % 3;
        int a = trail_pick(h, g_tr_time - back);
        if (a < 0) continue;
        int b = trail_pick(h, h->t[a] - back);
        memcpy(g_mem + r + 0x2090 + (uint32_t)prev * 64u, h->m[a], 64);
        memcpy(g_mem + r + 0x2150 + (uint32_t)prev * 16u, h->p[a], 16);
        if (b >= 0){
            memcpy(g_mem + r + 0x2090 + (uint32_t)old * 64u, h->m[b], 64);
            memcpy(g_mem + r + 0x2150 + (uint32_t)old * 16u, h->p[b], 16);
        }
    }
}

/* ---- F4: EAXSound::Update 0x483B40 ----------------------------------------
 * __thiscall(ecx = audio manager, [esp+4] = real dt), ret 4.  It returns
 * unless more than 15 ms ([0x787AF4], shared constant) of QPC time passed
 * since [this+0xA4], then hands its single frame dt to the 9 slot objects:
 * engine pitch and gear-shift sound logic at 0.5x at 120 fps (WFP #1180).
 * The dt of gated calls is carried into the next real update (dstudnicki
 * NFSU2HighFPSAudioFix scheme, at most 0.25 s), with vanilla pass-through
 * when sound is off or paused, for dt = 0 calls (0x5CCDDE) and a carry
 * reset when the manager object changes (review B.6). */
void sub_00483b40_orig(CPU *c);
static uint32_t g_au_this = 0;
static double   g_au_carry = 0;
void sub_00483b40(CPU *c){
    uint32_t self = c->ecx;
    float dt = MRF32(c->esp + 4u);
    if (self != g_au_this){ g_au_this = self; g_au_carry = 0; }
    int ok = self && mem_is_committed(self + 0xa7) && MR32(A_SND_ON) && !MR32(A_SND_PAUSE);
    if (!ok || dt <= 0.f){ if (dt > 0.f) g_au_carry = 0; sub_00483b40_orig(c); return; }
    int active = !MR8(self + 0x13);
    uint32_t stamp = MR32(self + 0xa4);
    double d2 = dt;
    if ((g_fix & FX_AUDIO) && active){
        d2 = (double)dt + g_au_carry; if (d2 > 0.25) d2 = 0.25;
        MRF32(c->esp + 4u) = (float)d2;
    }
    sub_00483b40_orig(c);
    if (MR32(self + 0xa4) != stamp){ g_f.au_upd++; g_f.au_dt += active ? d2 : 0.0; g_au_carry = 0; }
    else g_au_carry = (g_fix & FX_AUDIO) && active ? d2 : 0.0;
}

/* ---- F6: Rain::Update 0x61F120 --------------------------------------------
 * __thiscall(ecx = the Rain at eView+0x64), once per frame in the world;
 * overrides.c (FORCERAIN) calls it through hfr_rain_update.  It returns at
 * once unless |world dt| > 9 ms ([0x7A8734], read only here), which is meant
 * for a world that stands but also skips every 1/120 s frame.  At 120 the
 * rain then ran only on frames that missed their slot, and only those
 * frames armed its screen tint (0x5D75E0, the main view's lighting): in rain
 * the picture jumped between tinted frames and ~20 % brighter ones, and the
 * grey storm sky flashed.  Here a frame shorter than 1/60 s (s = 60 dt < 1)
 * runs it too, with its per-call steps scaled to world time as at 60 fps
 * (longer frames run it untouched, as F2):
 *   a rain drop moves by its velocity (one of ten at Rain+0x5120) plus a
 *   per-type share of the wind (Rain+0x4D4C..+0x4D68) a call (0x61EE8B,
 *   both read only there, written at init and by the tweak 0x61E3E0): x s;
 *   it also moves by -0.02 x the camera's velocity (camera +0x1E0, m/s),
 *   flattened and capped at length 1 a call, i.e. 50 m/s (0x61E8B9): for
 *   the call the rain sees that velocity capped at 50 m/s and x s, so the
 *   game's cap never acts and the share is s x the 60 fps one.  Its length
 *   (Rain+0x5090, written only there) lengthens every streak (0x613A70) and
 *   is divided by s again afterwards;
 *   lens drops slide out by [0x803AC4] x the camera speed a call (0x614141,
 *   read only there): the constant makes up for the cap, the s is in the
 *   velocity already;
 *   the cloud cover eases 5 % / 0.15 % a call towards its target (0x61F2CC,
 *   [0x7A67C0]/[0x7A67C4], read only here): 1 - (1 - f)^s.
 * A streak's direction comes from the last step, which the scaling keeps.
 * All of it is put back after the call (a drop value only if the call did
 * not write it again).
 * At fps = 30 (s = 2) the rain would fall at half speed, and the camera's
 * share cannot be scaled up past the game's cap: the update runs twice
 * instead, each call a 60 fps frame with half the world dt (its own copy at
 * 0x61F134 is the gate's and steps a timer at [0x8A1D50] and the easing of
 * Rain+0x1F0). */
void sub_0061f120_orig(CPU *c);
#define A_RAIN_GATE   0x007a8734u   /* 0.009 s */
#define A_RAIN_EASE   0x007a67c0u   /* cloud easing a call: towards clear, +4 towards overcast */
#define A_RAIN_SLIDE  0x00803ac4u   /* lens drop slide per camera speed */
#define RAIN_WIND     0x4d4cu       /* Rain: 4 drop types x {wind share, half of it} */
#define RAIN_FALL     0x5120u       /* Rain: 10 drop velocities {x, y, z, -99999.9} */
#define RAIN_STRETCH  0x5090u       /* Rain: streak length added for the camera speed */
#define RAIN_VMAX     50.0          /* m/s: the game's cap, 1 / 0.02 */
void hfr_rain_update(CPU *c){
    uint32_t rain = c->ecx;
    double s = 60.0 * MRF32(A_WORLDDT);
    uint32_t view = rain && mem_is_committed(rain + RAIN_FALL + 0x9fu) ? MR32(rain + 0x1ecu) : 0;
    uint32_t cam = view && mem_is_committed(view + 0x43u) ? MR32(view + 0x40u) : 0;
    if ((g_fix & FX_RAIN) && s > 1.5 && at_30()){
        float wdt = MRF32(A_WORLDDT);
        uint32_t ret = MR32(c->esp);                         /* popped by the call: pushed again for the second */
        MRF32(A_WORLDDT) = wdt * 0.5f;
        sub_0061f120_orig(c);
        c->esp -= 4u; MR32(c->esp) = ret; c->ecx = rain;
        sub_0061f120_orig(c);
        MRF32(A_WORLDDT) = wdt;
        return;
    }
    if (!(g_fix & FX_RAIN) || !(s > 0.0 && s < 1.0) || !cam || !mem_is_committed(cam + 0x1ebu)){
        sub_0061f120_orig(c); return; }
    float gate = MRF32(A_RAIN_GATE), up = MRF32(A_RAIN_EASE), down = MRF32(A_RAIN_EASE + 4u);
    float slide = MRF32(A_RAIN_SLIDE), stretch = MRF32(rain + RAIN_STRETCH), old[38], v[3];
    uint32_t at[38], put[38];
    for (uint32_t i = 0; i < 8; i++) at[i] = rain + RAIN_WIND + 4u * i;
    for (uint32_t i = 0; i < 30; i++) at[8 + i] = rain + RAIN_FALL + 16u * (i / 3) + 4u * (i % 3);
    for (int i = 0; i < 38; i++){ old[i] = MRF32(at[i]); MRF32(at[i]) = (float)(old[i] * s); put[i] = MR32(at[i]); }
    double vl = 0;
    for (uint32_t i = 0; i < 3; i++){ v[i] = MRF32(cam + 0x1e0u + 4u * i); vl += (double)v[i] * v[i]; }
    vl = sqrt(vl);
    double cap = vl > RAIN_VMAX ? RAIN_VMAX / vl : 1.0;
    for (uint32_t i = 0; i < 3; i++) MRF32(cam + 0x1e0u + 4u * i) = (float)(v[i] * cap * s);
    MRF32(A_RAIN_SLIDE) = (float)(slide / cap);
    MRF32(rain + RAIN_STRETCH) = -1.f;                       /* not computed this call: stays -1 */
    MRF32(A_RAIN_GATE) = 0.f;
    MRF32(A_RAIN_EASE) = (float)(1.0 - pow(1.0 - up, s));
    MRF32(A_RAIN_EASE + 4u) = (float)(1.0 - pow(1.0 - down, s));
    sub_0061f120_orig(c);
    for (int i = 0; i < 38; i++) if (MR32(at[i]) == put[i]) MRF32(at[i]) = old[i];
    for (uint32_t i = 0; i < 3; i++) MRF32(cam + 0x1e0u + 4u * i) = v[i];
    float st = MRF32(rain + RAIN_STRETCH);
    MRF32(rain + RAIN_STRETCH) = st < 0.f ? stretch : (float)fmin(st / s, 1.0);
    MRF32(A_RAIN_GATE) = gate; MRF32(A_RAIN_EASE) = up; MRF32(A_RAIN_EASE + 4u) = down;
    MRF32(A_RAIN_SLIDE) = slide;
}

/* ---- F7: the front end's spinners, 0x630DE0 ------------------------------
 * The front end's car renderer (garage, shops, car lot: GameFlow not 6)
 * turns its spinners by a constant 0.5 degree a call ([0x7A667C], read only
 * there; the angle [0x8A44F8]), unless the front end is paused (0x510C30).
 * It runs twice a rendered frame: 60 degrees a second at 60 fps, twice that
 * at 120, four times at 240 (a player's report at 240: "way too fast").
 * The world's spinners turn by time (0x5ECAC0).  After each FrameTimeUpdate
 * the step becomes 0.5 x 60 x the frame's real dt: 60 degrees a second at
 * every rate, the game's own 0.5 at 60 (tests/scripts/fe_spinners.txt). */
#define A_FE_SPIN     0x007a667cu   /* 0.5: degrees a front-end spinner turns a frame */
static void fe_spinners(void){
    static float van = -1.f;
    if (van < 0.f) van = MRF32(A_FE_SPIN);
    float dt = MRF32(A_REALDT);
    MRF32(A_FE_SPIN) = (g_fix & FX_SPIN) && dt > 0.f ? (float)(van * 60.0 * dt) : van;
}

/* ---- exit report -------------------------------------------------------- */
void hfr_report(void){
    if (!g_resolved) return;
    if (g_sec.frames) stat_add(&g_tot, &g_sec), memset(&g_sec, 0, sizeof g_sec);
    char fs[96]; fix_str(g_fix, fs, sizeof fs);
    fprintf(stderr, "\n=== high frame rate (framerate.c) ===\n");
    fprintf(stderr, "  mode %s, display %d Hz, content rate %d, fixes %s\n",
            mode_name(), display_hz(), mode_rate(), fs);
    if (g_tot.frames) stat_line(g_log == 2 ? "all" : "race", &g_tot);
    { uint64_t n = 0, acc = 0; for (int i = 0; i < WH_BINS; i++) n += g_wh[i];
      if (n){ int b[3] = {-1, -1, -1}; const uint64_t want[3] = {n / 2, n * 95 / 100, n * 99 / 100};
          for (int i = 0; i < WH_BINS; i++){ acc += g_wh[i];
              for (int k = 0; k < 3; k++) if (b[k] < 0 && acc >= want[k]) b[k] = i; }
          char v[3][24];
          for (int k = 0; k < 3; k++)
              if (b[k] == WH_BINS - 1) snprintf(v[k], sizeof v[k], "> %.2f", (WH_BINS - 1) * 0.25);
              else snprintf(v[k], sizeof v[k], "<= %.2f", (b[k] + 1) * 0.25);
          fprintf(stderr, "  guest work per race frame (pacer to pacer minus its sleep; includes the wait for the presenter): "
                  "p50 %s ms, p95 %s ms, p99 %s ms\n", v[0], v[1], v[2]); } }
    { uint64_t pv = atomic_load(&g_vs_period);
      fprintf(stderr, "  vsync clock: %s, measured refresh %.4f Hz; pacer locked on %.1f %% of frames\n",
              atomic_load(&g_vs_ts) ? "running" : "none", pv ? 1e9 / pv : 0.0,
              g_lock_n ? 100.0 * g_lock_ok / g_lock_n : 0.0); }
    iv_report("presented (presentedTime)", &g_ph);
    iv_report("present calls (main thread)", &g_pc);
    if (g_trace) fflush(g_trace);
}
