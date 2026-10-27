/* mwmemmove.c -- Most Wanted's lifted CRT memmove (0x7C4840, GAME §2.5)
 * against a reference memmove.
 *
 *   tests/mwmemmove.sh [--wasm]     (builds and runs it)
 *
 * The routine copies the bulk with rep movsd and the head and tail through
 * eight jumps into tables inside its own code, one of them indexed
 * backwards (neg ecx; jmp [ecx*4 + 0x7C4AD8]).  The lifter makes each table
 * target a function of its own and each table jump a DISPATCH_TAIL; another
 * project's lifter missed the target 0x7C4AFC of that backward table and the
 * game crashed in races.  The same code is in the exe a second time at
 * 0x7D6D30 (13 call sites, its tables 0x124F0 further on): both are tested.
 *
 * Cases: every alignment of the source and of the destination mod 16, every
 * length 0..300 and some large ones (up to 1 MB), each with the regions
 * apart (the destination below and above the source), overlapping with the
 * destination below the source (forward copy) and above it (backward copy,
 * three distances each, the smallest one the alignments allow, one in the
 * middle and the largest below the length) and, for equal alignments, the
 * same address.  A case passes when the bytes around both regions equal a
 * host memmove of the same bytes (64 bytes before and after included), eax
 * is the destination, eip and esp are those of a cdecl return, ebx, esi, edi,
 * ebp and the arguments are kept and the direction flag is clear again.  At
 * the end every table entry the code can reach must have been dispatched to.
 *
 * The exe comes in on stdin (the wasm build has no file system), the lifted
 * code is GAME=mw web/gen.sh's: native objects from tests/mwshards.sh, or
 * with --wasm the objects GAME=mw web/build.sh compiled, run under
 * node:wasi, where a table jump is a real tail call (DISPATCH_TAIL with
 * musttail, through dispatch_tail). */
#include "cpu.h"
#include "mwimage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef __wasm__
#include <signal.h>
#include <setjmp.h>
#endif

typedef void (*guest_fn)(CPU *);
extern const uint32_t g_fn_count;
extern const uint32_t g_fn_addr[];
extern const guest_fn g_fn_ptr[];
guest_fn_t g_dtab[DISPATCH_TEXT_SIZE];
void sub_007c4840(CPU *);
void sub_007d6d30(CPU *);

#define ARENA   0x10000000u                  /* the buffers (8 MB) */
#define ARENASZ 0x00800000u
#define STKTOP  0x11000000u                  /* the guest stack (64 KB below) */
#define RETMAGIC 0x0BADC0DEu
#define GUARD   64u

static guest_fn fn_at(uint32_t a){
    uint32_t lo = 0, hi = g_fn_count;
    while (lo < hi){ uint32_t m = (lo + hi) / 2; if (g_fn_addr[m] < a) lo = m + 1; else hi = m; }
    return lo < g_fn_count && g_fn_addr[lo] == a ? g_fn_ptr[lo] : 0;
}

/* the table targets the copies dispatched to */
#define MAXT 128
static uint32_t g_seen[MAXT]; static unsigned g_nseen;
static uint32_t g_left;                      /* a jump to an address that is no lifted function */
static void seen(uint32_t a){
    for (unsigned i = 0; i < g_nseen; i++) if (g_seen[i] == a) return;
    if (g_nseen < MAXT) g_seen[g_nseen++] = a;
}

#ifdef __wasm__
static void left(CPU *c, uint32_t a){ g_left = a; c->eip = 0; }   /* the case fails: eip is not RETMAGIC */
void dispatch_indirect(CPU *c, uint32_t a){
    guest_fn f = fn_at(a);
    seen(a);
    if (f) f(c); else left(c, a);
}
/* DISPATCH_TAIL's way on wasm when g_dtab has no entry: the target in eip,
 * a tail call */
