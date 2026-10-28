/* Indirect call dispatch: guest address -> lifted native function.
 * O(1) direct table the size of .text: the function itself (or the run-time
 * patch that replaces it), so a hit is one load and one call.          */
#include "cpu.h"
uint64_t g_icall_hits = 0;
#include <stdio.h>
#include <stdlib.h>
#include "game.h"
#include "module.h"

typedef void (*guest_fn)(CPU*);
extern const uint32_t g_fn_addr[];
extern const guest_fn g_fn_ptr[];
extern const uint32_t g_fn_count;

#define TEXT_BASE DISPATCH_TEXT_BASE
#define TEXT_SIZE DISPATCH_TEXT_SIZE

guest_fn_t g_dtab[DISPATCH_TEXT_SIZE];   /* cpu.h */
static int g_dtab_ready;
static uint32_t g_icall_want;         /* ICALL=<address> (diagnostics), 0 = off */
static void icall_note(CPU *c, uint32_t addr);
/* ICALL and EFXSTAT watch indirect calls to some functions: those stay out of
 * the table, so the lifted code's inline fast path (cpu.h) passes them to
 * dispatch_indirect, which finds them here. */
static int g_diag;
static int g_init_on;                         /* the initializer tables are being counted (below) */
extern int g_efx_stat;                        /* EFXSTAT (d3dx_hle.c) */
int efx_watched(uint32_t a);                  /* EFXSTAT watches it (UG2's D3DX code: d3dx_hle.c) */
void efx_dispatch(CPU *c, uint32_t a);        /* an indirect call into that code (d3dx_hle.c) */
static int diag_watched(uint32_t a){
    return (g_icall_want && a == g_icall_want) || efx_watched(a);
}
static guest_fn diag_fn(uint32_t a){
    uint32_t lo = 0, hi = g_fn_count;
    while (lo < hi){ uint32_t m = (lo + hi) / 2; if (g_fn_addr[m] < a) lo = m + 1; else hi = m; }
    return lo < g_fn_count && g_fn_addr[lo] == a ? g_fn_ptr[lo] : 0;
}

/* statistics */
uint64_t g_stat_indirect_ok = 0, g_stat_indirect_miss = 0, g_stat_unsupported = 0;
#define MISS_MAX 64
static uint32_t g_miss[MISS_MAX], g_miss_from[MISS_MAX]; static int g_miss_n = 0;
extern uint32_t g_watch_prev_ip;
static uint32_t g_unsup[MISS_MAX]; static int g_unsup_n = 0;

extern void (*g_backtrace_fn)(struct CPU*,int);
void guest_backtrace(CPU*,int);

static void patch_apply(void);
void dispatch_init(void){
    if (g_dtab_ready) return;
    g_dtab_ready = 1;
    g_backtrace_fn = (void(*)(struct CPU*,int))guest_backtrace;
    { const char *e = getenv("ICALL");
      g_icall_want = e ? (uint32_t)strtoul(e,0,0) : 0;
      g_diag = g_icall_want || g_efx_stat; }
    uint32_t dup = 0, oob = 0;
    for (uint32_t i = 0; i < g_fn_count; i++){
        uint32_t a = g_fn_addr[i];
        if (a - TEXT_BASE >= TEXT_SIZE){ oob++; continue; }
        if (g_dtab[a - TEXT_BASE]) dup++;
        g_dtab[a - TEXT_BASE] = g_fn_ptr[i];
    }
    patch_apply();
    fprintf(stderr, "dispatch: %u functions registered (%u duplicates, %u out of range), table %.1f MB\n",
            g_fn_count, dup, oob, TEXT_SIZE*(double)sizeof(guest_fn)/1048576.0);
}

int  win32_call(CPU*, uint32_t);
int  run_generated(CPU*, uint32_t);
void genexec_report(void);
int  com_call(CPU*, uint32_t);
int  g_guest_exited = 0;

