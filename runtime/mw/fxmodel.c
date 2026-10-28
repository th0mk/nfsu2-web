/* fxmodel.c -- the effect model (ID3DXEffect HLE, part 0): nodes, lookups,
 * storage with D3DX's Set/Get rules, change counters, the effect pool and
 * the CTAB match.  fxmodel.h says what each part does; docs/mw/fx-model.md
 * is the contract and the section numbers are its. */
#include "fxmodel.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- objects ---- */

typedef struct {
    fxm_node n;
    const fx2_value *v;          /* PARAM, ELEMENT, MEMBER, ANNOT: the value it is, or holds an element of */
    uint32_t e;                  /* the element of v it is (0 unless it is an element) */
    uint32_t pidx;               /* PARAM, ELEMENT, MEMBER: its top's parameter index; TECH: technique index */
    uint32_t off;                /* PARAM, ELEMENT, MEMBER: byte offset in its top's block; ANNOT: dword offset in adata */
    uint32_t end;                /* PARAM, ELEMENT, MEMBER: one past the last id of its subtree */
    uint32_t kid0, nkids;        /* elements or members, or a technique's passes, in kids[] */
    uint32_t annot0;             /* PARAM, TECH, PASS: the first annotation's id (they follow one another) */
    uint32_t path0;              /* inbuf: the path's offset in pathbuf */
    int inbuf;
    uint32_t dep0, ndeps;        /* PASS: in deps[] */
    uint32_t vs, ps;             /* TECH: §3.5's versions */
    const fx2_pass *pass;        /* PASS */
} node;

typedef struct { fxm_effect *e; uint32_t id; } holder;
typedef struct {                 /* a pool slot (§2.7) */
    uint32_t no, bytes;
    uint32_t *data;
    uint64_t ver;
    uint32_t nh, caph;
    holder *h;                   /* in the order they joined; h[0]'s typedef is the one new parameters match */
} slot;

struct fxm_pool {
    slot **slots;                /* by number; NULL: a free number */
    uint32_t n, cap;
    uint64_t clock;
    uint32_t effects;
};

typedef struct {                 /* a top-level parameter's storage */
    uint32_t *data;
    uint64_t *ver;               /* &own, or the slot's */
    uint64_t own;
    slot *s;                     /* shared: its slot */
} block;

struct fxm_effect {
    const fx2_effect *fx;
    fxm_pool *pool;
    int x87pc;                   /* the caller's x87 precision for SetVector's colour form (§2.5): 24, 53 or 64 */
    uint64_t own_clock, *clock;
    node *nodes;                 /* ids 1..nn; nodes[0] is not used */
    uint32_t nn, capn;
    uint32_t *params, *techs;    /* ids by index */
    uint32_t *kids;
    uint32_t nk, capk;
    uint8_t *pathbuf;
    uint32_t npb, cappb;
    uint32_t *ht;                /* fxm_by_name: node ids by path hash, open addressing */
    uint32_t htmask;
    block *blocks;               /* by parameter index */
    uint32_t *own;               /* the storage of the parameters that are not shared */
    uint32_t *adata;             /* the annotations' values */
    uint32_t nad, capad;
    fxm_prog *progs;
    uint32_t nprogs, capp;
    uint32_t *pc0, cappc;        /* while building: each program's first constant in consts[] */
    uint32_t *res2prog;          /* by resource: program index + 1, 0 for none */
    fxm_const *consts;
    uint32_t nc, capc;
    uint32_t *cl0, capcl;        /* while building: each constant's first leaf in leaves[] */
    fxm_leaf *leaves;
    uint32_t nl, capl;
    uint32_t *deps;
    uint32_t nd, capd;
    uint32_t *mark;              /* while building deps: by parameter index, the pass that has it */
    uint64_t mem, cap;           /* bytes allocated and the bound (fxmodel.h) */
    int failed;
    char *err;
    uint32_t errlen;
};

#define NODE(e, id) (&(e)->nodes[id])

static int fail(fxm_effect *e, const char *fmt, ...){
    if (!e->failed && e->err && e->errlen){
        va_list ap; va_start(ap, fmt); vsnprintf(e->err, e->errlen, fmt, ap); va_end(ap);
    }
    e->failed = 1;
    return -1;
}

static int charge(fxm_effect *e, uint64_t bytes){
    if (bytes > e->cap - e->mem)
        return fail(e, "out of memory: the model of a %u-byte effect would take more than %llu bytes",
                    e->fx->size, (unsigned long long)e->cap);
    e->mem += bytes;
    return 0;
}

/* n zeroed items of each bytes (at least one, so the result is never NULL
 * on success); NULL with e->failed set */
static void *zalloc(fxm_effect *e, uint64_t n, size_t each){
    if (e->failed) return NULL;
    if (!n) n = 1;
    if (n > ((uint64_t)1 << 32) / each || n * each != (uint64_t)(size_t)(n * each) || charge(e, n * each)){
        fail(e, "out of memory");
        return NULL;
    }
    void *p = calloc((size_t)n, each);
    if (!p) fail(e, "out of memory");
    return p;
}

/* p holds *cap items of each bytes: room for need, the new ones zeroed.
 * Returns the (maybe moved) array; e->failed tells whether it worked. */
static void *grow(fxm_effect *e, void *p, uint32_t *cap, uint64_t need, size_t each){
    if (e->failed || need <= *cap) return p;
    uint64_t nc = *cap ? *cap : 16;
    while (nc < need) nc *= 2;
    if (nc > 0xFFFFFFFFu) nc = need;
    if (need > 0xFFFFFFFFu || nc > ((uint64_t)1 << 40) / each || nc * each != (uint64_t)(size_t)(nc * each) ||
        charge(e, (nc - *cap) * each)){                      /* the last but one: a 32-bit size_t (wasm32) */
        fail(e, "out of memory");
        return p;
    }
    void *q = realloc(p, (size_t)(nc * each));
    if (!q){ fail(e, "out of memory"); return p; }
    memset((uint8_t *)q + (size_t)*cap * each, 0, (size_t)((nc - *cap) * each));
    *cap = (uint32_t)nc;
    return q;
}
#define GROW(e, arr, cap, need) ((arr) = grow((e), (arr), &(cap), (need), sizeof *(arr)), !(e)->failed)

static int is_num_type(uint32_t t){ return t == FX2_T_BOOL || t == FX2_T_INT || t == FX2_T_FLOAT; }
static int is_texture(uint32_t t){ return t >= FX2_T_TEXTURE && t <= FX2_T_TEXTURECUBE; }
static int is_sampler(uint32_t t){ return t >= FX2_T_SAMPLER && t <= FX2_T_SAMPLERCUBE; }
/* an object type that holds a guest pointer (§2.4, W): any other type in class OBJECT has no storage */
static int is_pointer(uint32_t t){
    return t == FX2_T_STRING || is_texture(t) || t == FX2_T_PIXELSHADER || t == FX2_T_VERTEXSHADER;
}
static int str_eq(fx2_str a, const uint8_t *s, uint32_t n){ return a.n == n && (!n || !memcmp(a.p, s, n)); }
static uint64_t sat(uint64_t x){ return x > ((uint64_t)1 << 42) ? (uint64_t)1 << 42 : x; }

/* §2.4: the bytes a value of typedef t takes (saturated far above any file) */
static uint64_t tbytes(const fx2_type *t){
    uint64_t one = 0;
    switch (fx2_value_kind(t)){
    case FX2_V_NUMERIC: one = sat(sat((uint64_t)t->rows * t->cols) * 4); break;
    case FX2_V_OBJECT: one = is_pointer(t->type) ? 4 : 0; break;
    case FX2_V_STRUCT:
        for (uint32_t m = 0; m < t->nmembers; m++) one = sat(one + tbytes(&t->members[m]));
        break;
    default: break;                                        /* a sampler or an unknown class takes nothing */
    }
    return sat(one * (t->elements ? t->elements : 1));
}

/* ---- nodes (§2.2) ---- */

static uint32_t new_node(fxm_effect *e){
    if ((uint64_t)e->nn + 2 > 0xFFFFFFFFu){ fail(e, "out of memory"); return 0; }
    if (!GROW(e, e->nodes, e->capn, (uint64_t)e->nn + 2)) return 0;
    return ++e->nn;
}

/* reserve n slots in kids[]; their first index, or -1 */
static int64_t new_kids(fxm_effect *e, uint32_t n){
    if (!GROW(e, e->kids, e->capk, (uint64_t)e->nk + n)) return -1;
    uint32_t k = e->nk;
    e->nk += n;
    return k;
}

/* node id's path: its parent's, then "[index]" (an element) or "." and its name (a member) */
static int child_path(fxm_effect *e, uint32_t id, uint32_t parent, int element, uint32_t index, fx2_str name){
    char num[16];
    uint32_t sn = element ? (uint32_t)snprintf(num, sizeof num, "[%u]", index) : 1 + name.n;
    uint64_t pn = NODE(e, parent)->n.path.n, total = pn + sn;
    if (total > 0xFFFFFFFFu) return fail(e, "out of memory");
    if (!GROW(e, e->pathbuf, e->cappb, (uint64_t)e->npb + total)) return -1;
    const node *pa = NODE(e, parent);
    uint8_t *w = e->pathbuf + e->npb;
    if (pn) memcpy(w, pa->inbuf ? e->pathbuf + pa->path0 : pa->n.path.p, (size_t)pn);
    if (element) memcpy(w + pn, num, sn);
    else { w[pn] = '.'; if (name.n) memcpy(w + pn + 1, name.p, name.n); }
    node *d = NODE(e, id);
    d->inbuf = 1; d->path0 = e->npb; d->n.path.n = (uint32_t)total;
    e->npb += (uint32_t)total;
    return 0;
}

