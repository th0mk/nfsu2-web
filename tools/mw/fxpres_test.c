/* fxpres_test.c -- unit tests for runtime/mw/fxpres.c, the FXLC preshader
 * evaluator (docs/mw/fx-preshader.md §6.1).
 *
 *   clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
 *         -Iruntime/mw -o out/mw/fx/bin/fxpres_test tools/mw/fxpres_test.c runtime/mw/fxpres.c \
 *         runtime/mw/fxmodel.c runtime/mw/fx2.c -lm
 *   out/mw/fx/bin/fxpres_test [--mw] [<NAME>.fxo...]
 *
 * Always: the number model (§3.1) against values worked out by hand, every
 * check of the loader (§2.2) and every opcode, form and operand rule of the
 * interpreter (§2.4, §3.2, §3.3) on programs built here.  With effect files:
 * every preshader loads, and with every input parameter set to random bits
 * each preshader state of a mov-only program equals its input converted the
 * way §4 says (the gather's and the scatter's conversions composed); --mw
 * adds the counts fx-preshader §7 gives for Most Wanted.  Nothing of the
 * game or the DLL is written here.  Exit status 1 when a check failed.
 */
#include "fxpres.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long g_checks, g_fails;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)){ g_fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define EQ64(got, want, what) do { uint64_t g_ = (got), w_ = (want); \
    CHECK(g_ == w_, "%s: 0x%016llx, want 0x%016llx", what, (unsigned long long)g_, (unsigned long long)w_); } while (0)

static uint64_t D(double d){ uint64_t b; memcpy(&b, &d, 8); return b; }
#define NAN_D   0xFFF8000000000000ull     /* the x87's default NaN */
#define ONE     0x3FF0000000000000ull

/* ======================= the number model (§3.1) ======================= */
static void test_numbers(void){
    EQ64(fxp_x87_rcp(D(3.0), 24), 0x3FD5555560000000ull, "1/3 at 24 bits");
    EQ64(fxp_x87_rcp(D(3.0), 53), 0x3FD5555555555555ull, "1/3 at 53 bits");
    EQ64(fxp_x87_rcp(D(3.0), 64), 0x3FD5555555555555ull, "1/3 at 64 bits");
    EQ64(fxp_x87_rsq(D(7.0), 53), 0x3FD83091E6A7F7E6ull, "1/sqrt(7) at 53 bits: correctly rounded");
    EQ64(fxp_x87_rsq(D(7.0), 64), 0x3FD83091E6A7F7E7ull, "1/sqrt(7) at 64 bits: rounded twice");
    EQ64(fxp_x87_rsq(D(-4.0), 64), D(0.5), "rsq takes |x|");
    EQ64(fxp_x87_rsq(0x8000000000000000ull, 53), 0x7FF0000000000000ull, "rsq(-0) = +inf");
    EQ64(fxp_x87_rsq(0x7FF0000000000000ull, 53), 0, "rsq(inf) = +0");
    /* 1 + (2^-53 + 2^-65): at 64 bits 1 + 2^-53, a tie to even on the store: 1; at 53 bits 1 + 2^-52 */
    uint64_t b = D(1.1102230246251565e-16 + 2.710505431213761e-20);
    EQ64(b, 0x3CA0010000000000ull, "the operand 2^-53 + 2^-65");
    EQ64(fxp_x87_add(ONE, b, 64), ONE, "double rounding at 64 bits");
    EQ64(fxp_x87_add(ONE, b, 53), 0x3FF0000000000001ull, "one rounding at 53 bits");
    EQ64(fxp_x87_add(ONE, b, 24), ONE, "24 bits");
    EQ64(fxp_x87_add(D(1.0 / 3.0), 0, 24), 0x3FD5555560000000ull, "x + 0 is rounded to 24 bits");
    EQ64(fxp_x87_add(ONE, D(-1.0), 64), 0, "1 - 1 = +0");
    EQ64(fxp_x87_add(0x8000000000000000ull, 0x8000000000000000ull, 64), 0x8000000000000000ull, "-0 + -0 = -0");
    EQ64(fxp_x87_add(0x8000000000000000ull, 0, 64), 0, "-0 + +0 = +0");
    EQ64(fxp_x87_add(0x7FF0000000000000ull, 0xFFF0000000000000ull, 64), NAN_D, "inf - inf");
    EQ64(fxp_x87_mul(0, 0x7FF0000000000000ull, 24), NAN_D, "0 * inf");
    EQ64(fxp_x87_mul(0x8000000000000000ull, D(3.0), 24), 0x8000000000000000ull, "-0 * 3");
    EQ64(fxp_x87_rcp(0x8000000000000000ull, 24), 0xFFF0000000000000ull, "1 / -0");
    /* NaNs: Intel's rule for a register and a memory operand (§3.1) */
    EQ64(fxp_x87_add(0x7FF8000000000001ull, 0x7FF0000000000002ull, 64), 0x7FF8000000000001ull, "QNaN reg, SNaN mem: the QNaN");
    EQ64(fxp_x87_add(0x7FF8000000000001ull, 0x7FF8000000000002ull, 64), 0x7FF8000000000002ull, "two QNaNs: larger significand");
    EQ64(fxp_x87_add(0x7FF8000000000002ull, 0x7FF8000000000001ull, 64), 0x7FF8000000000002ull, "two QNaNs, the other way");
    EQ64(fxp_x87_add(0xFFF8000000000002ull, 0x7FF8000000000002ull, 64), 0x7FF8000000000002ull, "equal significands: the positive one");
    EQ64(fxp_x87_add(0x7FF0000000000005ull, ONE, 64), 0x7FF8000000000005ull, "an SNaN register is loaded quiet");
    EQ64(fxp_x87_mul(ONE, 0xFFF0000000000007ull, 64), 0xFFF8000000000007ull, "an SNaN memory operand comes back quiet");
    EQ64(fxp_x87_rcp(0x7FF0000000000003ull, 64), 0x7FF8000000000003ull, "1 / SNaN");
    /* subnormals and overflow on the store */
    EQ64(fxp_x87_mul(0x0010000000000000ull, D(0.5), 64), 0x0008000000000000ull, "half the smallest normal");
    EQ64(fxp_x87_mul(0x0010000000000001ull, D(0.5), 64), 0x0008000000000000ull, "a subnormal tie to even, down");
    EQ64(fxp_x87_mul(0x0010000000000003ull, D(0.5), 64), 0x0008000000000002ull, "a subnormal tie to even, up");
    EQ64(fxp_x87_mul(1, D(0.5), 64), 0, "half the smallest subnormal: a tie to 0");
    EQ64(fxp_x87_mul(1, D(0.75), 64), 1, "three quarters of it: 1");
    EQ64(fxp_x87_mul(0x7FEFFFFFFFFFFFFFull, D(2.0), 64), 0x7FF0000000000000ull, "overflow");
    EQ64(fxp_x87_mul(0x7FEFFFFFFFFFFFFFull, D(1.0000000000000002), 24), 0x7FF0000000000000ull, "DBL_MAX * (1 + eps) at 24 bits");
    EQ64(fxp_x87_mul(D(3.0), D(1.0 / 3.0), 53), ONE, "3 * (1/3) at 53 bits");
    EQ64(fxp_x87_mul(1, 1, 64), 0, "the smallest subnormal squared: 0");
    /* _ftol2, the double/float conversions, fild */
    static const struct { double v; uint32_t w; } tl[] = { { 2.5, 2 }, { -2.5, 0xFFFFFFFEu }, { 0.7, 0 }, { -0.7, 0 },
        { 4294967301.0, 5 }, { 9223372036854775808.0, 0 }, { -2147483648.0, 0x80000000u }, { 1e300, 0 },
        { 3.9999999999999996, 3 }, { -1.0, 0xFFFFFFFFu } };
    for (unsigned i = 0; i < sizeof tl / sizeof tl[0]; i++)
        CHECK(fxp_ftol(D(tl[i].v)) == tl[i].w, "ftol(%.17g) = 0x%08x, want 0x%08x", tl[i].v, fxp_ftol(D(tl[i].v)), tl[i].w);
    CHECK(fxp_ftol(NAN_D) == 0 && fxp_ftol(0x7FF0000000000000ull) == 0, "ftol of NaN and inf: 0");
    CHECK(fxp_d2f(D(1.0000001192092896)) == 0x3F800001u, "d2f exact");
    CHECK(fxp_d2f(D(1.00000005960464477539)) == 0x3F800000u, "d2f: a tie to even, down");
    CHECK(fxp_d2f(D(1.0000001788139343)) == 0x3F800002u, "d2f: a tie to even, up");
    CHECK(fxp_d2f(0x7FF0000000000001ull) == 0x7FC00000u, "d2f of an SNaN: quiet, payload cut");
    CHECK(fxp_d2f(0xFFF8000020000000ull) == 0xFFC00001u, "d2f of a NaN payload");
    CHECK(fxp_d2f(D(1.401298464324817e-45)) == 1u, "d2f: the smallest float subnormal");
    CHECK(fxp_d2f(D(0.7006492321624085e-45)) == 0u, "d2f: half of it, a tie to 0");
    CHECK(fxp_d2f(D(3.4028235677973366e38)) == 0x7F800000u, "d2f: overflow");
    CHECK(fxp_d2f(D(3.4028234663852886e38)) == 0x7F7FFFFFu, "d2f: FLT_MAX");
    EQ64(fxp_f2d(0x7F800001u), 0x7FF8000020000000ull, "f2d of an SNaN: quiet");
    EQ64(fxp_f2d(1u), D(1.401298464324817e-45), "f2d of a subnormal");
    EQ64(fxp_f2d(0x80000000u), 0x8000000000000000ull, "f2d of -0");
    EQ64(fxp_f2d(0x3F800000u), ONE, "f2d of 1");
    EQ64(fxp_i2d(0x80000000u), D(-2147483648.0), "fild INT_MIN");
    EQ64(fxp_i2d(0xFFFFFFFFu), D(-1.0), "fild -1");
    EQ64(fxp_i2d(0x01000001u), D(16777217.0), "fild 2^24 + 1: exact");
}