/* --- run-time patches: replace indirect call targets ---
 * Without regenerating (only functions that are called indirectly).  A
 * patch inside .text goes into the dispatch table itself (at dispatch_init,
 * or at once when that has run); one elsewhere (a lifted DLL) is looked up
 * in this list before the DLL's own functions. */
#define PATCH_MAX 24
static struct { uint32_t addr; guest_fn fn; } g_patch[PATCH_MAX];
static int g_patch_n = 0;
static void patch_apply(void){
    for (int i = 0; i < g_patch_n; i++)
        if (g_patch[i].addr - TEXT_BASE < TEXT_SIZE) g_dtab[g_patch[i].addr - TEXT_BASE] = g_patch[i].fn;
    if (g_diag)
        for (uint32_t i = 0; i < g_fn_count; i++)
            if (g_fn_addr[i] - TEXT_BASE < TEXT_SIZE && diag_watched(g_fn_addr[i])) g_dtab[g_fn_addr[i] - TEXT_BASE] = 0;
}
void dispatch_patch(uint32_t a, guest_fn f){
    if (a >= 0xE0000000u){        /* the order in dispatch_indirect relies on this */
        fprintf(stderr,"dispatch_patch: address %08x outside the text range, ignored\n", a);
        return; }
    if (g_patch_n < PATCH_MAX){ g_patch[g_patch_n].addr=a; g_patch[g_patch_n].fn=f; g_patch_n++; }
    if (g_dtab_ready) patch_apply();
}
static guest_fn patch_at(uint32_t addr){
    for (int i = 0; i < g_patch_n; i++) if (g_patch[i].addr == addr) return g_patch[i].fn;
    return 0;
}
/* a .text function the table leaves out for ICALL/EFXSTAT (patch_apply) or
 * while the initializer tables are counted (below) */
static guest_fn diag_lookup(uint32_t a){
    if (!g_init_on && !diag_watched(a)) return 0;
    guest_fn f = patch_at(a);
    return f ? f : diag_fn(a);
}

/* --- the C runtime's initializer tables, counted (game.h init_tables) ---
 * L-051: a recompiler that misses the shape of _initterm's call leaves every
 * global constructor unrun, and globals keep null pointers.  Each table's
 * runner calls its slots one by one through `call [edx]`, so while the tables
 * run their functions stay out of g_dtab: those calls come to
 * dispatch_indirect, which counts the ones the runner makes (its return
 * address on the stack, edx a slot of the table) and makes the call.  When a
 * table's last function has returned, or a function of a stopping table
 * returned non-zero, its functions go back into the table; after the last
 * table one [init] line says how many of each table's functions ran and reads
 * the pointer the constructors set.  initcount_report prints the line at exit
 * if the tables never finished (a boot that stopped in them). */