void dispatch_tail(CPU *c){
    uint32_t a = c->eip;
    guest_fn f = fn_at(a);
    seen(a);
    if (f){ __attribute__((musttail)) return f(c); }
    left(c, a);
}
#else
static sigjmp_buf g_jb;
static volatile uint32_t g_fault_at;
void dispatch_indirect(CPU *c, uint32_t a){
    guest_fn f = fn_at(a);
    seen(a);
    if (f){ f(c); return; }
    g_left = a; siglongjmp(g_jb, 2);
}
static void on_fault(int sig, siginfo_t *si, void *uc){
    (void)sig; (void)uc;
    g_fault_at = (uint32_t)((uint8_t *)si->si_addr - g_mem);
    siglongjmp(g_jb, 1);
}
void mem_init(void); void mem_commit(uint32_t, uint32_t);
#endif
/* nothing the copies run needs these: a call is a failure */
void unsupported_insn(CPU *c, uint32_t a, const char *mn){ fprintf(stderr, "trap at 0x%08x (%s)\n", a, mn); g_left = a; c->eip = 0; }
void cpuid_shim(CPU *c){ (void)c; fprintf(stderr, "cpuid in a copy routine\n"); exit(1); }
uint64_t rdtsc_shim(void){ fprintf(stderr, "rdtsc in a copy routine\n"); exit(1); }

static uint64_t g_rs = 0x9E3779B97F4A7C15ull;
static uint64_t rnd64(void){ g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return g_rs; }

typedef struct { uint32_t va; guest_fn fn; const char *name; } Target;
static const Target T[] = {
    { 0x007C4840u, sub_007c4840, "memmove" },
    { 0x007D6D30u, sub_007d6d30, "its twin" },
};
/* the tables inside each routine, as offsets from its entry, and the indexes
 * its code can reach (from the disassembly of 0x7C4840):
 *   +0x060 [eax*4]  eax = dst & 3, not 0          (head of a forward copy)
 *   +0x0E0 [ecx*4]  ecx = dwords left, 0..7
 *   +0x14C [edx*4]  edx = bytes left, 0..3; also reached as +0x15C [ecx*4]
 *                   with ecx = count - 4 < 0 (a count below 4)
 *   +0x1EC [eax*4]  eax = dst & 3, not 0          (head of a backward copy)
 *   +0x298 [ecx*4]  after neg ecx: 0..-7          (the backward table)
 *   +0x2E8 [edx*4] and [ecx*4]: 0..3 */
static const struct { uint32_t off; int lo, hi; } TAB[] = {
    { 0x060, 1, 3 }, { 0x0E0, 0, 7 }, { 0x14C, 0, 3 }, { 0x1EC, 1, 3 }, { 0x298, -7, 0 }, { 0x2E8, 0, 3 },
};

