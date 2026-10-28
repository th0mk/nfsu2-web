/* fxpres.c -- the FXLC preshader evaluator (docs/mw/fx-preshader.md; the
 * section numbers below are its).  fxpres.h says what each part does.
 *
 * The exact operations are computed in integers: an x87 register is a sign,
 * an exponent and a 64-bit significand, every result is rounded to the
 * precision control's 24, 53 or 64 bits with the 15-bit exponent of the x87
 * (so never subnormal there), then to a double when it is stored (§3.1).
 * Only the opcodes of the approximate class call the C library (§3.2). */
#include "fxpres.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

static const char *const k_err[FXP_NERR] = { "ok", "version", "nofxlc", "noclit", "operand", "scan", "noise",
                                             "inputs", "range", "stale", "type", "arg", "nomem" };
const char *fxp_err_name(int err){ return err >= 0 && err < FXP_NERR ? k_err[err] : NULL; }

/* ======================= the number model (§3.1) ======================= */

enum { XZ, XF, XI, XN };                 /* zero, finite non-zero, infinity, NaN */
typedef struct {
    int kind, sign;
    int32_t e;                           /* XF: value = m * 2^(e - 63) */
    uint64_t m;                          /* XF: bit 63 set.  XN: the x87 significand (bit 63, the quiet bit 62, payload),
                                          * always quiet */
    int snan;                            /* XN read as a memory operand: it was signalling (Intel's rule, x_nan) */
} xr;

#define DEFAULT_NAN_D 0xFFF8000000000000ull   /* the x87's real indefinite, stored as a double */

static int clz64(uint64_t v){ return v ? __builtin_clzll(v) : 64; }
static int clz128(u128 v){
    uint64_t hi = (uint64_t)(v >> 64);
    return hi ? clz64(hi) : 64 + clz64((uint64_t)v);
}

/* a double as an x87 operand: exact, a NaN quiet (fld: a signalling NaN is loaded quiet, invalid masked).
 * x_mem keeps that it was signalling, for the NaN rule of an arithmetic memory operand (x_nan). */