static const fx2_str k_empty = { (const uint8_t *)"", 0 };

/* A parameter, element or member and its subtree (§2.2: depth first, an
 * array's elements, a struct's members).  v is the value it is a part of
 * and el its element there; off is its byte offset in its top's block.
 * Returns its id, 0 when memory ran out. */
static uint32_t value_node(fxm_effect *e, uint32_t kind, uint32_t parent, uint32_t index, uint32_t top, uint32_t pidx,
                           const fx2_type *t, const fx2_value *v, uint32_t el, uint32_t flags, uint64_t off){
    uint32_t id = new_node(e);
    if (!id) return 0;
    if (off > 0xFFFFFFFFu){ fail(e, "out of memory"); return 0; }
    node *d = NODE(e, id);
    d->n.kind = kind; d->n.parent = parent; d->n.index = index; d->n.top = top ? top : id;
    d->n.name = t->name; d->n.sem = t->sem;
    d->n.cls = t->cls; d->n.type = t->type; d->n.rows = t->rows; d->n.cols = t->cols;
    d->n.elements = kind == FXM_K_ELEMENT ? 0 : t->elements; d->n.members = t->nmembers;
    d->n.flags = flags; d->n.t = t;
    d->v = v; d->e = el; d->pidx = pidx; d->off = (uint32_t)off;
    if (kind == FXM_K_PARAM) d->n.path = t->name.p ? t->name : k_empty;
    else if (child_path(e, id, parent, kind == FXM_K_ELEMENT, index, t->name)) return 0;
    top = top ? top : id;

    uint32_t vk = fx2_value_kind(t);
    uint64_t bytes = 0;
    if (kind != FXM_K_ELEMENT && t->elements){              /* an array: its elements */
        uint32_t ne = t->elements;
        int64_t k0 = new_kids(e, ne);
        if (k0 < 0) return 0;
        NODE(e, id)->kid0 = (uint32_t)k0; NODE(e, id)->nkids = ne;
        uint64_t eb = 0;
        for (uint32_t i = 0; i < ne; i++){
            uint32_t c = value_node(e, FXM_K_ELEMENT, id, i, top, pidx, t, v, i, flags, off + eb * i);
            if (!c) return 0;
            if (!i) eb = NODE(e, c)->n.bytes;
            e->kids[k0 + i] = c;
        }
        bytes = eb * ne;
    } else if (vk == FX2_V_STRUCT){                         /* a struct: its members, one after another */
        uint32_t nm = t->nmembers;
        int64_t k0 = new_kids(e, nm);
        if (k0 < 0) return 0;
        NODE(e, id)->kid0 = (uint32_t)k0; NODE(e, id)->nkids = nm;
        uint64_t mo = off;
        for (uint32_t m = 0; m < nm; m++){
            uint32_t c = value_node(e, FXM_K_MEMBER, id, m, top, pidx, &t->members[m],
                                    &v->members[(uint64_t)el * nm + m], 0, flags, mo);
            if (!c) return 0;
            mo += NODE(e, c)->n.bytes;
            e->kids[k0 + m] = c;
        }
        bytes = mo - off;
    } else if (vk == FX2_V_NUMERIC) bytes = (uint64_t)t->rows * t->cols * 4;
    else if (vk == FX2_V_OBJECT){
        bytes = is_pointer(t->type) ? 4 : 0;
        NODE(e, id)->n.object = fx2_rd32(v->data + 4 * (uint64_t)el);
    } else if (vk == FX2_V_SAMPLER) NODE(e, id)->n.block = &v->blocks[el];
    if (bytes > 0xFFFFFFFFu - off){ fail(e, "out of memory"); return 0; }   /* the value is in the file: never */
    d = NODE(e, id);
    d->n.bytes = (uint32_t)bytes;
    d->end = e->nn + 1;
    return id;
}

static void init_value(uint32_t *dst, const fx2_value *v);

/* §2.5.2: the n annotations of owner, each a node with its file value in adata */
static int annot_nodes(fxm_effect *e, uint32_t owner, const fx2_annot *a, uint32_t n){
    NODE(e, owner)->annot0 = e->nn + 1;
    NODE(e, owner)->n.nannots = n;
    for (uint32_t k = 0; k < n; k++){
        uint32_t id = new_node(e);
        if (!id) return -1;
        const fx2_type *t = a[k].t;
        uint64_t bytes = tbytes(t);
        if (bytes > 0xFFFFFFFFu) return fail(e, "out of memory");
        if (!GROW(e, e->adata, e->capad, (uint64_t)e->nad + bytes / 4)) return -1;
        node *d = NODE(e, id);
        d->n.kind = FXM_K_ANNOT; d->n.parent = owner; d->n.index = k;
        d->n.name = t->name; d->n.sem = t->sem; d->n.path = t->name;
        d->n.cls = t->cls; d->n.type = t->type; d->n.rows = t->rows; d->n.cols = t->cols;
        d->n.elements = t->elements; d->n.members = t->nmembers;
        d->n.bytes = (uint32_t)bytes; d->n.flags = FXM_ANNOTATION; d->n.t = t;
        d->v = &a[k].v; d->off = e->nad;
        if (a[k].v.kind == FX2_V_OBJECT && !t->elements) d->n.object = fx2_rd32(a[k].v.data);
        if (bytes) init_value(e->adata + e->nad, &a[k].v);
        e->nad += (uint32_t)(bytes / 4);
    }
    return 0;
}

/* the default of value v, all its elements, laid out as §2.4 says (an
 * object takes its 0 pointer, a sampler nothing) */
static void init_value(uint32_t *dst, const fx2_value *v){
    const fx2_type *t = v->t;
    if (v->kind == FX2_V_NUMERIC){
        uint64_t k = (uint64_t)t->rows * t->cols * v->n;
        for (uint64_t i = 0; i < k; i++) dst[i] = fx2_rd32(v->data + 4 * i);
    } else if (v->kind == FX2_V_STRUCT){
        uint64_t o = 0, k = (uint64_t)v->n * t->nmembers;
        for (uint64_t i = 0; i < k; i++){
            const fx2_value *mv = &v->members[i];
            uint64_t b = tbytes(mv->t);
            if (b) init_value(dst + o / 4, mv);
            o += b;
        }
    }
}

/* a numeric leaf (it holds its own dwords): rows x cols of them */
static int num_leaf(const node *d){
    return d->n.kind <= FXM_K_MEMBER && !d->n.elements && fx2_value_kind(d->n.t) == FX2_V_NUMERIC;
}
static int obj_leaf(const node *d){
    return d->n.kind <= FXM_K_MEMBER && !d->n.elements && fx2_value_kind(d->n.t) == FX2_V_OBJECT && d->n.bytes;
}

/* the defaults of top-level parameter id into its block's data */
static void init_param(fxm_effect *e, uint32_t id, uint32_t *data){
    for (uint32_t j = id; j < NODE(e, id)->end; j++){
        const node *d = NODE(e, j);
        if (!num_leaf(d)) continue;
        uint64_t per = (uint64_t)d->n.rows * d->n.cols;
        const uint8_t *src = d->v->data + 4 * per * d->e;
        for (uint64_t i = 0; i < per; i++) data[d->off / 4 + i] = fx2_rd32(src + 4 * i);
    }
}

/* every non-zero object pointer in the subtree of top-level parameter id, stored at data */
static void drop_objects(const fxm_effect *e, uint32_t id, const uint32_t *data,
                         void (*drop)(void *ctx, uint32_t ptr), void *ctx){
    if (!drop) return;
    for (uint32_t j = id; j < NODE(e, id)->end; j++){
        const node *d = NODE(e, j);
        if (obj_leaf(d) && data[d->off / 4]) drop(ctx, data[d->off / 4]);
    }
}

/* ---- fxm_by_name's table: FNV-1a over the path ---- */

static uint32_t fnv(uint32_t h, const uint8_t *s, uint32_t n){
    for (uint32_t i = 0; i < n; i++){ h ^= s[i]; h *= 16777619u; }
    return h;
}
#define FNV0 2166136261u

static int named(const node *d){ return d->n.kind >= FXM_K_PARAM && d->n.kind <= FXM_K_MEMBER; }

/* the first named node whose path is a, then (sep) '.', then b */
static uint32_t ht_find(const fxm_effect *e, const uint8_t *a, uint32_t an, int sep, const uint8_t *b, uint32_t bn){
    if (!e->ht) return 0;
    uint64_t len = (uint64_t)an + (sep ? 1 : 0) + bn;
    uint32_t h = fnv(FNV0, a, an);
    if (sep) h = fnv(h, (const uint8_t *)".", 1);
    h = fnv(h, b, bn);
    for (uint32_t i = h & e->htmask; e->ht[i]; i = (i + 1) & e->htmask){
        const fx2_str p = NODE(e, e->ht[i])->n.path;
        if (p.n != len) continue;
        if (an && memcmp(p.p, a, an)) continue;
        if (sep && p.p[an] != '.') continue;
        if (bn && memcmp(p.p + an + (sep ? 1 : 0), b, bn)) continue;
        return e->ht[i];
    }
    return 0;
}

