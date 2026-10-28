/* mworacle.c -- differential test of Most Wanted's lifted exe (gen/mw/all)
 * against the ORIGINAL x86 code of speed.exe under Unicorn, on a sample of
 * functions that need no import and no HLE.
 *
 *   tests/mworacle.sh [cases-per-function] [functions]   (picks, builds and runs it)
 *
 * tests/mworacle_pick.py picks the functions (fully lifted closures of at most
 * 12 functions: no indirect call or jump but a switch's, no trap, no
 * CPUID/RDTSC, no int3)
 * and reads from each one's code which stack arguments and registers it
 * dereferences, reads as a float or a double.  For every case both sides start
 * from the same state:
 *   - the image as the loader maps it at 0x00400000 (the file's sections),
 *     read-only in the 16 KB host pages that lie wholly in read-only sections
 *     and the same on Unicorn (the host's page is the native side's granule),
 *   - a 64 KB scratch area of random dwords, a third of them pointers into it,
 *     floats of the game's sizes, small numbers, text; the zero-initialised
 *     part of .data (bss) filled the same way once,
 *   - a 64 KB stack of random bytes with the return address (an unmapped
 *     sentinel) and 16 argument slots, filled by the function's spec
 *     (pointers into the scratch area, small indexes, floats, doubles,
 *     values),
 *   - a TEB at fs (SEH chain end, stack bounds, a TLS array),
 *   - random general registers (pointers where the code uses them as
 *     addresses) and arithmetic flags, DF clear,
 *   - the x87 control word of a game thread (0x027F, PC=53) or of the main
 *     thread after Direct3D's CreateDevice (0x007F, PC=24), an empty x87
 *     stack but for the values the code takes there (the C runtime's helpers),
 *     random x87 registers below it, random XMM and MMX registers, MXCSR 0x1F80,
 *   - no signaling NaN anywhere in these inputs (quiet()).
 * The lifted function runs on the runtime's guest memory, the original under
 * Unicorn on private copies.  After a return both must have: the same eight
 * general registers and eip, DF, the arithmetic flags when the function
 * returns flags (tools/flagabi.py's list: a FLAGABI lift leaves them
 * undefined elsewhere; the ones every path to its ret defines), the same x87
 * top, control word and live stack slots (bit-exact as 80-bit values), XMM
 * registers, MMX registers (MMX code), and the same bytes in the scratch
 * area, the stack from the returned esp up, the TEB and the image's writable
 * pages.  A fault must happen on both sides at the same address.
 *
 * Classes that are not differences, each counted on its own:
 *   loop     both sides at their limit (the original after ORACLE_BUDGET
 *            Unicorn instructions, the lifted after ORACLE_LIFT_MS of wall
 *            clock), and both still at a long limit (ORACLE_LONG_S) when run
 *            again.  The two limits are in different units (L-208), so a case
 *            at its limit runs again before it counts: a lifted side at its
 *            limit with ORACLE_LONG_S (when the original is at its limit too
 *            and the lifted side still is, the original also with
 *            ORACLE_LONG_S); then, when the lifted side has ended and the
 *            original has not, the original with what is left of
 *            ORACLE_LONG_ORIG_S, a time budget per function (the
 *            original-loop re-run; ORACLE_LONG_S once it is spent).  Unicorn
 *            needs 8 to 61 times the native lifted code's time on a long
 *            loop: 0x852570 5.0 s lifted, 67.5 s Unicorn; 0x5B4E70 3.6 s,
 *            84.5 s; 0x7FD823 2.0 s, 122 s; 0x8184E5 21.3 s, 181 s.  So the
 *            original gets the longer limit, and a lifted side still at its
 *            limit after ORACLE_LONG_S where the original returned is a
 *            difference, not a slow emulator.  A re-run that
 *            ends one-sided (one side returns or faults, the other is still
 *            at its limit) is a DIFF, both ways: that lifted side, and an
 *            original still at its limit when the budget is spent where the
 *            lifted side returned.  Once a re-run of the lifted side, of the
 *            original alone (after the budget) or of both still stops, the
 *            function gets no more re-runs of that kind (a budget each); the
 *            both-sides one also goes when its original needed more than
 *            ORACLE_LONG_S, so a later case at its limit on both sides counts
 *            as a loop, as it did before the original-loop re-run: the
 *            budget pays for at most one both-sides case that needs more
 *            than ORACLE_LONG_S, besides the one-sided ones.  ORACLE_MUTATE=
 *            loop (test only) makes every lifted run end at its limit,
 *            ORACLE_MUTATE=eax flips bit 0 of the lifted side's eax after a
 *            return: both must give DIFFs (tests/mworacle_rules.sh)
 *   #DE      the original raised a divide error (a zero divisor or a quotient
 *            too large: Windows would end the game there); the lift skips a
 *            zero divisor and truncates (tools/lift.py div/idiv)
 *   libm     the case differed after an x87 instruction the lift computes
 *            with libm (fsin fcos fsincos fptan f2xm1 fyl2x fpatan fprem
 *            fprem1) or a
 *            constant it loads as a double (fldpi fldl2e fldl2t fldlg2 fldln2)
 *            and is the same when the original runs again with the lift's
 *            formula or constant there (on_xsite); among them the
 *            sine/cosine/tangent operands >= 2^63 or infinite, which x87
 *            leaves unchanged and libm reduces
 *   range    an x87 value on the original's path was one no double holds
 *            (out of range, or a significand over 53 bits: PC=64, tbyte
 *            loads, 64-bit integers), which the lift's double registers
 *            cannot match (x87_range_check), and the case is the same when
 *            the x87 stack values and the bytes the original's x87 stores
 *            wrote are left out (CMP_NOVALS: the registers, flags, x87
 *            control word and top, XMM registers, the rest of memory)
 *   read back  the same, but integer code reads back an fstp m80 of such
 *            a value, whose bytes are the lift's double's (f80_store), not
 *            the 80-bit value's: the case is the same (CMP_NOVALS) when the
 *            original runs again with each fstp m80 of a value no double
 *            holds writing that value as the lift holds it, the 80-bit image
 *            of the nearest double (f80_rne) or of f80_load's double (the
 *            lift's fld m80) -- the lifted side's own bytes there when they
 *            are one of those two images, else the nearest double's -- and a
 *            non-x87 instruction reads such bytes before anything else writes
 *            them (range_subst).  Every register and every other byte must
 *            be the same then.  0x7D8CC0, the C runtime's FDIV workaround,
 *            scales an 80-bit denormal divisor by 2^63, stores it and loads
 *            its significand's high dword into eax (0x7D8DBC, 0x7D8CC8):
 *            x87 keeps a value below 2^-16319, the lift 0 (cpu.h f80_load
 *            flushes the denormal; tools/isatest.py pins that sequence, the
 *            lift's model and not Intel's result).  Nothing in MW passes it such
 *            an operand: it runs only when _adjust_fdiv [0x9BA9F4] is 1
 *            (IsProcessorFeaturePresent(0), else the FDIV test 0x7CD9C0: 0)
 *   fsw      the same but for a register holding an fnstsw's low byte, where
 *            the original has x87 exception flags its status word still shows
 *            and the lift keeps none (cpu.h fsw_get), and the case is the
 *            same when the original runs again with its exception flags
 *            cleared before every fnstsw (fsw_rerun)
 *   nan      the only differing values are NaNs on both sides, in a function
 *            with x87 or SSE code, one of each pair the default NaN (sign
 *            aside: the host's, which the lift makes, or the indefinite,
 *            which x87 and SSE make); other payloads must be the same.  On
 *            the x87 stack, whose lifted values are doubles: the lifted one
 *            the default NaN, or the two the same in the bits a double holds
 *   order    both faulted, at different addresses, and the original touches
 *            the lifted side's address too (fault_order: clang reorders
 *            loads no store separates)
 *   dead     the original faulted, the lifted side did not, and the
 *            original given zero pages there ends exactly as the lifted side,
 *            or with a divide error and then, given pages of 1s, exactly as
 *            the lifted side (dead_access: clang drops a load whose value is
 *            unused)
 * Everything else is a DIFF.  ORACLE_FNS=<hex,...> runs those functions only,
 * ORACLE_CASE=<n> that case only, printing both sides' state.
 *
 * Test-only: Unicorn is GPLv2 and never part of the game binary or the app. */
#include "cpu.h"
#include "mwimage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unicorn/unicorn.h>

typedef void (*guest_fn)(CPU *);
extern const uint32_t g_fn_count;
extern const uint32_t g_fn_addr[];
extern const guest_fn g_fn_ptr[];
guest_fn_t g_dtab[DISPATCH_TEXT_SIZE];
void mem_init(void); void mem_commit(uint32_t, uint32_t);

/* ---- layout --------------------------------------------------------------- */
#define SCR      0x20000000u
#define SCRSZ    0x00010000u
#define STK      0x30000000u
#define STKSZ    0x00010000u
#define ESPC     (STK + STKSZ - 0x400u)      /* esp at the call: the return address */
#define TEB      0x31000000u
#define TEBSZ    0x00004000u
#define SENTINEL 0xFFFE0000u                 /* the return address: unmapped, never executed */
#define GDT_VA   0xFFFF0000u
#define HPG      0x4000u                     /* the host's page: the native protection granule */

static MwImage g_im;
static uint32_t g_img_lo, g_img_hi;          /* the image, rounded to host pages */
static uint8_t *g_img0;                      /* the freshly loaded image [g_img_lo, g_img_hi) */
/* the image's natively writable host pages: the only image bytes a case can change */
static struct { uint32_t lo, hi; } g_rw[16]; static int g_nrw;

static guest_fn fn_at(uint32_t a){
    uint32_t lo = 0, hi = g_fn_count;
    while (lo < hi){ uint32_t m = (lo + hi) / 2; if (g_fn_addr[m] < a) lo = m + 1; else hi = m; }
    return lo < g_fn_count && g_fn_addr[lo] == a ? g_fn_ptr[lo] : 0;
}

/* ---- the lifted side's way out ----------------------------------------- */
static sigjmp_buf g_jb;
static volatile uint32_t g_left_at, g_fault_at;
/* nothing in a picked closure calls these (tests/mworacle_pick.py): a call is
 * an outcome of its own, "left" */
void dispatch_indirect(CPU *c, uint32_t a){ (void)c; g_left_at = a; siglongjmp(g_jb, 2); }
void unsupported_insn(CPU *c, uint32_t a, const char *mn){ (void)c; (void)mn; g_left_at = a; siglongjmp(g_jb, 3); }
void cpuid_shim(CPU *c){ (void)c; g_left_at = 0xC0; siglongjmp(g_jb, 2); }
uint64_t rdtsc_shim(void){ g_left_at = 0xC1; siglongjmp(g_jb, 2); }
static volatile int g_phase;                 /* 1 while the lifted code runs, 2 while Unicorn does */
static void on_fault(int sig, siginfo_t *si, void *uc){
    (void)sig; (void)uc;
    uint8_t *p = (uint8_t *)si->si_addr;
    if (p < g_mem || p >= g_mem + GSZ || g_phase != 1){
        char m[160];
        int n = snprintf(m, sizeof m, "mworacle: a host fault (signal %d at %p) %s\n", sig, (void *)p,
                         g_phase == 1 ? "in the lifted code, outside guest memory" : g_phase == 2 ? "in Unicorn" : "in the test");
        write(2, m, (size_t)n); _exit(3);
    }
    g_fault_at = (uint32_t)(p - g_mem);
    siglongjmp(g_jb, 1);
}
static void on_alarm(int sig){ (void)sig; g_left_at = 0; siglongjmp(g_jb, 4); }