/* ======================= programs ======================= */
typedef struct { uint32_t w[4096]; uint32_t n; } blob;
static void put(blob *b, uint32_t v){ if (b->n < 4096) b->w[b->n++] = v; }
/* version, an empty CTAB, CLIT with lits, FXLC with the instructions in ins (nins of them), trailer, end */
static void build(blob *b, uint32_t version, const uint64_t *lits, uint32_t nlit, const uint32_t *ins, uint32_t nw,
                  uint32_t nins){
    b->n = 0;
    put(b, version);
    put(b, 0xFFFEu | 8u << 16); put(b, 0x42415443u); put(b, 28); put(b, 0); put(b, 0x46580200u); put(b, 0); put(b, 28);
    put(b, 0x100); put(b, 0);
    put(b, 0xFFFEu | (2 + 2 * nlit) << 16); put(b, 0x54494C43u); put(b, nlit);
    for (uint32_t i = 0; i < nlit; i++){ put(b, (uint32_t)lits[i]); put(b, (uint32_t)(lits[i] >> 32)); }
    put(b, 0xFFFEu | (4 + nw) << 16); put(b, 0x434C5846u); put(b, nins);
    for (uint32_t i = 0; i < nw; i++) put(b, ins[i]);
    put(b, 0xF0F0F0F0u); put(b, 0x0F0F0F0Fu);
    put(b, 0x0000FFFFu);
}
/* an instruction: token, inputs, then the output (table, offset) */
typedef struct { uint32_t w[1024]; uint32_t n, nins; } prog;
static void op_begin(prog *p, uint32_t op, uint32_t ncomp, int scalar, uint32_t nin){
    p->w[p->n++] = op << 20 | ncomp | (scalar ? 0x80000000u : 0); p->w[p->n++] = nin; p->nins++;
}
static void in_reg(prog *p, uint32_t table, uint32_t off){ p->w[p->n++] = 0; p->w[p->n++] = table; p->w[p->n++] = off; }
static void in_idx(prog *p, uint32_t it, uint32_t io, uint32_t table, uint32_t off){
    p->w[p->n++] = 1; p->w[p->n++] = it; p->w[p->n++] = io; p->w[p->n++] = table; p->w[p->n++] = off;
}
static void out_reg(prog *p, uint32_t table, uint32_t off){ in_reg(p, table, off); }
static void ins1(prog *p, uint32_t op, uint32_t n, int sc, uint32_t t0, uint32_t o0, uint32_t dt, uint32_t dof){
    op_begin(p, op, n, sc, 1); in_reg(p, t0, o0); out_reg(p, dt, dof);
}
static void ins2(prog *p, uint32_t op, uint32_t n, int sc, uint32_t t0, uint32_t o0, uint32_t t1, uint32_t o1,
                 uint32_t dt, uint32_t dof){
    op_begin(p, op, n, sc, 2); in_reg(p, t0, o0); in_reg(p, t1, o1); out_reg(p, dt, dof);
}

typedef struct { uint64_t c[64], o[64]; fxp_regs x; int err; } run_out;
static int g_unknown;            /* run with the temporaries unknown (fxp_regs.temps_unknown), as fxp_eval does */
/* load and run p with the literals lits on c (64 doubles, mask 63) and o (64, mask 63), at pc */
static int run(const prog *p, const uint64_t *lits, uint32_t nlit, const uint64_t *c, int pc, run_out *r){
    static blob b;
    build(&b, 0x46580200u, lits, nlit, p->w, p->n, p->nins);
    fxp_prog *fp = NULL;
    int err = fxp_load(&fp, (const uint8_t *)b.w, 4 * b.n);
    if (err) return -err;
    memset(r, 0, sizeof *r);
    if (c) memcpy(r->c, c, sizeof r->c);
    r->x.c = r->c; r->x.csize = 64; r->x.cmask = 63;
    r->x.o = r->o; r->x.osize = 64; r->x.omask = 63;
    r->x.temps_unknown = g_unknown;
    r->x.pc = pc;
    r->err = fxp_run(fp, &r->x);
    fxp_free(fp);
    return r->err;
}
static int load_words(const uint32_t *w, uint32_t n){
    fxp_prog *fp = NULL;
    int err = fxp_load(&fp, (const uint8_t *)w, 4 * n);
    fxp_free(fp);
    return err;
}

/* ======================= the loader (§2.1, §2.2) ======================= */
/* the programs, blobs and run tables live in static storage: the test runs in wasm at wasi-sdk's default 64 KB stack
 * (tools/mw/fxdump.sh --wasm) */
