/* fx2.c -- compiled D3DX effects (fx_2_0): the container parser and the
 * shader, CTAB and preshader readers.  fx2.h says what each part does;
 * docs/mw/fx2-format.md is the format and the section numbers are its. */
#include "fx2.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_types[FX2_NTYPES] = {
    "VOID", "BOOL", "INT", "FLOAT", "STRING", "TEXTURE", "TEXTURE1D", "TEXTURE2D", "TEXTURE3D",
    "TEXTURECUBE", "SAMPLER", "SAMPLER1D", "SAMPLER2D", "SAMPLER3D", "SAMPLERCUBE", "PIXELSHADER",
    "VERTEXSHADER", "PIXELFRAGMENT", "VERTEXFRAGMENT", "UNSUPPORTED" };
static const char *const k_classes[FX2_NCLASSES] = {
    "SCALAR", "VECTOR", "MATRIX_ROWS", "MATRIX_COLUMNS", "OBJECT", "STRUCT" };

/* §3.1: the operation of a state entry is a row of this table */
static const char *const k_states[FX2_NSTATES] = {
    "ZENABLE", "FILLMODE", "SHADEMODE", "ZWRITEENABLE", "ALPHATESTENABLE", "LASTPIXEL",
    "SRCBLEND", "DESTBLEND", "CULLMODE", "ZFUNC", "ALPHAREF", "ALPHAFUNC",
    "DITHERENABLE", "ALPHABLENDENABLE", "FOGENABLE", "SPECULARENABLE", "FOGCOLOR", "FOGTABLEMODE",
    "FOGSTART", "FOGEND", "FOGDENSITY", "RANGEFOGENABLE", "STENCILENABLE", "STENCILFAIL",
    "STENCILZFAIL", "STENCILPASS", "STENCILFUNC", "STENCILREF", "STENCILMASK", "STENCILWRITEMASK",
    "TEXTUREFACTOR", "WRAP0", "WRAP1", "WRAP2", "WRAP3", "WRAP4",
    "WRAP5", "WRAP6", "WRAP7", "WRAP8", "WRAP9", "WRAP10",              /* 39..46: WRAP8..15 right after WRAP7 (§3.2) */
    "WRAP11", "WRAP12", "WRAP13", "WRAP14", "WRAP15", "CLIPPING",
    "LIGHTING", "AMBIENT", "FOGVERTEXMODE", "COLORVERTEX", "LOCALVIEWER", "NORMALIZENORMALS",
    "DIFFUSEMATERIALSOURCE", "SPECULARMATERIALSOURCE", "AMBIENTMATERIALSOURCE", "EMISSIVEMATERIALSOURCE", "VERTEXBLEND", "CLIPPLANEENABLE",
    "POINTSIZE", "POINTSIZE_MIN", "POINTSIZE_MAX", "POINTSPRITEENABLE", "POINTSCALEENABLE", "POINTSCALE_A",
    "POINTSCALE_B", "POINTSCALE_C", "MULTISAMPLEANTIALIAS", "MULTISAMPLEMASK", "PATCHEDGESTYLE", "DEBUGMONITORTOKEN",
    "INDEXEDVERTEXBLENDENABLE", "COLORWRITEENABLE", "TWEENFACTOR", "BLENDOP", "POSITIONDEGREE", "NORMALDEGREE",
    "SCISSORTESTENABLE", "SLOPESCALEDEPTHBIAS", "ANTIALIASEDLINEENABLE", "MINTESSELLATIONLEVEL", "MAXTESSELLATIONLEVEL", "ADAPTIVETESS_X",
    "ADAPTIVETESS_Y", "ADAPTIVETESS_Z", "ADAPTIVETESS_W", "ENABLEADAPTIVETESSELLATION", "TWOSIDEDSTENCILMODE", "CCW_STENCILFAIL",
    "CCW_STENCILZFAIL", "CCW_STENCILPASS", "CCW_STENCILFUNC", "COLORWRITEENABLE1", "COLORWRITEENABLE2", "COLORWRITEENABLE3",
    "BLENDFACTOR", "SRGBWRITEENABLE", "DEPTHBIAS", "SEPARATEALPHABLENDENABLE", "SRCBLENDALPHA", "DESTBLENDALPHA",
    "BLENDOPALPHA", "COLOROP", "COLORARG0", "COLORARG1", "COLORARG2", "ALPHAOP",
    "ALPHAARG0", "ALPHAARG1", "ALPHAARG2", "RESULTARG", "BUMPENVMAT00", "BUMPENVMAT01",
    "BUMPENVMAT10", "BUMPENVMAT11", "TEXCOORDINDEX", "BUMPENVLSCALE", "BUMPENVLOFFSET", "TEXTURETRANSFORMFLAGS",
    "CONSTANT", "PATCHSEGMENTS", "FVF", "PROJECTIONTRANSFORM", "VIEWTRANSFORM", "WORLDTRANSFORM",
    "TEXTURETRANSFORM", "MATERIALDIFFUSE", "MATERIALAMBIENT", "MATERIALSPECULAR", "MATERIALEMISSIVE", "MATERIALPOWER",
    "LIGHTTYPE", "LIGHTDIFFUSE", "LIGHTSPECULAR", "LIGHTAMBIENT", "LIGHTPOSITION", "LIGHTDIRECTION",
    "LIGHTRANGE", "LIGHTFALLOFF", "LIGHTATTENUATION0", "LIGHTATTENUATION1", "LIGHTATTENUATION2", "LIGHTTHETA",
    "LIGHTPHI", "LIGHTENABLE", "VERTEXSHADER", "PIXELSHADER", "VERTEXSHADERCONSTANTF", "VERTEXSHADERCONSTANTB",
    "VERTEXSHADERCONSTANTI", "VERTEXSHADERCONSTANT", "VERTEXSHADERCONSTANT1", "VERTEXSHADERCONSTANT2", "VERTEXSHADERCONSTANT3", "VERTEXSHADERCONSTANT4",
    "PIXELSHADERCONSTANTF", "PIXELSHADERCONSTANTB", "PIXELSHADERCONSTANTI", "PIXELSHADERCONSTANT", "PIXELSHADERCONSTANT1", "PIXELSHADERCONSTANT2",
    "PIXELSHADERCONSTANT3", "PIXELSHADERCONSTANT4", "TEXTURE", "ADDRESSU", "ADDRESSV", "ADDRESSW",
    "BORDERCOLOR", "MAGFILTER", "MINFILTER", "MIPFILTER", "MIPMAPLODBIAS", "MAXMIPLEVEL",
    "MAXANISOTROPY", "SRGBTEXTURE", "ELEMENTINDEX", "DMAPOFFSET", "SAMPLER", "STATEBLOCK",
    "$PRESHADER_VS", "$PRESHADER_PS" };

