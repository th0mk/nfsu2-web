/* watchdog.c -- stall watchdog (v1.2 NA port, long-session track).
 *
 * After codepdbh/nfsu2-recompiled's idea: if the guest has not presented a
 * frame for a while, log once what every guest thread is doing, so a hang
 * in a long career run can be diagnosed from the log alone.
 *
 *   NFSU2_WATCHDOG=<s>   seconds without a Present before the report
 *                        (default 15; 0 turns the watchdog off).  While the
 *                        game is loading (its flow state, game.h, neither
 *                        the front end nor in game) the limit is at
 *                        least 20 s.  It used to be 60 s, and players stuck
 *                        on a loading screen reloaded before the report
 *                        came; a first load that waits on the network for
 *                        its data may now get a report it did not need
 *                        (only log lines).
 *
 * The report gives, per guest thread: its state (running, waiting for the
 * GIL, waiting on a kernel object, in a contended critical section,
 * sleeping) and since when, the guest call chain read from its stack, the
 * GIL holder, and the critical sections held with owner and recursion.  It
 * is printed once per stall; the next Present re-arms it.  The watchdog
 * only reads; it never changes what the game does.  A game that names its
 * threads (game.h thread_names: Most Wanted's) also gets each thread's name
 * and routine, and its eip as a lifted function and offset (L-073); UG2's
 * table names none, so its report keeps the lines it always had.
 *
 * The web page's Download log asks too (watchdog_log_download): players
 * download a frozen game's log before the limit (2026-10-24: 13 s after the
 * last frame, no report), so the log gets how long ago the last frame was
 * and, after 3 s or more, the report then. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE                    /* glibc's pthread_setname_np (native tests) */
#endif
#include "cpu.h"
#include "game.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define MAXCPUS 32                               /* win32_ext.c */
enum { TS_RUN = 0, TS_GIL, TS_WAIT, TS_WAITM, TS_CS, TS_SLEEP, TS_IDLE };
typedef struct { volatile int what; volatile uint32_t obj; volatile uint64_t since_us; } TState;
extern TState   g_tstate[MAXCPUS];
extern CPU     *g_all_cpus[MAXCPUS];
extern uint32_t g_all_tids[MAXCPUS];
extern uint32_t g_all_start[MAXCPUS];
extern int      g_all_n;
extern CPU     *g_cur_cpu;
void guest_backtrace(CPU *, int);
uint32_t guest_nearest_fn(uint32_t a);                   /* dispatch.c */
uint32_t module_nearest_fn(uint32_t a, const char **mod); /* module.c */
int  win32_cs_held(uint32_t *key, uint32_t *owner, int *depth, int max);
int  win32_obj_describe(uint32_t h, char *buf, size_t n);

static _Atomic uint64_t g_wd_present_us = 0;
static _Atomic long     g_wd_frames = 0;
static uint64_t g_wd_reports = 0;

static uint64_t wd_now_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
/* Seconds from a stamp another thread took to now.  In the browser every
 * thread is a worker with its own performance.timeOrigin + now() (and the
 * guest threads' since_us come from nfsu2_now_ms, rounded another way): a
 * stamp taken a moment ago can read a few microseconds ahead of this
 * thread's now.  Unsigned, that was 2^64 us ("no Present for
 * 18446744073709.6 s" once in ~60 runs); a stamp ahead counts as no time. */
static double wd_since_s(uint64_t now, uint64_t then){
    return now > then ? (double)(now - then) / 1e6 : 0.0;
}
/* com.c Present (guest thread). */
void watchdog_on_present(void){
    atomic_store(&g_wd_present_us, wd_now_us());
    atomic_fetch_add(&g_wd_frames, 1);
}

/* a guest address as the lifted function it is in, " (sub_007ec601+0x14)"
 * (" (server.dll sub_00d0a000+0x8)" in a lifted DLL), or "" */
static void wd_symbol(uint32_t a, char *b, size_t n){
    const char *mn = 0; uint32_t f = guest_nearest_fn(a);
    b[0] = 0;
    if (f) snprintf(b, n, " (sub_%08x+0x%x)", f, a - f);
    else if (a && (f = module_nearest_fn(a, &mn)) != 0) snprintf(b, n, " (%s sub_%08x+0x%x)", mn ? mn : "?", f, a - f);
}
/* " timer (0x007ec601)": the name and routine of the thread in slot i (game.h thread_names) */
static void wd_name(int i, char *b, size_t n){
    uint32_t r = g_all_start[i];
    const char *nm = r ? game_thread_name(r) : 0;
    if (nm) snprintf(b, n, " %s (0x%08x)", nm, r);
    else if (r) snprintf(b, n, " (0x%08x)", r);
    else snprintf(b, n, " (routine not known)");
}

