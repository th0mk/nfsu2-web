/* Win32 layer - second round: registry, window, synchronization, threads */
#include <strings.h>
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#include <sys/event.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mount.h>
#include <stddef.h>

void dispatch_indirect(CPU*,uint32_t);
void mem_commit(uint32_t,uint32_t);
uint32_t win32_alloc(uint32_t n, int zero);
uint32_t res_find(uint32_t,uint32_t,int);
uint32_t res_data(uint32_t);
uint32_t res_size(uint32_t);

/* ---- object table (HANDLE -> kind/state) ---- */
#define H_BASE MM_H_BASE                  /* the game's object handle values (memmap.h) */
typedef enum { OB_NONE=0, OB_EVENT, OB_MUTEX, OB_THREAD, OB_FILE, OB_KEY, OB_SEM, OB_TIMER } ObKind;
typedef struct {
    ObKind kind; int signaled; int manual;
    pthread_mutex_t mtx; pthread_cond_t cv;
    uint32_t guest_arg, guest_entry, tid;
    uint32_t owner_tid; int rec;      /* MUTEX: ownership + recursion */
    pthread_t th; int started; void *pending;
    char name[64];
    int refs;                         /* v12na: open handle count (DuplicateHandle) */
    int prim_ok;                      /* mtx/cv set up once (kept when the slot is reused) */
    /* OB_THREAD: how it ended (ExitThread argument or the entry's eax), and a
     * TerminateThread request it honours at its next import call */
    uint32_t exit_code; int ended; volatile int term_req;
    int closed;                       /* OB_THREAD: last handle closed while it still ran */
    uint32_t teb, stack_lo;           /* OB_THREAD: recycled when the thread ends */
    /* OB_TIMER (waitable timers, below): armed or not, the next due time on
     * the virtual clock (host_us) and the period, 0 = one-shot */
    int t_armed; uint64_t t_due_us, t_period_us;
    /* OB_THREAD: who started it (CreateThread's return address), its place in
     * the order threads ended, and how often an import looked its handle up
     * (waits, CloseHandle, GetExitCodeThread, DuplicateHandle, ...): obj_reclaim */
    uint32_t creator; uint64_t end_seq; uint32_t looked;
    uint32_t routine;                 /* OB_THREAD: the routine it runs (game.h thread_start), for the watchdog */
    uint32_t gen;                     /* handle generation (obj_reclaim); kept when the slot is reused */
    uint32_t serial;                  /* which object the slot holds (obj_new counts them): WSAWaitForMultipleEvents
                                       * and a WaitForMultipleObjects with a waitable timer (wait_set) notice an
                                       * object closed during the wait even when its slot already holds the next,
                                       * and an overlapped receive sets only the event it began with;
                                       * WaitForSingleObject and the other WaitForMultipleObjects do not look at it */
    /* OB_KEY with the game's key table (game.h reg_keys): which key (its index
     * + 1; 0 = the flat registry) and the access it was opened with */
    int reg_key; uint32_t reg_access;
    int sem_max;                      /* OB_SEM: its maximum count (its count is signaled) */
} Obj;
/* v12na: 4096 (was 512).  The game never closes the handle of the thread it
 * starts for every race load, so one slot per load stays in use (measured:
 * +1 per load, heap-07); 512 would have run out after ~500 loads.  Since
 * then a full table takes such a thread's slot back (obj_reclaim). */
#define OBJ_MAX MM_H_MAX
static Obj g_obj[OBJ_MAX];
static int g_obj_n = 1;
/* A handle is H_BASE + gen * OBJ_MAX + slot.  gen stays 0 until obj_reclaim
 * gives a slot to a new object: the reclaimed thread's handle then names
 * nothing (a late CloseHandle or wait on it is that of a closed handle and
 * cannot reach the slot's new object).  The game's handle range holds
 * MM_H_GENS generations (memmap.h): handles stay below 0x00B00000. */
#define OBJ_GENS MM_H_GENS
static int obj_slot(uint32_t h){                /* -1: not ours, or an older generation's */
    uint32_t d = h - H_BASE;                     /* below H_BASE wraps past the range */
    if (d >= OBJ_GENS * OBJ_MAX) return -1;
    uint32_t i = d % OBJ_MAX;
    return d / OBJ_MAX == g_obj[i].gen ? (int)i : -1;
}
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t waits, sets; uint32_t waiter_tid, setter_tid; } WStat;
static WStat g_wstat[64];

/* ---- BIG GUEST LOCK ----------------------------------------------------
 * Only one thread runs guest code at a time.  That keeps the guest memory and
 * allocators shared by the lifted code safe, and keeps the instruction trace
 * consistent.  The lock is released only in blocking calls (Sleep/Wait*). */
#ifndef __wasm__
static pthread_mutex_t g_guest = PTHREAD_MUTEX_INITIALIZER;
#define gil_acquire(who) pthread_mutex_lock(&g_guest)
#define gil_acquire_frame(who) pthread_mutex_lock(&g_guest)
#define gil_keep_for_mutex() 0
#define gil_release()    pthread_mutex_unlock(&g_guest)
#endif
extern CPU *g_cur_cpu;
static _Thread_local CPU *tl_cpu = 0;
static _Thread_local void *tl_obj = 0;   /* this thread's object */
/* CPU state of every guest thread: so that on a hang the stack traces of ALL
 * of them can be dumped (a single thread's can be misleading). */
#define MAXCPUS 32
CPU *g_all_cpus[MAXCPUS];
uint32_t g_all_tids[MAXCPUS];
uint32_t g_all_start[MAXCPUS];       /* the routine each runs (the main thread: the entry point), watchdog.c */
int g_all_n = 0;
/* Stall watchdog (watchdog.c): what each guest thread is doing right now. */
enum { TS_RUN = 0, TS_GIL, TS_WAIT, TS_WAITM, TS_CS, TS_SLEEP, TS_IDLE };
typedef struct { volatile int what; volatile uint32_t obj; volatile uint64_t since_us; } TState;
TState g_tstate[MAXCPUS];
static _Thread_local int tl_slot = -1;
static uint64_t gil_now_us(void);
static void ts_set(int what, uint32_t obj){
    if (tl_slot < 0) return;
    g_tstate[tl_slot].what = what; g_tstate[tl_slot].obj = obj; g_tstate[tl_slot].since_us = gil_now_us();
}
/* what the thread registered in slot i runs: a CreateThread's routine; the first one registered is the
 * main thread (main_web.c, the entry point); another without a thread object (the WINMM timer) has none */
static uint32_t slot_routine(int i){ return tl_obj ? ((Obj *)tl_obj)->routine : i == 0 ? g_game.entry_va : 0u; }
static void reg_cpu(CPU *c){
    for (int i=0;i<g_all_n;i++) if (g_all_cpus[i]==c){ tl_slot = i; return; }
    for (int i=0;i<g_all_n;i++) if (!g_all_cpus[i]){             /* a slot an ended thread left */
        g_all_cpus[i] = c; g_all_tids[i] = c->fs_base ? MR32(c->fs_base+0x24) : 0;
        g_all_start[i] = slot_routine(i); tl_slot = i; return; }
    if (g_all_n < MAXCPUS){
        g_all_cpus[g_all_n] = c;
        g_all_tids[g_all_n] = c->fs_base ? MR32(c->fs_base+0x24) : 0;
        g_all_start[g_all_n] = slot_routine(g_all_n);
        tl_slot = g_all_n;
        g_all_n++;
    }
}
/* A guest thread ends: its CPU lived on its host stack. */
static void unreg_cpu(CPU *c){
    for (int i=0;i<g_all_n;i++) if (g_all_cpus[i]==c){ g_all_cpus[i] = 0; g_all_tids[i] = 0; g_tstate[i].what = TS_RUN; }
    tl_slot = -1;
}
/* --- measuring how long the big lock (GIL) is HELD ---------------------
 * A guest thread does not release the GIL until it makes a Win32 call; a
 * long computation (e.g. decoding an audio stream) starves the other threads.
 * The audio writes ~50 ms of silence every 30 s -- this measures whether
 * that is the culprit.  GILLOG=<ms> reports every hold above the threshold. */
#ifdef __wasm__
#include "web/host_abi.h"
#endif
static uint64_t gil_now_us(void){
#ifdef __wasm__
    /* the page's monotonic clock without WASI's clock_time_get (its i64 argument
     * costs a BigInt per call into JS): this runs on every guest lock hand-off */
    return (uint64_t)(nfsu2_now_ms() * 1000.0);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
#endif
}
static _Thread_local uint64_t tl_gil_t0 = 0;
uint64_t g_gil_max_us = 0, g_gil_long = 0;
static int gil_thr_us(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("GILLOG"); v = (e && *e) ? atoi(e)*1000 : 0; }
    return v;
}
static void gil_took(void){ tl_gil_t0 = gil_now_us(); }
#ifdef __wasm__
static double gil_host_wait_ms(void);   /* of the current hold: file I/O round trips, sleeps */
#endif
static void gil_gave(void){
    if (!tl_gil_t0) return;
    uint64_t d = gil_now_us() - tl_gil_t0;
    tl_gil_t0 = 0;
    if (d > g_gil_max_us) g_gil_max_us = d;
    int thr = gil_thr_us();
    if (thr && d > (uint64_t)thr){
        g_gil_long++;
        static int shown = 0;
        if (shown < 80){ shown++;
            double audio_time(void);
#ifdef __wasm__
            fprintf(stderr, "  [GIL] t=%.3f s  lock held %.1f ms  guest eip=%08x tid %u (host waits %.1f ms)\n",
                    audio_time(), d/1000.0, tl_cpu ? tl_cpu->eip : 0u,
                    (tl_cpu && tl_cpu->fs_base) ? MR32(tl_cpu->fs_base + 0x24u) : 0u, gil_host_wait_ms());
#else
            fprintf(stderr, "  [GIL] t=%.3f s  lock held %.1f ms  guest eip=%08x\n",
                    audio_time(), d/1000.0, tl_cpu ? tl_cpu->eip : 0u);
#endif
        }
    }
}
#ifdef __wasm__
/* wasm: the guest lock is a FIFO ticket lock (web/ticketlock.h): browsers
 * schedule workers with no fairness promise, and an unordered futex mutex let
 * the main thread re-take the lock ahead of the DirectSound feeder, the
 * loader and the timer.  Every acquisition that had to queue is measured:
 * how long the thread waited (holder's time included) and the hand-off
 * latency (the previous owner's unlock to this thread running: the cost of
 * a wasm atomic wait/notify between two workers).  Reported at exit
 * (win32 report) and per [status] line. */
#include "web/ticketlock.h"
#include "web/host_abi.h"
static ticketlock g_gil = TICKETLOCK_INIT;
/* NFSU2_GIL=mutex (measurement only): the old unordered pthread mutex, with
 * the same statistics, to compare against the ticket lock */
static pthread_mutex_t g_gil_mx = PTHREAD_MUTEX_INITIALIZER;
static int g_gil_use_mx = -1;
typedef struct { uint32_t tid; uint64_t n, queued; double wait_ms, wait_max_ms, hand_ms, hand_max_ms;
                 uint32_t whist[24], hhist[24]; } GilStat;   /* hist: [2^k, 2^(k+1)) us, k=0 below 2 us */
static GilStat g_gs[64]; static int g_gs_n;
static GilStat g_gs_tot;                         /* all threads */
static GilStat g_gs_win;                         /* since the last [status] line */
static _Thread_local GilStat *tl_gs; static _Thread_local uint32_t tl_gs_tid = ~0u;
static double g_gil_rel_ms;                      /* last unlock (written by the owner) */
/* Busy time (there is no thread CPU clock in the browser): the time a thread
 * holds the guest lock, minus what it spent blocked in the host meanwhile
 * (file I/O round trips, sleeps; web/src/host/game-worker.ts).  Guest code
 * runs only under the lock, and every guest wait (pacer, Sleep, Wait*, the
 * present wait) releases it, so this is the guest thread's CPU time. */
static _Thread_local double tl_acq_ms, tl_acq_blk, tl_busy_ms; static _Thread_local int tl_holding;
static _Thread_local int tl_barged;              /* taken ahead of the queue (gil_acquire_frame) */
static double g_busy_all_ms;                     /* every guest thread (updated by the owner) */
static double held_busy(double now){ return (now - tl_acq_ms) - (nfsu2_blocked_ms() - tl_acq_blk); }
static double gil_host_wait_ms(void){ return tl_holding ? nfsu2_blocked_ms() - tl_acq_blk : 0.0; }
double win32_thread_busy_ms(void){ return tl_busy_ms + (tl_holding ? held_busy(nfsu2_now_ms()) : 0.0); }
double win32_busy_all_ms(void){ return g_busy_all_ms + (tl_holding ? held_busy(nfsu2_now_ms()) : 0.0); }
uint64_t g_gil_fair_yields;                      /* fair yields that handed the lock over */
static int gil_bucket(double ms){ double us = ms * 1000.0; int k = 0; while (us >= 2.0 && k < 23){ us *= 0.5; k++; } return k; }
static void gil_note(GilStat *s, double w, double h){
    if (!s) return;
    s->queued++; s->wait_ms += w; if (w > s->wait_max_ms) s->wait_max_ms = w; s->whist[gil_bucket(w)]++;
    s->hand_ms += h; if (h > s->hand_max_ms) s->hand_max_ms = h; s->hhist[gil_bucket(h)]++;
}
static GilStat *gil_stat_for(CPU *who){                /* GIL held */
    uint32_t tid = (who && who->fs_base) ? MR32(who->fs_base + 0x24u) : 0u;
    if (tl_gs && tl_gs_tid == tid) return tl_gs;
    GilStat *s = 0;
    for (int i = 0; i < g_gs_n; i++) if (g_gs[i].tid == tid){ s = &g_gs[i]; break; }
    if (!s && g_gs_n < 64){ s = &g_gs[g_gs_n++]; s->tid = tid; }
    tl_gs = s; tl_gs_tid = tid;
    return s;
}
static void gil_acquire(CPU *who){
    if (g_gil_use_mx < 0){ const char *e = getenv("NFSU2_GIL"); g_gil_use_mx = e && !strcmp(e, "mutex"); }
    if (g_gil_use_mx){
        double t0 = 0;
        int queued = pthread_mutex_trylock(&g_gil_mx) != 0;
        if (queued){ t0 = nfsu2_now_ms(); pthread_mutex_lock(&g_gil_mx); }
        double t1 = nfsu2_now_ms(), h = t1 - g_gil_rel_ms;
        GilStat *s = gil_stat_for(who); if (s) s->n++; g_gs_tot.n++; g_gs_win.n++;
        if (queued){ if (h < 0) h = 0; gil_note(s, t1 - t0, h); gil_note(&g_gs_tot, t1 - t0, h); gil_note(&g_gs_win, t1 - t0, h); }
        tl_acq_ms = t1; tl_acq_blk = nfsu2_blocked_ms(); tl_holding = 1;
        return;
    }
    uint32_t me = ticket_take(&g_gil);
    if (ticket_try(&g_gil, me)){
        GilStat *s = gil_stat_for(who); if (s) s->n++; g_gs_tot.n++; g_gs_win.n++;
        tl_acq_ms = nfsu2_now_ms(); tl_acq_blk = nfsu2_blocked_ms(); tl_holding = 1;
        return; }
    double t0 = nfsu2_now_ms();
    ticket_wait(&g_gil, me);
    double t1 = nfsu2_now_ms(), h = t1 - g_gil_rel_ms;
    if (h < 0) h = 0;
    GilStat *s = gil_stat_for(who); if (s) s->n++; g_gs_tot.n++; g_gs_win.n++;
    gil_note(s, t1 - t0, h); gil_note(&g_gs_tot, t1 - t0, h); gil_note(&g_gs_win, t1 - t0, h);
    tl_acq_ms = t1; tl_acq_blk = nfsu2_blocked_ms(); tl_holding = 1;
}
/* The frame pacer's thread at its slot boundary (guest_sleep_frame_ns): it
 * takes a free lock ahead of the queue (ticketlock.h ticket_barge), and while
 * another thread holds it, it polls instead of sleeping, as it was spinning
 * on the clock anyway.  Before, it queued behind the threads that came during
 * its sleep, and one of them still waking up kept the lock idle for up to
 * 4.6 ms: a missed 120 Hz slot, ~4 times in a 35 s race (Ryzen 5900X,
 * Chrome 155).  Once per frame only, so the others still have the whole
 * pacer sleep; a holder still busy after 20 ms (a loading screen's long
 * call) is queued for as before. */
uint64_t g_gil_frame_takes, g_gil_frame_queued;
static void gil_acquire_frame(CPU *who){
    if (g_gil_use_mx < 0){ const char *e = getenv("NFSU2_GIL"); g_gil_use_mx = e && !strcmp(e, "mutex"); }
    if (g_gil_use_mx){ gil_acquire(who); return; }
    g_gil_frame_takes++;
    if (ticket_barge(&g_gil, 1)){
        GilStat *s = gil_stat_for(who); if (s) s->n++; g_gs_tot.n++; g_gs_win.n++;
        tl_acq_ms = nfsu2_now_ms(); tl_acq_blk = nfsu2_blocked_ms(); tl_holding = 1; tl_barged = 1;
        return; }
    double t0 = nfsu2_now_ms(), t1 = t0;
    while (t1 - t0 < 20.0){
        if (ticket_barge(&g_gil, 256)){
            t1 = nfsu2_now_ms();
            double h = t1 - g_gil_rel_ms; if (h < 0) h = 0;
            GilStat *s = gil_stat_for(who); if (s) s->n++; g_gs_tot.n++; g_gs_win.n++;
            gil_note(s, t1 - t0, h); gil_note(&g_gs_tot, t1 - t0, h); gil_note(&g_gs_win, t1 - t0, h);
            tl_acq_ms = t1; tl_acq_blk = nfsu2_blocked_ms(); tl_holding = 1; tl_barged = 1;
            return; }
        t1 = nfsu2_now_ms();
    }
    g_gil_frame_queued++;
    gil_acquire(who);
}
/* WaitForSingleObject on a free mutex (win32_ext_call): keep the GIL instead
 * of handing it over, unless another thread queues for it and this one has
 * held it for 2 ms.  The game thread handed it over at every take of the
 * sound library's mutex (~119,000 times in a 50 s race), and each hand-over
 * to a waiting worker risks a late wake-up (ticketlock.h).  Taking every
 * free mutex outright cut the late frames of a race's warm drive from ~8 to
 * ~6 a run, but in a heavy free-roam scene (32 friends' cars, 13.5 ms of
 * game work a frame) the other threads then waited 4 to 10 times longer
 * (the loader 0.7 -> 8.7 ms on average, p99 33 -> 262 ms): the game thread
 * let go only at its pacer sleep.  The 2 ms bound keeps their waits near
 * what the old hand-overs gave.  NFSU2_GIL_KEEP_MS sets it (0: hand over at
 * every take, as before). */
static double g_gil_keep_ms = -1;
static int gil_keep_for_mutex(void){
    if (g_gil_keep_ms < 0){ const char *e = getenv("NFSU2_GIL_KEEP_MS"); g_gil_keep_ms = e && *e ? atof(e) : 2.0; }
    if (g_gil_use_mx || !(g_gil_keep_ms > 0) || !tl_holding) return 0;
    return !ticket_queued(&g_gil) || nfsu2_now_ms() - tl_acq_ms < g_gil_keep_ms;
}
static void gil_release(void){
    double now = nfsu2_now_ms();
    if (tl_holding){
        double b = held_busy(now);
        if (b > 0){ tl_busy_ms += b; g_busy_all_ms += b; }
        tl_holding = 0;
    }
    g_gil_rel_ms = now;
    if (g_gil_use_mx) pthread_mutex_unlock(&g_gil_mx);
    else if (tl_barged){ tl_barged = 0; ticket_barge_unlock(&g_gil); }
    else ticket_unlock(&g_gil);
}
static double gil_pct(const uint32_t *hist, uint64_t n, double p){   /* upper edge of the bucket, ms */
    uint64_t want = (uint64_t)(p * (double)n + 0.5), acc = 0;
    for (int k = 0; k < 24; k++){ acc += hist[k]; if (acc >= want && want) return (double)(2u << k) / 1000.0; }
    return 0;
}
static void gil_print(const char *what, const GilStat *s){
    fprintf(stderr, "  %-12s %9llu taken, %8llu queued (%4.1f%%): wait avg %.3f p50<%.3f p99<%.3f max %.2f ms;"
                    " hand-off avg %.3f p50<%.3f p99<%.3f max %.2f ms\n", what,
            (unsigned long long)s->n, (unsigned long long)s->queued, s->n ? 100.0 * (double)s->queued / (double)s->n : 0.0,
            s->queued ? s->wait_ms / (double)s->queued : 0.0, gil_pct(s->whist, s->queued, 0.5), gil_pct(s->whist, s->queued, 0.99),
            s->wait_max_ms, s->queued ? s->hand_ms / (double)s->queued : 0.0, gil_pct(s->hhist, s->queued, 0.5),
            gil_pct(s->hhist, s->queued, 0.99), s->hand_max_ms);
}
void win32_gil_report(void){
    /* the mutex cannot tell whether anyone took it: there every fair yield
     * released it (and every 32nd napped 50 us) */
    fprintf(stderr, "  guest lock (%s), queued acquisitions; %llu fair yields %s; %llu frame-boundary takes, %llu queued after 20 ms:\n",
            g_gil_use_mx > 0 ? "pthread mutex, NFSU2_GIL=mutex" : "FIFO ticket lock", (unsigned long long)g_gil_fair_yields,
            g_gil_use_mx > 0 ? "released it" : "handed it over",
            (unsigned long long)g_gil_frame_takes, (unsigned long long)g_gil_frame_queued);
    gil_print("all", &g_gs_tot);
    for (int i = 0; i < g_gs_n; i++){ char nm[32]; snprintf(nm, sizeof nm, "tid %u", g_gs[i].tid); gil_print(nm, &g_gs[i]); }
}
/* [status] (autotest.c): the guest lock since the last status line, and the
 * worst wait of any one thread in that window */