const char *fx2_type_name(uint32_t type){ return type < FX2_NTYPES ? k_types[type] : NULL; }
const char *fx2_class_name(uint32_t cls){ return cls < FX2_NCLASSES ? k_classes[cls] : NULL; }
const char *fx2_state_name(uint32_t op){ return op < FX2_NSTATES ? k_states[op] : NULL; }

/* the bytes before the first NUL, all n without one */
static uint32_t len_nul(const uint8_t *p, uint32_t n){
    uint32_t k = 0;
    while (k < n && p[k]) k++;
    return k;
}

uint32_t fx2_value_kind(const fx2_type *t){
    if (t->cls <= FX2_C_MATRIX_COLUMNS) return FX2_V_NUMERIC;
    if (t->cls == FX2_C_OBJECT)
        return t->type >= FX2_T_SAMPLER && t->type <= FX2_T_SAMPLERCUBE ? FX2_V_SAMPLER : FX2_V_OBJECT;
    if (t->cls == FX2_C_STRUCT) return FX2_V_STRUCT;
    return FX2_V_UNKNOWN;
}

/* ---- the parse ---- */

/* The tree's allocations come from chunks, freed together by fx2_free; a
 * new chunk is pushed in front (a large request gets one of its own). */
typedef struct mchunk { struct mchunk *next; size_t used, cap; } mchunk;
#define M_ALIGN  16
#define M_HDR    ((sizeof(mchunk) + M_ALIGN - 1) & ~(size_t)(M_ALIGN - 1))
#define M_CHUNK  65536

typedef struct {
    fx2_effect *fx;
    const uint8_t *b;            /* the body: file + 8 */
    uint64_t bn;                 /* body bytes */
    uint64_t dn;                 /* the data area is body [0, dn): dn = tables */
    uint64_t pos;                /* the table area cursor, a body offset */
    uint64_t budget;             /* typedefs and value elements still allowed */
    int depth;
    int failed;
    char *err;
    uint32_t errlen;
} ctx;

static int fail(ctx *c, const char *fmt, ...){
    if (!c->failed && c->err && c->errlen){
        va_list ap; va_start(ap, fmt); vsnprintf(c->err, c->errlen, fmt, ap); va_end(ap);
    }
    c->failed = 1;
    return -1;
}

/* n zeroed items of each bytes; NULL for n = 0, and NULL with c->failed set
 * when memory runs out */
