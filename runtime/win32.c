/* Stage 3 seed: the Win32 API subset needed to boot + tracing for the rest. */
#include <stdatomic.h>
#include "enhance.h"
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>

/* getenv() SCANS the environment array and takes a lock on every call.  On
 * per-draw paths (e.g. the "PDLIST" check in SetVertexShaderConstantF was
 * the FIRST term of the && chain) that has a measurable cost: in the
 * profile getenv+__findenv_locked ate 3% of the guest thread.
 * Environment variables do not change during a run -> resolve once, keep. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

jmp_buf g_exit_jmp;
int g_exit_code = 0;
int g_exit_armed = 0;

#include "module.h"
extern const ImpEnt g_imp[]; extern const uint32_t g_imp_count;
/* One import index space for the exe and the lifted DLLs (module.c): the
 * exe's g_imp first, then each module's table (UG2: server.dll's g_srv_imp).
 * IAT slot values are IMP_MAGIC + index into this table. */
static ImpEnt *g_imps; static uint32_t g_nimps;
int modheap_owns(uint32_t h); uint32_t modheap_create(int mod); int modheap_destroy(uint32_t h);
uint32_t modheap_alloc(uint32_t h, uint32_t n, uint32_t flags); int modheap_free(uint32_t h, uint32_t u);
uint32_t modheap_realloc(uint32_t h, uint32_t u, uint32_t n, uint32_t flags); uint32_t modheap_size(uint32_t h, uint32_t u);
void modheap_report(void);
void mem_commit(uint32_t,uint32_t);
void guest_backtrace(CPU*,int);
int win32_ext_call(CPU*, const char*, uint32_t*);
void guest_fair_yield(void);
int fileio_call(CPU*, const char*, uint32_t*);
void fileio_report(void);
int winsock_call(CPU*, const char*, const char*, uint32_t*);   /* ws2_32, netapi32 (winsock.c) */
void winsock_report(void);
uint64_t win32_ext_threads(void);
uint64_t win32_ext_threads_exited(void);
uint32_t com_direct3dcreate9(void);
uint32_t com_directinput8create(uint32_t);
uint32_t com_directsoundcreate(uint32_t);
void com_init(void);
int g_bt_on = 0;

#define IMP_MAGIC 0xF0000000u

/* Guest heap, VirtualAlloc and friends: runtime/heap.c. */
void     heap_init(void);
void     heap_report(void);
int      heap_api_kind(const char *name);
uint32_t heap_api(CPU *c, int kind);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
uint32_t gheap_usize(uint32_t p);
void     heap_totals(uint64_t *live, uint64_t *peak, uint64_t *va, uint64_t *held);
/* ---- HYBRID virtual clock ---------------------------------------------
 * Tried:
 *   (a) fixed increment per call -> irregular time, animations broken
 *   (b) real monotonic clock     -> the emulator is slow, so the game sees a
 *       huge "elapsed time" every frame and enters its catch-up loop (20->1 fps)
 *   (c) only 16.67 ms per frame -> frame pacing right BUT code that waits on
 *       time without presenting a frame (video player) goes into an ENDLESS
 *       LOOP: measured, timeGetTime was called 231 million times.
 *   (d) CHOSEN: 16.67 ms per frame + creep forward slowly when the clock is
 *       polled many times and no frame arrives.  That keeps the pacing right
 *       and guarantees progress for loops that wait on time.          */
static _Atomic uint64_t g_vclock_base  = 0;   /* virtual time at frame start */
static _Atomic uint64_t g_vclock_creep = 0;   /* busy-wait compensation     */
static _Atomic uint64_t g_real_base    = 0;   /* real time of that frame    */
static _Atomic uint32_t g_vclock_polls = 0;