/* ---- the sample ---------------------------------------------------------- */
enum { T_X87 = 1, T_MMX = 2, T_SSE = 4, T_FS = 8, T_RFL = 16, T_FSW = 32 };
typedef struct { uint32_t va; int nfn; char args[33]; char regs[8]; unsigned tr; guest_fn fn; uint32_t xs[16]; char xt[16]; int nxs, xin; uint32_t rfl; } Fn;
static Fn *g_fns; static int g_nfns;
static int load_sample(const char *path){
    FILE *f = fopen(path, "r");
    if (!f){ fprintf(stderr, "%s: cannot open\n", path); return 1; }
    char ln[256]; int cap = 0;
    while (fgets(ln, sizeof ln, f)){
        if (ln[0] == '#' || ln[0] == '\n') continue;
        if (g_nfns == cap) g_fns = realloc(g_fns, sizeof *g_fns * (size_t)(cap = cap ? 2 * cap : 2048));
        Fn *q = &g_fns[g_nfns]; memset(q, 0, sizeof *q);
        char a[40], r[16], t[16], g[400];
        if (sscanf(ln, "%x %d %39s %15s %15s %399s", &q->va, &q->nfn, a, r, t, g) != 6 || strlen(r) != 7){
            fprintf(stderr, "%s: bad line: %s", path, ln); fclose(f); return 1; }
        for (char *p = g; *p && *p != '-' && q->nxs < 16; ){
            q->xs[q->nxs] = (uint32_t)strtoul(p, &p, 16);
            if (*p == ':') p++;
            q->xt[q->nxs++] = *p ? *p++ : '?';
            if (*p == ',') p++;
        }
        snprintf(q->args, sizeof q->args, "%s", strcmp(a, "-") ? a : "");
        memcpy(q->regs, r, 7);
        for (const char *p = t; *p; p++){
            q->tr |= *p == 'x' ? T_X87 : *p == 'm' ? T_MMX : *p == 's' ? T_SSE : *p == 'F' ? T_FS : *p == 'R' ? T_RFL
                   : *p == 'w' ? T_FSW : 0;
            if (*p == 'x' && p[1] >= '1' && p[1] <= '7') q->xin = p[1] - '0';
            if (*p == 'R'){ q->rfl = (uint32_t)strtoul(p + 1, 0, 16); break; }
        }
        if (!(q->fn = fn_at(q->va))){ fprintf(stderr, "%s: 0x%08x is no lifted function\n", path, q->va); fclose(f); return 1; }
        g_nfns++;
    }
    fclose(f);
    return 0;
}

/* ---- random inputs ------------------------------------------------------- */
static uint64_t g_rs;
static uint32_t rnd_raw(void){ g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 16); }
static uint32_t quiet(uint32_t v);
static uint32_t rnd(void){ return quiet(rnd_raw()); }
/* a float of the sizes game code works with, now and then a special one */
static float rnd_float(void){
    uint32_t k = rnd() % 100;
    if (k < 8){ static const float S[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 100.0f, 1e-6f }; return S[rnd() % 8]; }
    if (k < 10){ static const uint32_t B[] = { 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x00000001u, 0x00400000u, 0x7F7FFFFFu };
                 float v; uint32_t b = B[rnd() % 6]; memcpy(&v, &b, 4); return v; }
    if (k < 25) return (float)((int)(rnd() % 2001) - 1000);
    float m = ldexpf((float)(rnd() & 0xFFFFFF) / 16777216.0f + 0.5f, (int)(rnd() % 40) - 20);
    return rnd() & 1 ? -m : m;
}
static double rnd_double(void){
    if (rnd() % 4 == 0) return (double)rnd_float();
    double m = ldexp((double)(((uint64_t)rnd() << 21) ^ rnd()) / 9007199254740992.0 + 0.5, (int)(rnd() % 60) - 30);
    return rnd() & 1 ? -m : m;
}
static uint32_t f2u(float v){ uint32_t u; memcpy(&u, &v, 4); return u; }
/* no input holds a signaling NaN, as a float or as a double's high half: the
 * lift loads floats with the host's conversion, which quiets one, so x87's
 * rule for an SNaN meeting a QNaN (the QNaN wins, SDM vol. 1 table 4-7; the
 * lift then compares two quiet NaNs and takes the larger significand) is not
 * modelled.  Found here at 0x487500 (fsub with an SNaN in memory; ORACLE_SNAN=1
 * keeps them, to see it); game data holds no SNaN */
static int g_keep_snan;                      /* ORACLE_SNAN=1: inputs keep their signaling NaNs (to see the class) */
static uint32_t quiet(uint32_t v){
    if (g_keep_snan) return v;
    if ((v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu) && !(v & 0x400000u)) v |= 0x400000u;
    if ((v & 0x7FF00000u) == 0x7FF00000u && !(v & 0x80000u)) v |= 0x80000u;
    return v;
}
static uint32_t scr_ptr(void){
    /* into the scratch area, 16-aligned mostly (SSE's aligned loads), away from its end */
    uint32_t o = rnd() % (SCRSZ - 0x800u);
    return SCR + (rnd() % 4 ? o & ~15u : o & ~3u);
}
static uint32_t rnd_value(void){
    switch (rnd() % 10){
    case 0: case 1: return rnd() % 16;
    case 2: return (uint32_t)((int)(rnd() % 3) - 1);
    case 3: return rnd() % 1000;
    case 4: return scr_ptr();
    case 5: return f2u(rnd_float());
    case 6: return rnd() % 0x10000;
    default: return rnd();
    }
}
static uint32_t rnd_ptr(void){ uint32_t k = rnd() % 20; return k == 0 ? 0 : k == 1 ? rnd() : scr_ptr(); }
static uint32_t rnd_index(void){ uint32_t k = rnd() % 10; return k < 7 ? rnd() % 16 : k < 9 ? rnd() % 256 : rnd_value(); }

/* the case's inputs */
typedef struct {
    uint32_t r[8];                           /* eax ecx edx ebx esp ebp esi edi */
    uint32_t fl;                             /* EFLAGS */
    uint16_t fcw; int top; double st[8];
    uint8_t xmm[8][16]; uint64_t mm[8];
} In;
static In g_in;
static uint8_t g_in_scr[SCRSZ], g_in_stk[STKSZ], g_in_teb[TEBSZ];

/* a dword of the scratch area: a third pointers into it, zeros, small
 * numbers, floats, text, anything */
static uint32_t rnd_dword(void){
    uint32_t k = rnd() % 12;
    return k < 4 ? scr_ptr() : k == 4 ? 0 : k == 5 ? rnd() % 64 : k < 8 ? f2u(rnd_float())
         : k == 8 ? 0x20202020u | (rnd() & 0x5F5F5F5Fu) : rnd();
}
static void make_case(const Fn *f){
    uint32_t *s = (uint32_t *)g_in_scr;
    for (uint32_t i = 0; i < SCRSZ / 4; i++) s[i] = rnd_dword();
    /* a few strings */
    for (int n = 0; n < 8; n++){
        uint32_t o = rnd() % (SCRSZ - 256u), l = rnd() % 64;
        for (uint32_t i = 0; i < l; i++) g_in_scr[o + i] = (uint8_t)(0x20 + rnd() % 95);
        g_in_scr[o + l] = 0;
    }
    for (uint32_t i = 0; i < STKSZ / 4; i++) ((uint32_t *)g_in_stk)[i] = rnd() % 3 ? rnd() : scr_ptr();
    uint32_t *a = (uint32_t *)(g_in_stk + (ESPC - STK));
    a[0] = SENTINEL;
    size_t na = strlen(f->args);
    for (uint32_t k = 0; k < 16; k++){
        char ch = k < na ? f->args[k] : 'v';
        if (ch == 'p') a[1 + k] = rnd_ptr();
        else if (ch == 'i') a[1 + k] = rnd_index();
        else if (ch == 'f') a[1 + k] = f2u(rnd_float());
        else if (ch == 'd' && k + 1 < 16){ double d = rnd_double(); memcpy(&a[1 + k], &d, 8); k++; }
        else a[1 + k] = rnd_value();
    }
    memset(g_in_teb, 0, TEBSZ);
    uint32_t *t = (uint32_t *)g_in_teb;
    t[0] = 0xFFFFFFFFu; t[1] = STK + STKSZ; t[2] = STK; t[0x18 / 4] = TEB; t[0x2C / 4] = TEB + 0x100u;
    t[0x30 / 4] = TEB + 0x1000u;
    for (int i = 0; i < 64; i++) t[0x100 / 4 + i] = scr_ptr();
    static const int RIX[7] = { 0, 1, 2, 3, 6, 7, 5 };   /* eax ecx edx ebx esi edi ebp in r[] */
    for (int i = 0; i < 7; i++) g_in.r[RIX[i]] = f->regs[i] == 'p' ? rnd_ptr() : f->regs[i] == 'i' ? rnd_index() : rnd_value();
    g_in.r[4] = ESPC;
    g_in.fl = 0x202u | (rnd() & 0x8D5u);     /* CF PF AF ZF SF OF random, DF clear */
    g_in.fcw = rnd() & 1 ? 0x027f : 0x007f;
    g_in.top = (8 - f->xin) & 7;             /* the x87 stack arguments the code reads (tests/mworacle_pick.py) */
    for (int i = 0; i < 8; i++) g_in.st[i] = g_in.fcw == 0x007f ? (double)rnd_float() : rnd_double();
    for (int i = 0; i < 8; i++){
        for (int j = 0; j < 4; j++){ uint32_t v = rnd() % 4 ? f2u(rnd_float()) : rnd(); memcpy(&g_in.xmm[i][4 * j], &v, 4); }
        g_in.mm[i] = ((uint64_t)rnd() << 32) | rnd();
    }
}

/* ---- the native image's protection ---------------------------------------- */
static int sec_writable(uint32_t a){
    for (uint32_t i = 0; i < g_im.nsec; i++){
        const MwSec *s = &g_im.sec[i];
        uint32_t hi = (s->va + s->vsize + 0xFFFu) & ~0xFFFu;
        if (a >= s->va && a < hi) return (s->chr & 0x80000000u) != 0;
    }
    return 0;                                /* headers, past the end: read-only */
}
static void protect_image(void){
    for (uint32_t p = g_img_lo; p < g_img_hi; p += HPG){
        int w = 0;
        for (uint32_t q = p; q < p + HPG; q += 0x1000u) w |= sec_writable(q);
        if (w){
            if (g_nrw && g_rw[g_nrw - 1].hi == p) g_rw[g_nrw - 1].hi = p + HPG;
            else { g_rw[g_nrw].lo = p; g_rw[g_nrw].hi = p + HPG; g_nrw++; }
        } else mprotect(g_mem + p, HPG, PROT_READ);
    }
}