static void *alloc(ctx *c, uint64_t n, size_t each){
    if (!n) return NULL;
    if (n > ((uint64_t)1 << 30) / each){ fail(c, "out of memory (%llu items)", (unsigned long long)n); return NULL; }
    size_t bytes = ((size_t)n * each + M_ALIGN - 1) & ~(size_t)(M_ALIGN - 1);
    mchunk *m = c->fx->mem;
    if (!m || m->cap - m->used < bytes){
        size_t cap = bytes > M_CHUNK ? bytes : M_CHUNK;
        mchunk *k = calloc(1, M_HDR + cap);
        if (!k){ fail(c, "out of memory"); return NULL; }
        k->cap = cap; k->next = m; c->fx->mem = k; m = k;
    }
    void *p = (uint8_t *)m + M_HDR + m->used;
    m->used += bytes;
    return p;
}

/* n bytes at body offset off, inside the data area, or NULL (failed) */
static const uint8_t *data_at(ctx *c, uint64_t off, uint64_t n, const char *what){
    if (off > c->dn || n > c->dn - off){
        fail(c, "%s at body +0x%llx (%llu bytes) is outside the data area (0x%llx bytes)", what,
             (unsigned long long)off, (unsigned long long)n, (unsigned long long)c->dn);
        return NULL;
    }
    return c->b + off;
}

/* the table area: bytes left at the cursor, n bytes there (then past them) */
static uint64_t tab_left(const ctx *c){ return c->pos < c->bn ? c->bn - c->pos : 0; }
static const uint8_t *tab_take(ctx *c, uint64_t n, const char *what){
    if (n > tab_left(c)){
        fail(c, "%s at body +0x%llx (%llu bytes) runs past the end of the file", what,
             (unsigned long long)c->pos, (unsigned long long)n);
        return NULL;
    }
    const uint8_t *p = c->b + c->pos;
    c->pos += n;
    return p;
}
static int tab32(ctx *c, uint32_t *v, const char *what){
    const uint8_t *p = tab_take(c, 4, what);
    if (!p) return -1;
    *v = fx2_rd32(p);
    return 0;
}
/* n entries of at least each bytes must still fit (checked before allocating them) */
static int tab_count(ctx *c, uint32_t n, uint32_t each, const char *what){
    if ((uint64_t)n * each > tab_left(c))
        return fail(c, "%s: %u entries at body +0x%llx do not fit in the %llu bytes left", what, n,
                    (unsigned long long)c->pos, (unsigned long long)tab_left(c));
    return 0;
}
/* a size-prefixed blob in the table area (object data, resources): size
 * bytes, then the cursor moves past them and their pad to 4 */
static const uint8_t *tab_blob(ctx *c, uint32_t size, const char *what){
    if (size > tab_left(c)){
        fail(c, "%s at body +0x%llx: %u bytes run past the end of the file", what, (unsigned long long)c->pos, size);
        return NULL;
    }
    const uint8_t *p = c->b + c->pos;
    c->pos += ((uint64_t)size + 3) & ~(uint64_t)3;
    return p;
}

/* §2.2: a name (size with its NUL, then the bytes); offset 0 is none */
static int name_at(ctx *c, uint32_t off, fx2_str *s, const char *what){
    s->p = NULL; s->n = 0;
    if (!off) return 0;
    const uint8_t *q = data_at(c, off, 4, what);
    if (!q) return -1;
    uint32_t n = fx2_rd32(q);
    const uint8_t *t = data_at(c, (uint64_t)off + 4, n, what);
    if (!t) return -1;
    s->p = t; s->n = len_nul(t, n);
    return 0;
}

static int spend(ctx *c, uint64_t n, uint64_t off, const char *what){
    if (n > c->budget)
        return fail(c, "%s at body +0x%llx: more typedefs and value elements than a file of %u bytes holds",
                    what, (unsigned long long)off, c->fx->size);
    c->budget -= n;
    return 0;
}

