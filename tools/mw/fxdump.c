/* fxdump.c -- the canonical text dump ("fx2dump 1", docs/mw/fx2-format.md
 * §5) of one compiled D3DX effect, read with runtime/mw/fx2.c, the parser
 * the runtime uses.
 *
 *   clang -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Iruntime/mw \
 *         -o out/mw/fx/bin/fxdump tools/mw/fxdump.c runtime/mw/fx2.c
 *   out/mw/fx/bin/fxdump <NAME>.fxo [<out.txt>]
 *
 * NAME is the file name without .fxo.  Without an output file the dump goes
 * to stdout.  An effect the parser refuses gives its message on stderr, exit
 * status 1 and no output file.  tools/mw/fxdump.sh builds it and dumps all 31
 * effects.  With -DFXDUMP_NO_MAIN this file is fxdump() alone, for
 * tools/mw/fxfuzz.c.
 */
#include "fx2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fxdump(FILE *f, const char *name, const uint8_t *buf, uint32_t size, char *err, uint32_t errlen);

/* ---- SHA-256 (FIPS 180-4), for the effect line ---- */

static const uint32_t k_sha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(uint32_t h[8], const uint8_t *b){
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; i++){
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, h, sizeof a);
    for (int i = 0; i < 64; i++){
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + k_sha[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof a[0]);             /* h = g ... b = a; a[4] is now d */
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += a[i];
}
static void sha256(const uint8_t *p, uint32_t n, uint8_t out[32]){
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t tail[128] = { 0 };
    uint32_t full = n / 64 * 64, rest = n - full, tn = rest < 56 ? 64 : 128;
    for (uint32_t i = 0; i < full; i += 64) sha_block(h, p + i);
    if (rest) memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++) tail[tn - 1 - i] = (uint8_t)((uint64_t)n * 8 >> 8 * i);
    sha_block(h, tail);
    if (tn == 128) sha_block(h, tail + 64);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

/* ---- §5.1: text ---- */

static void ind(FILE *f, int lv){ for (int i = 0; i < lv; i++) fputs("  ", f); }
static void unk(FILE *f, uint32_t v){ fprintf(f, "UNKNOWN(0x%08x)", v); }
static void name_or_unk(FILE *f, const char *name, uint32_t v){ if (name) fputs(name, f); else unk(f, v); }

static void qstr(FILE *f, const uint8_t *p, uint32_t n){
    fputc('"', f);
    for (uint32_t i = 0; i < n; i++){
        uint8_t ch = p[i];
        if (ch == '"' || ch == '\\'){ fputc('\\', f); fputc(ch, f); }
        else if (ch >= 0x20 && ch <= 0x7E) fputc(ch, f);
        else fprintf(f, "\\x%02x", ch);
    }
    fputc('"', f);
}
static void qname(FILE *f, fx2_str s){ if (s.p) qstr(f, s.p, s.n); else fputc('-', f); }

/* Gp: C99 %.<p>g, with nan, inf and -inf spelled out */
static void gnum(FILE *f, double x, int prec){
    if (isnan(x)) fputs("nan", f);
    else if (isinf(x)) fputs(x < 0 ? "-inf" : "inf", f);
    else fprintf(f, "%.*g", prec, x);
}
static void f32(FILE *f, uint32_t w){ float x; memcpy(&x, &w, 4); fprintf(f, "0x%08x:", w); gnum(f, x, 9); }
static void f64(FILE *f, const uint8_t *p){
    uint64_t q = (uint64_t)fx2_rd32(p) | (uint64_t)fx2_rd32(p + 4) << 32;
    double x; memcpy(&x, &q, 8);
    fprintf(f, "0x%016llx:", (unsigned long long)q); gnum(f, x, 17);
}

/* ---- §5.4: types and values ---- */