#define INIT_MAX 4
static struct { uint32_t want, ran, last, stop_slot, stop_eax; int watched, done, stopped; } g_init[INIT_MAX];
static int g_init_n, g_init_printed;
static int init_table_lifted(uint32_t f){ return f - TEXT_BASE < TEXT_SIZE && diag_fn(f); }
/* f is a function of another table still counted than t */
static int init_counted_elsewhere(uint32_t f, int t){
    for (int k = 0; k < g_init_n; k++){
        if (k == t || !g_init[k].watched) continue;
        for (uint32_t s = g_game.init_tables[k].lo; s < g_game.init_tables[k].hi; s += 4u)
            if (mem_is_committed(s) && MR32(s) == f) return 1;
    }
    return 0;
}
/* while counting, table t's functions call through dispatch_indirect; after, through g_dtab again */
static void init_watch(int t, int on){
    const InitTable *it = &g_game.init_tables[t];
    for (uint32_t s = it->lo; s < it->hi; s += 4u){
        uint32_t f = mem_is_committed(s) ? MR32(s) : 0u;
        if (!f || !init_table_lifted(f)) continue;
        if (on) g_dtab[f - TEXT_BASE] = 0;
        else if (!diag_watched(f) && !init_counted_elsewhere(f, t)){
            guest_fn p = patch_at(f); g_dtab[f - TEXT_BASE] = p ? p : diag_fn(f); }
    }
    g_init[t].watched = on;
}
static void init_line(const char *why){
    if (g_init_printed || !g_init_n) return;
    g_init_printed = 1;
    char b[640]; int k = snprintf(b, sizeof b, "[init]");
    for (int t = 0; t < g_init_n && k < (int)sizeof b - 160; t++){
        const InitTable *it = &g_game.init_tables[t];
        k += snprintf(b + k, sizeof b - (size_t)k, "%s %u of %u %s (%s 0x%08x-0x%08x, %u slots, by 0x%08x from 0x%08x",
                      t ? "," : "", g_init[t].ran, g_init[t].want, it->what, it->name, it->lo, it->hi,
                      (it->hi - it->lo) / 4u, it->runner, it->from);
        if (g_init[t].stopped)
            k += snprintf(b + k, sizeof b - (size_t)k, "; stopped: slot 0x%08x returned 0x%08x", g_init[t].stop_slot, g_init[t].stop_eax);
        else if (!g_init[t].done) k += snprintf(b + k, sizeof b - (size_t)k, g_init[t].ran ? "; not finished" : "; not run");
        k += snprintf(b + k, sizeof b - (size_t)k, ")");
    }
    if (g_game.init_check){
        uint32_t v = mem_is_committed(g_game.init_check) ? MR32(g_game.init_check) : 0u;
        if (v) snprintf(b + k, sizeof b - (size_t)k, "; [0x%08x] = 0x%08x, set", g_game.init_check, v);
        else snprintf(b + k, sizeof b - (size_t)k, "; [0x%08x] = 0: NULL, a constructor did not run its body", g_game.init_check);
    }
    fprintf(stderr, "%s%s%s\n", b, why ? "; " : "", why ? why : "");
}
static void init_finish(const char *why){
    for (int t = 0; t < g_init_n; t++) if (g_init[t].watched) init_watch(t, 0);
    g_init_on = 0;
    g_diag = g_icall_want || g_efx_stat;
    init_line(why);
}
/* the call into f at addr, which dispatch_indirect found while counting */
static void init_call(CPU *c, uint32_t addr, guest_fn f){
    uint32_t ret = MR32(c->esp), slot = c->edx;
    int t = 0;
    for (; t < g_init_n; t++){
        const InitTable *it = &g_game.init_tables[t];
        if (g_init[t].watched && ret == it->ret && slot >= it->lo && slot < it->hi && !((slot - it->lo) & 3u)
            && MR32(slot) == addr) break;
    }
    if (t == g_init_n){ f(c); return; }           /* another call into one of them: not the runner's */
    g_init[t].ran++;
    f(c);
    if (g_game.init_tables[t].stop && c->eax){ g_init[t].stopped = 1; g_init[t].stop_slot = slot; g_init[t].stop_eax = c->eax; }
    else if (slot == g_init[t].last) g_init[t].done = 1;
    else return;
    init_watch(t, 0);
    int all = 1;
    for (int k = 0; k < g_init_n; k++) if (!g_init[k].done) all = 0;
    if (all || g_init[t].stopped) init_finish(0);
}
/* main_web.c: before the entry point, once dispatch_init and the game's install ran */
void initcount_install(void){
    if (!g_game.init_tables || g_init_n) return;
    g_init_n = g_game.ninit_tables < INIT_MAX ? g_game.ninit_tables : INIT_MAX;
    for (int t = 0; t < g_init_n; t++){
        const InitTable *it = &g_game.init_tables[t];
        for (uint32_t s = it->lo; s < it->hi; s += 4u)
            if (mem_is_committed(s) && MR32(s)){ g_init[t].want++; g_init[t].last = s; }
        init_watch(t, 1);
        if (!g_init[t].want) g_init[t].done = 1;     /* nothing to run (not the game's own tables?) */
    }
    g_init_on = 1; g_diag = 1;
}
/* the exit report (main_web.c): the line, if the tables never finished */
void initcount_report(void){
    if (g_init_n && !g_init_printed) init_finish("at exit: the tables did not finish");
}