/* §2.3: the typedef at off; *end is the offset after it (struct members follow it) */
static int type_read(ctx *c, uint64_t off, fx2_type *t, uint64_t *end){
    if (spend(c, 1, off, "typedef")) return -1;
    if (++c->depth > FX2_DEPTH) return fail(c, "typedef at body +0x%llx: nested deeper than %d", (unsigned long long)off, FX2_DEPTH);
    const uint8_t *q = data_at(c, off, 20, "typedef");
    if (!q) return -1;
    t->off = (uint32_t)off;
    t->type = fx2_rd32(q); t->cls = fx2_rd32(q + 4); t->elements = fx2_rd32(q + 16);
    if (name_at(c, fx2_rd32(q + 8), &t->name, "typedef name") ||
        name_at(c, fx2_rd32(q + 12), &t->sem, "typedef semantic")) return -1;
    uint64_t p = off + 20;
    if (t->cls <= FX2_C_MATRIX_COLUMNS){
        if (!(q = data_at(c, p, 8, "typedef dimensions"))) return -1;
        uint32_t a = fx2_rd32(q), b = fx2_rd32(q + 4);
        if (t->cls == FX2_C_VECTOR){ t->cols = a; t->rows = b; }   /* a vector stores columns first */
        else { t->rows = a; t->cols = b; }
        p += 8;
    } else if (t->cls == FX2_C_STRUCT){
        if (!(q = data_at(c, p, 4, "struct member count"))) return -1;
        t->nmembers = fx2_rd32(q);
        p += 4;
        if ((uint64_t)t->nmembers * 20 > c->dn - p)      /* a member typedef takes 20 bytes or more */
            return fail(c, "typedef at body +0x%llx: %u members do not fit in the data area", (unsigned long long)off, t->nmembers);
        t->members = alloc(c, t->nmembers, sizeof(fx2_type));
        if (c->failed) return -1;
        for (uint32_t m = 0; m < t->nmembers; m++)
            if (type_read(c, p, &t->members[m], &p)) return -1;
    }
    c->depth--;
    *end = p;
    return 0;
}

static int value_read(ctx *c, const fx2_type *t, uint64_t off, fx2_value *v);

/* §2.5.5: a state entry's 16 bytes, from a pass or a sampler block */
static int type_value(ctx *c, uint32_t toff, uint32_t voff, fx2_type **t, fx2_value *v){
    uint64_t end;
    *t = alloc(c, 1, sizeof(fx2_type));
    if (c->failed || type_read(c, toff, *t, &end)) return -1;
    return value_read(c, *t, voff, v);
}
static int state_read(ctx *c, const uint8_t *q, fx2_state *s){
    s->op = fx2_rd32(q); s->index = fx2_rd32(q + 4);
    return type_value(c, fx2_rd32(q + 8), fx2_rd32(q + 12), &s->t, &s->v);
}

/* §2.4: a sampler element is a state count, then 16 bytes per state */
static int block_read(ctx *c, uint64_t *p, fx2_block *bk){
    const uint8_t *q = data_at(c, *p, 4, "sampler state count");
    if (!q) return -1;
    bk->nstates = fx2_rd32(q);
    *p += 4;
    if (!(q = data_at(c, *p, (uint64_t)bk->nstates * 16, "sampler states"))) return -1;
    bk->states = alloc(c, bk->nstates, sizeof(fx2_state));
    if (c->failed) return -1;
    for (uint32_t k = 0; k < bk->nstates; k++)
        if (state_read(c, q + 16 * (uint64_t)k, &bk->states[k])) return -1;
    *p += (uint64_t)bk->nstates * 16;
    return 0;
}

/* an object id held by a value of this type: the first one names the object's type */
static void hold(ctx *c, uint32_t id, uint32_t type){
    fx2_object *o = id < c->fx->nobjects ? &c->fx->objects[id] : NULL;
    if (o && !o->held){ o->held = 1; o->type = type; }
}

/* §2.4: the value of typedef t at off, element after element */
static int value_read(ctx *c, const fx2_type *t, uint64_t off, fx2_value *v){
    v->t = t; v->kind = fx2_value_kind(t); v->n = t->elements ? t->elements : 1; v->off = (uint32_t)off;
    if (spend(c, v->n, off, "value")) return -1;
    if (++c->depth > FX2_DEPTH) return fail(c, "value at body +0x%llx: nested deeper than %d", (unsigned long long)off, FX2_DEPTH);
    uint64_t room = off < c->dn ? c->dn - off : 0, p = off;
    switch (v->kind){
    case FX2_V_NUMERIC: {
        uint64_t per = (uint64_t)t->rows * t->cols;      /* dwords per element */
        if (per && v->n > room / 4 / per)
            return fail(c, "value at body +0x%llx: %u x %llu dwords do not fit in the data area",
                        (unsigned long long)off, v->n, (unsigned long long)per);
        if (!(v->data = data_at(c, off, per * v->n * 4, "value"))) return -1;
        p += per * v->n * 4;
        break; }
    case FX2_V_OBJECT:
        if (!(v->data = data_at(c, off, (uint64_t)v->n * 4, "object value"))) return -1;
        for (uint32_t e = 0; e < v->n; e++) hold(c, fx2_rd32(v->data + 4 * (uint64_t)e), t->type);
        p += (uint64_t)v->n * 4;
        break;
    case FX2_V_SAMPLER:
        if ((uint64_t)v->n * 4 > room)
            return fail(c, "value at body +0x%llx: %u sampler blocks do not fit in the data area", (unsigned long long)off, v->n);
        v->blocks = alloc(c, v->n, sizeof(fx2_block));
        if (c->failed) return -1;
        for (uint32_t e = 0; e < v->n; e++)
            if (block_read(c, &p, &v->blocks[e])) return -1;
        break;
    case FX2_V_STRUCT: {
        uint64_t k = (uint64_t)v->n * t->nmembers;       /* each member value spends at least 1 */
        if (k > c->budget) return spend(c, k, off, "struct value");
        v->members = alloc(c, k, sizeof(fx2_value));
        if (c->failed) return -1;
        for (uint64_t i = 0; i < k; i++){
            fx2_value *mv = &v->members[i];
            if (value_read(c, &t->members[i % t->nmembers], p, mv)) return -1;
            p += mv->size;
        }
        break; }
    default:                                                /* a class past the table: no data */
        break;
    }
    v->size = (uint32_t)(p - off);
    c->depth--;
    return 0;
}