static xr x_mem(uint64_t b){
    xr r = { XZ, (int)(b >> 63), 0, 0, 0 };
    uint32_t ex = (uint32_t)(b >> 52) & 0x7FF;
    uint64_t fr = b & 0xFFFFFFFFFFFFFull;
    if (ex == 0x7FF){
        if (!fr){ r.kind = XI; return r; }
        r.kind = XN; r.m = 0xC000000000000000ull | fr << 11; r.snan = !(fr >> 51);
        return r;
    }
    if (!ex){
        if (!fr) return r;
        int s = clz64(fr);
        r.kind = XF; r.m = fr << s; r.e = -1074 + 63 - s;
        return r;
    }
    r.kind = XF; r.m = 0x8000000000000000ull | fr << 11; r.e = (int32_t)ex - 1023;
    return r;
}
static xr x_from_d(uint64_t b){ xr r = x_mem(b); r.snan = 0; return r; }
/* fstp qword: round to nearest even, subnormals, overflow to infinity */
static uint64_t x_to_d(xr a){
    uint64_t s = (uint64_t)a.sign << 63;
    if (a.kind == XZ) return s;
    if (a.kind == XI) return s | 0x7FF0000000000000ull;
    if (a.kind == XN) return s | 0x7FF0000000000000ull | (a.m >> 11 & 0xFFFFFFFFFFFFFull) | 0x8000000000000ull;
    int32_t e = a.e;
    uint64_t q, rem, half;
    if (e >= -1022){
        q = a.m >> 11; rem = a.m & 0x7FF; half = 0x400;
        if (rem > half || (rem == half && (q & 1))) q++;
        if (q >> 53){ q >>= 1; e++; }
        if (e > 1023) return s | 0x7FF0000000000000ull;
        return s | (uint64_t)(e + 1023) << 52 | (q & 0xFFFFFFFFFFFFFull);
    }
    int32_t k = -(e + 1011);              /* the right shift that leaves units of 2^-1074; >= 12 */
    if (k >= 64)                          /* below one unit: above half of it rounds up, half ties to even 0 */
        return s | (k == 64 && a.m > 0x8000000000000000ull ? 1u : 0u);
    q = a.m >> k; rem = a.m & ((1ull << k) - 1); half = 1ull << (k - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return s | q;                                       /* q = 2^52 is the smallest normal */
}
/* round sign * M * 2^E (M != 0; sticky: the exact value is a little more) to p bits, unbounded exponent */
static xr x_round(int sign, int32_t E, u128 M, int sticky, int p){
    int z = clz128(M);
    M <<= z; E -= z;                       /* the top bit at 127: value = M * 2^E */
    int drop = 128 - p;
    u128 q = M >> drop, rem = M & (((u128)1 << drop) - 1), half = (u128)1 << (drop - 1);
    if (rem > half || (rem == half && (sticky || (q & 1)))) q++;
    int32_t e = E + 127;
    if (q >> p){ q >>= 1; e++; }
    xr r = { XF, sign, e, (uint64_t)q << (64 - p), 0 };
    return r;
}
static xr x_default_nan(void){ xr r = { XN, 1, 0, 0xC000000000000000ull, 0 }; return r; }
/* An operation on a register (reg: never signalling, fld made it quiet) and an operand from memory, at least
 * one a NaN (Intel SDM vol. 1 table 4-7, x87): a NaN with a number gives the NaN, quiet; a signalling NaN in
 * memory with a quiet NaN gives the quiet one; two quiet NaNs give the larger significand, and with equal
 * significands the positive one (the SDM does not say: Unicorn's QEMU rule, §3.1). */
static xr x_nan(xr reg, xr mem){
    reg.snan = 0;
    if (reg.kind != XN){ mem.snan = 0; return mem; }
    if (mem.kind != XN || mem.snan) return reg;
    if (reg.m != mem.m) return reg.m > mem.m ? reg : mem;
    return reg.sign <= mem.sign ? reg : mem;
}
static int pc_bits(int pc){ return pc == 24 ? 24 : pc == 53 ? 53 : 64; }

static xr x_add(xr a, xr b, int pc){
    int p = pc_bits(pc);
    if (a.kind == XN || b.kind == XN) return x_nan(a, b);
    if (a.kind == XI || b.kind == XI){
        if (a.kind == XI && b.kind == XI && a.sign != b.sign) return x_default_nan();
        return a.kind == XI ? a : b;
    }
    if (a.kind == XZ && b.kind == XZ){ xr r = { XZ, a.sign & b.sign, 0, 0, 0 }; return r; }
    if (a.kind == XZ) return x_round(b.sign, b.e - 63, b.m, 0, p);      /* x + 0 is still rounded to p bits */
    if (b.kind == XZ) return x_round(a.sign, a.e - 63, a.m, 0, p);
    if (b.e > a.e || (b.e == a.e && b.m > a.m)){ xr t = a; a = b; b = t; }     /* |a| >= |b| */
    u128 A = (u128)a.m << 64, B = (u128)b.m << 64;                         /* value = M * 2^(e - 127) */
    uint32_t d = (uint32_t)(a.e - b.e);
    int sticky = 0;
    if (d >= 128){ B = 0; sticky = 1; }
    else if (d){ sticky = (B & (((u128)1 << d) - 1)) != 0; B >>= d; }
    int32_t E = a.e - 127;
    if (a.sign == b.sign){
        u128 S = A + B;
        if (S < A){                                     /* the carry out of bit 127 */
            sticky |= (int)(S & 1); S = S >> 1 | (u128)1 << 127; E++;
        }
        return x_round(a.sign, E, S, sticky, p);
    }
    u128 S = A - B;
    if (sticky) S -= 1;                                 /* the exact value lies in (S - 1, S): S - 1 and sticky */
    if (!S && !sticky){ xr r = { XZ, 0, 0, 0, 0 }; return r; }   /* x - x = +0 */
    return x_round(a.sign, E, S, sticky, p);
}
static xr x_mul(xr a, xr b, int pc){
    if (a.kind == XN || b.kind == XN) return x_nan(a, b);
    int sign = a.sign ^ b.sign;
    if ((a.kind == XI && b.kind == XZ) || (a.kind == XZ && b.kind == XI)) return x_default_nan();
    if (a.kind == XI || b.kind == XI){ xr r = { XI, sign, 0, 0, 0 }; return r; }
    if (a.kind == XZ || b.kind == XZ){ xr r = { XZ, sign, 0, 0, 0 }; return r; }
    return x_round(sign, a.e + b.e - 126, (u128)a.m * b.m, 0, pc_bits(pc));
}
static xr x_div(xr a, xr b, int pc){
    if (a.kind == XN || b.kind == XN) return x_nan(a, b);
    int sign = a.sign ^ b.sign;
    if ((a.kind == XI && b.kind == XI) || (a.kind == XZ && b.kind == XZ)) return x_default_nan();
    if (a.kind == XI || b.kind == XZ){ xr r = { XI, sign, 0, 0, 0 }; return r; }
    if (a.kind == XZ || b.kind == XI){ xr r = { XZ, sign, 0, 0, 0 }; return r; }
    u128 N = (u128)a.m << 64, Q = N / b.m, R = N % b.m;   /* a.m / b.m = (Q + R / b.m) * 2^-64 */
    u128 N2 = R << 2, Q2 = N2 / b.m, R2 = N2 % b.m;       /* two more quotient bits */
    return x_round(sign, a.e - b.e - 66, Q << 2 | Q2, R2 != 0, pc_bits(pc));
}
/* the square root of a value >= 0 */
static xr x_sqrt(xr a, int pc){
    if (a.kind != XF) return a;                         /* +0, -0, +inf, NaN as they are (a is never negative here) */
    int32_t E2 = a.e - 63;
    u128 m = a.m;
    if (E2 & 1){ m <<= 1; E2 -= 1; }                    /* value = m * 2^E2, E2 even, m < 2^65 */
    /* X = m * 2^(2j) with 2j = 68: the root has 66 or 67 bits; digit by digit, two radicand bits a step */
    const int j2 = 68, nbits = 65 + j2 + 1;             /* bits of X, rounded up to even below */
    u128 root = 0, rem = 0;
    for (int i = (nbits & ~1) - 2; i >= 0; i -= 2){
        int b1 = i + 1 - j2 >= 0 && i + 1 - j2 < 128 ? (int)(m >> (i + 1 - j2) & 1) : 0;
        int b0 = i - j2 >= 0 && i - j2 < 128 ? (int)(m >> (i - j2) & 1) : 0;
        rem = rem << 2 | (u128)(b1 << 1 | b0);
        u128 t = root << 2 | 1;
        if (rem >= t){ rem -= t; root = root << 1 | 1; } else root <<= 1;
    }
    return x_round(0, (E2 - j2) / 2, root, rem != 0, pc_bits(pc));
}
/* -1 a < b, 0 equal, 1 a > b, 2 unordered */
static int x_cmp(xr a, xr b){
    if (a.kind == XN || b.kind == XN) return 2;
    if (a.kind == XZ && b.kind == XZ) return 0;
    int sa = a.kind == XZ ? 0 : a.sign ? -1 : 1, sb = b.kind == XZ ? 0 : b.sign ? -1 : 1;
    if (sa != sb) return sa < sb ? -1 : 1;
    int mag;                                            /* compare magnitudes, then apply the sign */
    if (a.kind == XI || b.kind == XI) mag = a.kind == b.kind ? 0 : a.kind == XI ? 1 : -1;
    else if (a.e != b.e) mag = a.e > b.e ? 1 : -1;
    else mag = a.m == b.m ? 0 : a.m > b.m ? 1 : -1;
    return sa < 0 ? -mag : mag;
}

uint64_t fxp_x87_add(uint64_t a, uint64_t b, int pc){ return x_to_d(x_add(x_from_d(a), x_mem(b), pc)); }
uint64_t fxp_x87_mul(uint64_t a, uint64_t b, int pc){ return x_to_d(x_mul(x_from_d(a), x_mem(b), pc)); }
static const xr k_one = { XF, 0, 0, 0x8000000000000000ull, 0 };
uint64_t fxp_x87_rcp(uint64_t a, int pc){ return x_to_d(x_div(k_one, x_mem(a), pc)); }
uint64_t fxp_x87_rsq(uint64_t a, int pc){
    xr x = x_from_d(a);
    x.sign = 0;                                         /* fabs, NaN too */
    return x_to_d(x_div(k_one, x_sqrt(x, pc), pc));
}
uint32_t fxp_ftol(uint64_t a){
    xr x = x_from_d(a);
    if (x.kind == XZ) return 0;
    if (x.kind != XF || x.e >= 63) return 0;            /* NaN, infinity, |a| >= 2^63: 0x8000000000000000 */
    if (x.e < 0) return 0;
    uint64_t t = x.m >> (63 - x.e);                     /* truncation toward zero */
    return (uint32_t)(x.sign ? 0 - t : t);
}
uint32_t fxp_d2f(uint64_t a){
    xr x = x_from_d(a);
    uint32_t s = (uint32_t)x.sign << 31;
    if (x.kind == XZ) return s;
    if (x.kind == XI) return s | 0x7F800000u;
    if (x.kind == XN) return s | 0x7FC00000u | (uint32_t)(x.m >> 40 & 0x7FFFFF);
    int32_t e = x.e;
    uint64_t q, rem, half;
    if (e >= -126){
        q = x.m >> 40; rem = x.m & 0xFFFFFFFFFFull; half = 0x8000000000ull;
        if (rem > half || (rem == half && (q & 1))) q++;
        if (q >> 24){ q >>= 1; e++; }
        if (e > 127) return s | 0x7F800000u;
        return s | (uint32_t)(e + 127) << 23 | (uint32_t)(q & 0x7FFFFF);
    }
    int32_t k = -(e + 86);                              /* units of 2^-149 */
    if (k >= 64){
        if (k == 64 && x.m > 0x8000000000000000ull) return s | 1;
        return s;
    }
    q = x.m >> k; rem = x.m & ((1ull << k) - 1); half = 1ull << (k - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return s | (uint32_t)q;
}
uint64_t fxp_f2d(uint32_t f){
    uint64_t s = (uint64_t)(f >> 31) << 63;
    uint32_t ex = f >> 23 & 0xFF, fr = f & 0x7FFFFF;
    if (ex == 0xFF) return s | 0x7FF0000000000000ull | (uint64_t)fr << 29 | (fr ? 0x8000000000000ull : 0);
    if (!ex){
        if (!fr) return s;
        int z = clz64(fr) - 40;                         /* fr < 2^23: normalise to bit 23 */
        return s | (uint64_t)(1023 - 126 - z) << 52 | ((uint64_t)fr << (29 + z) & 0xFFFFFFFFFFFFFull);
    }
    return s | (uint64_t)(ex + 1023 - 127) << 52 | (uint64_t)fr << 29;
}
uint64_t fxp_i2d(uint32_t i){
    int32_t v = (int32_t)i;
    if (!v) return 0;
    uint64_t s = v < 0 ? 1ull << 63 : 0, m = v < 0 ? (uint64_t)-(int64_t)v : (uint64_t)v;
    int z = clz64(m);
    return s | (uint64_t)(1023 + 63 - z) << 52 | (m << z << 1 >> 12);
}

/* ======================= the program (§2) ======================= */

typedef struct { uint32_t table, offset; } fxp_pair;
typedef struct { uint32_t npairs, pair0; } fxp_opd;           /* index pairs, then the register pair */
typedef struct { uint32_t token, opcode, ncomp, scalar, nin, opd0; } fxp_ins;   /* opds: inputs, then the output */
struct fxp_prog {
    const uint8_t *blob; uint32_t size;
    uint32_t lit;                        /* the byte offset of the literals (after the CLIT count) */
    uint32_t nlit;                       /* the count as D3DX divides by it: 0 becomes 1 (§2.2) */
    uint32_t nins; fxp_ins *ins;
    fxp_opd *opds; fxp_pair *pairs;
    int approx;
};
uint32_t fxp_ninstr(const fxp_prog *p){ return p->nins; }
int fxp_approx(const fxp_prog *p){ return p->approx; }
void fxp_free(fxp_prog *p){ if (p){ free(p->ins); free(p->opds); free(p->pairs); free(p); } }

#define FOURCC_FXLC 0x434C5846u
#define FOURCC_CLIT 0x54494C43u
static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p){ return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* D3DXFindShaderComment (0x4DA2E5, §2.1), bounded to the blob: 0 found (*data: its byte offset), 1 not
 * found (the end token), -1 ran past the end */
static int find_comment(const uint8_t *b, uint32_t size, uint32_t fourcc, uint32_t *data){
    uint32_t version = rd32(b);
    uint64_t i = 4;
    for (;;){
        if ((uint64_t)i + 4 > size) return -1;
        uint32_t tok = rd32(b + i);
        if (!(tok & 0x80000000u)){
            uint32_t op = tok & 0xFFFF;
            if (op == 0xFFFF) return 1;
            if (op == 0xFFFE){
                uint32_t len = tok >> 16 & 0x7FFF;
                if (len > 1){
                    if ((uint64_t)i + 8 > size) return -1;
                    if (rd32(b + i + 4) == fourcc){ *data = (uint32_t)i + 8; return 0; }
                }
                i += 4 * len;
            } else if ((version & 0xFFFF) >= 0x200) i += 4 * (tok >> 24 & 0xF);
            else if (op == 0x51) i += 20;
        }
        i += 4;
    }
}

typedef struct { fxp_ins *ins; fxp_opd *opds; fxp_pair *pairs; uint32_t nins, nopds, npairs, cins, copds, cpairs; } dec;
static int grow(void **p, uint32_t *cap, uint32_t need, size_t el){
    if (need <= *cap) return 0;
    uint32_t c = *cap ? *cap : 16;
    while (c < need){ if (c > 0x7FFFFFFFu / 2) return -1; c *= 2; }
    void *q = realloc(*p, (size_t)c * el);
    if (!q) return -1;
    *p = q; *cap = c; return 0;
}

int fxp_load(fxp_prog **out, const uint8_t *b, uint32_t size){
    *out = NULL;
    if (size < 4) return FXP_E_SCAN;                    /* D3DX reads the version past the end */
    uint32_t v = rd32(b);
    if (v < 0x46580200u || (v > 0x46580201u && v != 0x54580100u)) return FXP_E_VERSION;
    uint32_t fx, cl;
    int r = find_comment(b, size, FOURCC_FXLC, &fx);
    if (r < 0) return FXP_E_SCAN;
    if (r) return FXP_E_NOFXLC;
    r = find_comment(b, size, FOURCC_CLIT, &cl);
    if (r < 0) return FXP_E_SCAN;
    if (r) return FXP_E_NOCLIT;
    if ((uint64_t)cl + 4 > size || (uint64_t)fx + 4 > size) return FXP_E_SCAN;   /* a count past the end */
    fxp_prog *p = calloc(1, sizeof *p);
    if (!p) return FXP_E_NOMEM;
    p->blob = b; p->size = size; p->lit = cl + 4;
    p->nlit = rd32(b + cl) ? rd32(b + cl) : 1;
    dec d; memset(&d, 0, sizeof d);
    int err = FXP_OK;
    uint32_t n = rd32(b + fx), q = fx + 4;               /* q: D3DX's pointer, a byte offset */
    /* every check is D3DX's (0x4E9146, 0x4E86B6, 0x4E872A); a read past the end always ends in one of them
     * failing, so it fails here at once (§2.2) */
#define NEED(nb) do { if ((uint64_t)q + (nb) > size){ err = FXP_E_OPERAND; goto done; } } while (0)
    for (uint32_t i = 0; i < n; i++){
        if (q >= size){ err = FXP_E_OPERAND; goto done; }
        NEED(8);
        uint32_t tok = rd32(b + q), nin = rd32(b + q + 4);
        q += 8;
        if (grow((void **)&d.ins, &d.cins, d.nins + 1, sizeof *d.ins)){ err = FXP_E_NOMEM; goto done; }
        fxp_ins *in = &d.ins[d.nins++];
        in->token = tok; in->opcode = tok >> 20 & 0x7FF; in->ncomp = tok & 0xFFFFF; in->scalar = tok >> 31;
        in->nin = nin; in->opd0 = d.nopds;
        for (uint64_t k = 0; k < nin; k++){                 /* the inputs */
            NEED(4);
            uint32_t idx = rd32(b + q);
            q += 4;
            if (q >= size){ err = FXP_E_OPERAND; goto done; }
            if (grow((void **)&d.opds, &d.copds, d.nopds + 1, sizeof *d.opds)){ err = FXP_E_NOMEM; goto done; }
            fxp_opd *o = &d.opds[d.nopds++];
            o->pair0 = d.npairs; o->npairs = 0;
            for (uint64_t j = 0; j < idx; j++){
                NEED(4);
                uint32_t t = rd32(b + q);
                if ((uint64_t)q + 8 >= size){ err = FXP_E_OPERAND; goto done; }
                uint32_t off = rd32(b + q + 4);
                q += 8;
                if (t == 9){ err = FXP_E_OPERAND; goto done; }
                if (grow((void **)&d.pairs, &d.cpairs, d.npairs + 1, sizeof *d.pairs)){ err = FXP_E_NOMEM; goto done; }
                d.pairs[d.npairs].table = t; d.pairs[d.npairs].offset = off; d.npairs++; o->npairs++;
            }
            NEED(8);
            uint32_t t = rd32(b + q), off = rd32(b + q + 4);
            q += 8;
            if (t == 0 || t > 9){ err = FXP_E_OPERAND; goto done; }
            if (grow((void **)&d.pairs, &d.cpairs, d.npairs + 1, sizeof *d.pairs)){ err = FXP_E_NOMEM; goto done; }
            d.pairs[d.npairs].table = t; d.pairs[d.npairs].offset = off; d.npairs++; o->npairs++;
        }
        NEED(4);                                            /* the output: flag 0, then one pair */
        if (rd32(b + q)){ err = FXP_E_OPERAND; goto done; }
        q += 4;
        NEED(8);
        uint32_t t = rd32(b + q), off = rd32(b + q + 4);
        q += 8;
        if (q >= size){ err = FXP_E_OPERAND; goto done; }
        if (t != 4 && t != 5 && t != 6 && t != 7){ err = FXP_E_OPERAND; goto done; }
        if (grow((void **)&d.opds, &d.copds, d.nopds + 1, sizeof *d.opds) ||
            grow((void **)&d.pairs, &d.cpairs, d.npairs + 1, sizeof *d.pairs)){ err = FXP_E_NOMEM; goto done; }
        d.opds[d.nopds].pair0 = d.npairs; d.opds[d.nopds].npairs = 1; d.nopds++;
        d.pairs[d.npairs].table = t; d.pairs[d.npairs].offset = off; d.npairs++;
    }
#undef NEED
    for (uint32_t i = 0; i < d.nins; i++){
        uint32_t op = d.ins[i].opcode;
        if (d.ins[i].scalar ? op == 0x206 : (op == 0x105 || op == 0x106 || (op >= 0x108 && op <= 0x10C) || op == 0x206))
            p->approx = 1;
    }
done:
    if (err){ free(d.ins); free(d.opds); free(d.pairs); free(p); return err; }
    p->nins = d.nins; p->ins = d.ins; p->opds = d.opds; p->pairs = d.pairs;
    *out = p;
    return FXP_OK;
}

/* ======================= the interpreter (§3) ======================= */

/* where an operand points: a table and an element */
typedef struct {
    int table;                           /* 1 literal, 2 c, 3 v, 4 o, 7 r; 9 D3DX's table 9, whose base is 0 in both
                                          * of its paths: every read through it is outside */
    uint32_t base;                       /* the element (literal: its byte offset in the blob) */
} ref;

/* wrote: the temporaries this run has written when they started unknown (fxp_regs.temps_unknown), else NULL.
 * FXP_OK, FXP_E_RANGE (outside a table), FXP_E_STALE (a temporary D3DX would read from its stack, §5.1) */
static int ref_get(const fxp_prog *p, const fxp_regs *x, const uint8_t *wrote, ref f, uint32_t k, uint64_t *v){
    uint64_t i;
    switch (f.table){
    case 1: i = (uint32_t)(f.base + 8u * k);           /* D3DX's pointer arithmetic: modulo 2^32 */
            if (i + 8 > p->size) return FXP_E_RANGE;
            *v = rd64(p->blob + i); return FXP_OK;
    case 2: i = (uint64_t)f.base + k; if (!x->c || i >= x->csize) return FXP_E_RANGE; *v = x->c[i]; return FXP_OK;
    case 3: i = (uint64_t)f.base + k; if (!x->v || i >= x->vsize) return FXP_E_RANGE; *v = x->v[i]; return FXP_OK;
    case 4: i = (uint64_t)f.base + k; if (!x->o || i >= x->osize) return FXP_E_RANGE; *v = x->o[i]; return FXP_OK;
    case 7: i = (uint64_t)f.base + k; if (i >= FXP_NTEMP) return FXP_E_RANGE;
            if (wrote && !wrote[i]) return FXP_E_STALE;
            *v = x->r[i]; return FXP_OK;
    default: return FXP_E_RANGE;
    }
}
static int ref_put(fxp_regs *x, uint8_t *wrote, ref f, uint32_t k, uint64_t v){
    uint64_t i = (uint64_t)f.base + k;
    if (f.table == 7){ if (i >= FXP_NTEMP) return -1; x->r[i] = v; if (wrote) wrote[i] = 1; return 0; }
    if (!x->o || i >= x->osize) return -1;
    x->o[i] = v; return 0;
}
/* where an operand points (§2.4): index pairs, then the register pair.  FXP_OK, ref_get's error for an index
 * register, or -1: the register is in table 8, which D3DX's decode has no case for (the slot keeps what it held) */
static int resolve(const fxp_prog *p, const fxp_regs *x, const uint8_t *wrote, const fxp_opd *o, ref *f){
    uint32_t e = 0;
    for (uint32_t j = 0; j < o->npairs; j++){
        const fxp_pair *pr = &p->pairs[o->pair0 + j];
        e = pr->offset + e * 4u;
        uint32_t t = pr->table;
        if (j + 1 < o->npairs){                         /* an index register: its value, truncated */
            if (t < 1 || t > 7) continue;               /* no register: the arithmetic alone */
            ref g; uint64_t v;
            switch (t){
            case 1: g.table = 1; g.base = p->lit + 8u * (e % p->nlit); break;
            case 2: g.table = 2; g.base = e & x->cmask; break;
            case 3: g.table = 3; g.base = e & x->vmask; break;
            case 7: g.table = 7; g.base = e & 0xFF; break;
            default: g.table = 4; g.base = e & x->omask; break;
            }
            int err = ref_get(p, x, wrote, g, 0, &v);
            if (err) return err;
            e = fxp_ftol(v);
            continue;
        }
        switch (t){
        case 1: f->table = 1; f->base = p->lit + 8u * (e % p->nlit); break;
        case 2: f->table = 2; f->base = e & x->cmask; break;
        case 3: f->table = 3; f->base = e & x->vmask; break;
        case 7: f->table = 7; f->base = e & 0xFF; break;
        case 8: return -1;
        case 9: f->table = 9; f->base = 0; break;
        default: f->table = 4; f->base = e & x->omask; break;     /* 4, 5, 6: one table */
        }
    }
    return FXP_OK;
}

static double d_of(uint64_t b){ double d; memcpy(&d, &b, 8); return d; }
static uint64_t b_of(double d){ uint64_t b; memcpy(&b, &d, 8); return b; }
/* fld qword; fstp qword: the same bits, except a signalling NaN comes back quiet */
static uint64_t ldst(uint64_t b){ return x_to_d(x_from_d(b)); }
static const uint64_t k_zero = 0, k_onebits = 0x3FF0000000000000ull;
#define TWO63 9223372036854775808.0

/* the approximate class (§3.2): the C library on the double operand(s), in D3DX's order where it has one */
#define LOG2E_BITS 0x3FF71547652B82FEull           /* the double D3DX multiplies ln|x| by */
static uint64_t approx1(uint32_t op, uint64_t a, int pc){
    xr x = x_from_d(a);
    if (op == 0x106){                                   /* fld; fabs; fldln2; fxch; fyl2x; fmul log2e */
        x.sign = 0;                                     /* fabs, a NaN too */
        if (x.kind == XN) return x_to_d(x);             /* fyl2x and fmul give the NaN */
        if (x.kind == XZ) return 0xFFF0000000000000ull; /* fyl2x(+0) = -inf (#Z masked), -inf * log2e = -inf */
        return x_to_d(x_mul(x_from_d(b_of(log(d_of(x_to_d(x))))), x_mem(LOG2E_BITS), pc));
    }
    if (x.kind == XN) return x_to_d(x);
    double v = d_of(a);
    switch (op){
    case 0x105: return b_of(exp2(v));
    case 0x108: case 0x109:
        if (x.kind == XI) return DEFAULT_NAN_D;                  /* fsin, fcos: invalid on an infinity */
        if (fabs(v) > TWO63) return a;                           /* out of range: C2 set, the operand stays */
        return b_of(op == 0x108 ? sin(v) : cos(v));
    case 0x10A: return b_of(asin(v));
    case 0x10B: return b_of(acos(v));
    default: return b_of(atan(v));
    }
}
static uint64_t approx_atan2(uint64_t y, uint64_t xx){
    xr a = x_from_d(y), b = x_from_d(xx);
    if (a.kind == XN || b.kind == XN) return x_to_d(x_nan(a, b));
    return b_of(atan2(d_of(y), d_of(xx)));
}

/* input slot s at component k (§3.3): slot 256 and up is D3DX's frame past its array; a slot no input of
 * this run has set holds what the stack held */
static int sget(const fxp_prog *p, const fxp_regs *x, const uint8_t *wrote, const ref *a, const uint8_t *set, uint32_t s,
                uint32_t k, uint64_t *v){
    if (s >= 256) return FXP_E_RANGE;
    if (!set[s]) return FXP_E_STALE;
    return ref_get(p, x, wrote, a[s], k, v);
}
#define GET(s, k, v) do { int e_ = sget(p, x, wrote, a, set, (s), (k), &(v)); if (e_) return e_; } while (0)
#define GETD(k, v) do { int e_ = ref_get(p, x, wrote, dst, (k), &(v)); if (e_) return e_; } while (0)
#define PUT(k, v) do { if (ref_put(x, wrote, dst, (k), (v))) return FXP_E_RANGE; } while (0)

int fxp_run(const fxp_prog *p, fxp_regs *x){
    int pc = x->pc;
    if (pc != 24 && pc != 53 && pc != 64) return FXP_E_ARG;
    /* D3DX's input pointers: one array of 256 for the whole run; each input writes its own slot, except one in
     * table 8; the reads go through the slots the opcode names, whatever the input count (§3.3) */
    ref a[256];
    uint8_t set[256], wbuf[FXP_NTEMP], *wrote = NULL;
    memset(set, 0, sizeof set);
    if (x->temps_unknown){ memset(wbuf, 0, sizeof wbuf); wrote = wbuf; }   /* D3DX's stack: track what is written */
    for (uint32_t i = 0; i < p->nins; i++){
        const fxp_ins *in = &p->ins[i];
        for (uint32_t k = 0; k < in->nin; k++){
            ref r;
            int rr = resolve(p, x, wrote, &p->opds[in->opd0 + k], &r);
            if (rr < 0) continue;                       /* table 8: the slot is not written */
            if (rr) return rr;
            if (k >= 256) return FXP_E_INPUTS;          /* the 257th: D3DX writes past its array */
            a[k] = r; set[k] = 1;
        }
        ref dst;
        if (resolve(p, x, wrote, &p->opds[in->opd0 + in->nin], &dst)) return FXP_E_RANGE;   /* tables 4..7, no index */
        uint32_t n = in->ncomp, op = in->opcode;
        uint64_t u, w, z;
        if (in->scalar){                                /* input 0 at component 0 for every component (§3.2) */
            switch (op){
            case 0x200: case 0x201: case 0x202: case 0x203:
                for (uint32_t k = 0; k < n; k++){
                    GET(0, 0, u); GET(1, k, w);
                    int c = x_cmp(x_from_d(u), x_from_d(w));
                    uint64_t r = op == 0x200 ? (c == -1 ? ldst(u) : ldst(w)) : op == 0x201 ? (c == 1 ? ldst(u) : ldst(w))
                               : op == 0x202 ? (c == -1 ? k_onebits : k_zero) : (c == 0 || c == 1 ? k_onebits : k_zero);
                    PUT(k, r);
                }
                break;
            case 0x204: case 0x205:                     /* fld in1[k]; fadd/fmul in0[0] */
                for (uint32_t k = 0; k < n; k++){
                    GET(1, k, w); GET(0, 0, u);
                    xr r = op == 0x204 ? x_add(x_from_d(w), x_mem(u), pc) : x_mul(x_from_d(w), x_mem(u), pc);
                    PUT(k, x_to_d(r));
                }
                break;
            case 0x206:
                for (uint32_t k = 0; k < n; k++){ GET(0, 0, u); GET(1, k, w); PUT(k, approx_atan2(u, w)); }
                break;
            case 0x500:                                 /* fld in1[k]; fmul in0[0]; fadd sum; fstp sum */
                PUT(0, k_zero);
                for (uint32_t k = 0; k < n; k++){
                    GET(1, k, w); GET(0, 0, u);
                    xr pr = x_mul(x_from_d(w), x_mem(u), pc);
                    GETD(0, z);
                    PUT(0, x_to_d(x_add(pr, x_mem(z), pc)));
                }
                break;
            default: break;                             /* no scalar form: nothing happens (noise too) */
            }
            continue;
        }
        switch (op){
        case 0x100:                                     /* a forward copy, bits as they are */
            for (uint32_t k = 0; k < n; k++){ GET(0, k, u); PUT(k, u); }
            break;
        case 0x101:
            for (uint32_t k = 0; k < n; k++){ GET(0, k, u); xr r = x_from_d(u); r.sign ^= 1; PUT(k, x_to_d(r)); }
            break;
        case 0x103:                                     /* fld1; fdiv in0[k] */
            for (uint32_t k = 0; k < n; k++){ GET(0, k, u); PUT(k, x_to_d(x_div(k_one, x_mem(u), pc))); }
            break;
        case 0x104:                                     /* x - floor(x): fld x, floor, fsubr x; floor exact */
            for (uint32_t k = 0; k < n; k++){
                GET(0, k, u);
                xr v = x_from_d(u);
                if (v.kind == XN){ PUT(k, x_to_d(v)); continue; }      /* floor gives the NaN; the rule gives it */
                xr f = x_from_d(b_of(floor(d_of(u))));
                f.sign ^= 1;                                           /* x + (-floor(x)) */
                GET(0, k, u);
                PUT(k, x_to_d(x_add(f, x_mem(u), pc)));
            }
            break;
        case 0x106:                                     /* the zero test reads slot k's component 0 (§3.2) */
            for (uint32_t k = 0; k < n; k++){
                GET(k, 0, z);
                if (x_cmp(x_from_d(z), x_from_d(k_zero)) == 0){ PUT(k, k_zero); continue; }
                GET(0, k, u);
                PUT(k, approx1(op, u, pc));
            }
            break;
        case 0x107:
            for (uint32_t k = 0; k < n; k++){ GET(0, k, u); PUT(k, fxp_x87_rsq(u, pc)); }
            break;
        case 0x105: case 0x108: case 0x109: case 0x10A: case 0x10B: case 0x10C:
            for (uint32_t k = 0; k < n; k++){ GET(0, k, u); PUT(k, approx1(op, u, pc)); }
            break;
        case 0x200: case 0x201: case 0x202: case 0x203:
            for (uint32_t k = 0; k < n; k++){
                GET(0, k, u); GET(1, k, w);
                int c = x_cmp(x_from_d(u), x_from_d(w));
                uint64_t r = op == 0x200 ? (c == -1 ? ldst(u) : ldst(w)) : op == 0x201 ? (c == 1 ? ldst(u) : ldst(w))
                           : op == 0x202 ? (c == -1 ? k_onebits : k_zero) : (c == 0 || c == 1 ? k_onebits : k_zero);
                PUT(k, r);
            }
            break;
        case 0x204: case 0x205:                         /* fld in0[k]; fadd/fmul in1[k] */
            for (uint32_t k = 0; k < n; k++){
                GET(0, k, u); GET(1, k, w);
                xr r = op == 0x204 ? x_add(x_from_d(u), x_mem(w), pc) : x_mul(x_from_d(u), x_mem(w), pc);
                PUT(k, x_to_d(r));
            }
            break;
        case 0x206:
            for (uint32_t k = 0; k < n; k++){ GET(0, k, u); GET(1, k, w); PUT(k, approx_atan2(u, w)); }
            break;
        case 0x300:                                     /* in0 >= 0 ? in1 : in2: only the slot chosen is read */
            for (uint32_t k = 0; k < n; k++){
                GET(0, k, u);
                int c = x_cmp(x_from_d(u), x_from_d(k_zero));
                GET(c == 0 || c == 1 ? 1 : 2, k, w);
                PUT(k, ldst(w));
            }
            break;
        case 0x500:                                     /* the sum written as it goes, from 0 */
            PUT(0, k_zero);
            for (uint32_t k = 0; k < n; k++){
                GET(0, k, u); GET(1, k, w);
                xr pr = x_mul(x_from_d(u), x_mem(w), pc);
                GETD(0, z);
                PUT(0, x_to_d(x_add(pr, x_mem(z), pc)));
            }
            break;
        case 0x502: {                                   /* input 0's first min(n, 3) components, then the noise */
            uint32_t m = n < 3 ? n : 3;
            for (uint32_t k = 0; k < m; k++) GET(0, k, u);
            return FXP_E_NOISE;
        }
        case 0x70E: {                                   /* component 0 of input k times that of input h + k */
            uint32_t h = in->nin / 2;
            PUT(0, k_zero);
            for (uint32_t k = 0; k < h; k++){
                GET(k, 0, u); GET(h + k, 0, w);
                xr pr = x_mul(x_from_d(u), x_mem(w), pc);
                GETD(0, z);
                PUT(0, x_to_d(x_add(pr, x_mem(z), pc)));
            }
            break;
        }
        case 0x711: {                                   /* component 0 of inputs 0 .. min(k, 3) - 1, then the noise */
            uint32_t m = in->nin < 3 ? in->nin : 3;
            for (uint32_t k = 0; k < m; k++) GET(k, 0, u);
            return FXP_E_NOISE;
        }
        default: break;                                 /* div, movc, nop and every other opcode: nothing */
        }
    }
    return FXP_OK;
}
#undef GET
#undef GETD
#undef PUT

int fxp_pc_from_cw(uint32_t cw){
    static const int k_pc[4] = { 24, 0, 53, 64 };       /* PC field 1 is reserved */
    return cw >> 10 & 3 ? 0 : k_pc[cw >> 8 & 3];        /* RC: nearest only */
}

/* ======================= the effect path (§4) ======================= */

typedef struct { fxp_prog *p; int err; } pent;
struct fxp_effect {
    const fxm_effect *e;
    const fxm_prog *base;                /* fxm_progs: entry i decodes program i */
    uint32_t n;
    pent *v;
};

int fxp_effect_new(fxp_effect **out, const fxm_effect *e){
    *out = NULL;
    fxp_effect *x = calloc(1, sizeof *x);
    if (!x) return FXP_E_NOMEM;
    x->e = e;
    x->base = fxm_progs(e, &x->n);
    x->v = calloc(x->n ? x->n : 1, sizeof *x->v);
    if (!x->v){ free(x); return FXP_E_NOMEM; }
    for (uint32_t i = 0; i < x->n; i++){
        int err = x->base[i].kind == FXM_P_PRESHADER ? fxp_load(&x->v[i].p, x->base[i].res->data, x->base[i].res->size)
                                                     : FXP_E_TYPE;
        if (fxp_refused_by_d3dx(err) || err == FXP_E_NOMEM){   /* D3DXCreateEffect fails with E_FAIL (§1, O) */
            fxp_effect_free(x);
            return err;
        }
        x->v[i].err = err;
    }
    *out = x;
    return FXP_OK;
}
void fxp_effect_free(fxp_effect *x){
    if (!x) return;
    for (uint32_t i = 0; i < x->n; i++) fxp_free(x->v[i].p);
    free(x->v); free(x);
}
int fxp_prog_error(const fxp_effect *x, const fxm_prog *pr){
    if (pr < x->base || pr >= x->base + x->n) return FXP_E_ARG;
    return x->v[pr - x->base].err;
}

/* the input table's size (§2.5): CTAB register ends of the float, bool and int sets */
static void ctab_ends(const fxm_prog *pr, uint32_t *fend, uint32_t *bend, uint32_t *iend){
    *fend = *bend = *iend = 0;
    for (uint32_t k = 0; k < pr->nread; k++){
        const fx2_const *c = &pr->consts[k].c;
        uint32_t end = c->reg + c->count, *t = c->set == 2 ? fend : c->set == 0 ? bend : c->set == 1 ? iend : NULL;
        if (t && end > *t) *t = end;
    }
}
static uint32_t smear(uint32_t v){ v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; return v; }
static uint64_t gather1(uint32_t type, uint32_t w){ return type == FX2_T_FLOAT ? fxp_f2d(w) : fxp_i2d(w); }

int fxp_eval(fxp_effect *x, const fxm_prog *pr, const fx2_type *t, int pc, uint32_t *out, uint32_t cap, uint32_t *n){
    *n = 0;
    if (pr < x->base || pr >= x->base + x->n || (pc != 24 && pc != 53 && pc != 64)) return FXP_E_ARG;
    const pent *q = &x->v[pr - x->base];
    if (q->err) return q->err;
    /* the values A scatters (§4.2): a numeric class, INT or FLOAT, no array, at most 4 rows: MW's, and what the
     * oracle ran.  D3DX refuses an object (E_FAIL); a struct (member by member), an array, rows past 4 and BOOL
     * it scatters in ways read from its code (X 0x500C68) but not run: A refuses them */
    if (!t || t->cls > FX2_C_MATRIX_COLUMNS || (t->type != FX2_T_INT && t->type != FX2_T_FLOAT) || t->elements ||
        t->rows > 4)
        return FXP_E_TYPE;
    /* gather (§4.1): every matched numeric constant of set c, its leaves by layout, into a table of mask + 1
     * doubles that D3DX zeroes (0x4FB467..0x4FB4D8, §2.5): past 4 fend (bool and int parts, padding) it reads 0 */
    uint32_t fend, bend, iend;
    ctab_ends(pr, &fend, &bend, &iend);
    uint64_t tot = (uint64_t)bend + 4ull * ((uint64_t)fend + iend);           /* < 2^21: 16-bit CTAB fields */
    uint32_t cmask = smear(tot ? (uint32_t)(tot - 1) : 0), csize = cmask + 1;
    uint64_t cs[256], os[64];             /* the tables of MW's preshaders fit here (at most 32 and 16) */
    uint64_t *c = csize <= 256 ? cs : calloc(csize, sizeof *c);
    if (!c) return FXP_E_NOMEM;
    if (c == cs) memset(cs, 0, 8 * (size_t)csize);       /* only [0, csize) is ever read */
    for (uint32_t k = 0; k < pr->nread; k++){
        const fxm_const *mc = &pr->consts[k];
        if (mc->why || mc->c.set != 2 || mc->layout > FXM_L_COLUMNS) continue;
        for (uint32_t l = 0; l < mc->nleaves; l++){
            const fxm_leaf *lf = &mc->leaves[l];
            const fxm_node *nd = fxm_node_get(x->e, lf->id);
            const uint32_t *d = fxm_data(x->e, lf->id);
            if (!nd || !d) continue;
            uint32_t rows = nd->rows, cols = nd->cols, nd_dw = nd->bytes / 4;
            for (uint32_t r = 0; r < lf->count; r++){
                uint32_t reg = lf->reg + r;
                if (reg >= fend) break;
                for (uint32_t cc = 0; cc < 4; cc++){
                    uint32_t at;                          /* the dword of the leaf's data, or none */
                    if (nd->cls == FX2_C_MATRIX_ROWS){    /* D3DX: register r is row r, whatever the constant's
                                                           * layout (0x5027B1, §4.1) */
                        if (r >= rows || cc >= cols) continue;
                        at = r * cols + cc;
                    } else {
                        switch (lf->layout){
                        case FXM_L_SCALAR: if (cc || r) continue; at = 0; break;
                        case FXM_L_VECTOR: if (cc >= cols) continue; at = r * cols + cc; break;
                        case FXM_L_ROWS:
                            if (cc >= cols || r >= rows) continue;
                            at = nd->cls == FX2_C_MATRIX_COLUMNS ? cc * rows + r : r * cols + cc; break;
                        default:                          /* columns: register r holds column r */
                            if (cc >= rows || r >= cols) continue;
                            at = nd->cls == FX2_C_MATRIX_COLUMNS ? r * rows + cc : cc * cols + r; break;
                        }
                    }
                    if (at < nd_dw) c[4 * reg + cc] = gather1(nd->type, d[at]);
                }
            }
        }
    }
    /* run, with the output table D3DX gives the value (§2.5): ceil(rows / 4) = 1 register per column */
    uint32_t rows = t->rows, cols = t->cols;
    uint64_t nreg = rows ? cols : 0;
    if (!nreg) nreg = 1;
    if (nreg > 0x10000){ if (c != cs) free(c); return FXP_E_RANGE; }
    uint32_t omask = smear((uint32_t)(4 * nreg - 1)), osize = omask + 1;
    uint64_t *o = osize <= 64 ? os : calloc(osize, sizeof *o);
    if (!o){ if (c != cs) free(c); return FXP_E_NOMEM; }
    if (o == os) memset(os, 0, 8 * (size_t)osize);
    fxp_regs xr_;                                       /* r is not cleared: with temps_unknown a temporary is read
                                                         * only after this run wrote it */
    xr_.c = c; xr_.csize = csize; xr_.cmask = cmask;
    xr_.v = NULL; xr_.vsize = xr_.vmask = 0;
    xr_.o = o; xr_.osize = osize; xr_.omask = omask;
    xr_.temps_unknown = 1;                              /* D3DX's temporaries are its stack (§5.1) */
    xr_.pc = pc;
    int err = fxp_run(q->p, &xr_);
    /* scatter (§4.2): column r, component cc from o[4r + cc]; FLOAT rounds to single, INT truncates (_ftol2) */
    if (!err){
        uint64_t need = (uint64_t)rows * cols;
        if (need > cap) err = FXP_E_ARG;
        else for (uint32_t r = 0; r < cols; r++)
            for (uint32_t cc = 0; cc < rows; cc++){
                uint64_t at = 4ull * r + cc;
                uint64_t v = at < osize ? o[at] : 0;
                out[(*n)++] = t->type == FX2_T_FLOAT ? fxp_d2f(v) : fxp_ftol(v);
            }
    }
    if (c != cs) free(c);
    if (o != os) free(o);
    return err;
}

int fxp_state_value(fxp_effect *x, const fx2_state *s, int pc, uint32_t *out, uint32_t cap, uint32_t *n){
    *n = 0;
    fxm_source src = fxm_state_source(x->e, s);
    if (src.src != FXM_S_PRESHADER || !src.prog) return FXP_E_ARG;
    return fxp_eval(x, src.prog, s->t, pc, out, cap, n);
}
