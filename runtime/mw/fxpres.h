/* fxpres.h -- the FXLC preshader evaluator: what d3dx9_26's interpreter
 * (0x4E8817) computes from a preshader, its literal table and its input
 * registers, under the x87 precision the caller runs at, and how D3DX fills
 * those registers from effect parameters and turns the outputs into a
 * state's value.  docs/mw/fx-preshader.md is the contract and the section
 * numbers are its.  No globals, no I/O, no floating-point hardware for the
 * exact operations (§3.1): the same bits on every host and in wasm.
 *
 * Every value crosses this API as the bits of an IEEE double (uint64_t), or
 * as the dwords of a state value, so nothing is rounded in transit.
 */
#ifndef MW_FXPRES_H
#define MW_FXPRES_H
#include "fxmodel.h"

/* fxp_load's and fxp_run's results (§2.2, §5) */
enum {
    FXP_OK = 0,
    /* D3DX refuses the preshader too (its loader returns E_FAIL, §2.2) */
    FXP_E_VERSION,       /* version not 0x46580200..0x46580201 or 0x54580100 */
    FXP_E_NOFXLC,        /* no FXLC comment (D3DXFindShaderComment, §2.1) */
    FXP_E_NOCLIT,        /* no CLIT comment */
    FXP_E_OPERAND,       /* an instruction or operand the loader refuses: past the end, a bad table or flag */
    /* D3DX accepts it; A stops where what D3DX does next depends on memory A does not have (§5.1) */
    FXP_E_SCAN,          /* fxp_load: the comment search, the version or a count read past the end of the blob */
    FXP_E_NOISE,         /* fxp_run: opcode 0x502 or 0x711 (not scalar) reached: D3DX's noise, from tables in the
                          * DLL (§3.2) */
    FXP_E_INPUTS,        /* fxp_run: an instruction's 257th input: D3DX writes past its slot array (§3.3) */
    FXP_E_RANGE,         /* fxp_run: an access outside a table, the literals, the blob or the temporaries, or
                          * through a table-9 input (NULL in both D3DX paths): D3DX touches the memory there */
    FXP_E_STALE,         /* fxp_run: a read through an input slot no instruction of this run has set (D3DX reads
                          * through what its stack held, §3.3), or with temps_unknown a read of a temporary no
                          * instruction of this run has written (D3DX's temporaries are its stack, §5.1) */
    FXP_E_TYPE,          /* fxp_eval: a value A does not scatter (§4.2): an object (D3DX: E_FAIL); a struct, an
                          * array, more than 4 rows or a BOOL, which D3DX's code handles in ways not run against
                          * it yet (X only); a type other than BOOL, INT and FLOAT */
    FXP_E_ARG,           /* a call the API does not take: pc not 24, 53 or 64; a state whose source is not a
                          * preshader; a program not of this effect; cap too small */
    FXP_E_NOMEM,
    FXP_NERR
};
const char *fxp_err_name(int err);              /* "ok", "version", ... ; NULL past the table */
static inline int fxp_refused_by_d3dx(int err){ return err >= FXP_E_VERSION && err <= FXP_E_OPERAND; }

typedef struct fxp_prog fxp_prog;

/* Decode and check a preshader blob (the resource's size bytes) as D3DX's
 * loader does (0x4E9146, §2.2).  The blob must outlive the program: the
 * literals are read from it.  0, a D3DX refusal (fxp_refused_by_d3dx),
 * FXP_E_SCAN or FXP_E_NOMEM (and *out NULL). */
int  fxp_load(fxp_prog **out, const uint8_t *blob, uint32_t size);
void fxp_free(fxp_prog *p);
uint32_t fxp_ninstr(const fxp_prog *p);
int  fxp_approx(const fxp_prog *p);             /* uses an opcode computed with the C library (§3.2: exp, log,
                                                 * sin, cos, asin, acos, atan, atan2) */

