/* fxmodeldump.c -- the canonical model dump ("fxmodel 1",
 * docs/mw/fx-model.md §5) of a set of compiled D3DX effects created with
 * one effect pool, built with runtime/mw/fxmodel.c on runtime/mw/fx2.c:
 * implementation A of the dump (B is tools/fxmodel.py).
 *
 *   clang -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -Iruntime/mw \
 *         -o out/mw/fx/bin/fxmodeldump tools/mw/fxmodeldump.c runtime/mw/fxmodel.c runtime/mw/fx2.c
 *   out/mw/fx/bin/fxmodeldump [--ops] <out.txt|-> <NAME>.fxo...
 *
 * NAME is a file name without .fxo; the effects are created in byte order
 * of NAME (strcmp) and all stay alive while the dump is written, before any
 * Set*.  An effect the parser refuses gives its message on stderr, exit
 * status 1 and no output file.  tools/mw/fxdump.sh --model builds it and
 * dumps all 31 into out/mw/fx/ma/all.txt.  With --ops it writes the
 * Set/Get/lookup trace instead ("fxmodel ops 1", below), which
 * tools/fxmodel.py --ops writes too: the two must be the same byte for byte.
 *
 * With -DFXMODELDUMP_NO_MAIN this file is fxmodel_dump() alone; with
 * -DFXMODEL_FUZZ as well it also defines fxdump() as tools/mw/fxfuzz.c
 * calls it: the model of each corrupted copy, twice in one pool, dumped,
 * then every Set, Get and lookup on every node, then freed and made again.
 */
#include "fxmodel.h"
#include "fxtext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {                 /* one effect of the set */
    const char *name;            /* NAME */
    const uint8_t *buf;
    uint32_t size;
    fx2_effect fx;
    fxm_effect *m;
} mfx;

int fxmodel_dump(FILE *f, const mfx *x, uint32_t n, const fxm_pool *pool);

static const char *num_name(uint32_t t){ return t == FX2_T_BOOL ? "bool" : t == FX2_T_INT ? "int" : t == FX2_T_FLOAT ? "float" : NULL; }

/* ---- §5.1: TYPED (an annotation's value) ---- */

static void typed(FILE *f, const mfx *x, uint32_t id){
    const fxm_node *nd = fxm_node_get(x->m, id);
    const fx2_annot *a = NULL;
    const fxm_node *o = fxm_node_get(x->m, nd->parent);
    if (o->kind == FXM_K_PARAM) a = &x->fx.params[o->index].annots[nd->index];
    else if (o->kind == FXM_K_TECH) a = &x->fx.techs[o->index].annots[nd->index];
    else a = &x->fx.techs[fxm_node_get(x->m, o->parent)->index].passes[o->index].annots[nd->index];
    const fx2_value *v = &a->v;
    if (nd->cls <= FX2_C_MATRIX_COLUMNS){
        const char *w = num_name(nd->type);
        fputs(w ? w : "raw", f);
        fputc(' ', f);
        val(f, nd->type, fxm_data(x->m, id), nd->bytes / 4);
        return;
    }
    if (v->kind == FX2_V_OBJECT){
        for (uint32_t e = 0; e < v->n; e++){
            uint32_t oid = fx2_rd32(v->data + 4 * (uint64_t)e);
            if (e) fputc(' ', f);
            if (nd->type == FX2_T_STRING){
                const fx2_odata *od = NULL;
                for (uint32_t k = 0; oid < x->fx.nobjects && k < x->fx.nodata && !od; k++)
                    if (x->fx.odata[k].id == oid) od = &x->fx.odata[k];
                fputs("string ", f);
                if (!od){ unk(f, oid); continue; }
                const uint8_t *z = od->size ? memchr(od->data, 0, od->size) : NULL;
                qstr(f, od->data, z ? (uint32_t)(z - od->data) : od->size);
            } else {
                fputs("obj ", f);
                if (oid < x->fx.nobjects) fprintf(f, "%u", oid); else unk(f, oid);
            }
        }
        return;
    }
    unk(f, nd->cls);                                           /* a sampler, a struct or an unknown class */
}

static void annots(FILE *f, const mfx *x, uint32_t owner, int lv){
    for (uint32_t k = 0, id; (id = fxm_annot(x->m, owner, k)); k++){
        const fxm_node *nd = fxm_node_get(x->m, id);
        ind(f, lv);
        fprintf(f, "annotation #%u %u name ", id, k); qname(f, nd->name);
        fputc(' ', f); tt(f, nd->t);
        fprintf(f, " flags 0x%08x = ", nd->flags);
        typed(f, x, id);
        fputc('\n', f);
    }
}

/* ---- §5.6, §5.7: states and constants ---- */