static int build_hash(fxm_effect *e){
    uint64_t m = 0;
    for (uint32_t id = 1; id <= e->nn; id++) m += named(NODE(e, id));
    uint64_t cap = 16;
    while (cap < 2 * m) cap *= 2;
    if (!(e->ht = zalloc(e, cap, sizeof *e->ht))) return -1;
    e->htmask = (uint32_t)(cap - 1);
    for (uint32_t id = 1; id <= e->nn; id++){                  /* in id order: the first node of a path comes */
        const node *d = NODE(e, id);                           /* first in its probe chain, so ht_find finds it */
        if (!named(d)) continue;
        uint32_t i = fnv(FNV0, d->n.path.p, d->n.path.n) & e->htmask;
        while (e->ht[i]) i = (i + 1) & e->htmask;
        e->ht[i] = id;
    }
    return 0;
}

/* ---- the pool (§2.7) ---- */

/* W's is_same_parameter: name, class, type, rows, columns, elements and members, members recursively */
static int same_type(const fx2_type *a, const fx2_type *b){
    if (!str_eq(a->name, b->name.p, b->name.n) || a->cls != b->cls || a->type != b->type || a->rows != b->rows ||
        a->cols != b->cols || a->elements != b->elements || a->nmembers != b->nmembers) return 0;
    for (uint32_t m = 0; m < a->nmembers; m++)
        if (!same_type(&a->members[m], &b->members[m])) return 0;
    return 1;
}

static int add_holder(slot *s, fxm_effect *e, uint32_t id){
    if (s->nh == s->caph){
        uint32_t cap = s->caph ? s->caph * 2 : 4;
        holder *h = realloc(s->h, (size_t)cap * sizeof *h);
        if (!h) return -1;
        s->h = h; s->caph = cap;
    }
    s->h[s->nh].e = e; s->h[s->nh].id = id; s->nh++;
    return 0;
}

/* top-level parameter i (node id) of e joins a live slot of the same name
 * and typedef, or opens one at the lowest free number with its default;
 * either way the slot's version becomes ++counter (W: set_dirty after
 * d3dx_pool_sync_shared_parameter, parameter by parameter) */
static int share(fxm_effect *e, uint32_t i, uint32_t id){
    fxm_pool *p = e->pool;
    const node *d = NODE(e, id);
    block *b = &e->blocks[i];
    for (uint32_t k = 0; k < p->n; k++){
        slot *s = p->slots[k];
        if (!s || !s->nh || !same_type(NODE(s->h[0].e, s->h[0].id)->n.t, d->n.t)) continue;
        if (add_holder(s, e, id)) return fail(e, "out of memory");
        b->s = s; b->data = s->data; b->ver = &s->ver;
        s->ver = ++*e->clock;
        return 0;
    }
    uint32_t no = 0;
    while (no < p->n && p->slots[no]) no++;
    if (no == p->n && p->n == p->cap){
        uint32_t cap = p->cap ? p->cap * 2 : 16;
        slot **ss = cap > p->cap ? realloc(p->slots, (size_t)cap * sizeof *ss) : NULL;
        if (!ss) return fail(e, "out of memory");
        p->slots = ss; p->cap = cap;
    }
    slot *s = calloc(1, sizeof *s);
    uint32_t *data = s ? calloc(d->n.bytes / 4 ? d->n.bytes / 4 : 1, 4) : NULL;
    if (!data || add_holder(s, e, id)){ free(data); if (s) free(s->h); free(s); return fail(e, "out of memory"); }
    s->no = no; s->bytes = d->n.bytes; s->data = data;
    s->ver = ++*e->clock;
    init_param(e, id, data);
    p->slots[no] = s;
    if (no == p->n) p->n++;
    b->s = s; b->data = data; b->ver = &s->ver;
    return 0;
}

/* e lets go of top-level parameter id's slot; the last holder frees it */
static void unshare(fxm_effect *e, block *b, uint32_t id, void (*drop)(void *ctx, uint32_t ptr), void *ctx){
    slot *s = b->s;
    uint32_t k = 0;
    while (k < s->nh && !(s->h[k].e == e && s->h[k].id == id)) k++;
    if (k == s->nh) return;
    memmove(s->h + k, s->h + k + 1, (size_t)(s->nh - k - 1) * sizeof *s->h);
    s->nh--;
    b->s = NULL; b->data = NULL; b->ver = &b->own;
    if (s->nh) return;
    drop_objects(e, id, s->data, drop, ctx);
    e->pool->slots[s->no] = NULL;
    free(s->data); free(s->h); free(s);
}

fxm_pool *fxm_pool_new(void){ return calloc(1, sizeof(fxm_pool)); }

void fxm_pool_free(fxm_pool *p){
    if (!p || p->effects) return;                              /* an effect still uses it: freeing it would leave
                                                                * that effect's slots dangling */
    for (uint32_t k = 0; k < p->n; k++)
        if (p->slots[k]){ free(p->slots[k]->data); free(p->slots[k]->h); free(p->slots[k]); }
    free(p->slots);
    free(p);
}

uint32_t fxm_pool_slots(const fxm_pool *p){ return p ? p->n : 0; }

/* ---- §3: programs and their constants ---- */

typedef struct { const uint8_t *p; uint32_t ndw, pos, len; int found; } ctab_find;
static void first_ctab(void *ctx, uint32_t pos, uint32_t len){
    ctab_find *f = ctx;
    if (!f->found && fx2_comment_bytes(f->ndw, pos, len) >= 4 && fx2_rd32(f->p + 4 * ((uint64_t)pos + 1)) == FX2_CTAB){
        f->found = 1; f->pos = pos; f->len = len;
    }
}

/* the state class of the row program pr sits on (FX2_NSCLASSES past the table) */
static uint32_t prog_row(const fxm_prog *pr){
    const fx2_state_info *si = pr->res->target ? fx2_state_get(pr->res->target->op) : NULL;
    return si ? si->cls : FX2_NSCLASSES;
}

/* §3.1: W creates a shader by its state's type and uploads its constants and sampler states from the
 * VERTEXSHADER and PIXELSHADER rows only, vertex or pixel as the row says; so it takes effect only where
 * the row, the type and the version token agree.  A preshader is evaluated on any row. */
static int applied(const fxm_prog *pr){
    if (pr->kind != FXM_P_SHADER) return 1;
    uint32_t c = prog_row(pr), t = pr->res->target->t->type, k = pr->version >> 16;
    return (c == FX2_SC_VSHADER && t == FX2_T_VERTEXSHADER && k == 0xFFFE) ||
           (c == FX2_SC_PSHADER && t == FX2_T_PIXELSHADER && k == 0xFFFF);
}

static uint32_t usage1(const fxm_effect *e, const fx2_res *r){
    const uint8_t *z = r->size ? memchr(r->data, 0, r->size) : NULL;
    return fxm_by_name(e, 0, r->data, z ? (uint32_t)(z - r->data) : r->size);
}

/* §3.5: the resource that decides state s, the last one in file order (W: each resource d3dx_parse_resource
 * reads overwrites the state's type, object data, shader, evaluator and referenced parameter); NULL for none */
static const fx2_res *last_res(const fx2_state *s){
    const fx2_res *r = s->res;
    while (r && r->next) r = r->next;
    return r;
}