static void tt(FILE *f, const fx2_type *t){
    name_or_unk(f, fx2_type_name(t->type), t->type);
    fputc(' ', f);
    name_or_unk(f, fx2_class_name(t->cls), t->cls);
    if (t->cls <= FX2_C_MATRIX_COLUMNS) fprintf(f, " %ux%u", t->rows, t->cols);
    else if (t->cls == FX2_C_STRUCT) fprintf(f, " %u", t->nmembers);
    if (t->elements) fprintf(f, " [%u]", t->elements);
}
static int simple(const fx2_value *v){ return v->kind == FX2_V_NUMERIC || v->kind == FX2_V_OBJECT; }

static void frag(FILE *f, const fx2_effect *fx, const fx2_value *v, uint32_t e){
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
        uint32_t w = fx2_rd32(q + 4 * i);
        if (i) fputc(' ', f);
        if (t->type == FX2_T_FLOAT) f32(f, w);
        else if (t->type == FX2_T_BOOL || t->type == FX2_T_INT) fprintf(f, "%d", (int32_t)w);
        else fprintf(f, "0x%08x", w);
    }
}
/* the end of an owner line: " = FRAG" for a simple value that is no array */
static void tail(FILE *f, const fx2_effect *fx, const fx2_value *v){
    if (simple(v) && !v->t->elements){ fputs(" = ", f); frag(f, fx, v, 0); }
}

static void states(FILE *f, const fx2_effect *fx, const fx2_state *s, uint32_t n, int lv);

/* the lines under an owner line, one level deeper */
static void value_lines(FILE *f, const fx2_effect *fx, const fx2_value *v, int lv){
    const fx2_type *t = v->t;
    int arr = t->elements != 0;
    if (simple(v) && !arr) return;
    for (uint32_t e = 0; e < v->n; e++){
        ind(f, lv);
        switch (v->kind){
        case FX2_V_NUMERIC: case FX2_V_OBJECT:
            fprintf(f, "value [%u] = ", e); frag(f, fx, v, e); fputc('\n', f);
            break;
        case FX2_V_SAMPLER:
            fputs("sampler", f);
            if (arr) fprintf(f, " [%u]", e);
            fprintf(f, " states %u\n", v->blocks[e].nstates);
            states(f, fx, v->blocks[e].states, v->blocks[e].nstates, lv + 1);
            break;
        case FX2_V_STRUCT:
            fputs("struct", f);
            if (arr) fprintf(f, " [%u]", e);
            fputc('\n', f);
            for (uint32_t m = 0; m < t->nmembers; m++){
                const fx2_value *mv = &v->members[(uint64_t)e * t->nmembers + m];
                ind(f, lv + 1);
                fprintf(f, "member %u ", m); tt(f, mv->t);
                fputs(" name ", f); qname(f, mv->t->name); fputs(" sem ", f); qname(f, mv->t->sem);
                tail(f, fx, mv); fputc('\n', f);
                value_lines(f, fx, mv, lv + 2);
            }
            break;
        default:
            fputs("value", f);
            if (arr) fprintf(f, " [%u]", e);
            fputs(" = ", f); unk(f, t->cls); fputc('\n', f);
        }
    }
}

/* ---- §5.5, §5.6: parameters, techniques, states ---- */

static void states(FILE *f, const fx2_effect *fx, const fx2_state *s, uint32_t n, int lv){
    for (uint32_t k = 0; k < n; k++, s++){
        ind(f, lv);
        fprintf(f, "state %u ", k);
        name_or_unk(f, fx2_state_name(s->op), s->op);
        fprintf(f, "[%u] op %u ", s->index, s->op);
        tt(f, s->t); tail(f, fx, &s->v);
        for (const fx2_res *r = s->res; r; r = r->next) fprintf(f, " res %u", (uint32_t)(r - fx->res));
        fputc('\n', f);
        value_lines(f, fx, &s->v, lv + 1);
    }
}

static void annots(FILE *f, const fx2_effect *fx, const fx2_annot *a, uint32_t n, int lv){
    for (uint32_t k = 0; k < n; k++, a++){
        ind(f, lv);
        fprintf(f, "annotation %u ", k); tt(f, a->t);
        fputs(" name ", f); qname(f, a->t->name); fputs(" sem ", f); qname(f, a->t->sem);
        tail(f, fx, &a->v); fputc('\n', f);
        value_lines(f, fx, &a->v, lv + 1);
    }
}