/* ---- Unicorn -------------------------------------------------------------- */
static uc_engine *g_uc;
static uint8_t *u_img, *u_scr, *u_stk, *u_teb, u_gdt[0x1000];
static int g_u_ev; static uint32_t g_u_addr; static int g_u_intno;   /* ev: 1 returned, 2 fault, 3 fetch, 5 interrupt */
static bool on_umem(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u){
    (void)uc; (void)sz; (void)v; (void)u;
    if (g_u_ev) return false;
    if ((t == UC_MEM_FETCH_UNMAPPED || t == UC_MEM_FETCH_PROT) && (uint32_t)a == SENTINEL) g_u_ev = 1;
    else if (t == UC_MEM_FETCH_UNMAPPED || t == UC_MEM_FETCH_PROT) g_u_ev = 3;
    else g_u_ev = 2;
    g_u_addr = (uint32_t)a;
    return false;
}
static void on_intr(uc_engine *uc, uint32_t intno, void *u){
    (void)u;
    if (!g_u_ev){ g_u_ev = 5; g_u_intno = (int)intno; uc_reg_read(uc, UC_X86_REG_EIP, &g_u_addr); }
    uc_emu_stop(uc);
}
static void gdt_entry(int i, uint32_t base, uint32_t limit, int code, int dpl){
    uint8_t *d = u_gdt + 8 * i;
    limit >>= 12;
    d[0] = limit & 0xFF; d[1] = (limit >> 8) & 0xFF;
    d[2] = base & 0xFF; d[3] = (base >> 8) & 0xFF; d[4] = (base >> 16) & 0xFF;
    d[5] = (uint8_t)(0x80 | (dpl << 5) | 0x10 | (code ? 0x0A : 0x02));
    d[6] = (uint8_t)(0xC0 | ((limit >> 16) & 0x0F));
    d[7] = (base >> 24) & 0xFF;
}
static void *pg_alloc(size_t n){ void *p = 0; if (posix_memalign(&p, 0x4000, n)) return 0; memset(p, 0, n); return p; }
/* The x87 instructions the lift computes with libm (tools/lift.py): fsin,
 * fcos, fsincos, fptan as sin/cos/tan of the double, f2xm1 as
 * exp2(x) - 1.0, fyl2x as y * log2(x), fpatan as atan2(y, x), fprem and
 * fprem1 as fmod and remainder (0x7D96CC: an exact zero remainder of a
 * negative dividend, -0 in the lift as on the hardware, +0 from Unicorn);
 * and fldpi,
 * fldl2e, fldl2t, fldlg2, fldln2, which it loads as doubles where x87 has
 * 64-bit significands (0x7CF261, the C runtime's exp: x * log2(e) one bit
 * apart, 2^-43 apart after 2^x).  Unicorn 2.1
 * (QEMU 5.0) uses host double functions too, but other formulas
 * (pow(2, x) - 1.0, log(x) / log(2.0)), leaves an fsin/fcos/fptan operand of
 * |x| >= 2^63 or infinite as it is (the hardware: unchanged and C2 for
 * |x| >= 2^63, the indefinite NaN for an infinity), and the hardware gives
 * 64-bit results; none of the three agree to the last bit.  A case that
 * differs after such an instruction ran is run again with the lift's
 * formula in Unicorn's place at each of them (on_xsite, g_xs_sub): a case
 * that is then the same differed by those formulas only, a class of its own.
 * |x| >= 2^63 or infinite at a sine, cosine or tangent is noted on the way */
static int g_trig_oor, g_xs_hit, g_xs_sub;
static double f80d(const uint8_t b[10]){
    uint64_t m; uint16_t se; memcpy(&m, b, 8); memcpy(&se, b + 8, 2);
    int sg = se >> 15, e = se & 0x7FFF;
    if (!e && !m) return sg ? -0.0 : 0.0;
    if (e == 0x7FFF) return (m << 1) ? (sg ? -NAN : NAN) : (sg ? -INFINITY : INFINITY);
    uint64_t hi = m >> 11, lo = m & 0x7FF;
    if (lo > 0x400 || (lo == 0x400 && (hi & 1))) hi++;
    double v = ldexp((double)hi, e - 16383 - 52);
    return sg ? -v : v;
}
static void on_xsite(uc_engine *uc, uint64_t a, uint32_t sz, void *u){
    (void)sz;
    char tag = (char)(uintptr_t)u;
    uint16_t sw; uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
    int top = sw >> 11 & 7;
    uint8_t b0[10], b1[10];
    uc_reg_read(uc, UC_X86_REG_FP0 + top, b0); uc_reg_read(uc, UC_X86_REG_FP0 + ((top + 1) & 7), b1);
    uint16_t se; memcpy(&se, b0 + 8, 2);
    if (strchr("scbt", tag) && (se & 0x7FFF) >= 16383 + 63) g_trig_oor = 1;
    g_xs_hit = 1;
    if (!g_xs_sub || MR8((uint32_t)a) != 0xD9) return;
    double x = f80d(b0), y = f80d(b1);
    int push = 0, pop = 0;
    switch (tag){
    case 's': f80_store(b0, sin(x)); break;
    case 'c': f80_store(b0, cos(x)); break;
    case 'b': f80_store(b1, sin(x)); f80_store(b0, cos(x)); push = 1; break;   /* ST(1) = sin, ST(0) = cos */
    case 't': f80_store(b1, tan(x)); f80_store(b0, 1.0); push = 1; break;      /* ST(1) = tan, ST(0) = 1 */
    case 'e': f80_store(b0, exp2(x) - 1.0); break;
    case 'l': f80_store(b1, y * log2(x)); pop = 1; break;
    case 'a': f80_store(b1, atan2(y, x)); pop = 1; break;
    case 'm': case 'M': {                    /* lift.py's fprem/fprem1: the whole remainder, C2 clear */
        double r = x87_r2(x, y, tag == 'm' ? fmod(x, y) : remainder(x, y));
        double q = y != 0.0 ? trunc(x / y) : 0.0;
        uint32_t qi = (uint32_t)(int64_t)fmod(fabs(q), 8.0);
        sw &= (uint16_t)~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
        if (qi & 1u) sw |= FSW_C1;
        if (qi & 2u) sw |= FSW_C3;
        if (qi & 4u) sw |= FSW_C0;
        f80_store(b0, r); break; }
    /* the constants as lift.py writes them (C literals: the nearest double) */
    case 'p': f80_store(b1, 3.14159265358979323846); goto konst;
    case 'q': f80_store(b1, 1.44269504088896340736); goto konst;
    case 'r': f80_store(b1, 3.32192809488736234787); goto konst;
    case 'g': f80_store(b1, 0.30102999566398119521); goto konst;
    case 'n': f80_store(b1, 0.69314718055994530942);
    konst: memcpy(b0, b1, 10); top = (top - 1) & 7; uc_reg_write(uc, UC_X86_REG_FP0 + top, b0);
        sw = (uint16_t)((sw & ~0x3800u) | (top << 11)); uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
        { uint32_t next = (uint32_t)a + 2u; uc_reg_write(uc, UC_X86_REG_EIP, &next); }
        return;
    default: return;
    }
    if (push){
        top = (top - 1) & 7;
        uc_reg_write(uc, UC_X86_REG_FP0 + top, b0); uc_reg_write(uc, UC_X86_REG_FP0 + ((top + 1) & 7), b1);
    } else if (pop){
        uc_reg_write(uc, UC_X86_REG_FP0 + ((top + 1) & 7), b1); top = (top + 1) & 7;
    } else uc_reg_write(uc, UC_X86_REG_FP0 + top, b0);
    sw = (uint16_t)((sw & ~0x3800u) | (top << 11)); uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
    uint32_t next = (uint32_t)a + 2u; uc_reg_write(uc, UC_X86_REG_EIP, &next);
}
static uc_hook g_trig_h[16]; static int g_ntrig_h;
static void trig_hooks(const Fn *f){
    for (int i = 0; i < g_ntrig_h; i++) uc_hook_del(g_uc, g_trig_h[i]);
    g_ntrig_h = 0;
    for (int i = 0; f && i < f->nxs; i++)
        uc_hook_add(g_uc, &g_trig_h[g_ntrig_h++], UC_HOOK_CODE, (void *)on_xsite, (void *)(uintptr_t)f->xt[i], f->xs[i], f->xs[i]);
    uc_ctl_flush_tb(g_uc);
}

/* An x87 value on the original's path that no double holds: above DBL_MAX
 * or below DBL_MIN (but not zero), or with a significand wider than 53 bits.
 * The x87 registers keep a 15-bit exponent whatever the precision control,
 * and 64-bit significands at PC=64, from tbyte loads and from 64-bit
 * integers; the lift's registers are doubles (cpu.h: "PC=53/64 stay double",
 * "Exponent range: extended vs double"), so it overflows to an infinity,
 * loses the value or rounds it where the original goes on (0x7CF261, the C
 * runtime's exp: 2^-1248 there, 0 in the lift; 0x7D8E12, the C runtime's
 * FDIV workaround, divides at PC=64: one double ulp apart).  Checked on
 * ST(0) and ST(1) before every instruction in a re-run of a differing case
 * (range_rerun) */
static int g_x87_range;
static int no_double(const uint8_t b[10]){
    uint16_t se; uint64_t m; memcpy(&m, b, 8); memcpy(&se, b + 8, 2);
    int e = se & 0x7FFF;
    return e != 0x7FFF && m && (e > 16383 + 1023 || e < 16383 - 1022 || (m & 0x7FFu));
}
static void x87_range_check(uc_engine *uc){
    uint16_t sw; uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
    for (int k = 0; k < 2; k++){
        uint8_t b[10]; uc_reg_read(uc, UC_X86_REG_FP0 + ((sw >> 11) + k & 7), b);
        if (no_double(b)) g_x87_range = 1;
    }
}
/* What such a value explains in memory: the bytes the original's x87 value
 * stores wrote in that re-run (fst/fstp m32/m64/m80, fist/fistp/fisttp
 * m16/m32/m64, fbstp, fnsave's image), which the range class leaves out of
 * the compare (x87_stored); any other byte must be the same.  A full list
 * keeps the rest in the compare (stricter, never looser) */
static struct { uint32_t lo, hi; } g_x87st[256]; static int g_nx87st;
static uint32_t g_x87_ip;                    /* the instruction on_x87 saw last */
static int is_x87_store(uint32_t a){
    if (a < g_im.base || a + 8 > g_im.base + g_im.size) return 0;
    uint32_t p = a;
    while (p < a + 4 && (MR8(p) == 0x66 || MR8(p) == 0x67 || MR8(p) == 0x2E || MR8(p) == 0x3E ||
                         MR8(p) == 0x26 || MR8(p) == 0x36 || MR8(p) == 0x64 || MR8(p) == 0x65)) p++;
    uint8_t op = MR8(p), mrm = MR8(p + 1), reg = (uint8_t)(mrm >> 3 & 7);
    if (mrm >= 0xC0) return 0;
    return (op == 0xD9 && (reg == 2 || reg == 3)) || (op == 0xDB && (reg == 1 || reg == 2 || reg == 3 || reg == 7)) ||
           (op == 0xDD && (reg == 1 || reg == 2 || reg == 3 || reg == 6)) ||
           (op == 0xDF && (reg == 1 || reg == 2 || reg == 3 || reg == 6 || reg == 7));
}
static void on_x87w(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u){
    (void)uc; (void)t; (void)v; (void)u;
    if (!is_x87_store(g_x87_ip)) return;
    uint32_t lo = (uint32_t)a, hi = lo + (uint32_t)sz;
    if (g_nx87st && lo <= g_x87st[g_nx87st - 1].hi && hi >= g_x87st[g_nx87st - 1].lo){
        if (lo < g_x87st[g_nx87st - 1].lo) g_x87st[g_nx87st - 1].lo = lo;
        if (hi > g_x87st[g_nx87st - 1].hi) g_x87st[g_nx87st - 1].hi = hi;
    } else if (g_nx87st < (int)(sizeof g_x87st / sizeof g_x87st[0])){ g_x87st[g_nx87st].lo = lo; g_x87st[g_nx87st].hi = hi; g_nx87st++; }
}
static int x87_stored(uint32_t a){
    for (int i = 0; i < g_nx87st; i++) if (a >= g_x87st[i].lo && a < g_x87st[i].hi) return 1;
    return 0;
}
static void on_x87(uc_engine *uc, uint64_t a, uint32_t sz, void *u){ (void)sz; (void)u; g_x87_ip = (uint32_t)a; if (!g_x87_range) x87_range_check(uc); }


/* ORACLE_TRACE=1 (with ORACLE_FNS and ORACLE_CASE): every instruction the
 * original runs, with the x87 stack before it */