/* §3.2 and §3.3: constant k of the CTAB c of program pr */
static int match_const(fxm_effect *e, const fxm_prog *pr, const fx2_ctab *c, uint32_t k){
    if (!GROW(e, e->consts, e->capc, (uint64_t)e->nc + 1) || !GROW(e, e->cl0, e->capcl, e->capc)) return -1;
    uint32_t qi = e->nc++;
    fxm_const *q = &e->consts[qi];
    memset(q, 0, sizeof *q);
    e->cl0[qi] = e->nl;
    q->k = k;
    fx2_ctab_const(c, k, &q->c);
    const fx2_const *o = &q->c;
    fx2_str nm;
    int r = fx2_ctab_str(c, o->name, &nm);
    if (r > 0) q->name = nm;
    uint32_t id = r > 0 ? fxm_by_name(e, 0, nm.p, nm.n) : 0;
    const node *p = id ? NODE(e, id) : NULL;
    if (!p || p->n.kind != FXM_K_PARAM){ q->why = 1; return 0; }                       /* step 2 */
    if (!o->typed){ q->why = 5; return 0; }                                              /* step 3 */
    int smp = p->n.cls == FX2_C_OBJECT && is_sampler(p->n.type);
    int num = p->n.cls <= FX2_C_MATRIX_COLUMNS && is_num_type(p->n.type);
    int st = p->n.cls == FX2_C_STRUCT;
    uint32_t nelem = p->n.elements ? p->n.elements : 1;
    /* Step 4: set s takes a sampler parameter under a constant of class
     * OBJECT whose count, cut to the constant's own elements (W's
     * RegisterCount: min(count, max(1, Elements)), §3.4), is at most
     * max(1, elements) (W's object branch); sets b, i, c take a numeric
     * parameter under a numeric class (SCALAR..MATRIX_COLUMNS), or a struct
     * parameter (its members are step 6's) */
    uint32_t count = o->count;
    if (o->set == 3 && count > (o->elements ? o->elements : 1)) count = o->elements ? o->elements : 1;
    int ok;
    if (o->set == 3) ok = smp && o->cls == FX2_C_OBJECT && count <= nelem && pr->kind == FXM_P_SHADER;
    else if (o->set <= 2) ok = ((num && o->cls <= FX2_C_MATRIX_COLUMNS) || st) && (pr->kind == FXM_P_SHADER || o->set == 2);
    else ok = 0;
    if (!ok){ q->why = 2; return 0; }                                                    /* step 4 */
    /* step 5 (W init_set_constants_param; W checks no element count for a sampler): an array needs
     * the same element count, anything else at most 1; StructMembers must equal the member count, 0
     * for a numeric parameter (W compares them for a non-array and for each element of an array) */
    if (!smp && ((p->n.elements ? o->elements != p->n.elements : o->elements > 1) || o->members != p->n.members)){
        q->why = 4; return 0;
    }
    if (st){ q->why = 7; return 0; }                                                     /* step 6: not built */
    if (num && (o->rows != p->n.rows || o->cols != p->n.cols)){ q->why = 3; return 0; }  /* step 7 */

    q->id = id; q->nelem = nelem;
    uint64_t per;
    if (o->set == 3){
        q->layout = FXM_L_SAMPLER; per = 1;
        q->stage = o->reg + (prog_row(pr) == FX2_SC_VSHADER ? 257 : 0);     /* W: by the row, not the token */
    } else {
        static const uint32_t to[3] = { FX2_T_BOOL, FX2_T_INT, FX2_T_FLOAT };
        q->from = p->n.type; q->to = to[o->set];
        q->layout = o->cls == FX2_C_SCALAR ? FXM_L_SCALAR : o->cls == FX2_C_VECTOR ? FXM_L_VECTOR
                  : o->cls == FX2_C_MATRIX_ROWS ? FXM_L_ROWS : FXM_L_COLUMNS;
        if (o->set == 0 || o->cls == FX2_C_SCALAR) per = (uint64_t)o->rows * o->cols;
        else per = o->cls == FX2_C_VECTOR ? 1 : o->cls == FX2_C_MATRIX_ROWS ? o->rows : o->cols;
    }
    q->per = (uint32_t)per;
    if (!GROW(e, e->leaves, e->capl, (uint64_t)e->nl + nelem)) return -1;
    q = &e->consts[qi];
    for (uint32_t j = 0; j < nelem; j++){
        fxm_leaf *l = &e->leaves[e->nl + j];
        uint64_t at = per * j, reg = o->reg + at;
        l->id = p->n.elements ? e->kids[p->kid0 + j] : id;
        l->reg = reg > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)reg;
        l->count = count > at ? (uint32_t)(count - at < per ? count - at : per) : 0;
        l->layout = q->layout;
        if (l->count) q->used++;
    }
    q->nleaves = nelem;
    e->nl += nelem;
    return 0;
}

static int build_progs(fxm_effect *e){
    const fx2_effect *fx = e->fx;
    if (!(e->res2prog = zalloc(e, fx->nres, sizeof *e->res2prog))) return -1;
    for (uint32_t ri = 0; ri < fx->nres; ri++){
        const fx2_res *r = &fx->res[ri];
        if (!r->target || r->usage) continue;
        uint32_t ty = r->target->t->type, kind;
        if (ty == FX2_T_VERTEXSHADER || ty == FX2_T_PIXELSHADER) kind = FXM_P_SHADER;
        else if (is_num_type(ty) || ty == FX2_T_STRING) kind = FXM_P_PRESHADER;
        else continue;
        if (!GROW(e, e->progs, e->capp, (uint64_t)e->nprogs + 1) || !GROW(e, e->pc0, e->cappc, e->capp)) return -1;
        uint32_t pi = e->nprogs++;
        fxm_prog *pr = &e->progs[pi];
        memset(pr, 0, sizeof *pr);
        e->pc0[pi] = e->nc;
        pr->kind = kind; pr->res = r;
        pr->version = r->size >= 4 ? fx2_rd32(r->data) : 0;
        e->res2prog[ri] = pi + 1;
        const uint8_t *pl = NULL;
        uint32_t nb = 0;
        if (kind == FXM_P_SHADER){
            fx2_shader sh;
            ctab_find f = { r->data, r->size / 4, 0, 0, 0 };
            fx2_shader_walk(r->data, r->size, &sh, first_ctab, &f);
            if (f.found){ nb = fx2_comment_bytes(f.ndw, f.pos, f.len) - 4; pl = r->data + 4 * ((uint64_t)f.pos + 2); }
        } else {
            fx2_pres ps;
            fx2_pres_read(r->data, r->size, &ps, NULL, NULL);
            if (ps.ctab){
                nb = fx2_comment_bytes(ps.ndw, ps.ctab, ps.ctab_len);
                if (nb >= 4){ nb -= 4; pl = r->data + 4 * ((uint64_t)ps.ctab + 2); }
            }
        }
        fx2_ctab c;
        if (!pl || fx2_ctab_init(&c, pl, nb) || c.size != 28) continue;       /* W: its Size must be 28 */
        pr->ctab = 1; pr->nconsts = c.nconsts;
        uint64_t fit = (uint64_t)c.cinfo + 20 <= nb ? (nb - (uint64_t)c.cinfo) / 20 : 0;
        uint32_t nread = c.nconsts < fit ? c.nconsts : (uint32_t)fit;
        pr->nread = nread;
        for (uint32_t k = 0; k < nread; k++)
            if (match_const(e, &e->progs[pi], &c, k)) return -1;
    }
    for (uint32_t k = 0; k < e->nc; k++) e->consts[k].leaves = e->consts[k].nleaves ? e->leaves + e->cl0[k] : NULL;
    for (uint32_t k = 0; k < e->nprogs; k++) e->progs[k].consts = e->progs[k].nread ? e->consts + e->pc0[k] : NULL;
    free(e->pc0); free(e->cl0);
    e->pc0 = e->cl0 = NULL;
    return 0;
}

/* ---- §3.5: pass dependencies and technique versions ---- */

static int dep_add(fxm_effect *e, uint32_t id, uint32_t pass){
    const node *d = NODE(e, NODE(e, id)->n.top);
    if (!d->n.bytes || e->mark[d->pidx] == pass) return 0;
    e->mark[d->pidx] = pass;
    if (!GROW(e, e->deps, e->capd, (uint64_t)e->nd + 1)) return -1;
    e->deps[e->nd++] = d->n.top;
    return 0;
}

/* what can change the states of state s, through the resource that decides
 * it: matched numeric constants and named parameters, and with samplers
 * the same over the bound blocks and over the block of a sampler named by
 * a usage-1 reference on a state of class SAMPLER (SAMPLER[i] = <smp>, row
 * 178: W applies its sampler states, each with its dirty test; any other
 * row takes the sampler as its value and applies none of them) */
static int dep_state(fxm_effect *e, const fx2_state *s, int samplers, uint32_t pass){
    const fx2_state_info *si = fx2_state_get(s->op);
    int setsampler = samplers && si && si->cls == FX2_SC_SAMPLER;
    const fx2_res *r = last_res(s);
    if (!r) return 0;
    uint32_t ri = (uint32_t)(r - e->fx->res);
    if (r->usage == 1){
        uint32_t id = usage1(e, r);
        if (id && dep_add(e, id, pass)) return -1;
        const node *d = id && setsampler && is_sampler(NODE(e, id)->n.type) ? NODE(e, id) : NULL;
        if (d && d->n.elements) d = NODE(e, e->kids[d->kid0]);            /* element 0: the model's choice (W reads the
                                                                           * array's data pointer, NULL; §8) */
        const fx2_block *bk = d ? d->n.block : NULL;
        for (uint32_t t = 0; bk && t < bk->nstates; t++)
            if (dep_state(e, &bk->states[t], 0, pass)) return -1;
        return 0;
    }
    if (r->usage || !e->res2prog[ri]) return 0;
    const fxm_prog *pr = &e->progs[e->res2prog[ri] - 1];
    for (uint32_t k = 0; applied(pr) && k < pr->nread; k++){                 /* one W never applies reads nothing */
        const fxm_const *q = &pr->consts[k];
        if (q->why) continue;
        if (q->layout != FXM_L_SAMPLER){ if (dep_add(e, q->id, pass)) return -1; continue; }
        if (!samplers) continue;
        for (uint32_t j = 0; j < q->nleaves; j++){
            const fx2_block *bk = q->leaves[j].count ? NODE(e, q->leaves[j].id)->n.block : NULL;
            for (uint32_t t = 0; bk && t < bk->nstates; t++)
                if (dep_state(e, &bk->states[t], 0, pass)) return -1;
        }
    }
    return 0;
}