/* §2.5.2: n annotation entries at the cursor */
static int annots_read(ctx *c, uint32_t n, fx2_annot **out){
    if (tab_count(c, n, 8, "annotations")) return -1;
    fx2_annot *a = *out = alloc(c, n, sizeof(fx2_annot));
    if (c->failed) return -1;
    for (uint32_t k = 0; k < n; k++){
        uint32_t toff, voff;
        if (tab32(c, &toff, "annotation typedef") || tab32(c, &voff, "annotation value") ||
            type_value(c, toff, voff, &a[k].t, &a[k].v)) return -1;
    }
    return 0;
}

/* n state entries at the cursor (a pass's) */
static int states_read(ctx *c, uint32_t n, fx2_state **out){
    if (tab_count(c, n, 16, "states")) return -1;
    fx2_state *s = *out = alloc(c, n, sizeof(fx2_state));
    if (c->failed) return -1;
    for (uint32_t k = 0; k < n; k++){
        const uint8_t *q = tab_take(c, 16, "state");
        if (!q || state_read(c, q, &s[k])) return -1;
    }
    return 0;
}

/* §5.6: the state a resource belongs to, or NULL */
static fx2_state *res_target(const fx2_effect *fx, const fx2_res *r){
    if (r->tech != FX2_NONE){
        if (r->tech >= fx->ntechs) return NULL;
        const fx2_tech *t = &fx->techs[r->tech];
        if (r->index >= t->npasses) return NULL;
        const fx2_pass *ps = &t->passes[r->index];
        return r->state < ps->nstates ? &ps->states[r->state] : NULL;
    }
    if (r->index >= fx->nparams) return NULL;
    const fx2_value *v = &fx->params[r->index].v;
    if (v->kind != FX2_V_SAMPLER) return NULL;
    uint32_t e = 0;
    if (v->t->elements){ if (r->element >= v->t->elements) return NULL; e = r->element; }
    const fx2_block *bk = &v->blocks[e];
    return r->state < bk->nstates ? &bk->states[r->state] : NULL;
}

