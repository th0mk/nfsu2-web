/* Debugging: guest watchpoint + instruction hooks */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


/* resolve the nearest function with the function address table */
extern const uint32_t g_fn_addr[]; extern const uint32_t g_fn_count;
static uint32_t near_fn(uint32_t a){
    uint32_t lo=0, hi=g_fn_count, best=0;
    while(lo<hi){ uint32_t m=(lo+hi)/2;
        if(g_fn_addr[m]<=a){ best=g_fn_addr[m]; lo=m+1; } else hi=m; }
    return best;
}

void itrace_dump(int n){
    if (!g_itrace_n){ fprintf(stderr,"  (instruction trace empty - not recorded without WATCH=1)\n"); return; }
    uint64_t start = g_itrace_n > (uint64_t)n ? g_itrace_n - n : 0;
    fprintf(stderr,"\n=== LAST %llu GUEST INSTRUCTIONS (executed in total: %llu) ===\n",
            (unsigned long long)(g_itrace_n-start), (unsigned long long)g_itrace_n);
    uint32_t last_fn = 0;
    for (uint64_t k=start; k<g_itrace_n; k++){
        uint32_t ip = g_itrace[(uint32_t)k & ITRACE_MASK];
        uint32_t f  = near_fn(ip);
        if (f != last_fn){ fprintf(stderr,"  --- sub_%08x ---\n", f); last_fn = f; }
        fprintf(stderr,"    %llu  0x%08x  (+0x%x)\n",
                (unsigned long long)k, ip, ip - f);
    }
}

#define WRING 128
typedef struct { uint32_t ip, oldv, newv; uint64_t seq; } WEnt;
static WEnt g_ring[WRING];
static uint64_t g_wcount = 0;

extern void (*g_itrace_dump_fn)(int);
void watch_report_change(uint32_t culprit_ip, uint32_t oldv, uint32_t newv){
    g_ring[g_wcount % WRING] = (WEnt){culprit_ip, oldv, newv, g_wcount};
    g_wcount++;
    /* On the first change dump the instruction trace: it shows exactly
     * which call chain led to the write. */
    if (g_wcount == 1 && getenv("WATCH_ITRACE")){
        fprintf(stderr,"\n*** watchpoint first change: 0x%08x  %08x -> %08x\n",
                culprit_ip, oldv, newv);
        itrace_dump(getenv("WATCH_ITRACE_N") ? atoi(getenv("WATCH_ITRACE_N")) : 120);
    }
}

static void __attribute__((constructor)) _dbg_init(void){ g_itrace_dump_fn = itrace_dump; }
void mem_commit(uint32_t,uint32_t);
void watch_arm(uint32_t addr){
    if (addr) mem_commit(addr & ~0x3FFFu, 0x4000u);   /* make sure the watched page exists */
    g_watch_addr = addr;
    g_watch_val  = addr ? MR32(addr) : 0;
    g_wcount = 0;
    fprintf(stderr,"watchpoint: guest 0x%08x armed (initial value 0x%08x)\n", addr, g_watch_val);
}

void watch_dump(const char *why, int maxn){
    fprintf(stderr,"\n=== WATCHPOINT DUMP (%s) ===\n", why);
    fprintf(stderr,"  watched address: 0x%08x   changes in total: %llu\n",
            g_watch_addr, (unsigned long long)g_wcount);
    uint64_t start = g_wcount > (uint64_t)maxn ? g_wcount - maxn : 0;
    for (uint64_t k = start; k < g_wcount; k++){
        WEnt *e = &g_ring[k % WRING];
        fprintf(stderr,"  #%-6llu insn 0x%08x   0x%08x -> 0x%08x\n",
                (unsigned long long)e->seq, e->ip, e->oldv, e->newv);
    }
}

/* --- instruction hooks ---
 * The lift emits guest_hook(c, va) before every hooked instruction; which
 * sites are hooked, and what they do, is the game's (runtime/game.h). */
uint64_t g_hook_hits = 0, g_gs_fail = 0;
static uint64_t g_frame_passes;      /* the main loop's passes through frame_site (game.h) */
uint64_t debug_frame_passes(void){ return g_frame_passes; }
void guest_hook(CPU *c, uint32_t ip){
    g_hook_hits++;
    if (ip == g_game.frame_site && ip){
        g_frame_passes++;
        if (g_game.on_frame) g_game.on_frame(c);
        return;
    }
    if (g_game.hook) g_game.hook(c, ip);
}


/* ---- function coverage buffer ------------------------------------------ */
#define COV_SIZE DISPATCH_TEXT_SIZE          /* one counter per byte of the exe's .text (cpu.h) */
uint32_t *g_cov = 0;
uint32_t g_last_fn = 0;
int      g_prof_on = 0;
static void __attribute__((constructor)) _cov_init(void){
    if (getenv("COVREPORT")){ g_cov = calloc(COV_SIZE, sizeof *g_cov); g_prof_on = 1; }
}
void cov_reset(void){
    if (g_cov) memset(g_cov, 0, (size_t)COV_SIZE * sizeof *g_cov);
}
/* Plain dump (no top-30 list, counts kept): autotest "covdump". */
void cov_dump(const char *path){
    if (!g_cov) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned long n = 0;
    for (uint32_t i = 0; i < COV_SIZE; i++)
        if (g_cov[i]){ fprintf(f, "%08x %u\n", DISPATCH_TEXT_BASE + i, g_cov[i]); n++; }
    fclose(f);
    fprintf(stderr, "  coverage: %lu functions -> %s\n", n, path);
}
void cov_report(const char *path){
    if (!g_cov) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned long n = 0;
    for (uint32_t i = 0; i < COV_SIZE; i++)
        if (g_cov[i]){ fprintf(f, "%08x %u\n", DISPATCH_TEXT_BASE + i, g_cov[i]); n++; }
    fclose(f);
    fprintf(stderr, "  coverage: %lu functions ran -> %s\n", n, path);
    /* MOST CALLED: a direct measurement of "what is the game doing?".
     * COVTOP=<n> lists n of them (30; tools/inline_pick.py reads ~1000). */
    { unsigned long long tot = 0; for (uint32_t i = 0; i < COV_SIZE; i++) tot += g_cov[i];
      fprintf(stderr, "  coverage: %llu calls in all\n", tot); }
    const char *ct = getenv("COVTOP");
    int top = ct && *ct ? atoi(ct) : 30;
    fprintf(stderr, "  --- MOST CALLED GUEST FUNCTIONS ---\n");
    for (int k = 0; k < top; k++){
        uint32_t bi = 0, best = 0;
        for (uint32_t i = 0; i < COV_SIZE; i++)
            if (g_cov[i] > best){ best = g_cov[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "    %10u  sub_%08x\n", best, DISPATCH_TEXT_BASE + bi);
        g_cov[bi] = 0;
    }
}