int win32_gil_status(char *buf, size_t n){
    GilStat *w = &g_gs_win;
    int k = snprintf(buf, n, " gil=%llu/%llu wait<%.2f/%.2fms hand<%.3f/%.2fms",
                     (unsigned long long)w->queued, (unsigned long long)w->n, gil_pct(w->whist, w->queued, 0.99), w->wait_max_ms,
                     gil_pct(w->hhist, w->queued, 0.99), w->hand_max_ms);
    memset(w, 0, sizeof *w);
    return k;
}
#endif
void guest_lock(CPU *c){ ts_set(TS_GIL, 0); gil_acquire(c); tl_cpu=c; g_cur_cpu=c; reg_cpu(c); gil_took(); ts_set(TS_RUN, 0); }
void guest_unlock(void){ ts_set(TS_IDLE, 0); gil_gave(); gil_release(); }
/* Exit path (boot.c): take the lock for good, so no guest thread can start
 * another file or registry write while the process goes down.  A helper
 * thread queues on the mutex (a trylock loop would never win against the
 * guest, which re-locks right after each yield) and then keeps it; returns
 * 0 if that did not happen within max_ms (a guest thread stuck in a call). */
static pthread_mutex_t g_frz_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_frz_cv = PTHREAD_COND_INITIALIZER;
static int g_frozen = 0;
static void *freeze_main(void *unused){
    (void)unused;
    gil_acquire(NULL);
    pthread_mutex_lock(&g_frz_mx); g_frozen = 1; pthread_cond_broadcast(&g_frz_cv);
    pthread_mutex_unlock(&g_frz_mx);
    for (;;) pause();
    return 0;
}
int win32_freeze_guest(int max_ms){
    pthread_t th;
    if (pthread_create(&th, 0, freeze_main, 0) != 0) return 0;
    pthread_detach(th);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += max_ms / 1000; ts.tv_nsec += (long)(max_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&g_frz_mx);
    while (!g_frozen && pthread_cond_timedwait(&g_frz_cv, &g_frz_mx, &ts) == 0) {}
    int ok = g_frozen;
    pthread_mutex_unlock(&g_frz_mx);
    return ok;
}
static void guest_yield_begin(void){ gil_gave(); gil_release(); }
static void guest_yield_end(void){
    ts_set(TS_GIL, 0);
    gil_acquire(tl_cpu); g_cur_cpu = tl_cpu; gil_took();
    ts_set(TS_RUN, 0);
}
static void guest_yield_end_frame(void){         /* the frame pacer's: gil_acquire_frame */
    ts_set(TS_GIL, 0);
    gil_acquire_frame(tl_cpu); g_cur_cpu = tl_cpu; gil_took();
    ts_set(TS_RUN, 0);
}
/* Host-side blocking waits inside an HLE call (metal_present) */
void guest_wait_begin(void){ ts_set(TS_IDLE, 0); guest_yield_begin(); }   /* watchdog: host side */
void guest_wait_end(void){ guest_yield_end(); }
/* the same around a wait on kernel objects (winsock.c's WSAWaitForMultipleEvents): the watchdog names the first */
void guest_wait_begin_objs(uint32_t h){ ts_set(TS_WAITM, h); guest_yield_begin(); }
/* Sleep with the big lock released (win32.c: the game's frame wait loop). */
uint64_t g_sleep_late_ns, g_sleep_gil_ns, g_sleep_n;    /* diagnostics: sleep lateness / lock wait */
#ifdef __wasm__
/* wasm: nanosleep is the worker's Atomics.wait with a timeout, which wakes on
 * the OS timer.  On Windows that timer ticks every 1 ms or every 15.6 ms,
 * depending on what else the browser is doing (Chrome 155, 2026-10-19: a
 * 1 ms wait took 15.5 ms at the median, a few came back after ~1 ms), and
 * every oversleep of the frame pacer cost a 120 Hz slot: a race ran at
 * 94 fps where the game thread's work alone allows 119.  So the wait ends a
 * margin before the deadline, the margin is waited in shorter waits as far
 * as the timer allows, and the rest is spun on the clock, with the guest
 * lock still released.  The margin is learned from the waits: it jumps to a
 * late wake-up and comes down slowly.  No spin while the page does not draw
 * (a hidden tab): nobody sees the frames then.
 *
 * Whether a wait can be tried at all comes from a probe thread, not from
 * the pacer's own waits.  Before, with the 15.6 ms timer the pacer spun its
 * whole sleep until its margin had come down below the sleep, every 1.5 to
 * 5 s, and the wait it then tried mostly overslept by up to 15 ms: one or two
 * slots lost, a hitch every few seconds.  The probe waits 1 ms right after a
 * wake-up, every ~10 ms, and keeps the lateness: up at once to a late one,
 * down in about a second to the fine timer's.  The pacer's margin is never
 * below what that lateness needs, so on the coarse timer it does not try.
 * The timer is the process's, so the probe's waits stand for the pacer's.
 * Without a probe (no thread to be had) the margin alone decides, as before. */
int hfr_display_live(void);              /* framerate.c */
static _Atomic int g_probe_late_us = -1;                    /* the probe's lateness, -1 = no probe (yet) */
static uint64_t g_probe_n, g_probe_coarse;                  /* diagnostics: probes, those > 4 ms late */
static uint64_t g_ps_n, g_ps_waits, g_ps_over;              /* pacer sleeps, waits tried, waits past the deadline */
static double g_ps_over_max;
/* diagnostics: ms spun on the clock (total, and since the last [status] line with its sleeps), and how late the
 * waits woke: < 0.25, 0.5, 1, 2, 4, 8 ms and later */
static double g_ps_spin_ms, g_ps_spin_win_ms, g_ps_margin; static uint64_t g_ps_win_n;
static uint64_t g_ps_halves, g_ps_hover; static double g_ps_hover_max;   /* the margin's waits, those past the deadline */
static uint64_t g_ps_late_h[7];
int pacer_spin_status(char *buf, size_t n){
    int k = g_ps_win_n ? snprintf(buf, n, " spin=%.2fms/f", g_ps_spin_win_ms / (double)g_ps_win_n) : 0;
    g_ps_spin_win_ms = 0; g_ps_win_n = 0;
    return k;
}
static void *timer_probe_main(void *unused){
    (void)unused;
    double est = -1.0;
    int run = 0, logged = 0;                                /* probes in a row > 4 ms late */
    for (;;){
        struct timespec one = { 0, 1000000L }, rest = { 0, 9000000L };
        double t0 = nfsu2_now_ms();
        nanosleep(&one, 0);
        double late = nfsu2_now_ms() - t0 - 1.0;
        if (late < 0.0) late = 0.0;
        est = est < 0.0 || late > est ? late : est + (late - est) * 0.03;
        atomic_store(&g_probe_late_us, (int)(est * 1000.0));
        g_probe_n++;
        if (!(late > 4.0)) run = 0;
        else {
            g_probe_coarse++;
            if (++run == 10 && !logged){                    /* once: a coarse timer, not a blip */
                logged = 1;
                fprintf(stderr, "[timer] 1 ms waits take %.1f ms: the frame pacer spins its sleeps while the browser's timer is this coarse\n",
                        late + 1.0);
            }
        }
        if (!hfr_display_live()) rest.tv_nsec = 250000000L;
        nanosleep(&rest, 0);
    }
    return 0;
}
static void timer_probe_start(void){
    pthread_t th;
    if (pthread_create(&th, 0, timer_probe_main, 0) == 0) pthread_detach(th);
}
/* exit report (main_web.c), under the frame-rate section */
void pacer_sleep_report(void){
    if (!g_ps_n) return;
    int pl = atomic_load(&g_probe_late_us);
    fprintf(stderr, "  pacer sleeps: %llu, an OS wait tried in %llu (%.1f %%), %llu of them woke past the deadline (worst %.2f ms)\n",
            (unsigned long long)g_ps_n, (unsigned long long)g_ps_waits, 100.0 * g_ps_waits / g_ps_n,
            (unsigned long long)g_ps_over, g_ps_over_max);
    if (pl >= 0)
        fprintf(stderr, "  timer probe: %llu waits of 1 ms, %.1f %% more than 4 ms late, lateness now %.2f ms\n",
                (unsigned long long)g_probe_n, g_probe_n ? 100.0 * g_probe_coarse / g_probe_n : 0.0, pl / 1000.0);
    else fprintf(stderr, "  timer probe: none (no thread)\n");
    uint64_t lw = 0; for (int i = 0; i < 7; i++) lw += g_ps_late_h[i];
    double q = lw ? 100.0 / (double)lw : 0.0;
    fprintf(stderr, "  pacer spin: %.2f ms per sleep, margin now %.2f ms; its waits woke late by < 0.25 ms %.1f %%, < 0.5 %.1f %%, "
            "< 1 %.1f %%, < 2 %.1f %%, < 4 %.1f %%, < 8 %.1f %%, more %.1f %%\n", g_ps_spin_ms / (double)g_ps_n, g_ps_margin,
            g_ps_late_h[0] * q, g_ps_late_h[1] * q, g_ps_late_h[2] * q, g_ps_late_h[3] * q, g_ps_late_h[4] * q, g_ps_late_h[5] * q,
            g_ps_late_h[6] * q);
    if (g_ps_halves)
        fprintf(stderr, "  pacer margin waits: %.2f per sleep, %llu woke past the deadline (worst %.2f ms)\n",
                (double)g_ps_halves / (double)g_ps_n, (unsigned long long)g_ps_hover, g_ps_hover_max);
}
static void ps_late(double ms){
    int i = ms < 0.25 ? 0 : ms < 0.5 ? 1 : ms < 1.0 ? 2 : ms < 2.0 ? 3 : ms < 4.0 ? 4 : ms < 8.0 ? 5 : 6;
    g_ps_late_h[i]++;
}
static void sleep_precise_ns(int64_t ns){
    static _Thread_local double margin = 2.0;               /* ms */
    static pthread_once_t probe_once = PTHREAD_ONCE_INIT;
    /* NFSU2_PACE_SPIN=0 (benchmarks): no spin, so the game thread's
     * instruction count is its work alone (web/tools/bench.mjs) */
    static int spin = -1;
    if (spin < 0){ const char *e = getenv("NFSU2_PACE_SPIN"); spin = !(e && *e == '0'); }
    if (!spin || !hfr_display_live()){
        struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
        double t0 = nfsu2_now_ms();
        nanosleep(&ts, 0);
        if (spin || !hfr_display_live()) return;            /* a hidden page: not counted */
        g_ps_n++; g_ps_win_n++; g_ps_waits++; ps_late(nfsu2_now_ms() - t0 - (double)ns / 1e6);
        return;
    }
    pthread_once(&probe_once, timer_probe_start);
    int pl = atomic_load(&g_probe_late_us);
    double need = pl < 0 ? 0.0 : pl / 1000.0 * 1.25 + 0.25;   /* what the probe's lateness needs */
    double t0 = nfsu2_now_ms(), until = t0 + (double)ns / 1e6, coarse = until - (margin > need ? margin : need) - t0;
    g_ps_n++; g_ps_win_n++;
    if (coarse >= 0.25){
        int64_t cns = (int64_t)(coarse * 1e6);
        struct timespec ts = { (time_t)(cns / 1000000000LL), (long)(cns % 1000000000LL) };
        nanosleep(&ts, 0);
        double t1 = nfsu2_now_ms(), want = (t1 - (t0 + coarse)) * 1.25 + 0.25;
        margin = want > margin ? want : margin - (margin - want) * 0.02;
        g_ps_waits++; ps_late(t1 - (t0 + coarse));
        if (t1 > until){ g_ps_over++; if (t1 - until > g_ps_over_max) g_ps_over_max = t1 - until; }
    } else margin *= 0.997;                                  /* no wait made */
    if (margin < 0.5) margin = 0.5;
    if (margin > 20.0) margin = 20.0;
    g_ps_margin = margin;
    /* The margin is mostly waited too, in waits of half what is left: a wait wakes late by a share of its
     * length (WebKit, Safari and every iPhone browser: a 10 ms wait 1-4 ms, a 1 ms wait 0.3 ms), so the
     * halves end close to the deadline, the last wait (as short as the probe's) ends what the probe's
     * lateness needs before it, and only that is spun.  Spinning the whole margin there was 2.7 ms a frame
     * at 60 fps, 0.7 ms with these waits, and the web content process took a quarter less CPU in free roam
     * with no more missed slots (Playwright WebKit, M4 Pro, 2026-10-24; a player's iPhone log has the
     * same 0.3 ms for the probe's 1 ms waits).  No such wait
     * is shorter than 0.25 ms or ends later than the probe's lateness allows, so a coarse timer (probe
     * lateness at the timer's tick) never tries one. */
    static int halve = -1;
    if (halve < 0){ const char *e = getenv("NFSU2_PACE_HALVE"); halve = !(e && *e == '0'); }
    double hneed = need > 0.4 ? need : 0.4;
    for (double rem; halve && pl >= 0 && (rem = until - nfsu2_now_ms()) >= hneed + 0.25;){
        int64_t hns = (int64_t)((rem >= 2.0 * hneed ? rem * 0.5 : rem - hneed) * 1e6);
        struct timespec ts = { (time_t)(hns / 1000000000LL), (long)(hns % 1000000000LL) };
        nanosleep(&ts, 0);
        g_ps_halves++;
        double t2 = nfsu2_now_ms();
        if (t2 > until){ g_ps_hover++; if (t2 - until > g_ps_hover_max) g_ps_hover_max = t2 - until; break; }
    }
    double s0 = nfsu2_now_ms(), s1 = s0;
    while (s1 < until) s1 = nfsu2_now_ms();
    g_ps_spin_ms += s1 - s0; g_ps_spin_win_ms += s1 - s0;
}
#else
/* On macOS nanosleep wakes up with the timer coalescing slack: in the game a
 * 3 ms request is 447 us late on average (separate measurement: median
 * ~770 us, QoS makes no difference).  A kevent timer with NOTE_CRITICAL is
 * not coalesced: median ~45 us. */
static void sleep_precise_ns(int64_t ns){
    static _Thread_local int kq = -1;
    if (kq < 0) kq = kqueue();
    if (kq >= 0){
        struct kevent ev, out;
        EV_SET(&ev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, ns, 0);
        if (kevent(kq, &ev, 1, &out, 1, NULL) == 1) return;
    }
    struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
    nanosleep(&ts, 0);
}
#endif
static void sleep_released(int64_t ns, int frame){
    struct timespec a, b, d;
    guest_yield_begin();
    clock_gettime(CLOCK_MONOTONIC, &a); sleep_precise_ns(ns); clock_gettime(CLOCK_MONOTONIC, &b);
    if (frame) guest_yield_end_frame(); else guest_yield_end();
    clock_gettime(CLOCK_MONOTONIC, &d);
    int64_t sl = (int64_t)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec) - ns;
    int64_t gl = (int64_t)(d.tv_sec - b.tv_sec) * 1000000000LL + (d.tv_nsec - b.tv_nsec);
    g_sleep_late_ns += sl > 0 ? (uint64_t)sl : 0; g_sleep_gil_ns += (uint64_t)gl; g_sleep_n++;
}
void guest_sleep_ns(int64_t ns){ sleep_released(ns, 0); }
/* The frame pacer's sleep to its slot boundary (framerate.c): the guest lock
 * is taken back ahead of the queue (gil_acquire_frame). */
void guest_sleep_frame_ns(int64_t ns){ sleep_released(ns, 1); }

/* --- frame rate lock (60 Hz) ---------------------------------------
 * The virtual clock advances EXACTLY 16.667 ms per frame, so the game's
 * speed = (real fps / 60).  At 117 fps from the emulator the game ran at
 * 195% speed.  Locking the frame to 60 Hz makes the speed independent of
 * the FPS.  The big lock is RELEASED while sleeping, or the worker threads
 * stall. */
/* TIMELOG=1: the virtual/real time ratio and frame count once a second.
 * To measure the "the game speeds up needlessly in cut scenes" complaint --
 * a ratio above 1.0 means the game really is running fast. */
static void timelog_tick(void){
    static int on = -1;
    if (on < 0){ const char *e = getenv("TIMELOG"); on = (e && *e && *e != '0'); }
    if (!on) return;
    uint64_t host_us(void);
    static uint64_t lr = 0, lv = 0; static long lf = 0;
    long metal_frames(void);
    uint64_t r = gil_now_us(), v = host_us();
    long f = metal_frames();
    if (!lr){ lr = r; lv = v; lf = f; return; }
    if (r - lr < 1000000ull) return;
    fprintf(stderr, "  [time] real %.2f s, virtual %.2f s, ratio %.3f, %ld frames (%.1f fps)\n",
            (r-lr)/1e6, (v-lv)/1e6, (double)(v-lv)/(double)(r-lr),
            f-lf, (f-lf)*1e6/(double)(r-lr));
    lr = r; lv = v; lf = f;
}

void frame_pace(void){
    timelog_tick();
    /* DEFAULT: NO BRAKE.  The 60 Hz rationale above NO LONGER HOLDS --
     * since the virtual clock moved to VCLOCK=real the game speed is
     * independent of the real fps (measured: virtual/real = 0.999 at both
     * 63.7 and 99 fps).  With the brake on, 61% of a race frame was
     * nanosleep; with it off the race went 63.7 -> 79.4 fps, the menus
     * 120 fps (the display refresh limit).  PACE60=1 restores the old
     * behaviour.  Vsync already cuts off runaway fps; the virtual clock's
     * per-frame step has a 4 ms (250 fps) floor. */
    static int off = -1;
    if (off < 0){ const char *e = getenv("PACE60");
                  int hfr_mode_off(void);
                  off = !(e && *e && *e != '0');
                  /* framerate.c paces every frame unless NFSU2_FPS=off: no second limiter */
                  if (!off && !hfr_mode_off()){
                      fprintf(stderr, "PACE60 ignored: the frame pacer is active (NFSU2_FPS=off restores the old timing)\n");
                      off = 1; } }
    if (off) return;
    static uint64_t deadline = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
    if (!deadline || now > deadline + 200000ull) deadline = now;   /* far behind: reset */
    deadline += 16667ull;
    if (now < deadline){
        uint64_t d = deadline - now;
        struct timespec sl = { (time_t)(d/1000000ull), (long)((d%1000000ull)*1000ull) };
        guest_yield_begin();
        nanosleep(&sl, 0);
        guest_yield_end();
    }
}

/* Fair GIL: guest code can also spin on non-blocking Win32 calls
 * (e.g. a wait loop spinning on EnterCriticalSection).  We periodically
 * release the lock and give the other guest threads a turn.           */
/* ---- REAL critical sections ------------------------------------------
 * EnterCriticalSection/LeaveCriticalSection used to be no-op stubs.  The
 * single big lock (GIL) covered for them most of the time BUT
 * guest_fair_yield releases the GIL every 1024 Win32 calls: a thread could
 * lose the lock while INSIDE a critical section and another thread could
 * enter the same section.  That silently corrupts the game's loader/audio/
 * video queues.
 *
 * The GIL MUST BE RELEASED while waiting for the lock, or it deadlocks.  */
#define MAXCS 512
static struct { uint32_t key; pthread_mutex_t m; int used; uint32_t owner; int depth; } g_cs[MAXCS];
static int g_cs_used = 0;
/* watchdog.c: critical sections held right now */
int win32_cs_held(uint32_t *key, uint32_t *owner, int *depth, int max){
    int n = 0;
    for (int i = 0; i < MAXCS && n < max; i++)
        if (g_cs[i].used && g_cs[i].owner){ key[n] = g_cs[i].key; owner[n] = g_cs[i].owner; depth[n] = g_cs[i].depth; n++; }
    return n;
}
int win32_cs_count(void){ return g_cs_used; }
static pthread_mutex_t g_cs_tab = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int tl_cs_depth = 0;
uint64_t g_cs_enter = 0, g_cs_contended = 0;