static void on_trace(uc_engine *uc, uint64_t a, uint32_t sz, void *u){
    (void)sz; (void)u;
    uint16_t sw; uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
    int top = sw >> 11 & 7;
    uint32_t r[8]; static const int R[8] = { UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                                             UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };
    for (int i = 0; i < 8; i++) uc_reg_read(uc, R[i], &r[i]);
    fprintf(stderr, "  trace %08x eax %08x ecx %08x edx %08x esp %08x top %d", (uint32_t)a, r[0], r[1], r[2], r[4], top);
    for (int k = 0; k < 3; k++){
        uint8_t b[10]; uc_reg_read(uc, UC_X86_REG_FP0 + ((top + k) & 7), b);
        uint64_t m; uint16_t se; memcpy(&m, b, 8); memcpy(&se, b + 8, 2);
        fprintf(stderr, " st%d %04x:%016llx", k, se, (unsigned long long)m);
    }
    fprintf(stderr, "\n");
}
static int uc_setup(void){
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &g_uc) != UC_ERR_OK){ fprintf(stderr, "uc_open failed\n"); return 1; }
    if (!u_img){ u_img = pg_alloc(g_img_hi - g_img_lo); u_scr = pg_alloc(SCRSZ); u_stk = pg_alloc(STKSZ); u_teb = pg_alloc(TEBSZ); }
    memcpy(u_img, g_img0, g_img_hi - g_img_lo);
    memset(u_gdt, 0, sizeof u_gdt);
    /* the image as the native side has it: readable to the end of its last
     * host page, writable in the host pages protect_image left writable (a
     * 16 KB page that holds the end of .rdata, .data and the start of .rsrc
     * is all writable there), executable in .text.  With the loader's 4 KB
     * protection instead, a write just past .data faulted at 0x9C7000 under
     * Unicorn and at the next host page natively (0x565A40), and a read just
     * past the image at 0xA79000 (0x5B61B0) */
    for (uint32_t p = g_img_lo; p < g_img_hi; ){
        uint32_t prot = 0, q;
        for (q = p; q < g_img_hi; q += 0x1000u){
            uint32_t pr = UC_PROT_READ;
            for (int i = 0; i < g_nrw; i++) if (q >= g_rw[i].lo && q < g_rw[i].hi) pr |= UC_PROT_WRITE;
            for (uint32_t i = 0; i < g_im.nsec; i++){
                const MwSec *s = &g_im.sec[i];
                if (q >= s->va && q < ((s->va + s->vsize + 0xFFFu) & ~0xFFFu) && (s->chr & 0x20000000u)) pr |= UC_PROT_EXEC;
            }
            if (q == p) prot = pr; else if (pr != prot) break;
        }
        if (uc_mem_map_ptr(g_uc, p, q - p, prot, u_img + (p - g_img_lo))){ fprintf(stderr, "cannot map 0x%08x\n", p); return 1; }
        p = q;
    }
    if (uc_mem_map_ptr(g_uc, SCR, SCRSZ, UC_PROT_READ | UC_PROT_WRITE, u_scr) ||
        uc_mem_map_ptr(g_uc, STK, STKSZ, UC_PROT_READ | UC_PROT_WRITE, u_stk) ||
        uc_mem_map_ptr(g_uc, TEB, TEBSZ, UC_PROT_READ | UC_PROT_WRITE, u_teb) ||
        uc_mem_map_ptr(g_uc, GDT_VA, 0x1000, UC_PROT_READ | UC_PROT_WRITE, u_gdt)){ fprintf(stderr, "cannot map the test regions\n"); return 1; }
    gdt_entry(1, 0, 0xFFFFF000u, 1, 3);
    gdt_entry(2, 0, 0xFFFFF000u, 0, 3);
    gdt_entry(3, 0, 0xFFFFF000u, 0, 0);
    gdt_entry(4, TEB, 0xFFFFF000u, 0, 3);
    gdt_entry(5, 0, 0xFFFFF000u, 1, 0);
    uc_x86_mmr gdtr = { 0, GDT_VA, 8 * 6 - 1, 0 };
    uc_reg_write(g_uc, UC_X86_REG_GDTR, &gdtr);
    uint32_t sel = (3 << 3) | 0; uc_reg_write(g_uc, UC_X86_REG_SS, &sel);
    sel = (2 << 3) | 3; uc_reg_write(g_uc, UC_X86_REG_DS, &sel); uc_reg_write(g_uc, UC_X86_REG_ES, &sel);
    uc_reg_write(g_uc, UC_X86_REG_GS, &sel);
    sel = (4 << 3) | 3; uc_reg_write(g_uc, UC_X86_REG_FS, &sel);
    uint32_t cr0 = 0x00000033u, cr4 = 0x00000600u;   /* PE MP ET NE; OSFXSR OSXMMEXCPT: SSE on */
    uc_reg_write(g_uc, UC_X86_REG_CR0, &cr0); uc_reg_write(g_uc, UC_X86_REG_CR4, &cr4);
    uc_hook h;
    uc_hook_add(g_uc, &h, UC_HOOK_MEM_UNMAPPED | UC_HOOK_MEM_PROT, (void *)on_umem, 0, 1, 0);
    uc_hook_add(g_uc, &h, UC_HOOK_INTR, (void *)on_intr, 0, 1, 0);
    if (getenv("ORACLE_TRACE")) uc_hook_add(g_uc, &h, UC_HOOK_CODE, (void *)on_trace, 0, 1, 0);
    return 0;
}

/* ---- running a case -------------------------------------------------------- */
enum { O_RET, O_FAULT, O_LEFT, O_TRAP, O_LOOP, O_DE, O_JUMP, O_ERR };
static const char *const ONAME[] = { "ret", "fault", "left", "trap", "loop", "#DE", "jump", "error" };
typedef struct {
    int how; uint32_t where;
    uint32_t r[8], fl; uint16_t fcw, fsw; int top; uint8_t st[8][10]; uint8_t xmm[8][16]; uint64_t mm[8];
} Out;
static Out g_lo, g_uo;                       /* lifted, original */
static uint8_t g_l_scr[SCRSZ], g_l_stk[STKSZ], g_l_teb[TEBSZ];   /* the lifted side's memory after the run */
static uint8_t *g_l_img;                     /* its writable image pages (by g_rw) */

static size_t g_budget = 20000000u;          /* Unicorn instructions per case (ORACLE_BUDGET) */
static int g_lift_ms = 500;                  /* lifted: wall clock per case (ORACLE_LIFT_MS) */
static int g_long_s = 60;                    /* re-run of the lifted side, or of both (ORACLE_LONG_S, 0: none) */
static int g_long_orig_s = 1800;             /* the original-loop re-runs' budget per function (ORACLE_LONG_ORIG_S, 0: none) */
static int g_mutate;                         /* ORACLE_MUTATE (test only): 1 eax, 2 loop */
static int g_u_long;                         /* the case's original: 0 its counted run, else its long re-run's seconds */
static double now_s(void){ struct timeval tv; gettimeofday(&tv, 0); return (double)tv.tv_sec + tv.tv_usec / 1e6; }

static void reset_memory(int lifted){
    for (int i = 0; i < g_nrw; i++){
        uint8_t *dst = lifted ? g_mem + g_rw[i].lo : u_img + (g_rw[i].lo - g_img_lo);
        memcpy(dst, g_img0 + (g_rw[i].lo - g_img_lo), g_rw[i].hi - g_rw[i].lo);
    }
    memcpy(lifted ? g_mem + SCR : u_scr, g_in_scr, SCRSZ);
    memcpy(lifted ? g_mem + STK : u_stk, g_in_stk, STKSZ);
    memcpy(lifted ? g_mem + TEB : u_teb, g_in_teb, TEBSZ);
}

static CPU g_cpu;
static void run_lifted(const Fn *f, int ms){
    CPU *c = &g_cpu; Out *o = &g_lo;
    reset_memory(1);
    memset(c, 0, sizeof *c);
    memcpy(c->r, g_in.r, sizeof c->r);
    eflags_set(c, g_in.fl);
    c->fs_base = TEB; c->fcw = g_in.fcw; c->top = g_in.top;
    memcpy(c->st, g_in.st, sizeof c->st);
    for (int i = 0; i < 8; i++){ memcpy(c->xmm[i].b, g_in.xmm[i], 16); c->mm[i] = g_in.mm[i]; }
    g_left_at = g_fault_at = 0;
    struct itimerval it = { { 0, 0 }, { ms / 1000, (ms % 1000) * 1000 } }, off = { { 0, 0 }, { 0, 0 } };
    int r = sigsetjmp(g_jb, 1);
    if (r == 0){ g_phase = 1; setitimer(ITIMER_REAL, &it, 0); f->fn(c); setitimer(ITIMER_REAL, &off, 0); g_phase = 0;
                 o->how = c->eip == SENTINEL ? O_RET : O_JUMP; o->where = c->eip; }
    else { setitimer(ITIMER_REAL, &off, 0); g_phase = 0;
           o->how = r == 1 ? O_FAULT : r == 2 ? O_LEFT : r == 3 ? O_TRAP : O_LOOP;
           o->where = r == 1 ? g_fault_at : g_left_at; }
    memcpy(o->r, c->r, sizeof o->r);
    o->fl = eflags_get(c) & 0x0CD5u;
    o->fcw = c->fcw; o->top = c->top & 7;
    for (int i = 0; i < 8; i++){ f80_store(o->st[i], c->st[i]); memcpy(o->xmm[i], c->xmm[i].b, 16); o->mm[i] = c->mm[i]; }
    memcpy(g_l_scr, g_mem + SCR, SCRSZ); memcpy(g_l_stk, g_mem + STK, STKSZ); memcpy(g_l_teb, g_mem + TEB, TEBSZ);
    for (int i = 0, k = 0; i < g_nrw; k += (int)(g_rw[i].hi - g_rw[i].lo), i++)
        memcpy(g_l_img + k, g_mem + g_rw[i].lo, g_rw[i].hi - g_rw[i].lo);
    /* ORACLE_MUTATE: a wrong integer result, a lifted side that never ends */
    if (g_mutate == 1 && o->how == O_RET) o->r[0] ^= 1u;
    if (g_mutate == 2){ o->how = O_LOOP; o->where = 0; }
}

/* the lifted side's bytes [a, a+n) after its run (its copies of the scratch
 * area, the stack, the TEB and the writable image pages); 0 when they are
 * not all in one of them */
static int lifted_bytes(uint32_t a, uint8_t *out, uint32_t n){
    struct { uint32_t lo, hi; const uint8_t *p; } R[3 + 16]; int nr = 0;
    R[nr].lo = SCR; R[nr].hi = SCR + SCRSZ; R[nr++].p = g_l_scr;
    R[nr].lo = STK; R[nr].hi = STK + STKSZ; R[nr++].p = g_l_stk;
    R[nr].lo = TEB; R[nr].hi = TEB + TEBSZ; R[nr++].p = g_l_teb;
    for (int i = 0, k = 0; i < g_nrw; k += (int)(g_rw[i].hi - g_rw[i].lo), i++){
        R[nr].lo = g_rw[i].lo; R[nr].hi = g_rw[i].hi; R[nr++].p = g_l_img + k; }
    for (int i = 0; i < nr; i++)
        if (a >= R[i].lo && a < R[i].hi && R[i].hi - a >= n){ memcpy(out, R[i].p + (a - R[i].lo), n); return 1; }
    return 0;
}