/* ---- §5.7: objects ---- */

/* lists in insertion order, one per object id: head/last per id, next per item */
typedef struct { uint32_t *head, *last, *next, *item, n, cap; int oom; } idlists;
static void il_init(idlists *l, uint32_t nids){
    memset(l, 0, sizeof *l);
    l->head = malloc(((size_t)nids + 1) * 4); l->last = malloc(((size_t)nids + 1) * 4);
    if (!l->head || !l->last){ l->oom = 1; return; }
    memset(l->head, 0xFF, ((size_t)nids + 1) * 4);
}
static void il_add(idlists *l, uint32_t id, uint32_t item){
    if (l->oom) return;
    if (l->n == l->cap){
        uint32_t cap = l->cap ? l->cap * 2 : 64;
        uint32_t *nx = realloc(l->next, (size_t)cap * 4), *it = nx ? realloc(l->item, (size_t)cap * 4) : NULL;
        if (nx) l->next = nx;
        if (!it){ l->oom = 1; return; }
        l->item = it; l->cap = cap;
    }
    l->next[l->n] = FX2_NONE; l->item[l->n] = item;
    if (l->head[id] == FX2_NONE) l->head[id] = l->n; else l->next[l->last[id]] = l->n;
    l->last[id] = l->n++;
}
static void il_free(idlists *l){ free(l->head); free(l->last); free(l->next); free(l->item); }

static int objects(FILE *f, const fx2_effect *fx){
    idlists od, rs;
    il_init(&od, fx->nobjects); il_init(&rs, fx->nobjects);
    for (uint32_t k = 0; k < fx->nodata; k++)
        if (fx->odata[k].id < fx->nobjects) il_add(&od, fx->odata[k].id, k);
    /* a resource on a state whose line ends in "= obj <id>" (an object
     * value that is no array; an array state gives its resources to none) */
    for (uint32_t r = 0; r < fx->nres; r++){
        const fx2_state *s = fx->res[r].target;
        if (!s || s->v.kind != FX2_V_OBJECT || s->t->elements) continue;
        uint32_t id = fx2_rd32(s->v.data);
        if (id < fx->nobjects) il_add(&rs, id, r);
    }
    int ok = !od.oom && !rs.oom;
    for (uint32_t id = 0; ok && id < fx->nobjects; id++){
        const fx2_object *o = &fx->objects[id];
        fprintf(f, "object %u ", id);
        if (o->held) name_or_unk(f, fx2_type_name(o->type), o->type); else fputc('-', f);
        for (uint32_t i = od.head[id]; i != FX2_NONE; i = od.next[i])
            fprintf(f, " string %u size %u", od.item[i], fx->odata[od.item[i]].size);
        for (uint32_t i = rs.head[id]; i != FX2_NONE; i = rs.next[i]) fprintf(f, " res %u", rs.item[i]);
        fputc('\n', f);
    }
    for (uint32_t k = 0; ok && k < fx->nodata; k++)            /* entries with no object id to go under */
        if (fx->odata[k].id >= fx->nobjects){ fputs("object ", f); unk(f, fx->odata[k].id); fprintf(f, " string %u size %u\n", k, fx->odata[k].size); }
    il_free(&od); il_free(&rs);
    return ok ? 0 : -1;
}

/* ---- §5.9, §5.10: shaders and preshaders ---- */

typedef struct { uint32_t (*c)[2]; uint32_t n, cap; int oom; } clist;   /* comments: dword index, L */
static void clist_add(void *ctx, uint32_t pos, uint32_t len){
    clist *l = ctx;
    if (l->oom) return;
    if (l->n == l->cap){
        uint32_t cap = l->cap ? l->cap * 2 : 8;
        void *c = realloc(l->c, (size_t)cap * sizeof l->c[0]);
        if (!c){ l->oom = 1; return; }
        l->c = c; l->cap = cap;
    }
    l->c[l->n][0] = pos; l->c[l->n][1] = len; l->n++;
}