static int cs_disabled(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("NOCS"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
static pthread_mutex_t *cs_for(uint32_t key){
    if (cs_disabled()) return 0;          /* old behaviour: no-op stub */
    pthread_mutex_lock(&g_cs_tab);
    unsigned h = (key >> 2) % MAXCS;
    for (unsigned i = 0; i < MAXCS; i++){
        unsigned j = (h + i) % MAXCS;
        if (g_cs[j].used && g_cs[j].key == key){
            pthread_mutex_unlock(&g_cs_tab); return &g_cs[j].m;
        }
        if (!g_cs[j].used){
            pthread_mutexattr_t a;
            pthread_mutexattr_init(&a);
            pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs[j].m, &a);
            pthread_mutexattr_destroy(&a);
            g_cs[j].key = key; g_cs[j].used = 1; g_cs_used++;
            pthread_mutex_unlock(&g_cs_tab); return &g_cs[j].m;
        }
    }
    pthread_mutex_unlock(&g_cs_tab);
    return 0;                      /* table full: fall back to the old behaviour */
}

/* Owner/recursion of a section (watchdog report); only its owner changes them. */
static void cs_owned(pthread_mutex_t *m, uint32_t tid, int d){
    size_t i = (size_t)(((char *)m - (char *)&g_cs[0].m) / (ptrdiff_t)sizeof g_cs[0]);
    if (i >= MAXCS) return;
    g_cs[i].depth += d;
    g_cs[i].owner = g_cs[i].depth > 0 ? (tid ? tid : g_cs[i].owner) : 0u;
}
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
static uint32_t g_wndproc = 0;
static uint32_t g_hwnd    = MM_HWND_GAME;
#define WMQ 256
static struct { uint32_t msg, wp, lp; } g_wmq[WMQ];
static int g_wmq_head = 0, g_wmq_tail = 0;
static int g_wm_activated = 0;
uint64_t g_wm_sent = 0, g_wm_dispatched = 0;
void lifecycle_pump(void); int lifecycle_may_park(void); int lifecycle_game_parked(void);   /* lifecycle.c */
void kbd_pump(void);                                               /* keyboard.c */

static void wm_push(uint32_t m, uint32_t wp, uint32_t lp){
    int nx = (g_wmq_tail + 1) % WMQ;
    if (nx == g_wmq_head) return;
    g_wmq[g_wmq_tail].msg = m; g_wmq[g_wmq_tail].wp = wp; g_wmq[g_wmq_tail].lp = lp;
    g_wmq_tail = nx;
}
/* autotest.c: synthetic WM_KEYDOWN/WM_KEYUP/WM_CHAR */
void win32_post_message(uint32_t m, uint32_t wp, uint32_t lp){ wm_push(m, wp, lp); }
/* free entries in the queue (keyboard.c moves host messages in up to that) */
int win32_msg_room(void){ return (g_wmq_head - g_wmq_tail - 1 + WMQ) % WMQ; }
static int wm_pop(uint32_t *m, uint32_t *wp, uint32_t *lp){
    if (g_wmq_head == g_wmq_tail) return 0;
    *m = g_wmq[g_wmq_head].msg; *wp = g_wmq[g_wmq_head].wp; *lp = g_wmq[g_wmq_head].lp;
    g_wmq_head = (g_wmq_head + 1) % WMQ;
    return 1;
}

static uint32_t cur_tid(CPU *c){
    return (c && c->fs_base && mem_is_committed(c->fs_base+0x24u))
           ? MR32(c->fs_base+0x24u) : 0u;
}

/* A guest function that must not lose the GIL to another guest thread
 * halfway (overrides.c sub_0073b900): no fair yield, and a mutex that is
 * free (or already ours) is taken without handing the GIL over, as Windows
 * does not switch threads there either.  A wait that blocks still releases
 * it. */
static _Thread_local int tl_noyield = 0;
void guest_noyield(int d){ tl_noyield += d; }
static int mutex_take_now(Obj *o, uint32_t me){
    int got = 0;
    pthread_mutex_lock(&o->mtx);
    if (o->owner_tid == me && o->rec > 0){ o->rec++; got = 1; }
    else if (o->signaled){ o->signaled = 0; o->owner_tid = me; o->rec = 1; got = 1; }
    pthread_mutex_unlock(&o->mtx);
    return got;
}

void guest_fair_yield(void){
    if (tl_cs_depth || tl_noyield) return;    /* never yield inside a critical section */
    static _Thread_local uint32_t n = 0;
    if ((++n & 0x3FFu) != 0) return;          /* once every 1024 calls */
#ifdef __wasm__
    /* FIFO lock: hand it over only when someone queues; they all run before
     * this thread gets it back, so no sched_yield or nap is needed */
    if (g_gil_use_mx){
        /* NFSU2_GIL=mutex: exactly the lock this replaced (the #else branch
         * below): release, sched_yield (a no-op in the browser), and on every
         * 32nd yield a 50 us nap, the only thing that lets a queued thread in */
        g_gil_fair_yields++;
        guest_yield_begin();
        sched_yield();
        if ((n & 0x7FFFu) == 0){ struct timespec ts = {0, 50000L}; nanosleep(&ts, 0); }
        guest_yield_end();
        return;
    }
    if (!ticket_queued(&g_gil)) return;
    g_gil_fair_yields++;
    guest_yield_begin();
    guest_yield_end();
#else
    guest_yield_begin();
    sched_yield();
    /* NOTE: there used to be a nanosleep(50us) here every time.  On macOS a
     * sleep of 50 us actually takes ~1-2 ms (scheduler quantum + timer
     * coalescing).  In the cut scene before a race the guest calls a Win32
     * function ~110 thousand times per frame; that meant ~110 sleeps =
     * ~165 ms per frame.  Measured: 70% of the guest thread in
     * __semwait_signal, i.e. SLEEP, not computation.  Frame time 235 ms
     * (4.6 fps).  sched_yield already provides the fairness; the sleep is
     * thinned out (only to guarantee the other threads make progress). */
    if ((n & 0x7FFFu) == 0){
        struct timespec ts = {0, 50000L}; nanosleep(&ts, 0);
    }
    guest_yield_end();
#endif
}

/* v12na: slots are now reused.  The table used to only grow
 * (CloseHandle/RegCloseKey released nothing): every RegOpenKey,
 * CreateEvent, CreateMutex, CreateSemaphore ate a slot for good and after
 * 511 the game got NULL handles -- a time bomb in a long career session.
 * Thread slots stay.  Since a thread may still be waiting, mtx/cv are
 * neither destroyed nor re-initialized. */
static int g_free[OBJ_MAX], g_nfree = 0;
static uint64_t g_obj_reused = 0, g_obj_full = 0, g_obj_reclaimed = 0;
/* The table is full (g_lock held; every caller of obj_new holds the guest
 * lock too).  Take back the slot of the thread that ended first among those
 * whose handle no import has looked up since CreateThread: never waited on,
 * closed, duplicated, resumed or asked for its exit code, so no wait on it
 * can be pending either (waits look their handles up before they give the
 * guest lock away).  SPEED2.EXE's per-load timer thread is one: sub_00726454
 * starts it (entry 0x7263C7: timeBeginPeriod, a callback every 10 ms until a
 * flag is cleared) for every race and world load and stores the handle at
 * 0x8B47B8, which nothing in the exe reads; each one ends before the next
 * starts, its handle open.  On Windows such a handle stays valid as long as
 * the process; the table here ran out after ~4,000 loads, and from then on
 * every CreateEvent, CreateThread and RegOpenKey got NULL.  The reclaimed
 * slot moves to the next generation, so the old handle names nothing from
 * then on.  Returns 0 when no thread qualifies. */
static int obj_reclaim(void){
    static uint64_t shown = 0;
    if (getenv("NFSU2_NO_OBJREUSE")) return 0;
    int best = 0;
    for (int i = 1; i < g_obj_n && i < OBJ_MAX; i++){
        Obj *o = &g_obj[i];
        if (o->kind != OB_THREAD || !o->ended || o->closed || o->refs != 1 || __atomic_load_n(&o->looked, __ATOMIC_RELAXED)) continue;
        if (!best || o->end_seq < g_obj[best].end_seq) best = i;
    }
    if (!best) return 0;
    Obj *o = &g_obj[best];
    pthread_mutex_lock(&o->mtx);
    if (shown++ < 8)
        fprintf(stderr, "  [object] table full: handle 0x%08x of the ended thread tid=%u (entry 0x%08x, started from 0x%08x,"
                        " handle never looked up) goes to a new object\n",
                H_BASE + o->gen * OBJ_MAX + (uint32_t)best, o->tid, o->guest_entry, o->creator);
    o->kind = OB_NONE; o->gen = (o->gen + 1u) % OBJ_GENS;
    pthread_mutex_unlock(&o->mtx);
    g_obj_reclaimed++;
    return best;
}
static uint32_t g_obj_serial;                  /* objects made so far (g_lock) */
static uint32_t obj_new(ObKind k){
    pthread_mutex_lock(&g_lock);
    int i = 0;
    if (g_nfree){ i = g_free[--g_nfree]; g_obj_reused++; }
    else if (g_obj_n < OBJ_MAX) i = g_obj_n++;
    else if ((i = obj_reclaim()) != 0) {}
    else if (!g_obj_full++) fprintf(stderr, "  [object] WARNING: object table full (%d)\n", OBJ_MAX - 1);
    uint32_t ser = i ? ++g_obj_serial : 0u;
    if (i && !ser) ser = ++g_obj_serial;            /* 0 names no object */
    pthread_mutex_unlock(&g_lock);
    if (!i) return 0;
    Obj *o=&g_obj[i];
    if (o->prim_ok){
        pthread_mutex_lock(&o->mtx);
        o->signaled = 0; o->manual = 0; o->guest_arg = o->guest_entry = o->tid = 0;
        o->owner_tid = 0; o->rec = 0; o->started = 0; o->pending = 0; o->name[0] = 0;
        o->exit_code = 0; o->ended = 0; o->term_req = 0; o->closed = 0; o->teb = o->stack_lo = 0;
        o->t_armed = 0; o->t_due_us = o->t_period_us = 0;
        o->creator = 0; o->end_seq = 0; o->looked = 0;
        o->reg_key = 0; o->reg_access = 0; o->sem_max = 0;
        o->refs = 1; o->kind = k; o->serial = ser;
        pthread_mutex_unlock(&o->mtx);
    } else {
        memset(o,0,sizeof *o); o->kind=k; o->refs = 1; o->serial = ser;
        pthread_mutex_init(&o->mtx,0); pthread_cond_init(&o->cv,0); o->prim_ok = 1;
    }
    /* the code that made it, for win32_obj_creators (an import's return address: the calling thread's eip
     * while the import runs); a thread's is set again where it starts, the same value */
    o->creator = tl_cpu ? tl_cpu->eip : 0u;
    return H_BASE + o->gen * OBJ_MAX + (uint32_t)i;
}
/* A thread in winsock.c's WSAWaitForMultipleEvents waits on the network's
 * event word, not on an object's condition variable: whatever signals or
 * closes an object wakes it too (winsock_wake; it returns at once while no
 * such wait runs, and is absent from builds without winsock.c). */
void winsock_wake(void) __attribute__((weak));
static void obj_wake(void){ if (winsock_wake) winsock_wake(); }
/* CloseHandle / RegCloseKey: free the slot when the last handle is closed */
static void obj_close(uint32_t h){
    int si = obj_slot(h);
    if (si < 0) return;
    uint32_t i = (uint32_t)si; Obj *o = &g_obj[i];
    if (!o->kind || o->kind == OB_FILE) return;
    if (o->kind == OB_THREAD){
        __atomic_fetch_add(&o->looked, 1u, __ATOMIC_RELAXED);
        /* v12na: a thread's slot is freed once it has ended and its last
         * handle is closed, whichever comes last (thread_finish does it when
         * the close came first).  The game starts a thread per race load and
         * server.dll's NetLib closes its idle thread's handle right away. */
        pthread_mutex_lock(&o->mtx);
        int lastt = --o->refs <= 0, gone = lastt && o->ended;
        if (lastt && !o->ended) o->closed = 1;
        if (gone) o->kind = OB_NONE;
        pthread_mutex_unlock(&o->mtx);
        if (gone && !getenv("NFSU2_NO_OBJREUSE")){
            pthread_mutex_lock(&g_lock); g_free[g_nfree++] = (int)i; pthread_mutex_unlock(&g_lock); }
        return;
    }
    pthread_mutex_lock(&o->mtx);
    int last = --o->refs <= 0;
    if (last){ o->kind = OB_NONE; pthread_cond_broadcast(&o->cv); }
    pthread_mutex_unlock(&o->mtx);
    if (last && !getenv("NFSU2_NO_OBJREUSE")){
        pthread_mutex_lock(&g_lock); g_free[g_nfree++] = (int)i; pthread_mutex_unlock(&g_lock); }
    if (last) obj_wake();                       /* a wait on it fails now (wait_try) */
}
static void thread_report(void);
/* v12na: object table occupancy report (called by win32.c win32_report) */
void win32_obj_report(void){
    int k[8] = {0};
    for (int i = 1; i < g_obj_n && i < OBJ_MAX; i++) if (g_obj[i].kind < 8) k[g_obj[i].kind]++;
    char tm[24] = "";                                   /* waitable timers: named only when there are some */
    if (k[OB_TIMER]) snprintf(tm, sizeof tm, ", timer %d", k[OB_TIMER]);
    fprintf(stderr, "  object table      : %d/%d slots used (event %d, mutex %d, thread %d, key %d, semaphore %d%s, free %d), %llu reused, full %llu, %llu reclaimed\n",
            g_obj_n - 1, OBJ_MAX - 1, k[OB_EVENT], k[OB_MUTEX], k[OB_THREAD], k[OB_KEY], k[OB_SEM], tm, k[OB_NONE],
            (unsigned long long)g_obj_reused, (unsigned long long)g_obj_full, (unsigned long long)g_obj_reclaimed);
    /* ended threads whose handle is still open, by entry and the code that
     * started them, and how often an import looked their handles up */
    struct { uint32_t entry, creator, n, looked; } grp[6]; int ng = 0, nend = 0;
    for (int i = 1; i < g_obj_n && i < OBJ_MAX; i++){
        Obj *o = &g_obj[i];
        if (o->kind != OB_THREAD || !o->ended) continue;
        nend++;
        int j = 0;
        while (j < ng && (grp[j].entry != o->guest_entry || grp[j].creator != o->creator)) j++;
        if (j == ng){ if (ng == 6) continue; grp[ng].entry = o->guest_entry; grp[ng].creator = o->creator; grp[ng].n = grp[ng].looked = 0; ng++; }
        grp[j].n++; grp[j].looked += __atomic_load_n(&o->looked, __ATOMIC_RELAXED);
    }
    if (nend){
        fprintf(stderr, "  ended threads     : %d with the handle still open:", nend);
        for (int j = 0; j < ng; j++)
            fprintf(stderr, "%s %u x entry 0x%08x from 0x%08x (handles looked up %u times)", j ? ";" : "",
                    grp[j].n, grp[j].entry, grp[j].creator, grp[j].looked);
        fprintf(stderr, "\n");
    }
    thread_report();
}
/* The objects in use now by kind and by the code that made them (obj_new's creator), one line: "[objects]
 * f=N 12 in use: event 0x007ecc5b 2, ...; thread 0x0085d69b 3 (1 ended), ...".  Not part of the exit
 * report; the test scripts ask for it (Most Wanted's "objects" command, runtime/mw/debug_mw.c), so the
 * objects a load leaves behind are named with their creator. */
void win32_obj_creators(long frame){
    static const char *const kn[] = { "none", "event", "mutex", "thread", "file", "key", "semaphore", "timer" };
    struct { int kind; uint32_t creator; int n, ended; } g[48]; int ng = 0, total = 0, more = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 1; i < g_obj_n && i < OBJ_MAX; i++){
        Obj *o = &g_obj[i];
        if (o->kind == OB_NONE || o->kind >= 8) continue;
        total++;
        int j = 0;
        while (j < ng && (g[j].kind != (int)o->kind || g[j].creator != o->creator)) j++;
        if (j == ng){ if (ng == 48){ more++; continue; } g[ng].kind = (int)o->kind; g[ng].creator = o->creator; g[ng].n = g[ng].ended = 0; ng++; }
        g[j].n++; if (o->kind == OB_THREAD && o->ended) g[j].ended++;
    }
    pthread_mutex_unlock(&g_lock);
    char line[2048]; int w = snprintf(line, sizeof line, "[objects] f=%ld %d in use:", frame, total);
    for (int k = 1; k < 8; k++){
        int first = 1;
        for (int j = 0; j < ng && w < (int)sizeof line - 64; j++){
            if (g[j].kind != k) continue;
            if (first) w += snprintf(line + w, sizeof line - (size_t)w, "%s%s ", ng && line[w - 1] != ':' ? "; " : " ", kn[k]);
            else       w += snprintf(line + w, sizeof line - (size_t)w, ", ");
            w += snprintf(line + w, sizeof line - (size_t)w, "0x%08x %d", g[j].creator, g[j].n);
            if (g[j].ended) w += snprintf(line + w, sizeof line - (size_t)w, " (%d ended)", g[j].ended);
            first = 0;
        }
    }
    if (more) snprintf(line + w, sizeof line - (size_t)w, "; %d more in other groups", more);
    fprintf(stderr, "%s\n", line);
}
/* watchdog.c: one line about a kernel object a thread waits on. */
int win32_obj_describe(uint32_t h, char *buf, size_t n){
    static const char *const kn[] = { "none", "event", "mutex", "thread", "file", "key", "semaphore", "timer" };
    int i = obj_slot(h);
    if (i < 0){ return snprintf(buf, n, "handle 0x%08x (not ours)", h); }
    Obj *o = &g_obj[i];
    if (o->kind == OB_TIMER){                           /* a waitable timer: when it is due, on the virtual clock */
        uint64_t host_us(void), now = host_us();
        char due[48] = "not armed";
        if (o->t_armed) snprintf(due, sizeof due, "%s %.1f ms", o->t_due_us >= now ? "due in" : "overdue by",
                                 (double)(o->t_due_us >= now ? o->t_due_us - now : now - o->t_due_us) / 1000.0);
        return snprintf(buf, n, "handle 0x%08x (timer%s%s, %s reset, signaled %d, %s, period %.1f ms)", h,
                        o->name[0] ? " " : "", o->name, o->manual ? "manual" : "auto", o->signaled, due,
                        (double)o->t_period_us / 1000.0);
    }
    if (o->kind == OB_SEM)
        return snprintf(buf, n, "handle 0x%08x (semaphore%s%s, count %d of at most %d)", h,
                        o->name[0] ? " " : "", o->name, o->signaled, o->sem_max);
    return snprintf(buf, n, "handle 0x%08x (%s%s%s, signaled %d, owner tid %u, thread tid %u)", h,
                    o->kind <= OB_TIMER ? kn[o->kind] : "?", o->name[0] ? " " : "", o->name,
                    o->signaled, o->owner_tid, o->tid);
}
/* For fileio.c to signal its overlapped completion event. */
void win32_signal_event(uint32_t h);

static Obj *obj_get(uint32_t h){
    int i = obj_slot(h);
    if (i < 0 || !g_obj[i].kind) return 0;
    Obj *o = &g_obj[i];
    if (o->kind == OB_THREAD) __atomic_fetch_add(&o->looked, 1u, __ATOMIC_RELAXED);   /* obj_reclaim */
    return o;
}

/* ---- guest thread ---- */
#define GTHREAD_STACK MM_TSTACK_SIZE      /* 1 MB */
static uint32_t g_tstack_next = MM_TSTACK_LO;   /* wasm: 32 x 1 MB below the heap (memmap.h) */
uint64_t g_threads_started = 0, g_threads_exited = 0;
/* v12na: TEBs (0x2000 each from 0x00C20000; 0x00C60000 belongs to the timer
 * thread) and 1 MB stacks are recycled when a thread ends.  Both used to be
 * bump-allocated, and the game starts a thread per race load: after ~30
 * loads a new thread's TEB would have landed on the timer thread's, and the
 * stacks would eventually have run into the main stack at 0x7F000000. */
#define TEB_TIMER MM_TEB_TIMER
static uint32_t g_teb_free[256], g_stk_free[256]; static int g_nteb_free = 0, g_nstk_free = 0;
static uint32_t g_teb_next = MM_TEB_FIRST;
static uint64_t g_teb_reused = 0, g_thread_refused = 0;
/* Both ranges are bounded (memmap.h): the TEBs end where the next region
 * starts (MW's server.dll), the stacks where the heap starts.  0 = none left:
 * CreateThread then fails as on Windows instead of overrunning them. */
static uint32_t teb_take(void){                       /* g_lock held */
    if (g_nteb_free){ g_teb_reused++; return g_teb_free[--g_nteb_free]; }
    if (g_teb_next == TEB_TIMER) g_teb_next += MM_TEB_SIZE;
    if (g_teb_next + MM_TEB_SIZE > MM_TEB_HI) return 0;
    uint32_t t = g_teb_next; g_teb_next += MM_TEB_SIZE;
    return t;
}
static uint32_t stack_take(void){                     /* g_lock held */
    if (g_nstk_free) return g_stk_free[--g_nstk_free];
    if (g_tstack_next + GTHREAD_STACK > MM_TSTACK_HI) return 0;
    uint32_t s0 = g_tstack_next; g_tstack_next += GTHREAD_STACK;
    return s0;
}
/* A thread that cannot have a stack or a TEB is not started, loudly. */
static void thread_refused(CPU *c, uint32_t entry, int no_stack){
    uint64_t n = __sync_add_and_fetch(&g_thread_refused, 1);
    if (n > 20) return;
    fprintf(stderr, "[thread] REFUSED: CreateThread(entry 0x%08x) gets NULL (ERROR_NOT_ENOUGH_MEMORY): ", entry);
    if (no_stack) fprintf(stderr, "all %u thread stacks 0x%08x-0x%08x are in use", (MM_TSTACK_HI - MM_TSTACK_LO) / GTHREAD_STACK,
                          MM_TSTACK_LO, MM_TSTACK_HI);
    else fprintf(stderr, "all TEBs 0x%08x-0x%08x are in use", MM_TEB_FIRST, MM_TEB_HI);
    fprintf(stderr, " (%llu threads running)%s\n", (unsigned long long)(g_threads_started - g_threads_exited),
            n == 20 ? "; no more of these lines" : "");
    void win32_set_last_error(CPU *, uint32_t);
    if (c) win32_set_last_error(c, 8u);
}
static void thread_report(void){
    fprintf(stderr, "  threads           : %llu started, %llu ended; TEBs %llu reused (next 0x%08x), %d stacks free;"
                    " critical sections %d of %d\n",
            (unsigned long long)g_threads_started, (unsigned long long)g_threads_exited,
            (unsigned long long)g_teb_reused, g_teb_next, g_nstk_free, g_cs_used, MAXCS);
    if (g_thread_refused) fprintf(stderr, "  threads refused   : %llu (no stack or TEB left)\n", (unsigned long long)g_thread_refused);
#ifdef __wasm__
    win32_gil_report();
#endif
}
/* NFSU2_THREAD_TEST=<n>[:<ms>]: the thread bounds' test, before the game
 * starts (runtime/web/main_web.c).  n guest threads made through the exe's
 * own CreateThread import, each running its Sleep import for ms (3000), so
 * all of them are alive at once; one that gets no stack or TEB is refused as
 * the game would see it.  The game starts once they have ended.  In the
 * browser each thread takes a worker: n beyond the page's pool (24, the test
 * page's pool=) would wait for one with the guest lock held. */