static void consts(FILE *f, const fxm_prog *pr, int lv){
    for (uint32_t k = 0; k < pr->nread; k++){
        const fxm_const *q = &pr->consts[k];
        ind(f, lv);
        fprintf(f, "const %u ", k);
        if (!q->c.name) fputc('-', f); else if (q->name.p) qstr(f, q->name.p, q->name.n); else unk(f, q->c.name);
        fputc(' ', f);
        if (q->c.set < 4) fputc("bics"[q->c.set], f); else unk(f, q->c.set);
        fprintf(f, "%u count %u -> ", q->c.reg, q->c.count);
        if (q->why) unk(f, q->why);
        else if (q->layout == FXM_L_SAMPLER) fprintf(f, "#%u sampler stage %u elements %u/%u", q->id, q->stage, q->used, q->nelem);
        else {
            static const char *const lay[] = { "scalar", "vector", "rows", "columns" };
            fprintf(f, "#%u %s", q->id, num_name(q->from));
            if (q->from != q->to) fprintf(f, ">%s", num_name(q->to));
            fprintf(f, " %s per %u elements %u/%u", lay[q->layout], q->per, q->used, q->nelem);
        }
        fputc('\n', f);
    }
    if (pr->ctab && pr->nread < pr->nconsts){ ind(f, lv); fprintf(f, "const %u ", pr->nread); unk(f, 6); fputc('\n', f); }
}

static void states(FILE *f, const mfx *x, const fx2_state *s, uint32_t n, int lv){
    for (uint32_t k = 0; k < n; k++, s++){
        const fx2_state_info *si = fx2_state_get(s->op);
        ind(f, lv);
        fprintf(f, "state %u ", k);
        if (si) fprintf(f, "%s[%u] %s %u ", si->name, s->index, fx2_sclass_name(si->cls), si->d3d);
        else { unk(f, s->op); fprintf(f, "[%u] - - ", s->index); }
        fxm_source src = fxm_state_source(x->m, s);
        const fxm_prog *pr = NULL;
        switch (src.src){
        case FXM_S_LITERAL:
            fputs("literal ", f);
            if ((s->v.kind == FX2_V_NUMERIC || s->v.kind == FX2_V_OBJECT) && !s->t->elements){ fputs("= ", f); frag(f, &x->fx, &s->v, 0); }
            else unk(f, s->t->cls);
            break;
        case FXM_S_PRESHADER:
            pr = src.prog;
            fprintf(f, "preshader res %u outputs %llu ctab ", (uint32_t)(pr->res - x->fx.res),
                    s->t->type == FX2_T_STRING ? 1ull : (unsigned long long)s->t->rows * s->t->cols);
            if (pr->ctab) fprintf(f, "%u", pr->nconsts); else fputc('-', f);
            break;
        case FXM_S_SHADER: {
            char vn[32];
            pr = src.prog;
            fprintf(f, "shader res %u ", (uint32_t)(pr->res - x->fx.res));
            if (!vname(vn, sizeof vn, pr->version)){ unk(f, pr->version); pr = NULL; break; }
            fprintf(f, "%s ctab ", vn);
            if (pr->ctab) fprintf(f, "%u", pr->nconsts); else fputc('-', f);
            break; }
        case FXM_S_PARAM:
            if (src.id){ fprintf(f, "param #%u", src.id); break; }
            fputs("param ", f); unk(f, 1); fputs(" name ", f);
            {
                const fx2_res *r = src.res;
                const uint8_t *z = r->size ? memchr(r->data, 0, r->size) : NULL;
                qstr(f, r->data, z ? (uint32_t)(z - r->data) : r->size);
            }
            break;
        default:
            unk(f, src.res->usage);
        }
        if (src.nres > 1){ fputc(' ', f); unk(f, src.nres); }
        fputc('\n', f);
        if (pr) consts(f, pr, lv + 1);
    }
}

/* ---- §5.4: parameter blocks ---- */

static void value_node(FILE *f, const mfx *x, uint32_t id, int lv){
    const fxm_node *nd = fxm_node_get(x->m, id);
    uint32_t vk = fx2_value_kind(nd->t);
    ind(f, lv);
    if (nd->kind == FXM_K_PARAM){
        fprintf(f, "param #%u %u path ", id, nd->index); qstr(f, nd->path.p, nd->path.n);
        fputc(' ', f); tt(f, nd->t);
        fputs(" sem ", f); qname(f, nd->sem);
        fprintf(f, " flags 0x%08x bytes %u shared ", nd->flags, nd->bytes);
        uint32_t s = fxm_shared_slot(x->m, id);
        if (s) fprintf(f, "%u", s - 1); else fputc('-', f);
        fprintf(f, " annotations %u", nd->nannots);
    } else if (nd->kind == FXM_K_ELEMENT){
        fprintf(f, "element #%u %u path ", id, nd->index); qstr(f, nd->path.p, nd->path.n);
        fputc(' ', f); tt_el(f, nd->t, 0);
        fprintf(f, " flags 0x%08x bytes %u", nd->flags, nd->bytes);
    } else {
        fprintf(f, "member #%u %u path ", id, nd->index); qstr(f, nd->path.p, nd->path.n);
        fputc(' ', f); tt(f, nd->t);
        fputs(" name ", f); qname(f, nd->name); fputs(" sem ", f); qname(f, nd->sem);
        fprintf(f, " flags 0x%08x bytes %u", nd->flags, nd->bytes);
    }
    if (!nd->elements){                                        /* a leaf with storage: its value */
        const uint32_t *d = fxm_data(x->m, id);
        if (vk == FX2_V_NUMERIC){ fputs(" = ", f); val(f, nd->type, d, (uint64_t)nd->rows * nd->cols); }
        else if (vk == FX2_V_OBJECT && nd->bytes){                /* an object of another type has none (§2.4) */
            fputs(" = obj ", f);
            if (nd->object < x->fx.nobjects) fprintf(f, "%u", nd->object); else unk(f, nd->object);
            fprintf(f, " data 0x%08x", d[0]);
        } else if (vk == FX2_V_UNKNOWN){ fputs(" = ", f); unk(f, nd->cls); }
    }
    fputc('\n', f);
    if (vk == FX2_V_SAMPLER && !nd->elements){
        ind(f, lv + 1);
        fprintf(f, "sampler states %u\n", nd->block->nstates);
        states(f, x, nd->block->states, nd->block->nstates, lv + 2);
    }
    if (nd->elements)
        for (uint32_t i = 0, c; (c = fxm_element(x->m, id, i)); i++) value_node(f, x, c, lv + 1);
    else if (nd->cls == FX2_C_STRUCT)
        for (uint32_t i = 0, c; (c = fxm_param(x->m, id, i)); i++) value_node(f, x, c, lv + 1);
    if (nd->kind == FXM_K_PARAM) annots(f, x, id, lv + 1);
}