/* the comment at dword pos: its FOURCC, or FX2_NONE when it has none inside the blob */
static uint32_t fourcc(const uint8_t *p, uint32_t ndw, uint32_t pos, uint32_t len){
    return fx2_comment_bytes(ndw, pos, len) >= 4 ? fx2_rd32(p + 4 * ((uint64_t)pos + 1)) : FX2_NONE;
}

static void comment_lines(FILE *f, const uint8_t *p, uint32_t ndw, const clist *cl, int lv){
    for (uint32_t k = 0; k < cl->n; k++){
        uint32_t pos = cl->c[k][0], len = cl->c[k][1];
        ind(f, lv);
        fputs("comment ", f);
        if (fx2_comment_bytes(ndw, pos, len) >= 4) qstr(f, p + 4 * ((uint64_t)pos + 1), 4); else fputs("\"\"", f);
        fprintf(f, " dwords %u\n", len);
    }
}

static void cstr(FILE *f, const fx2_ctab *c, uint32_t off){
    fx2_str s;
    int r = fx2_ctab_str(c, off, &s);
    if (r > 0) qstr(f, s.p, s.n); else if (r == 0) fputc('-', f); else unk(f, off);
}

/* a CTAB comment (L >= 8) at dword pos: the ctab line and its const lines */
static void ctab_lines(FILE *f, const uint8_t *p, uint32_t ndw, uint32_t pos, uint32_t len, int lv){
    uint32_t nb = fx2_comment_bytes(ndw, pos, len);
    fx2_ctab c;
    if (len < 8 || nb < 4 || fx2_ctab_init(&c, p + 4 * ((uint64_t)pos + 2), nb - 4)) return;
    ind(f, lv);
    fprintf(f, "ctab size %u creator ", c.size); cstr(f, &c, c.creator);
    fprintf(f, " version 0x%08x target ", c.version); cstr(f, &c, c.target);
    fprintf(f, " flags 0x%08x constants %u\n", c.flags, c.nconsts);
    for (uint32_t k = 0; k < c.nconsts; k++){
        fx2_const o;
        ind(f, lv);
        fprintf(f, "const %u ", k);
        if (fx2_ctab_const(&c, k, &o)){                     /* the entries stop at the first one outside */
            unk(f, (uint32_t)((uint64_t)c.cinfo + (uint64_t)k * 20)); fputc('\n', f);
            break;
        }
        cstr(f, &c, o.name); fputc(' ', f);
        if (o.set < 4) fputc("bics"[o.set], f); else unk(f, o.set);
        fprintf(f, "%u count %u ", o.reg, o.count);
        if (o.typed){
            name_or_unk(f, fx2_class_name(o.cls), o.cls); fputc(' ', f);
            name_or_unk(f, fx2_type_name(o.type), o.type);
            fprintf(f, " %ux%u elements %u members %u", o.rows, o.cols, o.elements, o.members);
        } else unk(f, o.typeinfo);
        fprintf(f, " default %s\n", o.defval ? "yes" : "no");
    }
}

static int shader_block(FILE *f, const uint8_t *p, uint32_t size, int lv){
    clist cl = { 0 };
    fx2_shader s;
    fx2_shader_walk(p, size, &s, clist_add, &cl);
    ind(f, lv);
    if (!s.known){
        fputs("shader ", f); unk(f, s.version); fprintf(f, " dwords %u\n", s.ndw);
        free(cl.c);
        return 0;
    }
    uint32_t major = s.version >> 8 & 0xFF, minor = s.version & 0xFF;
    fprintf(f, "shader %s_", s.version >> 16 == 0xFFFE ? "vs" : "ps");
    if (major == 2 && minor == 1) fputs("2_x", f); else fprintf(f, "%u_%u", major, minor);
    fprintf(f, " 0x%08x dwords %u instructions %u ", s.version, s.ndw, s.ninstr);
    if (s.ended) fputs("end", f); else unk(f, s.stop);
    fputc('\n', f);
    comment_lines(f, p, s.ndw, &cl, lv);
    for (uint32_t k = 0; k < cl.n; k++)
        if (fourcc(p, s.ndw, cl.c[k][0], cl.c[k][1]) == FX2_CTAB) ctab_lines(f, p, s.ndw, cl.c[k][0], cl.c[k][1], lv);
    free(cl.c);
    return cl.oom ? -1 : 0;
}