void win32_thread_test(CPU *c){
    const char *e = getenv("NFSU2_THREAD_TEST");
    if (!e || !*e) return;
    int n = atoi(e); const char *m = strchr(e, ':');
    uint32_t ms = m ? (uint32_t)strtoul(m + 1, 0, 0) : 3000u;
    uint32_t win32_import_va(const char *), call_guest(CPU *, uint32_t, int, const uint32_t *);
    uint32_t create = win32_import_va("CreateThread"), sleep_ = win32_import_va("Sleep");
    if (!create || !sleep_){ fprintf(stderr, "[thread] test: the exe imports no CreateThread or Sleep\n"); return; }
    int made = 0, refused = 0; uint32_t top = 0;
    guest_lock(c);
    for (int i = 0; i < n; i++){
        const uint32_t a[6] = { 0, 0, sleep_, ms, 0, 0 };
        uint32_t h = call_guest(c, create, 6, a);
        Obj *o = obj_get(h);
        if (!o){ refused++; continue; }
        made++;
        if (o->stack_lo + GTHREAD_STACK > top) top = o->stack_lo + GTHREAD_STACK;
    }
    guest_unlock();
    fprintf(stderr, "[thread] test: %d threads asked for, %d started, %d refused; the highest stack ends at 0x%08x"
                    " (thread stacks 0x%08x-0x%08x, heap from 0x%08x)\n", n, made, refused, top,
            MM_TSTACK_LO, MM_TSTACK_HI, MM_HEAP_LO);
    for (uint32_t w = 0; w < ms / 10u + 1000u && g_threads_exited < (uint64_t)made; w++) usleep(10000);
    fprintf(stderr, "[thread] test: %llu of them have ended; the game starts\n", (unsigned long long)g_threads_exited);
}
/* The thread ended (its entry returned, ExitThread, or a TerminateThread
 * request honoured): CPU slot, COM's cached CPU, TEB and stack given back,
 * exit code recorded, waiters woken; the slot is freed if its last handle was
 * closed already.  Guest lock held. */
uint32_t module_nearest_fn(uint32_t addr, const char **mod);   /* module.c */
static void thread_finish(Obj *o, uint32_t code, CPU *c){
    void com_forget_cpu(CPU *);
    unreg_cpu(c);
    com_forget_cpu(c);                                /* com.c caches the CPU of the last COM call */
    __sync_fetch_and_add(&g_threads_exited, 1);
    if (!o) return;
    { const char *mn = 0;                               /* threads of a lifted DLL: say so */
      if (module_nearest_fn(o->guest_entry, &mn))
          fprintf(stderr, "[thread] tid=%u (%s, entry 0x%08x) ended, exit code %u\n", o->tid, mn, o->guest_entry, code); }
    pthread_mutex_lock(&g_lock);
    if (o->teb && g_nteb_free < 256) g_teb_free[g_nteb_free++] = o->teb;
    if (o->stack_lo && g_nstk_free < 256) g_stk_free[g_nstk_free++] = o->stack_lo;
    pthread_mutex_unlock(&g_lock);
    o->teb = o->stack_lo = 0;
    static uint64_t ends = 0;                         /* guest lock held: obj_reclaim's order */
    pthread_mutex_lock(&o->mtx);
    o->exit_code = code; o->ended = 1; o->signaled = 1; o->end_seq = ++ends;
    pthread_cond_broadcast(&o->cv);
    int gone = o->closed;
    if (gone) o->kind = OB_NONE;
    pthread_mutex_unlock(&o->mtx);
    if (gone && !getenv("NFSU2_NO_OBJREUSE")){
        pthread_mutex_lock(&g_lock); g_free[g_nfree++] = (int)(o - g_obj); pthread_mutex_unlock(&g_lock); }
}

typedef struct { uint32_t entry, arg, teb, stack; Obj *ob; } TArg;   /* TEB and stack: taken by CreateThread */

void mem_altstack_install(void);
/* Guest threads need a BIG stack: in lifted code every guest call is a host
 * C call, and lifted frames are wide.  The main guest thread (tests/boot.c)
 * got 1 GB, but the worker threads the game starts with CreateThread got the
 * macOS default of 512 KB through `pthread_create(...,0,...)` -> HOST STACK
 * OVERFLOW in deep recursion (SIGSEGV at an address outside the guest
 * space).  The reservation is virtual address space; it is only committed
 * as it is used. */
static int guest_pthread_create(pthread_t *th, void *(*fn)(void*), void *arg){
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0) return pthread_create(th, 0, fn, arg);
#ifdef __wasm__
    /* wasm: this is only the shadow stack in linear memory (address-taken
     * locals); the call chain itself lives on the engine's stack */
    pthread_attr_setstacksize(&at, 4u*1024u*1024u);
#else
    pthread_attr_setstacksize(&at, 256ull*1024ull*1024ull);
#endif
    int rc = pthread_create(th, &at, fn, arg);
    pthread_attr_destroy(&at);
    if (rc != 0) rc = pthread_create(th, 0, fn, arg);   /* last resort */
    return rc;
}

static void *guest_thread_main(void *p){
    mem_altstack_install();
    TArg *t = (TArg*)p;
    CPU cpu; memset(&cpu,0,sizeof cpu);
    uint32_t stack_lo = t->stack, stack_hi = stack_lo + GTHREAD_STACK;
    mem_commit(stack_lo, GTHREAD_STACK);
    cpu.esp = stack_hi - 0x1000u;
    cpu.top = 0; cpu.fcw = 0x027f;   /* Windows default: PC=53, RC near, masked */
    cpu.fs_base = t->teb; cpu.gs_base = t->teb;
    memset(g_mem + t->teb, 0, MM_TEB_SIZE);                /* a recycled TEB starts clean (TLS, last error) */
    /* TEB */
    MR32(t->teb+0x00)=0xFFFFFFFFu; MR32(t->teb+0x04)=stack_hi; MR32(t->teb+0x08)=stack_lo;
    MR32(t->teb+0x18)=t->teb; MR32(t->teb+0x24)=t->ob?t->ob->tid:0;
    MR32(t->teb+0x2C)=t->teb+0x1000u; MR32(t->teb+0x30)=MM_PEB;
    /* stdcall: arg, then the return address */
    { CPU *c=&cpu; PUSH32(t->arg); PUSH32(0xDEAD7EADu); }
    __sync_fetch_and_add(&g_threads_started, 1);
    { static int sh=0; if (sh<64){ sh++;                   /* rare: one per race load */
        fprintf(stderr,"[thread] start tid=%u entry=0x%08x arg=0x%08x teb=0x%08x stack=0x%08x from 0x%08x (%llu running)\n",
                t->ob?t->ob->tid:0, t->entry, t->arg, t->teb, stack_lo, t->ob?t->ob->creator:0,
                (unsigned long long)(g_threads_started - g_threads_exited)); } }
    tl_obj = t->ob;
    /* diagnostics: make it recognizable in thread lists (perflog thread.csv, sample) */
    { char nm[32]; snprintf(nm, sizeof nm, "guest %u @%06x", t->ob ? t->ob->tid : 0u, t->entry & 0xFFFFFFu);
      pthread_setname_np(nm); }
    guest_lock(&cpu);
    dispatch_indirect(&cpu, t->entry);
    { static int sh=0; if (sh++ < 64) fprintf(stderr,"[thread] end tid=%u (returned, eax=0x%08x)\n", t->ob?t->ob->tid:0, cpu.eax); }
    thread_finish(t->ob, cpu.eax, &cpu);                /* the entry returned */
    guest_unlock();
    free(t);
    return 0;
}

/* ---- TerminateThread -----------------------------------------------------
 * A pthread cannot be killed safely, so the request is cooperative: the
 * target ends (as ExitThread would) at its next import call.  Only the lifted
 * DLLs get this (server.dll's StopServer, after its 10 s wait); SPEED2.EXE
 * keeps the old no-op, see GetExitCodeThread. */
static _Atomic int g_term_pending = 0;
void win32_thread_term_check(CPU *c){
    (void)c;
    if (__builtin_expect(!g_term_pending, 1)) return;
    Obj *o = (Obj*)tl_obj;
    if (!o || !o->term_req) return;
    g_term_pending--;
    fprintf(stderr, "[thread] tid=%u ends here: TerminateThread(%u) was requested\n", o->tid, o->exit_code);
    thread_finish(o, o->exit_code, c);
    guest_unlock();
    pthread_exit(0);
}
int win32_caller_module(void);
/* Windows' thread exit semantics (STILL_ACTIVE, real exit codes, working
 * TerminateThread) for SPEED2.EXE too: NFSU2_THREAD_EXIT=windows.  Off by
 * default: the exe has always been told that every thread had ended with 0,
 * and its RealCore wait loop (0x6F5A04) spins until that is true. */
static int exit_semantics(void){
    if (win32_caller_module()) return 1;
    static int g = -1;
    if (g < 0){ const char *e = getenv("NFSU2_THREAD_EXIT"); g = e && !strcasecmp(e, "windows"); }
    return g;
}

void win32_signal_event(uint32_t h){
    Obj *o = obj_get(h);
    if (!o || o->kind != OB_EVENT) return;
    pthread_mutex_lock(&o->mtx);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mtx);
    obj_wake();
}

/* ---- waitable timers -----------------------------------------------------
 * CreateWaitableTimerA, SetWaitableTimer (no game imports CancelWaitableTimer):
 * Most Wanted's 100 Hz thread (0x7EC601, GAME.md 6.1) waits on a manual-reset
 * timer that it re-arms with absolute FILETIME due times.  The timers run on
 * the virtual clock (host_us), as the WINMM timer below does, and so does the
 * guest's system time (vsys_ft: GetSystemTime, SystemTimeToFileTime): an
 * absolute due time made from GetSystemTime and a relative one (negative,
 * 100 ns units) land on one scale, and a due time in the past fires at once.
 *
 * Nothing drives a timer.  Whoever looks at it (a wait, SetWaitableTimer)
 * brings it up to date first (timer_poll); a periodic timer's missed periods
 * signal it once, as on Windows.  A waiter releases the guest lock and sleeps
 * on the object's condition variable in slices of at most TIMER_SLICE_US of
 * real time (shorter up to the due time), checking again under the object's
 * lock after each: every wait is finite and none misses a wake-up (L-070).
 * A guest INFINITE wait is such slices until the timer fires.  Completion
 * routines (APCs) are not supported: logged, the timer still signals. */
#define TIMER_SLICE_US 2000u
#define FT_EPOCH 134320464000000000ull   /* 2026-08-24 12:00:00 UTC as a FILETIME: the date GetSystemTime always gave */
#define WAIT_FAILED_ 0xFFFFFFFFu
uint64_t host_us(void);
void win32_set_last_error(CPU *c, uint32_t e);
/* The guest's system time, UTC, in FILETIME units: that date plus the virtual clock. */
static uint64_t vsys_ft(void){ return FT_EPOCH + host_us() * 10ull; }
static uint64_t mono_us(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
/* o->mtx held: signal the timer if it is due by now */
static void timer_poll(Obj *o, uint64_t now){
    if (o->kind != OB_TIMER || !o->t_armed || now < o->t_due_us) return;
    o->signaled = 1;
    if (o->t_period_us) o->t_due_us += o->t_period_us * ((now - o->t_due_us) / o->t_period_us + 1u);
    else o->t_armed = 0;
}
/* how long to sleep before looking at o again (o->mtx held) */
static uint64_t timer_slice_us(const Obj *o, uint64_t now, uint64_t s){
    if (o->kind == OB_TIMER && o->t_armed && o->t_due_us > now && o->t_due_us - now < s) s = o->t_due_us - now;
    return s < 100u ? 100u : s;
}
/* sleep on o's condition variable for at most us microseconds (o->mtx held) */
static void cv_wait_us(Obj *o, uint64_t us){
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(us / 1000000u); ts.tv_nsec += (long)(us % 1000000u) * 1000L;
    if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&o->cv, &o->mtx, &ts);
}
/* WaitForSingleObject on a timer, guest lock released: WAIT_OBJECT_0, WAIT_TIMEOUT
 * after ms of real time, or WAIT_FAILED when the handle is closed meanwhile */
static uint32_t timer_wait(Obj *o, uint32_t ms){
    uint64_t t0 = mono_us(), lim = (uint64_t)ms * 1000u;
    uint32_t r;
    pthread_mutex_lock(&o->mtx);
    for (;;){
        if (o->kind != OB_TIMER){ r = WAIT_FAILED_; break; }
        uint64_t now = host_us();
        timer_poll(o, now);
        if (o->signaled){ if (!o->manual) o->signaled = 0; r = 0; break; }
        uint64_t el = mono_us() - t0;
        if (ms != 0xFFFFFFFFu && el >= lim){ r = 0x102u; break; }
        uint64_t s = timer_slice_us(o, now, TIMER_SLICE_US);
        if (ms != 0xFFFFFFFFu && lim - el < s) s = lim - el;
        cv_wait_us(o, s);
    }
    pthread_mutex_unlock(&o->mtx);
    return r;
}
/* A wait's own reference to a semaphore, taken under the guest lock before
 * the wait gives that away (sem_hold) and let go under it again (sem_drop).
 * On Windows a wait holds on to its object: a CloseHandle meanwhile leaves
 * the semaphore to the wait, which goes on.  So it does here, and the slot,
 * which obj_new would otherwise give the next object at once, stays this
 * semaphore's until its last wait has let go: a waiter never takes a count
 * of a new semaphore made in its place.  The last reference ends it as
 * obj_close does. */
static void sem_hold(Obj *o){
    pthread_mutex_lock(&o->mtx); o->refs++; pthread_mutex_unlock(&o->mtx);
}
static void sem_drop(Obj *o){
    pthread_mutex_lock(&o->mtx);
    int last = --o->refs <= 0;
    if (last){ o->kind = OB_NONE; pthread_cond_broadcast(&o->cv); }
    pthread_mutex_unlock(&o->mtx);
    if (last && !getenv("NFSU2_NO_OBJREUSE")){
        pthread_mutex_lock(&g_lock); g_free[g_nfree++] = (int)(o - g_obj); pthread_mutex_unlock(&g_lock); }
}
/* WaitForSingleObject on a semaphore, guest lock released, the wait's
 * reference held (sem_hold): Windows' rules.  A count above 0 lets one waiter
 * through and goes down by one; WAIT_TIMEOUT after ms of real time (none for
 * INFINITE).  ReleaseSemaphore wakes every waiter, each looks again under the
 * lock, so a wake-up another waiter took, or none at all, is no early
 * return; the sleep is cut into slices of at most SEM_SLICE_US, after each of
 * which the semaphore and the time are looked at again.  (The generic wait
 * below used to take the whole count at once and gave up after one sleep of
 * at most 2 s, INFINITE too.) */
#define SEM_SLICE_US 50000u
static uint32_t sem_wait_one(Obj *o, uint32_t ms){
    uint64_t t0 = mono_us(), lim = (uint64_t)ms * 1000u;
    uint32_t r;
    pthread_mutex_lock(&o->mtx);
    for (;;){
        if (o->kind != OB_SEM){ r = WAIT_FAILED_; break; }         /* never while the wait's reference is held */
        if (o->signaled > 0){ o->signaled--; r = 0; break; }
        uint64_t el = mono_us() - t0;
        if (ms != 0xFFFFFFFFu && el >= lim){ r = 0x102u; break; }
        uint64_t s = SEM_SLICE_US;
        if (ms != 0xFFFFFFFFu && lim - el < s) s = lim - el;
        cv_wait_us(o, s);
    }
    pthread_mutex_unlock(&o->mtx);
    return r;
}
/* WaitForMultipleObjects over a set that holds a waitable timer or a
 * semaphore.  Only such sets come here; the others keep the polling loop of
 * win32_ext_call as it always was.  Before the first CreateWaitableTimerA or
 * CreateSemaphoreA no set is even looked at (g_timers_made, g_sems_made): UG2
 * imports neither, so its waits run as they always did.
 * Windows' rules: any (the lowest index that is ready) or all at once,
 * WAIT_TIMEOUT after ms of real time, WAIT_FAILED for a handle that is no
 * object, a repeated one in a wait for all, or one closed meanwhile (not a
 * semaphore: the wait holds on to those, sem_hold).  Every
 * object is looked at under its own lock (all of them, in address order, for
 * a wait for all), then the thread sleeps at most 1 ms, less up to a timer's
 * due time.  Guest lock released. */
static uint32_t g_timers_made;                   /* CreateWaitableTimerA's, under the guest lock */
static uint32_t g_sems_made;                     /* CreateSemaphoreA's, under the guest lock */
static int wfmo_has_timer(uint32_t cnt, uint32_t parr){
    if (!g_timers_made && !g_sems_made) return 0;
    if (!parr || cnt == 0 || cnt > 64u || !mem_is_committed(parr) || !mem_is_committed(parr + 4u * cnt - 1u)) return 0;
    for (uint32_t i = 0; i < cnt; i++){ Obj *o = obj_get(MR32(parr + 4u * i)); if (o && (o->kind == OB_TIMER || o->kind == OB_SEM)) return 1; }
    return 0;
}
static int obj_ready(const Obj *o, uint32_t me){
    if (o->kind == OB_MUTEX) return o->signaled || (o->owner_tid == me && o->rec > 0);
    return o->signaled > 0;
}
static void obj_take(Obj *o, uint32_t me){
    if (o->kind == OB_MUTEX){ if (o->owner_tid == me && o->rec > 0) o->rec++; else { o->signaled = 0; o->owner_tid = me; o->rec = 1; } }
    else if (o->kind == OB_SEM) o->signaled--;
    else if (!o->manual) o->signaled = 0;                /* auto-reset event or timer; a thread stays signaled */
}
static int obj_addr_cmp(const void *a, const void *b){
    uintptr_t x = (uintptr_t)*(Obj *const *)a, y = (uintptr_t)*(Obj *const *)b;
    return (x > y) - (x < y);
}
/* One look at a wait set, each object under its own lock: any: the lowest
 * index that is ready, taken (WAIT_OBJECT_0 + i); all: every object ready at
 * once, all taken (WAIT_OBJECT_0); else WAIT_TIMEOUT, and *s (us) is lowered
 * to the nearest timer's due time.  WAIT_FAILED with *err 6
 * (ERROR_INVALID_HANDLE) when an object was closed since the wait began: its
 * slot is free or holds another object (ser: each object's serial then).  lk:
 * the same objects in address order, the order a wait for all locks them in. */
static uint32_t wait_try(Obj *const *ob, Obj *const *lk, const uint32_t *ser, uint32_t cnt, int all, uint32_t me,
                         uint32_t *err, uint64_t *s){
    uint64_t now = host_us();
    if (!all){
        for (uint32_t i = 0; i < cnt; i++){
            Obj *o = ob[i];
            pthread_mutex_lock(&o->mtx);
            if (o->kind == OB_NONE || o->serial != ser[i]){ pthread_mutex_unlock(&o->mtx); *err = 6u; return WAIT_FAILED_; }
            timer_poll(o, now);
            if (obj_ready(o, me)){ obj_take(o, me); pthread_mutex_unlock(&o->mtx); return i; }
            *s = timer_slice_us(o, now, *s);
            pthread_mutex_unlock(&o->mtx);
        }
        return 0x102u;
    }
    int ready = 1, gone = 0;
    for (uint32_t i = 0; i < cnt; i++) pthread_mutex_lock(&lk[i]->mtx);
    for (uint32_t i = 0; i < cnt; i++){
        Obj *o = ob[i];
        if (o->kind == OB_NONE || o->serial != ser[i]){ gone = 1; break; }
        timer_poll(o, now);
        if (!obj_ready(o, me)){ ready = 0; *s = timer_slice_us(o, now, *s); }
    }
    if (ready && !gone) for (uint32_t i = 0; i < cnt; i++) obj_take(ob[i], me);
    for (uint32_t i = cnt; i-- > 0; ) pthread_mutex_unlock(&lk[i]->mtx);
    if (gone){ *err = 6u; return WAIT_FAILED_; }
    return ready ? 0u : 0x102u;
}
/* a wait for all: the objects in locking order; 0, or 87 (ERROR_INVALID_PARAMETER) for one named twice */
static uint32_t wait_order(Obj *const *ob, Obj **lk, uint32_t cnt){
    memcpy(lk, ob, cnt * sizeof *lk); qsort(lk, cnt, sizeof *lk, obj_addr_cmp);
    for (uint32_t i = 1; i < cnt; i++) if (lk[i] == lk[i - 1]) return 87u;
    return 0;
}
/* ser: the objects' serials, read when the handles were looked up (guest lock held) */
static uint32_t wait_set(Obj **ob, const uint32_t *ser, uint32_t cnt, int all, uint32_t ms, uint32_t me, uint32_t *err){
    Obj *lk[64];
    if (all && (*err = wait_order(ob, lk, cnt)) != 0) return WAIT_FAILED_;
    uint64_t t0 = mono_us(), lim = (uint64_t)ms * 1000u;
    for (;;){
        uint64_t s = 1000u;
        uint32_t r = wait_try(ob, lk, ser, cnt, all, me, err, &s);
        if (r != 0x102u) return r;
        uint64_t el = mono_us() - t0;
        if (ms != 0xFFFFFFFFu && el >= lim) return 0x102u;
        if (ms != 0xFFFFFFFFu && lim - el < s) s = lim - el;
        struct timespec ts = { (time_t)(s / 1000000u), (long)(s % 1000000u) * 1000L };
        nanosleep(&ts, 0);
    }
}