static void test_loader(void){
    static prog p; memset(&p, 0, sizeof p);
    ins2(&p, 0x204, 4, 0, 2, 0, 1, 1, 4, 0);
    static blob b;
    uint64_t lits[2] = { ONE, D(2.0) };
    static const struct { uint32_t v; int err; } vers[] = { { 0x46580200u, 0 }, { 0x46580201u, 0 }, { 0x54580100u, 0 },
        { 0x465801FFu, FXP_E_VERSION }, { 0x46580202u, FXP_E_VERSION }, { 0x54580101u, FXP_E_VERSION },
        { 0xFFFF0200u, FXP_E_VERSION }, { 0, FXP_E_VERSION } };
    for (unsigned i = 0; i < sizeof vers / sizeof vers[0]; i++){
        build(&b, vers[i].v, lits, 2, p.w, p.n, p.nins);
        CHECK(load_words(b.w, b.n) == vers[i].err, "version 0x%08x: %s", vers[i].v, fxp_err_name(load_words(b.w, b.n)));
    }
    build(&b, 0x46580200u, lits, 2, p.w, p.n, p.nins);
    static uint32_t good[4096];
    uint32_t n = b.n; memcpy(good, b.w, 4 * n);
    CHECK(load_words(good, n) == FXP_OK, "the good program");
    CHECK(load_words(good, 0) == FXP_E_SCAN && load_words(good, 1) == FXP_E_SCAN, "no comment before the end: scan");
    /* the comment search: CLIT's FOURCC changed, FXLC's changed, an end token before them */
    static uint32_t w[4096];
    memcpy(w, good, 4 * n); w[11] = 0x54494C44u;
    CHECK(load_words(w, n) == FXP_E_NOCLIT, "no CLIT");
    memcpy(w, good, 4 * n); w[18] = 0x434C5847u;
    CHECK(load_words(w, n) == FXP_E_NOFXLC, "no FXLC");
    memcpy(w, good, 4 * n); w[1] = 0x0000FFFFu;
    CHECK(load_words(w, n) == FXP_E_NOFXLC, "an end token first: FXLC is not found");
    /* skipped by the search: a token with bit 31, a comment of length 1 holding the FOURCC, an instruction whose
     * length field covers a fake comment (version >= 2.0) */
    uint32_t lead[3][4] = { { 0x8000FFFEu, 0, 0, 0 }, { 0x0001FFFEu, 0x434C5846u, 0, 0 }, { 0x02000001u, 0x0004FFFEu, 0x434C5846u, 0 } };
    uint32_t nlead[3] = { 1, 2, 3 };
    for (int k = 0; k < 3; k++){
        w[0] = good[0];
        memcpy(w + 1, lead[k], 4 * nlead[k]);
        memcpy(w + 1 + nlead[k], good + 1, 4 * (n - 1));
        CHECK(load_words(w, n + nlead[k]) == FXP_OK, "lead %d skipped by the search", k);
    }
    /* version 1.x: an instruction's length is not used; def takes 6 dwords */
    {
        uint32_t v[16] = { 0x46580100u };
        CHECK(load_words(v, 1) == FXP_E_VERSION, "1.0 is not a preshader version");
    }
    /* CLIT count 0: D3DX divides by 1, the literal is the 8 bytes after the count (the FXLC comment's start) */
    {
        static prog q; memset(&q, 0, sizeof q);
        ins1(&q, 0x100, 2, 0, 1, 5, 4, 0);
        static run_out r;
        CHECK(run(&q, NULL, 0, NULL, 64, &r) == FXP_OK, "count 0 runs");
        EQ64(r.o[0], (uint64_t)0x434C5846u << 32 | (0xFFFEu | (4 + q.n) << 16), "count 0: literal 0 is the FXLC token and FOURCC");
        CHECK(r.o[1] == ((uint64_t)q.w[0] << 32 | 1u), "count 0: component 1 runs on into FXLC");
    }
    /* operands: input tables, index tables, output tables and flags */
    for (uint32_t t = 0; t <= 10; t++){
        static prog q; memset(&q, 0, sizeof q);
        ins1(&q, 0x100, 1, 0, t, 0, 4, 0);
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        int want = t == 0 || t > 9 ? FXP_E_OPERAND : FXP_OK;          /* 8 and 9 load: they matter at run time */
        CHECK(load_words(b.w, b.n) == want, "input table %u: %s", t, fxp_err_name(load_words(b.w, b.n)));
        memset(&q, 0, sizeof q);
        op_begin(&q, 0x100, 1, 0, 1); in_idx(&q, t, 0, 2, 0); out_reg(&q, 4, 0);
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        CHECK(load_words(b.w, b.n) == (t == 9 ? FXP_E_OPERAND : FXP_OK), "index table %u", t);
        memset(&q, 0, sizeof q);
        ins1(&q, 0x100, 1, 0, 2, 0, t, 0);
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        CHECK(load_words(b.w, b.n) == (t >= 4 && t <= 7 ? FXP_OK : FXP_E_OPERAND), "output table %u", t);
    }
    {
        static prog q; memset(&q, 0, sizeof q);
        op_begin(&q, 0x100, 1, 0, 1); in_reg(&q, 2, 0); in_idx(&q, 2, 0, 4, 0);       /* the output has a flag 1 */
        q.w[q.n - 5] = 1;
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        CHECK(load_words(b.w, b.n) == FXP_E_OPERAND, "an output with an index");
        memset(&q, 0, sizeof q);
        op_begin(&q, 0x70E, 1, 0, 257);
        for (int k = 0; k < 257; k++) in_reg(&q, 2, 0);
        out_reg(&q, 4, 0);
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        CHECK(load_words(b.w, b.n) == FXP_OK, "257 inputs load (the run stops, test_slots)");
        memset(&q, 0, sizeof q);
        ins1(&q, 0x502, 3, 0, 2, 0, 4, 0);
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        CHECK(load_words(b.w, b.n) == FXP_OK, "noise loads (the run stops, test_slots)");
        memset(&q, 0, sizeof q);
        op_begin(&q, 0x000, 1, 0, 0); out_reg(&q, 4, 0);
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        CHECK(load_words(b.w, b.n) == FXP_OK, "nop with no input");
    }
    /* the ends: every truncation of the instruction stream fails, and the output's last pair must end before
     * the blob's end (jae) where an input's may end on it (ja) */
    {
        static prog q; memset(&q, 0, sizeof q);
        ins2(&q, 0x204, 4, 0, 2, 0, 1, 1, 4, 0);
        build(&b, 0x46580200u, lits, 2, q.w, q.n, q.nins);
        uint32_t fxlc_end = b.n - 3;                    /* without the trailer and the end token */
        int bad = 0;
        for (uint32_t k = 20; k < fxlc_end; k++) bad += load_words(b.w, k) != FXP_E_OPERAND;
        CHECK(!bad, "%d truncations inside the instruction did not fail", bad);
        CHECK(load_words(b.w, fxlc_end) == FXP_E_OPERAND, "the output ends on the blob's end: refused");
        static uint8_t bytes[4 * 4096];
        memcpy(bytes, b.w, 4 * fxlc_end);
        fxp_prog *fp = NULL;
        CHECK(fxp_load(&fp, bytes, 4 * fxlc_end + 1) == FXP_OK, "one byte after it: loaded");
        fxp_free(fp);
    }
}

/* ======================= the interpreter (§2.4, §3) ======================= */
/* where D3DX reads through a slot, and only there (§3.3, X 0x4E8BF6..0x4E9127): a slot nothing set is stale
 * only when it is read; tables 8 and 9; the 257th input; noise; log's zero test before its operand */
