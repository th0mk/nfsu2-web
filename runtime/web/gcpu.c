/* wasm: guest CPU time per frame, the key number for 120 fps.
 *
 * The Mac reads the guest main thread's CPU clock at every Present
 * ([status] gcpu, PERFLOG guest.csv).  The browser has no thread CPU clock,
 * so the busy time comes from the guest lock (win32_ext.c
 * win32_thread_busy_ms: time holding the lock minus host waits; pacer
 * sleeps, Sleep/Wait* and the present wait all release the lock).
 *
 * Per Present (autotest_on_present, the guest main thread) the frame's busy
 * time goes into a histogram per game flow (game.h flow: the game's
 * GameFlowManager state, front end and racing named) and into a window for
 * the next [status] line, which then
 * shows its distribution: gp=p50/p95/p99/max ms.  The exit report prints the
 * distribution per flow.
 *
 * NFSU2_GCPU_FRAMES=1 (the perf track's benchmark, web/tools/bench.mjs) also
 * keeps every racing frame's busy time and Present time and prints them at
 * exit as "[gcpuf]" lines ("busy_us@present_ms_since_first;...", after a
 * "[gcpuf0] <first Present, epoch ms>" line), so the
 * benchmark can split the race into tier-up and warm parts and count late
 * frames itself. */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

double win32_thread_busy_ms(void);

/* Buckets: 0.025 ms wide up to 50 ms, then 5 % wide (geometric) up to
 * 50 * 1.05^NL = 123 s; a percentile is its bucket's midpoint (at most
 * 0.0125 ms or 2.5 % off), never more than the maximum.  A frame above
 * the last bucket (it would be a hang) reads as the maximum. */
#define BW   0.025                      /* linear bucket width, ms */
#define NB   2000                       /* linear: [0, 50) ms */
#define LO   (NB * BW)                  /* 50 ms */
#define GR   1.05                       /* geometric step above that */
#define NL   160                        /* geometric: [50 ms, 123 s) */
typedef struct { uint64_t n; double sum, max; uint32_t h[NB + NL + 1]; } Dist;
static Dist  g_flow[8];
static float g_win[16384]; static int g_nwin;
static double g_last = -1;
static int g_fr_on = -1;                /* NFSU2_GCPU_FRAMES */
static struct { float busy; double t; } *g_fr; static int g_nfr, g_cfr;
static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

static void dist_add(Dist *d, double ms){
    d->n++; d->sum += ms; if (ms > d->max) d->max = ms;
    int k;
    if (!(ms >= 0)) k = 0;
    else if (ms < LO) k = (int)(ms / BW);
    else { k = NB + (int)(log(ms / LO) / log(GR)); if (k > NB + NL) k = NB + NL; }
    d->h[k]++;
}
static double bucket_mid(int k){
    if (k < NB) return (k + 0.5) * BW;
    return LO * pow(GR, (double)(k - NB) + 0.5);
}
static double dist_pct(const Dist *d, double p){
    uint64_t want = (uint64_t)(p * (double)d->n + 0.5), acc = 0;
    if (!want) want = 1;
    for (int k = 0; k <= NB + NL; k++){
        acc += d->h[k];
        if (acc >= want){ double v = k < NB + NL ? bucket_mid(k) : d->max; return v < d->max ? v : d->max; }
    }
    return d->max;
}

void gcpu_frame(void){
    double b = win32_thread_busy_ms();
    if (g_last >= 0){
        double ms = b - g_last;
        uint32_t flow = game_flow();
        dist_add(&g_flow[flow & 7], ms);
        if (g_nwin < (int)(sizeof g_win / sizeof g_win[0])) g_win[g_nwin++] = (float)ms;
        if (g_fr_on < 0){ const char *e = getenv("NFSU2_GCPU_FRAMES"); g_fr_on = e && *e == '1'; }
        if (g_fr_on && (flow & 7) == g_game.flow_race){
            if (g_nfr == g_cfr){ int nc = g_cfr ? 2 * g_cfr : 16384; void *p = realloc(g_fr, (size_t)nc * sizeof *g_fr);
                if (p){ g_fr = p; g_cfr = nc; } }
            if (g_nfr < g_cfr){ g_fr[g_nfr].busy = (float)ms; g_fr[g_nfr].t = now_ms(); g_nfr++; }
        }
    }
    g_last = b;
}

static int fcmp(const void *a, const void *b){ float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
/* " gp=p50/p95/p99/max" over the frames since the last call */
int gcpu_status(char *buf, size_t n){
    if (!g_nwin) return snprintf(buf, n, " gp=-");
    qsort(g_win, (size_t)g_nwin, sizeof g_win[0], fcmp);
    #define Q(p) g_win[(int)((p) * (g_nwin - 1) + 0.5)]
    int k = snprintf(buf, n, " gp=%.2f/%.2f/%.2f/%.2fms", Q(0.5), Q(0.95), Q(0.99), g_win[g_nwin - 1]);
    #undef Q
    g_nwin = 0;
    return k;
}

void gcpu_report(void){
    fprintf(stderr, "\n  guest main thread busy time per frame (ms; the guest lock held minus host waits):\n");
    for (int f = 0; f < 8; f++){
        const Dist *d = &g_flow[f];
        if (!d->n) continue;
        char nm[32];
        snprintf(nm, sizeof nm, "flow %d%s", f, !g_game.flow ? "" : (uint32_t)f == g_game.flow_fe ? " (front end)"
                                                 : (uint32_t)f == g_game.flow_race ? " (racing)" : "");
        fprintf(stderr, "    %-18s %7llu frames: mean %.2f  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n", nm,
                (unsigned long long)d->n, d->sum / (double)d->n, dist_pct(d, 0.5), dist_pct(d, 0.95), dist_pct(d, 0.99), d->max);
    }
    if (g_nfr){
        char line[4096]; int k = 0;
        fprintf(stderr, "[gcpuf0] %.3f\n", g_fr[0].t);      /* the first racing frame's Present, epoch ms */
        for (int i = 0; i < g_nfr; i++){
            k += snprintf(line + k, sizeof line - (size_t)k, "%s%d@%.2f", k ? ";" : "", (int)(g_fr[i].busy * 1000.0f + 0.5f),
                          g_fr[i].t - g_fr[0].t);
            if (k > (int)sizeof line - 64 || i == g_nfr - 1){ fprintf(stderr, "[gcpuf] %s\n", line); k = 0; }
        }
    }
}