/* ---- kernel objects for winsock.c ------------------------------------------
 * WSACreateEvent, WSASetEvent, WSAResetEvent and WSACloseEvent are this
 * table's events (ws2_32 makes them with CreateEvent: manual reset, not
 * signaled), and WSAWaitForMultipleEvents takes any of its objects, as
 * WaitForMultipleObjectsEx does: Most Wanted's socket thread waits on an
 * event of CreateEventA together with WSACreateEvent's (GAME.md 3.3).  The
 * wait itself is winsock.c's (it completes overlapped receives between its
 * looks); these are its looks, which need no guest lock. */
uint32_t win32_event_create(int manual, int initial){
    uint32_t h = obj_new(OB_EVENT); Obj *o = obj_get(h);
    if (!o) return 0;
    pthread_mutex_lock(&o->mtx); o->manual = manual != 0; o->signaled = initial != 0; pthread_mutex_unlock(&o->mtx);
    return h;
}
/* SetEvent (on 1) / ResetEvent (0) of an event; 0 when h names no event */
int win32_event_set(uint32_t h, int on){
    Obj *o = obj_get(h);
    if (!o) return 0;
    pthread_mutex_lock(&o->mtx);
    int ok = o->kind == OB_EVENT;
    if (ok){ o->signaled = on != 0; if (on) pthread_cond_broadcast(&o->cv); }
    pthread_mutex_unlock(&o->mtx);
    if (ok && on) obj_wake();
    return ok;
}
/* CloseHandle of an event; 0 when h names no event */
int win32_event_close(uint32_t h){
    Obj *o = obj_get(h);
    if (!o || o->kind != OB_EVENT) return 0;
    obj_close(h);
    return 1;
}
/* the object h names now (its serial), 0 for none */
uint32_t win32_obj_serial(uint32_t h){
    Obj *o = obj_get(h);
    if (!o) return 0;
    pthread_mutex_lock(&o->mtx); uint32_t s = o->kind != OB_NONE ? o->serial : 0u; pthread_mutex_unlock(&o->mtx);
    return s;
}
/* sets the event h only while it is still the object with serial ser: an
 * overlapped receive signals the event it began with, never one that took
 * over its handle after a WSACloseEvent (Windows holds the object itself) */
void win32_event_signal_obj(uint32_t h, uint32_t ser){
    int i = obj_slot(h);
    if (i < 0 || !ser) return;
    Obj *o = &g_obj[i];
    pthread_mutex_lock(&o->mtx);
    int ok = o->kind == OB_EVENT && o->serial == ser;
    if (ok){ o->signaled = 1; pthread_cond_broadcast(&o->cv); }
    pthread_mutex_unlock(&o->mtx);
    if (ok) obj_wake();
}
/* A wait's handles, looked up once (guest lock held): each object's serial,
 * and whether all are events (every way an event is set or closed calls
 * winsock_wake; the other kinds are looked at in short slices).  0, or 6
 * (ERROR_INVALID_HANDLE: one names no object) or 87 (ERROR_INVALID_PARAMETER:
 * one named twice in a wait for all). */
uint32_t win32_wait_ids(const uint32_t *h, uint32_t cnt, int all, uint32_t *ser, int *events_only){
    Obj *ob[64], *lk[64];
    if (!cnt || cnt > 64u) return 87u;
    *events_only = 1;
    for (uint32_t i = 0; i < cnt; i++){
        if (!(ob[i] = obj_get(h[i]))) return 6u;
        pthread_mutex_lock(&ob[i]->mtx);
        ser[i] = ob[i]->kind != OB_NONE ? ob[i]->serial : 0u;
        if (ob[i]->kind != OB_EVENT) *events_only = 0;
        pthread_mutex_unlock(&ob[i]->mtx);
        if (!ser[i]) return 6u;
    }
    return all ? wait_order(ob, lk, cnt) : 0u;
}
/* one look (wait_try) at handles whose serials win32_wait_ids gave; no guest lock needed.  *s (us): how long the
 * caller may sleep before the next look, lowered to the nearest timer's due time */
uint32_t win32_wait_try(const uint32_t *h, const uint32_t *ser, uint32_t cnt, int all, uint32_t me, uint32_t *err, uint64_t *s){
    Obj *ob[64], *lk[64];
    for (uint32_t i = 0; i < cnt; i++){
        int k = obj_slot(h[i]);
        if (k < 0){ *err = 6u; return WAIT_FAILED_; }        /* the slot went to another generation (obj_reclaim) */
        ob[i] = &g_obj[k];
    }
    if (all && wait_order(ob, lk, cnt)){ *err = 87u; return WAIT_FAILED_; }
    return wait_try(ob, lk, ser, cnt, all, me, err, s);
}
uint32_t win32_thread_id(CPU *c){ return cur_tid(c); }
/* SYSTEMTIME (16 bytes at p) of a FILETIME */
static void ft_to_systime(uint32_t p, uint64_t ft){
    uint64_t ms = ft / 10000ull;
    time_t t = (time_t)(ms / 1000ull) - (time_t)11644473600LL;
    struct tm g; gmtime_r(&t, &g);
    mem_commit(p, 16);
    MR16(p+0)=(uint16_t)(g.tm_year+1900); MR16(p+2)=(uint16_t)(g.tm_mon+1); MR16(p+4)=(uint16_t)g.tm_wday;
    MR16(p+6)=(uint16_t)g.tm_mday; MR16(p+8)=(uint16_t)g.tm_hour; MR16(p+10)=(uint16_t)g.tm_min;
    MR16(p+12)=(uint16_t)g.tm_sec; MR16(p+14)=(uint16_t)(ms % 1000ull);
}
/* SystemTimeToFileTime: 0 for a SYSTEMTIME Windows refuses (wDayOfWeek is not looked at) */
static int systime_to_ft(uint32_t p, uint64_t *ft){
    int y = MR16(p+0), mo = MR16(p+2), d = MR16(p+6), h = MR16(p+8), mi = MR16(p+10), s = MR16(p+12), ms = MR16(p+14);
    static const uint8_t dim[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (y < 1601 || y > 30827 || mo < 1 || mo > 12 || d < 1 || d > dim[mo-1] + (mo == 2 && leap) ||
        h > 23 || mi > 59 || s > 59 || ms > 999) return 0;
    /* days since 1601-01-01: whole 400-year cycles of 146,097 days from March 1600 */
    int yy = y - (mo <= 2), era = yy / 400, yoe = yy - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 584694;   /* 1601-01-01 = 0 */
    *ft = ((uint64_t)days * 86400ull + (uint64_t)(h * 3600 + mi * 60 + s)) * 10000000ull + (uint64_t)ms * 10000ull;
    return 1;
}

/* ---- WINMM multimedia timer --------------------------------------------
 * NFSU2 drives its front-end update loop from the timeSetEvent callback;
 * with a stub the menu animations do not advance and input is not
 * processed.  The game re-arms its one-shot timer inside the callback, so a
 * thread per timer is not an option: ONE timer thread processes all the
 * entries in turn.                                                       */
#define MAXTIMER 32
typedef struct {
    uint32_t proc, user, id;
    uint64_t due_us;          /* 0 = free */
    uint32_t period_us;       /* 0 = one-shot */
} Timer;

static Timer    g_tm[MAXTIMER];
static uint32_t g_timer_next_id = 1;
static int      g_timer_running = 0;
static uint32_t g_timer_stack = 0;      /* taken by timeSetEvent before the thread starts */
uint64_t        g_timer_ticks = 0;

/* Timers must use GAME time too; otherwise, when the emulator is slow, the
 * callbacks run far too often relative to the game's own time. */
uint64_t host_us(void);
static uint64_t now_us(void){ return host_us(); }

static void *timer_thread_main(void *p){
    (void)p;
    mem_altstack_install();
    CPU cpu; memset(&cpu,0,sizeof cpu);
    uint32_t stack_lo, stack_hi, teb;
    pthread_mutex_lock(&g_lock);
    stack_lo = g_timer_stack; stack_hi = stack_lo + GTHREAD_STACK;
    teb = TEB_TIMER;
    pthread_mutex_unlock(&g_lock);
    mem_commit(stack_lo, GTHREAD_STACK);
    mem_commit(teb, MM_TEB_SIZE);
    cpu.top = 0; cpu.fcw = 0x027f;   /* Windows default: PC=53, RC near, masked */
    cpu.fs_base = teb; cpu.gs_base = teb;
    MR32(teb+0x00)=0xFFFFFFFFu; MR32(teb+0x04)=stack_hi; MR32(teb+0x08)=stack_lo;
    MR32(teb+0x18)=teb; MR32(teb+0x24)=999u;
    MR32(teb+0x2C)=teb+0x1000u; MR32(teb+0x30)=MM_PEB;

    for (;;){
        struct timespec ts = {0, 1000000L};      /* 1 ms */
        nanosleep(&ts, 0);
        uint64_t now = now_us();
        for (int i = 0; i < MAXTIMER; i++){
            uint32_t proc, user, id;
            pthread_mutex_lock(&g_lock);
            if (!g_tm[i].due_us || g_tm[i].due_us > now){ pthread_mutex_unlock(&g_lock); continue; }
            proc = g_tm[i].proc; user = g_tm[i].user; id = g_tm[i].id;
            if (g_tm[i].period_us) g_tm[i].due_us = now + g_tm[i].period_us;
            else                   g_tm[i].due_us = 0;      /* one-shot: done */
            pthread_mutex_unlock(&g_lock);

            cpu.esp = stack_hi - 0x1000u;
            { CPU *c=&cpu;
              /* stdcall: dw2, dw1, dwUser, uMsg, uTimerID  (right to left) */
              PUSH32(0u); PUSH32(0u); PUSH32(user); PUSH32(0u); PUSH32(id);
              PUSH32(0xDEAD7EADu); }
            guest_lock(&cpu);
            dispatch_indirect(&cpu, proc);
            guest_unlock();
            g_timer_ticks++;
        }
    }
    return 0;
}

/* ---- registry: in memory, "not found" by default ---- */
/* If the game cannot find a value it uses its own default - which is what we want. */
#define ERROR_SUCCESS         0u
#define ERROR_FILE_NOT_FOUND  2u


/* --- guest printf (wvsprintfA / wsprintfA): %[-0+ #][width][.prec][hlI]
 * d i u x X o c s % ; arguments are 32-bit stack slots (va_list = guest ptr). */
static uint32_t guest_vformat(uint32_t out, uint32_t cap, const char *fmt, uint32_t va){
    char tmp[1100]; uint32_t n = 0;
    for (const char *p = fmt; *p && n < sizeof tmp - 64; p++){
        if (*p != '%'){ tmp[n++] = *p; continue; }
        char spec[32]; int k = 0; spec[k++] = '%'; p++;
        while (*p && strchr("-+ #0", *p) && k < 20) spec[k++] = *p++;
        while (*p >= '0' && *p <= '9' && k < 24) spec[k++] = *p++;
        if (*p == '.'){ spec[k++] = *p++; while (*p >= '0' && *p <= '9' && k < 28) spec[k++] = *p++; }
        while (*p == 'l' || *p == 'h' || *p == 'I') p++;
        char cv = *p; if (!cv) break;
        spec[k++] = cv; spec[k] = 0;
        int w = 0;
        if (cv == '%'){ tmp[n++] = '%'; continue; }
        uint32_t a = mem_is_committed(va + 3) ? MR32(va) : 0u; va += 4;
        if (cv == 's'){
            const char *sv = (a && mem_is_committed(a)) ? (const char *)(g_mem + a) : "(null)";
            char sv2[512]; snprintf(sv2, sizeof sv2, "%s", sv);
            w = snprintf(tmp + n, sizeof tmp - n, spec, sv2);
        } else if (cv == 'c') w = snprintf(tmp + n, sizeof tmp - n, spec, (int)(a & 0xFF));
        else if (cv == 'd' || cv == 'i') w = snprintf(tmp + n, sizeof tmp - n, spec, (int)(int32_t)a);
        else if (cv == 'u' || cv == 'x' || cv == 'X' || cv == 'o') w = snprintf(tmp + n, sizeof tmp - n, spec, (unsigned)a);
        else { tmp[n++] = '%'; tmp[n++] = cv; continue; }
        if (w > 0) n += (uint32_t)w < sizeof tmp - n ? (uint32_t)w : (uint32_t)(sizeof tmp - n - 1);
    }
    tmp[n] = 0;
    if (n >= cap) n = cap ? cap - 1 : 0;     /* wsprintf: at most 1024 chars */
    mem_commit(out, n + 1); memcpy(g_mem + out, tmp, n); MR8(out + n) = 0;
    return n;
}
/* ---- exported API ---- */
static uint32_t A(CPU *c, int i){ return MR32(c->esp + 4u*(uint32_t)i); }

/* --- simple persistent registry ----------------------------------------
 * The settings the game writes (graphics detail, resolution, sound) used to
 * be thrown away.  Here they are kept and written to ~/.nfsu2_registry (the
 * game's own file, game.h reg_file: Most Wanted's is ~/.nfsmw_registry). */
/* 512: DirectInput user maps (dinput_kbd.c) add an entry per rebound key */
#define MAXREG 512
static struct { char name[64]; int is_str; uint32_t dw; char sv[128]; } g_reg[MAXREG];
static int g_regn = 0;
static int g_reg_loaded = 0;

static const char *reg_path(void){
    static char pth[512];
    /* NFSU2_REGFILE=<path>: lets test runs use their own registry file */
    const char *rf = getenv("NFSU2_REGFILE");
    if (rf && *rf){ snprintf(pth, sizeof pth, "%s", rf); return pth; }
    const char *h = getenv("HOME");
    snprintf(pth, sizeof pth, "%s/%s", h ? h : ".", g_game.reg_file ? g_game.reg_file : ".nfsu2_registry");
    return pth;
}
static void reg_load(void){
    if (g_reg_loaded) return;
    g_reg_loaded = 1;
    FILE *f = fopen(reg_path(), "r");
    fprintf(stderr, "  [registry] registry file %s%s\n", reg_path(), f ? "" : " (new)");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && g_regn < MAXREG){
        char nm[64], val[128]; int isstr = 0; unsigned dw = 0;
        if (sscanf(line, "d %63[^=]=%u", nm, &dw) == 2){
            snprintf(g_reg[g_regn].name, 64, "%s", nm);
            g_reg[g_regn].is_str = 0; g_reg[g_regn].dw = dw; g_regn++;
        } else if (sscanf(line, "s %63[^=]=%127[^\n]", nm, val) == 2){
            snprintf(g_reg[g_regn].name, 64, "%s", nm);
            snprintf(g_reg[g_regn].sv, 128, "%s", val);
            g_reg[g_regn].is_str = 1; g_regn++;
        }
        (void)isstr;
    }
    fclose(f);
}
/* Written to "<file>.tmp" and renamed over the old file, so quitting (or a
 * crash) in the middle of the game's 29 back-to-back RegSetValueExA calls
 * can never leave a truncated registry behind. */
static void reg_save(void){
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", reg_path());
    FILE *f = fopen(tmp, "w");
    if (!f){ static int said = 0;
        if (!said++) fprintf(stderr, "  [registry] cannot write %s\n", tmp);
        return; }
    for (int i = 0; i < g_regn; i++){
        if (g_reg[i].is_str) fprintf(f, "s %s=%s\n", g_reg[i].name, g_reg[i].sv);
        else                 fprintf(f, "d %s=%u\n", g_reg[i].name, g_reg[i].dw);
    }
    if (fclose(f) == 0) rename(tmp, reg_path());
    else unlink(tmp);
}
static int reg_find(const char *nm){
    reg_load();
    for (int i = 0; i < g_regn; i++)
        if (!strcasecmp(g_reg[i].name, nm)) return i;
    return -1;
}
static void reg_put_dword(const char *nm, uint32_t v){
    int i = reg_find(nm);
    if (i < 0){ if (g_regn >= MAXREG) return; i = g_regn++;
                snprintf(g_reg[i].name, 64, "%s", nm); }
    g_reg[i].is_str = 0; g_reg[i].dw = v; reg_save();
}
static void reg_set_string(const char *nm, const char *v, int save){
    int i = reg_find(nm);
    if (i < 0){ if (g_regn >= MAXREG) return; i = g_regn++;
                snprintf(g_reg[i].name, 64, "%s", nm); }
    g_reg[i].is_str = 1; snprintf(g_reg[i].sv, 128, "%s", v);
    if (save) reg_save();
}
static void reg_put_string(const char *nm, const char *v){ reg_set_string(nm, v, 1); }
/* For the DirectInput user maps (dinput_kbd.c): set/delete without writing
 * the file each time, then one win32_reg_flush(). */
int win32_reg_get_str(const char *nm, char *out, unsigned cap){
    int i = reg_find(nm);
    if (i < 0 || !g_reg[i].is_str) return 0;
    snprintf(out, cap, "%s", g_reg[i].sv);
    return 1;
}
void win32_reg_set_str(const char *nm, const char *v){ reg_set_string(nm, v, 0); }
void win32_reg_del(const char *nm){
    int i = reg_find(nm);
    if (i < 0) return;
    memmove(&g_reg[i], &g_reg[i + 1], (size_t)(g_regn - i - 1) * sizeof g_reg[0]);
    g_regn--;
}
void win32_reg_flush(void){ reg_load(); reg_save(); }
/* A DWORD value of our own (hornmenu.c's HornVolume): read, and set with
 * the file written at once. */
int win32_reg_get_dword(const char *nm, uint32_t *out){
    int i = reg_find(nm);
    if (i < 0 || g_reg[i].is_str) return 0;
    *out = g_reg[i].dw;
    return 1;
}
void win32_reg_put_dword(const char *nm, uint32_t v){ reg_put_dword(nm, v); }

/* The CPU's clock as the registry's ~MHz gives it: the speed rdtsc counts
 * at (dispatch.c), NFSU2_TSC_MHZ or 3,000. */
uint32_t win32_cpu_mhz(void){
    const char *e = getenv("NFSU2_TSC_MHZ");
    return e && *e ? (uint32_t)strtoul(e,0,0) : 3000u;
}

/* RegQueryValueExA's answer, a REG_SZ (sval) or a REG_DWORD (have_d): the
 * type, the size, the data when it fits (ERROR_MORE_DATA 234 when not), or
 * ERROR_FILE_NOT_FOUND for neither */
static uint32_t reg_answer(uint32_t ptype, uint32_t pdata, uint32_t pcb, const char *sval, uint32_t dval, int have_d){
    uint32_t r;
    if (sval){
        uint32_t need = (uint32_t)strlen(sval) + 1u;
        if (ptype){ mem_commit(ptype,4); MR32(ptype)=1u; }      /* REG_SZ */
        if (!pdata){ if(pcb){ mem_commit(pcb,4); MR32(pcb)=need; } r=ERROR_SUCCESS; }
        else {
            uint32_t cap = pcb ? MR32(pcb) : need;
            if (cap < need){ if(pcb) MR32(pcb)=need; r=234u; } /* ERROR_MORE_DATA */
            else { mem_commit(pdata,need); memcpy(g_mem+pdata, sval, need);
                   if(pcb) MR32(pcb)=need; r=ERROR_SUCCESS; }
        }
    } else if (have_d){
        if (ptype){ mem_commit(ptype,4); MR32(ptype)=4u; }      /* REG_DWORD */
        if (!pdata){ if(pcb){ mem_commit(pcb,4); MR32(pcb)=4u; } r=ERROR_SUCCESS; }
        else {
            uint32_t cap = pcb ? MR32(pcb) : 4u;
            if (cap < 4u){ if(pcb) MR32(pcb)=4u; r=234u; }
            else { mem_commit(pdata,4); MR32(pdata)=dval;
                   if(pcb) MR32(pcb)=4u; r=ERROR_SUCCESS; }
        }
    } else {
        if(pcb){ mem_commit(pcb,4); MR32(pcb)=0; }
        r=ERROR_FILE_NOT_FOUND;
    }
    return r;
}

/* ---- the game's registry keys (game.h reg_keys: Most Wanted's) ----------
 * The registry as the game's machine has it: the keys of the table and no
 * others, each answering for its own values; what the game writes is kept in
 * the registry file (above) when its key keeps values (store).  As Windows:
 * a key the table lacks does not open (ERROR_FILE_NOT_FOUND) and cannot be
 * made (our machine adds no keys: ERROR_ACCESS_DENIED), a handle opened
 * without KEY_SET_VALUE cannot write (L-056) and one without KEY_QUERY_VALUE
 * cannot read, a value nothing stored and the table does not name is not
 * there.  Every call logs one "[reg]" line with what it answered.  UG2 has no
 * key table: its registry is the flat one of win32_ext_call, as always. */