static void test_slots(uint64_t *c){
    static run_out r;
    static prog p;
#define NEWP() memset(&p, 0, sizeof p)
    for (int i = 0; i < 64; i++) c[i] = D((double)i);
    c[5] = D(-1.0);
    /* cmp with two inputs reads slot 2 only where input 0 is negative or NaN (0x4E90E5..0x4E9127) */
    NEWP(); op_begin(&p, 0x300, 3, 0, 2); in_reg(&p, 2, 1); in_reg(&p, 2, 10); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "cmp, 2 inputs, input 0 >= 0: slot 2 is not read");
    EQ64(r.o[0], D(10.0), "cmp picks slot 1"); EQ64(r.o[2], D(12.0), "cmp picks slot 1, component 2");
    NEWP(); op_begin(&p, 0x300, 3, 0, 2); in_reg(&p, 2, 3); in_reg(&p, 2, 10); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "cmp, 2 inputs, input 0 negative at component 2: stale");
    EQ64(r.o[1], D(11.0), "the components before it are written");
    /* no component, no read: an opcode with fewer inputs than it reads and n = 0 runs */
    NEWP(); ins1(&p, 0x204, 0, 0, 2, 0, 4, 0); op_begin(&p, 0x100, 0, 0, 0); out_reg(&p, 4, 1);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "add and mov of 0 components with missing inputs");
    /* log: component k's zero test reads slot k's component 0; input 0 at k is read only when that is not 0 */
    NEWP(); op_begin(&p, 0x70E, 1, 0, 2); in_reg(&p, 2, 1); in_reg(&p, 2, 0); out_reg(&p, 7, 9);
    ins1(&p, 0x106, 2, 0, 2, 63, 4, 0);                 /* slot 1 = c[0] = 0: component 1 (c[64]) is not read */
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "log: the zero test first, then the operand");
    double l63; memcpy(&l63, &r.o[0], 8);
    CHECK(fabs(l63 - log2(63.0)) < 1e-14, "log2(63): %.17g", l63); EQ64(r.o[1], 0, "log component 1: zero test");
    /* log of +-0 behind a non-zero test is fyl2x(+0) = -inf; of a NaN, the NaN without its sign (fabs).  Component
     * 0's test reads slot 0, the operand itself, so these sit at components 1.. */
    c[20] = D(1.0); c[21] = 0; c[22] = 0x8000000000000000ull; c[23] = 0xFFFC000000ABCDEFull; c[24] = 0x7FF0000000000001ull;
    NEWP(); op_begin(&p, 0x70E, 1, 0, 8); for (int k = 0; k < 8; k++) in_reg(&p, 2, 1); out_reg(&p, 7, 9);
    ins1(&p, 0x106, 5, 0, 2, 20, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "log of zeros and NaNs");
    EQ64(r.o[0], 0, "log(1) = 0");
    EQ64(r.o[1], 0xFFF0000000000000ull, "log(+0) = -inf"); EQ64(r.o[2], 0xFFF0000000000000ull, "log(-0) = -inf");
    EQ64(r.o[3], 0x7FFC000000ABCDEFull, "log(-NaN): fabs clears the sign");
    EQ64(r.o[4], 0x7FF8000000000001ull, "log(SNaN): quiet");
    for (int i = 20; i < 25; i++) c[i] = D((double)i);
    /* table 8: the slot keeps what the instruction before wrote there; with nothing there, stale */
    NEWP(); ins1(&p, 0x100, 1, 0, 2, 7, 4, 0); ins1(&p, 0x100, 2, 0, 8, 0, 4, 1);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "table 8 after a mov");
    EQ64(r.o[1], D(7.0), "table 8 reads what slot 0 pointed at (c[7])"); EQ64(r.o[2], D(8.0), "and on");
    NEWP(); ins1(&p, 0x100, 1, 0, 8, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "table 8 first: stale");
    /* table 9: address 0 in both D3DX paths; read only when there is a component */
    NEWP(); ins1(&p, 0x100, 1, 0, 9, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_RANGE, "table 9 read");
    NEWP(); ins1(&p, 0x100, 0, 0, 9, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "table 9, no component");
    /* the 257th input stops the run before the opcode */
    NEWP(); ins1(&p, 0x100, 1, 0, 2, 3, 4, 0);
    op_begin(&p, 0x70E, 1, 0, 257); for (int k = 0; k < 257; k++) in_reg(&p, 2, 0); out_reg(&p, 4, 1);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_INPUTS, "257 inputs");
    EQ64(r.o[0], D(3.0), "the instruction before ran"); EQ64(r.o[1], 0, "the dotswiz did not");
    NEWP(); op_begin(&p, 0x70E, 1, 0, 256); for (int k = 0; k < 256; k++) in_reg(&p, 2, 1); out_reg(&p, 4, 1);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "256 inputs");
    EQ64(r.o[1], D(128.0), "128 products of 1");
    /* noise stops the run after reading its inputs; the scalar forms do nothing */
    NEWP(); ins1(&p, 0x502, 3, 0, 2, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_NOISE, "noise");
    NEWP(); ins1(&p, 0x502, 3, 0, 2, 62, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_RANGE, "noise reads its 3 components first: c[64] is outside");
    NEWP(); op_begin(&p, 0x711, 1, 0, 0); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_NOISE, "noise of no input");
    NEWP(); ins1(&p, 0x502, 3, 1, 2, 0, 4, 0); op_begin(&p, 0x711, 1, 1, 0); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "scalar noise forms: nothing");
    /* 0x502 reads min(n, 3) components, 0x711 min(k, 3) inputs (0x4E8771, 0x4E87C0; review R2-05): the 4th is out
     * of reach (c[64]; table 9, address 0), and reading it would be a range stop */
    NEWP(); ins1(&p, 0x502, 4, 0, 2, 61, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_NOISE, "noise of 4 components reads 3: c[61..63]");
    NEWP(); ins1(&p, 0x502, 8, 0, 2, 61, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_NOISE, "noise of 8 components reads 3");
    NEWP(); op_begin(&p, 0x711, 1, 0, 4); in_reg(&p, 2, 1); in_reg(&p, 2, 2); in_reg(&p, 2, 3); in_reg(&p, 9, 0);
    out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_NOISE, "0x711 with 4 inputs reads 3: the 4th (table 9) is not read");
    NEWP(); op_begin(&p, 0x711, 1, 0, 3); in_reg(&p, 2, 1); in_reg(&p, 2, 2); in_reg(&p, 9, 0); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_RANGE, "0x711 with 3 inputs reads the 3rd (table 9)");
    /* temporaries that start unknown (fxp_regs.temps_unknown, as in the effect path; review R2-09): a read of one no
     * instruction of the run wrote stops (D3DX reads its stack), at the read: directly, as an index, at component 1,
     * through cmp's choice; one the run wrote is read; dot writes its 0 before it reads its output */
    g_unknown = 1;
    NEWP(); ins1(&p, 0x100, 1, 0, 7, 5, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "an unknown temporary read");
    NEWP(); ins1(&p, 0x100, 2, 0, 2, 1, 7, 5); ins1(&p, 0x100, 2, 0, 7, 5, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "temporaries written, then read");
    EQ64(r.o[1], D(2.0), "the value written");
    NEWP(); ins1(&p, 0x100, 1, 0, 2, 1, 7, 5); ins1(&p, 0x100, 2, 0, 7, 5, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "component 1 of a temporary not written");
    EQ64(r.o[0], D(1.0), "component 0 is copied first");
    NEWP(); op_begin(&p, 0x100, 1, 0, 1); in_idx(&p, 7, 3, 2, 1); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "an index register in an unknown temporary");
    NEWP(); op_begin(&p, 0x300, 2, 0, 3); in_reg(&p, 2, 1); in_reg(&p, 2, 4); in_reg(&p, 7, 50); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "cmp does not read the input it does not pick");
    NEWP(); op_begin(&p, 0x300, 2, 0, 3); in_reg(&p, 2, 4); in_reg(&p, 2, 5); in_reg(&p, 7, 50); out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "cmp picks the unknown temporary (c[5] < 0)");
    NEWP(); ins2(&p, 0x500, 2, 0, 2, 1, 2, 2, 7, 10); ins1(&p, 0x100, 1, 0, 7, 10, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "dot into a temporary writes its 0 first");
    EQ64(r.o[0], D(1.0 * 2.0 + 2.0 * 3.0), "the dot");
    g_unknown = 0;
    NEWP(); ins1(&p, 0x100, 1, 0, 7, 5, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "known temporaries: the caller's (0)");
    /* the precision from the x87 control word: PC 0, 2, 3 with rounding to nearest; PC 1 is reserved, and another
     * rounding control is not modelled (review R2-09) */
    CHECK(fxp_pc_from_cw(0x007F) == 24 && fxp_pc_from_cw(0x027F) == 53 && fxp_pc_from_cw(0x037F) == 64, "PC 0, 2, 3");
    CHECK(fxp_pc_from_cw(0x137F) == 64 && fxp_pc_from_cw(0x0000) == 24, "the other bits do not matter");
    CHECK(!fxp_pc_from_cw(0x017F), "PC 1 (reserved): refused");
    CHECK(!fxp_pc_from_cw(0x047F) && !fxp_pc_from_cw(0x0A7F) && !fxp_pc_from_cw(0x0F7F), "RC down, up, chop: refused");
    NEWP(); ins1(&p, 0x100, 1, 0, 2, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, fxp_pc_from_cw(0x0C7F), &r) == FXP_E_ARG, "a refused control word's pc is refused");
    /* the precision must be one of the three */
    NEWP(); ins1(&p, 0x100, 1, 0, 2, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 0, &r) == FXP_E_ARG && run(&p, NULL, 0, c, 2, &r) == FXP_E_ARG, "pc 0 and 2 refused");
#undef NEWP
}

