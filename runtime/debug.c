/* Debugging: guest watchpoint + instruction hooks */
#include "cpu.h"
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
void guest_backtrace(CPU*,int);

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

/* --- instruction hooks --- */
uint64_t g_hook_hits = 0, g_gs_fail = 0;
static int g_gs_reported = 0;

static const struct { uint32_t ip; const char *what; } g_hook_names[] = {
    {0x0076115eu,"__security_check_cookie entry"},
    {0x00761167u,"jne -> __report_gsfailure"},
    {0x0076112du,"__report_gsfailure body"},
    {0x00761141u,"call reporter(1,0)"},
    {0x0076c25cu,"reporter function entry"},
    {0x0076c2b9u,"took the 'Buffer overrun' branch"},
    {0x0076c2a0u,"reporter: error type switch"},
};
/* --- recursion hunter: sub_0065ae25 --- */
typedef struct { uint32_t a1,a3,esp,cnt,esi,cntw,idx0,s4,eax,e48,edi,edival,inner; } RecEnt;
#define RECR 32
static RecEnt g_rec[RECR];
static uint32_t g_rec_n = 0, g_rec_esp0 = 0;
void guest_hook(CPU *c, uint32_t ip){
    g_hook_hits++;
    { int splitscreen_hook(CPU *, uint32_t); if (splitscreen_hook(c, ip)) return; }
    { int coop_hook(CPU *, uint32_t); if (coop_hook(c, ip)) return; }
    { int ws_hook(CPU *, uint32_t); if (ws_hook(c, ip)) return; }
    { int roam_hook(CPU *, uint32_t); if (roam_hook(c, ip)) return; }
    { int language_hook(CPU *, uint32_t); if (language_hook(c, ip)) return; }
    { int hornmenu_hook(CPU *, uint32_t); if (hornmenu_hook(c, ip)) return; }
    { int tonemenu_hook(CPU *, uint32_t); if (tonemenu_hook(c, ip)) return; }
    { int powerunits_hook(CPU *, uint32_t); if (powerunits_hook(c, ip)) return; }
    { int shownames_hook(CPU *, uint32_t); if (shownames_hook(c, ip)) return; }
    { int hudbank_hook(CPU *, uint32_t); if (hudbank_hook(c, ip)) return; }
    { int regional_hook(CPU *, uint32_t); if (regional_hook(c, ip)) return; }
    { int garage_hook(CPU *, uint32_t); if (garage_hook(c, ip)) return; }
    { int vinylcat_hook(CPU *, uint32_t); if (vinylcat_hook(c, ip)) return; }
    { int sndlead_hook(CPU *, uint32_t); if (sndlead_hook(c, ip)) return; }
    { int drift_hook(CPU *, uint32_t); if (drift_hook(c, ip)) return; }
    { int quickrace_hook(CPU *, uint32_t); if (quickrace_hook(c, ip)) return; }
    { int trafficcars_hook(CPU *, uint32_t); if (trafficcars_hook(c, ip)) return; }
    if (ip == 0x0065ae25u){
        uint32_t a1=MR32(c->esp+4u), a2=MR32(c->esp+8u), a3=MR32(c->esp+12u);
        if (!g_rec_esp0) g_rec_esp0 = c->esp;
        uint32_t esi_ = MR32(c->ecx+0x2cu) + a1*0x64u;
        uint32_t cnt_ = mem_is_committed(esi_+0x34) ? MR16(esi_+0x34) : 0xFFFFu;
        uint32_t ebx_ = (mem_is_committed(esi_+0x14)&&mem_is_committed(esi_))
                        ? (MR32(esi_+0x14) + MR32(esi_)) : 0;
        uint32_t idx0 = (ebx_ && mem_is_committed(ebx_)) ? MR16(ebx_) : 0xFFFFu;
        uint32_t s4   = mem_is_committed(esi_+4) ? MR32(esi_+4) : 0;
        uint32_t sbase= mem_is_committed(esi_)   ? MR32(esi_)   : 0;
        uint32_t eaxv = idx0*0x64u + s4;
        uint32_t e48  = mem_is_committed(eaxv+0x48) ? MR32(eaxv+0x48) : 0;
        uint32_t edi_ = e48 + sbase;
        uint32_t ediw = mem_is_committed(edi_) ? MR16(edi_) : 0xFFFFu;
        uint32_t pc   = mem_is_committed(eaxv+0xc) ? MR32(eaxv+0xc) : 0;
        uint32_t inner= (pc && mem_is_committed(pc+0x10)) ? MR32(pc+0x10) : 0;
        (void)a2;
        g_rec[g_rec_n % RECR] = (RecEnt){a1,a3 & 0xffffu,c->esp,g_rec_n,esi_,cnt_,idx0,s4,eaxv,e48,edi_,ediw,inner};
        g_rec_n++;
        uint32_t depth = (g_rec_esp0 > c->esp) ? (g_rec_esp0 - c->esp) : 0;
        if (depth > 0x400000u){       /* 4 MB of stack used = runaway recursion */
            fprintf(stderr,"\n*** RUNAWAY RECURSION: %u calls, %u KB of stack used\n",
                    g_rec_n, depth/1024u);
            fprintf(stderr,"    last %d calls (arg1, arg2, arg3, esp):\n", RECR);
            uint32_t start = g_rec_n > RECR ? g_rec_n - RECR : 0;
            for (uint32_t k=start;k<g_rec_n;k++){
                RecEnt *e=&g_rec[k % RECR];
                fprintf(stderr,"      #%-7u a1=%-4u outerCnt=%-3u idx0=%-4u [esi+4]=0x%08x"
                               " eax=0x%08x [eax+0x48]=0x%08x edi=0x%08x [edi]=%-5u innerCnt=%u\n",
                        e->cnt, e->a1, e->cntw, e->idx0, e->s4, e->eax, e->e48,
                        e->edi, e->edival, e->inner);
            }
            itrace_dump(12);
            if (g_watch_addr) watch_dump("at the recursion", 20);
            _exit(5);
        }
        return;
    }
    if (ip == 0x0066887eu){
        /* movzx esi, word [edi+4] : type/class key */
        static int n=0;
        if (n<40){
            uint32_t e = c->edi;
            uint32_t cls = mem_is_committed(e+4) ? MR16(e+4) : 0xFFFFu;
            uint32_t t   = mem_is_committed(e)   ? MR16(e)   : 0xFFFFu;
            uint32_t el  = mem_is_committed(e+6) ? MR16(e+6) : 0xFFFFu;
            fprintf(stderr,"  [type %2d] edi=0x%08x  [edi]=%-5u [edi+4]=%-5u [edi+6]=%-5u\n",
                    n, e, t, cls, el);
            n++;
        }
        return;
    }
    if (ip == 0x0076115eu){                       /* __security_check_cookie entry */
        uint32_t expect = MR32(0x0081a1d0u);
        if (c->ecx != expect){
            g_gs_fail++;
            if (g_gs_reported < 3){
                g_gs_reported++;
                uint32_t cookie_slot = c->ebp - 4u;
                fprintf(stderr,"\n*** /GS COOKIE FAILURE ***\n");
                fprintf(stderr,"  ecx (stack^ret)   = 0x%08x\n", c->ecx);
                fprintf(stderr,"  expected (global) = 0x%08x\n", expect);
                fprintf(stderr,"  ebp               = 0x%08x\n", c->ebp);
                fprintf(stderr,"  COOKIE SLOT       = 0x%08x  (value 0x%08x)\n",
                        cookie_slot, MR32(cookie_slot));
                fprintf(stderr,"  return address [ebp+4] = 0x%08x\n", MR32(c->ebp + 4u));
                fprintf(stderr,"  slot ^ return          = 0x%08x\n",
                        MR32(cookie_slot) ^ MR32(c->ebp + 4u));
                guest_backtrace(c, 8);
                if (g_watch_addr) watch_dump("cookie check failed", 24);
            }
        }
    }
}


/* ---- function coverage buffer ------------------------------------------ */
#define COV_SIZE 3676913u
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
        if (g_cov[i]){ fprintf(f, "%08x %u\n", 0x00401000u + i, g_cov[i]); n++; }
    fclose(f);
    fprintf(stderr, "  coverage: %lu functions -> %s\n", n, path);
}
void cov_report(const char *path){
    if (!g_cov) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    unsigned long n = 0;
    for (uint32_t i = 0; i < COV_SIZE; i++)
        if (g_cov[i]){ fprintf(f, "%08x %u\n", 0x00401000u + i, g_cov[i]); n++; }
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
        fprintf(stderr, "    %10u  sub_%08x\n", best, 0x00401000u + bi);
        g_cov[bi] = 0;
    }
}