/* ---- §5.2, §5.3, §5.5, §5.8: the dump ---- */

static int same_name(fx2_str a, fx2_str b){ return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }

int fxmodel_dump(FILE *f, const mfx *x, uint32_t n, const fxm_pool *pool){
    fprintf(f, "fxmodel 1\neffects %u\n", n);
    for (uint32_t i = 0; i < n; i++){
        const mfx *e = &x[i];
        char hex[65];
        sha256_hex(e->buf, e->size, hex);
        fprintf(f, "effect %u ", i); qstr(f, (const uint8_t *)e->name, (uint32_t)strlen(e->name));
        fprintf(f, " size %u sha256 %s\n", e->size, hex);
        uint32_t shared = 0;
        for (uint32_t p = 0; p < e->fx.nparams; p++) shared += fxm_shared_slot(e->m, fxm_param(e->m, 0, p)) != 0;
        fprintf(f, "desc parameters %u techniques %u nodes %u shared %u\n", e->fx.nparams, e->fx.ntechs, fxm_nodes(e->m), shared);
        for (uint32_t p = 0; p < e->fx.nparams; p++) value_node(f, e, fxm_param(e->m, 0, p), 1);
        for (uint32_t t = 0; t < e->fx.ntechs; t++){
            uint32_t tid = fxm_tech(e->m, t), vs, ps;
            const fxm_node *tn = fxm_node_get(e->m, tid);
            const fx2_tech *te = &e->fx.techs[t];
            char vn[32];
            fxm_tech_versions(e->m, tid, &vs, &ps);
            ind(f, 1);
            fprintf(f, "technique #%u %u name ", tid, t); qname(f, tn->name);
            fprintf(f, " passes %u annotations %u vs %s", te->npasses, te->nannots, vname(vn, sizeof vn, vs) ? vn : "-");
            fprintf(f, " ps %s\n", vname(vn, sizeof vn, ps) ? vn : "-");
            annots(f, e, tid, 2);
            for (uint32_t j = 0; j < te->npasses; j++){
                uint32_t pid = fxm_pass(e->m, tid, j);
                const uint32_t *deps;
                uint32_t nd = fxm_pass_deps(e->m, pid, &deps);
                ind(f, 2);
                fprintf(f, "pass #%u %u name ", pid, j); qname(f, fxm_node_get(e->m, pid)->name);
                fprintf(f, " states %u annotations %u deps %u", te->passes[j].nstates, te->passes[j].nannots, nd);
                for (uint32_t k = 0; k < nd; k++) fprintf(f, " #%u", deps[k]);
                fputc('\n', f);
                annots(f, e, pid, 3);
                states(f, e, te->passes[j].states, te->passes[j].nstates, 3);
            }
        }
    }

    /* §5.8: live slots in number order; holders and apart parameters in input order, then id order */
    uint32_t live = 0, ns = fxm_pool_slots(pool);
    for (uint32_t s = 0; s < ns; s++){
        int any = 0;
        for (uint32_t i = 0; i < n && !any; i++)
            for (uint32_t p = 0; p < x[i].fx.nparams && !any; p++) any = fxm_shared_slot(x[i].m, fxm_param(x[i].m, 0, p)) == s + 1;
        live += any;
    }
    fprintf(f, "pool slots %u\n", live);
    for (uint32_t s = 0; s < ns; s++){
        const mfx *h0 = NULL;
        uint32_t id0 = 0, holders = 0;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t p = 0; p < x[i].fx.nparams; p++){
                uint32_t id = fxm_param(x[i].m, 0, p);
                if (fxm_shared_slot(x[i].m, id) != s + 1) continue;
                if (!holders++){ h0 = &x[i]; id0 = id; }
            }
        if (!holders) continue;
        const fxm_node *nd = fxm_node_get(h0->m, id0);
        ind(f, 1);
        fprintf(f, "slot %u name ", s); qname(f, nd->name);
        fputc(' ', f); tt(f, nd->t);
        fprintf(f, " bytes %u holders %u = ", nd->bytes, holders);
        const uint32_t *d = fxm_data(h0->m, id0);
        if (nd->cls == FX2_C_OBJECT){ fputs("data ", f); val(f, FX2_T_VOID, d, nd->bytes / 4); }
        else val(f, nd->type, d, nd->bytes / 4);
        fputc('\n', f);
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t p = 0; p < x[i].fx.nparams; p++){
                uint32_t id = fxm_param(x[i].m, 0, p);
                if (fxm_shared_slot(x[i].m, id) != s + 1) continue;
                ind(f, 2);
                fprintf(f, "holder %u ", i); qstr(f, (const uint8_t *)x[i].name, (uint32_t)strlen(x[i].name));
                fprintf(f, " #%u\n", id);
            }
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t p = 0; p < x[i].fx.nparams; p++){
                uint32_t id = fxm_param(x[i].m, 0, p);
                const fxm_node *q = fxm_node_get(x[i].m, id);
                if (fxm_shared_slot(x[i].m, id) == s + 1 || !same_name(q->name, nd->name)) continue;
                ind(f, 2);
                fprintf(f, "apart %u ", i); qstr(f, (const uint8_t *)x[i].name, (uint32_t)strlen(x[i].name));
                fprintf(f, " #%u flags 0x%08x\n", id, q->flags);
            }
    }
    fputs("end\n", f);
    return ferror(f) ? -1 : 0;
}

