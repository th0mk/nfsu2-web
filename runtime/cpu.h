/* NFSU2 native port - x86-32 CPU state and guest memory access (macOS ARM64) */
#ifndef NFSU2_CPU_H
#define NFSU2_CPU_H
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Guest memory base. Guest 32-bit address -> host address in one ARM64 instruction:
 *   ldr w0, [x_base, w_addr, uxtw]        (zero extra cost)              */
#ifdef __wasm__
/* wasm: guest address == linear memory address.  The guest owns
 * [0, GUEST_END); the runtime's data, heap and stacks live above it
 * (wasm-ld --global-base=GUEST_END).  Built with
 * -fno-delete-null-pointer-checks: address 0 is valid wasm memory, so
 * g_mem + a is plain integer arithmetic and every MR* is one load/store with
 * the guest displacement in the offset immediate. */
#define g_mem ((uint8_t *)0)
#define GUEST_END 0x28000000u
#define GSZ ((uint64_t)GUEST_END)
#else
extern uint8_t *g_mem;
/* The whole 32-bit guest address space is reserved as PROT_NONE; the regions
 * in use are made RW with mem_commit. A bad guest access then becomes a
 * diagnosable signal instead of crashing the host.                       */
#define GSZ 0x100000000ull
#endif

#ifdef __wasm__
/* Guest memory is untyped: the game stores a float with fstp and reads the
 * same bytes back with mov (every by-value vector copy does), so MRF32 and
 * MR32 of one address must never be assumed apart.  Plain float/uint32_t
 * lvalues let clang's type-based alias analysis do exactly that: clang 23's
 * wasm backend (DAGCombiner with IR alias analysis) moved the i32 load of
 * "fstp [esp+8]; mov edx,[esp+8]" above the f32 store in the vector
 * normalize at 0x4870B0, so the vectors it returned had a stale z and the
 * car-light and street-lamp glows were drawn far above the scene.
 * may_alias gives the guest accesses the
 * character type's alias set: they may alias one another whatever their
 * type.  The Mac build keeps the plain types (same C, same latent hazard;
 * Apple clang's arm64 code for 0x4870B0 keeps the order). */
typedef uint16_t __attribute__((may_alias)) gm_u16;
typedef uint32_t __attribute__((may_alias)) gm_u32;
typedef uint64_t __attribute__((may_alias)) gm_u64;
typedef float    __attribute__((may_alias)) gm_f32;
typedef double   __attribute__((may_alias)) gm_f64;
#endif

#if defined(__wasm__) && defined(NFSU2_GUEST_OBJ)
/* Lifted code (web/build.sh compiles the shards with NFSU2_GUEST_OBJ) reaches
 * guest memory through g_guest: an undefined weak data symbol, which wasm-ld
 * places at address 0, so g_guest + a is the same address as g_mem + a.  The
 * difference is for clang's alias analysis.  g_mem + a is an inttoptr, which
 * may point anywhere, the restrict CPU struct included: every guest store
 * forced a reload of every register the function used next (c->esp after each
 * push) and every register write was stored at once.  g_guest is an object of
 * its own, and a global never aliases a restrict argument, so the registers
 * stay in wasm locals across guest loads and stores until the next call.
 * True because the CPU structs (and all of the runtime's data) live above
 * GUEST_END, where no guest access goes.  The runtime keeps g_mem: its own
 * accesses may well mix guest and host pointers.
 * One hazard: wasm-ld resolves g_guest + K in data to 0, not K (an undefined
 * weak symbol loses its addend there).  Code keeps g_guest and K apart, data
 * does not: clang can turn a switch whose arms load from constant guest
 * addresses into a lookup table of g_guest + K.  sub_0050e2e0 (the first
 * screen of a career shop) was one: every entry read guest address 0, the
 * screen name was NULL and entering any upgrade shop trapped.  web/build.sh
 * compiles a shard that has such a table without jump tables. */
extern __attribute__((weak)) uint8_t g_guest[];
#define GMEM_ g_guest
#else
#define GMEM_ g_mem
#endif

#if defined(__wasm__) && defined(NFSU2_MEMCHECK)
/* debug build (web/build.sh MEMCHECK=shard_0025,...): every guest access is
 * checked against the guest span and reported with the lifted function */