#define FXP_NTEMP 256
typedef struct {                 /* the interpreter's tables (§2.5); doubles as their bits */
    const uint64_t *c; uint32_t csize, cmask;   /* table 2: input registers, 4 per register */
    const uint64_t *v; uint32_t vsize, vmask;   /* table 3: NULL refuses every read (FXP_E_RANGE) */
    uint64_t *o; uint32_t osize, omask;         /* tables 4, 5, 6 (oc, ob, oi: one table in both D3DX paths) */
    uint64_t r[FXP_NTEMP];                      /* table 7: temporaries (D3DX: the stack, never cleared) */
    int temps_unknown;                          /* 0: r holds what D3DX's temporaries hold (the oracle clears them);
                                                 * 1: they hold what D3DX's stack held, so a read of a temporary
                                                 * no instruction of this run has written stops (FXP_E_STALE, §5.1;
                                                 * fxp_eval runs so), and r need not be set */
    int pc;                                     /* the x87 precision: 24, 53 or 64 (§3.4) */
} fxp_regs;
/* Run the program on x: FXP_OK; FXP_E_RANGE, FXP_E_STALE, FXP_E_INPUTS or
 * FXP_E_NOISE where D3DX goes on with memory A does not have (the tables are
 * as D3DX would have left them up to that access); FXP_E_ARG for a pc that
 * is not 24, 53 or 64.  It needs about 3 KB of stack (fxp_eval about 8 KB in
 * all). */
int  fxp_run(const fxp_prog *p, fxp_regs *x);
/* The pc for an x87 control word (§4.4): 24, 53 or 64 for the PC field 0, 2
 * or 3 with rounding to nearest; 0, which every fxp_* call refuses
 * (FXP_E_ARG), for the reserved PC field 1 or another rounding control (D3DX
 * computes under the caller's RC, which A does not model). */
int  fxp_pc_from_cw(uint32_t cw);

/* ---- the effect path (§4): gather, run, scatter ---- */
typedef struct fxp_effect fxp_effect;
/* Decode every preshader of the model (fxm_progs).  When D3DX's loader
 * refuses one (fxp_refused_by_d3dx), D3DXCreateEffect fails with E_FAIL
 * (§1, O): that error is returned and *out is NULL.  A program A refuses on
 * its own (FXP_E_SCAN) is kept with its error, which fxp_prog_error and
 * fxp_state_value return. */
int  fxp_effect_new(fxp_effect **out, const fxm_effect *e);
void fxp_effect_free(fxp_effect *x);
/* fxp_load's result for program pr of the model: FXP_OK, FXP_E_SCAN,
 * FXP_E_TYPE (not a preshader) or FXP_E_ARG (not this effect's). */
int  fxp_prog_error(const fxp_effect *x, const fxm_prog *pr);
/* The value of state s, whose source (fxm_state_source) is a preshader,
 * from the effect's current parameter values at precision pc (24, 53 or
 * 64: fxp_pc_from_cw of the caller's x87 control word): the dwords of its
 * value as D3DX stores them (rows x cols of its typedef, FLOAT as IEEE
 * single bits, INT as integers), *n of them, at most cap.  FXP_OK, the
 * program's error, fxp_run's (the temporaries unknown), FXP_E_TYPE, or
 * FXP_E_ARG (a state that is not a preshader's, a pc that is not 24, 53 or
 * 64, cap too small). */
int  fxp_state_value(fxp_effect *x, const fx2_state *s, int pc, uint32_t *out, uint32_t cap, uint32_t *n);
/* The same for program pr of the model and a value of typedef t. */
int  fxp_eval(fxp_effect *x, const fxm_prog *pr, const fx2_type *t, int pc, uint32_t *out, uint32_t cap, uint32_t *n);

/* ---- the number model (§3.1), exposed for the tests ---- */
uint64_t fxp_x87_add(uint64_t a, uint64_t b, int pc);      /* fld a; fadd b; fstp: a + b */
uint64_t fxp_x87_mul(uint64_t a, uint64_t b, int pc);
uint64_t fxp_x87_rcp(uint64_t a, int pc);                  /* fld1; fdiv a */
uint64_t fxp_x87_rsq(uint64_t a, int pc);                  /* fld a; fabs; fsqrt; fdivr 1.0 */
uint32_t fxp_ftol(uint64_t a);                             /* _ftol2's low dword: truncation, 0 for NaN, inf, |a| >= 2^63 */
uint32_t fxp_d2f(uint64_t a);                              /* fld qword; fstp dword */
uint64_t fxp_f2d(uint32_t f);                              /* fld dword; fstp qword */
uint64_t fxp_i2d(uint32_t i);                              /* fild dword; fstp qword */
#endif