static uint8_t *g_exp;                       /* the expected bytes of the span */
static long g_cases, g_fail;
static int run_case(const Target *t, uint32_t src, uint32_t dst, uint32_t len, const char *mode){
    uint32_t lo = (src < dst ? src : dst) - GUARD, hi = (src > dst ? src : dst) + len + GUARD, n = hi - lo;
    for (uint32_t i = 0; i < n; i += 8){ uint64_t r = rnd64(); memcpy(g_mem + lo + i, &r, n - i < 8 ? n - i : 8); }
    memcpy(g_exp, g_mem + lo, n);
    memmove(g_exp + (dst - lo), g_exp + (src - lo), len);
    static CPU cpu; CPU *c = &cpu;
    memset(c, 0, sizeof *c);
    c->fcw = 0x027f;
    c->eax = (uint32_t)rnd64(); c->ecx = (uint32_t)rnd64(); c->edx = (uint32_t)rnd64();
    c->ebx = 0xB0B0B0B0u; c->esi = 0x51515151u; c->edi = 0xD1D1D1D1u; c->ebp = 0xBEBEBEBEu;
    c->esp = STKTOP;
    PUSH32(len); PUSH32(src); PUSH32(dst); PUSH32(RETMAGIC);
    uint32_t esp0 = c->esp;
    const char *why = 0;
    g_left = 0;
#ifdef __wasm__
    t->fn(c);
#else
    int r = sigsetjmp(g_jb, 1);
    if (r == 0) t->fn(c);
    else why = r == 1 ? "guest memory fault" : "jumped to an address that is no lifted function";
#endif
    if (!why && g_left) why = "left the lifted code";
    int at = -1;
    if (!why){
        if (c->eip != RETMAGIC) why = "eip";
        else if (c->esp != esp0 + 4u) why = "esp";
        else if (c->eax != dst) why = "eax";
        else if (c->ebx != 0xB0B0B0B0u || c->esi != 0x51515151u || c->edi != 0xD1D1D1D1u || c->ebp != 0xBEBEBEBEu) why = "ebx/esi/edi/ebp";
        else if (c->df) why = "direction flag";
        else if (MR32(esp0 + 4u) != dst || MR32(esp0 + 8u) != src || MR32(esp0 + 12u) != len) why = "arguments";
        else for (uint32_t i = 0; i < n; i++) if (g_mem[lo + i] != g_exp[i]){ at = (int)i; why = "bytes"; break; }
    }
    g_cases++;
    if (!why) return 0;
    if (g_fail++ < 10){
        printf("  FAIL %s 0x%08x: src %08x (mod 16 = %2u) dst %08x (%2u) len %u, %s: %s", t->name, t->va, src, src & 15,
               dst, dst & 15, len, mode, why);
#ifndef __wasm__
        if (g_fault_at && why[0] == 'g') printf(" at %08x", g_fault_at);
#endif
        if (g_left) printf(" (0x%08x)", g_left);
        if (at >= 0) printf(" at span byte %d (address %08x): %02x, expected %02x", at, lo + (uint32_t)at, g_mem[lo + (uint32_t)at], g_exp[at]);
        printf("\n");
    }
    return 1;
}

/* the overlap distances for the smallest one d0 (1..16) the alignments
 * allow: d0, one in the middle and the largest below len (d0 + 16 k); a
 * d0 >= len (no overlap left at this alignment and length) is still run
 * once: the regions touch */
static uint32_t dists(uint32_t d0, uint32_t len, uint32_t *ds){
    uint32_t nd = 0;
    ds[nd++] = d0;
    if (len > d0 + 16u){
        uint32_t dmax = d0 + (len - 1u - d0) / 16u * 16u, dmid = d0 + (dmax - d0) / 32u * 16u;
        if (dmid != d0 && dmid != dmax) ds[nd++] = dmid;
        if (dmax != d0) ds[nd++] = dmax;
    }
    return nd;
}
/* every overlap of one alignment pair and length */
static void cases(const Target *t, uint32_t sa, uint32_t da, uint32_t len){
    uint32_t base = ARENA + 0x1000u, far = (base + len + 0x2000u + 63u) / 64u * 64u;
    /* apart: destination above, then below the source */
    run_case(t, base + sa, far + da, len, "apart, dst above");
    run_case(t, far + sa, base + da, len, "apart, dst below");
    uint32_t mid = ARENA + ARENASZ / 2u, ds[3], nd;
    /* forward: dst = src - d, d = sa - da mod 16 (+ 16 k) */
    nd = dists(((sa - da) & 15u) ? (sa - da) & 15u : 16u, len, ds);
    for (uint32_t k = 0; k < nd; k++) run_case(t, mid + sa, mid + sa - ds[k], len, "overlap, dst below");
    /* backward: dst = src + d, d = da - sa mod 16 (+ 16 k) */
    nd = dists(((da - sa) & 15u) ? (da - sa) & 15u : 16u, len, ds);
    for (uint32_t k = 0; k < nd; k++) run_case(t, mid + sa, mid + sa + ds[k], len, "overlap, dst above");
    if (sa == da) run_case(t, mid + sa, mid + sa, len, "same address");
}