static int parse(ctx *c, const uint8_t *file, uint32_t size){
    fx2_effect *fx = c->fx;
    if (size < 12) return fail(c, "%u bytes: shorter than the header", size);
    fx->tag = fx2_rd32(file); fx->tables = fx2_rd32(file + 4); fx->data0 = fx2_rd32(file + 8);
    c->b = file + 8; c->bn = size - 8;
    if (fx->tables > c->bn) return fail(c, "table area at body +0x%x is past the end of the file", fx->tables);
    c->dn = c->pos = fx->tables;
    /* every typedef takes 20 bytes and every value element 4 or more, each
     * referenced once: a file that needs more nodes than it has bytes
     * repeats or nests them (a bound on time and memory; §5.11 has this
     * and every other reason a file is refused) */
    c->budget = (uint64_t)size + 64;

    /* §2.5 */
    if (tab32(c, &fx->nparams, "parameter count") || tab32(c, &fx->ntechs, "technique count") ||
        tab32(c, &fx->shaders, "shader count") || tab32(c, &fx->nobjects, "object count")) return -1;
    if (fx->nobjects > size / 4 + 1)      /* every id but 0 is held by a 4-byte value */
        return fail(c, "object count %u is more than a file of %u bytes holds", fx->nobjects, size);
    fx->objects = alloc(c, fx->nobjects, sizeof(fx2_object));
    if (c->failed) return -1;

    if (tab_count(c, fx->nparams, 16, "parameters")) return -1;
    fx->params = alloc(c, fx->nparams, sizeof(fx2_param));
    if (c->failed) return -1;
    for (uint32_t i = 0; i < fx->nparams; i++){
        fx2_param *pa = &fx->params[i];
        uint32_t toff, voff;
        if (tab32(c, &toff, "parameter typedef") || tab32(c, &voff, "parameter value") ||
            tab32(c, &pa->flags, "parameter flags") || tab32(c, &pa->nannots, "parameter annotation count") ||
            type_value(c, toff, voff, &pa->t, &pa->v) || annots_read(c, pa->nannots, &pa->annots)) return -1;
    }

    if (tab_count(c, fx->ntechs, 12, "techniques")) return -1;
    fx->techs = alloc(c, fx->ntechs, sizeof(fx2_tech));
    if (c->failed) return -1;
    for (uint32_t i = 0; i < fx->ntechs; i++){
        fx2_tech *te = &fx->techs[i];
        uint32_t noff;
        if (tab32(c, &noff, "technique name") || name_at(c, noff, &te->name, "technique name") ||
            tab32(c, &te->nannots, "technique annotation count") || tab32(c, &te->npasses, "pass count") ||
            annots_read(c, te->nannots, &te->annots) || tab_count(c, te->npasses, 12, "passes")) return -1;
        te->passes = alloc(c, te->npasses, sizeof(fx2_pass));
        if (c->failed) return -1;
        for (uint32_t j = 0; j < te->npasses; j++){
            fx2_pass *ps = &te->passes[j];
            if (tab32(c, &noff, "pass name") || name_at(c, noff, &ps->name, "pass name") ||
                tab32(c, &ps->nannots, "pass annotation count") || tab32(c, &ps->nstates, "pass state count") ||
                annots_read(c, ps->nannots, &ps->annots) || states_read(c, ps->nstates, &ps->states)) return -1;
        }
    }

    if (tab32(c, &fx->nodata, "object-data count") || tab32(c, &fx->nres, "resource count")) return -1;
    if (tab_count(c, fx->nodata, 8, "object data")) return -1;
    fx->odata = alloc(c, fx->nodata, sizeof(fx2_odata));
    if (c->failed) return -1;
    for (uint32_t k = 0; k < fx->nodata; k++){
        fx2_odata *od = &fx->odata[k];
        if (tab32(c, &od->id, "object-data id") || tab32(c, &od->size, "object-data size") ||
            !(od->data = tab_blob(c, od->size, "object data"))) return -1;
    }
    if (tab_count(c, fx->nres, 24, "resources")) return -1;
    fx->res = alloc(c, fx->nres, sizeof(fx2_res));
    if (c->failed) return -1;
    for (uint32_t k = 0; k < fx->nres; k++){
        fx2_res *r = &fx->res[k];
        if (tab32(c, &r->tech, "resource technique") || tab32(c, &r->index, "resource index") ||
            tab32(c, &r->element, "resource element") || tab32(c, &r->state, "resource state") ||
            tab32(c, &r->usage, "resource usage") || tab32(c, &r->size, "resource size") ||
            !(r->data = tab_blob(c, r->size, "resource data"))) return -1;
    }
    /* each state's list in file order: link from the last resource back */
    for (uint32_t k = fx->nres; k-- > 0; ){
        fx2_res *r = &fx->res[k];
        if ((r->target = res_target(fx, r))){ r->next = r->target->res; r->target->res = r; }
    }
    return 0;
}

int fx2_parse(fx2_effect *fx, const uint8_t *file, uint32_t size, char *err, uint32_t errlen){
    memset(fx, 0, sizeof *fx);
    fx->file = file; fx->size = size;
    ctx c;
    memset(&c, 0, sizeof c);
    c.fx = fx; c.err = err; c.errlen = errlen;
    if (err && errlen) err[0] = 0;
    if (parse(&c, file, size) == 0) return 0;
    fx2_free(fx);
    return -1;
}

void fx2_free(fx2_effect *fx){
    mchunk *m = fx->mem;
    while (m){ mchunk *n = m->next; free(m); m = n; }
    memset(fx, 0, sizeof *fx);
}

/* ---- shaders (§4.1, §5.9) ---- */

uint32_t fx2_comment_bytes(uint32_t ndw, uint32_t pos, uint32_t len){
    uint64_t end = (uint64_t)pos + 1 + len;
    if (end > ndw) end = ndw;
    return end > (uint64_t)pos + 1 ? (uint32_t)(end - pos - 1) * 4 : 0;
}