uint8_t *guest_memchk(uint32_t a, unsigned n, const char *fn);
#define MR8(a)   (*(uint8_t *)guest_memchk((uint32_t)(a), 1, __func__))
#define MR16(a)  (*(gm_u16  *)guest_memchk((uint32_t)(a), 2, __func__))
#define MR32(a)  (*(gm_u32  *)guest_memchk((uint32_t)(a), 4, __func__))
#define MR64(a)  (*(gm_u64  *)guest_memchk((uint32_t)(a), 8, __func__))
#define MRF32(a) (*(gm_f32  *)guest_memchk((uint32_t)(a), 4, __func__))
#define MRF64(a) (*(gm_f64  *)guest_memchk((uint32_t)(a), 8, __func__))
#elif defined(__wasm__)
#define MR8(a)   (*(uint8_t *)(GMEM_ + (uint32_t)(a)))
#define MR16(a)  (*(gm_u16  *)(GMEM_ + (uint32_t)(a)))
#define MR32(a)  (*(gm_u32  *)(GMEM_ + (uint32_t)(a)))
#define MR64(a)  (*(gm_u64  *)(GMEM_ + (uint32_t)(a)))
#define MRF32(a) (*(gm_f32  *)(GMEM_ + (uint32_t)(a)))
#define MRF64(a) (*(gm_f64  *)(GMEM_ + (uint32_t)(a)))
#else
#define MR8(a)   (*(uint8_t  *)(g_mem + (uint32_t)(a)))
#define MR16(a)  (*(uint16_t *)(g_mem + (uint32_t)(a)))
#define MR32(a)  (*(uint32_t *)(g_mem + (uint32_t)(a)))
#define MR64(a)  (*(uint64_t *)(g_mem + (uint32_t)(a)))
#define MRF32(a) (*(float    *)(g_mem + (uint32_t)(a)))
#define MRF64(a) (*(double   *)(g_mem + (uint32_t)(a)))
#endif

enum { R_EAX=0,R_ECX,R_EDX,R_EBX,R_ESP,R_EBP,R_ESI,R_EDI };

/* An indirect or out-of-range jmp at the end of a lifted function: control
 * goes to the target and never comes back here.  On wasm it has to be a real
 * tail call (return_call), otherwise every pass through a lifted loop that
 * leaves by a jump table adds frames until the ~1 MB engine stack overflows.
 * The target travels in eip so
 * dispatch_tail has the lifted functions' own signature.  The Mac keeps the
 * plain call (clang turns it into a tail call on arm64 anyway). */
struct CPU;
void dispatch_indirect(struct CPU *c, uint32_t addr);
void dispatch_tail(struct CPU *c);
/* The dispatch table (dispatch.c): .text address -> lifted function, or the
 * run-time patch that replaces it; NULL for anything else.  An indirect call
 * or jump in lifted code (DISPATCH_CALL, DISPATCH_TAIL) that finds its target
 * there calls it directly: one load and one call_indirect instead of a call
 * into the dispatcher.  Everything else (COM, Win32, server.dll, generated
 * code, misses, the addresses ICALL/EFXSTAT watch) takes dispatch_indirect /
 * dispatch_tail as before.  The exe's .text comes from the lift's own
 * gameinfo.h (tools/genall.py writes it into gen/<game>/all, which the build
 * puts on the include path).  Without one (a UG2 lift made before
 * genall.py wrote it, a test build without a lift) the values are UG2's,
 * runtime/gameinfo_ug2.h; web/tools/gencheck.sh refuses another game's lift
 * that has none. */
#if __has_include("gameinfo.h")
#include "gameinfo.h"
#else
#include "gameinfo_ug2.h"
#endif
#define DISPATCH_TEXT_BASE GAMEINFO_TEXT_BASE
#define DISPATCH_TEXT_SIZE GAMEINFO_TEXT_SIZE
typedef void (*guest_fn_t)(struct CPU *);
extern guest_fn_t g_dtab[DISPATCH_TEXT_SIZE];
static inline guest_fn_t dispatch_fast(uint32_t a){
    uint32_t o = a - DISPATCH_TEXT_BASE;
    return o < DISPATCH_TEXT_SIZE ? g_dtab[o] : (guest_fn_t)0;
}
#define DISPATCH_CALL(c_, t_) do { uint32_t _dc = (uint32_t)(t_); guest_fn_t _df = dispatch_fast(_dc); \
    if (_df) _df(c_); else dispatch_indirect((c_), _dc); } while (0)
#ifdef __wasm__
#define DISPATCH_TAIL(c_, t_) { uint32_t _dc = (uint32_t)(t_); guest_fn_t _df = dispatch_fast(_dc); \
    if (_df) { __attribute__((musttail)) return _df(c_); } \
    (c_)->eip = _dc; __attribute__((musttail)) return dispatch_tail(c_); }
#else
#define DISPATCH_TAIL(c_, t_) { dispatch_indirect((c_), (uint32_t)(t_)); return; }
#endif

typedef struct CPU {
    union {
        uint32_t r[8];
        struct { uint32_t eax,ecx,edx,ebx,esp,ebp,esi,edi; };
    };
    uint32_t eip;
    uint32_t fs_base, gs_base;   /* TEB / TLS base (flat model) */
    /* flags - each kept as 0/1 (clang DCE drops the unused ones) */
    uint32_t cf,pf,af,zf,sf,of,df;
    /* x87 stack */
    double   st[8];
    int32_t  top;
    uint16_t fcw, fsw;
    /* MMX (not shared with x87, kept separately) */
    uint64_t mm[8];
    /* SSE */
    union { uint8_t b[16]; float f[4]; double d[2]; uint32_t u32[4]; uint64_t u64[2]; } xmm[8];
    /* EFLAGS system bits (preserved by pushfd/popfd): ID (21), AC (18).
     * Without them the game's "is CPUID there" test (it tries to flip the
     * ID bit, 0x5CDCF5 / 0x5C0B09) always failed, the CPU MHz was never
     * measured and the first run picked performance level 0. */
    uint32_t sysfl;
} CPU;

