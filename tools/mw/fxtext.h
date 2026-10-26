/* fxtext.h -- the text rules the canonical effect dumps share
 * (docs/mw/fx2-format.md §5.1, §5.4; docs/mw/fx-model.md §5.1) and the
 * SHA-256 of their effect lines, for tools/mw/fxdump.c, fxmodeldump.c and
 * fxmodel_test.c.  Header-only: every function is static inline. */
#ifndef MW_FXTEXT_H
#define MW_FXTEXT_H
#include "fx2.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- SHA-256 (FIPS 180-4) ---- */

static const uint32_t fxt_k_sha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define FXT_ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static inline void fxt_sha_block(uint32_t h[8], const uint8_t *b){
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++){
        uint32_t s0 = FXT_ROR(w[i - 15], 7) ^ FXT_ROR(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = FXT_ROR(w[i - 2], 17) ^ FXT_ROR(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, h, sizeof a);
    for (int i = 0; i < 64; i++){
        uint32_t t1 = a[7] + (FXT_ROR(a[4], 6) ^ FXT_ROR(a[4], 11) ^ FXT_ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + fxt_k_sha[i] + w[i];
        uint32_t t2 = (FXT_ROR(a[0], 2) ^ FXT_ROR(a[0], 13) ^ FXT_ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof a[0]);             /* h = g ... b = a; a[4] is now d */
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += a[i];
}
static inline void sha256(const uint8_t *p, uint32_t n, uint8_t out[32]){
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t tail[128] = { 0 };
    uint32_t full = n / 64 * 64, rest = n - full, tn = rest < 56 ? 64 : 128;
    for (uint32_t i = 0; i < full; i += 64) fxt_sha_block(h, p + i);
    if (rest) memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++) tail[tn - 1 - i] = (uint8_t)((uint64_t)n * 8 >> 8 * i);
    fxt_sha_block(h, tail);
    if (tn == 128) fxt_sha_block(h, tail + 64);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}
/* the 64 lowercase hex digits and a NUL */
static inline void sha256_hex(const uint8_t *p, uint32_t n, char out[65]){
    uint8_t sum[32];
    sha256(p, n, sum);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", sum[i]);
}

/* ---- text (fx2-format §5.1) ---- */

static inline void ind(FILE *f, int lv){ for (int i = 0; i < lv; i++) fputs("  ", f); }
static inline void unk(FILE *f, uint32_t v){ fprintf(f, "UNKNOWN(0x%08x)", v); }
static inline void name_or_unk(FILE *f, const char *name, uint32_t v){ if (name) fputs(name, f); else unk(f, v); }

static inline void qstr(FILE *f, const uint8_t *p, uint32_t n){
    fputc('"', f);
    for (uint32_t i = 0; i < n; i++){
        uint8_t ch = p[i];
        if (ch == '"' || ch == '\\'){ fputc('\\', f); fputc(ch, f); }
        else if (ch >= 0x20 && ch <= 0x7E) fputc(ch, f);
        else fprintf(f, "\\x%02x", ch);
    }
    fputc('"', f);
}
static inline void qname(FILE *f, fx2_str s){ if (s.p) qstr(f, s.p, s.n); else fputc('-', f); }

/* Gp: C99 %.<p>g, with nan, inf and -inf spelled out */
static inline void gnum(FILE *f, double x, int prec){
    if (isnan(x)) fputs("nan", f);
    else if (isinf(x)) fputs(x < 0 ? "-inf" : "inf", f);
    else fprintf(f, "%.*g", prec, x);
}
static inline void f32(FILE *f, uint32_t w){ float x; memcpy(&x, &w, 4); fprintf(f, "0x%08x:", w); gnum(f, x, 9); }
static inline void f64(FILE *f, const uint8_t *p){
    uint64_t q = (uint64_t)fx2_rd32(p) | (uint64_t)fx2_rd32(p + 4) << 32;
    double x; memcpy(&x, &q, 8);
    fprintf(f, "0x%016llx:", (unsigned long long)q); gnum(f, x, 17);
}

/* TT (fx2-format §5.4); without the element count for an element (fx-model §5.1) */
static inline void tt_el(FILE *f, const fx2_type *t, int with_elements){
    name_or_unk(f, fx2_type_name(t->type), t->type);
    fputc(' ', f);
    name_or_unk(f, fx2_class_name(t->cls), t->cls);
    if (t->cls <= FX2_C_MATRIX_COLUMNS) fprintf(f, " %ux%u", t->rows, t->cols);
    else if (t->cls == FX2_C_STRUCT) fprintf(f, " %u", t->nmembers);
    if (with_elements && t->elements) fprintf(f, " [%u]", t->elements);
}
static inline void tt(FILE *f, const fx2_type *t){ tt_el(f, t, 1); }

/* one dword of a value of type type: F32 for FLOAT, INT for BOOL and INT, HEX8 otherwise */
static inline void word(FILE *f, uint32_t type, uint32_t w){
    if (type == FX2_T_FLOAT) f32(f, w);
    else if (type == FX2_T_BOOL || type == FX2_T_INT) fprintf(f, "%d", (int32_t)w);
    else fprintf(f, "0x%08x", w);
}
/* VAL (fx-model §5.1): n dwords separated by spaces, "-" for none */
static inline void val(FILE *f, uint32_t type, const uint32_t *w, uint64_t n){
    if (!n){ fputc('-', f); return; }
    for (uint64_t i = 0; i < n; i++){ if (i) fputc(' ', f); word(f, type, w[i]); }
}

/* FRAG (fx2-format §5.4) of element e of a numeric or object value */
static inline void frag(FILE *f, const fx2_effect *fx, const fx2_value *v, uint32_t e){
    const fx2_type *t = v->t;
    if (v->kind == FX2_V_OBJECT){
        uint32_t id = fx2_rd32(v->data + 4 * (uint64_t)e);
        fputs("obj ", f);
        if (id < fx->nobjects) fprintf(f, "%u", id); else unk(f, id);
        return;
    }
    uint64_t per = (uint64_t)t->rows * t->cols;
    if (!per){ fputc('-', f); return; }
    const uint8_t *q = v->data + 4 * per * e;
    for (uint64_t i = 0; i < per; i++){
        if (i) fputc(' ', f);
        word(f, t->type, fx2_rd32(q + 4 * i));
    }
}

/* VN (fx2-format §5.9): vs_1_1, ps_2_x, ...; 0 when the high word is neither 0xFFFE nor 0xFFFF */
static inline int vname(char *out, size_t n, uint32_t version){
    if (version >> 16 != 0xFFFE && version >> 16 != 0xFFFF) return 0;
    uint32_t major = version >> 8 & 0xFF, minor = version & 0xFF;
    if (major == 2 && minor == 1) snprintf(out, n, "%s_2_x", version >> 16 == 0xFFFE ? "vs" : "ps");
    else snprintf(out, n, "%s_%u_%u", version >> 16 == 0xFFFE ? "vs" : "ps", major, minor);
    return 1;
}
#endif