/* long_s 0: at most g_budget instructions; else no count, long_s seconds */
static void run_unicorn(const Fn *f, int long_s){
    Out *o = &g_uo;
    reset_memory(0);
    static const int R[8] = { UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                              UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI };
    for (int i = 0; i < 8; i++) uc_reg_write(g_uc, R[i], &g_in.r[i]);
    uc_reg_write(g_uc, UC_X86_REG_EFLAGS, &g_in.fl);
    uint16_t cw = g_in.fcw; uc_reg_write(g_uc, UC_X86_REG_FPCW, &cw);
    uint16_t sw = (uint16_t)(g_in.top << 11); uc_reg_write(g_uc, UC_X86_REG_FPSW, &sw);
    uint16_t tag = 0; uc_reg_write(g_uc, UC_X86_REG_FPTAG, &tag);
    /* an MMX register is the significand of its x87 register (Unicorn 2.1 has no
     * MMX register access in 32-bit mode); MMX code gets an empty x87 stack */
    for (int i = 0; i < 8; i++){
        uint8_t b[10]; f80_store(b, g_in.st[i]);
        if (f->tr & T_MMX){ memcpy(b, &g_in.mm[i], 8); b[8] = b[9] = 0xFF; }
        uc_reg_write(g_uc, UC_X86_REG_FP0 + i, b);
    }
    for (int i = 0; i < 8; i++) uc_reg_write(g_uc, UC_X86_REG_XMM0 + i, g_in.xmm[i]);
    uint32_t mx = 0x1F80; uc_reg_write(g_uc, UC_X86_REG_MXCSR, &mx);
    g_u_ev = 0; g_u_addr = 0; g_u_intno = -1; g_trig_oor = 0; g_x87_range = 0; g_xs_hit = 0;
    /* until = the sentinel (with 0 there a jump to address 0 ended the run as if
     * it had used its budget): Unicorn stops before it fetches there, or the
     * unmapped fetch's hook does */
    g_phase = 2;
    uc_err e = uc_emu_start(g_uc, f->va, SENTINEL, long_s ? (uint64_t)long_s * 1000000u : 0, long_s ? 0 : g_budget);
    g_phase = 0;
    /* blocks translated without the instruction count would be reused by the
     * next counted run, which then never stops (tests/srvoracle.c): drop them */
    if (long_s) uc_ctl_flush_tb(g_uc);
    uint32_t eip = 0; uc_reg_read(g_uc, UC_X86_REG_EIP, &eip);
    if (getenv("ORACLE_DEBUG")) fprintf(stderr, "  unicorn %s: err %d (%s), eip %08x, event %d at %08x\n", long_s ? "long" : "counted",
                                        e, uc_strerror(e), eip, g_u_ev, g_u_addr);
    if (g_u_ev == 1 || (!g_u_ev && e == UC_ERR_OK && eip == SENTINEL)){ o->how = O_RET; o->where = SENTINEL; }
    else if (g_u_ev == 3){ o->how = O_JUMP; o->where = g_u_addr; }
    else if (g_u_ev == 2){ o->how = O_FAULT; o->where = g_u_addr; }
    else if (g_u_ev == 5){ o->how = g_u_intno == 0 ? O_DE : O_ERR; o->where = g_u_addr;
                           if (o->how == O_ERR) fprintf(stderr, "  unicorn: interrupt %d at 0x%08x\n", g_u_intno, g_u_addr); }
    else if (e == UC_ERR_OK){ o->how = O_LOOP; o->where = 0; }
    else { o->how = O_ERR; o->where = eip; fprintf(stderr, "  unicorn: %s at 0x%08x\n", uc_strerror(e), eip); }
    for (int i = 0; i < 8; i++) uc_reg_read(g_uc, R[i], &o->r[i]);
    uint32_t fl; uc_reg_read(g_uc, UC_X86_REG_EFLAGS, &fl); o->fl = fl & 0x0CD5u;
    uc_reg_read(g_uc, UC_X86_REG_FPCW, &cw); o->fcw = cw;
    uc_reg_read(g_uc, UC_X86_REG_FPSW, &sw); o->top = (sw >> 11) & 7; o->fsw = sw;
    for (int i = 0; i < 8; i++){ uc_reg_read(g_uc, UC_X86_REG_FP0 + i, o->st[i]); uc_reg_read(g_uc, UC_X86_REG_XMM0 + i, o->xmm[i]); }
    for (int i = 0; i < 8; i++) memcpy(&o->mm[i], o->st[i], 8);
    /* an interrupt (a divide error) leaves Unicorn's exception state behind: the
     * next one became a double fault (interrupt 8) and later runs stopped at
     * once, as if at their limit.  A fresh engine for the next run */
    if (g_u_ev == 5){
        uc_close(g_uc); g_ntrig_h = 0;
        if (uc_setup()){ fprintf(stderr, "mworacle: Unicorn could not be set up again\n"); exit(2); }
        trig_hooks(f);
    }
}

/* ---- comparing ------------------------------------------------------------- */
static int is_nan32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return (v & 0x7F800000u) == 0x7F800000u && (v & 0x7FFFFFu); }
static int is_nan64(const uint8_t *p){ uint64_t v; memcpy(&v, p, 8); return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0xFFFFFFFFFFFFFull); }
static int is_nan80(const uint8_t *p){ uint16_t se; uint64_t m; memcpy(&m, p, 8); memcpy(&se, p + 8, 2); return (se & 0x7FFF) == 0x7FFF && (m << 1); }
/* the default NaN, either sign: the host's quiet NaN, which the lift's
 * arithmetic makes from non-NaN operands, and the indefinite, which x87 and
 * SSE make (the same, negative) */
static int is_dnan32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return (v & 0x7FFFFFFFu) == 0x7FC00000u; }
static int is_dnan64(const uint8_t *p){ uint64_t v; memcpy(&v, p, 8); return (v & 0x7FFFFFFFFFFFFFFFull) == 0x7FF8000000000000ull; }
/* the first byte of [lo, lo+n) where a and b differ other than as two NaNs
 * (a float or a double of the same aligned slot, -1 = none); *nan counts NaN
 * pairs.  Only in a function with x87 or SSE code (fp), and only when one of
 * the two is the default NaN: a NaN that is only copied, or that an
 * operation passes on, keeps its payload on both sides.  skip: not the bytes
 * the original's x87 stores wrote (x87_stored, the range class) */
static long mem_diff(const uint8_t *a, const uint8_t *b, uint32_t n, uint32_t base, int fp, int skip, int *nan){
    for (uint32_t i = 0; i < n; ){
        if (!memcmp(a + i, b + i, n - i < 64 ? n - i : 64)){ i += 64; continue; }
        uint32_t e = i + 64 < n ? i + 64 : n;
        for (; i < e; i++){
            if (a[i] == b[i] || (skip && x87_stored(base + i))) continue;
            uint32_t a4 = i & ~3u, a8 = i & ~7u, g4 = (base + a4) & 3u, g8 = (base + a8) & 7u;
            if (fp && !g4 && a4 + 4 <= n && is_nan32(a + a4) && is_nan32(b + a4) && (is_dnan32(a + a4) || is_dnan32(b + a4))){
                (*nan)++;
                if (getenv("ORACLE_DEBUG")){ uint32_t x, y; memcpy(&x, a + a4, 4); memcpy(&y, b + a4, 4);
                    fprintf(stderr, "  NaN pair at %08x: lifted %08x, original %08x\n", base + a4, x, y); }
                i = a4 + 3; continue; }
            if (fp && !g8 && a8 + 8 <= n && is_nan64(a + a8) && is_nan64(b + a8) && (is_dnan64(a + a8) || is_dnan64(b + a8))){
                (*nan)++; i = a8 + 7; continue; }
            return (long)i;
        }
    }
    return -1;
}

typedef struct { long cases, same, fault, loop, de, nan, diff, jump, order, dead, trig, trig_oor, fsw, range, rback; } Count;
/* a divide error's instruction: div or idiv (F6 /6, F6 /7, F7 /6, F7 /7, after
 * operand-size and segment prefixes) */
static int is_div(uint32_t a){
    if (a < g_im.base || a + 8 > g_im.base + g_im.size) return 0;
    uint32_t p = a;
    while (p < a + 4 && (MR8(p) == 0x66 || MR8(p) == 0x2E || MR8(p) == 0x3E || MR8(p) == 0x26 ||
                         MR8(p) == 0x36 || MR8(p) == 0x64 || MR8(p) == 0x65)) p++;
    uint8_t op = MR8(p), reg = (uint8_t)(MR8(p + 1) >> 3 & 7);
    return (op == 0xF6 || op == 0xF7) && (reg == 6 || reg == 7);
}
static char g_why[256];
/* how: CMP_NOVALS leaves out the x87 stack values and the bytes the
 * original's x87 stores wrote (the range class), CMP_NOFSW takes no fnstsw
 * low byte as the same (fsw_rerun) */
enum { CMP_NOVALS = 1, CMP_NOFSW = 2 };
/* 0 same, 1 diff (g_why says how), 2 same with NaN-only differences, 3 both
 * faulted, at different addresses (fault_order decides), 4 the original
 * raised a divide error at a div/idiv, 5 same but for x87 exception flags in
 * an fnstsw's low byte (fsw_rerun decides) */