static void operand(FILE *f, const fx2_operand *o){
    name_or_unk(f, fx2_fxlc_table(o->table), o->table);
    fprintf(f, "[%u", o->offset);
    if (o->flag == 1){ fputc('+', f); name_or_unk(f, fx2_fxlc_table(o->itable), o->itable); fprintf(f, "[%u]", o->ioffset); }
    fputc(']', f);
}

/* the FXLC comment at dword pos: the instructions line, ins lines, trailer */
static void fxlc_lines(FILE *f, const uint8_t *p, uint32_t ndw, uint32_t pos, uint32_t len, int lv){
    const uint8_t *q = p + 4 * ((uint64_t)pos + 2);
    uint32_t nd = (fx2_comment_bytes(ndw, pos, len) - 4) / 4, tpos = nd;
    ind(f, lv);
    if (nd < 1){ fputs("instructions ", f); unk(f, len); fputc('\n', f); }
    else {
        uint32_t m = fx2_rd32(q), at = 1, k;
        fprintf(f, "instructions %u\n", m);
        for (k = 0; k < m; k++){
            fx2_ins in;
            fx2_fxlc_ins(q, nd, at, &in);
            ind(f, lv);
            fprintf(f, "ins %u ", k);
            if (in.status == FX2_INS_SHORT){ unk(f, nd - at); fputc('\n', f); break; }
            name_or_unk(f, fx2_fxlc_name(in.opcode, in.nin), in.opcode);
            if (in.scalar) fputs(".s", f);
            fprintf(f, " %u", in.ncomp);
            if (in.token >> 16 & 0xF){ fputc(' ', f); unk(f, in.token); }
            if (in.status == FX2_INS_FLAG){ fputc(' ', f); unk(f, in.flag); fputc('\n', f); tpos = in.end; break; }
            fputc(' ', f); operand(f, &in.out);
            for (uint32_t i = 0, o = in.in; i < in.nin; i++){
                fx2_operand op;
                o = fx2_fxlc_operand(q, nd, o, &op);
                fputc(' ', f); operand(f, &op);
            }
            fputc('\n', f);
            at = in.end;
        }
        if (k == m) tpos = at;
    }
    ind(f, lv);
    fputs("trailer", f);
    for (uint32_t i = tpos; i < nd; i++) fprintf(f, " 0x%08x", fx2_rd32(q + 4 * (uint64_t)i));
    fputc('\n', f);
}

static int pres_block(FILE *f, const uint8_t *p, uint32_t size, int lv){
    clist cl = { 0 };
    fx2_pres ps;
    fx2_pres_read(p, size, &ps, clist_add, &cl);
    ind(f, lv);
    if (!ps.known){
        fputs("preshader ", f); unk(f, ps.version); fprintf(f, " dwords %u\n", ps.ndw);
        free(cl.c);
        return 0;
    }
    fprintf(f, "preshader 0x%08x dwords %u ", ps.version, ps.ndw);
    if (ps.ended) fputs("end", f); else unk(f, ps.stop);
    fputc('\n', f);
    comment_lines(f, p, ps.ndw, &cl, lv);
    if (ps.ctab) ctab_lines(f, p, ps.ndw, ps.ctab, ps.ctab_len, lv);
    ind(f, lv);
    if (!ps.clit) fputs("literals -\n", f);
    else {
        uint32_t count;
        const uint8_t *lits;
        int r = fx2_clit(p + 4 * ((uint64_t)ps.clit + 2), fx2_comment_bytes(ps.ndw, ps.clit, ps.clit_len) - 4, &count, &lits);
        fputs("literals ", f);
        if (r == -1){ unk(f, ps.clit_len); fputc('\n', f); }
        else if (r == -2){ unk(f, count); fputc('\n', f); }
        else {
            fprintf(f, "%u\n", count);
            for (uint32_t k = 0; k < count; k++){ ind(f, lv); fprintf(f, "lit %u ", k); f64(f, lits + 8 * (uint64_t)k); fputc('\n', f); }
        }
    }
    if (ps.fxlc) fxlc_lines(f, p, ps.ndw, ps.fxlc, ps.fxlc_len, lv);
    else { ind(f, lv); fputs("instructions -\n", f); }
    free(cl.c);
    return cl.oom ? -1 : 0;
}