#ifdef FXMODEL_FUZZ
/* ---- the fuzz entry (tools/mw/fxfuzz.c's fxdump) ---- */

int fxdump(FILE *f, const char *name, const uint8_t *buf, uint32_t size, char *err, uint32_t errlen);

static void count_drop(void *ctx, uint32_t ptr){ (void)ptr; (*(unsigned long *)ctx)++; }

/* copies the parser took but the model refused (its memory bound): none expected */
static unsigned long g_model_refused;
static void report(void){ fprintf(stderr, "fxmodelfuzz: %lu parsed copies had no model (the memory bound)\n", g_model_refused); }

/* every Set, Get and lookup on every node; a lookup that does not find a
 * node by its own path is a model bug and stops the run */
static void exercise(fxm_effect *m){
    uint32_t nn = fxm_nodes(m);
    for (uint32_t id = 0; id <= nn + 1; id++){
        const fxm_node *nd = fxm_node_get(m, id);
        uint64_t room = 64 + (nd ? (uint64_t)nd->bytes / 4 + 16 * ((uint64_t)nd->elements + 1) : 0);
        uint32_t *b = calloc((size_t)room, 4), v4[4] = { 0x3F800000u, 0xBF000000u, 0x7FC00000u, 0x4F000000u }, m16[16];
        if (!b){ fprintf(stderr, "fxmodelfuzz: out of memory\n"); exit(1); }
        for (int i = 0; i < 16; i++) m16[i] = 0x40000000u + (uint32_t)i;
        for (uint64_t i = 0; i < room; i++) b[i] = (uint32_t)(i * 0x9E3779B9u);
        int ch;
        uint32_t old, w, ec = nd ? nd->elements : 0;
        fxm_set_value(m, id, b, (uint32_t)(room * 4), &ch);
        fxm_set_bool(m, id, 5, &ch); fxm_set_int(m, id, 0x80FF4020u, &ch); fxm_set_float(m, id, 0xC0490FDBu, &ch);
        for (uint32_t t = FX2_T_BOOL; t <= FX2_T_FLOAT; t++) fxm_set_array(m, id, t, b, (uint32_t)room, &ch);
        fxm_set_vector(m, id, v4, &ch); fxm_set_vector_array(m, id, b, ec, &ch);
        fxm_set_matrix(m, id, m16, 0, &ch); fxm_set_matrix(m, id, m16, 1, &ch);
        fxm_set_matrix_array(m, id, b, ec < (room - 64) / 16 ? ec : 0, 1, &ch);
        fxm_set_object(m, id, 0x13570000u + id, &old, &ch);
        fxm_get_value(m, id, b, (uint32_t)(room * 4));
        fxm_get_bool(m, id, &w); fxm_get_int(m, id, &w); fxm_get_float(m, id, &w);
        for (uint32_t t = FX2_T_BOOL; t <= FX2_T_FLOAT; t++) fxm_get_array(m, id, t, b, (uint32_t)room);
        fxm_get_vector(m, id, v4); fxm_get_vector_array(m, id, b, ec < (room - 64) / 4 ? ec : 0);
        fxm_get_matrix(m, id, m16, 0); fxm_get_matrix(m, id, m16, 1);
        fxm_get_matrix_array(m, id, b, ec < (room - 64) / 16 ? ec : 0, 0);
        fxm_get_object(m, id, &w);
        fxm_data(m, id); fxm_version(m, id); fxm_shared_slot(m, id);
        free(b);
        if (!nd) continue;
        fxm_param(m, id, 0); fxm_element(m, id, 0); fxm_annot(m, id, 0); fxm_pass(m, id, 0);
        fxm_by_semantic(m, 0, nd->sem.p, nd->sem.n); fxm_by_semantic(m, id, NULL, 0);
        fxm_annot_by_name(m, id, nd->name.p, nd->name.n); fxm_pass_by_name(m, id, nd->name.p, nd->name.n);
        fxm_tech_by_name(m, nd->name.p, nd->name.n); fxm_pass_deps(m, id, NULL); fxm_tech_versions(m, id, &w, &old);
        if (nd->kind <= FXM_K_MEMBER && nd->path.n){                  /* "" names nothing (fx-model §2.3) */
            uint32_t got = fxm_by_name(m, 0, nd->path.p, nd->path.n);
            const fxm_node *g = fxm_node_get(m, got);
            if (!g || g->path.n != nd->path.n || memcmp(g->path.p, nd->path.p, nd->path.n) || got > id){
                fprintf(stderr, "fxmodelfuzz: node #%u is not found by its path\n", id);
                abort();
            }
            if (nd->parent && nd->name.n && !fxm_by_name(m, nd->parent, nd->name.p, nd->name.n) && nd->kind == FXM_K_MEMBER){
                fprintf(stderr, "fxmodelfuzz: member #%u is not found under its parent\n", id);
                abort();
            }
        }
    }
}