void fx2_shader_walk(const uint8_t *p, uint32_t size, fx2_shader *s, fx2_comment_fn fn, void *ctx){
    memset(s, 0, sizeof *s);
    s->ndw = size / 4;
    s->version = s->ndw ? fx2_rd32(p) : 0;
    if (s->version >> 16 != 0xFFFE && s->version >> 16 != 0xFFFF) return;
    s->known = 1;
    uint32_t major = s->version >> 8 & 0xFF, i = 1;      /* i stays below 2^30 + 2^15 */
    while (i < s->ndw){
        uint32_t tok = fx2_rd32(p + 4 * (uint64_t)i), op = tok & 0xFFFF;
        if (tok == FX2_END) break;
        if (op == 0xFFFE){                                  /* a comment: L dwords follow */
            uint32_t len = tok >> 16 & 0x7FFF;
            s->ncomments++;
            if (fn) fn(ctx, i, len);
            i += 1 + len;
            continue;
        }
        s->ninstr++;
        if (major >= 2) i += 1 + (tok >> 24 & 0xF);         /* the length field */
        else if (op == 0x51 || op == 0x30) i += 6;          /* def, defi */
        else if (op == 0x2F) i += 3;                        /* defb */
        else {                                              /* every following token with bit 31 */
            i++;
            while (i < s->ndw && fx2_rd32(p + 4 * (uint64_t)i) & 0x80000000u) i++;
        }
    }
    s->stop = i;
    s->ended = i + 1 == s->ndw && fx2_rd32(p + 4 * (uint64_t)i) == FX2_END;
}

/* ---- constant tables (§4.2) ---- */

int fx2_ctab_init(fx2_ctab *c, const uint8_t *p, uint32_t n){
    memset(c, 0, sizeof *c);
    if (n < 28) return -1;
    c->p = p; c->n = n;
    c->size = fx2_rd32(p); c->creator = fx2_rd32(p + 4); c->version = fx2_rd32(p + 8);
    c->nconsts = fx2_rd32(p + 12); c->cinfo = fx2_rd32(p + 16); c->flags = fx2_rd32(p + 20); c->target = fx2_rd32(p + 24);
    return 0;
}

int fx2_ctab_const(const fx2_ctab *c, uint32_t k, fx2_const *o){
    memset(o, 0, sizeof *o);
    uint64_t e = (uint64_t)c->cinfo + (uint64_t)k * 20;
    if (e + 20 > c->n) return -1;
    const uint8_t *q = c->p + e;
    o->name = fx2_rd32(q); o->set = fx2_rd16(q + 4); o->reg = fx2_rd16(q + 6); o->count = fx2_rd16(q + 8);
    o->reserved = fx2_rd16(q + 10); o->typeinfo = fx2_rd32(q + 12); o->defval = fx2_rd32(q + 16);
    if ((uint64_t)o->typeinfo + 16 <= c->n){
        const uint8_t *t = c->p + o->typeinfo;
        o->typed = 1;
        o->cls = fx2_rd16(t); o->type = fx2_rd16(t + 2); o->rows = fx2_rd16(t + 4); o->cols = fx2_rd16(t + 6);
        o->elements = fx2_rd16(t + 8); o->members = fx2_rd16(t + 10); o->memberinfo = fx2_rd32(t + 12);
    }
    return 0;
}

int fx2_ctab_str(const fx2_ctab *c, uint32_t off, fx2_str *s){
    s->p = NULL; s->n = 0;
    if (!off) return 0;
    if (off >= c->n) return -1;
    s->p = c->p + off; s->n = len_nul(s->p, c->n - off);
    return 1;
}

/* ---- preshaders (§4.3, §5.10) ---- */

void fx2_pres_read(const uint8_t *p, uint32_t size, fx2_pres *ps, fx2_comment_fn fn, void *ctx){
    memset(ps, 0, sizeof *ps);
    ps->ndw = size / 4;
    ps->version = ps->ndw ? fx2_rd32(p) : 0;
    if (ps->version >> 16 != 0x4658) return;
    ps->known = 1;
    uint32_t i = 1, tok;
    while (i < ps->ndw && ((tok = fx2_rd32(p + 4 * (uint64_t)i)) & 0xFFFF) == 0xFFFE){
        uint32_t len = tok >> 16 & 0x7FFF;
        ps->ncomments++;
        if (fn) fn(ctx, i, len);
        if (fx2_comment_bytes(ps->ndw, i, len) >= 4){
            uint32_t tag = fx2_rd32(p + 4 * ((uint64_t)i + 1));
            if (tag == FX2_CTAB && !ps->ctab){ ps->ctab = i; ps->ctab_len = len; }
            if (tag == FX2_CLIT && !ps->clit){ ps->clit = i; ps->clit_len = len; }
            if (tag == FX2_FXLC && !ps->fxlc){ ps->fxlc = i; ps->fxlc_len = len; }
        }
        i += 1 + len;
    }
    ps->stop = i;
    ps->ended = i + 1 == ps->ndw && fx2_rd32(p + 4 * (uint64_t)i) == FX2_END;
}