/* ---- §5.8: resources ---- */

static int resource(FILE *f, const fx2_effect *fx, uint32_t k){
    const fx2_res *r = &fx->res[k];
    if (r->tech != FX2_NONE)
        fprintf(f, "resource %u technique %u pass %u state %u usage %u size %u\n", k, r->tech, r->index, r->state, r->usage, r->size);
    else {
        fprintf(f, "resource %u param %u element ", k, r->index);
        if (r->element == FX2_NONE) fputc('-', f);
        else if (r->target && r->element && !fx->params[r->index].t->elements) unk(f, r->element);   /* D3DX ignores it */
        else fprintf(f, "%u", r->element);
        fprintf(f, " state %u usage %u size %u\n", r->state, r->usage, r->size);
    }
    uint32_t ty = r->target ? r->target->t->type : FX2_NONE;
    if (r->target && r->usage == 0 && (ty == FX2_T_VERTEXSHADER || ty == FX2_T_PIXELSHADER))
        return shader_block(f, r->data, r->size, 1);
    if (r->target && r->usage == 0 && (ty == FX2_T_BOOL || ty == FX2_T_INT || ty == FX2_T_FLOAT || ty == FX2_T_STRING))
        return pres_block(f, r->data, r->size, 1);
    ind(f, 1);
    if (r->target && r->usage == 1){
        const uint8_t *z = r->size ? memchr(r->data, 0, r->size) : NULL;
        fputs("name ", f); qstr(f, r->data, z ? (uint32_t)(z - r->data) : r->size); fputc('\n', f);
        return 0;
    }
    if (r->target && r->usage == 2){
        fputs("select ", f);
        if (r->size < 4){ unk(f, r->size); fputc('\n', f); return 0; }   /* no room for S2 */
        uint32_t s2 = fx2_rd32(r->data);
        if ((uint64_t)4 + s2 > r->size){ unk(f, s2); fputc('\n', f); return 0; }
        const uint8_t *z = s2 ? memchr(r->data + 4, 0, s2) : NULL;
        qstr(f, r->data + 4, z ? (uint32_t)(z - r->data - 4) : s2); fputc('\n', f);
        return pres_block(f, r->data + 4 + s2, r->size - 4 - s2, 1);
    }
    unk(f, r->usage); fputc('\n', f);
    return 0;
}

/* ---- §5.2: the dump ---- */