static int cmp_u32(const void *a, const void *b){
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int build_deps(fxm_effect *e){
    if (!(e->mark = zalloc(e, e->fx->nparams, sizeof *e->mark))) return -1;
    for (uint32_t id = 1; id <= e->nn; id++){
        if (NODE(e, id)->n.kind != FXM_K_PASS) continue;
        uint32_t d0 = e->nd;
        const fx2_pass *ps = NODE(e, id)->pass;
        for (uint32_t k = 0; k < ps->nstates; k++)
            if (dep_state(e, &ps->states[k], 1, id)) return -1;
        if (e->nd - d0 > 1)            /* none yet: deps may still be NULL */
            qsort(e->deps + d0, e->nd - d0, sizeof *e->deps, cmp_u32);
        NODE(e, id)->dep0 = d0; NODE(e, id)->ndeps = e->nd - d0;
    }
    free(e->mark);
    e->mark = NULL;
    for (uint32_t id = 1; id <= e->nn; id++){
        node *t = NODE(e, id);
        if (t->n.kind != FXM_K_TECH) continue;
        for (uint32_t j = 0; j < t->nkids; j++){
            const fx2_pass *ps = NODE(e, e->kids[t->kid0 + j])->pass;
            for (uint32_t k = 0; k < ps->nstates; k++){
                const fx2_res *r = last_res(&ps->states[k]);                 /* the resource that decides */
                uint32_t pi = r ? e->res2prog[r - e->fx->res] : 0;
                if (!r || r->usage || !pi || e->progs[pi - 1].kind != FXM_P_SHADER || !applied(&e->progs[pi - 1])) continue;
                uint32_t v = e->progs[pi - 1].version, *best = v >> 16 == 0xFFFE ? &t->vs : v >> 16 == 0xFFFF ? &t->ps : NULL;
                if (best && (!*best || (v & 0xFFFF) > (*best & 0xFFFF))) *best = v;
            }
        }
    }
    return 0;
}

/* ---- creation and release ---- */

static int build(fxm_effect *e){
    const fx2_effect *fx = e->fx;
    if (!(e->params = zalloc(e, fx->nparams, sizeof *e->params)) || !(e->techs = zalloc(e, fx->ntechs, sizeof *e->techs)))
        return -1;
    for (uint32_t i = 0; i < fx->nparams; i++){                /* §2.2 1. */
        const fx2_param *pa = &fx->params[i];
        uint32_t id = value_node(e, FXM_K_PARAM, 0, i, 0, i, pa->t, &pa->v, 0, pa->flags, 0);
        if (!id || annot_nodes(e, id, pa->annots, pa->nannots)) return -1;
        e->params[i] = id;
    }
    for (uint32_t i = 0; i < fx->ntechs; i++){                 /* §2.2 2. */
        const fx2_tech *te = &fx->techs[i];
        uint32_t id = new_node(e);
        int64_t k0 = id ? new_kids(e, te->npasses) : -1;
        if (k0 < 0) return -1;
        node *t = NODE(e, id);
        t->n.kind = FXM_K_TECH; t->n.index = i; t->n.name = t->n.path = te->name;
        t->pidx = i; t->kid0 = (uint32_t)k0; t->nkids = te->npasses;
        e->techs[i] = id;
        if (annot_nodes(e, id, te->annots, te->nannots)) return -1;
        for (uint32_t j = 0; j < te->npasses; j++){
            uint32_t pid = new_node(e);
            if (!pid) return -1;
            node *p = NODE(e, pid);
            p->n.kind = FXM_K_PASS; p->n.parent = id; p->n.index = j;
            p->n.name = p->n.path = te->passes[j].name; p->pass = &te->passes[j];
            e->kids[k0 + j] = pid;
            if (annot_nodes(e, pid, te->passes[j].annots, te->passes[j].nannots)) return -1;
        }
    }
    for (uint32_t id = 1; id <= e->nn; id++)                   /* the path buffer moves no more */
        if (NODE(e, id)->inbuf) NODE(e, id)->n.path.p = e->pathbuf + NODE(e, id)->path0;
    if (build_hash(e) || build_progs(e) || build_deps(e)) return -1;   /* they read no storage */

    /* §2.4, §2.6, §2.7: storage and versions, in id order, last, so that a creation refused before here leaves
     * the pool's slots, versions and counter as they were (W syncs the pool at the end of d3dx_parse_effect) */
    if (!(e->blocks = zalloc(e, fx->nparams, sizeof *e->blocks))) return -1;
    uint64_t own = 0;
    for (uint32_t i = 0; i < fx->nparams; i++){
        const node *d = NODE(e, e->params[i]);
        e->blocks[i].ver = &e->blocks[i].own;
        if (!(e->pool && (d->n.flags & FXM_SHARED) && !is_sampler(d->n.type))) own += d->n.bytes / 4;
    }
    if (!(e->own = zalloc(e, own, 4))) return -1;
    own = 0;
    for (uint32_t i = 0; i < fx->nparams; i++){
        uint32_t id = e->params[i];
        const node *d = NODE(e, id);
        block *b = &e->blocks[i];
        if (e->pool && (d->n.flags & FXM_SHARED) && !is_sampler(d->n.type)){
            if (share(e, i, id)) return -1;
            continue;
        }
        b->data = e->own + own;
        own += d->n.bytes / 4;
        b->own = ++*e->clock;
        init_param(e, id, b->data);
    }
    return 0;
}

static void destroy(fxm_effect *e, void (*drop)(void *ctx, uint32_t ptr), void *ctx){
    for (uint32_t i = 0; e->blocks && i < e->fx->nparams; i++){
        block *b = &e->blocks[i];
        if (b->s) unshare(e, b, e->params[i], drop, ctx);
        else if (b->data) drop_objects(e, e->params[i], b->data, drop, ctx);
    }
    if (e->pool) e->pool->effects--;
    free(e->nodes); free(e->params); free(e->techs); free(e->kids); free(e->pathbuf); free(e->ht);
    free(e->blocks); free(e->own); free(e->adata); free(e->progs); free(e->pc0); free(e->res2prog);
    free(e->consts); free(e->cl0); free(e->leaves); free(e->deps); free(e->mark);
    free(e);
}

int fxm_effect_new(fxm_effect **out, const fx2_effect *fx, fxm_pool *pool, char *err, uint32_t errlen){
    if (err && errlen) err[0] = 0;
    if (!out || !fx){ if (err && errlen) snprintf(err, errlen, "no effect"); return -1; }
    *out = NULL;
    fxm_effect *e = calloc(1, sizeof *e);
    if (!e){ if (err && errlen) snprintf(err, errlen, "out of memory"); return -1; }
    e->fx = fx; e->pool = pool; e->err = err; e->errlen = errlen;
    e->cap = (uint64_t)fx->size * 1024 + ((uint64_t)16 << 20);
    e->clock = pool ? &pool->clock : &e->own_clock;
    e->x87pc = 24;
    if (pool) pool->effects++;
    int r = build(e);
    e->err = NULL; e->errlen = 0;
    if (r){ destroy(e, NULL, NULL); return -1; }
    *out = e;
    return 0;
}

void fxm_effect_free(fxm_effect *e, void (*drop)(void *ctx, uint32_t ptr), void *ctx){
    if (e) destroy(e, drop, ctx);
}

/* ---- nodes and lookups (§2.3) ---- */

static const node *nget(const fxm_effect *e, uint32_t id){ return e && id && id <= e->nn ? NODE(e, id) : NULL; }

uint32_t fxm_nodes(const fxm_effect *e){ return e ? e->nn : 0; }
const fxm_node *fxm_node_get(const fxm_effect *e, uint32_t id){ const node *d = nget(e, id); return d ? &d->n : NULL; }

uint32_t fxm_shared_slot(const fxm_effect *e, uint32_t id){
    const node *d = nget(e, id);
    return d && d->n.kind == FXM_K_PARAM && e->blocks[d->pidx].s ? e->blocks[d->pidx].s->no + 1 : 0;
}

uint32_t fxm_param(const fxm_effect *e, uint32_t parent, uint32_t index){
    if (!e) return 0;
    if (!parent) return index < e->fx->nparams ? e->params[index] : 0;
    const node *p = nget(e, parent);
    if (!p || !named(p) || p->n.elements || p->n.cls != FX2_C_STRUCT || index >= p->nkids) return 0;
    return e->kids[p->kid0 + index];
}

uint32_t fxm_element(const fxm_effect *e, uint32_t parent, uint32_t index){
    if (!e) return 0;
    if (!parent) return index < e->fx->nparams ? e->params[index] : 0;
    const node *p = nget(e, parent);
    if (!p || !named(p) || !p->n.elements || index >= p->nkids) return 0;
    return e->kids[p->kid0 + index];
}

uint32_t fxm_by_name(const fxm_effect *e, uint32_t parent, const uint8_t *s, uint32_t n){
    if (!e) return 0;
    if (!s){                                                   /* W: NULL gives the parent's own handle */
        const node *p = nget(e, parent);
        return p && (named(p) || p->n.kind == FXM_K_ANNOT) ? parent : 0;
    }
    if (!n) return 0;                                          /* W: "" is none */
    if (!parent) return ht_find(e, s, n, 0, NULL, 0);
    const node *p = nget(e, parent);
    if (!p || !named(p)) return 0;
    return ht_find(e, p->n.path.p, p->n.path.n, 1, s, n);
}

/* stricmp in the C locale: ASCII letters fold, every other byte compares as it is */
static int ieq(fx2_str a, const uint8_t *s, uint32_t n){
    if (!a.p || a.n != n) return 0;
    for (uint32_t i = 0; i < n; i++){
        uint8_t x = a.p[i], y = s[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return 1;
}

uint32_t fxm_by_semantic(const fxm_effect *e, uint32_t parent, const uint8_t *s, uint32_t n){
    if (!e) return 0;
    const uint32_t *ids;
    uint32_t count;
    if (!parent){ ids = e->params; count = e->fx->nparams; }
    else {
        /* W loops over the member count of the parent's members[], which hold an array's elements (each
         * with the array's semantic); the model stops at the elements */
        const node *p = nget(e, parent);
        if (!p || !named(p) || p->n.cls != FX2_C_STRUCT) return 0;
        count = p->n.members < p->nkids ? p->n.members : p->nkids;
        if (!count) return 0;                  /* none: kids may still be NULL */
        ids = e->kids + p->kid0;
    }
    for (uint32_t i = 0; i < count; i++){
        fx2_str sem = NODE(e, ids[i])->n.sem;
        if (s ? ieq(sem, s, n) : !sem.p) return ids[i];
    }
    return 0;
}

static int owner_ok(const node *d){
    return d && (d->n.kind == FXM_K_PARAM || d->n.kind == FXM_K_TECH || d->n.kind == FXM_K_PASS);
}

uint32_t fxm_annot(const fxm_effect *e, uint32_t owner, uint32_t index){
    const node *o = nget(e, owner);
    return owner_ok(o) && index < o->n.nannots ? o->annot0 + index : 0;
}

uint32_t fxm_annot_by_name(const fxm_effect *e, uint32_t owner, const uint8_t *s, uint32_t n){
    const node *o = nget(e, owner);
    if (!owner_ok(o) || !s || !n) return 0;                    /* W: NULL or "" is none */
    for (uint32_t k = 0; k < o->n.nannots; k++)
        if (str_eq(NODE(e, o->annot0 + k)->n.name, s, n)) return o->annot0 + k;
    return 0;
}

uint32_t fxm_tech(const fxm_effect *e, uint32_t index){ return e && index < e->fx->ntechs ? e->techs[index] : 0; }

uint32_t fxm_tech_by_name(const fxm_effect *e, const uint8_t *s, uint32_t n){
    if (!e || !s) return 0;                                    /* W: NULL is none ("" matches offset 0, §2.2) */
    for (uint32_t i = 0; i < e->fx->ntechs; i++)
        if (str_eq(NODE(e, e->techs[i])->n.name, s, n)) return e->techs[i];
    return 0;
}

uint32_t fxm_pass(const fxm_effect *e, uint32_t tech, uint32_t index){
    const node *t = nget(e, tech);
    return t && t->n.kind == FXM_K_TECH && index < t->nkids ? e->kids[t->kid0 + index] : 0;
}

uint32_t fxm_pass_by_name(const fxm_effect *e, uint32_t tech, const uint8_t *s, uint32_t n){
    const node *t = nget(e, tech);
    if (!t || t->n.kind != FXM_K_TECH || !s) return 0;        /* W: NULL is none */
    for (uint32_t j = 0; j < t->nkids; j++)
        if (str_eq(NODE(e, e->kids[t->kid0 + j])->n.name, s, n)) return e->kids[t->kid0 + j];
    return 0;
}

/* ---- values (§2.4..§2.6) ---- */

#define F_ONE 0x3F800000u

static uint32_t fbits(float x){ uint32_t w; memcpy(&w, &x, 4); return w; }
static float fval(uint32_t w){ float x; memcpy(&x, &w, 4); return x; }

/* FLOAT to INT: toward zero; NaN and |x| >= 2^31 give x86's integer indefinite */
static uint32_t f2i(uint32_t w){
    float x = fval(w);
    if (x != x || x >= 2147483648.0f || x <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)x;
}

/* FLOAT to INT as d3dx9_26's SetFloat, SetFloatArray and SetVectorArray do it (X: fld dword, _ftol2 at 0x600050):
 * truncation toward zero to 64 bits, the low dword; NaN, the infinities and |x| >= 2^63 give 0 (the low dword of
 * 0x8000000000000000).  3e9 -> 0xB2D05E00, -3e9 -> 0x4D2FA200 (O) */
static uint32_t f2i_ftol(uint32_t w){
    uint32_t ex = w >> 23 & 0xFF, m = (w & 0x7FFFFF) | 0x800000;
    if (ex == 0xFF || ex < 127 || ex >= 127 + 63) return 0;   /* NaN, inf, |x| < 1, |x| >= 2^63 */
    int sh = (int)ex - 127 - 23;
    uint64_t t = sh >= 0 ? (uint64_t)m << sh : (uint64_t)(m >> -sh);
    return (uint32_t)(w >> 31 ? 0 - t : t);
}
/* d3dx9_26's test of a float against 0 (fucompp): -0 is 0, a NaN is not */
static int f_nonzero(uint32_t w){ return (w & 0x7FFFFFFFu) != 0; }
/* the two floats compare equal on the x87 (fucompp): -0 equals +0, a NaN equals nothing */
static int f_equal(uint32_t a, uint32_t b){
    if (((a & 0x7F800000u) == 0x7F800000u && (a & 0x7FFFFFu)) || ((b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu)))
        return 0;
    return a == b || !((a | b) & 0x7FFFFFFFu);
}

/* §2.5's conv(to, from, w) (W's set_number) */
static uint32_t conv(uint32_t to, uint32_t from, uint32_t w){
    if (to == from) return w;                            /* the same type: copied as it is */
    if (!is_num_type(to)) return 0;
    if (from == FX2_T_VOID) return w;                    /* a VOID source is read as raw bits, */
    if (!is_num_type(from)) return 0;                    /* any other type as 0 */
    if (to == FX2_T_BOOL) return w != 0;                 /* a FLOAT bit test: -0.0 is TRUE */
    if (to == FX2_T_INT) return from == FX2_T_BOOL ? w != 0 : f2i(w);
    return from == FX2_T_BOOL ? (w ? F_ONE : 0) : fbits((float)(int32_t)w);
}

/* a colour component: clamped to [0, 1] (NaN to 0), x 255, truncated.  NaN is
 * tested on the bits: clang folds a float clamp into fmaxnm/fminnm (wasm:
 * f32.max), which turns a signalling NaN into a NaN instead of 0, and the
 * conversion of that NaN is undefined (found by fxdiff_synth.py --model) */
static uint32_t to_byte(uint32_t w){
    if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu)) return 0;
    float x = fval(w);
    if (x <= 0.0f) return 0;
    if (x >= 1.0f) return 255;
    return (uint32_t)(x * 255.0f);
}
static uint32_t from_byte(uint32_t b){ return fbits((float)(b & 0xFF) * (1.0f / 255.0f)); }
/* SetVector's colour form (X 0x5041D4): a channel x >= 1 gives 255, x <= 0 gives 0; anything else, NaN included,
 * trunc(x * 255 + 0.5) with the multiply and the add rounded at the caller's x87 precision (_ftol2: a NaN gives 0).
 * x * 255 is exact in a double; at 24 bits each step rounds to a float's 24 bits (the values are far from the
 * float range's ends) */
static uint32_t to_byte_round(uint32_t w, int pc){
    float x = fval(w);
    if ((w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu)) return 0;
    if (x >= 1.0f) return 255;
    if (x <= 0.0f) return 0;
    double y = (double)x * 255.0;
    if (pc == 24) y = (double)(float)((double)(float)y + 0.5);
    else y += 0.5;
    return (uint32_t)y;
}

static int valued(const node *d){ return d && d->n.kind >= FXM_K_PARAM && d->n.kind <= FXM_K_ANNOT; }
static int writable(const node *d){ return d && d->n.kind >= FXM_K_PARAM && d->n.kind <= FXM_K_MEMBER; }
/* 1x1 (W checks the shape only: any type; only numeric classes have rows and columns) */
static int one_by_one(const node *d){ return d->n.rows == 1 && d->n.cols == 1 && !d->n.elements; }
/* SetInt/GetInt's colour form, after 1x1: a FLOAT VECTOR of other than 2 columns or a FLOAT
 * MATRIX_ROWS of one column and other than 2 rows; it has min(rows x cols, 4) components (W writes
 * 3, and a 4th past 3 cells, even where the storage is smaller) */
static int colour(const node *d){
    return !d->n.elements && d->n.type == FX2_T_FLOAT && !one_by_one(d) &&
           ((d->n.cls == FX2_C_VECTOR && d->n.cols != 2) ||
            (d->n.cls == FX2_C_MATRIX_ROWS && d->n.cols == 1 && d->n.rows != 2));
}
static uint32_t colour_comps(const node *d){ return d->n.bytes / 4 < 4 ? d->n.bytes / 4 : 4; }

/* the node's dwords: NULL when it has none */
static const uint32_t *rd(const fxm_effect *e, const node *d){
    if (!d->n.bytes) return NULL;
    return d->n.kind == FXM_K_ANNOT ? e->adata + d->off : e->blocks[d->pidx].data + d->off / 4;
}
static uint32_t *wr(fxm_effect *e, const node *d){ return d->n.bytes ? e->blocks[d->pidx].data + d->off / 4 : NULL; }

static void put(uint32_t *d, uint32_t w, int *ch){ if (*d != w){ *d = w; *ch = 1; } }

/* a write is done: the block's version rises when the rule says so */
static uint32_t done(fxm_effect *e, const node *d, int ch, int always, int *changed){
    if (always || ch) *e->blocks[d->pidx].ver = ++*e->clock;
    if (changed) *changed = ch;
    return FXM_OK;
}
static uint32_t refuse(int *changed, uint32_t hr){ if (changed) *changed = 0; return hr; }

void fxm_set_x87_pc(fxm_effect *e, int pc){ e->x87pc = pc == 53 || pc == 64 ? pc : 24; }

uint32_t fxm_set_value(fxm_effect *e, uint32_t id, const uint32_t *src, uint32_t bytes, int *changed){
    const node *d = nget(e, id);
    if (!writable(d)) return refuse(changed, FXM_INVALIDCALL);
    if (fx2_value_kind(d->n.t) == FX2_V_SAMPLER) return refuse(changed, FXM_FAIL);
    if (!src || bytes < d->n.bytes) return refuse(changed, FXM_INVALIDCALL);
    uint32_t *w = wr(e, d), t = d->n.type;
    int ch = 0;
    /* W's set_value switches on the node's own type: VOID, BOOL, INT, FLOAT, STRING and the textures are
     * copied; any other type (a shader, a sampler type in a numeric class, ...) writes nothing, and the
     * version still rises */
    if (t == FX2_T_VOID || is_num_type(t) || t == FX2_T_STRING || is_texture(t))
        for (uint32_t i = 0; i < d->n.bytes / 4; i++) put(&w[i], src[i], &ch);
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_value(const fxm_effect *e, uint32_t id, uint32_t *dst, uint32_t bytes){
    const node *d = nget(e, id);
    if (!valued(d)) return FXM_INVALIDCALL;
    if (fx2_value_kind(d->n.t) == FX2_V_SAMPLER) return FXM_FAIL;
    if (!dst || bytes < d->n.bytes) return FXM_INVALIDCALL;
    if (!d->n.bytes) return FXM_OK;
    memcpy(dst, rd(e, d), d->n.bytes);
    if (d->n.type == FX2_T_BOOL)                            /* W switches on the node's own type: a struct's */
        for (uint32_t i = 0; i < d->n.bytes / 4; i++) dst[i] = dst[i] != 0;   /* BOOL members stay raw */
    return FXM_OK;
}

uint32_t fxm_set_bool(fxm_effect *e, uint32_t id, uint32_t b, int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || !one_by_one(d)) return refuse(changed, FXM_INVALIDCALL);
    int ch = 0;
    put(wr(e, d), conv(d->n.type, FX2_T_BOOL, b), &ch);
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_bool(const fxm_effect *e, uint32_t id, uint32_t *b){
    const node *d = nget(e, id);
    if (!valued(d) || !one_by_one(d) || !b) return FXM_INVALIDCALL;
    *b = conv(FX2_T_BOOL, d->n.type, rd(e, d)[0]);
    return FXM_OK;
}

uint32_t fxm_set_int(fxm_effect *e, uint32_t id, uint32_t n, int *changed){
    const node *d = nget(e, id);
    if (!writable(d)) return refuse(changed, FXM_INVALIDCALL);
    int ch = 0;
    if (one_by_one(d)){                                       /* X 0x50481E: each type its own test of "the same" */
        uint32_t *w = wr(e, d), t = d->n.type;
        if (t == FX2_T_BOOL){ if (!n == !*w) return done(e, d, 0, 0, changed); put(w, n != 0, &ch); }
        else if (t == FX2_T_FLOAT){                           /* the int against the stored float, exactly */
            float x = fval(*w);
            if (x == x && (double)x == (double)(int32_t)n) return done(e, d, 0, 0, changed);
            put(w, conv(FX2_T_FLOAT, FX2_T_INT, n), &ch);
            return done(e, d, ch, 1, changed);
        }
        else if (t == FX2_T_INT) put(w, n, &ch);
        else return refuse(changed, FXM_INVALIDCALL);           /* no case for any other type */
        return done(e, d, ch, 0, changed);
    }
    if (!colour(d)) return refuse(changed, FXM_INVALIDCALL);
    uint32_t *w = wr(e, d), k = colour_comps(d);
    static const int shift[4] = { 16, 8, 0, 24 };            /* components 0, 1, 2, 3 <- bytes 2, 1, 0, 3 */
    for (uint32_t c = 0; c < k; c++) put(&w[c], from_byte(n >> shift[c]), &ch);
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_int(const fxm_effect *e, uint32_t id, uint32_t *n){
    const node *d = nget(e, id);
    if (!valued(d) || !n) return FXM_INVALIDCALL;
    const uint32_t *r = rd(e, d);
    if (one_by_one(d)){ *n = conv(FX2_T_INT, d->n.type, r[0]); return FXM_OK; }
    if (!colour(d)) return FXM_INVALIDCALL;
    uint32_t k = colour_comps(d);                            /* a component past the storage reads as 0 */
    *n = 0;
    static const int shift[4] = { 16, 8, 0, 24 };
    for (uint32_t c = 0; c < k; c++) *n |= to_byte(r[c]) << shift[c];
    return FXM_OK;
}

uint32_t fxm_set_float(fxm_effect *e, uint32_t id, uint32_t f, int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || !one_by_one(d)) return refuse(changed, FXM_INVALIDCALL);
    uint32_t *w = wr(e, d), t = d->n.type, v;
    int ch = 0;
    /* X 0x505554: FLOAT compares as floats (fucompp: -0 is +0, a NaN always writes); INT and BOOL compare the
     * dword they would store; any other type is refused */
    if (t == FX2_T_FLOAT){
        if (f_equal(f, *w)) return done(e, d, 0, 0, changed);
        put(w, f, &ch);
        return done(e, d, ch, 1, changed);
    }
    if (t == FX2_T_INT) v = f2i_ftol(f);
    else if (t == FX2_T_BOOL) v = f_nonzero(f);
    else return refuse(changed, FXM_INVALIDCALL);
    if (v == *w) return done(e, d, 0, 0, changed);
    put(w, v, &ch);
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_float(const fxm_effect *e, uint32_t id, uint32_t *f){
    const node *d = nget(e, id);
    if (!valued(d) || !one_by_one(d) || !f) return FXM_INVALIDCALL;
    *f = conv(FX2_T_FLOAT, d->n.type, rd(e, d)[0]);
    return FXM_OK;
}

uint32_t fxm_set_array(fxm_effect *e, uint32_t id, uint32_t type, const uint32_t *src, uint32_t count, int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || !is_num_type(type) || d->n.cls > FX2_C_MATRIX_ROWS || (!src && count))
        return refuse(changed, FXM_INVALIDCALL);
    uint32_t k = count < d->n.bytes / 4 ? count : d->n.bytes / 4, from = type == FX2_T_BOOL ? FX2_T_INT : type;
    uint32_t *w = wr(e, d), t = d->n.type;
    int ch = 0;
    for (uint32_t i = 0; i < k; i++){                         /* BOOL input is not cropped */
        uint32_t v = from == FX2_T_FLOAT && t == FX2_T_INT ? f2i_ftol(src[i])           /* X 0x505272 */
                   : from == FX2_T_FLOAT && t == FX2_T_BOOL ? (uint32_t)f_nonzero(src[i]) /* X 0x5052FB */
                   : conv(t, from, src[i]);
        put(&w[i], v, &ch);
    }
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_array(const fxm_effect *e, uint32_t id, uint32_t type, uint32_t *dst, uint32_t count){
    const node *d = nget(e, id);
    /* W tests the pointer first: NULL is refused whatever the count */
    if (!valued(d) || !is_num_type(type) || d->n.cls > FX2_C_MATRIX_COLUMNS || !dst) return FXM_INVALIDCALL;
    uint32_t k = count < d->n.bytes / 4 ? count : d->n.bytes / 4;
    const uint32_t *r = rd(e, d);
    for (uint32_t i = 0; i < k; i++) dst[i] = conv(type, d->n.type, r[i]);
    return FXM_OK;
}

/* SetVector/GetVector's shape, and the components a float4 gives it */
static int vec_shape(const node *d){ return !d->n.elements && (d->n.cls == FX2_C_SCALAR || d->n.cls == FX2_C_VECTOR); }
static uint32_t vec_comps(const node *d, uint64_t room){
    uint64_t c = d->n.cols < 4 ? d->n.cols : 4;
    return (uint32_t)(c < room ? c : room);
}
static int int_colour(const node *d){ return d->n.type == FX2_T_INT && d->n.rows == 1 && d->n.cols == 1; }

uint32_t fxm_set_vector(fxm_effect *e, uint32_t id, const uint32_t v[4], int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || !vec_shape(d) || !v) return refuse(changed, FXM_INVALIDCALL);
    uint32_t *w = wr(e, d);
    int ch = 0;
    if (int_colour(d)){
        int pc = e->x87pc;
        put(w, to_byte_round(v[0], pc) << 16 | to_byte_round(v[1], pc) << 8 | to_byte_round(v[2], pc) |
               to_byte_round(v[3], pc) << 24, &ch);
    }
    else for (uint32_t c = 0, n = vec_comps(d, d->n.bytes / 4); c < n; c++)       /* BOOL: X 0x50433A */
        put(&w[c], d->n.type == FX2_T_BOOL ? (uint32_t)f_nonzero(v[c]) : conv(d->n.type, FX2_T_FLOAT, v[c]), &ch);
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_vector(const fxm_effect *e, uint32_t id, uint32_t v[4]){
    const node *d = nget(e, id);
    if (!valued(d) || !vec_shape(d) || !v) return FXM_INVALIDCALL;
    const uint32_t *r = rd(e, d);
    if (int_colour(d)){
        v[0] = from_byte(r[0] >> 16); v[1] = from_byte(r[0] >> 8); v[2] = from_byte(r[0]); v[3] = from_byte(r[0] >> 24);
        return FXM_OK;
    }
    memset(v, 0, 16);
    for (uint32_t c = 0, n = vec_comps(d, d->n.bytes / 4); c < n; c++) v[c] = conv(FX2_T_FLOAT, d->n.type, r[c]);
    return FXM_OK;
}

/* the dwords one element of array node d takes */
static uint64_t stride(const node *d){ return d->n.elements ? (uint64_t)d->n.bytes / 4 / d->n.elements : 0; }

uint32_t fxm_set_vector_array(fxm_effect *e, uint32_t id, const uint32_t *v, uint32_t count, int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || d->n.cls != FX2_C_VECTOR || !d->n.elements || count > d->n.elements || (!v && count))
        return refuse(changed, FXM_INVALIDCALL);
    uint64_t s = stride(d);
    uint32_t *w = wr(e, d), n = vec_comps(d, s);
    int ch = 0;
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t c = 0; c < n; c++){
            uint32_t x = v[4 * (uint64_t)i + c];
            put(&w[s * i + c], d->n.type == FX2_T_INT ? f2i_ftol(x) : d->n.type == FX2_T_BOOL ? (uint32_t)f_nonzero(x)
                               : conv(d->n.type, FX2_T_FLOAT, x), &ch);           /* X 0x503BC8, 0x503C1A */
        }
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_vector_array(const fxm_effect *e, uint32_t id, uint32_t *v, uint32_t count){
    if (!count) return FXM_OK;                               /* W returns before it looks at the handle */
    const node *d = nget(e, id);
    if (!valued(d) || d->n.cls != FX2_C_VECTOR || !d->n.elements || count > d->n.elements || (!v && count))
        return FXM_INVALIDCALL;
    uint64_t s = stride(d);
    const uint32_t *r = rd(e, d);
    uint32_t n = vec_comps(d, s);
    for (uint32_t i = 0; i < count; i++){
        memset(v + 4 * (uint64_t)i, 0, 16);
        for (uint32_t c = 0; c < n; c++) v[4 * (uint64_t)i + c] = conv(FX2_T_FLOAT, d->n.type, r[s * i + c]);
    }
    return FXM_OK;
}

/* the top-left rows x cols (at most 4 x 4) of the row-major 4x4 m into w */
static void mat_put(const node *d, uint32_t *w, const uint32_t *m, int transpose, int *ch){
    uint32_t R = d->n.rows < 4 ? d->n.rows : 4, C = d->n.cols < 4 ? d->n.cols : 4;
    for (uint32_t i = 0; i < R; i++)
        for (uint32_t k = 0; k < C; k++)
            put(&w[(uint64_t)i * d->n.cols + k], conv(d->n.type, FX2_T_FLOAT, transpose ? m[4 * k + i] : m[4 * i + k]), ch);
}
static void mat_get(const node *d, const uint32_t *r, uint32_t *m, int transpose){
    uint32_t R = d->n.rows < 4 ? d->n.rows : 4, C = d->n.cols < 4 ? d->n.cols : 4;
    memset(m, 0, 64);
    for (uint32_t i = 0; i < R; i++)
        for (uint32_t k = 0; k < C; k++)
            m[transpose ? 4 * k + i : 4 * i + k] = conv(FX2_T_FLOAT, d->n.type, r[(uint64_t)i * d->n.cols + k]);
}

uint32_t fxm_set_matrix(fxm_effect *e, uint32_t id, const uint32_t m[16], int transpose, int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || d->n.cls != FX2_C_MATRIX_ROWS || d->n.elements || !m) return refuse(changed, FXM_INVALIDCALL);
    int ch = 0;
    if (d->n.bytes) mat_put(d, wr(e, d), m, transpose, &ch);
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_matrix(const fxm_effect *e, uint32_t id, uint32_t m[16], int transpose){
    const node *d = nget(e, id);
    if (!valued(d) || d->n.elements || !m) return FXM_INVALIDCALL;
    if (d->n.cls != FX2_C_MATRIX_ROWS && !(transpose && (d->n.cls == FX2_C_SCALAR || d->n.cls == FX2_C_VECTOR)))
        return FXM_INVALIDCALL;
    int tr = transpose && d->n.cls == FX2_C_MATRIX_ROWS;     /* W: a scalar or vector comes back in row 0 */
    if (d->n.bytes) mat_get(d, rd(e, d), m, tr); else memset(m, 0, 64);
    return FXM_OK;
}

uint32_t fxm_set_matrix_array(fxm_effect *e, uint32_t id, const uint32_t *m, uint32_t count, int transpose,
                              int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || d->n.cls != FX2_C_MATRIX_ROWS || count > d->n.elements || (!m && count))   /* a non-array */
        return refuse(changed, FXM_INVALIDCALL);                                                  /* takes count 0 */
    uint64_t s = stride(d);
    int ch = 0;
    for (uint32_t i = 0; i < count && s; i++) mat_put(d, wr(e, d) + s * i, m + 16 * (uint64_t)i, transpose, &ch);
    return done(e, d, ch, 1, changed);
}

uint32_t fxm_get_matrix_array(const fxm_effect *e, uint32_t id, uint32_t *m, uint32_t count, int transpose){
    if (!count) return FXM_OK;                               /* W returns before it looks at the handle */
    const node *d = nget(e, id);
    if (!valued(d) || d->n.cls != FX2_C_MATRIX_ROWS || !d->n.elements || count > d->n.elements || (!m && count))
        return FXM_INVALIDCALL;
    uint64_t s = stride(d);
    for (uint32_t i = 0; i < count; i++){
        if (s) mat_get(d, rd(e, d) + s * i, m + 16 * (uint64_t)i, transpose);
        else memset(m + 16 * (uint64_t)i, 0, 64);
    }
    return FXM_OK;
}

/* SetTexture/GetTexture: a texture type, elements 0; SetString: type STRING (W checks nothing else, so an
 * array takes it in its first dword); GetString: type STRING, elements 0.  W checks no class; a node
 * without storage is refused here (W would write past it). */
static int obj_shape(const node *d, int set){
    if (d->n.bytes < 4) return 0;
    if (is_texture(d->n.type)) return !d->n.elements;
    return d->n.type == FX2_T_STRING && (set || !d->n.elements);
}

uint32_t fxm_set_object(fxm_effect *e, uint32_t id, uint32_t ptr, uint32_t *old, int *changed){
    const node *d = nget(e, id);
    if (!writable(d) || !obj_shape(d, 1)) return refuse(changed, FXM_INVALIDCALL);
    uint32_t *w = wr(e, d);
    if (old) *old = *w;
    int ch = 0;
    put(w, ptr, &ch);
    return done(e, d, ch, d->n.type == FX2_T_STRING, changed);   /* a texture: only when it changes */
}

uint32_t fxm_get_object(const fxm_effect *e, uint32_t id, uint32_t *ptr){
    const node *d = nget(e, id);
    if (!valued(d) || !obj_shape(d, 0) || !ptr) return FXM_INVALIDCALL;
    *ptr = rd(e, d)[0];
    return FXM_OK;
}

const uint32_t *fxm_data(const fxm_effect *e, uint32_t id){
    const node *d = nget(e, id);
    return valued(d) ? rd(e, d) : NULL;
}

uint64_t fxm_version(const fxm_effect *e, uint32_t id){
    const node *d = nget(e, id);
    return writable(d) ? *e->blocks[d->pidx].ver : 0;
}

uint64_t fxm_clock(const fxm_effect *e){ return e ? *e->clock : 0; }

/* ---- §3 queries ---- */

fxm_source fxm_state_source(const fxm_effect *e, const fx2_state *s){
    fxm_source o;
    memset(&o, 0, sizeof o);
    o.src = FXM_S_LITERAL;
    if (!e || !s || !s->res) return o;
    for (const fx2_res *r = s->res; r; r = r->next) o.nres++;
    const fx2_res *r = o.res = last_res(s);
    uintptr_t a = (uintptr_t)e->fx->res, x = (uintptr_t)r;
    if (x < a || x >= a + (uintptr_t)e->fx->nres * sizeof *r){ o.src = FXM_S_UNKNOWN; return o; }   /* another effect's */
    uint32_t ri = (uint32_t)((x - a) / sizeof *r);
    if (r->usage == 1){ o.src = FXM_S_PARAM; o.id = usage1(e, r); }
    else if (r->usage == 0 && e->res2prog[ri]){
        o.prog = &e->progs[e->res2prog[ri] - 1];
        o.src = o.prog->kind == FXM_P_SHADER ? FXM_S_SHADER : FXM_S_PRESHADER;
    } else o.src = FXM_S_UNKNOWN;
    return o;
}

uint32_t fxm_pass_deps(const fxm_effect *e, uint32_t pass, const uint32_t **ids){
    const node *d = nget(e, pass);
    if (ids) *ids = NULL;
    if (!d || d->n.kind != FXM_K_PASS) return 0;
    if (ids && d->ndeps) *ids = e->deps + d->dep0;
    return d->ndeps;
}

void fxm_tech_versions(const fxm_effect *e, uint32_t tech, uint32_t *vs, uint32_t *ps){
    const node *d = nget(e, tech);
    int ok = d && d->n.kind == FXM_K_TECH;
    if (vs) *vs = ok ? d->vs : 0;
    if (ps) *ps = ok ? d->ps : 0;
}

const fxm_prog *fxm_progs(const fxm_effect *e, uint32_t *n){
    if (n) *n = e ? e->nprogs : 0;
    return e && e->nprogs ? e->progs : NULL;
}