static void test_ops(void){
    static uint64_t c[64];
    for (int i = 0; i < 64; i++) c[i] = D((double)i);
    static run_out r;
    static prog p;
#define NEWP() memset(&p, 0, sizeof p)
    /* mov copies bits, a signalling NaN too; neg, min and cmp go through the x87 and make it quiet */
    c[10] = 0x7FF0000000000001ull; c[11] = 0x8000000000000000ull; c[12] = D(-3.0); c[13] = NAN_D;
    NEWP(); ins1(&p, 0x100, 4, 0, 2, 10, 4, 0); ins1(&p, 0x101, 4, 0, 2, 10, 4, 4);
    ins2(&p, 0x200, 4, 0, 2, 10, 2, 1, 4, 8);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "mov, neg, min");
    EQ64(r.o[0], 0x7FF0000000000001ull, "mov keeps a signalling NaN");
    EQ64(r.o[4], 0xFFF8000000000001ull, "neg makes it quiet");
    EQ64(r.o[5], 0, "neg -0");
    EQ64(r.o[8], D(1.0), "min(NaN, 1): the second");
    EQ64(r.o[9], 0x8000000000000000ull, "min(-0, 2)");
    EQ64(r.o[10], D(-3.0), "min(-3, 3)");
    EQ64(r.o[11], D(4.0), "min(NaN, 4): the second");
    /* lt, ge, max, cmp with NaN and -0 */
    NEWP(); ins2(&p, 0x202, 4, 0, 2, 10, 2, 1, 4, 0); ins2(&p, 0x203, 4, 0, 2, 10, 2, 1, 4, 4);
    ins2(&p, 0x201, 4, 0, 2, 10, 2, 1, 4, 8);
    op_begin(&p, 0x300, 4, 0, 3); in_reg(&p, 2, 10); in_reg(&p, 2, 20); in_reg(&p, 2, 30); out_reg(&p, 4, 12);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "lt ge max cmp");
    EQ64(r.o[0], 0, "lt(NaN, 1)"); EQ64(r.o[1], ONE, "lt(-0, 2)"); EQ64(r.o[2], ONE, "lt(-3, 3)"); EQ64(r.o[3], 0, "lt(NaN, 4)");
    EQ64(r.o[4], 0, "ge(NaN, 1)"); EQ64(r.o[5], 0, "ge(-0, 2)"); EQ64(r.o[6], 0, "ge(-3, 3)");
    EQ64(r.o[8], D(1.0), "max(NaN, 1): the second"); EQ64(r.o[10], D(3.0), "max(-3, 3)");
    EQ64(r.o[12], D(30.0), "cmp(SNaN): the third");
    EQ64(r.o[13], D(21.0), "cmp(-0) >= 0: the second");
    EQ64(r.o[14], D(32.0), "cmp(-3): the third");
    EQ64(r.o[15], D(33.0), "cmp(NaN): the third");
    /* rcp, rsq, frc */
    c[14] = D(-2.5); c[15] = 0x8000000000000000ull; c[16] = D(4.0); c[17] = 0xFFF0000000000000ull;
    NEWP(); ins1(&p, 0x103, 4, 0, 2, 14, 4, 0); ins1(&p, 0x107, 4, 0, 2, 14, 4, 4); ins1(&p, 0x104, 4, 0, 2, 14, 4, 8);
    CHECK(run(&p, NULL, 0, c, 53, &r) == FXP_OK, "rcp rsq frc");
    EQ64(r.o[0], D(-0.4), "1/-2.5"); EQ64(r.o[1], 0xFFF0000000000000ull, "1/-0"); EQ64(r.o[3], 0x8000000000000000ull, "1/-inf");
    EQ64(r.o[5], 0x7FF0000000000000ull, "rsq(-0)"); EQ64(r.o[6], D(0.5), "rsq(4)"); EQ64(r.o[7], 0, "rsq(-inf)");
    EQ64(r.o[8], D(0.5), "frc(-2.5)"); EQ64(r.o[9], 0, "frc(-0) = +0"); EQ64(r.o[10], 0, "frc(4)"); EQ64(r.o[11], NAN_D, "frc(-inf)");
    /* add and mul at 24 bits: rounded before the store */
    c[18] = D(1.0 / 3.0); c[19] = D(0.1);
    NEWP(); ins2(&p, 0x204, 2, 0, 2, 18, 2, 0, 4, 0); ins2(&p, 0x205, 2, 0, 2, 18, 2, 3, 4, 2);
    CHECK(run(&p, NULL, 0, c, 24, &r) == FXP_OK, "add mul at 24");
    EQ64(r.o[0], 0x3FD5555560000000ull, "1/3 + 0 at 24 bits");
    EQ64(r.o[3], fxp_x87_mul(D(0.1), D(4.0), 24), "0.1 * 4 at 24 bits");
    /* scalar forms: input 0 at component 0; a unary opcode with the scalar flag does nothing */
    NEWP(); ins2(&p, 0x204, 3, 1, 2, 2, 2, 4, 4, 0); ins1(&p, 0x100, 3, 1, 2, 9, 4, 4);
    ins2(&p, 0x500, 3, 1, 2, 2, 2, 4, 4, 8);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "scalar");
    EQ64(r.o[0], D(6.0), "add.s 2 + 4"); EQ64(r.o[1], D(7.0), "add.s 2 + 5"); EQ64(r.o[2], D(8.0), "add.s 2 + 6");
    EQ64(r.o[4], 0, "mov.s: nothing");
    EQ64(r.o[8], D(30.0), "dot.s: 2*4 + 2*5 + 2*6");
    /* dot writes its 0 first, then the sum as it goes: with the output on input 0 */
    for (int i = 0; i < 8; i++) c[i] = D((double)(i + 1));
    NEWP(); ins1(&p, 0x100, 8, 0, 2, 0, 7, 0);
    ins2(&p, 0x500, 3, 0, 7, 0, 7, 4, 7, 0);               /* r0 = 0, then r0 + r0*r4, r0 + r1*r5, ... */
    ins1(&p, 0x100, 1, 0, 7, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "dot aliasing");
    EQ64(r.o[0], D(0 * 5 + 2 * 6 + 3 * 7), "dot reads its own 0");
    /* which operand is loaded (fld) and which is the memory operand (§3.2; review R2-04): with a quiet NaN against a
     * signalling one, the quiet register NaN wins over a signalling memory NaN, while a signalling register NaN is
     * loaded quiet and the larger significand of two quiet NaNs wins (§3.1).  add, mul and dot load input 0, their
     * scalar forms input 1 (b[j]), dotswiz its first half */
    c[20] = 0x7FF8000000000005ull; c[24] = 0x7FF0000000000009ull;
    NEWP(); ins2(&p, 0x204, 1, 0, 2, 20, 2, 24, 4, 0); ins2(&p, 0x205, 1, 0, 2, 20, 2, 24, 4, 1);
    ins2(&p, 0x500, 1, 0, 2, 20, 2, 24, 4, 2);
    ins2(&p, 0x204, 1, 1, 2, 20, 2, 24, 4, 3); ins2(&p, 0x205, 1, 1, 2, 20, 2, 24, 4, 4);
    ins2(&p, 0x500, 1, 1, 2, 20, 2, 24, 4, 5);
    ins2(&p, 0x204, 1, 1, 2, 24, 2, 20, 4, 6); ins2(&p, 0x500, 1, 1, 2, 24, 2, 20, 4, 7);
    op_begin(&p, 0x70E, 1, 0, 2); in_reg(&p, 2, 20); in_reg(&p, 2, 24); out_reg(&p, 4, 8);
    op_begin(&p, 0x70E, 1, 0, 2); in_reg(&p, 2, 24); in_reg(&p, 2, 20); out_reg(&p, 4, 9);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "NaN operand roles");
    EQ64(r.o[0], 0x7FF8000000000005ull, "add: fld a (QNaN 5), fadd b (SNaN 9): the register");
    EQ64(r.o[1], 0x7FF8000000000005ull, "mul: the same");
    EQ64(r.o[2], 0x7FF8000000000005ull, "dot: fld a, fmul b: the register");
    EQ64(r.o[3], 0x7FF8000000000009ull, "add.s: fld b[j] (SNaN 9, loaded quiet), fadd a[0] (QNaN 5): the larger");
    EQ64(r.o[4], 0x7FF8000000000009ull, "mul.s: the same");
    EQ64(r.o[5], 0x7FF8000000000009ull, "dot.s: fld b[j], fmul a[0]: the larger");
    EQ64(r.o[6], 0x7FF8000000000005ull, "add.s: fld b[j] (QNaN 5), fadd a[0] (SNaN 9): the register");
    EQ64(r.o[7], 0x7FF8000000000005ull, "dot.s: the same");
    EQ64(r.o[8], 0x7FF8000000000005ull, "dotswiz: fld input 0 (QNaN 5), fmul input 1 (SNaN 9): the register");
    EQ64(r.o[9], 0x7FF8000000000009ull, "dotswiz: fld input 0 (SNaN 9, quiet), fmul input 1 (QNaN 5): the larger");
    c[20] = D(20.0); c[24] = D(24.0);
    /* dotswiz: component 0 of input k times input h + k */
    NEWP(); op_begin(&p, 0x70E, 3, 0, 6);
    for (int k = 0; k < 6; k++) in_reg(&p, 2, (uint32_t)k);
    out_reg(&p, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "dotswiz6");
    EQ64(r.o[0], D(1 * 4 + 2 * 5 + 3 * 6), "dotswiz6"); EQ64(r.o[1], 0, "dotswiz writes component 0 only");
    /* opcodes that do nothing: div, movc, 0x102, nop, an unknown one */
    NEWP(); ins2(&p, 0x208, 2, 0, 2, 1, 2, 2, 4, 0); op_begin(&p, 0x301, 2, 0, 3); in_reg(&p, 2, 0); in_reg(&p, 2, 1);
    in_reg(&p, 2, 2); out_reg(&p, 4, 2); ins1(&p, 0x102, 1, 0, 2, 1, 4, 4); ins1(&p, 0x7FF, 1, 0, 2, 1, 4, 5);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "no-ops");
    CHECK(!r.o[0] && !r.o[1] && !r.o[2] && !r.o[3] && !r.o[4] && !r.o[5], "div, movc, 0x102, 0x7FF write nothing");
    /* relative addressing: offset + 4 * trunc(index), from c and from a literal; NaN gives 0 */
    for (int i = 0; i < 64; i++) c[i] = D((double)i);
    c[60] = D(2.7); c[61] = NAN_D; c[62] = D(-1.0);
    uint64_t lits[3] = { D(1.9), D(100.0), D(7.0) };
    NEWP(); op_begin(&p, 0x100, 1, 0, 1); in_idx(&p, 2, 60, 2, 1); out_reg(&p, 4, 0);
    op_begin(&p, 0x100, 1, 0, 1); in_idx(&p, 2, 61, 2, 1); out_reg(&p, 4, 1);
    op_begin(&p, 0x100, 1, 0, 1); in_idx(&p, 2, 62, 2, 9); out_reg(&p, 4, 2);
    op_begin(&p, 0x100, 1, 0, 1); in_idx(&p, 1, 3, 2, 3); out_reg(&p, 4, 3);       /* literal 3 mod 3 = 0: 1.9 -> 1 */
    op_begin(&p, 0x100, 1, 0, 1); in_idx(&p, 0, 2, 2, 1); out_reg(&p, 4, 4);       /* table 0: the offset itself */
    op_begin(&p, 0x100, 2, 0, 1); in_reg(&p, 1, 5); out_reg(&p, 4, 5);              /* literal 5 mod 3 = 2, then on */
    op_begin(&p, 0x100, 1, 0, 1); in_reg(&p, 2, 70); out_reg(&p, 4, 7);             /* c[70 & 63] */
    CHECK(run(&p, lits, 3, c, 64, &r) == FXP_OK, "addressing");
    EQ64(r.o[0], D(9.0), "c[1 + 4*2]"); EQ64(r.o[1], D(1.0), "a NaN index is 0");
    EQ64(r.o[2], D(5.0), "c[9 - 4]"); EQ64(r.o[3], D(7.0), "c[3 + 4*1]"); EQ64(r.o[4], D(9.0), "c[1 + 4*2] by table 0");
    EQ64(r.o[5], D(7.0), "literal 2"); EQ64(r.o[7], D(6.0), "c[70 & 63]");
    EQ64(r.o[6], (uint64_t)0x434C5846u << 32 | (0xFFFEu | (4 + p.n) << 16), "a literal past the table reads on (FXLC)");
    /* temporaries wrap at 256 elements; reading past them, or c past its size, is refused */
    NEWP(); ins1(&p, 0x100, 1, 0, 2, 5, 7, 300); ins1(&p, 0x100, 1, 0, 7, 44, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "temporaries & 0xFF");
    EQ64(r.o[0], D(5.0), "r[300 & 255]");
    NEWP(); ins1(&p, 0x100, 2, 0, 2, 0, 7, 255);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_RANGE, "a temporary past 255");
    NEWP(); ins1(&p, 0x100, 2, 0, 2, 63, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_RANGE, "c past its 64");
    /* the input slots (§3.3): log's zero test reads slot k; an opcode reads more slots than it has inputs */
    NEWP(); op_begin(&p, 0x70E, 1, 0, 8);
    for (int k = 0; k < 8; k++) in_reg(&p, 2, (uint32_t)(k == 1 ? 0 : 8 + k));
    out_reg(&p, 7, 100);
    ins1(&p, 0x106, 3, 0, 2, 4, 4, 0);                  /* slot 1 is the dotswiz's c[0] = 0: component 1 is 0 */
    ins1(&p, 0x204, 2, 0, 2, 20, 4, 4);                 /* one input: in1 is slot 1, still c[0] */
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "slots");
    double l0, l2; memcpy(&l0, &r.o[0], 8); memcpy(&l2, &r.o[2], 8);
    CHECK(fabs(l0 - 2.0) < 1e-15 && fabs(l2 - log2(6.0)) < 1e-15, "log2(4), log2(6): %.17g %.17g", l0, l2);
    EQ64(r.o[1], 0, "log component 1: slot 1 reads 0");
    EQ64(r.o[4], D(20.0), "add with one input: in1 is slot 1 (c[0])"); EQ64(r.o[5], D(22.0), "c[21] + c[1]");
    NEWP(); ins1(&p, 0x204, 1, 0, 2, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "a slot no instruction set");
    NEWP(); ins1(&p, 0x106, 2, 0, 2, 4, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_E_STALE, "log with 2 components and 1 input");
    test_slots(c);
    /* the approximate class: fsin keeps an operand past 2^63, an infinity is invalid */
    c[0] = D(1e30); c[1] = 0x7FF0000000000000ull; c[2] = D(-9223372036854775808.0); c[3] = 0;
    NEWP(); ins1(&p, 0x108, 4, 0, 2, 0, 4, 0);
    CHECK(run(&p, NULL, 0, c, 64, &r) == FXP_OK, "sin");
    EQ64(r.o[0], D(1e30), "sin past 2^63: the operand"); EQ64(r.o[1], NAN_D, "sin(inf): invalid");
    CHECK(r.o[2] != c[2], "sin(-2^63) is computed"); EQ64(r.o[3], 0, "sin(0)");