int fxdump(FILE *f, const char *name, const uint8_t *buf, uint32_t size, char *err, uint32_t errlen){
    mfx x[2];
    memset(x, 0, sizeof x);
    if (fx2_parse(&x[0].fx, buf, size, err, errlen)) return -1;
    x[1].fx = x[0].fx;
    x[0].name = x[1].name = name; x[0].buf = x[1].buf = buf; x[0].size = x[1].size = size;
    fxm_pool *pool = fxm_pool_new();
    unsigned long drops = 0;
    int r = -1;
    static int once;
    if (!once++) atexit(report);
    if (!pool || fxm_effect_new(&x[0].m, &x[0].fx, pool, err, errlen) || fxm_effect_new(&x[1].m, &x[1].fx, pool, err, errlen)){
        g_model_refused++;
        goto out;
    }
    if (fxmodel_dump(f, x, 2, pool)) goto out;
    exercise(x[0].m);
    fxm_effect_free(x[0].m, count_drop, &drops);              /* the second keeps the slots */
    if (fxm_effect_new(&x[0].m, &x[0].fx, pool, err, errlen)) goto out;
    exercise(x[1].m);
    fxm_effect_free(x[1].m, count_drop, &drops); x[1].m = NULL;
    fxm_effect *solo;                                          /* and one without a pool */
    if (fxm_effect_new(&solo, &x[0].fx, NULL, err, errlen)) goto out;
    exercise(solo);
    fxm_effect_free(solo, count_drop, &drops);
    r = 0;
out:
    fxm_effect_free(x[0].m, count_drop, &drops);
    fxm_effect_free(x[1].m, count_drop, &drops);
    fxm_pool_free(pool);
    fx2_free(&x[0].fx);
    return r;
}
#endif

#ifndef FXMODELDUMP_NO_MAIN
/* ---- --ops: the Set/Get/lookup trace (tools/fxmodel.py --ops writes the same) ----
 * Every node id 0..N+1 of every effect in input order, all in one pool:
 * each Set*, Get* and lookup with fixed inputs, its result, changed flag,
 * the node's version and what it read; then the first effect freed and
 * made again, and every effect freed, with the dropped pointers.  Versions
 * are shared through the pool, so a write through one holder shows in the
 * others' later lines. */

static const uint32_t k_pat[12] = { 0x3FF33333u, 0xBFF33333u, 0x7FC00000u, 0x4F32D05Eu, 0x80000000u, 5u,
                                    0xFFFFFFFFu, 0x3F800000u, 0u, 0x40490FDBu, 0x437F0000u, 0x3F000000u };
static uint32_t pat(uint32_t k){ return k_pat[k % 12] ^ (k / 12); }

static void o_set(FILE *f, const fxm_effect *m, uint32_t id, uint32_t hr, int ch){
    fprintf(f, " -> 0x%08x ch %d v %llu\n", hr, hr ? 0 : ch, (unsigned long long)fxm_version(m, id));
}
static void o_get(FILE *f, uint32_t hr, const uint32_t *w, uint64_t n){
    fprintf(f, " -> 0x%08x", hr);
    if (!hr){ fputs(" =", f); for (uint64_t i = 0; i < n; i++) fprintf(f, " %08x", w[i]); }
    fputc('\n', f);
}
static void o_id(FILE *f, const char *what, uint32_t r){ fprintf(f, "  %s -> #%u\n", what, r); }