int fxdump(FILE *f, const char *name, const uint8_t *buf, uint32_t size, char *err, uint32_t errlen){
    fx2_effect fx;
    if (fx2_parse(&fx, buf, size, err, errlen)) return -1;
    uint8_t sum[32];
    sha256(buf, size, sum);
    fputs("fx2dump 1\neffect ", f); qstr(f, (const uint8_t *)name, (uint32_t)strlen(name));
    fprintf(f, " size %u sha256 ", size);
    for (int i = 0; i < 32; i++) fprintf(f, "%02x", sum[i]);
    fprintf(f, "\nheader tag 0x%08x tables 0x%08x data0 0x%08x params %u techniques %u shaders %u objects %u strings %u resources %u\n",
            fx.tag, fx.tables, fx.data0, fx.nparams, fx.ntechs, fx.shaders, fx.nobjects, fx.nodata, fx.nres);
    for (uint32_t i = 0; i < fx.nparams; i++){
        const fx2_param *pa = &fx.params[i];
        fprintf(f, "param %u ", i); tt(f, pa->t);
        fputs(" name ", f); qname(f, pa->t->name); fputs(" sem ", f); qname(f, pa->t->sem);
        fprintf(f, " flags 0x%08x annotations %u", pa->flags, pa->nannots);
        tail(f, &fx, &pa->v); fputc('\n', f);
        value_lines(f, &fx, &pa->v, 1);
        annots(f, &fx, pa->annots, pa->nannots, 1);
    }
    for (uint32_t i = 0; i < fx.ntechs; i++){
        const fx2_tech *te = &fx.techs[i];
        fprintf(f, "technique %u name ", i); qname(f, te->name);
        fprintf(f, " annotations %u passes %u\n", te->nannots, te->npasses);
        annots(f, &fx, te->annots, te->nannots, 1);
        for (uint32_t j = 0; j < te->npasses; j++){
            const fx2_pass *ps = &te->passes[j];
            ind(f, 1);
            fprintf(f, "pass %u name ", j); qname(f, ps->name);
            fprintf(f, " annotations %u states %u\n", ps->nannots, ps->nstates);
            annots(f, &fx, ps->annots, ps->nannots, 2);
            states(f, &fx, ps->states, ps->nstates, 2);
        }
    }
    int r = objects(f, &fx);
    for (uint32_t k = 0; k < fx.nres; k++) r |= resource(f, &fx, k);
    fputs("end\n", f);
    fx2_free(&fx);
    if (r && err && errlen) snprintf(err, errlen, "out of memory while writing the dump");
    return r ? -1 : 0;
}

#ifndef FXDUMP_NO_MAIN
int main(int argc, char **argv){
    if (argc < 2 || argc > 3){ fprintf(stderr, "usage: fxdump <NAME>.fxo [<out.txt>]\n"); return 2; }
    FILE *in = fopen(argv[1], "rb");
    if (!in){ perror(argv[1]); return 1; }
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0, got;
    do {
        if (n == cap){
            cap = cap ? cap * 2 : 65536;
            uint8_t *b = cap <= 0xFFFFFFFFu ? realloc(buf, cap) : NULL;
            if (!b){ fprintf(stderr, "fxdump: %s: too large\n", argv[1]); return 1; }
            buf = b;
        }
        got = fread(buf + n, 1, cap - n, in);
        n += got;
    } while (got);
    if (ferror(in)){ perror(argv[1]); return 1; }
    fclose(in);

    const char *base = strrchr(argv[1], '/');
    base = base ? base + 1 : argv[1];
    char name[1024];
    snprintf(name, sizeof name, "%s", base);
    size_t nl = strlen(name);
    if (nl > 4 && !strcmp(name + nl - 4, ".fxo")) name[nl - 4] = 0;

    /* into <out>.tmp, renamed when it is whole */
    char tmp[4096], err[512];
    FILE *f = stdout;
    if (argc == 3){
        snprintf(tmp, sizeof tmp, "%s.tmp", argv[2]);
        if (!(f = fopen(tmp, "w"))){ perror(tmp); return 1; }
    }
    err[0] = 0;
    int r = fxdump(f, name, buf, (uint32_t)n, err, sizeof err);
    free(buf);
    if (r){ fprintf(stderr, "fxdump: %s: %s\n", argv[1], err); if (argc == 3){ fclose(f); remove(tmp); } return 1; }
    if (argc == 3){
        if (fclose(f)){ perror(tmp); remove(tmp); return 1; }
        if (rename(tmp, argv[2])){ perror(argv[2]); remove(tmp); return 1; }
    } else if (fflush(stdout)){ perror("stdout"); return 1; }
    return 0;
}
#endif