#define ERROR_ACCESS_DENIED   5u
#define ERROR_INVALID_HANDLE  6u
#define KEY_QUERY_VALUE 0x0001u
#define KEY_SET_VALUE   0x0002u
#define KEY_ALL_ACCESS  0x000F003Fu
static unsigned g_reglog_n;                    /* "[reg]" lines so far: the first 2,000 are written */
#define REGLOG(...) do { if (g_reglog_n++ < 2000u) fprintf(stderr, __VA_ARGS__); } while (0)
/* the predefined keys' short names, 0 for any other handle */
static const char *reg_root_name(uint32_t h){
    switch (h){
    case 0x80000000u: return "HKCR"; case 0x80000001u: return "HKCU"; case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";  case 0x80000004u: return "HKPD"; case 0x80000005u: return "HKCC";
    case 0x80000006u: return "HKDD";
    default: return 0;
    }
}
/* two key paths name the same key: part by part without case, extra backslashes ignored */
static int reg_path_eq(const char *a, const char *b){
    for (;;){
        while (*a == '\\') a++;
        while (*b == '\\') b++;
        if (!*a || !*b) return !*a && !*b;
        for (; *a && *a != '\\' && *b && *b != '\\'; a++, b++)
            if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        if ((*a && *a != '\\') || (*b && *b != '\\')) return 0;
    }
}
/* a value as the log shows it: REG_SZ quoted, bytes below 0x20 as \xNN */
static const char *reg_show(const char *sval, uint32_t dval, int have_d, char *out, size_t n){
    if (have_d){ snprintf(out, n, "%u", dval); return out; }
    if (!sval){ snprintf(out, n, "-"); return out; }
    size_t k = 0; out[k++] = '"';
    for (const unsigned char *s = (const unsigned char *)sval; *s && k + 6 < n; s++){
        if (*s < 0x20) k += (size_t)snprintf(out + k, n - k, "\\x%02x", *s);
        else out[k++] = (char)*s;
    }
    out[k++] = '"'; out[k] = 0;
    return out;
}
/* the table key a handle names (0: none) */
static const RegKey *reg_key_of(uint32_t h, Obj **po){
    Obj *o = obj_get(h);
    if (po) *po = o;
    return o && o->kind == OB_KEY && o->reg_key ? &g_game.reg_keys[o->reg_key - 1] : 0;
}
/* RegOpenKeyA, RegOpenKeyExA, RegCreateKeyA, RegCreateKeyExA */
static uint32_t reg_open(CPU *c, const char *n){
    int ex = !strcmp(n, "RegOpenKeyExA"), cex = !strcmp(n, "RegCreateKeyExA"), create = !strncmp(n, "RegCreate", 9);
    uint32_t hk = A(c,0), psub = A(c,1);
    uint32_t access = ex ? A(c,3) : cex ? A(c,5) : KEY_ALL_ACCESS;
    uint32_t phk = ex ? A(c,4) : cex ? A(c,7) : A(c,2);
    const char *sub = psub && mem_is_committed(psub) ? (const char *)(g_mem + psub) : "";
    char path[400]; uint32_t root; const RegKey *pk = reg_key_of(hk, 0);
    if (reg_root_name(hk)){ root = hk; snprintf(path, sizeof path, "%s", sub); }
    else if (pk){ root = pk->root; snprintf(path, sizeof path, "%s\\%s", pk->path, sub); }
    else {
        REGLOG("[reg] %s(0x%08x, \"%s\"): not a key, error 6 (from 0x%08x)\n", n, hk, sub, c->eip);
        if (phk){ mem_commit(phk,4); MR32(phk) = 0; }
        return ERROR_INVALID_HANDLE;
    }
    if (reg_root_name(hk) && !*sub){                    /* the predefined key itself */
        if (phk){ mem_commit(phk,4); MR32(phk) = hk; }
        if (cex && A(c,8)){ mem_commit(A(c,8),4); MR32(A(c,8)) = 2u; }  /* REG_OPENED_EXISTING_KEY */
        return ERROR_SUCCESS;
    }
    const char *rn = reg_root_name(root);
    int k = 0;
    for (int i = 0; g_game.reg_keys[i].path; i++)
        if (g_game.reg_keys[i].root == root && reg_path_eq(g_game.reg_keys[i].path, path)){ k = i + 1; break; }
    if (!k){
        REGLOG("[reg] %s(%s\\%s): %s (from 0x%08x)\n", n, rn, path,
               create ? "this machine makes no keys of its own, error 5" : "no such key, error 2", c->eip);
        if (phk){ mem_commit(phk,4); MR32(phk) = 0; }
        return create ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
    }
    uint32_t h = obj_new(OB_KEY); Obj *o = obj_get(h);
    if (!o){ if (phk){ mem_commit(phk,4); MR32(phk) = 0; } return 14u; }   /* ERROR_OUTOFMEMORY */
    o->reg_key = k; o->reg_access = access;
    if (phk){ mem_commit(phk,4); MR32(phk) = h; }
    if (cex && A(c,8)){ mem_commit(A(c,8),4); MR32(A(c,8)) = 2u; }      /* REG_OPENED_EXISTING_KEY */
    REGLOG("[reg] %s(%s\\%s, access 0x%x) -> 0x%08x (from 0x%08x)\n", n, rn, g_game.reg_keys[k - 1].path,
           access, h, c->eip);
    return ERROR_SUCCESS;
}
/* RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData) */
static uint32_t reg_query(CPU *c){
    uint32_t hk = A(c,0), pname = A(c,1), ptype = A(c,3), pdata = A(c,4), pcb = A(c,5);
    const char *vn = pname && mem_is_committed(pname) ? (const char *)(g_mem + pname) : "";
    Obj *o; const RegKey *k = reg_key_of(hk, &o);
    if (!k){
        uint32_t e = reg_root_name(hk) ? ERROR_FILE_NOT_FOUND : ERROR_INVALID_HANDLE;
        REGLOG("[reg] RegQueryValueExA(0x%08x, \"%s\"): %s, error %u (from 0x%08x)\n", hk, vn,
               e == ERROR_INVALID_HANDLE ? "not a key" : "no such value", e, c->eip);
        if (pcb){ mem_commit(pcb,4); MR32(pcb) = 0; }
        return e;
    }
    if (!(o->reg_access & KEY_QUERY_VALUE)){
        REGLOG("[reg] RegQueryValueExA(%s\\%s): opened without KEY_QUERY_VALUE (0x%x), error 5 (from 0x%08x)\n",
               k->path, vn, o->reg_access, c->eip);
        return ERROR_ACCESS_DENIED;
    }
    const RegDefault *d = 0;
    for (const RegDefault *v = k->values; v && v->name; v++) if (!strcasecmp(v->name, vn)){ d = v; break; }
    const char *sval = 0, *from = "not there"; uint32_t dval = 0; int have_d = 0, ri;
    if (d && (d->str_fn || d->dword_fn)){
        if (d->str_fn) sval = d->str_fn(); else { dval = d->dword_fn(); have_d = 1; }
        from = "the runtime's";
    } else if (k->store && *vn && (ri = reg_find(vn)) >= 0){
        if (g_reg[ri].is_str) sval = g_reg[ri].sv; else { dval = g_reg[ri].dw; have_d = 1; }
        from = "stored";
    } else if (d){
        if (d->str) sval = d->str; else { dval = d->dword; have_d = 1; }
        from = "the default";
    }
    uint32_t r = reg_answer(ptype, pdata, pcb, sval, dval, have_d);
    char sh[200];
    if (sval || have_d)
        REGLOG("[reg] %s\\%s = %s (%s, %s)%s (from 0x%08x)\n", k->path, vn, reg_show(sval, dval, have_d, sh, sizeof sh),
               have_d ? "REG_DWORD" : "REG_SZ", from, r == 234u ? ": too long for its buffer, error 234" : "", c->eip);
    else REGLOG("[reg] %s\\%s: not there, error 2 (from 0x%08x)\n", k->path, vn, c->eip);
    return r;
}
/* RegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData) */
static uint32_t reg_set(CPU *c){
    uint32_t hk = A(c,0), pname = A(c,1), type = A(c,3), pdata = A(c,4), cb = A(c,5);
    const char *vn = pname && mem_is_committed(pname) ? (const char *)(g_mem + pname) : "";
    Obj *o; const RegKey *k = reg_key_of(hk, &o);
    const char *why = 0; uint32_t e = ERROR_ACCESS_DENIED;
    if (!k){ why = "not a key"; e = ERROR_INVALID_HANDLE; }
    else if (!(o->reg_access & KEY_SET_VALUE)) why = "opened without KEY_SET_VALUE";
    else if (!k->store) why = "this key keeps nothing";
    else if (!*vn) why = "the key's default value is not kept";
    else if (type == 4u){
        if (cb < 4u || !pdata || !mem_is_committed(pdata) || !mem_is_committed(pdata + 3u)){ why = "no 4 bytes of data"; e = 87u; }
    } else if (type == 1u){
        if (cb && (!pdata || !mem_is_committed(pdata) || !mem_is_committed(pdata + cb - 1u))){ why = "no data"; e = 87u; }
    } else { why = "only REG_SZ and REG_DWORD are kept"; e = 50u; }               /* ERROR_NOT_SUPPORTED */
    if (why){
        REGLOG("[reg] RegSetValueExA(%s\\%s, type %u, %u bytes): %s, error %u (from 0x%08x)\n",
               k ? k->path : "?", vn, type, cb, why, e, c->eip);
        return e;
    }
    char sv[128], sh[200];
    if (type == 4u){
        reg_put_dword(vn, MR32(pdata));
        REGLOG("[reg] %s\\%s := %s (REG_DWORD) (from 0x%08x)\n", k->path, vn, reg_show(0, MR32(pdata), 1, sh, sizeof sh), c->eip);
    } else {                                     /* REG_SZ: its bytes up to the first NUL, at most 127 */
        uint32_t l = 0;
        while (l < cb && l < sizeof sv - 1 && MR8(pdata + l)) { sv[l] = (char)MR8(pdata + l); l++; }
        sv[l] = 0;
        reg_put_string(vn, sv);
        REGLOG("[reg] %s\\%s := %s (REG_SZ, %u bytes) (from 0x%08x)\n", k->path, vn, reg_show(sv, 0, 0, sh, sizeof sh), cb, c->eip);
    }
    return ERROR_SUCCESS;
}

/* GetTimeZoneInformation: the host's real zone.  TIME_ZONE_INFORMATION
 * (172 bytes): Bias@0, StandardName[32 WCHAR]@4, StandardDate@68,
 * StandardBias@84, DaylightName@88, DaylightDate@152, DaylightBias@168.
 * Transition dates are this year's, as absolute dates (wYear != 0), which
 * the CRT's tzset/_isindst accept; found by scanning the year hour by hour. */
static void put_systime(uint32_t p, time_t t){
    struct tm g; gmtime_r(&t, &g);
    MR16(p+0)=(uint16_t)(g.tm_year+1900); MR16(p+2)=(uint16_t)(g.tm_mon+1); MR16(p+4)=(uint16_t)g.tm_wday;
    MR16(p+6)=(uint16_t)g.tm_mday; MR16(p+8)=(uint16_t)g.tm_hour; MR16(p+10)=(uint16_t)g.tm_min;
    MR16(p+12)=0; MR16(p+14)=0;
}
static uint32_t tz_info(uint32_t p){
    time_t now = time(0); struct tm lt; localtime_r(&now, &lt);
    struct tm y0 = {0}; y0.tm_year = lt.tm_year; y0.tm_mday = 1; y0.tm_isdst = -1;
    time_t t0 = mktime(&y0);
    long std_off = lt.tm_isdst > 0 ? 0 : lt.tm_gmtoff, dst_off = lt.tm_isdst > 0 ? lt.tm_gmtoff : 0;
    time_t to_dst = 0, to_std = 0;
    struct tm a; localtime_r(&t0, &a);
    int prev = a.tm_isdst > 0;
    for (time_t t = t0 + 3600; t < t0 + 366 * 86400; t += 3600){
        struct tm b; localtime_r(&t, &b);
        int d = b.tm_isdst > 0;
        if (d) dst_off = b.tm_gmtoff; else std_off = b.tm_gmtoff;
        if (d != prev){
            time_t lo = t - 3600, hi = t;                 /* to the minute */
            while (hi - lo > 60){ time_t m = lo + (hi - lo) / 2; struct tm x; localtime_r(&m, &x);
                                  if ((x.tm_isdst > 0) == d) hi = m; else lo = m; }
            if (d) to_dst = hi; else to_std = hi;
            prev = d;
        }
    }
    if (!std_off && !dst_off) std_off = lt.tm_gmtoff;
    if (p){
        mem_commit(p, 172); memset(g_mem + p, 0, 172);
        MR32(p) = (uint32_t)(int32_t)(-std_off / 60);
        const char *sn = tzname[0] ? tzname[0] : "", *dn = tzname[1] ? tzname[1] : "";
        for (int i = 0; i < 31 && sn[i]; i++) MR16(p + 4 + 2*i) = (uint8_t)sn[i];
        for (int i = 0; i < 31 && dn[i]; i++) MR16(p + 88 + 2*i) = (uint8_t)dn[i];
        if (to_dst && to_std){
            put_systime(p + 152, to_dst + std_off);       /* wall clock before the switch */
            put_systime(p + 68,  to_std + dst_off);
            MR32(p + 168) = (uint32_t)(int32_t)(-(dst_off - std_off) / 60);
        }
    }
#ifdef __wasm__
    /* the zone comes from the page (runtime/web/tz.c): show what the game got */
    { static int shown; if (!shown++){
        struct tm a, b; time_t td = to_dst + std_off, ts = to_std + dst_off; gmtime_r(&td, &a); gmtime_r(&ts, &b);
        fprintf(stderr, "[hle] GetTimeZoneInformation: bias %ld min, \"%s\" / \"%s\", daylight %04d-%02d-%02d %02d:%02d,"
                " standard %04d-%02d-%02d %02d:%02d (local) -> %s\n", -std_off / 60, tzname[0], tzname[1],
                a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday,
                b.tm_hour, b.tm_min, !to_dst || !to_std ? "unknown (no DST)" : lt.tm_isdst > 0 ? "daylight" : "standard"); } }
#endif
    if (!to_dst || !to_std) return 0;                    /* TIME_ZONE_ID_UNKNOWN: no DST */
    return lt.tm_isdst > 0 ? 2u : 1u;                    /* DAYLIGHT / STANDARD */
}
/* Free/total bytes of the volume holding the saves (NFSU2_SAVEDIR, else HOME). */
static uint32_t disk_space(uint64_t *fr, uint64_t *tot){
    struct statfs sf; const char *d = getenv("NFSU2_SAVEDIR");
    if (!d || !*d) d = getenv("HOME");
    if (!d || statfs(d, &sf) != 0){ *fr = 8ull << 30; *tot = 64ull << 30; return 1; }
    *fr = (uint64_t)sf.f_bavail * sf.f_bsize; *tot = (uint64_t)sf.f_blocks * sf.f_bsize;
    return 1;
}