static int compare(const Fn *f, int how){
    const Out *l = &g_lo, *u = &g_uo;
    int fp = (f->tr & (T_X87 | T_SSE)) != 0;
    if (u->how == O_DE){
        if (is_div(u->where)) return 4;
        snprintf(g_why, sizeof g_why, "the original's divide error at %08x is no div/idiv", u->where); return 1;
    }
    if (l->how != u->how){ snprintf(g_why, sizeof g_why, "outcome: lifted %s at %08x, original %s at %08x",
                                    ONAME[l->how], l->where, ONAME[u->how], u->where); return 1; }
    if (l->how == O_FAULT || l->how == O_LEFT || l->how == O_TRAP || l->how == O_DE){
        /* an access across a page end: the host reports the first byte that is not
         * mapped, Unicorn the address of the access */
        if (l->how == O_FAULT && l->where > u->where && l->where - u->where < 16 && !(l->where & 0xFFFu)) return 0;
        if (l->where != u->where){ snprintf(g_why, sizeof g_why, "%s at %08x, original at %08x", ONAME[l->how], l->where, u->where);
                                   return l->how == O_FAULT ? 3 : 1; }
        return 0;
    }
    if (l->how == O_LOOP || l->how == O_ERR) return 0;
    /* returned (or jumped to the same address) */
    if (l->where != u->where){ snprintf(g_why, sizeof g_why, "eip %08x, original %08x", l->where, u->where); return 1; }
    static const char *const RN[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    int fsw = 0;
    for (int i = 0; i < 8; i++){
        if (l->r[i] == u->r[i]) continue;
        /* fnstsw's low byte: the lift keeps no x87 exception flags (cpu.h fsw_get:
         * condition codes and TOP), the original's sticky ones (0x6512A0: ZE after
         * a division by zero) are still set when it returns */
        uint32_t d = l->r[i] ^ u->r[i];
        if (!(how & CMP_NOFSW) && (f->tr & T_FSW) && !(d & ~0xFFu) && !(l->r[i] & d) && !(d & ~(uint32_t)(u->fsw & 0xFFu))){ fsw = 1; continue; }
        snprintf(g_why, sizeof g_why, "%s %08x, original %08x", RN[i], l->r[i], u->r[i]); return 1;
    }
    if ((l->fl ^ u->fl) & 0x400u){ snprintf(g_why, sizeof g_why, "DF %u, original %u", l->fl >> 10 & 1, u->fl >> 10 & 1); return 1; }
    if ((f->tr & T_RFL) && (l->fl ^ u->fl) & f->rfl){
        snprintf(g_why, sizeof g_why, "flags %03x, original %03x (compared %03x)", l->fl & 0x8D5u, u->fl & 0x8D5u, f->rfl); return 1; }
    int nan = 0;
    if (l->fcw != u->fcw){ snprintf(g_why, sizeof g_why, "x87 control word %04x, original %04x", l->fcw, u->fcw); return 1; }
    if (l->top != u->top){ snprintf(g_why, sizeof g_why, "x87 top %d, original %d", l->top, u->top); return 1; }
    if (!(f->tr & T_MMX)){
        int live = (8 - l->top) & 7;         /* values on the stack, counted from the empty stack (top 0) */
        if (live <= 4 && !(how & CMP_NOVALS))
            for (int k = 0; k < live; k++){
                int p = (l->top + k) & 7;
                if (!memcmp(l->st[p], u->st[p], 10)) continue;
                uint64_t lm, um; uint16_t le, ue;
                memcpy(&lm, l->st[p], 8); memcpy(&le, l->st[p] + 8, 2); memcpy(&um, u->st[p], 8); memcpy(&ue, u->st[p] + 8, 2);
                /* a NaN pair: the lifted value is a double, so the lifted one the default NaN (either sign: an
                 * invalid operation's indefinite, f80_load's host NaN), or the two the same in a double's bits */
                if (is_nan80(l->st[p]) && is_nan80(u->st[p]) &&
                    (lm == 0xC000000000000000ull || (!((le ^ ue) & 0x8000u) && lm >> 11 == um >> 11))){ nan++; continue; }
                snprintf(g_why, sizeof g_why, "ST(%d) %04x:%016llx, original %04x:%016llx", k, le, (unsigned long long)lm, ue, (unsigned long long)um);
                return 1;
            }
    } else
        for (int i = 0; i < 8; i++)
            if (l->mm[i] != u->mm[i]){ snprintf(g_why, sizeof g_why, "mm%d %016llx, original %016llx", i, (unsigned long long)l->mm[i], (unsigned long long)u->mm[i]); return 1; }
    for (int i = 0; i < 8; i++)
        if (memcmp(l->xmm[i], u->xmm[i], 16)){
            int n2 = 0;
            if (mem_diff(l->xmm[i], u->xmm[i], 16, 0, fp, 0, &n2) < 0){ nan += n2; continue; }
            uint64_t lq[2], uq[2]; memcpy(lq, l->xmm[i], 16); memcpy(uq, u->xmm[i], 16);
            snprintf(g_why, sizeof g_why, "xmm%d %016llx%016llx, original %016llx%016llx", i, (unsigned long long)lq[1],
                     (unsigned long long)lq[0], (unsigned long long)uq[1], (unsigned long long)uq[0]); return 1;
        }
    long at;
    struct { const char *name; const uint8_t *a, *b; uint32_t n, base; } M[3 + 16];
    int nm = 0;
    M[nm++] = (typeof(M[0])){ "scratch", g_l_scr, u_scr, SCRSZ, SCR };
    /* the stack from the returned esp up: below it is dead, and the lift inlines
     * _ftol2 (FTOL2INL), whose temporaries the original writes there */
    uint32_t sl = l->r[4] > STK && l->r[4] < STK + STKSZ ? l->r[4] - STK : 0;
    M[nm++] = (typeof(M[0])){ "stack", g_l_stk + sl, u_stk + sl, STKSZ - sl, STK + sl };
    M[nm++] = (typeof(M[0])){ "TEB", g_l_teb, u_teb, TEBSZ, TEB };
    for (int i = 0, k = 0; i < g_nrw; k += (int)(g_rw[i].hi - g_rw[i].lo), i++)
        M[nm++] = (typeof(M[0])){ "image", g_l_img + k, u_img + (g_rw[i].lo - g_img_lo), g_rw[i].hi - g_rw[i].lo, g_rw[i].lo };
    for (int m = 0; m < nm; m++)
        if ((at = mem_diff(M[m].a, M[m].b, M[m].n, M[m].base, fp, how & CMP_NOVALS, &nan)) >= 0){
            uint32_t ad = (M[m].base + (uint32_t)at) & ~3u, o4 = ad - M[m].base, lv, uv;
            memcpy(&lv, M[m].a + o4, 4); memcpy(&uv, M[m].b + o4, 4);
            snprintf(g_why, sizeof g_why, "%s [%08x] = %08x, original %08x", M[m].name, ad, lv, uv);
            return 1;
        }
    return fsw ? 5 : nan ? 2 : 0;
}

/* a hook on every instruction slows Unicorn down several times (a long re-run
 * of 0x497430 no longer finished in its 60 s), so the check runs only when an
 * x87 case differs: the original once more with it (and with the lift's
 * constants and formulas at its libm sites: an fldpi is no such value), and
 * notes where its x87 stores write (on_x87w) */
static int range_rerun(const Fn *f){
    uc_hook h, hw; Out keep = g_uo;
    g_nx87st = 0;
    uc_hook_add(g_uc, &h, UC_HOOK_CODE, (void *)on_x87, 0, 1, 0);
    uc_hook_add(g_uc, &hw, UC_HOOK_MEM_WRITE, (void *)on_x87w, 0, 1, 0);
    uc_ctl_flush_tb(g_uc);
    g_xs_sub = 1; run_unicorn(f, 0); g_xs_sub = 0;
    int hit = g_x87_range;
    if (g_u_ev != 5){ uc_hook_del(g_uc, h); uc_hook_del(g_uc, hw); }
    uc_ctl_flush_tb(g_uc);
    g_uo = keep;
    return hit;
}

/* The read-back class (range_subst): the nearest double to an 80-bit value
 * (round to nearest even; gradual underflow; an infinity past DBL_MAX), what
 * the lift's double arithmetic holds for a result x87 keeps wider */
static double f80_rne(const uint8_t b[10]){
    uint64_t m; uint16_t se; memcpy(&m, b, 8); memcpy(&se, b + 8, 2);
    int sg = se >> 15, e = se & 0x7FFF;
    if (e == 0x7FFF) return f80_load(b);
    if (!m) return sg ? -0.0 : 0.0;
    int E = (e ? e : 1) - 16383, lz = __builtin_clzll(m);   /* value = m * 2^(E-63) */
    m <<= lz; E -= lz;                                         /* bit 63 set: value in [2^E, 2^(E+1)) */
    double v;
    if (E > 1023) v = INFINITY;
    else if (E < -1075) v = 0.0;
    else {
        int keep = E >= -1022 ? 53 : E + 1075;                 /* significand bits a double keeps there */
        int sh = 64 - keep;                                    /* 11..64 */
        uint64_t q = sh == 64 ? 0 : m >> sh, rem = sh == 64 ? m : m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        if (rem > half || (rem == half && (q & 1))) q++;
        v = ldexp((double)q, E - 63 + sh);                     /* exact, or an infinity past DBL_MAX */
    }
    return sg ? -v : v;
}
/* an x87 instruction (D8-DF after prefixes), an fstp m80 (DB /7 to memory) */
static uint32_t skip_prefixes(uint32_t a){
    uint32_t p = a;
    while (p < a + 4 && (MR8(p) == 0x66 || MR8(p) == 0x67 || MR8(p) == 0x2E || MR8(p) == 0x3E ||
                         MR8(p) == 0x26 || MR8(p) == 0x36 || MR8(p) == 0x64 || MR8(p) == 0x65)) p++;
    return p;
}
static int in_image(uint32_t a){ return a >= g_im.base && a + 8 <= g_im.base + g_im.size; }
static int is_x87_op(uint32_t a){ if (!in_image(a)) return 0; uint8_t op = MR8(skip_prefixes(a)); return op >= 0xD8 && op <= 0xDF; }
static int is_fstp_m80(uint32_t a){
    if (!in_image(a)) return 0;
    uint32_t p = skip_prefixes(a);
    return MR8(p) == 0xDB && MR8(p + 1) < 0xC0 && (MR8(p + 1) >> 3 & 7) == 7;
}
static struct { uint32_t lo, hi; } g_sub[64]; static int g_nsub, g_nsub_all, g_sub_read, g_sub_lift;
static int g_sub_pend; static uint32_t g_sub_ip, g_sub_at; static uint8_t g_sub_v[10];
/* the store has been made: its bytes become the lift's for the value */
static void sub_apply(uc_engine *uc){
    uint8_t rne[10], ld[10], lb[10], *w = rne;
    f80_store(rne, f80_rne(g_sub_v)); f80_store(ld, f80_load(g_sub_v));
    if (lifted_bytes(g_sub_at, lb, 10) && (!memcmp(lb, rne, 10) || !memcmp(lb, ld, 10))){ w = lb; g_sub_lift++; }
    uc_mem_write(uc, g_sub_at, w, 10);
    if (g_nsub < (int)(sizeof g_sub / sizeof g_sub[0])){ g_sub[g_nsub].lo = g_sub_at; g_sub[g_nsub].hi = g_sub_at + 10u; g_nsub++; }
    g_nsub_all++;
    if (getenv("ORACLE_DEBUG")){
        uint64_t m; uint16_t se; memcpy(&m, g_sub_v, 8); memcpy(&se, g_sub_v + 8, 2);
        uint64_t wm; uint16_t wse; memcpy(&wm, w, 8); memcpy(&wse, w + 8, 2);
        fprintf(stderr, "  range_subst: fstp m80 at %08x to %08x: %04x:%016llx -> %04x:%016llx (%s)\n", g_sub_ip, g_sub_at,
                se, (unsigned long long)m, wse, (unsigned long long)wm, w == lb ? "the lifted side's" : "the nearest double");
    }
}
static void on_sub_code(uc_engine *uc, uint64_t a, uint32_t sz, void *u){
    (void)sz; (void)u;
    if (g_sub_pend == 2) sub_apply(uc);
    g_sub_pend = 0;
    g_x87_ip = (uint32_t)a;
    if (is_fstp_m80((uint32_t)a)){
        uint16_t sw; uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
        uc_reg_read(uc, UC_X86_REG_FP0 + (sw >> 11 & 7), g_sub_v);
        if (no_double(g_sub_v)){ g_sub_pend = 1; g_sub_ip = (uint32_t)a; }
    }
}
static void on_sub_w(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u){
    on_x87w(uc, t, a, sz, v, u);             /* the bytes this run's x87 stores write (CMP_NOVALS) */
    uint32_t lo = (uint32_t)a, hi = lo + (uint32_t)sz;
    if (g_sub_pend && g_x87_ip == g_sub_ip){ if (g_sub_pend == 1 || lo < g_sub_at) g_sub_at = lo; g_sub_pend = 2; return; }
    for (int i = 0; i < g_nsub; i++)         /* written over: no longer the substituted bytes */
        if (lo < g_sub[i].hi && hi > g_sub[i].lo) g_sub[i].lo = g_sub[i].hi = 0;
}
static void on_sub_r(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u){
    (void)uc; (void)t; (void)v; (void)u;
    if (is_x87_op(g_x87_ip)) return;
    uint32_t lo = (uint32_t)a, hi = lo + (uint32_t)sz;
    for (int i = 0; i < g_nsub; i++) if (lo < g_sub[i].hi && hi > g_sub[i].lo) g_sub_read = 1;
}
/* 1: the case is the same with the lift's bytes at the fstp m80 sites of
 * values no double holds (CMP_NOVALS, after a run where integer code read
 * such bytes); else 0 and g_why says what differs */
static int range_subst(const Fn *f){
    uc_hook hc, hw, hr; Out keep = g_uo;
    g_nx87st = 0; g_nsub = g_nsub_all = g_sub_read = g_sub_lift = 0; g_sub_pend = 0; g_sub_at = 0;
    uc_hook_add(g_uc, &hc, UC_HOOK_CODE, (void *)on_sub_code, 0, 1, 0);
    uc_hook_add(g_uc, &hw, UC_HOOK_MEM_WRITE, (void *)on_sub_w, 0, 1, 0);
    uc_hook_add(g_uc, &hr, UC_HOOK_MEM_READ, (void *)on_sub_r, 0, 1, 0);
    uc_ctl_flush_tb(g_uc);
    g_xs_sub = 1; run_unicorn(f, g_u_long); g_xs_sub = 0;
    if (g_u_ev != 5){ uc_hook_del(g_uc, hc); uc_hook_del(g_uc, hw); uc_hook_del(g_uc, hr); }
    uc_ctl_flush_tb(g_uc);
    int r = 1;
    if (!g_nsub_all) snprintf(g_why, sizeof g_why, "range_subst: no fstp m80 of a value no double holds");
    else if (!g_sub_read) snprintf(g_why, sizeof g_why, "range_subst: no integer read of the %d substituted fstp m80", g_nsub_all);
    else r = compare(f, CMP_NOVALS);
    if (getenv("ORACLE_DEBUG")) fprintf(stderr, "range_subst: %d stores (%d with the lifted side's bytes), integer reads %d: %s\n",
                                        g_nsub_all, g_sub_lift, g_sub_read, r == 0 || r == 2 ? "then the same" : g_why);
    g_uo = keep;
    return r == 0 || r == 2;
}

/* The fsw class: the lift keeps no x87 exception flags (cpu.h fsw_get), so
 * an fnstsw gives 0 there in the low byte where the original's sticky flags
 * show (0x6512A0: ZE after a division by zero).  The register rule in
 * compare() alone would also take any register that differs in those bits
 * as the same, so the original runs once more with its exception flags,
 * SF and ES cleared before every fnstsw (DF E0, DD /7 to memory), as the
 * lift has them, and that run must be the same without the rule (how |
 * CMP_NOFSW; xs: with the lift's libm formulas, as the run it replaces; with
 * the limit the case's original ran to, g_u_long).  Its compare's result;
 * g_why says what still differs */
static void on_fnstsw(uc_engine *uc, uint64_t a, uint32_t sz, void *u){
    (void)sz; (void)u;
    uint32_t p = (uint32_t)a;
    if (p < g_im.base || p + 8 > g_im.base + g_im.size) return;
    while (p < (uint32_t)a + 4 && (MR8(p) == 0x66 || MR8(p) == 0x67 || MR8(p) == 0x2E || MR8(p) == 0x3E ||
                                   MR8(p) == 0x26 || MR8(p) == 0x36 || MR8(p) == 0x64 || MR8(p) == 0x65)) p++;
    uint8_t op = MR8(p), mrm = MR8(p + 1);
    if ((op == 0xDF && mrm == 0xE0) || (op == 0xDD && (mrm >> 3 & 7) == 7 && mrm < 0xC0)){
        uint16_t sw; uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
        sw &= (uint16_t)~0xFFu; uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
    }
}
static int fsw_rerun(const Fn *f, int how, int xs){
    uc_hook h; Out keep = g_uo;
    uc_hook_add(g_uc, &h, UC_HOOK_CODE, (void *)on_fnstsw, 0, 1, 0);
    uc_ctl_flush_tb(g_uc);
    g_xs_sub = xs; run_unicorn(f, g_u_long); g_xs_sub = 0;
    if (g_u_ev != 5) uc_hook_del(g_uc, h);
    uc_ctl_flush_tb(g_uc);
    int r = compare(f, how | CMP_NOFSW);
    if (r == 4) snprintf(g_why, sizeof g_why, "the original with its x87 exception flags cleared: a divide error at %08x", g_uo.where);
    if (getenv("ORACLE_DEBUG")) fprintf(stderr, "fsw_rerun: %s\n", r == 0 || r == 2 ? "then the same" : g_why);
    g_uo = keep;
    return r;
}
/* compare(), with an fnstsw low byte taken as the same only when fsw_rerun
 * agrees (else 1) */
static int compare_x(const Fn *f, int how, int xs){
    int r = compare(f, how);
    if (r == 5){ int r2 = fsw_rerun(f, how, xs); if (r2 != 0 && r2 != 2) r = 1; }
    return r;
}

/* Both sides faulted, at different addresses.  The lifted C makes the
 * original's accesses in the original's order, but clang may reorder loads
 * that no store separates: 0x5B3FA0 reads [ecx+0x8C], [ecx+0x60], [ecx+0x88],
 * [ecx+0x90], and its -O3 code reads +0x90 first, so with ecx unmapped the
 * host faults there.  Accepted only when the original touches the lifted
 * side's address too: the case runs again under Unicorn with zero pages
 * where the original faults, and the lifted side's address must be read or
 * written (or fault) before the run ends; anything else is a difference. */
static int g_touch; static uint32_t g_touch_at;
static void on_touch(uc_engine *uc, uc_mem_type t, uint64_t a, int sz, int64_t v, void *u){
    (void)uc; (void)t; (void)v; (void)u;
    if (g_touch_at >= (uint32_t)a && g_touch_at < (uint32_t)a + (uint32_t)sz) g_touch = 1;
}
static int page_mapped(uint32_t pg){
    uc_mem_region *rg; uint32_t nr; int mapped = 0;
    if (uc_mem_regions(g_uc, &rg, &nr) == UC_ERR_OK){
        for (uint32_t i = 0; i < nr; i++) if (pg >= rg[i].begin && pg <= rg[i].end) mapped = 1;
        uc_free(rg);
    }
    return mapped;
}
static int fault_order(const Fn *f){
    uint32_t al = g_lo.where, pgs[4]; int np = 0, ok = 0;
    Out keep = g_uo;
    uc_hook h; g_touch = 0; g_touch_at = al;
    uc_hook_add(g_uc, &h, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, (void *)on_touch, 0, (al & ~15u) > 16 ? (al & ~15u) - 16 : 0, (al | 15u));
    /* a zero page wherever the original faults, up to four, until it touches
     * the lifted side's address (0x5B3FA0 with ecx 0x932DEF9A: +0x8C and +0x90
     * on one page, +0x60 on the page before) */
    while (np < 4 && g_uo.how == O_FAULT){
        uint32_t pg = g_uo.where & ~0xFFFu;
        if (page_mapped(pg)) break;          /* a protection fault: no second look */
        if (uc_mem_map(g_uc, pg, 0x1000, UC_PROT_READ | UC_PROT_WRITE)) break;
        pgs[np++] = pg;
        uc_ctl_flush_tb(g_uc);
        run_unicorn(f, 0);
        if (g_touch || (g_uo.how == O_FAULT && g_uo.where <= al && al - g_uo.where < 16)){ ok = 1; break; }
    }
    if (getenv("ORACLE_DEBUG")) fprintf(stderr, "fault_order: lifted %08x, %d zero pages: touched %d, last run %s at %08x\n",
                                        al, np, g_touch, ONAME[g_uo.how], g_uo.where);
    if (g_u_ev != 5) uc_hook_del(g_uc, h);
    for (int i = 0; i < np; i++) uc_mem_unmap(g_uc, pgs[i], 0x1000);
    uc_ctl_flush_tb(g_uc);
    g_uo = keep;
    return ok;
}

/* The original faulted, the lifted side did not.  clang drops a load whose
 * value is never used: 0x455AE0 reads edi = [arg3] for a search loop that
 * does not run when the list is empty, and returns; with arg3 = 0 the
 * original faults on that read, the -O3 lifted code never makes it.
 * Accepted only when the original, given readable zero pages where it
 * faulted (up to four), then ends exactly as the lifted side did: same
 * outcome, registers, x87/SSE state and memory (compare).  Or it raises a
 * divide error at a div/idiv (compare looks at nothing else then): 0x492130
 * divides its hash by [ecx+4], and with a hash of 0 (an empty string) the
 * lift's result is 0 whatever the divisor, so clang drops that load, where
 * the original reads a zero divisor from the zero page.  Then the original
 * runs once more with pages of 1s there (dwords of 1: a divisor that cannot
 * fault), and must end exactly as the lifted side did. */
static int dead_access(const Fn *f){
    Out keep = g_uo;
    uint32_t pgs[4]; int np = 0, ok = 0;
    while (np < 4 && g_uo.how == O_FAULT){
        uint32_t pg = g_uo.where & ~0xFFFu;
        if (page_mapped(pg) || uc_mem_map(g_uc, pg, 0x1000, UC_PROT_READ | UC_PROT_WRITE)) break;
        pgs[np++] = pg;
        uc_ctl_flush_tb(g_uc);
        run_unicorn(f, 0);
        int r = compare(f, 0);
        if (r == 0 || r == 2){ ok = 1; break; }
        if (r == 4){
            /* the divide error gave a fresh engine (run_unicorn): the pages again, of 1s */
            static uint32_t ones[0x400];
            if (!ones[0]) for (int i = 0; i < 0x400; i++) ones[i] = 1;
            int m = 1;
            for (int i = 0; i < np && m; i++)
                m = !uc_mem_map(g_uc, pgs[i], 0x1000, UC_PROT_READ | UC_PROT_WRITE) && !uc_mem_write(g_uc, pgs[i], ones, sizeof ones);
            if (m){ uc_ctl_flush_tb(g_uc); run_unicorn(f, 0); r = compare(f, 0); ok = r == 0 || r == 2; }
            break;
        }
        if (g_uo.how != O_FAULT) break;
    }
    if (getenv("ORACLE_DEBUG")) fprintf(stderr, "dead_access: %d zero pages, %s\n", np, ok ? "then the same" : "still different");
    for (int i = 0; i < np; i++) uc_mem_unmap(g_uc, pgs[i], 0x1000);
    uc_ctl_flush_tb(g_uc);
    g_uo = keep;
    return ok;
}

static uint32_t g_de[64]; static int g_nde;   /* the div/idiv instructions that raised a divide error */

/* The lifted side returned to an address that is not the sentinel (a return
 * address the case overwrote): a lifted ret ends the C function there, while
 * Unicorn runs on if that address is code.  The original runs again and
 * stops when it gets there, so the two are compared at the same point */
static uint32_t g_stop_at;
static void on_stop(uc_engine *uc, uint64_t a, uint32_t sz, void *u){
    (void)sz; (void)u;
    if (!g_u_ev && (uint32_t)a == g_stop_at){ g_u_ev = 3; g_u_addr = g_stop_at; uc_emu_stop(uc); }
}
static void run_unicorn_to(const Fn *f, uint32_t a){
    uc_hook h; g_stop_at = a;
    uc_hook_add(g_uc, &h, UC_HOOK_CODE, (void *)on_stop, 0, a, a);
    uc_ctl_flush_tb(g_uc);
    run_unicorn(f, 0);
    if (g_u_ev != 5) uc_hook_del(g_uc, h);   /* (an interrupt gave a fresh engine) */
    uc_ctl_flush_tb(g_uc);
}

static void show_case(const Fn *f, int k){
    printf("    case %d: fcw %04x, x87 top %d, eflags %03x, regs", k, g_in.fcw, g_in.top, g_in.fl);
    for (int i = 0; i < 8; i++) printf(" %08x", g_in.r[i]);
    printf("\n    args:");
    for (size_t i = 0; i < 16 && i < strlen(f->args) + 2; i++) printf(" %08x", ((uint32_t *)(g_in_stk + (ESPC - STK)))[1 + i]);
    printf("\n    lifted   %-5s %08x:", ONAME[g_lo.how], g_lo.where);
    for (int i = 0; i < 8; i++) printf(" %08x", g_lo.r[i]);
    printf(" fl %03x top %d\n    original %-5s %08x:", g_lo.fl, g_lo.top, ONAME[g_uo.how], g_uo.where);
    for (int i = 0; i < 8; i++) printf(" %08x", g_uo.r[i]);
    printf(" fl %03x top %d\n", g_uo.fl, g_uo.top);
}

int main(int argc, char **argv){
    setvbuf(stdout, 0, _IOLBF, 0);
    if (argc < 3){ fprintf(stderr, "usage: mworacle <sample.txt> <speed.exe> [cases]\n"); return 2; }
    int ncase = argc > 3 ? atoi(argv[3]) : 150;
    const char *e;
    if ((e = getenv("ORACLE_BUDGET"))) g_budget = (size_t)strtoull(e, 0, 0);
    if ((e = getenv("ORACLE_LIFT_MS"))) g_lift_ms = atoi(e);
    if ((e = getenv("ORACLE_LONG_S"))) g_long_s = atoi(e) < 0 ? 0 : atoi(e);
    if ((e = getenv("ORACLE_LONG_ORIG_S"))) g_long_orig_s = atoi(e) < 0 ? 0 : atoi(e);
    if ((e = getenv("ORACLE_MUTATE"))){
        g_mutate = !strcmp(e, "eax") ? 1 : !strcmp(e, "loop") ? 2 : -1;
        if (g_mutate < 0){ fprintf(stderr, "ORACLE_MUTATE=%s: eax or loop\n", e); return 2; }
        fprintf(stderr, "mworacle: ORACLE_MUTATE=%s (a test of the compare rules: every lifted %s)\n", e,
                g_mutate == 1 ? "return gets eax ^ 1" : "run ends at its limit");
    }
    g_keep_snan = getenv("ORACLE_SNAN") && atoi(getenv("ORACLE_SNAN"));
    const char *only = getenv("ORACLE_FNS");
    int one = getenv("ORACLE_CASE") ? atoi(getenv("ORACLE_CASE")) : -1;

    size_t n; uint8_t *exe = mwimage_read(argv[2], &n);
    if (!exe || mwimage_parse(exe, n, 0, 0, &g_im)) return 2;
    mem_init();
    g_img_lo = g_im.base & ~(HPG - 1); g_img_hi = (g_im.base + g_im.size + HPG - 1) & ~(HPG - 1);
    mem_commit(g_img_lo, g_img_hi - g_img_lo);
    if (mwimage_parse(exe, n, 1, g_mem, &g_im)) return 2;
    free(exe);
    /* the zero-initialised part of every writable section (.data's bss) gets
     * random dwords like the scratch area's, once (a pointer read from a global
     * leads somewhere); the file's own bytes stay as they are */
    g_rs = 0x5EED0B55ull;
    for (uint32_t i = 0; i < g_im.nsec; i++){
        const MwSec *sc = &g_im.sec[i];
        if (!(sc->chr & 0x80000000u) || sc->raw >= sc->vsize) continue;
        for (uint32_t a = (sc->va + sc->raw + 3u) & ~3u; a + 4 <= sc->va + sc->vsize; a += 4) MR32(a) = rnd_dword();
    }
    g_img0 = malloc(g_img_hi - g_img_lo); memcpy(g_img0, g_mem + g_img_lo, g_img_hi - g_img_lo);
    protect_image();
    { size_t t = 0; for (int i = 0; i < g_nrw; i++) t += g_rw[i].hi - g_rw[i].lo; g_l_img = malloc(t); }
    mem_commit(SCR, SCRSZ); mem_commit(STK, STKSZ); mem_commit(TEB, TEBSZ);
    for (uint32_t i = 0; i < g_fn_count; i++){
        uint32_t o = g_fn_addr[i] - DISPATCH_TEXT_BASE;
        if (o < DISPATCH_TEXT_SIZE) g_dtab[o] = g_fn_ptr[i];
    }
    if (load_sample(argv[1]) || uc_setup()) return 2;
    /* the handlers run on their own stack: a lifted recursion may end in the host stack's guard */
    static uint8_t altstk[1 << 16];
    stack_t ss = { .ss_sp = altstk, .ss_size = sizeof altstk, .ss_flags = 0 }; sigaltstack(&ss, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    signal(SIGALRM, on_alarm);

    printf("Most Wanted: lifted C (gen/mw/all) against the original x86 (Unicorn), %d functions, %d cases each\n", g_nfns, ncase);
    printf("  limits: %zu Unicorn instructions, %d ms lifted, re-runs %d s, the original's when the lifted side ended %d s a function\n",
           g_budget, g_lift_ms, g_long_s, g_long_orig_s);
    Count tot = { 0 }; int nfn = 0, reruns = 0, fn_ret = 0, fn_ret10 = 0;
    long fault_fn = 0;
    for (int fi = 0; fi < g_nfns; fi++){
        const Fn *f = &g_fns[fi];
        if (only){ char h[16]; snprintf(h, sizeof h, "%x", f->va); if (!strstr(only, h)) continue; }
        Count c = { 0 }; int shown = 0, rerun1 = g_long_s > 0, rerun2 = rerun1;   /* re-runs of the lifted side, of both */
        double orig_left = g_long_orig_s;    /* the original-loop re-runs' budget (s) */
        trig_hooks(f);
        g_rs = 0x9E3779B97F4A7C15ull ^ ((uint64_t)f->va << 20);
        for (int k = 0; k < ncase; k++){
            make_case(f);
            if (one >= 0 && k != one) continue;
            run_unicorn(f, 0);
            run_lifted(f, g_lift_ms);
            /* a side at its limit runs again with a long one: the lifted side with g_long_s, and when the
             * original was at its limit too, the original: with g_long_s while the lifted side still stops,
             * else with what is left of the function's g_long_orig_s (g_long_s once that is spent).  The
             * both-sides budget goes as soon as a side would still stop after g_long_s, as before the
             * original-loop re-run (it then took more than g_long_s).  An original at its limit where the
             * lifted side ended in its first run gets what is left of g_long_orig_s (g_long_s under the
             * one-side budget once that is spent).  A re-run after which one side still stops and the other
             * has ended is a DIFF (compare) */
            int ul = g_uo.how == O_LOOP, ll = g_lo.how == O_LOOP;
            g_u_long = 0;
            if (ll && (ul ? rerun2 : rerun1) && g_uo.how != O_DE){
                double t0 = now_s();
                run_lifted(f, g_long_s * 1000);
                double tl = now_s() - t0;
                reruns++;
                fprintf(stderr, "  re-run %08x case %d, %s with %d s: lifted %s after %.1f s\n", f->va, k,
                        ul ? "both" : "the lifted side", g_long_s, ONAME[g_lo.how], tl);
                if (!ul){ if (g_lo.how == O_LOOP) rerun1 = 0; }
                else {
                    int budget = g_lo.how != O_LOOP && orig_left >= 1.0, lim = budget ? (int)orig_left : g_long_s;
                    t0 = now_s(); run_unicorn(f, lim); double tu = now_s() - t0;
                    g_u_long = lim;
                    if (budget) orig_left -= tu;
                    if (g_lo.how == O_LOOP || g_uo.how == O_LOOP || tu > g_long_s) rerun2 = 0;
                    fprintf(stderr, "  re-run %08x case %d, the original with %d s%s: original %s after %.1f s\n", f->va, k, lim,
                            budget ? " (the lifted side ended)" : "", ONAME[g_uo.how], tu);
                }
            } else if (ul && !ll){
                int budget = orig_left >= 1.0, lim = budget ? (int)orig_left : rerun1 ? g_long_s : 0;
                if (lim){
                    double t0 = now_s(); run_unicorn(f, lim); double tu = now_s() - t0;
                    g_u_long = lim; reruns++;
                    if (budget) orig_left -= tu; else if (g_uo.how == O_LOOP) rerun1 = 0;
                    fprintf(stderr, "  re-run %08x case %d, the original with %d s%s (the lifted side: %s): original %s after %.1f s\n",
                            f->va, k, lim, budget ? "" : " (its budget spent)", ONAME[g_lo.how], ONAME[g_uo.how], tu);
                }
            }
            if (g_lo.how == O_JUMP && !(g_uo.how == O_JUMP && g_uo.where == g_lo.where)) run_unicorn_to(f, g_lo.where);
            int r = compare_x(f, 0, 0);
            c.cases++;
            if (r == 1 && g_xs_hit){
                /* again with the lift's formulas at the libm-lifted x87 instructions */
                char why[sizeof g_why]; memcpy(why, g_why, sizeof why);
                Out keep = g_uo; int oor = g_trig_oor;
                g_xs_sub = 1; run_unicorn(f, 0); g_xs_sub = 0;
                int r2 = compare_x(f, 0, 1);
                g_uo = keep;
                if (r2 == 0 || r2 == 2 || r2 == 5){ c.trig++; c.trig_oor += oor; continue; }
                memcpy(g_why, why, sizeof why);
            }
            /* a value no double holds explains the x87 stack values and what was stored from them, nothing
             * else: the rest must be the same (else g_why says what differs there); and integer code's reads
             * of an fstp m80 of such a value when the original then, given the lift's bytes there, ends the
             * same (range_subst) */
            if (r == 1 && (f->tr & T_X87) && range_rerun(f)){
                if (compare_x(f, CMP_NOVALS, 0) != 1){ c.range++; continue; }
                char why[sizeof g_why]; memcpy(why, g_why, sizeof why);
                if (range_subst(f)){ c.rback++; continue; }
                memcpy(g_why, why, sizeof why);
            }
            if (r == 5){ c.fsw++; continue; }
            if (r == 4){
                c.de++;
                int q = 0; while (q < g_nde && g_de[q] != g_uo.where) q++;
                if (q == g_nde && g_nde < 64) g_de[g_nde++] = g_uo.where;
                continue;
            }
            if (r == 3){ if (fault_order(f)){ c.order++; continue; } r = 1; }
            else if (r == 1 && g_uo.how == O_FAULT && g_lo.how != O_FAULT){
                char why[sizeof g_why]; memcpy(why, g_why, sizeof why);
                if (dead_access(f)){ c.dead++; continue; }
                memcpy(g_why, why, sizeof why);
            }
            if (r == 1){
                c.diff++;
                if (shown++ < 3 || one >= 0){ printf("  DIFF %08x case %d: %s\n", f->va, k, g_why); show_case(f, k); }
            } else if (r == 2) c.nan++;
            else if (g_uo.how == O_RET) c.same++;
            else if (g_uo.how == O_JUMP) c.jump++;
            else if (g_uo.how == O_FAULT) c.fault++;
            else if (g_uo.how == O_DE) c.de++;
            else if (g_uo.how == O_LOOP) c.loop++;
            else c.diff++;
            if (one >= 0 && r != 1){ printf("  %08x case %d: same (%s)\n", f->va, k, ONAME[g_uo.how]); show_case(f, k); }
        }
        nfn++;
        tot.cases += c.cases; tot.same += c.same; tot.fault += c.fault; tot.loop += c.loop; tot.de += c.de;
        tot.nan += c.nan; tot.diff += c.diff; tot.jump += c.jump; tot.order += c.order; tot.dead += c.dead; tot.trig += c.trig;
        tot.fsw += c.fsw; tot.range += c.range; tot.rback += c.rback; tot.trig_oor += c.trig_oor;
        if (c.same + c.nan) fn_ret++;
        if (c.same + c.nan >= 10) fn_ret10++;
        if (!(c.same + c.nan + c.jump)) fault_fn++;
        if (c.diff || getenv("ORACLE_VERBOSE"))
            printf("  %08x %2d %-8s %-7s cases %4ld: same %4ld, fault %4ld (order %ld, dead %ld), jump %3ld, loop %3ld, #DE %3ld, libm %ld, range %ld, read back %ld, fsw %ld, nan %3ld, DIFF %ld\n",
                   f->va, f->nfn, f->args[0] ? f->args : "-", f->regs, c.cases, c.same, c.fault, c.order, c.dead, c.jump, c.loop, c.de, c.trig, c.range, c.rback, c.fsw, c.nan, c.diff);
    }
    printf("functions %d, cases %ld: returned the same %ld, faulted at the same address %ld, faulted in another order %ld, "
           "faulted on a load the lift drops %ld, jumped to the same address %ld, loop %ld, #DE %ld, "
           "libm x87 formulas %ld (%ld with a sine/cosine/tangent operand >= 2^63), "
           "x87 values no double holds %ld, x87 values read back as integers %ld, x87 exception flags %ld, NaN-only %ld, DIFF %ld\n",
           nfn, tot.cases, tot.same, tot.fault, tot.order, tot.dead, tot.jump, tot.loop, tot.de, tot.trig, tot.trig_oor,
           tot.range, tot.rback, tot.fsw, tot.nan, tot.diff);
    printf("functions with a returning case: %d (with 10 or more: %d); with none (every case faulted or looped): %ld; re-runs: %d\n",
           fn_ret, fn_ret10, fault_fn, reruns);
    if (g_nde){ printf("divide errors at %d div/idiv instruction%s:", g_nde, g_nde > 1 ? "s" : ""); for (int q = 0; q < g_nde; q++) printf(" %08x", g_de[q]); printf("\n"); }
    printf("result: %s (%ld differing cases)\n", tot.diff ? "FAIL" : "PASS", tot.diff);
    uc_close(g_uc);
    return tot.diff ? 1 : 0;
}