#undef NEWP
}

/* ======================= the effect path (§4) on effect files ======================= */
static uint8_t *slurp(const char *path, uint32_t *size){
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (!b || (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n)){ fclose(f); free(b); return NULL; }
    fclose(f); *size = (uint32_t)n; return b;
}
static uint64_t g_rs = 0x853C49E6748FEA9Bull;
static uint32_t rnd(void){ g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 32); }

static unsigned long n_pres, n_ins, n_mov, n_states, n_checked;
/* a mov-only preshader whose every output component reads one c component: the state's dword k must be the
 * input converted (§4.1 then §4.2) */
static void check_state(const fxm_effect *m, fxp_effect *x, const fx2_state *s, const char *where, int pc){
    fxm_source src = fxm_state_source(m, s);
    if (src.src != FXM_S_PRESHADER) return;
    n_states++;
    uint32_t v[64], n = 0;
    int err = fxp_state_value(x, s, pc, v, 64, &n);
    CHECK(!err, "%s: %s", where, fxp_err_name(err));
    if (err) return;
    const fxm_prog *pr = src.prog;
    fxp_prog *fp = NULL;
    if (fxp_load(&fp, pr->res->data, pr->res->size)) return;
    /* find, for each output component, the c component a mov writes to it, then which leaf dword that is */
    fx2_pres ps; fx2_pres_read(pr->res->data, pr->res->size, &ps, NULL, NULL);
    const uint8_t *q = pr->res->data + 4 * (ps.fxlc + 2);
    uint32_t ndw = ps.fxlc_len - 1, cnt = fx2_rd32(q), pos = 1;
    uint64_t src_of[64]; uint8_t have[64] = { 0 }; int all_mov = 1;
    for (uint32_t i = 0; i < cnt; i++){
        fx2_ins in; fx2_fxlc_ins(q, ndw, pos, &in);
        if (in.status != FX2_INS_OK){ all_mov = 0; break; }
        fx2_operand o; fx2_fxlc_operand(q, ndw, in.in, &o);
        if (in.opcode != 0x100 || in.scalar || in.nin != 1 || o.flag || in.out.table != 4) all_mov = 0;
        for (uint32_t k = 0; k < in.ncomp && in.out.offset + k < 64; k++){
            have[in.out.offset + k] = 1;
            src_of[in.out.offset + k] = (uint64_t)o.table << 32 | (o.offset + k);
        }
        pos = in.end;
    }
    fxp_free(fp);
    if (!all_mov) return;
    for (uint32_t k = 0; k < n && k < 64; k++){
        if (!have[k]) continue;
        uint32_t table = (uint32_t)(src_of[k] >> 32), off = (uint32_t)src_of[k];
        uint64_t in;
        if (table == 1){                                  /* a literal */
            if (ps.clit == 0) continue;
            const uint8_t *lit = pr->res->data + 4 * (ps.clit + 3);
            uint32_t nl = fx2_rd32(lit - 4);
            if (!nl) continue;
            in = (uint64_t)fx2_rd32(lit + 8 * (off % nl)) | (uint64_t)fx2_rd32(lit + 8 * (off % nl) + 4) << 32;
        } else {                                          /* c: the constant and leaf that hold it */
            uint32_t reg = off / 4, comp = off % 4, found = 0;
            in = 0;
            for (uint32_t ci = 0; ci < pr->nread && !found; ci++){
                const fxm_const *mc = &pr->consts[ci];
                if (mc->why || mc->c.set != 2) continue;
                for (uint32_t l = 0; l < mc->nleaves && !found; l++){
                    const fxm_leaf *lf = &mc->leaves[l];
                    if (reg < lf->reg || reg >= lf->reg + lf->count) continue;
                    const fxm_node *nd = fxm_node_get(m, lf->id);
                    const uint32_t *d = fxm_data(m, lf->id);
                    uint32_t r = reg - lf->reg, at = r * nd->cols + comp;     /* a row, a vector, a scalar */
                    if (nd->cls == FX2_C_SCALAR && comp) continue;
                    in = nd->type == FX2_T_FLOAT ? fxp_f2d(d[at]) : fxp_i2d(d[at]);
                    found = 1;
                }
            }
            if (!found) continue;
        }
        uint32_t want = s->t->type == FX2_T_FLOAT ? fxp_d2f(in) : fxp_ftol(in);
        CHECK(v[k] == want, "%s: dword %u is 0x%08x, the input converted 0x%08x", where, k, v[k], want);
        n_checked++;
    }
}