int main(int argc, char **argv){
    (void)argc; (void)argv;
    setvbuf(stdout, 0, _IOLBF, 0);
    /* the exe on stdin */
    size_t n; uint8_t *exe = mwimage_read(0, &n);
    if (!exe) return 2;
#ifndef __wasm__
    mem_init();
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
#endif
    MwImage im;
    if (mwimage_parse(exe, n, 0, 0, &im)) return 2;
#ifndef __wasm__
    mem_commit(im.base, im.size);
    mem_commit(ARENA, ARENASZ); mem_commit(STKTOP - 0x10000u, 0x10000u);
#endif
    if (mwimage_parse(exe, n, 1, g_mem, &im)) return 2;
    free(exe);
    g_exp = malloc(ARENASZ);
    static const uint32_t BIG[] = { 301, 511, 512, 513, 1000, 1027, 4095, 4096, 4097, 65549, 1048583 };
    int bad = 0;
    /* native: a table jump is dispatch_indirect, which sees every target.  On
     * wasm DISPATCH_TAIL finds the target in g_dtab and tail-calls it with no
     * call in between: the first pass runs that way (the game's way), the
     * second with an empty g_dtab, so each jump goes through dispatch_tail,
     * which counts the targets */
#ifdef __wasm__
    const int npass = 2;
#else
    const int npass = 1;
#endif
    for (int pass = 0; pass < npass; pass++){
        memset(g_dtab, 0, sizeof g_dtab);
        if (pass == 0)
            for (uint32_t i = 0; i < g_fn_count; i++){
                uint32_t o = g_fn_addr[i] - DISPATCH_TEXT_BASE;
                if (o < DISPATCH_TEXT_SIZE) g_dtab[o] = g_fn_ptr[i];
            }
        printf("Most Wanted's lifted memmove against a reference memmove (%s)\n",
#ifdef __wasm__
               pass == 0 ? "wasm under node:wasi, table jumps through g_dtab" : "wasm under node:wasi, table jumps through dispatch_tail"
#else
               "native"
#endif
               );
        for (unsigned t = 0; t < sizeof T / sizeof T[0]; t++){
            long c0 = g_cases, f0 = g_fail;
            g_nseen = 0;
            for (uint32_t len = 0; len <= 300; len++)
                for (uint32_t s = 0; s < 16; s++) for (uint32_t d = 0; d < 16; d++) cases(&T[t], s, d, len);
            for (unsigned b = 0; b < sizeof BIG / sizeof BIG[0]; b++)
                for (uint32_t s = 0; s < 16; s++) for (uint32_t d = 0; d < 16; d++) cases(&T[t], s, d, BIG[b]);
            printf("  0x%08x %-9s %8ld cases, %ld failed", T[t].va, T[t].name, g_cases - c0, g_fail - f0);
            if (npass == 2 && pass == 0){ printf("\n"); continue; }
            /* every reachable table entry was a target */
            int missing = 0, entries = 0;
            for (unsigned k = 0; k < sizeof TAB / sizeof TAB[0]; k++)
                for (int i = TAB[k].lo; i <= TAB[k].hi; i++){
                    uint32_t slot = T[t].va + TAB[k].off + (uint32_t)(4 * i), tg = MR32(slot), hit = 0;
                    for (unsigned q = 0; q < g_nseen; q++) if (g_seen[q] == tg) hit = 1;
                    entries++;
                    if (!hit){ missing++; printf("\n  FAIL %s: table entry [0x%08x] = 0x%08x was never a target", T[t].name, slot, tg); }
                    if (!fn_at(tg)){ missing++; printf("\n  FAIL %s: table entry [0x%08x] = 0x%08x is no lifted function", T[t].name, slot, tg); }
                }
            printf("; %u distinct targets dispatched to, %d reachable table entries, %d missed\n", g_nseen, entries, missing);
            bad += missing;
        }
    }
    printf("result: %s (%ld cases, %ld failed)\n", g_fail || bad ? "FAIL" : "PASS", g_cases, g_fail);
    return g_fail || bad ? 1 : 0;
}