static pthread_mutex_t g_wd_mx = PTHREAD_MUTEX_INITIALIZER;   /* the watchdog's thread and the page's input thread */
static void wd_report(double stalled, uint32_t flow){
    pthread_mutex_lock(&g_wd_mx);
    g_wd_reports++;
    uint64_t now = wd_now_us();
    fprintf(stderr, "\n[watchdog] no Present for %.1f s (game flow %u, %ld frames so far); guest threads:\n",
            stalled, flow, atomic_load(&g_wd_frames));
    static const char *const what[] = { "running", "waiting for the GIL", "waiting on", "waiting on (multiple) first",
                                        "in contended critical section", "sleeping", "outside guest code (host side)" };
    for (int i = 0; i < g_all_n && i < MAXCPUS; i++){
        CPU *c = g_all_cpus[i];
        if (!c) continue;
        int w = g_tstate[i].what; uint32_t o = g_tstate[i].obj;
        double for_s = g_tstate[i].since_us ? wd_since_s(now, g_tstate[i].since_us) : 0.0;
        char desc[96] = "";
        if (w == TS_WAIT || w == TS_WAITM) win32_obj_describe(o, desc, sizeof desc);
        else if (w == TS_CS) snprintf(desc, sizeof desc, "0x%08x", o);
        else if (w == TS_SLEEP) snprintf(desc, sizeof desc, "%u ms", o);
        char nm[64] = "", at[64] = "";
        if (g_game.thread_names){ wd_name(i, nm, sizeof nm); wd_symbol(c->eip, at, sizeof at); }
        fprintf(stderr, "  thread %d tid=%u%s%s: %s %s for %.1f s, esp=0x%08x eip=0x%08x%s\n",
                i, g_all_tids[i], nm, c == g_cur_cpu ? " (holds the GIL)" : "",
                (w >= 0 && w <= TS_IDLE) ? what[w] : "?", desc, for_s, c->esp, c->eip, at);
        guest_backtrace(c, 12);
    }
    uint32_t key[32], owner[32]; int depth[32];
    int n = win32_cs_held(key, owner, depth, 32);
    if (!n) fprintf(stderr, "  critical sections held: none\n");
    for (int k = 0; k < n; k++)
        fprintf(stderr, "  critical section 0x%08x held by tid %u (depth %d)\n", key[k], owner[k], depth[k]);
    fprintf(stderr, "[watchdog] end of report\n\n");
    pthread_mutex_unlock(&g_wd_mx);
}

/* lifecycle.c (app track) defines the real one: true while the game has been
 * told it is inactive and parks in GetMessageA (no Present, on purpose, as
 * after an alt-tab on Windows).  During a LAN session an inactive game is
 * not parked (lifecycle_may_park): it keeps presenting, so the limit applies
 * as in the foreground.  Without lifecycle.c the game is never parked. */
__attribute__((weak)) int lifecycle_game_parked(void){ return 0; }

/* the page's Download log (runtime/web/input_web.c, on its input thread,
 * which runs while the game's threads are stuck): how long ago the last
 * frame was, the report after 3 s or more, then the line the page waits for
 * before it makes the file.  Also with NFSU2_WATCHDOG=0. */
void watchdog_log_download(void){
    uint64_t last = atomic_load(&g_wd_present_us);
    uint32_t flow = game_flow();
    if (!last) fprintf(stderr, "[watchdog] log downloaded before the first frame (game flow %u)\n", flow);
    else {
        double ago = wd_since_s(wd_now_us(), last);
        int parked = lifecycle_game_parked();
        fprintf(stderr, "[watchdog] log downloaded: the last frame %.1f s ago (game flow %u, %ld frames so far%s)\n",
                ago, flow, atomic_load(&g_wd_frames), parked ? ", parked in the background" : "");
        if (ago >= 3.0 && !parked) wd_report(ago, flow);
    }
    fprintf(stderr, "[watchdog] log download ready\n");
}

static void *wd_main(void *arg){
    double lim = *(double *)arg;
#ifdef __linux__
    pthread_setname_np(pthread_self(), "watchdog");
#else
    pthread_setname_np("watchdog");    /* macOS's, and the web build's (wasm_plat.h) */
#endif
    int reported = 0;
    for (;;){
        sleep(1);
        uint64_t last = atomic_load(&g_wd_present_us);
        if (!last) continue;                     /* not armed before the first frame */
        if (lifecycle_game_parked()){            /* parked in the background: not a stall; */
            atomic_store(&g_wd_present_us, wd_now_us());   /* the limit restarts on return */
            reported = 0; continue; }
        double stalled = wd_since_s(wd_now_us(), last);
        uint32_t flow = game_flow();
        double l = (flow == g_game.flow_fe || flow == g_game.flow_race) ? lim : (lim > 20.0 ? lim : 20.0);
        if (stalled < l){ reported = 0; continue; }
        if (!reported){ reported = 1; wd_report(stalled, flow); }
    }
    return 0;
}

void watchdog_start(void){
    const char *e = getenv("NFSU2_WATCHDOG");
    static double lim;
    lim = (e && *e) ? atof(e) : 15.0;
    if (lim <= 0.0) return;
    pthread_t th;
    if (pthread_create(&th, 0, wd_main, &lim) == 0) pthread_detach(th);
}
uint64_t watchdog_reports(void){ return g_wd_reports; }