/* ---- registers and flags in locals (tools/lregs.py, LREGS) ------------
 * A shard lifted with LREGS=1 defines NFSU2_LREGS: its functions keep the
 * general registers and the six arithmetic flags in locals (_r_eax ...
 * _r_of) and store them to the CPU struct only around calls, returns, tail
 * jumps and the helpers that take the CPU (the stores are written out by
 * tools/lregs.py, which knows what changed).  RG_() names them in the macros
 * below; everywhere else they are the struct's fields.  LR_FG / LR_FF reload
 * the registers / flags from the struct after a call. */
#ifdef NFSU2_LREGS
#define RG_(x) _r_##x
#else
#define RG_(x) c->x
#endif
#define LR_ENTER uint32_t _r_eax = c->eax, _r_ecx = c->ecx, _r_edx = c->edx, _r_ebx = c->ebx, \
    _r_esp = c->esp, _r_ebp = c->ebp, _r_esi = c->esi, _r_edi = c->edi, \
    _r_cf = c->cf, _r_pf = c->pf, _r_af = c->af, _r_zf = c->zf, _r_sf = c->sf, _r_of = c->of
#define LR_FG do{ _r_eax = c->eax; _r_ecx = c->ecx; _r_edx = c->edx; _r_ebx = c->ebx; \
    _r_esp = c->esp; _r_ebp = c->ebp; _r_esi = c->esi; _r_edi = c->edi; }while(0)
#define LR_FF do{ _r_cf = c->cf; _r_pf = c->pf; _r_af = c->af; _r_zf = c->zf; _r_sf = c->sf; _r_of = c->of; }while(0)
/* pushfd / popfd on the locals (eflags_get / eflags_set below) */
#define LR_EFLAGS_GET() (0x00000202u | (_r_cf&1u) | ((_r_pf&1u)<<2) | ((_r_af&1u)<<4) \
    | ((_r_zf&1u)<<6) | ((_r_sf&1u)<<7) | ((c->df&1u)<<10) | ((_r_of&1u)<<11) | (c->sysfl & 0x00240000u))
#define LR_EFLAGS_SET(f) do{ uint32_t _ef = (f); _r_cf = _ef&1u; _r_pf = (_ef>>2)&1u; _r_af = (_ef>>4)&1u; \
    _r_zf = (_ef>>6)&1u; _r_sf = (_ef>>7)&1u; c->df = (_ef>>10)&1u; _r_of = (_ef>>11)&1u; \
    c->sysfl = _ef & 0x00240000u; }while(0)

/* ---- flag helpers ----------------------------------------------------- */
static inline uint32_t parity8(uint32_t v){ v&=0xff; v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }

/* ---- flag masks --------------------------------------------------------
 * Every arithmetic instruction computed all six flags EAGERLY (ZF/SF/PF/CF/OF/AF),
 * but the following branch usually reads only one.  Measured: the generated
 * code has 110k FLAGS_ADD + 130k FLAGS_SUB + 130k FLAGS_LOG, while OF is
 * read directly in only 22 places (CC_O/CC_NO) and PF in 5.7k.
 * The generator (tools/lift.py) now solves flag LIVENESS over the control
 * flow graph and passes each call a "flags read later" mask.  The mask is a
 * compile-time constant, so the unused branches are removed entirely.     */
#define F_CF  0x01u
#define F_PF  0x02u
#define F_AF  0x04u
#define F_ZF  0x08u
#define F_SF  0x10u
#define F_OF  0x20u
#define F_ALL 0x3Fu