static int by_name(const void *a, const void *b){ return strcmp(*(char *const *)a, *(char *const *)b); }
static void test_effects(int nfiles, char **files, int mw){
    if (!nfiles) return;
    qsort(files, (size_t)nfiles, sizeof *files, by_name);
    fxm_pool *pool = fxm_pool_new();
    uint8_t **buf = calloc((size_t)nfiles, sizeof *buf);
    fx2_effect *fx = calloc((size_t)nfiles, sizeof *fx);
    fxm_effect **m = calloc((size_t)nfiles, sizeof *m);
    fxp_effect **x = calloc((size_t)nfiles, sizeof *x);
    char err[256];
    for (int i = 0; i < nfiles; i++){
        uint32_t sz;
        if (!(buf[i] = slurp(files[i], &sz))){ fprintf(stderr, "fxpres_test: cannot read %s\n", files[i]); exit(1); }
        if (fx2_parse(&fx[i], buf[i], sz, err, sizeof err) || fxm_effect_new(&m[i], &fx[i], pool, err, sizeof err)){
            fprintf(stderr, "fxpres_test: %s: %s\n", files[i], err); exit(1);
        }
        CHECK(fxp_effect_new(&x[i], m[i]) == FXP_OK, "%s: fxp_effect_new", files[i]);
        uint32_t np; const fxm_prog *pr = fxm_progs(m[i], &np);
        for (uint32_t k = 0; k < np; k++){
            if (pr[k].kind != FXM_P_PRESHADER) continue;
            n_pres++;
            fxp_prog *fp = NULL;
            int e = fxp_load(&fp, pr[k].res->data, pr[k].res->size);
            CHECK(!e, "%s: preshader %u: %s", files[i], k, fxp_err_name(e));
            if (fp){ n_ins += fxp_ninstr(fp); CHECK(!fxp_approx(fp), "%s: preshader %u is approximate", files[i], k); }
            fxp_free(fp);
            fx2_pres ps; fx2_pres_read(pr[k].res->data, pr[k].res->size, &ps, NULL, NULL);
            const uint8_t *q = pr[k].res->data + 4 * (ps.fxlc + 2);
            uint32_t ndw = ps.fxlc_len - 1, cnt = fx2_rd32(q), pos = 1;
            for (uint32_t j = 0; j < cnt; j++){
                fx2_ins in; fx2_fxlc_ins(q, ndw, pos, &in);
                if (in.status != FX2_INS_OK) break;
                n_mov += in.opcode == 0x100;
                pos = in.end;
            }
        }
    }
    /* three rounds of random bits in every numeric parameter, each state checked at each precision */
    for (int round = 0; round < 3; round++){
        for (int i = 0; i < nfiles; i++)
            for (uint32_t id = 1; id <= fxm_nodes(m[i]); id++){
                const fxm_node *nd = fxm_node_get(m[i], id);
                if (nd->kind != FXM_K_PARAM || nd->cls > FX2_C_MATRIX_COLUMNS || !nd->bytes) continue;
                uint32_t w[256], k = nd->bytes / 4 < 256 ? nd->bytes / 4 : 256;
                for (uint32_t j = 0; j < k; j++) w[j] = rnd() & 1 ? rnd() : rnd() % 16;
                if (nd->bytes / 4 <= 256) fxm_set_value(m[i], id, w, nd->bytes, NULL);
            }
        static const int pcs[3] = { 24, 53, 64 };
        for (int i = 0; i < nfiles; i++)
            for (uint32_t t = 0; t < fx[i].ntechs; t++)
                for (uint32_t p = 0; p < fx[i].techs[t].npasses; p++){
                    const fx2_pass *ps = &fx[i].techs[t].passes[p];
                    char where[512];
                    for (uint32_t s = 0; s < ps->nstates; s++){
                        snprintf(where, sizeof where, "%s tech %u pass %u state %u", files[i], t, p, s);
                        check_state(m[i], x[i], &ps->states[s], where, pcs[round]);
                    }
                }
    }
    printf("effects: %d files, %lu preshaders, %lu instructions (%lu mov), %lu pass states evaluated, %lu dwords "
           "equal their input converted\n", nfiles, n_pres, n_ins, n_mov, n_states, n_checked);
    /* the API's refusals (fxpres.h): a pc that is not 24, 53 or 64, a state that is not a preshader's, a short cap */
    {
        int pre = 0, other = 0;
        for (uint32_t t = 0; t < fx[0].ntechs && !(pre && other); t++)
            for (uint32_t p = 0; p < fx[0].techs[t].npasses; p++)
                for (uint32_t k = 0; k < fx[0].techs[t].passes[p].nstates; k++){
                    const fx2_state *st = &fx[0].techs[t].passes[p].states[k];
                    fxm_source src = fxm_state_source(m[0], st);
                    uint32_t v[64], n = 1;
                    if (src.src == FXM_S_PRESHADER && !pre){
                        pre = 1;
                        CHECK(fxp_state_value(x[0], st, 0, v, 64, &n) == FXP_E_ARG && !n, "pc 0: refused");
                        CHECK(fxp_state_value(x[0], st, 3, v, 64, &n) == FXP_E_ARG, "pc 3: refused");
                        CHECK(fxp_state_value(x[0], st, 24, v, 0, &n) == FXP_E_ARG, "cap 0: refused");
                        CHECK(fxp_prog_error(x[0], src.prog) == FXP_OK, "fxp_prog_error of a preshader that loads");
                    } else if (src.src != FXM_S_PRESHADER && !other){
                        other = 1;
                        CHECK(fxp_state_value(x[0], st, 24, v, 64, &n) == FXP_E_ARG, "a state that is not a preshader's");
                        if (src.src == FXM_S_SHADER && src.prog)
                            CHECK(fxp_prog_error(x[0], src.prog) == FXP_E_TYPE, "fxp_prog_error of a shader");
                    }
                }
        CHECK(pre && other, "%s: a preshader state and another one", files[0]);
        CHECK(fxp_prog_error(x[0], NULL) == FXP_E_ARG, "fxp_prog_error of no program");
    }
    /* a preshader D3DX's loader refuses makes D3DXCreateEffect fail (O: E_FAIL): fxp_effect_new refuses the effect */
    {
        uint32_t np; const fxm_prog *pr = fxm_progs(m[0], &np), *pp = NULL;
        for (uint32_t k = 0; k < np && !pp; k++) if (pr[k].kind == FXM_P_PRESHADER) pp = &pr[k];
        uint32_t sz; uint8_t *copy = slurp(files[0], &sz);
        if (pp && copy && pp->res->data >= buf[0] && pp->res->data < buf[0] + sz){
            size_t at = (size_t)(pp->res->data - buf[0]);
            copy[at + 1] = 3;                               /* version 0x46580300 */
            fx2_effect fc; fxm_effect *mc = NULL; fxp_effect *xc = (fxp_effect *)1;
            if (!fx2_parse(&fc, copy, sz, err, sizeof err) && !fxm_effect_new(&mc, &fc, NULL, err, sizeof err)){
                CHECK(fxp_effect_new(&xc, mc) == FXP_E_VERSION && !xc, "a refused preshader refuses the effect");
                fxm_effect_free(mc, NULL, NULL);
            } else CHECK(0, "the copy with a bad preshader version parses: %s", err);
            fx2_free(&fc);
        } else CHECK(0, "%s: a preshader inside the file", files[0]);
        free(copy);
    }
    /* D3DX's input table is mask + 1 doubles and zeroed (0x4FB467..0x4FB4D8, §2.5; review R2-01): copies of the effects
     * whose preshaders read past their float registers (c[4 fend], or c[0] with no constant at all) evaluate, and
     * what they read there is 0 (the oracle's pad rounds check that against D3DX) */
    {
        unsigned long npatched = 0, nstates = 0, nzero = 0;
        for (int i = 0; i < nfiles; i++){
            uint32_t sz; uint8_t *copy = slurp(files[i], &sz);
            uint32_t np; const fxm_prog *pr = fxm_progs(m[i], &np);
            static uint32_t at_res[4096], first_out[4096]; uint32_t nat = 0;
            for (uint32_t k = 0; k < np && copy; k++){
                if (pr[k].kind != FXM_P_PRESHADER) continue;
                uint32_t fend = 0, bend = 0, iend = 0;
                for (uint32_t j = 0; j < pr[k].nread; j++){
                    const fx2_const *c = &pr[k].consts[j].c;
                    uint32_t end = c->reg + c->count, *t = c->set == 2 ? &fend : c->set == 0 ? &bend : c->set == 1 ? &iend : NULL;
                    if (t && end > *t) *t = end;
                }
                uint32_t tot = bend + 4 * (fend + iend), cmask = tot ? tot - 1 : 0;
                cmask |= cmask >> 1; cmask |= cmask >> 2; cmask |= cmask >> 4; cmask |= cmask >> 8; cmask |= cmask >> 16;
                fx2_pres ps; fx2_pres_read(pr[k].res->data, pr[k].res->size, &ps, NULL, NULL);
                const uint8_t *q = pr[k].res->data + 4 * (ps.fxlc + 2);
                uint32_t ndw = ps.fxlc_len - 1, cnt = fx2_rd32(q), pos = 1;
                for (uint32_t j = 0; j < cnt; j++){
                    fx2_ins in; fx2_fxlc_ins(q, ndw, pos, &in);
                    if (in.status != FX2_INS_OK) break;
                    pos = in.end;
                    fx2_operand o; fx2_fxlc_operand(q, ndw, in.in, &o);
                    if (in.opcode != 0x100 || in.scalar || in.nin != 1 || o.flag || 4 * fend + in.ncomp > cmask + 1) continue;
                    if (fend ? o.table != 2 : o.table != 1) continue;
                    size_t at = (size_t)(q - buf[i]) + 4 * ((size_t)in.in + 1);       /* the register pair */
                    uint32_t t2 = 2, off = 4 * fend;
                    memcpy(copy + at, &t2, 4); memcpy(copy + at + 4, &off, 4);
                    if (nat < 4096){ at_res[nat] = (uint32_t)(pr[k].res->data - buf[i]); first_out[nat++] = in.out.offset; }
                    npatched++;
                    break;
                }
            }
            fx2_effect fc; fxm_effect *mc = NULL; fxp_effect *xc = NULL;
            if (copy && !fx2_parse(&fc, copy, sz, err, sizeof err)){
                if (!fxm_effect_new(&mc, &fc, NULL, err, sizeof err) && fxp_effect_new(&xc, mc) == FXP_OK){
                    for (uint32_t t = 0; t < fc.ntechs; t++)
                        for (uint32_t p = 0; p < fc.techs[t].npasses; p++)
                            for (uint32_t k = 0; k < fc.techs[t].passes[p].nstates; k++){
                                const fx2_state *st = &fc.techs[t].passes[p].states[k];
                                fxm_source src = fxm_state_source(mc, st);
                                if (src.src != FXM_S_PRESHADER) continue;
                                uint32_t v[64], n = 0, off = (uint32_t)(src.prog->res->data - copy), j = 0;
                                while (j < nat && at_res[j] != off) j++;
                                if (j == nat) continue;
                                int e = fxp_state_value(xc, st, 24, v, 64, &n);
                                CHECK(e == FXP_OK, "%s tech %u pass %u state %u reads the padding: %s", files[i], t, p, k,
                                      fxp_err_name(e));
                                nstates++;
                                if (!e && first_out[j] < n) nzero += v[first_out[j]] == 0;
                            }
                } else CHECK(0, "%s: the padded copy makes a model", files[i]);
                fxp_effect_free(xc); fxm_effect_free(mc, NULL, NULL); fx2_free(&fc);
            } else CHECK(0, "%s: the padded copy parses", files[i]);
            free(copy);
        }
        CHECK(!mw || npatched == 150, "150 of MW's preshaders read past their float registers, not %lu", npatched);
        CHECK(nstates > 0 && nzero == nstates, "every pass state reading the padding is 0 there: %lu of %lu", nzero, nstates);
        printf("padding: %lu preshaders read past their float registers, %lu pass states read 0 there\n", npatched, nstates);
    }
    /* in an effect the temporaries are D3DX's stack (§5.1; review R2-09): a copy whose first preshader on a pass state
     * reads r[0] before anything writes it stops there (FXP_E_STALE) */
    {
        uint32_t sz; uint8_t *copy = slurp(files[0], &sz);
        const fx2_state *st = NULL; fxm_source src = { 0 };
        for (uint32_t t = 0; t < fx[0].ntechs && !st; t++)
            for (uint32_t p = 0; p < fx[0].techs[t].npasses && !st; p++)
                for (uint32_t k = 0; k < fx[0].techs[t].passes[p].nstates && !st; k++){
                    src = fxm_state_source(m[0], &fx[0].techs[t].passes[p].states[k]);
                    if (src.src == FXM_S_PRESHADER) st = &fx[0].techs[t].passes[p].states[k];
                }
        int done = 0;
        if (st && copy){
            fx2_pres ps; fx2_pres_read(src.prog->res->data, src.prog->res->size, &ps, NULL, NULL);
            const uint8_t *q = src.prog->res->data + 4 * (ps.fxlc + 2);
            fx2_ins in; fx2_fxlc_ins(q, ps.fxlc_len - 1, 1, &in);
            fx2_operand o; fx2_fxlc_operand(q, ps.fxlc_len - 1, in.in, &o);
            if (in.status == FX2_INS_OK && in.nin == 1 && !o.flag){
                size_t at = (size_t)(q - buf[0]) + 4 * ((size_t)in.in + 1);
                uint32_t t7 = 7, zero = 0;
                memcpy(copy + at, &t7, 4); memcpy(copy + at + 4, &zero, 4);
                fx2_effect fc; fxm_effect *mc = NULL; fxp_effect *xc = NULL;
                if (!fx2_parse(&fc, copy, sz, err, sizeof err)){
                    if (!fxm_effect_new(&mc, &fc, NULL, err, sizeof err) && fxp_effect_new(&xc, mc) == FXP_OK){
                        uint32_t v[64], n;
                        for (uint32_t t = 0; t < fc.ntechs && !done; t++)
                            for (uint32_t p = 0; p < fc.techs[t].npasses && !done; p++)
                                for (uint32_t k = 0; k < fc.techs[t].passes[p].nstates && !done; k++){
                                    fxm_source s2 = fxm_state_source(mc, &fc.techs[t].passes[p].states[k]);
                                    if (s2.src != FXM_S_PRESHADER || s2.prog->res->data - copy != src.prog->res->data - buf[0])
                                        continue;
                                    CHECK(fxp_state_value(xc, &fc.techs[t].passes[p].states[k], 24, v, 64, &n) == FXP_E_STALE,
                                          "a preshader reading a temporary nothing wrote: stale");
                                    done = 1;
                                }
                    }
                    fxp_effect_free(xc); fxm_effect_free(mc, NULL, NULL); fx2_free(&fc);
                }
            }
        }
        CHECK(done, "%s: a preshader state made to read r[0]", files[0]);
        free(copy);
    }
    /* the values A scatters (§4.2; review R2-06): numeric, INT or FLOAT, no array, at most 4 rows; D3DX refuses an
     * object and scatters a struct, an array, rows past 4 and BOOL in ways read (X) but not run: A refuses them */
    {
        const fx2_state *st = NULL; fxm_source src = { 0 };
        for (uint32_t t = 0; t < fx[0].ntechs && !st; t++)
            for (uint32_t p = 0; p < fx[0].techs[t].npasses && !st; p++)
                for (uint32_t k = 0; k < fx[0].techs[t].passes[p].nstates && !st; k++){
                    src = fxm_state_source(m[0], &fx[0].techs[t].passes[p].states[k]);
                    if (src.src == FXM_S_PRESHADER && fx[0].techs[t].passes[p].states[k].t->type == FX2_T_INT)
                        st = &fx[0].techs[t].passes[p].states[k];
                }
        if (st){
            uint32_t v[64], n;
            fx2_type t = *st->t;
            CHECK(fxp_eval(x[0], src.prog, &t, 24, v, 64, &n) == FXP_OK, "an INT value");
            t = *st->t; t.elements = 2;
            CHECK(fxp_eval(x[0], src.prog, &t, 24, v, 64, &n) == FXP_E_TYPE, "an array value: refused");
            t = *st->t; t.rows = 5;
            CHECK(fxp_eval(x[0], src.prog, &t, 24, v, 64, &n) == FXP_E_TYPE, "5 rows: refused");
            t = *st->t; t.rows = 4; t.cols = 4; t.type = FX2_T_FLOAT;
            CHECK(fxp_eval(x[0], src.prog, &t, 24, v, 64, &n) == FXP_OK && n == 16, "a FLOAT 4 x 4 value");
            t = *st->t; t.type = FX2_T_BOOL;
            CHECK(fxp_eval(x[0], src.prog, &t, 24, v, 64, &n) == FXP_E_TYPE, "a BOOL value: refused");
            t = *st->t; t.cls = FX2_C_STRUCT;
            CHECK(fxp_eval(x[0], src.prog, &t, 24, v, 64, &n) == FXP_E_TYPE, "a struct: refused");
            t = *st->t; t.cls = FX2_C_OBJECT;
            CHECK(fxp_eval(x[0], src.prog, &t, 24, v, 64, &n) == FXP_E_TYPE, "an object: refused");
        } else CHECK(0, "%s: an INT preshader state", files[0]);
    }
    if (mw){
        CHECK(n_pres == 616, "fx-preshader §7: 616 preshaders, not %lu", n_pres);
        CHECK(n_ins == 739 && n_mov == 739, "fx-preshader §7: 739 instructions, all mov, not %lu (%lu)", n_ins, n_mov);
    }
    for (int i = 0; i < nfiles; i++){ fxp_effect_free(x[i]); fxm_effect_free(m[i], NULL, NULL); fx2_free(&fx[i]); free(buf[i]); }
    fxm_pool_free(pool);
    free(buf); free(fx); free(m); free(x);
}

int main(int argc, char **argv){
    int mw = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "--mw")){ mw = 1; i++; }
    test_numbers();
    test_loader();
    test_ops();
    test_effects(argc - i, argv + i, mw);
    printf("fxpres_test: %lu checks, %lu failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