int win32_ext_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    /* --- Registry --- */
    /* the game's key table (Most Wanted): its keys and values (reg_open ...) */
    if (g_game.reg_keys && (!strcmp(n,"RegOpenKeyA") || !strcmp(n,"RegOpenKeyExA") ||
                            !strcmp(n,"RegCreateKeyA") || !strcmp(n,"RegCreateKeyExA"))) r = reg_open(c, n);
    else if (g_game.reg_keys && !strcmp(n,"RegQueryValueExA")) r = reg_query(c);
    else if (g_game.reg_keys && !strcmp(n,"RegSetValueExA")) r = reg_set(c);
    else if (g_game.reg_keys && !strcmp(n,"RegCloseKey")){
        Obj *o = obj_get(A(c,0));
        if (o && o->kind == OB_KEY){ obj_close(A(c,0)); r = ERROR_SUCCESS; }
        else r = reg_root_name(A(c,0)) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE; }
    /* the flat registry (UG2): every key opens, values by name alone */
    else if (!strcmp(n,"RegOpenKeyA")){ uint32_t p=A(c,2); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegOpenKeyExA")){
        if (getenv("REGLOG")){ uint32_t sp=A(c,1);
            fprintf(stderr,"  [registry] OpenKeyEx(\"%s\")\n",
                    (sp && mem_is_committed(sp)) ? (const char*)(g_mem+sp) : "(null)"); }
        uint32_t p=A(c,4); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCreateKeyA")){ uint32_t p=A(c,2); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCreateKeyExA")){ uint32_t p=A(c,7); if(p) MR32(p)=obj_new(OB_KEY); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegQueryValueExA")){
        /* An empty registry sends the game down the "first install" path
         * on EVERY START (NotFirstTime is not found) and without install
         * information the front-end flow does not proceed.  We return a
         * plausible installed-game profile. */
        uint32_t nameptr=A(c,1), ptype=A(c,3), pdata=A(c,4), pcb=A(c,5);
        const char *vn = (nameptr && mem_is_committed(nameptr))
                         ? (const char*)(g_mem+nameptr) : "";
        if (getenv("REGLOG"))
            fprintf(stderr,"  [registry] QueryValue(\"%s\")\n", vn);

        const char *sval = 0; uint32_t dval = 0; int have_d = 0;
        const char *language_registry(void);
        /* look at the STORED value first; the language is always the setting's */
        { int ri = strcasecmp(vn, "Language") ? reg_find(vn) : -1;
          if (ri >= 0){
              if (g_reg[ri].is_str) sval = g_reg[ri].sv;
              else { dval = g_reg[ri].dw; have_d = 1; }
          } }
        if (sval || have_d) { /* the stored value will be used */ }
        /* v12na: the registry is now persistent (reg_save), and the game
         * writes NotFirstTime itself on its first start.  A fake
         * NotFirstTime=1 meant the first-start "system check" (0x5CED60 ->
         * 0x5CEC90: CPU MHz + GPU score -> g_PerformanceLevel, default
         * graphics settings) NEVER ran; result: all effects off, 640x480.
         * NFSU2_FAKE_NOTFIRSTTIME=1 is the old behaviour. */
        else if (!strcasecmp(vn,"NotFirstTime") && getenv("NFSU2_FAKE_NOTFIRSTTIME")){ dval=1; have_d=1; }
        /* Always present on Windows.  Without it RealCore (0x6F625D)
         * measures the CPU speed by busy-waiting on QPC (0x6F5F14: 5 x 200 ms
         * in the same frame); since the virtual clock advances at most
         * ~250 ms within a frame, it spun forever on the movie path.  Same
         * speed as rdtsc. */
        else if (!strcasecmp(vn,"~MHz")){ dval = win32_cpu_mhz(); have_d=1; }
        else if (!strcasecmp(vn,"Language"))      sval=language_registry();   /* runtime/language.c */
        else {                                    /* the game's install profile (runtime/game.h) */
            for (const RegDefault *d = g_game.reg_defaults; d && d->name; d++)
                if (!strcasecmp(vn, d->name)){
                    if (d->str) sval = d->str; else { dval = d->dword; have_d = 1; }
                    break;
                }
        }

        r = reg_answer(ptype, pdata, pcb, sval, dval, have_d);
    }
    else if (!strcmp(n,"RegQueryValueA")){ r=ERROR_FILE_NOT_FOUND; }
    else if (!strcmp(n,"RegSetValueExA")||!strcmp(n,"RegSetValueA")){
        /* Writes used to be SILENTLY DISCARDED: the graphics/sound
         * settings the game saved were reset on every start.  Now they
         * are kept and written to disk. */
        uint32_t nameptr=A(c,1), type=A(c,3), pdata=A(c,4), cb=A(c,5);
        const char *vn = (nameptr && mem_is_committed(nameptr))
                         ? (const char*)(g_mem+nameptr) : "";
        if (*vn){
            if (type == 4u && pdata && mem_is_committed(pdata+3))
                reg_put_dword(vn, MR32(pdata));
            else if (type == 1u && pdata && mem_is_committed(pdata))
                reg_put_string(vn, (const char*)(g_mem+pdata));
            if (getenv("REGLOG"))
                fprintf(stderr,"  [registry] SetValue(\"%s\") type=%u size=%u\n", vn, type, cb);
        }
        r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegCloseKey")){ Obj *o = obj_get(A(c,0)); if (o && o->kind == OB_KEY) obj_close(A(c,0)); r=ERROR_SUCCESS; }
    else if (!strcmp(n,"RegDeleteKeyA")||!strcmp(n,"RegDeleteValueA")) r=ERROR_SUCCESS;
    else if (!strcmp(n,"RegEnumKeyExA")||!strcmp(n,"RegEnumValueA")) r=259u; /* ERROR_NO_MORE_ITEMS */
    /* --- window / USER32 --- */
    /* Window message path.  DirectInput devices are only Acquired when the
     * application gets FOCUS; the game learns that from WM_ACTIVATEAPP/
     * WM_SETFOCUS messages.  Since PeekMessage always returned 0 and
     * DispatchMessage never called the window procedure, the game never
     * concluded "I am active" -> the keyboard was never Acquired.       */

    else if (!strcmp(n,"RegisterClassExA")||!strcmp(n,"RegisterClassA")){
        /* WNDCLASSEXA: +8 lpfnWndProc | WNDCLASSA: +4 lpfnWndProc */
        uint32_t wc=A(c,0);
        if (wc && mem_is_committed(wc+11)){
            uint32_t off = !strcmp(n,"RegisterClassExA") ? 8u : 4u;
            uint32_t wp = MR32(wc+off);
            if (wp) { g_wndproc = wp;
                fprintf(stderr,"  [window] class registered, wndproc=0x%08x\n", wp); }
        }
        r=0xC001u; }
    else if (!strcmp(n,"UnregisterClassA")) r=1;
    else if (!strcmp(n,"CreateWindowExA")){
        r = g_hwnd;
        if (!g_wm_activated){ g_wm_activated = 1;
            wm_push(0x0018u, 1u, 0u);   /* WM_SHOWWINDOW  */
            wm_push(0x001Cu, 1u, 0u);   /* WM_ACTIVATEAPP  active */
            wm_push(0x0006u, 1u, 0u);   /* WM_ACTIVATE     WA_ACTIVE */
            wm_push(0x0007u, 0u, 0u);   /* WM_SETFOCUS     */
            wm_push(0x000Fu, 0u, 0u);   /* WM_PAINT        */
        } }
    else if (!strcmp(n,"DestroyWindow")) r=1;
    else if (!strcmp(n,"GetDesktopWindow")) r=MM_HWND_DESKTOP;
    else if (!strcmp(n,"GetActiveWindow")||!strcmp(n,"GetForegroundWindow")) r=MM_HWND_GAME;
    else if (!strcmp(n,"LoadIconA")||!strcmp(n,"LoadCursorA")) r=MM_HICON;
    else if (!strcmp(n,"SetCursor")) r=MM_HICON;
    else if (!strcmp(n,"ShowCursor")){ static int cnt=0; r=(uint32_t)(A(c,0)?++cnt:--cnt); }
    else if (!strcmp(n,"GetCursorPos")){ uint32_t p=A(c,0);
        if(p){ int mx=0,my=0; void metal_get_cursor(int*,int*); metal_get_cursor(&mx,&my);
               mem_commit(p,8); MR32(p)=(uint32_t)mx; MR32(p+4)=(uint32_t)my; } r=1; }
    else if (!strcmp(n,"SetCursorPos")) r=1;
    else if (!strcmp(n,"GetSystemMetrics")){
        unsigned gw = 1280, gh = 720; void metal_game_size(unsigned*,unsigned*); metal_game_size(&gw,&gh);
        switch(A(c,0)){ case 0: r=gw; break; case 1: r=gh; break;
                        case 4: r=19; break; case 5: r=1; break; case 6: r=1; break;
                        case 32: r=4; case 33: r=4; break; default: r=0; } }
    else if (!strcmp(n,"AdjustWindowRect")){ r=1; }
    else if (!strcmp(n,"GetClientRect")||!strcmp(n,"GetWindowRect")){
        /* The game's real resolution: the cursor mapping follows it (see
         * metal_game_size). */
        unsigned gw = 1280, gh = 720; void metal_game_size(unsigned*,unsigned*); metal_game_size(&gw,&gh);
        uint32_t p=A(c,1); if(p){mem_commit(p,16);MR32(p)=0;MR32(p+4)=0;MR32(p+8)=gw;MR32(p+12)=gh;} r=1; }
    else if (!strcmp(n,"ShowWindow")||!strcmp(n,"UpdateWindow")||!strcmp(n,"SetFocus")
           ||!strcmp(n,"SetForegroundWindow")||!strcmp(n,"SetWindowPos")
           ||!strcmp(n,"InvalidateRect")||!strcmp(n,"SetWindowTextA")) r=1;
    else if (!strcmp(n,"SetWindowLongA")||!strcmp(n,"GetWindowLongA")) r=0;
    else if (!strcmp(n,"PeekMessageA")||!strcmp(n,"GetMessageA")){
        /* MSG: +0 hwnd +4 message +8 wParam +12 lParam +16 time +20 pt */
        uint32_t pm=A(c,0), m=0, wp=0, lp=0;
        int remove = !strcmp(n,"GetMessageA") ? 1 : (A(c,4) & 1u);
        /* Host keyboard and mouse messages (keyboard.c), quit requests and
         * app (de)activation (WM_CLOSE / WM_ACTIVATE(APP), lifecycle.c) join
         * the queue here, once the game window exists. */
        if (g_wm_activated){ kbd_pump(); lifecycle_pump(); }
        /* GetMessageA blocks while the queue is empty -- but only while the
         * game has been told it is inactive: that is the one case where it
         * calls it (pump 0x5BF1C0), and where Windows would block too; and
         * not during a LAN session (lifecycle_may_park).  The guest lock is
         * released meanwhile so the audio and timer threads keep running;
         * the sound output is muted (audio_set_muted) until the game is
         * active again (lifecycle_pump).  A message that wakes it before
         * that (a mouse move over the page) runs one frame of an inactive
         * game, whose streams nothing refills: unmuted, the mixer would play
         * their stale buffers, a movie's last second over and over. */
        if (remove && !strcmp(n,"GetMessageA") && g_wmq_head == g_wmq_tail && lifecycle_may_park()){
            void audio_set_muted(int);
            audio_set_muted(1);
            guest_yield_begin();
            for (;;){
                struct timespec ts = { 0, 5000000L }; nanosleep(&ts, 0);
                guest_yield_end();
                kbd_pump(); lifecycle_pump();
                if (g_wmq_head != g_wmq_tail || !lifecycle_may_park()) break;
                guest_yield_begin();
            }
            if (!lifecycle_game_parked()) audio_set_muted(0);
        }
        if (g_wmq_head != g_wmq_tail){
            if (remove) wm_pop(&m,&wp,&lp);
            else { m=g_wmq[g_wmq_head].msg; wp=g_wmq[g_wmq_head].wp; lp=g_wmq[g_wmq_head].lp; }
            if (pm){ mem_commit(pm,28);
                MR32(pm)=g_hwnd; MR32(pm+4)=m; MR32(pm+8)=wp; MR32(pm+12)=lp;
                MR32(pm+16)=0; MR32(pm+20)=0; MR32(pm+24)=0; }
            g_wm_sent++;
            r=1;
        } else {
            if (pm){ mem_commit(pm,28); memset(g_mem+pm,0,28); MR32(pm)=g_hwnd; }
            r = !strcmp(n,"GetMessageA") ? 1u : 0u;   /* GetMessage 0 = WM_QUIT */
        }
    }
    else if (!strcmp(n,"DispatchMessageA")){
        uint32_t pm=A(c,0);
        if (g_wndproc && pm && mem_is_committed(pm+15)){
            uint32_t args[4] = { MR32(pm), MR32(pm+4), MR32(pm+8), MR32(pm+12) };
            if (args[1]){ g_wm_dispatched++; r = call_guest(c, g_wndproc, 4, args); }
        }
    }
    else if (!strcmp(n,"TranslateMessage")
           ||!strcmp(n,"DefWindowProcA")||!strcmp(n,"PostMessageA")
           ||!strcmp(n,"SendMessageA")||!strcmp(n,"PostQuitMessage")) r=0;
    else if (!strcmp(n,"ReleaseCapture")||!strcmp(n,"SetCapture")) r=1;
    else if (!strcmp(n,"IsIconic")) r=0;
    else if (!strcmp(n,"GetKeyboardState")){ uint32_t p=A(c,0); if(p){mem_commit(p,256);memset(g_mem+p,0,256);} r=1; }
    else if (!strcmp(n,"GetAsyncKeyState")) r=0;
    /* --- synchronization --- */
    else if (!strcmp(n,"CreateEventA")){
        uint32_t h=obj_new(OB_EVENT); Obj*o=obj_get(h);
        if(o){ o->manual=(int)A(c,1); o->signaled=(int)A(c,2); } r=h; }
    else if (!strcmp(n,"SetEvent")){ Obj*o=obj_get(A(c,0));
        { uint32_t h=A(c,0);
          if (h>=H_BASE && h<H_BASE+64){ g_wstat[h-H_BASE].sets++;
            g_wstat[h-H_BASE].setter_tid =
                (c->fs_base && mem_is_committed(c->fs_base+0x24u))
                ? MR32(c->fs_base+0x24u) : 0u; } }
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled=1; pthread_cond_broadcast(&o->cv); pthread_mutex_unlock(&o->mtx); obj_wake(); } r=1; }
    else if (!strcmp(n,"ResetEvent")){ Obj*o=obj_get(A(c,0));
        if(o){ pthread_mutex_lock(&o->mtx); o->signaled=0; pthread_mutex_unlock(&o->mtx);} r=1; }
    else if (!strcmp(n,"CreateMutexA")){
        uint32_t h=obj_new(OB_MUTEX); Obj*o=obj_get(h);
        if(o){ if (A(c,1)){ o->signaled=0; o->owner_tid=cur_tid(c); o->rec=1; }
               else       { o->signaled=1; o->owner_tid=0; o->rec=0; } }
        r=h; }
    else if (!strcmp(n,"ReleaseMutex")){
        Obj*o=obj_get(A(c,0)); uint32_t me=cur_tid(c);
        r=0;
        if(o){ pthread_mutex_lock(&o->mtx);
               if (o->kind == OB_MUTEX){
                   if (o->owner_tid == me && o->rec > 0){
                       if (--o->rec == 0){ o->owner_tid = 0; o->signaled = 1;
                                           pthread_cond_broadcast(&o->cv); }
                       r=1;
                   } else r=0;            /* we are not the owner */
               } else { o->signaled=1; pthread_cond_broadcast(&o->cv); r=1; }
               pthread_mutex_unlock(&o->mtx); }
    }
    /* Semaphores, Windows' rules (Most Wanted takes them through a wrapper that
     * keeps its own count with lock xadd; UG2 imports none).  CreateSemaphoreA(
     * attributes, lInitialCount, lMaximumCount, lpName): NULL with
     * ERROR_INVALID_PARAMETER for a maximum below 1 or an initial count
     * outside 0..maximum.  A name is only kept for the logs: on Windows a
     * second CreateSemaphoreA with the same name opens the first one (and
     * says ERROR_ALREADY_EXISTS), in the same process too; that is not done
     * here (CreateEventA and CreateMutexA leave their names out too).  Most
     * Wanted's wrapper (0x834E3F) passes a name only when its caller set one;
     * the one semaphore it makes by the main menu has none.
     * ReleaseSemaphore(hSemaphore, lReleaseCount, lpPreviousCount): FALSE with
     * ERROR_INVALID_PARAMETER for a count below 1, ERROR_TOO_MANY_POSTS past the
     * maximum (the count stays), ERROR_INVALID_HANDLE for a handle that is no
     * semaphore; on success the count before goes to lpPreviousCount (if any)
     * and every waiter looks again (sem_wait_one, wait_set). */
    else if (!strcmp(n,"CreateSemaphoreA")){
        int32_t ini = (int32_t)A(c,1), mx = (int32_t)A(c,2); uint32_t nm = A(c,3);
        r = 0;
        if (mx < 1 || ini < 0 || ini > mx) win32_set_last_error(c, 87u);
        else if (!(r = obj_new(OB_SEM))) win32_set_last_error(c, 8u);      /* ERROR_NOT_ENOUGH_MEMORY: the table is full */
        else {
            Obj *o = obj_get(r);
            pthread_mutex_lock(&o->mtx);
            o->signaled = ini; o->sem_max = mx;
            if (nm && mem_is_committed(nm)) snprintf(o->name, sizeof o->name, "%s", (const char *)(g_mem + nm));
            pthread_mutex_unlock(&o->mtx);
            g_sems_made++;
            static int sh = 0;
            if (sh++ < 8) fprintf(stderr, "[sem] CreateSemaphoreA(%d of at most %d%s%s) -> 0x%08x (from 0x%08x)\n",
                                  ini, mx, o->name[0] ? ", " : "", o->name, r, c->eip);
        } }
    else if (!strcmp(n,"ReleaseSemaphore")){
        Obj *o = obj_get(A(c,0)); int32_t add = (int32_t)A(c,1); uint32_t pp = A(c,2), err = 0; int prev = 0;
        if (!o) err = 6u;
        else {
            pthread_mutex_lock(&o->mtx);
            if (o->kind != OB_SEM) err = 6u;
            else if (add < 1) err = 87u;
            else if ((int64_t)o->signaled + add > (int64_t)o->sem_max) err = 298u;      /* ERROR_TOO_MANY_POSTS */
            else { prev = o->signaled; o->signaled += add; pthread_cond_broadcast(&o->cv); }
            pthread_mutex_unlock(&o->mtx);
        }
        if (err){ win32_set_last_error(c, err); r = 0; }
        else { if (pp){ mem_commit(pp, 4); MR32(pp) = (uint32_t)prev; } r = 1; } }
    /* --- waitable timers (see timer_poll) --- */
    else if (!strcmp(n,"CreateWaitableTimerA")){       /* (attributes, bManualReset, lpTimerName) */
        uint32_t h = obj_new(OB_TIMER); Obj *o = obj_get(h);
        if (o){
            o->manual = A(c,1) != 0;
            if (A(c,2) && mem_is_committed(A(c,2))) snprintf(o->name, sizeof o->name, "%s", (const char *)(g_mem + A(c,2)));
            g_timers_made++;                            /* WaitForMultipleObjects looks for timers from now on */
        } else win32_set_last_error(c, 8u);             /* ERROR_NOT_ENOUGH_MEMORY: the table is full */
        { static int sh = 0; if (sh++ < 8)
            fprintf(stderr, "[timer] CreateWaitableTimerA(%s reset%s%s) -> 0x%08x (from 0x%08x)\n",
                    A(c,1) ? "manual" : "auto", o && o->name[0] ? ", " : "", o ? o->name : "", h, c->eip); }
        r = h; }
    else if (!strcmp(n,"SetWaitableTimer")){
        /* (hTimer, const LARGE_INTEGER *pDueTime, LONG lPeriod ms, pfnCompletionRoutine, its argument, fResume) */
        Obj *o = obj_get(A(c,0)); uint32_t pd = A(c,1); int32_t per = (int32_t)A(c,2);
        if (!o || o->kind != OB_TIMER){ win32_set_last_error(c, 6u); r = 0; }          /* ERROR_INVALID_HANDLE */
        else if (!pd || !mem_is_committed(pd) || !mem_is_committed(pd + 7u) || per < 0){
            win32_set_last_error(c, 87u); r = 0; }                                      /* ERROR_INVALID_PARAMETER */
        else {
            int64_t due = (int64_t)((uint64_t)MR32(pd) | ((uint64_t)MR32(pd + 4u) << 32));
            uint64_t now = host_us(), at;
            if (due < 0) at = now + ((0ull - (uint64_t)due) + 9u) / 10u;                /* relative, 100 ns units */
            else at = (uint64_t)due > FT_EPOCH ? ((uint64_t)due - FT_EPOCH + 9u) / 10u : 0u;   /* absolute FILETIME */
            pthread_mutex_lock(&o->mtx);
            o->t_due_us = at; o->t_period_us = (uint64_t)per * 1000u; o->t_armed = 1; o->signaled = 0;
            pthread_cond_broadcast(&o->cv);
            pthread_mutex_unlock(&o->mtx);
            { static int sh = 0; if (sh++ < 4)
                fprintf(stderr, "[timer] SetWaitableTimer(0x%08x): %s due time, fires %s %.1f ms, period %d ms (from 0x%08x)\n",
                        A(c,0), due < 0 ? "relative" : "absolute", at >= now ? "in" : "at once, late by",
                        (double)(at >= now ? at - now : now - at) / 1000.0, per, c->eip); }
            if (A(c,3)){ static int sh = 0; if (sh++ < 4)
                fprintf(stderr, "[timer] SetWaitableTimer(0x%08x): completion routine 0x%08x not supported (no APCs);"
                                " the timer still signals\n", A(c,0), A(c,3)); }
            r = 1;
            if (A(c,5)) win32_set_last_error(c, 50u);   /* fResume: ERROR_NOT_SUPPORTED, and still TRUE (no power management) */
        } }
    else if (!strcmp(n,"WaitForSingleObject")){
        Obj*o=obj_get(A(c,0)); uint32_t ms=A(c,1);
        { uint32_t h=A(c,0);
          uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u))
                         ? MR32(c->fs_base+0x24u) : 0u;
          if (h>=H_BASE && h<H_BASE+64){ g_wstat[h-H_BASE].waits++;
                                         g_wstat[h-H_BASE].waiter_tid = tid; }
          static int sh=0, want=-1;
          if (want < 0) want = getenv("WAITLOG") ? 1 : 0;    /* v12na: was unconditional log spam */
          if (want && ++sh % 400 == 0){
            fprintf(stderr,"  [event] handle  kind signal  waits    SetEvent  waiter-tid\n");
            for (int i=0;i<64;i++) if (g_wstat[i].waits || g_wstat[i].sets)
                fprintf(stderr, "         h%-4d %3d %6d %8llu %9llu  tid=%u\n", i,
                        g_obj[i].kind, g_obj[i].signaled,
                        (unsigned long long)g_wstat[i].waits,
                        (unsigned long long)g_wstat[i].sets,
                        g_wstat[i].waiter_tid);
          } }
        if(!o){ r=0; }
        else if (o->kind == OB_MUTEX && (tl_noyield || gil_keep_for_mutex()) && mutex_take_now(o, cur_tid(c))) r=0;   /* WAIT_OBJECT_0 */
        else if (o->kind == OB_MUTEX){
            /* MUTEX: ownership + RECURSION.  The same thread must be able to
             * take the same mutex again; otherwise the game's CRT wrapper,
             * which takes nested locks, deadlocks itself (that is how the
             * video player hung). */
            uint32_t me = cur_tid(c);
            ts_set(TS_WAIT, A(c,0));
            guest_yield_begin();
            pthread_mutex_lock(&o->mtx);
            if (o->owner_tid == me && o->rec > 0){
                o->rec++; r = 0;                       /* WAIT_OBJECT_0 */
            } else {
                struct timespec ts;
                int timed_out = 0;
                while (!o->signaled && !timed_out){
                    if (!ms){ timed_out = 1; break; }
                    clock_gettime(CLOCK_REALTIME,&ts);
                    uint32_t wait = (ms==0xFFFFFFFFu)? 2000u : (ms>2000u?2000u:ms);
                    ts.tv_sec += wait/1000; ts.tv_nsec += (long)(wait%1000)*1000000L;
                    if(ts.tv_nsec>=1000000000L){ts.tv_sec++;ts.tv_nsec-=1000000000L;}
                    if (pthread_cond_timedwait(&o->cv,&o->mtx,&ts) != 0 &&
                        ms != 0xFFFFFFFFu) timed_out = 1;
                    if (ms != 0xFFFFFFFFu) break;
                }
                if (o->signaled){ o->signaled = 0; o->owner_tid = me; o->rec = 1; r = 0; }
                else r = 0x102u;                        /* WAIT_TIMEOUT */
            }
            pthread_mutex_unlock(&o->mtx);
            guest_yield_end();
        }
        else if (o->kind == OB_TIMER){                  /* waitable timer: on the virtual clock, in finite slices */
            ts_set(TS_WAIT, A(c,0));
            guest_yield_begin();
            r = timer_wait(o, ms);
            guest_yield_end();
            if (r == WAIT_FAILED_) win32_set_last_error(c, 6u);   /* ERROR_INVALID_HANDLE: closed during the wait */
        }
        else if (o->kind == OB_SEM){                    /* semaphore: one of its count, in finite slices */
            ts_set(TS_WAIT, A(c,0));
            sem_hold(o);
            guest_yield_begin();
            r = sem_wait_one(o, ms);
            guest_yield_end();
            sem_drop(o);
            if (r == WAIT_FAILED_) win32_set_last_error(c, 6u);
        }
        else {
            ts_set(TS_WAIT, A(c,0));
            guest_yield_begin();
            pthread_mutex_lock(&o->mtx);
            if(!o->signaled && ms){
                struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
                uint32_t wait = (ms==0xFFFFFFFFu)? 2000u : (ms>2000u?2000u:ms);
                ts.tv_sec += wait/1000; ts.tv_nsec += (long)(wait%1000)*1000000L;
                if(ts.tv_nsec>=1000000000L){ts.tv_sec++;ts.tv_nsec-=1000000000L;}
                pthread_cond_timedwait(&o->cv,&o->mtx,&ts);
            }
            r = o->signaled ? 0u : 0x102u;              /* WAIT_OBJECT_0 / WAIT_TIMEOUT */
            if(o->signaled && !o->manual) o->signaled=0;
            pthread_mutex_unlock(&o->mtx);
            guest_yield_end();
        } }
    /* --- thread --- */
    else if (!strcmp(n,"CreateThread")){
        uint32_t entry=A(c,2), arg=A(c,3), ptid=A(c,5);
        { static int sh=0; if (getenv("VERBOSE") && sh<40){ sh++;
            fprintf(stderr,"      [thread] CreateThread(attr=0x%08x stack=0x%x entry=0x%08x arg=0x%08x flags=0x%x ptid=0x%08x)\n",
                    A(c,0),A(c,1),entry,arg,A(c,4),ptid); } }
        /* the thread's stack and TEB first: without them it does not start */
        uint32_t stk, teb = 0;
        pthread_mutex_lock(&g_lock);
        stk = stack_take();
        if (stk && !(teb = teb_take())) g_stk_free[g_nstk_free++] = stk;
        pthread_mutex_unlock(&g_lock);
        if (!teb) thread_refused(c, entry, !stk);
        uint32_t h=teb ? obj_new(OB_THREAD) : 0; Obj*o=obj_get(h);
        static uint32_t next_tid=100;
        if (teb && !o){                         /* object table full: give them back */
            pthread_mutex_lock(&g_lock);
            g_stk_free[g_nstk_free++] = stk; g_teb_free[g_nteb_free++] = teb;
            pthread_mutex_unlock(&g_lock);
        }
        if(o){
            o->tid = ++next_tid; o->guest_entry = entry; o->guest_arg = arg;
            o->creator = c->eip; o->looked = 0;   /* the return address; the game's own lookups count from here */
            /* the routine it runs, read now: the game's own start routines may keep it on the creator's stack */
            o->routine = g_game.thread_start ? g_game.thread_start(entry, arg) : entry;
            if (g_game.thread_names){ static int sh = 0; if (sh < 64){ sh++;
                const char *tn = game_thread_name(o->routine);
                fprintf(stderr, "[thread] tid=%u runs 0x%08x (%s), CreateThread(entry 0x%08x) from 0x%08x\n",
                        o->tid, o->routine, tn ? tn : "no name", entry, o->creator); } }
            o->manual = 1;                      /* an ended thread stays signaled for every waiter */
            if(ptid) MR32(ptid)=o->tid;
            { const char *mn = 0;
              if (module_nearest_fn(entry, &mn))
                  fprintf(stderr, "[thread] tid=%u (%s, entry 0x%08x) started\n", o->tid, mn, entry); }
            o->teb = teb; o->stack_lo = stk;
            mem_commit(teb, MM_TEB_SIZE);
            TArg *t = calloc(1,sizeof *t);
            t->entry=entry; t->arg=arg; t->teb=teb; t->stack=stk; t->ob=o;
            o->pending = t;
            if (A(c,4) & 0x4u){                 /* CREATE_SUSPENDED */
                o->started = 0;                 /* waits for ResumeThread */
            } else {
                if (guest_pthread_create(&o->th,guest_thread_main,t)==0){
                    o->started=1; o->pending=0; pthread_detach(o->th);
                } else {
                    free(t); o->pending = 0;
                    pthread_mutex_lock(&g_lock);
                    g_stk_free[g_nstk_free++] = stk; g_teb_free[g_nteb_free++] = teb;
                    pthread_mutex_unlock(&g_lock);
                    o->teb = o->stack_lo = 0;
                }
            }
        }
        r=h; }
    else if (!strcmp(n,"GetCurrentThread")) r=0xFFFFFFFEu;
    else if (!strcmp(n,"ResumeThread")){
        Obj*o=obj_get(A(c,0));
        if (o && !o->started && o->pending){
            if (guest_pthread_create(&o->th,guest_thread_main,o->pending)==0){
                o->started=1; o->pending=0; pthread_detach(o->th); }
        }
        r=1; }
    else if (!strcmp(n,"SuspendThread")) r=0;
    else if (!strcmp(n,"ExitThread")){
        /* REALLY end the guest thread.  Otherwise execution falls from
         * _endthreadex into the _threadstartex prologue and the thread
         * restarts itself.                                                 */
        Obj *o = (Obj*)tl_obj;
        { static int sh=0; if (sh++ < 64) fprintf(stderr,"[thread] end tid=%u (ExitThread %u)\n", o ? o->tid : 0u, A(c,0)); }
        if (o && o->term_req){ o->term_req = 0; g_term_pending--; }
        thread_finish(o, A(c,0), c);
        guest_unlock();
        pthread_exit(0);
        r=1; }
    else if (!strcmp(n,"TerminateThread")){
        Obj *o = obj_get(A(c,0)); r = 1;
        if (!exit_semantics()) { /* SPEED2.EXE: old no-op */ }
        else if (!o || o->kind != OB_THREAD) r = 0;
        else if (!o->ended && !o->term_req){
            fprintf(stderr, "[thread] TerminateThread(tid=%u, %u) from tid=%u: the thread ends at its next import call\n",
                    o->tid, A(c,1), cur_tid(c));
            o->exit_code = A(c,1); o->term_req = 1; g_term_pending++;
            if (o == (Obj*)tl_obj) win32_thread_term_check(c);    /* itself: right now */
        } }
    else if (!strcmp(n,"GetExitCodeThread")){
        uint32_t h=A(c,0), p=A(c,1); Obj *o = obj_get(h);
        if (!exit_semantics()){ if(p) MR32(p)=0; r=1; }            /* SPEED2.EXE: old answer */
        else if (h == 0xFFFFFFFEu){ if(p) MR32(p)=0x103u; r=1; }   /* GetCurrentThread() */
        else if (!o || o->kind != OB_THREAD) r=0;
        else {
            uint32_t code = o->ended ? o->exit_code : 0x103u;        /* STILL_ACTIVE */
            if (p){ mem_commit(p,4); MR32(p)=code; }
            r=1;
            /* evidence for the hosting tests: how long StopServer waited */
            { static uint32_t last_h = 0, polls = 0;
              if (h != last_h){ last_h = h; polls = 0; }
              polls++;
              if (o->ended) fprintf(stderr, "[thread] GetExitCodeThread(tid=%u) -> %u after %u STILL_ACTIVE poll(s)\n",
                                    o->tid, code, polls - 1); }
            /* StopServer polls this in a loop without sleeping; on Windows the
             * scheduler would run the thread it waits for, here it needs the
             * guest lock: give it up for a moment. */
            if (!o->ended){ guest_yield_begin(); struct timespec ts={0,1000000L}; nanosleep(&ts,0); guest_yield_end(); }
        } }
    else if (!strcmp(n,"SetThreadAffinityMask")||!strcmp(n,"SetProcessAffinityMask")) r=1;
    else if (!strcmp(n,"GetProcessAffinityMask")){
        uint32_t a=A(c,1),b=A(c,2); if(a) MR32(a)=0xFF; if(b) MR32(b)=0xFF; r=1; }
    else if (!strcmp(n,"SetPriorityClass")) r=1;
    else if (!strcmp(n,"GetPriorityClass")) r=0x20;
    /* --- handles / misc --- */
    else if (!strcmp(n,"Sleep")||!strcmp(n,"SleepEx")){
        /* the main thread's sleep moves the clock (game.h vclock_sleep_real; win32.c) */
        uint64_t vclock_sleep_begin(void); void vclock_sleep_end(uint64_t);
        int vs = g_game.vclock_sleep_real && tl_slot == 0 && A(c,0);
        uint64_t t0 = vs ? vclock_sleep_begin() : 0u;
        uint32_t ms=A(c,0);
        ts_set(TS_SLEEP, ms);
        guest_yield_begin();
        { struct timespec ts={ (time_t)(ms/1000u), (long)((ms%1000u)*1000000u) };
          if(!ms){ ts.tv_nsec = 200000L; }        /* Sleep(0): just yield */
          nanosleep(&ts,0); }
        guest_yield_end();
        if (vs) vclock_sleep_end(t0);
        r=0; }
    else if (!strcmp(n,"WaitForMultipleObjects") && wfmo_has_timer(A(c,0), A(c,1))){
        /* a set with a waitable timer or a semaphore: Windows' rules (wait_set) */
        uint32_t cnt = A(c,0), parr = A(c,1), err = 0, ser[64]; Obj *ob[64];
        for (uint32_t i = 0; i < cnt; i++){
            if (!(ob[i] = obj_get(MR32(parr + 4u * i)))){ err = 6u; continue; }   /* ERROR_INVALID_HANDLE */
            pthread_mutex_lock(&ob[i]->mtx); ser[i] = ob[i]->serial; pthread_mutex_unlock(&ob[i]->mtx);
        }
        if (err) r = WAIT_FAILED_;
        else {
            ts_set(TS_WAITM, MR32(parr));
            uint64_t held = 0;                          /* the semaphores' references, one per handle in the set */
            for (uint32_t i = 0; i < cnt; i++) if (ob[i]->kind == OB_SEM){ sem_hold(ob[i]); held |= 1ull << i; }
            guest_yield_begin();
            r = wait_set(ob, ser, cnt, A(c,2) != 0, A(c,3), cur_tid(c), &err);
            guest_yield_end();
            for (uint32_t i = 0; i < cnt; i++) if (held >> i & 1u) sem_drop(ob[i]);
        }
        if (r == WAIT_FAILED_) win32_set_last_error(c, err); }
    else if (!strcmp(n,"WaitForMultipleObjects")){
        uint32_t cnt=A(c,0), parr=A(c,1), waitall=A(c,2), ms=A(c,3);
        (void)waitall;
        ts_set(TS_WAITM, cnt && parr && mem_is_committed(parr+3) ? MR32(parr) : 0u);
        for (uint32_t i=0;i<cnt && i<64;i++) obj_get(MR32(parr+4u*i));   /* looked up under the guest lock: obj_reclaim */
        guest_yield_begin();
        uint32_t got=0x102u;
        for (int spin=0; spin<200; spin++){
            for (uint32_t i=0;i<cnt && i<64;i++){
                Obj*o=obj_get(MR32(parr+4u*i));
                if(o){ pthread_mutex_lock(&o->mtx);
                       int sig=o->signaled;
                       if(sig && !o->manual) o->signaled=0;
                       pthread_mutex_unlock(&o->mtx);
                       if(sig){ got=i; break; } }
            }
            if (got!=0x102u || ms==0) break;
            struct timespec ts={0,1000000L}; nanosleep(&ts,0);
        }
        guest_yield_end();
        r=got; }
    else if (!strcmp(n,"SwitchToThread")){ guest_yield_begin(); sched_yield(); guest_yield_end(); r=1; }
    else if (!strcmp(n,"CloseHandle")){ obj_close(A(c,0)); r=1; }
    else if (!strcmp(n,"DuplicateHandle")){ uint32_t p=A(c,3); if(p) MR32(p)=A(c,1);
        { Obj *o = obj_get(A(c,1)); if (o){ pthread_mutex_lock(&o->mtx); o->refs++; pthread_mutex_unlock(&o->mtx); } }
        r=1; }
    else if (!strcmp(n,"SetErrorMode")) r=0;
    else if (!strcmp(n,"GetLogicalDrives")) r=0x0000000Cu;             /* C: and D: */
    else if (!strcmp(n,"GetDriveTypeA")) r=3;                          /* DRIVE_FIXED */
    else if (!strcmp(n,"GetCurrentDirectoryA")){
        uint32_t p=A(c,1); const char*s=g_game.vroot; uint32_t l=(uint32_t)strlen(s);
        if(p){ mem_commit(p,l+1); memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"SetCurrentDirectoryA")) r=1;
    else if (!strcmp(n,"GetLongPathNameA")||!strcmp(n,"GetShortPathNameA")){
        uint32_t src=A(c,0), dst=A(c,1), sz=A(c,2);
        uint32_t l = src? (uint32_t)strlen((char*)(g_mem+src)) : 0;
        if(dst && sz>l){ memcpy(g_mem+dst, g_mem+src, l+1); }
        r=l; }
    else if (!strcmp(n,"GetSystemDirectoryA")||!strcmp(n,"GetWindowsDirectoryA")){
        uint32_t p=A(c,0); const char*s="C:\\Windows"; uint32_t l=(uint32_t)strlen(s);
        if(p){ mem_commit(p,l+1); memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"FileTimeToLocalFileTime")){     /* (in, out): add the CURRENT UTC offset, as Windows does */
        uint32_t a=A(c,0), b=A(c,1);
        if (a && b && mem_is_committed(a+7)){
            time_t now = time(0); struct tm lt; localtime_r(&now, &lt);
            uint64_t ft = (uint64_t)MR32(a) | ((uint64_t)MR32(a+4) << 32);
            ft += (uint64_t)((int64_t)lt.tm_gmtoff * 10000000LL);
            mem_commit(b,8); MR32(b)=(uint32_t)ft; MR32(b+4)=(uint32_t)(ft >> 32); r=1; }
        else r=0; }
    else if (!strcmp(n,"FileTimeToSystemTime")){        /* (FILETIME*, SYSTEMTIME*) */
        uint32_t a=A(c,0), b=A(c,1); r=0;
        if (a && b && mem_is_committed(a+7)){
            uint64_t ft = (uint64_t)MR32(a) | ((uint64_t)MR32(a+4) << 32);
            uint64_t ms = ft / 10000ull;
            time_t t = (time_t)(ms / 1000ull) - (time_t)11644473600LL;
            struct tm tmv; gmtime_r(&t, &tmv);
            mem_commit(b,16);
            MR16(b+0)=(uint16_t)(tmv.tm_year+1900); MR16(b+2)=(uint16_t)(tmv.tm_mon+1);
            MR16(b+4)=(uint16_t)tmv.tm_wday;         MR16(b+6)=(uint16_t)tmv.tm_mday;
            MR16(b+8)=(uint16_t)tmv.tm_hour;         MR16(b+10)=(uint16_t)tmv.tm_min;
            MR16(b+12)=(uint16_t)tmv.tm_sec;         MR16(b+14)=(uint16_t)(ms % 1000ull);
            r=1; } }
    else if (!strcmp(n,"GetSystemTimeAsFileTime")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,8);
            static uint64_t ft = 130000000000000000ull; ft += 100000;
            MR32(p)=(uint32_t)ft; MR32(p+4)=(uint32_t)(ft>>32); } r=0; }
    else if (!strcmp(n,"GetLocalTime")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,16); memset(g_mem+p,0,16);
            MR16(p)=2026; MR16(p+2)=8; MR16(p+6)=24; MR16(p+8)=12; } r=0; }
    else if (!strcmp(n,"GetSystemTime")){              /* the same date, moving with the virtual clock (vsys_ft) */
        uint32_t p = A(c,0); if (p) ft_to_systime(p, vsys_ft()); r = 0; }
    else if (!strcmp(n,"SystemTimeToFileTime")){       /* (const SYSTEMTIME *, FILETIME *) */
        uint32_t a = A(c,0), b = A(c,1); uint64_t ft = 0;
        if (a && b && mem_is_committed(a) && mem_is_committed(a + 15u) && systime_to_ft(a, &ft)){
            mem_commit(b, 8); MR32(b) = (uint32_t)ft; MR32(b+4) = (uint32_t)(ft >> 32); r = 1; }
        else { win32_set_last_error(c, 87u); r = 0; } }   /* ERROR_INVALID_PARAMETER */
    /* --- copy protection / anti-debug: return failure --- */
    else if (!strcmp(n,"CreateToolhelp32Snapshot")) r=0xFFFFFFFFu;
    else if (!strcmp(n,"Process32First")||!strcmp(n,"Process32Next")) r=0;
    else if (!strcmp(n,"IsDebuggerPresent")) r=0;
    /* --- resources --- */
    else if (!strcmp(n,"FindResourceA")) r = res_find(A(c,1), A(c,2), 0);
    else if (!strcmp(n,"FindResourceW")) r = res_find(A(c,1), A(c,2), 1);
    else if (!strcmp(n,"LoadResource"))  r = A(c,1);            /* pass the HRSRC through */
    else if (!strcmp(n,"LockResource"))  r = res_data(A(c,0));
    else if (!strcmp(n,"SizeofResource"))r = res_size(A(c,1));
    else if (!strcmp(n,"FreeResource"))  r = 1;
    else if (!strcmp(n,"GetSystemInfo")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,36); memset(g_mem+p,0,36);
            MR32(p+4)=4096; MR32(p+20)=10; MR32(p+32)=0x000006d8; } r=0; }
    else if (!strcmp(n,"GlobalMemoryStatus")){
        uint32_t p=A(c,0); if(p){ mem_commit(p,32); memset(g_mem+p,0,32);
            MR32(p)=32; MR32(p+4)=50; MR32(p+8)=0x40000000u; MR32(p+12)=0x20000000u; } r=0; }
    else if (!strcmp(n,"IsBadReadPtr")||!strcmp(n,"IsBadWritePtr")||!strcmp(n,"IsBadCodePtr")) r=0;
    else if (!strcmp(n,"wvsprintfA")){        /* (lpOut, lpFmt, arglist) */
        uint32_t o = A(c,0), f = A(c,1), va = A(c,2);
        r = (o && f && mem_is_committed(f)) ? guest_vformat(o, 1025u, (const char *)(g_mem + f), va) : 0; }
    else if (!strcmp(n,"wsprintfA")){         /* cdecl (lpOut, lpFmt, ...) */
        uint32_t o = A(c,0), f = A(c,1);
        r = (o && f && mem_is_committed(f)) ? guest_vformat(o, 1025u, (const char *)(g_mem + f), c->esp + 8u) : 0; }
    else if (!strcmp(n,"GetThreadPriority")) r = 0;            /* THREAD_PRIORITY_NORMAL */
    else if (!strcmp(n,"SetThreadPriority")) r = 1;
    else if (!strcmp(n,"GetTimeZoneInformation")) r = tz_info(A(c,0));
    else if (!strcmp(n,"GetDiskFreeSpaceExA")){
        /* (dir, &freeToCaller, &total, &totalFree), ULARGE_INTEGERs: the
         * volume that holds the saves.  RealCore's drive object (0x6FAEB3)
         * returns freeToCaller as a 64-bit value. */
        uint64_t fr = 0, tot = 0; r = disk_space(&fr, &tot);
        uint32_t pf = A(c,1), pt = A(c,2), pz = A(c,3);
        if (pf){ mem_commit(pf,8); MR32(pf)=(uint32_t)fr; MR32(pf+4)=(uint32_t)(fr>>32); }
        if (pt){ mem_commit(pt,8); MR32(pt)=(uint32_t)tot; MR32(pt+4)=(uint32_t)(tot>>32); }
        if (pz){ mem_commit(pz,8); MR32(pz)=(uint32_t)fr; MR32(pz+4)=(uint32_t)(fr>>32); } }
    else if (!strcmp(n,"GetDiskFreeSpaceA")){
        /* (root, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters):
         * 4 KB clusters, both counts capped below 2 GB so 32-bit products
         * (the CRT's _getdiskfree, old game code) cannot overflow. */
        uint64_t fr = 0, tot = 0; r = disk_space(&fr, &tot);
        uint32_t cf = (uint32_t)((fr >> 12) > 0x7FFFFu ? 0x7FFFFu : (fr >> 12));
        uint32_t ct = (uint32_t)((tot >> 12) > 0x7FFFFu ? 0x7FFFFu : (tot >> 12));
        uint32_t q[4] = { 8u, 512u, cf, ct };
        for (int i = 0; i < 4; i++){ uint32_t pp = A(c, 1 + i); if (pp){ mem_commit(pp,4); MR32(pp) = q[i]; } } }
    else if (!strcmp(n,"DeleteObject")) r = 1;
    else if (!strcmp(n,"BeginPaint")){ uint32_t ps=A(c,1);
        if (ps){ mem_commit(ps,64); memset(g_mem+ps,0,64); } r = MM_HDC; }
    else if (!strcmp(n,"EndPaint")) r = 1;
    else if (!strcmp(n,"OutputDebugStringA")){ uint32_t p=A(c,0);
        if(p) fprintf(stderr,"[dbg] %s", (char*)(g_mem+p)); r=0; }
    else if (!strcmp(n,"timeBeginPeriod")||!strcmp(n,"timeEndPeriod")) r=0;
    else if (!strcmp(n,"timeGetTime")){ uint32_t host_ms(void); r = host_ms(); }
    else if (!strcmp(n,"timeSetEvent")){
        /* (uDelay, uResolution, lpTimeProc, dwUser, fuEvent) */
        uint32_t delay=A(c,0), proc=A(c,2), user=A(c,3), flags=A(c,4);
        uint32_t id=0;
        if (proc && !getenv("NOTIMER")){
            pthread_mutex_lock(&g_lock);
            if (!g_timer_running){
                g_timer_stack = stack_take();
                if (!g_timer_stack){
                    static int said = 0;      /* no timer thread yet: a later timeSetEvent tries again */
                    if (said++ < 20) fprintf(stderr, "[timer] REFUSED: no thread stack left for the timer thread"
                                                     " (all %u of 0x%08x-0x%08x in use)\n",
                                             (MM_TSTACK_HI - MM_TSTACK_LO) / GTHREAD_STACK, MM_TSTACK_LO, MM_TSTACK_HI);
                } else {
                    g_timer_running = 1;
                    pthread_t th2;
                    if (pthread_create(&th2,0,timer_thread_main,0)==0) pthread_detach(th2);
                    else { g_timer_running = 0; g_stk_free[g_nstk_free++] = g_timer_stack; }
                }
            }
            for (int i=0;i<MAXTIMER;i++){
                if (g_tm[i].due_us) continue;
                uint32_t us = delay ? delay*1000u : 1000u;
                g_tm[i].proc=proc; g_tm[i].user=user;
                g_tm[i].id = id = g_timer_next_id++;
                g_tm[i].period_us = (flags & 1u) ? us : 0u;
                g_tm[i].due_us = now_us() + us;
                break;
            }
            pthread_mutex_unlock(&g_lock);
            { static int sh2=0; if (sh2<3){ sh2++;
                fprintf(stderr,"  [timer] timeSetEvent %u ms, %s, callback 0x%08x\n",
                        delay, (flags&1u)?"periodic":"one-shot", proc); } }
        }
        r=id; }
    else if (!strcmp(n,"timeKillEvent")){
        uint32_t id=A(c,0);
        pthread_mutex_lock(&g_lock);
        for (int i=0;i<MAXTIMER;i++)
            if (g_tm[i].due_us && g_tm[i].id==id){ g_tm[i].due_us=0; break; }
        pthread_mutex_unlock(&g_lock);
        r=0; }
    else if (!strcmp(n,"timeGetDevCaps")){
        uint32_t p=A(c,0);
        if(p){ mem_commit(p,8); MR32(p)=1u; MR32(p+4)=1000000u; }
        r=0; }
    else if (!strcmp(n,"timeBeginPeriod")||!strcmp(n,"timeEndPeriod")) r=0;
    else if (!strcmp(n,"joyGetNumDevs")) r=0;
    /* --- REAL critical sections ---------------------------------------
     * With a no-op stub the GIL's fair sharing (it releases the lock every
     * 1024 Win32 calls) breaks mutual exclusion: a thread can lose the lock
     * while inside a critical section.                                  */
    else if (!strcmp(n,"InitializeCriticalSection")||
             !strcmp(n,"InitializeCriticalSectionAndSpinCount")){
        (void)cs_for(A(c,0)); r=1; }
    else if (!strcmp(n,"EnterCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        if (m){
            g_cs_enter++;
            if (pthread_mutex_trylock(m) != 0){
                g_cs_contended++;
                ts_set(TS_CS, A(c,0));
                guest_yield_begin();          /* release the GIL while waiting */
                pthread_mutex_lock(m);
                guest_yield_end();
            }
            tl_cs_depth++;
            cs_owned(m, cur_tid(c), +1);
        }
        r=1; }
    else if (!strcmp(n,"TryEnterCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        r=1;
        if (m){ if (pthread_mutex_trylock(m)==0){ tl_cs_depth++; cs_owned(m, cur_tid(c), +1); } else r=0; } }
    else if (!strcmp(n,"LeaveCriticalSection")){
        pthread_mutex_t *m = cs_for(A(c,0));
        if (m && tl_cs_depth > 0){ tl_cs_depth--; cs_owned(m, 0, -1); pthread_mutex_unlock(m); }
        r=1; }
    else if (!strcmp(n,"DeleteCriticalSection")) r=1;
    else return 0;                       /* not this layer's business */
    *out = r;
    return 1;
}
uint64_t win32_ext_threads(void){ return g_threads_started; }
int win32_threads_live(void){ return (int)(g_threads_started - g_threads_exited); }
/* [status] obj=: kernel-object slots in use (events, mutexes, keys, ...). */
int win32_obj_live(void){
    int n = 0;
    for (int i = 1; i < g_obj_n && i < OBJ_MAX; i++) if (g_obj[i].kind != OB_NONE) n++;
    return n;
}
uint64_t win32_ext_threads_exited(void){ return g_threads_exited; }