#define SETZSP_M(res,bits,M) do{ \
    if((M)&F_ZF) RG_(zf) = ((uint##bits##_t)(res)==0); \
    if((M)&F_SF) RG_(sf) = (((uint##bits##_t)(res))>>((bits)-1))&1; \
    if((M)&F_PF) RG_(pf) = parity8((uint32_t)(res)); }while(0)
#define SETZSP(res,bits) SETZSP_M(res,bits,F_ALL)

/* ADD */
#define FLAGS_ADD_M(a,b,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) RG_(cf) = ((uint##bits##_t)(res) < (uint##bits##_t)(a)); \
    if((M)&F_AF) RG_(af) = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) RG_(of) = ((~((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_ADD(a,b,res,bits) FLAGS_ADD_M(a,b,res,bits,F_ALL)
/* SUB / CMP */
#define FLAGS_SUB_M(a,b,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) RG_(cf) = ((uint##bits##_t)(a) < (uint##bits##_t)(b)); \
    if((M)&F_AF) RG_(af) = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) RG_(of) = ((((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_SUB(a,b,res,bits) FLAGS_SUB_M(a,b,res,bits,F_ALL)
/* ADC/SBB: with carry-in 1 the result can EQUAL the input; the (res<a)
 * test then missed the carry (example: sbb eax,eax  CF=1 -> CF=1).       */
#define FLAGS_ADC_M(a,b,cin,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) RG_(cf) = (cin) ? ((uint##bits##_t)(res) <= (uint##bits##_t)(a)) \
                               : ((uint##bits##_t)(res) <  (uint##bits##_t)(a)); \
    if((M)&F_AF) RG_(af) = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) RG_(of) = ((~((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_ADC(a,b,cin,res,bits) FLAGS_ADC_M(a,b,cin,res,bits,F_ALL)
#define FLAGS_SBB_M(a,b,cin,res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) RG_(cf) = (cin) ? ((uint##bits##_t)(a) <= (uint##bits##_t)(b)) \
                               : ((uint##bits##_t)(a) <  (uint##bits##_t)(b)); \
    if((M)&F_AF) RG_(af) = ((((a)^(b)^(res))>>4)&1); \
    if((M)&F_OF) RG_(of) = ((((a)^(b)) & ((a)^(res)))>>((bits)-1))&1; }while(0)
#define FLAGS_SBB(a,b,cin,res,bits) FLAGS_SBB_M(a,b,cin,res,bits,F_ALL)
/* logical: CF=OF=0 */
#define FLAGS_LOG_M(res,bits,M) do{ SETZSP_M(res,bits,M); \
    if((M)&F_CF) RG_(cf)=0; if((M)&F_OF) RG_(of)=0; if((M)&F_AF) RG_(af)=0; }while(0)
#define FLAGS_LOG(res,bits) FLAGS_LOG_M(res,bits,F_ALL)

/* ---- stack ------------------------------------------------------------ */
/* CRITICAL: the operand may depend on esp (example: push [esp+8], push esp).
 * x86 semantics: PUSH reads the value first, then decrements esp.        */
#define PUSH32(v) do{ uint32_t _pv = (uint32_t)(v); RG_(esp) -= 4; MR32(RG_(esp)) = _pv; }while(0)
/* POP: esp is incremented first; the destination address uses the NEW esp (pop [esp+N]). */
#define POP32(d)  do{ uint32_t _qv = MR32(RG_(esp)); RG_(esp) += 4; (d) = _qv; }while(0)

/* ---- conditions ------------------------------------------------------- */
#define CC_O   (RG_(of))
#define CC_NO  (!RG_(of))
#define CC_B   (RG_(cf))
#define CC_AE  (!RG_(cf))
#define CC_E   (RG_(zf))
#define CC_NE  (!RG_(zf))
#define CC_BE  (RG_(cf)||RG_(zf))
#define CC_A   (!RG_(cf)&&!RG_(zf))
#define CC_S   (RG_(sf))
#define CC_NS  (!RG_(sf))
#define CC_P   (RG_(pf))
#define CC_NP  (!RG_(pf))
#define CC_L   (RG_(sf)!=RG_(of))
#define CC_GE  (RG_(sf)==RG_(of))
#define CC_LE  (RG_(zf)||(RG_(sf)!=RG_(of)))
#define CC_G   (!RG_(zf)&&(RG_(sf)==RG_(of)))


/* ---- x87 FPU stack ----------------------------------------------------
 * NOTE: ARM64 has no 80-bit extended float; we use double (64-bit).
 * This can deviate slightly from x86's 80-bit intermediate precision.    */
#define ST(n)    (c->st[(c->top + (n)) & 7])
/* CAUTION: the source value must be computed BEFORE top changes.  ST(i) is
 * defined relative to top, so "fld st(0)" in the old FPUSH read the newly
 * opened empty slot -> it loaded garbage instead of copying itself.  That
 * broke the CRT's __ftol routine (it starts with fld st(0)), i.e. EVERY
 * float->int conversion in the game.  Symptom: the frame-delta measurement
 * always came out as 1 unit. */
#define FPUSH(v) do{ double _fpv = (double)(v); \
    c->top = (c->top - 1) & 7; c->st[c->top] = _fpv; }while(0)
#define FPOP()   do{ c->top = (c->top + 1) & 7; }while(0)

/* x87 compare -> status word C0/C2/C3 bits (8/10/14) */
#define FSW_C0 (1u<<8)
#define FSW_C1 (1u<<9)
#define FSW_C2 (1u<<10)
#define FSW_C3 (1u<<14)
/* fnstsw: on real hardware the TOP field (bits 11-13) is read as well. */
static inline uint16_t fsw_get(const struct CPU *c){
    return (uint16_t)((c->fsw & (uint16_t)~0x3800u) | (uint16_t)((c->top & 7u) << 11));
}
/* X87OPT: with in-block x87 register allocation c->top is written at the end
 * of the region; fnstsw takes the (static) top at that point explicitly. */
static inline uint16_t fsw_get_top(const struct CPU *c, uint32_t top){
    return (uint16_t)((c->fsw & (uint16_t)~0x3800u) | (uint16_t)((top & 7u) << 11));
}
static inline void fcompare(struct CPU *c, double a, double b){
    c->fsw &= (uint16_t)~(FSW_C0|FSW_C2|FSW_C3);
    if (a != a || b != b)      c->fsw |= FSW_C0|FSW_C2|FSW_C3;  /* unordered (NaN) */
    else if (a > b)            ;                                 /* all 0 */
    else if (a < b)            c->fsw |= FSW_C0;
    else                       c->fsw |= FSW_C3;
}
/* fcomi / fucomi: write EFLAGS directly.  Its locals are not named _x: an
 * X87OPT region keeps the x87 stack in an array _x[] (tools/lift.py), and
 * FCOMI(_x[8], _x[12]) would read the macro's own _x (MW's sub_0067edfc) */
#define FCOMI(a,b) do{ double _fa=(a),_fb=(b); \
    if(_fa!=_fa||_fb!=_fb){ RG_(zf)=1;RG_(pf)=1;RG_(cf)=1; } \
    else { RG_(pf)=0; RG_(zf)=(_fa==_fb); RG_(cf)=(_fa<_fb); } RG_(of)=RG_(sf)=RG_(af)=0; }while(0)

/* ---- EFLAGS pack/unpack (pushfd/popfd/sahf) --------------------------- */
static inline uint32_t eflags_get(const struct CPU *c){
    return 0x00000202u | (c->cf&1u) | ((c->pf&1u)<<2) | ((c->af&1u)<<4)
         | ((c->zf&1u)<<6) | ((c->sf&1u)<<7) | ((c->df&1u)<<10) | ((c->of&1u)<<11)
         | (c->sysfl & 0x00240000u);
}
static inline void eflags_set(struct CPU *c, uint32_t f){
    c->cf=f&1u; c->pf=(f>>2)&1u; c->af=(f>>4)&1u;
    c->zf=(f>>6)&1u; c->sf=(f>>7)&1u; c->df=(f>>10)&1u; c->of=(f>>11)&1u;
    c->sysfl = f & 0x00240000u;
}

/* ---- string instructions ---------------------------------------------- */
#define DFSTEP(n) (c->df ? -(int32_t)(n) : (int32_t)(n))

/* ---- watchpoint: watch a guest memory address ------------------------ */
extern uint32_t g_watch_addr, g_watch_val, g_watch_prev_ip;
void watch_report_change(uint32_t culprit_ip, uint32_t oldv, uint32_t newv);

/* Instruction-level execution trace: the last 64K guest instruction addresses.
 * On a crash/error it answers "how did we get here" exactly.              */
#define ITRACE_BITS 16
#define ITRACE_SIZE (1u << ITRACE_BITS)
#define ITRACE_MASK (ITRACE_SIZE - 1u)
extern uint32_t g_itrace[ITRACE_SIZE];
extern uint64_t g_itrace_n;

static inline void watch_check(uint32_t ip){
    g_itrace[(uint32_t)g_itrace_n & ITRACE_MASK] = ip;
    g_itrace_n++;
    if (__builtin_expect(g_watch_addr != 0u, 0)){
        uint32_t v = *(uint32_t*)(g_mem + g_watch_addr);
        if (__builtin_expect(v != g_watch_val, 0)){
            watch_report_change(g_watch_prev_ip, g_watch_val, v);
            g_watch_val = v;
        }
    }
    g_watch_prev_ip = ip;
}
void itrace_dump(int n);

/* ---- function coverage measurement (if generated with COVERAGE=1) -----
 * Goal: answer "which code NEVER runs?".  Shows the subsystem that does
 * not run directly instead of chasing symptoms.                        */
extern uint32_t *g_cov;   /* CALL COUNT per function */
extern uint32_t g_last_fn;          /* for the sampler: the last function entered */
extern int      g_prof_on;          /* 1 if SAMPLE / COVERAGE is on */
/* CAUTION: g_last_fn used to be written UNCONDITIONALLY here.  That is a
 * global store on EVERY guest function entry (millions per second).  Write
 * it only while measuring. */
static inline void cov_hit(uint32_t va){
    if (__builtin_expect(g_prof_on, 0)){
        g_last_fn = va;
        if (g_cov && va >= DISPATCH_TEXT_BASE && va < DISPATCH_TEXT_BASE + (DISPATCH_TEXT_SIZE))
            g_cov[va - DISPATCH_TEXT_BASE]++;
    }
}
void cov_report(const char *path);

/* x87 rounding: the result follows the control word's RC field (bits 10-11).
 * 00=nearest (even), 01=down, 10=up, 11=toward zero (chop).
 * The CRT's __ftol routine sets RC to 11 and calls fistp; if we ignore that,
 * every float->int conversion comes out one too high when the fraction >= 0.5. */
static inline double x87_round(uint16_t fcw, double v){
    switch ((fcw >> 10) & 3u){
    case 0:  return nearbyint(v);      /* host mode: nearest, even */
    case 1:  return floor(v);
    case 2:  return ceil(v);
    default: return trunc(v);
    }
}

/* ---- x87 precision control (lift-time X87PC=1, tools/lift.py) ---------
 * The hardware rounds the result of FADD/FSUB/FMUL/FDIV/FSQRT (and their
 * P/R/I forms) to the significand width the control word's PC field
 * (bits 8-9) selects: 00 = 24 bits, 10 = 53, 11 = 64.  The game's main
 * thread runs at PC=00 on Windows (Direct3D's CreateDevice without
 * D3DCREATE_FPU_PRESERVE sets it), so its intermediates are single
 * precision with the extended exponent range.  Our registers are doubles:
 * PC=53/64 stay double, PC=24 rounds the double result to 24 bits in the
 * rounding direction of the RC field.  The double is the correctly rounded
 * (to nearest) exact result, so with RC = nearest this is exact whenever
 * both operands have at most 24 significant bits (Figueroa: 53 >= 2*24+2);
 * with a wider operand (fild of a big int, a double constant) a 24-bit tie
 * can very rarely round differently.  With RC = down/up/chop it is NOT
 * exact: when the nearest double is itself a 24-bit value (e.g. an add or
 * subtract of operands more than ~29 binary orders apart) the directed
 * rounding is lost, 1 ulp off (checked against Unicorn's x87).  The game
 * does no x87 arithmetic at such RC on its PC=24 thread (its fldcw sites
 * are float->int loops and CRT/D3DX code at PC=64), so this is accepted.
 * Exponent range: extended vs double, irrelevant here.                   */
/* Out of line and cold: the fast path below inlines only the FCVT pair, which
 * keeps an X87PC=1 build ~20 MB smaller. */
static __attribute__((noinline, cold, unused)) double x87_round24(double v, uint32_t rc){
    uint64_t b; memcpy(&b, &v, 8);
    uint64_t low = b & 0x1FFFFFFFull;            /* the 29 bits below 24 */
    if (!low || ((b >> 52) & 0x7FFu) == 0x7FFu) return v;   /* exact, inf, nan */
    uint64_t t = b - low;                        /* toward zero */
    int up;
    if (rc == 0) up = low > 0x10000000ull || (low == 0x10000000ull && (t & 0x20000000ull));
    else if (rc == 3) up = 0;
    else up = (int)(b >> 63) == (rc == 1);       /* down: negatives grow; up: positives */
    if (up) t += 0x20000000ull;
    memcpy(&v, &t, 8); return v;
}
/* PC=24 with RC=nearest (the game thread's word): the hardware double->float
 * conversion rounds to nearest even exactly like x87_round24 as long as the
 * result is a normal float, and is ~40% cheaper on the dependency chain.
 * Zero, float subnormals, overflow and NaN take the exact path. */
static inline double x87_pcround(uint16_t fcw, double v){
    if (__builtin_expect((fcw & 0x0F00u) == 0u, 1)){
        float f = (float)v, a = __builtin_fabsf(f);
        if (__builtin_expect(a > 0x1p-126f && a <= 0x1.fffffep127f, 1)) return (double)f;
#ifdef __wasm__
        /* Zero is exact too.  The game's matrix products make many zero
         * terms, and on wasm their call to the out-of-line path cost ~1.4 %
         * of a race frame. */
        if (v == 0.0) return v;
#endif
        return x87_round24(v, 0);
    }
    if (fcw & 0x0300u) return v;                 /* PC=53/64: double */
    return x87_round24(v, (fcw >> 10) & 3u);
}
#define X87R(v) x87_pcround(c->fcw, (v))

/* ---- invalid-operation results and out-of-range conversions ----------
 * On x86 invalid operations such as 0/0, sqrt(-x) produce "real indefinite":
 * a quiet NaN with a NEGATIVE sign.  C's sqrt/0.0 result is a positive NaN,
 * so it has to be fixed up separately.  If one of the inputs is a NaN, that
 * one propagates.                                                       */
#define X87_INDEF (-__builtin_nan(""))
/* Two NaN operands: the x87 returns the one with the LARGER significand
 * (Intel SDM vol. 1, table 4-7); the ARM FPU (and the old code here) returns
 * the first.  Found by the drift oracle (runtime/oracle.c): the rigid-
 * body integrator 0x5AA480 adds NaN garbage of unused state slots.
 * The NaN path is out of line and cold: inlined at every x87 operation it
 * made the lifted code ~8 MB (17 %) larger, X87PC=0 builds included. */
static __attribute__((noinline, cold, unused)) double x87_r2_nan(double a, double b){
    if (a != a && b != b){
        uint64_t x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8);
        return (y & 0x000FFFFFFFFFFFFFull) > (x & 0x000FFFFFFFFFFFFFull) ? b : a;
    }
    if (a != a) return a;
    if (b != b) return b;
    return X87_INDEF;
}
static inline double x87_r2(double a, double b, double r){
    if (__builtin_expect(r == r, 1)) return r;
    return x87_r2_nan(a, b);
}
static inline double x87_r1(double a, double r){
    if (r == r) return r;
    if (a != a) return a;
    return X87_INDEF;
}
/* X87R(x87_r2(a, b, r)) in one test (tools/lift.py emits X87R2 for the x87
 * arithmetic).  On the game thread's word (PC=24, RC=nearest) a result in the
 * normal float range is no NaN, so the NaN test of x87_r2 is only made on the
 * way to the out-of-line path, which does the two in a row.  Same results:
 * that path is taken for everything the fast tests below do not decide. */
static __attribute__((noinline, cold, unused)) double x87_r2pc_slow(uint16_t fcw, double a, double b, double r){
    return x87_pcround(fcw, x87_r2(a, b, r));
}
static inline double x87_r2pc(uint16_t fcw, double a, double b, double r){
    if (__builtin_expect((fcw & 0x0F00u) == 0u, 1)){
        float f = (float)r, m = __builtin_fabsf(f);
        if (__builtin_expect(m > 0x1p-126f && m <= 0x1.fffffep127f, 1)) return (double)f;
        if (r == 0.0) return r;                  /* exact (see x87_pcround) */
    } else if ((fcw & 0x0300u) && __builtin_expect(r == r, 1)) return r;   /* PC=53/64 */
    return x87_r2pc_slow(fcw, a, b, r);
}
#define X87R2(a, b, r) x87_r2pc(c->fcw, (a), (b), (r))
static inline float sse_r2f(float a, float b, float r){
    if (r == r) return r;
    if (a != a) return a;
    if (b != b) return b;
    return (float)X87_INDEF;
}
static inline float sse_r1f(float a, float r){
    if (r == r) return r;
    if (a != a) return a;
    return (float)X87_INDEF;
}
static inline double sse_r2d(double a, double b, double r){ return x87_r2(a,b,r); }
static inline double sse_r1d(double a, double r){ return x87_r1(a,r); }

/* Out of range / NaN -> "integer indefinite" (the most negative value). */
static inline uint32_t x86_cvt_i32(double v){
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint16_t x86_cvt_i16(double v){
    if (!(v >= -32768.0 && v <= 32767.0)) return 0x8000u;
    return (uint16_t)(int16_t)v;
}
static inline uint64_t x86_cvt_i64(double v){
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        return 0x8000000000000000ull;
    return (uint64_t)(int64_t)v;
}

/* The CRT's _ftol2 (sub_0075bed4; runtime/overrides.c tells its story):
 * ST(0) -> int64 in edx:eax, truncated toward zero, ST(0) popped, with the
 * lifted body's ecx, flags and x87 slots.  The override runs FTOL2_BODY, and
 * a lift with FTOL2INL=1 (web/gen.sh) puts FTOL2_INL at the game's ~1,700
 * call sites instead of a call: ~11,000 calls a race frame, whose ecx and
 * flags the callers never read (with LREGS clang then drops them). */
#define FTOL2_BODY(c) do { \
    uint32_t _fT = (uint32_t)(c)->top & 7u; \
    double _fv = (c)->st[_fT]; \
    uint64_t _fq = x86_cvt_i64(x87_round((c)->fcw, _fv));        /* fld st(0); fst m32; fistp m64 */ \
    double _frq = (double)(int64_t)_fq;                           /* fild m64 */ \
    (c)->st[(_fT - 1u) & 7u] = _frq; \
    uint32_t _flo = (uint32_t)_fq, _fhi = (uint32_t)(_fq >> 32); \
    float _ffv = (float)_fv; uint32_t _fedx; memcpy(&_fedx, &_ffv, 4); \
    RG_(eax) = _flo; \
    if (_flo == 0u) _fedx = _fhi; \
    if (_flo == 0u && (_fhi & 0x7fffffffu) == 0u){                /* 0 or integer indefinite: both popped */ \
        RG_(edx) = _fhi; FLAGS_LOG_M(0u, 32, 0x3fu); (c)->top = (int32_t)((_fT + 1u) & 7u); \
    } else { \
        double _fd = X87R(x87_r2(_fv, _frq, _fv - _frq));        /* fsubp: ST(1) = v - r, pop */ \
        (c)->st[_fT] = _fd; \
        float _ffd = (float)_fd; uint32_t _fecx; memcpy(&_fecx, &_ffd, 4);   /* fstp m32 */ \
        (c)->top = (int32_t)((_fT + 1u) & 7u); \
        if (_fedx >> 31) _fecx ^= 0x80000000u;                     /* negative: +1 when rounded down */ \
        uint32_t _fe2 = _fecx + 0x7fffffffu, _fcf = _fe2 < _fecx; \
        RG_(ecx) = _fe2; \
        if (_fedx >> 31){ \
            uint32_t _fa = _flo + _fcf, _fc2 = _fcf ? _fa <= _flo : _fa < _flo; RG_(eax) = _fa; \
            uint32_t _fr = _fhi + _fc2; FLAGS_ADC_M(_fhi, 0u, _fc2, _fr, 32, 0x3fu); RG_(edx) = _fr; \
        } else { \
            uint32_t _fa = _flo - _fcf, _fc2 = _fcf ? _flo <= 0u : 0u; RG_(eax) = _fa; \
            uint32_t _fr = _fhi - _fc2; FLAGS_SBB_M(_fhi, 0u, _fc2, _fr, 32, 0x3fu); RG_(edx) = _fr; \
        } \
    } } while (0)
#define FTOL2_INL(c) do { FTOL2_BODY(c); POP32((c)->eip); } while (0)

/* ---- 80-bit extended floating point (x87 tbyte) <-> double ----------- */
static inline void f80_store(uint8_t *p, double v){
    /* sign, 15-bit exponent, 64-bit explicit mantissa */
    uint64_t bits; memcpy(&bits, &v, 8);
    uint32_t sign = (uint32_t)(bits >> 63);
    int32_t  exp  = (int32_t)((bits >> 52) & 0x7FF);
    uint64_t man  = bits & 0xFFFFFFFFFFFFFull;
    uint16_t e80; uint64_t m80;
    if (exp == 0x7FF){ e80 = 0x7FFF; m80 = man ? (0xC000000000000000ull | (man << 11)) : 0x8000000000000000ull; }
    else if (exp == 0){
        if (!man){ e80 = 0; m80 = 0; }
        else { int sh = 0; while (!(man & 0x10000000000000ull)) { man <<= 1; sh++; }
               e80 = (uint16_t)(16383 - 1022 - sh); m80 = (man << 11) | 0x8000000000000000ull; }
    } else { e80 = (uint16_t)(exp - 1023 + 16383); m80 = 0x8000000000000000ull | (man << 11); }
    memcpy(p, &m80, 8);
    uint16_t se = (uint16_t)((sign << 15) | e80);
    memcpy(p + 8, &se, 2);
}
static inline double f80_load(const uint8_t *p){
    uint64_t m80; uint16_t se;
    memcpy(&m80, p, 8); memcpy(&se, p + 8, 2);
    uint32_t sign = (uint32_t)(se >> 15);
    int32_t  e80  = (int32_t)(se & 0x7FFF);
    if (e80 == 0 && m80 == 0) return sign ? -0.0 : 0.0;
    if (e80 == 0x7FFF){
        double inf = sign ? -1.0/0.0 : 1.0/0.0;
        return (m80 << 1) ? (0.0/0.0) : inf;
    }
    int32_t exp = e80 - 16383 + 1023;
    uint64_t man = (m80 & 0x7FFFFFFFFFFFFFFFull) >> 11;
    if (exp <= 0)   return sign ? -0.0 : 0.0;
    if (exp >= 0x7FF){ double inf = sign ? -1.0/0.0 : 1.0/0.0; return inf; }
    uint64_t bits = ((uint64_t)sign << 63) | ((uint64_t)(uint32_t)exp << 52) | man;
    double d; memcpy(&d, &bits, 8); return d;
}
/* frstor's registers (tools/lift.py X87ENV): what fnsave stored with
 * f80_store comes back as it was.  A NaN keeps its sign and the payload a
 * double holds, quiet bit set (f80_load: the host's NaN); a value in a
 * double's denormal range (e80 != 0, -52 < exp <= 0) becomes that denormal,
 * truncated as f80_load truncates (f80_load: 0); the rest is f80_load's.
 * f80_load stays as it is for fld tbyte, in both games' lifts (MW's too:
 * 0x7D8CC0's two tbyte operands; UG2's lift must not change) */
static inline double f80_load_x(const uint8_t *p){
    uint64_t m80; uint16_t se;
    memcpy(&m80, p, 8); memcpy(&se, p + 8, 2);
    uint64_t sign = (uint64_t)(se >> 15);
    int32_t  e80  = (int32_t)(se & 0x7FFF);
    int32_t  exp  = e80 - 16383 + 1023;
    uint64_t bits;
    if (e80 == 0x7FFF && (m80 << 1)) bits = (sign << 63) | 0x7FF8000000000000ull | ((m80 >> 11) & 0xFFFFFFFFFFFFFull);
    else if (e80 != 0 && exp <= 0 && exp > -52) bits = (sign << 63) | (m80 >> (12 - exp));
    else return f80_load(p);
    double d; memcpy(&d, &bits, 8); return d;
}
#define WCHK(ip) watch_check(ip)
void guest_hook(CPU *c, uint32_t ip);

void mem_init(void);
void mem_commit(uint32_t addr, uint32_t size);
int  mem_load_pe(const char *path);
void mem_fault_handler_install(void);
void mem_altstack_install(void);
extern uint8_t g_cbits[1u<<15];   /* bitmap of the committed 16 KB pages */
static inline int mem_is_committed(uint32_t addr){
    uint32_t pg = (addr) >> 14;
    return (g_cbits[pg>>3] >> (pg&7)) & 1;
}

#endif