typedef struct { uint32_t *p; uint32_t n, cap; } droplist;
static void collect(void *ctx, uint32_t ptr){
    droplist *d = ctx;
    if (d->n == d->cap){
        uint32_t cap = d->cap ? d->cap * 2 : 16, *p = realloc(d->p, (size_t)cap * 4);
        if (!p){ fprintf(stderr, "fxmodeldump: out of memory\n"); exit(1); }
        d->p = p; d->cap = cap;
    }
    d->p[d->n++] = ptr;
}
static int cmp_drop(const void *a, const void *b){
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void free_traced(FILE *f, mfx *x, uint32_t i){
    droplist d = { NULL, 0, 0 };
    fxm_effect_free(x[i].m, collect, &d);
    x[i].m = NULL;
    qsort(d.p, d.n, 4, cmp_drop);
    fprintf(f, "free %u drops %u", i, d.n);
    for (uint32_t k = 0; k < d.n; k++) fprintf(f, " %08x", d.p[k]);
    fputc('\n', f);
    free(d.p);
}

static void ops_node(FILE *f, fxm_effect *m, uint32_t id){
    const fxm_node *nd = fxm_node_get(m, id);
    uint32_t W = nd ? nd->bytes / 4 : 0, E = nd ? (nd->elements < 64 ? nd->elements : 64) : 0;
    uint32_t A = W + 1 < 64 ? W + 1 : 64, room = (W > 16 * (E + 1) ? W : 16 * (E + 1)) + 64;
    uint32_t *src = calloc(room, 4), *dst = calloc(room, 4), w = 0, old = 0;
    if (!src || !dst){ fprintf(stderr, "fxmodeldump: out of memory\n"); exit(1); }
    for (uint32_t k = 0; k < room; k++) src[k] = pat(k);
    static const uint32_t v1[4] = { 0x7FC00000u, 0x40000000u, 0xBF000000u, 0x3F400000u };
    static const uint32_t v2[4] = { 0x3E800000u, 0x3FC00000u, 0xBF800000u, 0x3F000000u };
    static const uint32_t v3[4] = { 0x7F800001u, 0xFF800001u, 0x7FBFFFFFu, 0x3F000000u };   /* signalling NaNs */
    int ch = 0;
    uint32_t hr;
    fprintf(f, " #%u\n", id);
#define SET(txt, call) do { fputs("  " txt, f); ch = 0; hr = (call); o_set(f, m, id, hr, ch); } while (0)
    fputs("  getvalue", f); hr = fxm_get_value(m, id, dst, W * 4); o_get(f, hr, dst, W);
    if (W){ fputs("  getvalue short", f); hr = fxm_get_value(m, id, dst, W * 4 - 4); o_get(f, hr, dst, 0); }
    SET("setbool 5", fxm_set_bool(m, id, 5, &ch));
    fputs("  getbool", f); hr = fxm_get_bool(m, id, &w); o_get(f, hr, &w, 1);
    SET("setint", fxm_set_int(m, id, 0x80FF4020u, &ch));
    SET("setint again", fxm_set_int(m, id, 0x80FF4020u, &ch));
    fputs("  getint", f); hr = fxm_get_int(m, id, &w); o_get(f, hr, &w, 1);
    SET("setfloat", fxm_set_float(m, id, 0xC0490FDBu, &ch));
    SET("setfloat again", fxm_set_float(m, id, 0xC0490FDBu, &ch));
    fputs("  getfloat", f); hr = fxm_get_float(m, id, &w); o_get(f, hr, &w, 1);
    fputs("  getint", f); hr = fxm_get_int(m, id, &w); o_get(f, hr, &w, 1);
    for (uint32_t t = FX2_T_BOOL; t <= FX2_T_FLOAT; t++){
        fprintf(f, "  setarray %u %u", t, A); ch = 0; hr = fxm_set_array(m, id, t, src + t, A, &ch); o_set(f, m, id, hr, ch);
        fprintf(f, "  getarray %u %u", t, A); hr = fxm_get_array(m, id, t, dst, A); o_get(f, hr, dst, A < W ? A : W);
        fprintf(f, "  setarray %u 3", t); ch = 0; hr = fxm_set_array(m, id, t, src + 7, 3, &ch); o_set(f, m, id, hr, ch);
    }
    SET("setvector 1", fxm_set_vector(m, id, v1, &ch));
    fputs("  getvector", f); hr = fxm_get_vector(m, id, dst); o_get(f, hr, dst, 4);
    SET("setvector 2", fxm_set_vector(m, id, v2, &ch));
    fputs("  getint", f); hr = fxm_get_int(m, id, &w); o_get(f, hr, &w, 1);
    SET("setvector 3", fxm_set_vector(m, id, v3, &ch));
    fputs("  getint", f); hr = fxm_get_int(m, id, &w); o_get(f, hr, &w, 1);
    fprintf(f, "  setvectorarray %u", E); ch = 0; hr = fxm_set_vector_array(m, id, src + 3, E, &ch); o_set(f, m, id, hr, ch);
    SET("setvectorarray 0", fxm_set_vector_array(m, id, src, 0, &ch));
    fprintf(f, "  setvectorarray %u", E + 1); ch = 0; hr = fxm_set_vector_array(m, id, src, E + 1, &ch); o_set(f, m, id, hr, ch);
    fprintf(f, "  getvectorarray %u", E); hr = fxm_get_vector_array(m, id, dst, E); o_get(f, hr, dst, 4 * (uint64_t)E);
    fputs("  getvectorarray 0", f); hr = fxm_get_vector_array(m, id, dst, 0); o_get(f, hr, dst, 0);
    SET("setmatrix", fxm_set_matrix(m, id, src + 5, 0, &ch));
    fputs("  getmatrix", f); hr = fxm_get_matrix(m, id, dst, 0); o_get(f, hr, dst, 16);
    SET("setmatrix transposed", fxm_set_matrix(m, id, src + 1, 1, &ch));
    fputs("  getmatrix transposed", f); hr = fxm_get_matrix(m, id, dst, 1); o_get(f, hr, dst, 16);
    fprintf(f, "  setmatrixarray %u", E); ch = 0; hr = fxm_set_matrix_array(m, id, src + 2, E, 0, &ch); o_set(f, m, id, hr, ch);
    SET("setmatrixarray 0", fxm_set_matrix_array(m, id, src, 0, 1, &ch));
    fprintf(f, "  getmatrixarray %u transposed", E); hr = fxm_get_matrix_array(m, id, dst, E, 1); o_get(f, hr, dst, 16 * (uint64_t)E);
    fputs("  getmatrixarray 0", f); hr = fxm_get_matrix_array(m, id, dst, 0, 0); o_get(f, hr, dst, 0);
    fputs("  setobject", f); ch = 0; hr = fxm_set_object(m, id, 0x13570000u + id, &old, &ch);
    fprintf(f, " -> 0x%08x ch %d v %llu", hr, hr ? 0 : ch, (unsigned long long)fxm_version(m, id));
    if (!hr) fprintf(f, " old %08x", old);
    fputc('\n', f);
    SET("setobject again", fxm_set_object(m, id, 0x13570000u + id, NULL, &ch));
    fputs("  getobject", f); hr = fxm_get_object(m, id, &w); o_get(f, hr, &w, 1);
    SET("setvalue", fxm_set_value(m, id, src + 4, W * 4, &ch));
    if (W) SET("setvalue short", fxm_set_value(m, id, src, W * 4 - 4, &ch));
    fputs("  getvalue", f); hr = fxm_get_value(m, id, dst, W * 4); o_get(f, hr, dst, W);
#undef SET
    o_id(f, "param 0", fxm_param(m, id, 0)); o_id(f, "param 1", fxm_param(m, id, 1));
    o_id(f, "element 0", fxm_element(m, id, 0)); o_id(f, "element last+1", fxm_element(m, id, nd ? nd->elements : 0));
    o_id(f, "annot 0", fxm_annot(m, id, 0)); o_id(f, "pass 0", fxm_pass(m, id, 0));
    if (nd){
        uint8_t low[256];
        uint32_t ln = nd->sem.n < sizeof low ? nd->sem.n : sizeof low;
        for (uint32_t i = 0; i < ln; i++) low[i] = nd->sem.p[i] >= 'A' && nd->sem.p[i] <= 'Z' ? nd->sem.p[i] + 32 : nd->sem.p[i];
        const uint8_t *nm = nd->name.p ? nd->name.p : (const uint8_t *)"";   /* no name: "" (NULL has lines of its own) */
        o_id(f, "by_name path", fxm_by_name(m, 0, nd->path.p, nd->path.n));
        o_id(f, "by_name under parent", nd->parent ? fxm_by_name(m, nd->parent, nm, nd->name.n) : 0);
        o_id(f, "by_name NULL under it", fxm_by_name(m, id, NULL, 0));
        o_id(f, "by_semantic", nd->sem.p ? fxm_by_semantic(m, 0, nd->sem.p, nd->sem.n) : 0);
        o_id(f, "by_semantic lower", nd->sem.p && ln == nd->sem.n ? fxm_by_semantic(m, 0, low, ln) : 0);
        o_id(f, "by_semantic NULL under it", fxm_by_semantic(m, id, NULL, 0));
        o_id(f, "annot_by_name shader", fxm_annot_by_name(m, id, (const uint8_t *)"shader", 6));
        o_id(f, "tech_by_name", fxm_tech_by_name(m, nm, nd->name.n));
        o_id(f, "pass_by_name", fxm_pass_by_name(m, id, nm, nd->name.n));
        o_id(f, "pass_by_name NULL", fxm_pass_by_name(m, id, NULL, 0));
    }
    free(src); free(dst);
}

static int fxmodel_ops(FILE *f, mfx *x, uint32_t n, fxm_pool *pool){
    fprintf(f, "fxmodel ops 1\neffects %u\n", n);
    for (uint32_t i = 0; i < n; i++){
        fprintf(f, "effect %u ", i); qstr(f, (const uint8_t *)x[i].name, (uint32_t)strlen(x[i].name));
        fprintf(f, " clock %llu\n", (unsigned long long)fxm_clock(x[i].m));
        for (uint32_t id = 0; id <= fxm_nodes(x[i].m) + 1; id++) ops_node(f, x[i].m, id);
    }
    if (n){                                                     /* lifetime (§2.7) */
        free_traced(f, x, 0);
        char err[512];
        if (fxm_effect_new(&x[0].m, &x[0].fx, pool, err, sizeof err)){ fprintf(stderr, "fxmodeldump: %s\n", err); return -1; }
        fprintf(f, "again 0 clock %llu\n", (unsigned long long)fxm_clock(x[0].m));
        uint32_t *dst = NULL;
        for (uint32_t p = 0; p < x[0].fx.nparams; p++){
            uint32_t id = fxm_param(x[0].m, 0, p);
            const fxm_node *nd = fxm_node_get(x[0].m, id);
            uint32_t *d2 = realloc(dst, (size_t)nd->bytes + 4);
            if (!d2){ free(dst); fprintf(stderr, "fxmodeldump: out of memory\n"); return -1; }
            dst = d2;
            fprintf(f, " #%u shared %u v %llu getvalue", id, fxm_shared_slot(x[0].m, id), (unsigned long long)fxm_version(x[0].m, id));
            uint32_t hr = fxm_get_value(x[0].m, id, dst, nd->bytes);
            o_get(f, hr, dst, nd->bytes / 4);
        }
        free(dst);
        for (uint32_t i = 0; i < n; i++) free_traced(f, x, i);
    }
    fputs("end\n", f);
    return ferror(f) ? -1 : 0;
}

static int by_name(const void *a, const void *b){ return strcmp(((const mfx *)a)->name, ((const mfx *)b)->name); }

static uint8_t *slurp(const char *path, uint32_t *size){
    FILE *in = fopen(path, "rb");
    if (!in){ perror(path); return NULL; }
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0, got;
    do {
        if (n == cap){
            cap = cap ? cap * 2 : 65536;
            uint8_t *b = cap <= 0xFFFFFFFFu ? realloc(buf, cap) : NULL;
            if (!b){ fprintf(stderr, "fxmodeldump: %s: too large\n", path); free(buf); fclose(in); return NULL; }
            buf = b;
        }
        got = fread(buf + n, 1, cap - n, in);
        n += got;
    } while (got);
    int bad = ferror(in);
    fclose(in);
    if (bad){ perror(path); free(buf); return NULL; }
    *size = (uint32_t)n;
    return buf;
}

int main(int argc, char **argv){
    int ops = argc > 1 && !strcmp(argv[1], "--ops");
    if (ops){ argc--; argv++; }
    if (argc < 3){ fprintf(stderr, "usage: fxmodeldump [--ops] <out.txt|-> <NAME>.fxo...\n"); return 2; }
    uint32_t n = (uint32_t)(argc - 2);
    mfx *x = calloc(n, sizeof *x);
    char (*names)[1024] = calloc(n, sizeof *names);
    if (!x || !names){ fprintf(stderr, "fxmodeldump: out of memory\n"); return 1; }
    for (uint32_t i = 0; i < n; i++){
        const char *path = argv[2 + i], *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        snprintf(names[i], sizeof names[i], "%s", base);
        size_t nl = strlen(names[i]);
        if (nl > 4 && !strcmp(names[i] + nl - 4, ".fxo")) names[i][nl - 4] = 0;
        x[i].name = names[i];
        uint8_t *b = slurp(path, &x[i].size);
        if (!b) return 1;
        x[i].buf = b;
        char err[512];
        if (fx2_parse(&x[i].fx, b, x[i].size, err, sizeof err)){ fprintf(stderr, "fxmodeldump: %s: %s\n", path, err); return 1; }
    }
    qsort(x, n, sizeof *x, by_name);                         /* the parse trees point into the buffers, not into x */
    fxm_pool *pool = fxm_pool_new();
    if (!pool){ fprintf(stderr, "fxmodeldump: out of memory\n"); return 1; }
    for (uint32_t i = 0; i < n; i++){
        char err[512];
        if (fxm_effect_new(&x[i].m, &x[i].fx, pool, err, sizeof err)){ fprintf(stderr, "fxmodeldump: %s: %s\n", x[i].name, err); return 1; }
    }

    /* into <out>.tmp, renamed when it is whole */
    int to_stdout = !strcmp(argv[1], "-");
    char tmp[4096];
    FILE *f = stdout;
    if (!to_stdout){
        snprintf(tmp, sizeof tmp, "%s.tmp", argv[1]);
        if (!(f = fopen(tmp, "w"))){ perror(tmp); return 1; }
    }
    int r = ops ? fxmodel_ops(f, x, n, pool) : fxmodel_dump(f, x, n, pool);
    for (uint32_t i = 0; i < n; i++){ fxm_effect_free(x[i].m, NULL, NULL); fx2_free(&x[i].fx); free((void *)x[i].buf); }
    fxm_pool_free(pool);
    free(x); free(names);
    if (to_stdout) return r || fflush(stdout) ? 1 : 0;
    if (fclose(f) || r){ perror(tmp); remove(tmp); return 1; }
    if (rename(tmp, argv[1])){ perror(argv[1]); remove(tmp); return 1; }
    return 0;
}
#endif