#ifdef __wasm__
#include "web/host_abi.h"
#endif
static uint64_t real_us(void){
#ifdef __wasm__
    /* The guest's most frequent host call (QueryPerformanceCounter, timeGetTime:
     * ~1,000 a race frame).  The page's monotonic clock without WASI's
     * clock_time_get, whose i64 argument costs a BigInt on every call into JS. */
    return (uint64_t)(nfsu2_now_ms() * 1000.0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

/* Virtual clock.
 * BETWEEN frames it advances exactly 16.667 ms: animation speed stays
 * independent of the emulator's speed.  WITHIN a frame it runs smoothly,
 * proportional to real time -- because the game measures the frame time by
 * sampling the clock TWICE within a frame and taking the difference
 * (0x005812d5).  The old clock, which jumped in one step, made it conclude
 * "the frame took zero", so the front-end tick [0x8651a8] never advanced
 * and every animation/timed transition froze. */
/* Measure the ratio of virtual time to real time: clock changes must not
 * speed the game up (it once ran at 2.01x). */
static _Atomic uint64_t g_vt_last = 0, g_rt_start = 0, g_vt_start = 0;
void vclock_report(void){
    uint64_t v0 = atomic_load(&g_vt_start), r0 = atomic_load(&g_rt_start);
    uint64_t v1 = atomic_load(&g_vt_last),  r1 = real_us();
    if (!r0 || r1 <= r0) return;
    fprintf(stderr, "  virtual clock     : %.3f x real time (%.1f s virtual / %.1f s real)\n",
            (double)(v1 - v0) / (double)(r1 - r0),
            (double)(v1 - v0)/1e6, (double)(r1 - r0)/1e6);
}
void vclock_tick(void){
    uint64_t cr = atomic_exchange(&g_vclock_creep, 0ull);
    uint64_t now = real_us();
    /* Per-frame advance.
     *   VCLOCK=fixed : exactly 16.667 ms per frame (old behaviour).  When the
     *                  emulator drops below 60 fps the GAME RUNS IN SLOW
     *                  MOTION -- and since the music is also played from
     *                  the game's clock, its pitch drops.  That was exactly
     *                  the user's "music slows down when fps drops"
     *                  (measured: at 50 fps virtual/real = 0.885, i.e. ~2
     *                  semitones flat).
     *   VCLOCK=real  : REAL elapsed time (default).  Game and music both
     *                  run in real time.  The per-frame step is capped at
     *                  50 ms so the game does not enter its catch-up loop. */
    { static _Atomic int mode = -1;
      int m = atomic_load(&mode);
      if (m < 0){ const char *e = ENVC("VCLOCK");
                  m = (e && *e == 'f') ? 0 : 1; atomic_store(&mode, m); }
      uint64_t rb = atomic_load(&g_real_base);
      uint64_t step = 16667ull;
      if (m && rb && now > rb){
          step = now - rb;
          /* above 250 fps: clamp.  The floor is 4 ms up to 120 fps content and
           * lower for 240 / uncapped (framerate.c), where frames routinely
           * come in under 4 ms. */
          { uint64_t hfr_vclock_floor_us(void); uint64_t fl = hfr_vclock_floor_us();
            if (step < fl) step = fl; }
          if (step > 50000ull) step = 50000ull;   /* huge dt -> catch-up loop */
      }
      atomic_fetch_add(&g_vclock_base, step + cr); }
    atomic_store(&g_real_base, now);
    atomic_store(&g_vclock_polls, 0);
    { uint64_t v = atomic_load(&g_vclock_base);
      if (!atomic_load(&g_rt_start)){ atomic_store(&g_rt_start, real_us());
                                      atomic_store(&g_vt_start, v); }
      atomic_store(&g_vt_last, v); }
}

/* oracle.c (cmp): the clock stands still while one guest function runs
 * twice (under the emulator, then lifted), so both see the same QPC/ms. */
_Atomic uint64_t g_vclock_freeze = 0;
uint64_t host_us(void){
    { uint64_t fz = atomic_load(&g_vclock_freeze); if (__builtin_expect(fz != 0, 0)) return fz; }
    uint64_t base = atomic_load(&g_vclock_base);
    uint64_t rb   = atomic_load(&g_real_base);
    uint64_t now  = real_us();
    uint64_t d    = (now > rb) ? (now - rb) : 0ull;
    /* Cap on the advance WITHIN a frame.  16 ms was FAR TOO TIGHT: code that
     * waits more than 16 ms of GAME time in one frame (the pre-race cut
     * scene) hit the cap and could only move on through the safety valve
     * below (after 200 ms) -> every frame was forced to take ~235 ms (4.6 fps,
     * measured).  Removing the cap entirely does not work either: the game
     * sees the real elapsed time and enters its catch-up loop.  A three-frame
     * budget is a good middle ground. */
    { static _Atomic uint64_t maxd = 0;
      uint64_t m = atomic_load(&maxd);
      if (!m){ const char *e = ENVC("VCLOCK_MAXD");
               m = (e && *e) ? (uint64_t)strtoull(e,0,0) : 50000ull;
               if (m < 16000ull) m = 16000ull;
               atomic_store(&maxd, m); }
      if (d > m) d = m; }

    /* Safety valve for loops that spin on the clock without producing a frame.
     *
     * NOTE: the criterion used to be the POLL COUNT (>2000 polls per frame ->
     * 400 us every 64 polls, 2 ms above 20000...).  More than 20 thousand
     * clock polls in a busy race frame is NORMAL; this added ~100 ms of fake
     * time per frame.  Measured: 2.04 s virtual in 1.01 s real (ratio 2.01)
     * -- cut scenes and the race ran twice as fast.  Now the criterion is
     * REAL TIME: it only kicks in when the frame has taken longer than
     * 200 ms (i.e. really stalled) and adds at most 100 ms per frame. */
    uint32_t n = atomic_fetch_add(&g_vclock_polls, 1);
    uint64_t stalled = (now > rb) ? (now - rb) : 0ull;      /* unclamped */
    /* Safety valve: threshold 200 ms -> 60 ms, step 2 ms -> 4 ms.  Since the
     * cap went up, this path now only runs on REAL stalls. */
    if (stalled > 60000ull){
        if (atomic_load(&g_vclock_creep) < 200000ull){
            if ((n & 15u) == 15u) atomic_fetch_add(&g_vclock_creep, 4000ull);
        } else
            /* Past the 200 ms the clock must still move, or a busy-wait in
             * the same long frame never sees its deadline: 0x45A630 waits
             * 20 ticks for each sound object it destroys on the way into a
             * race, and the career hung there for good when that frame had
             * already run long (career-46/seg08).  1 us per read keeps the
             * frame's extra game time small. */
            atomic_fetch_add(&g_vclock_creep, 1ull);
    }
    return base + d + atomic_load(&g_vclock_creep);
}
uint32_t host_ms(void){ return (uint32_t)(host_us() / 1000ull) + 1000u; }
uint64_t host_qpc(void){ return host_us(); }

uint32_t win32_block_size(uint32_t u){ return gheap_usize(u); }
/* guest address space the heap has reserved (the bump pointer with
 * NFSU2_HEAP=bump), and the bytes in live heap blocks (status line "heap=") */
uint32_t win32_heap_used(void){ uint64_t v = 0; heap_totals(0, 0, &v, 0); return (uint32_t)v; }
uint32_t win32_heap_live(void){ uint64_t l = 0; heap_totals(&l, 0, 0, 0); return (uint32_t)l; }

/* --- tracing --- */
typedef struct { uint32_t calls; uint32_t unimpl; } ImpStat;
static ImpStat *stat_;
static uint32_t trace_n = 0, trace_max = 0;
void win32_set_trace_max(uint32_t v){ trace_max = v; }
static uint32_t unimpl_first = 0xffffffffu;
static const char *unimpl_name = 0;
uint64_t g_win32_calls = 0, g_win32_unimpl = 0;

static uint32_t tls_slot[128]; static int tls_next = 0;
static uint32_t tls_free[128]; static int tls_nfree = 0;
int win32_tls_in_use(void){ return tls_next - tls_nfree; }
static uint32_t g_cmdline = 0;
static uint64_t tick0;

static uint32_t ARG(CPU*c,int i){ return MR32(c->esp + 4u*(uint32_t)i); }  /* after the return address has been POPped */

/* QPCWHO=1: who calls QueryPerformanceCounter (thread, return address, one
 * frame up).  At 120 fps ~21 thousand calls per frame were seen (~2
 * thousand at 60 fps PACE60): a time-based wait loop.  The call runs under
 * the GIL, the table is lock-free. */
typedef struct { uint32_t tid, ra, ra2; uint64_t n; } QpcWho;
static QpcWho g_qw[128];
static void qpc_report(void){
    fprintf(stderr, "\n=== QPCWHO: QueryPerformanceCounter callers ===\n");
    for (int k = 0; k < 128; k++){
        int b = -1;
        for (int i = 0; i < 128; i++) if (g_qw[i].n && (b < 0 || g_qw[i].n > g_qw[b].n)) b = i;
        if (b < 0 || k >= 24) break;
        fprintf(stderr, "  %12llu  thread %5u  return 0x%08x  parent 0x%08x\n",
                (unsigned long long)g_qw[b].n, g_qw[b].tid, g_qw[b].ra, g_qw[b].ra2);
        g_qw[b].n = 0;
    }
}
static void qpc_who(CPU *c){
    static int init = 0; static uint64_t tot = 0;
    if (!init){ init = 1; atexit(qpc_report); }
    if ((++tot & 0xFFFFFu) == 0){ long metal_frames(void);
        fprintf(stderr, "  [qpc] %llu M calls, frame %ld\n", (unsigned long long)(tot >> 20), metal_frames()); }
    uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 0u;
    uint32_t ra = c->eip;                       /* after POP32: return address */
    /* one frame up: the game knows its own clock wrapper (game.h qpc_caller),
     * else the caller's frame */
    uint32_t ra2 = g_game.qpc_caller ? g_game.qpc_caller(c) : 0u;
    if (!ra2) ra2 = (c->ebp && mem_is_committed(c->ebp+4u)) ? MR32(c->ebp+4u) : 0u;
    uint32_t h = (tid * 2654435761u ^ ra * 40503u ^ ra2) & 127u;
    for (int i = 0; i < 128; i++, h = (h + 1) & 127u){
        QpcWho *w = &g_qw[h];
        if (!w->n){ w->tid = tid; w->ra = ra; w->ra2 = ra2; w->n = 1; return; }
        if (w->tid == tid && w->ra == ra && w->ra2 == ra2){ w->n++; return; }
    }
}

/* Import index -> heap.c API kind (0 = not a heap call), filled in win32_init. */
static int8_t *g_hk; static int g_hk_heapalloc;
/* HeapAlloc debugging aids (unchanged from the bump allocator days). */
static void heapalloc_debug(CPU *c, uint32_t r){
    uint32_t sz = ARG(c,2);
    { const char *w = ENVC("TRACE_ALLOC");
      if (w){ uint32_t want = (uint32_t)strtoul(w,0,0);
        if (sz == want){ extern uint32_t g_watch_prev_ip;
          fprintf(stderr,"      [alloc] HeapAlloc(%u) -> 0x%08x   calling guest instruction: 0x%08x\n",
                  sz, r, g_watch_prev_ip);
          guest_backtrace(c, 12); } } }
    /* WATCHALLOC=<size>: set a watchpoint at +16 of the first allocation of
     * that size.  The constant block is written only once, so this is the
     * only way to catch the write. */
    /* WATCHAT=<address>: set a watchpoint on the allocation that returns exactly that address */
    { const char *wa = ENVC("WATCHAT");
      if (wa && r){ static int armed2 = 0;
        uint32_t want3 = (uint32_t)strtoul(wa,0,0);
        if (!armed2 && r == want3){ armed2 = 1;
          void watch_arm(uint32_t);
          const char *wo = ENVC("WATCHOFF");
          uint32_t off3 = wo ? (uint32_t)strtoul(wo,0,0) : 0u;
          fprintf(stderr,"      [alloc] target block 0x%08x (%u bytes) allocated, watching +%u\n",
                  r, sz, off3);
          watch_arm(r + off3);
          if (ENVC("MPROT")){ void mprot_arm(uint32_t); mprot_arm(r + off3); }
        } } }
    { const char *w2 = ENVC("WATCHALLOC");
      if (w2 && r){ static int armed = 0; static int seen = 0;
        uint32_t want2 = (uint32_t)strtoul(w2,0,0);
        const char *ws = ENVC("WATCHALLOC_N");
        int wantn = ws ? atoi(ws) : 1;
        if (!armed && sz == want2 && ++seen == wantn){ armed = 1;
          void watch_arm(uint32_t);
          fprintf(stderr,"      [alloc] %u-byte block 0x%08x allocated; watchpoint at +16\n", sz, r);
          watch_arm(r); } } }
    static int shown=0;
    if (ENVC("VERBOSE") && shown<2 && sz>0x100000u){ shown++;
        fprintf(stderr,"      [heap] HeapAlloc(size=0x%08x) SUSPICIOUS. ebp=0x%08x esp=0x%08x\n", sz, c->ebp, c->esp);
        fprintf(stderr,"        stack (from esp):");
        for(int k=0;k<14;k++) fprintf(stderr," %s%08x", k?"":"", MR32(c->esp+4u*k));
        fprintf(stderr,"\n        around ebp    :");
        for(int k=-4;k<=6;k++) fprintf(stderr," %s%08x", k==0?">":"", MR32(c->ebp+4u*(uint32_t)k));
        fprintf(stderr,"\n        esi=%08x edi=%08x ebx=%08x\n", c->esi,c->edi,c->ebx); }
}

/* Per-thread last error in the TEB (+0x34, the Windows layout): GetLastError /
 * SetLastError, the same slot WSAGetLastError / WSASetLastError use
 * (winsock.c); module.c (server.dll) calls win32_set_last_error too. */
void win32_set_last_error(CPU *c, uint32_t e){
    if (c && c->fs_base && mem_is_committed(c->fs_base + 0x34u)) MR32(c->fs_base + 0x34u) = e;
}
uint32_t win32_get_last_error(CPU *c){
    return (c && c->fs_base && mem_is_committed(c->fs_base + 0x34u)) ? MR32(c->fs_base + 0x34u) : 0u;
}
/* GetModuleHandleA: our exe and the DLLs it imports are "loaded" (they all
 * map to the image, as before, and GetProcAddress resolves by import name);
 * anything else (d3d9d.dll, mscoree.dll, ...) is not. */
static int module_known(uint32_t p){
    if (!mem_is_committed(p)) return 0;
    char nm[64]; snprintf(nm, sizeof nm, "%s", (const char *)(g_mem + p));
    const char *b = nm;
    for (const char *q = nm; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    size_t l = strlen(b);
    { const char *x = g_game.exe_name, *dot = strrchr(x, '.');     /* the exe, with or without ".exe" */
      if (!strcasecmp(b, x) || (dot && l == (size_t)(dot - x) && !strncasecmp(b, x, l))) return 1; }
    for (uint32_t i = 0; i < g_nimps; i++){            /* the exe's and server.dll's imports */
        const char *d = g_imps[i].dll; size_t dl = strlen(d);
        if (!strcasecmp(b, d)) return 1;
        if (!strchr(b, '.') && dl == l + 4 && !strncasecmp(b, d, l) && !strcasecmp(d + l, ".dll")) return 1;
    }
    return 0;
}
/* RaiseException(code, flags, nargs, args): log everything useful. */
static void raise_exception_report(CPU *c){
    uint32_t code = ARG(c,0), fl = ARG(c,1), na = ARG(c,2), pa = ARG(c,3);
    const char *what = code == 0xE06D7363u ? "C++ exception (throw)"
                     : code == 0xC000008Eu ? "float divide by zero" : code == 0xC000008Fu ? "float inexact result"
                     : code == 0xC0000090u ? "float invalid operation" : code == 0xC0000091u ? "float overflow"
                     : code == 0xC0000093u ? "float underflow" : code == 0xC000008Du ? "float denormal operand"
                     : code == 0xC0000092u ? "float stack check" : "?";
    fprintf(stderr, "\n*** GUEST RAISEEXCEPTION: code 0x%08x (%s), flags 0x%x, %u args, from 0x%08x (thread %u)\n",
            code, what, fl, na, c->eip,
            (c->fs_base && mem_is_committed(c->fs_base + 0x24u)) ? MR32(c->fs_base + 0x24u) : 0u);
    fprintf(stderr, "    x87 control word 0x%04x (exception mask 0x%02x%s, PC %u, RC %u), status 0x%04x\n",
            c->fcw, c->fcw & 0x3Fu, (c->fcw & 0x3Fu) == 0x3Fu ? " = all masked" : " -- UNMASKED exceptions",
            (c->fcw >> 8) & 3u, (c->fcw >> 10) & 3u, c->fsw);
    for (uint32_t i = 0; i < na && i < 8 && pa && mem_is_committed(pa + 4u * i + 3u); i++)
        fprintf(stderr, "    arg[%u] = 0x%08x\n", i, MR32(pa + 4u * i));
    /* MSVC C++ throw: args = { magic, object, ThrowInfo* }; name the type */
    if (code == 0xE06D7363u && na >= 3 && pa && mem_is_committed(pa + 11)){
        uint32_t ti = MR32(pa + 8);
        uint32_t cta = (ti && mem_is_committed(ti + 15)) ? MR32(ti + 12) : 0;
        uint32_t ct  = (cta && mem_is_committed(cta + 7)) ? MR32(cta + 4) : 0;
        uint32_t td  = (ct && mem_is_committed(ct + 7)) ? MR32(ct + 4) : 0;
        if (td && mem_is_committed(td + 8)) fprintf(stderr, "    thrown type %.80s\n", (const char *)(g_mem + td + 8));
        else if (!ti) fprintf(stderr, "    rethrow (throw;) of the current exception\n");
    }
    guest_backtrace(c, 16);
}
/* NFSU2_NETLOG=1: log every network-related import call (ws2_32, netapi32,
 * tapi32, plus LoadLibraryA/GetProcAddress/FreeLibrary) with its raw
 * arguments, a few decoded ones and the result.  Research aid for the LAN
 * track; off by default.  Must run before the stdcall stack cleanup. */
static void netlog(CPU *c, const ImpEnt *e, uint32_t r, int handled){
    static int on = -1;
    if (on < 0){ const char *v = getenv("NFSU2_NETLOG"); on = v && *v && *v != '0'; }
    if (!on) return;
    const char *d = e->dll, *n = e->name;
    int net = !strncasecmp(d, "ws2_32", 6) || !strncasecmp(d, "netapi32", 8) || !strncasecmp(d, "tapi32", 6);
    if (!net && strcmp(n, "LoadLibraryA") && strcmp(n, "GetProcAddress") && strcmp(n, "FreeLibrary")) return;
    char a[160]; int k = 0; a[0] = 0;
    for (int i = 0; i < e->nargs && i < 9 && k < (int)sizeof a - 12; i++)
        k += snprintf(a + k, sizeof a - (size_t)k, "%s0x%x", i ? "," : "", ARG(c, i));
    char x[128]; x[0] = 0;
    int si = !strcmp(n, "LoadLibraryA") || !strcmp(n, "gethostbyname") ? 0 : !strcmp(n, "GetProcAddress") ? 1 : -1;
    int sa = !strcmp(n, "bind") || !strcmp(n, "connect") ? 1 : !strcmp(n, "sendto") ? 4 : -1;
    if (si >= 0){ uint32_t p = ARG(c, si);
        if (p > 0xffffu && mem_is_committed(p) && mem_is_committed(p + 95u))
            snprintf(x, sizeof x, " \"%.96s\"", (const char *)(g_mem + p)); }
    if (sa >= 0){ uint32_t p = ARG(c, sa);
        if (p && mem_is_committed(p) && mem_is_committed(p + 15u))
            snprintf(x, sizeof x, " {af %u %u.%u.%u.%u:%u}", MR16(p), MR8(p + 4), MR8(p + 5), MR8(p + 6), MR8(p + 7),
                     (unsigned)(MR8(p + 2) << 8 | MR8(p + 3))); }
    long metal_frames(void);
    uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base + 0x24u)) ? MR32(c->fs_base + 0x24u) : 0u;
    fprintf(stderr, "[net] f=%ld tid=%u %s!%s(%s)%s -> 0x%x%s from 0x%08x\n",
            metal_frames(), tid, d, n, a, x, r, handled ? "" : " [stub]", c->eip);
    /* NFSU2_NETLOG=2: also the first 64 bytes of each outgoing payload */
    if (*getenv("NFSU2_NETLOG") == '2' && (!strcmp(n, "sendto") || !strcmp(n, "send"))){
        uint32_t p = ARG(c, 1), len = ARG(c, 2), m = len < 64u ? len : 64u;
        if (p && mem_is_committed(p) && mem_is_committed(p + m)){
            char h[200]; int hk = 0;
            for (uint32_t i = 0; i < m; i++) hk += snprintf(h + hk, sizeof h - (size_t)hk, "%02x", MR8(p + i));
            fprintf(stderr, "[net]   payload %u bytes: %s%s\n", len, h, len > m ? "..." : "");
        }
    }
}

/* Which module's import is being served on this thread: 0 = the exe, m =
 * lifted module m (module.h; UG2: 1 = server.dll).  win32_ext.c gives the
 * lifted DLLs Windows' thread exit semantics without changing what the exe
 * has always been told. */
static _Thread_local int tl_imp_mod = 0;
int win32_caller_module(void){ return tl_imp_mod; }
/* Real monotonic milliseconds (server.dll's GetTickCount, see win32_call). */
static uint32_t real_ms(void){ return (uint32_t)(real_us() / 1000ull) + 1000u; }

/* The address the guest calls an import at (IMP_MAGIC + index), 0 when
 * neither the exe nor a lifted DLL imports it (win32_ext.c's thread test). */
uint32_t win32_import_va(const char *name){
    for (uint32_t i = 0; i < g_nimps; i++) if (!strcmp(g_imps[i].name, name)) return IMP_MAGIC + i;
    return 0;
}

int win32_call(CPU *c, uint32_t idx){
    if (idx >= g_nimps) return 0;
    const ImpEnt *e = &g_imps[idx];
    { void win32_thread_term_check(CPU*); win32_thread_term_check(c); }   /* TerminateThread */
    int prev_mod = tl_imp_mod; tl_imp_mod = idx >= g_imp_count ? module_of_import(idx) : 0;
    stat_[idx].calls++; g_win32_calls++;
    guest_fair_yield();          /* give the other guest threads a turn */
    POP32(c->eip);                      /* take the return address */
    int handled = 1;
    const char *n = e->name;
    uint32_t r = 0;

    /* QueryPerformanceCounter is the most frequent call (frame limiter); to
     * skip the name chain's ~20 strcmps the exe's import index is cached on
     * its first call.  Only the exe's: a lifted DLL loaded before the entry
     * point (Most Wanted's D3DX oracle) calls it first, and a cached DLL index
     * left the exe's own calls unanswered.  The DLLs' calls take the name
     * test below. */
    static uint32_t qpc_idx = 0xFFFFFFFFu;
    /* server.dll (every lifted DLL) runs on real time.  The HLE's
     * GetTickCount is the frame-paced virtual clock, which nearly stops while
     * the game does not present (loads, hitches); the LAN server's ping/idle
     * timeouts, its advert cadence and its 33 ms service loop would stretch
     * against Windows clients on real time.  Its time() comes from
     * GetSystemTimeAsFileTime: real UTC as well (the HLE's own is a counter
     * that advances per call, which would end StopServer's 10 s wait after
     * 1,000 polls instead of 10 s). */
    if (idx >= g_imp_count && !strcmp(n,"GetTickCount")) r = real_ms();
    else if (idx >= g_imp_count && !strcmp(n,"GetSystemTimeAsFileTime")){
        uint32_t p = ARG(c,0);
        if (p){ struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t ft = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100u;
            mem_commit(p, 8); MR32(p) = (uint32_t)ft; MR32(p+4) = (uint32_t)(ft >> 32); }
        r = 0; }
    else if (idx == qpc_idx || (qpc_idx == 0xFFFFFFFFu && idx < g_imp_count && !strcmp(n,"QueryPerformanceCounter") && (qpc_idx = idx, 1))
             || (idx >= g_imp_count && !strcmp(n,"QueryPerformanceCounter"))){
        if (ENVC("QPCWHO")) qpc_who(c);
        if (g_game.on_qpc) g_game.on_qpc(c);    /* UG2: its frame wait loop sleeps (framerate.c) */
        uint64_t v = host_qpc(); uint32_t p=ARG(c,0);
        if(p){ MR32(p)=(uint32_t)v; MR32(p+4)=(uint32_t)(v>>32); } r=1; }
    /* A lifted DLL's own heaps (server.dll's CRT: HeapCreate from that DLL)
     * are modheap.c's, each DLL's in its own part of guest memory nothing
     * else uses (memmap.h modheap); every other heap call is heap.c's. */
    else if (g_hk[idx] > 0 && idx >= g_imp_count && !strcmp(n,"HeapCreate")) r = modheap_create(tl_imp_mod);
    else if (g_hk[idx] > 0 && n[0] == 'H' && !strncmp(n,"Heap",4) && modheap_owns(ARG(c,0))){
        uint32_t h = ARG(c,0);
        if      (!strcmp(n,"HeapDestroy")) r = (uint32_t)modheap_destroy(h);
        else if (!strcmp(n,"HeapAlloc"))   r = modheap_alloc(h, ARG(c,2), ARG(c,1));
        else if (!strcmp(n,"HeapFree"))    r = (uint32_t)modheap_free(h, ARG(c,2));
        else if (!strcmp(n,"HeapReAlloc")) r = modheap_realloc(h, ARG(c,2), ARG(c,3), ARG(c,1));
        else if (!strcmp(n,"HeapSize"))    r = modheap_size(h, ARG(c,2));
        else handled = 0; }
    else if (g_hk[idx] > 0){
        r = heap_api(c, g_hk[idx]);
        if (g_hk[idx] == g_hk_heapalloc) heapalloc_debug(c, r); }
    else if (!strcmp(n,"GetVersion"))            r = 0x0A280105u;      /* 5.1.2600 */
    else if (!strcmp(n,"GetModuleHandleA")){
        /* NULL and the DLLs the exe or server.dll import: the exe's base, as
         * before (GetProcAddress resolves by import name); a lifted module
         * (server.dll): its base while loaded, NULL otherwise; anything else
         * (d3d9d.dll, mscoree.dll, ...) is not loaded */
        uint32_t p = ARG(c,0);
        const char *nm = p && mem_is_committed(p) ? (const char*)(g_mem + p) : 0;
        if (nm && module_known_name(nm)) r = module_by_name(nm);
        else r = (!p || module_known(p)) ? 0x00400000u : 0u;
        if (!r) win32_set_last_error(c, 126);            /* ERROR_MOD_NOT_FOUND */
        if (p && mem_is_committed(p)){ static char seen[16][40]; static int ns = 0; int k;
            for (k = 0; k < ns && strncasecmp(seen[k], (const char*)(g_mem+p), 39); k++) ;
            if (k == ns && ns < 16){ snprintf(seen[ns++], 40, "%s", (const char*)(g_mem+p));
                fprintf(stderr,"[hle] GetModuleHandleA(\"%s\") -> 0x%08x (from 0x%08x)\n", seen[ns-1], r, c->eip); } } }
    else if (!strcmp(n,"GetCommandLineA"))       r = g_cmdline;
    else if (!strcmp(n,"GetEnvironmentStrings")||!strcmp(n,"GetEnvironmentStringsW")){
        static uint32_t env = 0;               /* empty block; FreeEnvironmentStrings is a no-op */
        if (!env) env = gheap_alloc(1, 8, 1);
        r = env; }
    else if (!strcmp(n,"FreeEnvironmentStringsA")||!strcmp(n,"FreeEnvironmentStringsW")) r=1;
    else if (!strcmp(n,"GetStartupInfoA")){ uint32_t p=ARG(c,0); if(p){ mem_commit(p,68); memset(g_mem+p,0,68); MR32(p)=68; } r=0; }
    else if (!strcmp(n,"GetStdHandle")){         /* 0x10 + (n & 3), or what SetStdHandle set (win32_misc.c:
                                                  * only a game whose g_game.win32_misc answers it; never UG2) */
        uint32_t win32_std_handle(uint32_t); r = win32_std_handle(ARG(c,0)); }
    else if (!strcmp(n,"GetFileType")){
        /* The CRT's _open asks right after CreateFileA: files are DISK, the
         * std handles (GetStdHandle: 0x10..0x13) CHAR, anything else UNKNOWN. */
        uint32_t h = ARG(c,0); extern int fileio_is_handle(uint32_t);
        if (h >= 0x10u && h <= 0x13u) r = 2;
        else if (fileio_is_handle(h)) r = 1;
        else { r = 0; win32_set_last_error(c, 6); } }
    else if (!strcmp(n,"SetHandleCount"))        r = ARG(c,0);
    else if (!strcmp(n,"GetVersionExA")||!strcmp(n,"GetVersionExW")){
        uint32_t p=ARG(c,0);
        if(p){ MR32(p+4)=5; MR32(p+8)=1; MR32(p+12)=2600; MR32(p+16)=2; }
        r = 1; }
    else if (!strcmp(n,"GetACP"))                r = 1252;
    else if (!strcmp(n,"GetOEMCP"))              r = 437;
    else if (!strcmp(n,"GetCPInfo")){ uint32_t p=ARG(c,1); if(p){ mem_commit(p,20); memset(g_mem+p,0,20); MR32(p)=1; } r=1; }
    else if (!strcmp(n,"IsValidCodePage"))       r = 1;
    else if (!strcmp(n,"TlsAlloc")){
        /* freed indices first (server.dll's CRT takes one per load and frees
         * it on unload; with no reuse, hosting would stop working after
         * ~120 sessions) */
        if (tls_nfree) r = tls_free[--tls_nfree];
        else r = (tls_next<128)? (uint32_t)tls_next++ : 0xffffffffu; }
    else if (!strcmp(n,"TlsSetValue")){
        uint32_t i=ARG(c,0), v=ARG(c,1);
        uint32_t arr = (c->fs_base && mem_is_committed(c->fs_base+0x2cu)) ? MR32(c->fs_base+0x2cu) : 0;
        if (arr && i<256u && mem_is_committed(arr+4u*i)){ MR32(arr+4u*i)=v; r=1; }
        else { if(i<128) tls_slot[i]=v; r=1; } }
    else if (!strcmp(n,"TlsGetValue")){
        uint32_t i=ARG(c,0);
        uint32_t arr = (c->fs_base && mem_is_committed(c->fs_base+0x2cu)) ? MR32(c->fs_base+0x2cu) : 0;
        if (arr && i<256u && mem_is_committed(arr+4u*i)) r = MR32(arr+4u*i);
        else r = (i<128)?tls_slot[i]:0; }
    else if (!strcmp(n,"TlsFree")){
        /* Windows clears the slot in every thread, so the next TlsAlloc of
         * this index reads NULL everywhere: here the main TEB, the created
         * threads' TEBs (0x00C20000 + 0x2000 n) and the timer thread's, all
         * in the TEB range (memmap.h), which ends where the next region
         * starts. */
        uint32_t i = ARG(c,0); r = 0;
        if (i < (uint32_t)tls_next){
            int dup = 0; for (int k = 0; k < tls_nfree; k++) dup |= tls_free[k] == i;
            if (!dup && tls_nfree < 128){
                tls_slot[i] = 0;
                for (uint32_t teb = MM_TEB_LO; teb + MM_TEB_SIZE <= MM_TEB_HI; teb += MM_TEB_SIZE)
                    if (mem_is_committed(teb + 0x2Cu) && MR32(teb + 0x18u) == teb){
                        uint32_t arr = MR32(teb + 0x2Cu);
                        if (arr && mem_is_committed(arr + 4u*i + 3u)) MR32(arr + 4u*i) = 0;
                    }
                tls_free[tls_nfree++] = i; r = 1; } } }
    /* Critical sections are NOT handled here; win32_ext.c implements them
     * with REAL recursive locks (see cs_for).  With a no-op stub the GIL's
     * fair sharing would break mutual exclusion. */
    else if (!strcmp(n,"GetCurrentThreadId")){
        /* the real thread id from the TEB (fs:[0x24]); every thread must be unique */
        r = (c->fs_base && mem_is_committed(c->fs_base+0x24u)) ? MR32(c->fs_base+0x24u) : 1u;
        if (!r) r = 1u; }
    else if (!strcmp(n,"GetCurrentProcessId"))   r = 100;
    else if (!strcmp(n,"GetCurrentProcess"))     r = 0xffffffffu;
    else if (!strcmp(n,"GetLastError"))          r = win32_get_last_error(c);
    else if (!strcmp(n,"SetLastError")){         win32_set_last_error(c, ARG(c,0)); r = 0; }
    else if (!strcmp(n,"GetTickCount")) r = host_ms();
    else if (!strcmp(n,"QueryPerformanceFrequency")){ uint32_t p=ARG(c,0); if(p){MR32(p)=1000000;MR32(p+4)=0;} r=1; }
    else if (!strcmp(n,"SetUnhandledExceptionFilter")) r = 0;
    else if (!strcmp(n,"UnhandledExceptionFilter"))    r = 0;
    else if (!strcmp(n,"IsProcessorFeaturePresent")){
        /* Truthful for a Pentium 4 class CPU: PF_COMPARE_EXCHANGE_DOUBLE (2),
         * PF_MMX (3), PF_XMMI (6), PF_RDTSC (8), PF_XMMI64 (10).  It used to
         * be TRUE for everything, so D3DX's PSGP selector (0x641790, the only
         * caller) installed its 3DNow! kernels (PF 7), which the lifter cannot
         * run (pswapd/pfacc...). */
        uint32_t f = ARG(c,0);
        r = (f == 2u || f == 3u || f == 6u || f == 8u || f == 10u) ? 1u : 0u;
        if (ENVC("NFSU2_PF_ALL")) r = 1;                 /* test-only: the old answer */
        { static uint32_t seen = 0; if (f < 32u && !(seen & (1u << f))){ seen |= 1u << f;
            fprintf(stderr,"[hle] IsProcessorFeaturePresent(%u) -> %u (from 0x%08x)\n", f, r, c->eip); } } }
    else if (!strcmp(n,"GetModuleFileNameA")){ uint32_t p=ARG(c,1),sz=ARG(c,2);
        const char*s=g_game.module_path; char ms[300];
        if (module_file_name(ARG(c,0), ms, sizeof ms)) s = ms;   /* server.dll: C:\NFSU2\server.dll */
        uint32_t l=(uint32_t)strlen(s);
        if(p&&sz){ if(l>=sz) l=sz-1; memcpy(g_mem+p,s,l); MR8(p+l)=0; } r=l; }
    else if (!strcmp(n,"ExitProcess")){ g_exit_code=(int)ARG(c,0);
        fprintf(stderr,"\n*** guest ExitProcess(%u) - exiting\n",ARG(c,0));
        { extern void discord_shutdown(void); discord_shutdown(); }   /* the game quit: clear the Discord presence */
        if(g_exit_armed) longjmp(g_exit_jmp,1); return -1; }
    else if (!strcmp(n,"LoadLibraryA")||!strcmp(n,"LoadLibraryW")){
        /* A lifted module (server.dll) is really loaded (module.c); every
         * other DLL keeps the old placeholder handle. */
        char nm[260] = ""; uint32_t p0 = ARG(c,0), err = 0;
        if (p0 && mem_is_committed(p0)){
            if (n[11] == 'W'){ uint32_t k = 0; for (; k < sizeof nm - 1 && MR16(p0 + 2*k); k++) nm[k] = (char)MR16(p0 + 2*k); nm[k] = 0; }
            else snprintf(nm, sizeof nm, "%s", (const char*)(g_mem + p0)); }
        if (nm[0] && module_known_name(nm)){
            r = module_load(c, nm, &err);
            if (!r){ void win32_set_last_error(CPU*, uint32_t); win32_set_last_error(c, err); }
        } else r = 0x20000000u;
        { static int sh=0; uint32_t p2=ARG(c,0);
          if (ENVC("VERBOSE") && sh<12){ sh++; fprintf(stderr,"      [dll] LoadLibrary(ptr=0x%08x) \"%s\"\n", p2, p2?(char*)(g_mem+p2):"(NULL)"); } } }
    else if (!strcmp(n,"GetProcAddress")){
        uint32_t pname = ARG(c,1); r = 0;
        if (module_handle(ARG(c,0))){              /* lifted module: its export table */
            uint32_t err = 0; r = module_proc(ARG(c,0), pname, &err);
            if (!r){ void win32_set_last_error(CPU*, uint32_t); win32_set_last_error(c, err); }
        }
        else if (pname > 0xffff){                  /* by name (not ordinal) */
            const char *want = (const char*)(g_mem + pname);
            for (uint32_t i=0;i<g_nimps;i++)
                if (!strcmp(g_imps[i].name, want)){ r = IMP_MAGIC + i; break; }
            { static int sh=0;
              if (ENVC("VERBOSE") && !r && sh<12){ sh++; fprintf(stderr,"      [dll] GetProcAddress(\"%s\") NOT FOUND\n", want); } }
        } }
    else if (!strcmp(n,"FreeLibrary")){ module_free(c, ARG(c,0)); r = 1; }
    else if (!strcmp(n,"InterlockedIncrement")){ uint32_t p=ARG(c,0); r=++MR32(p); }
    else if (!strcmp(n,"InterlockedDecrement")){ uint32_t p=ARG(c,0); r=--MR32(p); }
    else if (!strcmp(n,"InterlockedExchange")){ uint32_t p=ARG(c,0); r=MR32(p); MR32(p)=ARG(c,1); }
    else if (!strcmp(n,"MultiByteToWideChar")){ uint32_t src=ARG(c,2); int sl=(int)ARG(c,3);
        uint32_t dst=ARG(c,4); int dl=(int)ARG(c,5);
        if(sl<0) sl=(int)strlen((char*)(g_mem+src))+1;
        if(dl==0){ r=(uint32_t)sl; } else { int k=0; for(;k<sl&&k<dl;k++) MR16(dst+2u*k)=MR8(src+k); r=(uint32_t)k; } }
    else if (!strcmp(n,"WideCharToMultiByte")){ uint32_t src=ARG(c,2); int sl=(int)ARG(c,3);
        uint32_t dst=ARG(c,4); int dl=(int)ARG(c,5);
        if(sl<0){ sl=0; while(MR16(src+2u*sl)) sl++; sl++; }
        if(dl==0){ r=(uint32_t)sl; } else { int k=0; for(;k<sl&&k<dl;k++) MR8(dst+k)=(uint8_t)MR16(src+2u*k); r=(uint32_t)k; } }
    else if (!strcmp(n,"GetStringTypeW")||!strcmp(n,"GetStringTypeA")){
        /* A: (LCID,dwInfoType,src,cch,out)  W: (dwInfoType,src,cch,out) */
        int aform = (n[13]=='A');
        uint32_t info = ARG(c, aform?1:0);
        uint32_t src  = ARG(c, aform?2:1);
        int      cch  = (int)ARG(c, aform?3:2);
        uint32_t out  = ARG(c, aform?4:3);
        int wide = !aform;
        if (cch < 0){ cch = 0; if(wide){ while(MR16(src+2u*cch)) cch++; } else { while(MR8(src+cch)) cch++; } cch++; }
        if (info == 1 && out){                       /* CT_CTYPE1 */
            for (int k=0;k<cch;k++){
                uint32_t ch = wide ? MR16(src+2u*k) : MR8(src+k);
                uint16_t f = 0x0200;                 /* C1_DEFINED */
                if (ch<128){
                    if (ch>='A'&&ch<='Z') f |= 0x0001|0x0100;
                    if (ch>='a'&&ch<='z') f |= 0x0002|0x0100;
                    if (ch>='0'&&ch<='9') f |= 0x0004|0x0080;
                    if ((ch>='a'&&ch<='f')||(ch>='A'&&ch<='F')) f |= 0x0080;
                    if (ch==' ') f |= 0x0008|0x0040;
                    if (ch=='\t'||ch=='\n'||ch=='\r'||ch==0x0b||ch==0x0c) f |= 0x0008;
                    if (ch<0x20||ch==0x7f) f |= 0x0020;
                    if (ch>0x20&&ch<0x7f&&!(f&(0x0100|0x0004))) f |= 0x0010;
                } else f |= 0x0100;
                MR16(out+2u*k) = f;
            }
        }
        r = 1; }
    else if (!strcmp(n,"LCMapStringW")||!strcmp(n,"LCMapStringA")){
        int wide = (n[11]=='W');
        uint32_t flags = ARG(c,1), src = ARG(c,2); int cch = (int)ARG(c,3);
        uint32_t dst = ARG(c,4); int cchd = (int)ARG(c,5);
        if (cch < 0){ cch = 0; if(wide){ while(MR16(src+2u*cch)) cch++; } else { while(MR8(src+cch)) cch++; } cch++; }
        if (cchd == 0){ r = (uint32_t)cch; }
        else {
            int k=0;
            for (;k<cch && k<cchd;k++){
                uint32_t ch = wide ? MR16(src+2u*k) : MR8(src+k);
                if ((flags & 0x00000100u) && ch<128 && ch>='A' && ch<='Z') ch += 32;   /* LOWERCASE */
                if ((flags & 0x00000200u) && ch<128 && ch>='a' && ch<='z') ch -= 32;   /* UPPERCASE */
                if (wide) MR16(dst+2u*k)=(uint16_t)ch; else MR8(dst+k)=(uint8_t)ch;
            }
            r = (uint32_t)k;
        } }
    else if (!strcmp(n,"GetLocaleInfoA")||!strcmp(n,"GetLocaleInfoW")){
        int wide = (n[13]=='W');
        uint32_t lct = ARG(c,1) & 0xffffu, p = ARG(c,2); int cch = (int)ARG(c,3);
        const char *v = 0; char num[16];
        switch (lct){
        case 0x0001: v="0409"; break;                 /* ILANGUAGE            */
        case 0x0002: v="English"; break;              /* SLANGUAGE            */
        case 0x1001: v="English"; break;              /* SENGLANGUAGE         */
        case 0x1002: v="United States"; break;        /* SENGCOUNTRY          */
        case 0x0003: v="ENU"; break;                  /* SABBREVLANGNAME      */
        case 0x0005: v="US"; break;                   /* SABBREVCTRYNAME      */
        case 0x000E: v="."; break;                    /* SDECIMAL             */
        case 0x000F: v=","; break;                    /* STHOUSAND            */
        case 0x0014: v="2"; break;                    /* IDIGITS              */
        case 0x000B: snprintf(num,sizeof num,"%d",437);  v=num; break; /* IDEFAULTCODEPAGE */
        case 0x1004: snprintf(num,sizeof num,"%d",1252); v=num; break; /* IDEFAULTANSICODEPAGE */
        case 0x0019: v="$"; break;                    /* SCURRENCY            */
        case 0x001D: v="/"; break;                    /* SDATE                */
        case 0x001E: v=":"; break;                    /* STIME                */
        default:     v="";  break;
        }
        uint32_t len = (uint32_t)strlen(v) + 1;
        if (cch == 0) r = wide ? len : len;
        else if (p){ uint32_t k=0; for(;k<len-1 && k<(uint32_t)cch-1;k++){ if(wide) MR16(p+2u*k)=v[k]; else MR8(p+k)=v[k]; }
                     if(wide) MR16(p+2u*k)=0; else MR8(p+k)=0; r=k+1; }
        else r = 0; }
    else if (!strcmp(n,"MessageBoxA")){
        uint32_t txt=ARG(c,1), cap=ARG(c,2);
        fprintf(stderr,"\n*** GUEST MESSAGEBOX ***\n  title: %s\n  text : %s\n***\n",
                cap?(char*)(g_mem+cap):"(none)", txt?(char*)(g_mem+txt):"(none)");
        r = 1; }
    else if (!strcmp(n,"Direct3DCreate9")) r = com_direct3dcreate9();
    else if (!strcmp(n,"DirectInput8Create")) r = com_directinput8create(ARG(c,3));
    else if (!strcmp(n,"DirectSoundCreate") || (!strcmp(n,"#1") && !strcasecmp(e->dll,"DSOUND.dll")))
        r = com_directsoundcreate(ARG(c,1));
    else if (!strcmp(n,"SetConsoleCtrlHandler")) r = 1;
    else if (!strcmp(n,"RtlUnwind"))             r = 0;
    else if (!strcmp(n,"RaiseException")){
        /* There is no guest SEH dispatch, so continuing after a raise is
         * undefined behaviour.  Stop loudly instead; NFSU2_RAISE=continue
         * (test-only) restores the old silent return. */
        raise_exception_report(c);
        if (!ENVC("NFSU2_RAISE") || strcmp(ENVC("NFSU2_RAISE"), "continue")){
            extern void (*g_report_fn)(void);
            if (g_report_fn) g_report_fn();
            fflush(stderr); _exit(5); }
        r = 0; }
    else if (!strcmp(n,"TerminateProcess")){ g_exit_code=(int)ARG(c,1);
        fprintf(stderr,"\n*** guest TerminateProcess(%u) - exiting\n",ARG(c,1));
        if(g_exit_armed) longjmp(g_exit_jmp,1); return -1; }
    else if (!strcmp(n,"FatalAppExitA")){
        /* (uAction, lpMessageText): Windows shows the text and ends the
         * process with STATUS_FATAL_APP_EXIT; here the log gets the text */
        uint32_t p = ARG(c,1); char msg[200] = "";
        if (p && mem_is_committed(p)) snprintf(msg, sizeof msg, "%s", (const char *)(g_mem + p));
        g_exit_code = (int)0x40000015u;
        fprintf(stderr, "\n*** guest FatalAppExitA(\"%s\") from 0x%08x - exiting\n", msg, c->eip);
        guest_backtrace(c, 12);
        if(g_exit_armed) longjmp(g_exit_jmp,1); return -1; }
    else if (winsock_call(c, e->dll, n, &r)) { /* ws2_32, netapi32: winsock.c */ }
    else if (fileio_call(c, n, &r)) { /* file I/O */ }
    else if (win32_ext_call(c, n, &r)) { /* second-round layer */ }
    else if (g_game.win32_misc && g_game.win32_misc(c, e->dll, n, &r)) { /* the game's own: win32_misc.c (game.h) */ }
    else { handled = 0; g_win32_unimpl++; stat_[idx].unimpl++;
           { extern void autotest_note_unimpl(const char*,const char*,uint32_t);
             autotest_note_unimpl(e->dll, n, c->eip); }
           if (unimpl_first==0xffffffffu){ unimpl_first=idx; unimpl_name=n; } }

    if (g_bt_on && (!strcmp(n,"GetModuleFileNameA")||!strcmp(n,"ExitProcess")||!strcmp(n,"RaiseException")))
        guest_backtrace(c, 14);
    if (trace_n < trace_max)
        fprintf(stderr, "  [%4u] %-14s %-32s (%d arg) %s-> 0x%08x\n",
                ++trace_n, e->dll, n, e->nargs, handled?"":"[STUB] ", r);
    else trace_n++;
    netlog(c, e, r, handled);
    if (idx >= g_imp_count && !strncasecmp(e->dll, "ws2_32", 6)){
        void module_note_socket_call(int, const char *, uint32_t, uint32_t);   /* module.c: unload report */
        module_note_socket_call(tl_imp_mod, n, ARG(c,0), r);
    }

    c->eax = r;
    c->esp += 4u * (uint32_t)e->nargs;      /* stdcall: the callee cleans up the arguments */
    tl_imp_mod = prev_mod;
    return 1;
}

void win32_init(void){
    module_init();
    g_nimps = g_imp_count;
    for (int m = 1; m <= module_count(); m++) g_nimps += *module_link(m)->imp_count;
    g_imps = calloc(g_nimps ? g_nimps : 1, sizeof *g_imps);
    memcpy(g_imps, g_imp, g_imp_count * sizeof *g_imps);
    for (int m = 1; m <= module_count(); m++){
        const ModLink *l = module_link(m);
        if (*l->imp_count) memcpy(g_imps + module_imp_first(m), l->imp, *l->imp_count * sizeof *g_imps);
    }
    stat_ = calloc(g_nimps ? g_nimps : 1, sizeof *stat_);
    heap_init();
    g_hk = calloc(g_nimps ? g_nimps : 1, 1);
    for (uint32_t i=0;i<g_nimps;i++) g_hk[i] = (int8_t)heap_api_kind(g_imps[i].name);
    g_hk_heapalloc = heap_api_kind("HeapAlloc");
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    tick0 = (uint64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
    g_cmdline = gheap_alloc(1, 64, 1);
    snprintf((char*)(g_mem+g_cmdline), 64, "%s", g_game.exe_name);
    /* fill the IAT with magic addresses */
    for (uint32_t i=0;i<g_imp_count;i++) MR32(g_imp[i].iat) = IMP_MAGIC + i;
    com_init();
    { void watchdog_start(void); watchdog_start(); }      /* NFSU2_WATCHDOG */
    /* addresses to read later for diagnosing the CRT heap mode */
    fprintf(stderr,"win32: %u imports bound to the IAT\n", g_imp_count);
    for (int m = 1; m <= module_count(); m++){
        const ModLink *l = module_link(m);
        if (*l->imp_count) fprintf(stderr,"win32: %s lifted into this build (%u imports, bound when it is loaded)\n",
                                   l->info->name, *l->imp_count);
    }
    { extern uint32_t g_image_base, g_image_size;          /* mem_load_pe */
      memmap_check(g_image_base, g_image_base + g_image_size, 1); }   /* the guest layout (memmap.h): stops if broken */
}

void win32_report(void){
    fprintf(stderr,"\n=== WIN32 LAYER ===\n");
    fprintf(stderr,"  import calls in total : %llu\n",(unsigned long long)g_win32_calls);
    fprintf(stderr,"  not implemented       : %llu\n",(unsigned long long)g_win32_unimpl);
    /* the most called unimplemented ones */
    fprintf(stderr,"  guest threads : %llu started, %llu exited\n",
            (unsigned long long)win32_ext_threads(),(unsigned long long)win32_ext_threads_exited());
    fileio_report();
    heap_report();
    winsock_report();
    { extern void win32_obj_report(void); win32_obj_report(); }
    { extern void res_report(void); res_report(); }
    { extern void overrides_report(void); overrides_report(); }
    module_report(); modheap_report();
    enh_report();
    { extern void d3dx_hle_report(void); d3dx_hle_report(); }
    fprintf(stderr,"  --- NOT IMPLEMENTED (called, a stub returned) ---\n");
    for (uint32_t i=0;i<g_nimps;i++)
        if (stat_[i].unimpl){
            /* a module's import, after its file name without the extension: "server!" */
            const ModLink *l = module_link(module_of_import(i));
            const char *mn = l ? l->info->name : "";
            int ml = l ? (int)strcspn(mn, ".") : 0;
            fprintf(stderr,"    %.*s%s%-14s %-32s %u times\n", ml, mn, l ? "!" : "", g_imps[i].dll, g_imps[i].name, stat_[i].unimpl);
        }
    fprintf(stderr,"  --- 15 MOST CALLED ---\n");
    for (int k=0;k<15;k++){
        uint32_t best=0, bi=0;
        for (uint32_t i=0;i<g_nimps;i++) if (stat_[i].calls>best){ best=stat_[i].calls; bi=i; }
        if(!best) break;
        fprintf(stderr,"    %-14s %-32s %u\n", g_imps[bi].dll, g_imps[bi].name, best);
        stat_[bi].calls = 0;
    }
    int shown=0;
    for (uint32_t i=0;i<g_nimps && shown<15;i++)
        if (stat_[i].calls){
            /* we do not know whether it was handled; just show the frequently called ones */
        }
    for (uint32_t i=0;i<g_nimps;i++)
        if (stat_[i].calls>1 && shown<15){ fprintf(stderr,"    %-30s %u times\n", g_imps[i].name, stat_[i].calls); shown++; }
}