int fx2_clit(const uint8_t *q, uint32_t n, uint32_t *count, const uint8_t **lits){
    *count = 0; *lits = NULL;
    if (n < 4) return -1;
    *count = fx2_rd32(q);
    if ((uint64_t)*count * 8 > n - 4) return -2;
    *lits = q + 4;
    return 0;
}

uint32_t fx2_fxlc_operand(const uint8_t *q, uint32_t ndw, uint32_t pos, fx2_operand *o){
    memset(o, 0, sizeof *o);
    if (pos >= ndw) return 0;
    o->flag = fx2_rd32(q + 4 * (uint64_t)pos);
    uint32_t words = o->flag == 0 ? 3 : o->flag == 1 ? 5 : 0;
    if (!words || ndw - pos < words) return 0;
    const uint8_t *w = q + 4 * (uint64_t)pos;
    if (o->flag == 1){ o->itable = fx2_rd32(w + 4); o->ioffset = fx2_rd32(w + 8); w += 8; }
    o->table = fx2_rd32(w + 4); o->offset = fx2_rd32(w + 8);
    return pos + words;
}

int fx2_fxlc_ins(const uint8_t *q, uint32_t ndw, uint32_t pos, fx2_ins *ins){
    memset(ins, 0, sizeof *ins);
    ins->pos = pos;
    ins->status = FX2_INS_SHORT;
    if (pos > ndw || ndw - pos < 2) return ins->status;
    const uint8_t *w = q + 4 * (uint64_t)pos;
    ins->token = fx2_rd32(w); ins->nin = fx2_rd32(w + 4);
    ins->ncomp = ins->token & 0xFFFF; ins->opcode = ins->token >> 20 & 0x7FF; ins->scalar = ins->token >> 31;
    uint32_t i = ins->in = pos + 2;
    for (uint64_t k = 0; ; k++){           /* nin inputs, then the output */
        if (i >= ndw) return ins->status;
        uint32_t flag = fx2_rd32(q + 4 * (uint64_t)i);
        if (flag > 1){ ins->status = FX2_INS_FLAG; ins->flag = flag; ins->end = i; return ins->status; }
        fx2_operand o;
        uint32_t next = fx2_fxlc_operand(q, ndw, i, &o);
        if (!next) return ins->status;
        if (k == ins->nin){ ins->out = o; ins->end = next; ins->status = FX2_INS_OK; return ins->status; }
        i = next;
    }
}

/* §4.3's opcodes, picked by opcode and input count */
static const struct { uint16_t op; uint8_t nin; const char *name; } k_fxlc[] = {
    { 0x000, 0, "nop" }, { 0x100, 1, "mov" }, { 0x101, 1, "neg" }, { 0x103, 1, "rcp" }, { 0x104, 1, "frc" },
    { 0x105, 1, "exp" }, { 0x106, 1, "log" }, { 0x107, 1, "rsq" }, { 0x108, 1, "sin" }, { 0x109, 1, "cos" },
    { 0x10a, 1, "asin" }, { 0x10b, 1, "acos" }, { 0x10c, 1, "atan" }, { 0x200, 2, "min" }, { 0x201, 2, "max" },
    { 0x202, 2, "lt" }, { 0x203, 2, "ge" }, { 0x204, 2, "add" }, { 0x205, 2, "mul" }, { 0x206, 2, "atan2" },
    { 0x208, 2, "div" }, { 0x300, 3, "cmp" }, { 0x301, 3, "movc" }, { 0x500, 2, "dot" },
    { 0x70e, 6, "dotswiz6" }, { 0x70e, 8, "dotswiz8" } };

const char *fx2_fxlc_name(uint32_t opcode, uint32_t nin){
    for (size_t k = 0; k < sizeof k_fxlc / sizeof k_fxlc[0]; k++)
        if (k_fxlc[k].op == opcode && k_fxlc[k].nin == nin) return k_fxlc[k].name;
    return NULL;
}

const char *fx2_fxlc_table(uint32_t code){
    static const char *const names[] = { "imm", "c", "v", "oc", "ob", "oi", "r" };
    return code >= 1 && code <= 7 ? names[code - 1] : NULL;
}