/* A tail jump (DISPATCH_TAIL in cpu.h): target in c->eip.  Lifted targets
 * and patches are tail-called so the jumping function's frame is gone; COM,
 * Win32 and generated code return here and end the chain like a call would. */
void dispatch_tail(CPU *c){
    uint32_t addr = c->eip, off = addr - TEXT_BASE;
    if (off < TEXT_SIZE){
        guest_fn f = g_dtab[off];
        if (!f && g_diag) f = diag_lookup(addr);
        if (f){ g_stat_indirect_ok++;
#ifdef __wasm__
            __attribute__((musttail))
#endif
            return f(c); }
    }
    /* a lifted DLL (server.dll, module.c): its loops jump through tables
     * too, so its targets are tail calls as well */
    else if (addr < 0xE0000000u){
        guest_fn f = patch_at(addr);
        if (!f) f = module_fn_at(addr);
        if (f){ g_stat_indirect_ok++;
#ifdef __wasm__
            __attribute__((musttail))
#endif
            return f(c); }
    }
    dispatch_indirect(c, addr);
}

/* ICALL=<address>: count the INDIRECT calls to that function and dump the
 * call chain for the first few.  Needed because virtual calls do not show up
 * in the static graph. */
static void icall_note(CPU *c, uint32_t addr){
    g_icall_hits++;
    { static uint32_t seen[256]; static int ns=0;
      uint32_t h2 = MR32(c->esp+8);
      int f3=0; for(int q=0;q<ns;q++) if(seen[q]==h2) f3=1;
      if(!f3 && ns<256){ seen[ns++]=h2;
        fprintf(stderr,"  [bind] hash=0x%08X %s\n", h2,
                h2==0x3EE7363Fu ? "  <<< WORLDVIEWPROJECTION" :
               (h2==0x04191C37u ? "  <<< TEXTUREOFFSET" : "")); } }
    static int shown = 0;
    if (shown < 3){ shown++;
        fprintf(stderr,"  [icall] 0x%08x called (arg1=%08x arg2=%08x arg3=%08x)\n",
                addr, MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
        guest_backtrace(c, 10);
    }
}

void dispatch_indirect(CPU *c, uint32_t addr){
    /* The CHEAP range test first.  COM vtable methods (0xE...) and Win32
     * import bridges (0xF...) are far above the guest text range, so they
     * NEVER collide with the patch table below -- patches are always inside
     * .text (dispatch_patch is only called with a guest function
     * address).  Every call used to scan the g_patch[] array linearly
     * first; a run has 377 million COM calls alone. */
    if (addr >= 0xE0000000u){
#ifdef __wasm__
        /* NFSU2_STACKPROBE: engine stack high-water at COM/Win32 calls (runtime/web/stackprobe.c) */
        { extern int g_stackprobe; if (g_stackprobe > 0){ void stackprobe_check(CPU *); stackprobe_check(c); } }
#endif
        if (addr < 0xF0000000u){ com_call(c, addr); return; }
        if (win32_call(c, addr - 0xF0000000u) < 0) g_guest_exited = 1;
        return;
    }
    /* a call into the exe's own D3DX code (UG2's, d3dx_hle.c): EFXSTAT's
     * inventory of the effect vtable calls, and with EFXSTAT the effect
     * methods are not in the table: their calls may change an effect (stage
     * 9's epoch) */
    efx_dispatch(c, addr);
    uint32_t off = addr - TEXT_BASE;
    if (off < TEXT_SIZE){
        guest_fn f = g_dtab[off];
        if (!f && g_diag) f = diag_lookup(addr);
        if (f){
            if (__builtin_expect(g_icall_want != 0, 0) && addr == g_icall_want) icall_note(c, addr);
            g_stat_indirect_ok++;
            if (__builtin_expect(g_init_on, 0)){ init_call(c, addr, f); return; }
            f(c); return; }
    }
    /* A run-time patch outside .text, then a lifted DLL (server.dll at
     * 0x00A00000, module.c): its own index, before the interpreter would take
     * the address as generated code. */
    else {
        guest_fn f = patch_at(addr);
        if (!f) f = module_fn_at(addr);
        if (f){ g_stat_indirect_ok++; f(c); return; }
    }
    /* No lifted counterpart.  The game may have generated code at this
     * address AT RUN TIME (NFSU2's material pipeline does this heavily).
     * Try a small x86 interpreter; on an unknown instruction it returns
     * safely (restores esp) and falls through to the diagnosis below. */
    { static int gx = -1;
      if (gx < 0){ const char *e = getenv("GENEXEC"); gx = (e && *e=='0') ? 0 : 1; }
      if (gx && mem_is_committed(addr) && run_generated(c, addr)){
          g_stat_indirect_ok++; return;
      } }

    g_stat_indirect_miss++;
    /* Caller: the return address is ALREADY ON THE STACK (before POP32).
     * g_watch_prev_ip used to be used, but that is only filled when the
     * instruction trace is on, so the report always showed 0x00000000. */
    { uint32_t ret = MR32(c->esp);
      if (g_miss_n < MISS_MAX){ g_miss_from[g_miss_n]=ret; g_miss[g_miss_n++]=addr; }
      static int shown = 0;
      if (shown < 5){ shown++;
          fprintf(stderr, "\n  [UNRESOLVED INDIRECT CALL] target=0x%08x  return=0x%08x\n"
                  "      eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x\n"
                  "      arg1=%08x arg2=%08x arg3=%08x\n",
                  addr, ret, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi,
                  MR32(c->esp+4), MR32(c->esp+8), MR32(c->esp+12));
          /* The bytes at the target: did the game generate CODE at run time
           * (a thunk), or is the pointer bad?  The quickest way to tell. */
          if (mem_is_committed(addr+15)){
              fprintf(stderr, "      96 bytes at the target:\n       ");
              for (int q = 0; q < 96; q++){
                  if (q && !(q & 15)) fprintf(stderr, "\n       ");
                  fprintf(stderr, " %02x", MR8(addr+q));
              }
              fprintf(stderr, "\n");
          } else fprintf(stderr, "      target NOT COMMITTED (not data, a garbage pointer)\n");
          guest_backtrace(c, 12);
      } }
    POP32(c->eip);            /* drop the return address, swallow the call */
}

void unsupported_insn(CPU *c, uint32_t addr, const char *mn){
    (void)c; (void)mn;
    g_stat_unsupported++;
    { extern void autotest_note_trap(uint32_t, const char*); autotest_note_trap(addr, mn); }
    if (g_unsup_n < MISS_MAX) g_unsup[g_unsup_n++] = addr;
}

void cpuid_shim(CPU *c){
    /* generic Pentium III-like: SSE yes, 3DNow no */
    switch (c->eax){
    case 0: c->eax=1; c->ebx=0x756e6547; c->edx=0x49656e69; c->ecx=0x6c65746e; break;
    case 1: c->eax=0x000006d8; c->ebx=0; c->ecx=0; c->edx=0x0383fbff; break;
    default: c->eax=c->ebx=c->ecx=c->edx=0; break;
    }
}
/* rdtsc: tied to real time, a fixed 3.0 GHz (consistent with QPC).  It used
 * to return +3000 per call, so the game's CPU MHz measurement (0x5C0AD0:
 * QPC + rdtsc + Sleep) found ~0 MHz and on the first run set ALL graphics
 * settings to the lowest (g_PerformanceLevel=0, 640x480).
 * Can be changed with NFSU2_TSC_MHZ. */
#include <mach/mach_time.h>
uint64_t rdtsc_shim(void){
    static mach_timebase_info_data_t tb; static uint64_t mhz = 0;
    if (!tb.denom){ mach_timebase_info(&tb);
        const char *e = getenv("NFSU2_TSC_MHZ"); mhz = e && *e ? strtoull(e, 0, 0) : 3000u; }
    __uint128_t ns = (__uint128_t)mach_absolute_time() * tb.numer / tb.denom;
    return (uint64_t)(ns * mhz / 1000u);
}

void dispatch_report(void){
    fprintf(stderr,"\n=== runtime ===\n");
    fprintf(stderr,"  indirect calls resolved  : %llu\n", (unsigned long long)g_stat_indirect_ok);
    fprintf(stderr,"  indirect calls unresolved: %llu\n", (unsigned long long)g_stat_indirect_miss);
    fprintf(stderr,"  unsupported instructions : %llu\n", (unsigned long long)g_stat_unsupported);
    genexec_report();
    if (g_miss_n){ fprintf(stderr,"  unresolved indirect calls (target <- calling instruction):\n");
        int shown=0;
        for(int i=0;i<g_miss_n && shown<12;i++){
            int dup=0; for(int j=0;j<i;j++) if(g_miss_from[j]==g_miss_from[i]) dup=1;
            if(dup) continue;
            fprintf(stderr,"    0x%08x  <-  0x%08x\n", g_miss[i], g_miss_from[i]); shown++; }
    }
    if (g_unsup_n){ fprintf(stderr,"  first trap addresses     :"); for(int i=0;i<g_unsup_n && i<10;i++) fprintf(stderr," 0x%08x",g_unsup[i]); fprintf(stderr,"\n"); }
}

/* --- guest stack tracer: resolves the return addresses in the .text range --- */
static uint32_t nearest_fn(uint32_t a){
    uint32_t lo=0, hi=g_fn_count, best=0;
    while(lo<hi){ uint32_t m=(lo+hi)/2;
        if(g_fn_addr[m]<=a){ best=g_fn_addr[m]; lo=m+1; } else hi=m; }
    return best;
}
void guest_backtrace(CPU *c, int maxn){
    fprintf(stderr,"    guest stack trace (esp=0x%08x):\n", c->esp);
    int shown=0;
    for (uint32_t k=0; k<2048 && shown<maxn; k++){
        uint32_t a = c->esp + 4u*k;
        if (!mem_is_committed(a)) continue;      /* skip an unmapped page */
        uint32_t v = MR32(a);
        const char *mn = 0; uint32_t mf;
        if (v - TEXT_BASE < TEXT_SIZE){
            uint32_t f = nearest_fn(v);
            fprintf(stderr,"      [%2d] +0x%04x  return 0x%08x   (sub_%08x + 0x%x)\n",
                    shown, k*4, v, f, v-f);
            shown++;
        } else if ((mf = module_nearest_fn(v, &mn)) != 0){
            fprintf(stderr,"      [%2d] +0x%04x  return 0x%08x   (%s sub_%08x + 0x%x)\n",
                    shown, k*4, v, mn, mf, v-mf);
            shown++;
        }
    }
}

/* Resolve the generated guest function from a faulting host PC: g_fn_ptr
 * maps host addresses to guest addresses; the nearest function start below
 * the PC is looked up. */
uint32_t guest_fn_from_host(const void *pc){
    uintptr_t p = (uintptr_t)pc, best = 0; uint32_t bestg = 0;
    for (uint32_t i = 0; i < g_fn_count; i++){
        uintptr_t f = (uintptr_t)g_fn_ptr[i];
        if (f <= p && f > best){ best = f; bestg = g_fn_addr[i]; }
    }
    /* the nearest start below pc may belong to a lifted DLL instead */
    { uint32_t mg = module_fn_from_host(pc);
      if (mg){ uintptr_t mf = 0;
          mod_fn fp = module_fn_at(mg); mf = (uintptr_t)fp;
          if (mf <= p && mf > best) bestg = mg; } }
    return bestg;
}
