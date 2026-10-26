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

/* §3.1: the operation of a state entry is a row of this table (fx2.h says
 * what the columns hold; docs/mw/fx-model.md §4.2) */
#define R(n, v)   { n, FX2_SC_RENDER, 1, 1, FX2_SV_DWORD, 1, v }
#define RF(n, v)  { n, FX2_SC_RENDER, 1, 1, FX2_T_FLOAT, 1, v }
#define T(n, v)   { n, FX2_SC_STAGE, 1, 1, FX2_SV_DWORD, 8, v }
#define TF(n, v)  { n, FX2_SC_STAGE, 1, 1, FX2_T_FLOAT, 8, v }
#define L(n, c, ty, k) { n, FX2_SC_LIGHT, 1, c, ty, 0, k }
#define C(n, s, ty, r, c, k) { n, s, r, c, ty, 0, k }
#define S(n, v)   { n, FX2_SC_SAMPLERSTATE, 1, 1, FX2_SV_DWORD, 261, v }
static const fx2_state_info k_states[FX2_NSTATES] = {
    R("ZENABLE", 7), R("FILLMODE", 8), R("SHADEMODE", 9), R("ZWRITEENABLE", 14),
    R("ALPHATESTENABLE", 15), R("LASTPIXEL", 16), R("SRCBLEND", 19), R("DESTBLEND", 20),
    R("CULLMODE", 22), R("ZFUNC", 23), R("ALPHAREF", 24), R("ALPHAFUNC", 25),
    R("DITHERENABLE", 26), R("ALPHABLENDENABLE", 27), R("FOGENABLE", 28), R("SPECULARENABLE", 29),
    R("FOGCOLOR", 34), R("FOGTABLEMODE", 35), RF("FOGSTART", 36), RF("FOGEND", 37),
    RF("FOGDENSITY", 38), R("RANGEFOGENABLE", 48), R("STENCILENABLE", 52), R("STENCILFAIL", 53),
    R("STENCILZFAIL", 54), R("STENCILPASS", 55), R("STENCILFUNC", 56), R("STENCILREF", 57),
    R("STENCILMASK", 58), R("STENCILWRITEMASK", 59), R("TEXTUREFACTOR", 60),
    R("WRAP0", 128), R("WRAP1", 129), R("WRAP2", 130), R("WRAP3", 131),                 /* 31..38 */
    R("WRAP4", 132), R("WRAP5", 133), R("WRAP6", 134), R("WRAP7", 135),
    R("WRAP8", 198), R("WRAP9", 199), R("WRAP10", 200), R("WRAP11", 201),               /* 39..46: WRAP8..15 right */
    R("WRAP12", 202), R("WRAP13", 203), R("WRAP14", 204), R("WRAP15", 205),             /* after WRAP7 (§3.2) */
    R("CLIPPING", 136), R("LIGHTING", 137), R("AMBIENT", 139), R("FOGVERTEXMODE", 140),
    R("COLORVERTEX", 141), R("LOCALVIEWER", 142), R("NORMALIZENORMALS", 143), R("DIFFUSEMATERIALSOURCE", 145),
    R("SPECULARMATERIALSOURCE", 146), R("AMBIENTMATERIALSOURCE", 147), R("EMISSIVEMATERIALSOURCE", 148),
    R("VERTEXBLEND", 151), R("CLIPPLANEENABLE", 152), RF("POINTSIZE", 154), RF("POINTSIZE_MIN", 155),
    RF("POINTSIZE_MAX", 166), R("POINTSPRITEENABLE", 156), R("POINTSCALEENABLE", 157),
    RF("POINTSCALE_A", 158), RF("POINTSCALE_B", 159), RF("POINTSCALE_C", 160),          /* 65..67 */
    R("MULTISAMPLEANTIALIAS", 161), R("MULTISAMPLEMASK", 162), R("PATCHEDGESTYLE", 163),
    R("DEBUGMONITORTOKEN", 165), R("INDEXEDVERTEXBLENDENABLE", 167), R("COLORWRITEENABLE", 168),   /* 73 */
    RF("TWEENFACTOR", 170), R("BLENDOP", 171), R("POSITIONDEGREE", 172), R("NORMALDEGREE", 173),
    R("SCISSORTESTENABLE", 174), RF("SLOPESCALEDEPTHBIAS", 175), R("ANTIALIASEDLINEENABLE", 176),
    RF("MINTESSELLATIONLEVEL", 178), RF("MAXTESSELLATIONLEVEL", 179), RF("ADAPTIVETESS_X", 180),
    RF("ADAPTIVETESS_Y", 181), RF("ADAPTIVETESS_Z", 182), RF("ADAPTIVETESS_W", 183),
    R("ENABLEADAPTIVETESSELLATION", 184), R("TWOSIDEDSTENCILMODE", 185),
    R("CCW_STENCILFAIL", 186),                  /* 89: d3dx9_26's table holds 53 (fx-model §4.3) */
    R("CCW_STENCILZFAIL", 187), R("CCW_STENCILPASS", 188), R("CCW_STENCILFUNC", 189),
    R("COLORWRITEENABLE1", 190), R("COLORWRITEENABLE2", 191), R("COLORWRITEENABLE3", 192),
    R("BLENDFACTOR", 193), R("SRGBWRITEENABLE", 194), RF("DEPTHBIAS", 195),
    R("SEPARATEALPHABLENDENABLE", 206), R("SRCBLENDALPHA", 207), R("DESTBLENDALPHA", 208),
    R("BLENDOPALPHA", 209),                     /* 102: d3dx9_26's table holds 171 (fx-model §4.3) */
    T("COLOROP", 1), T("COLORARG0", 26), T("COLORARG1", 2), T("COLORARG2", 3),          /* 103.. */
    T("ALPHAOP", 4), T("ALPHAARG0", 27), T("ALPHAARG1", 5), T("ALPHAARG2", 6),
    T("RESULTARG", 28), TF("BUMPENVMAT00", 7), TF("BUMPENVMAT01", 8), TF("BUMPENVMAT10", 9),
    TF("BUMPENVMAT11", 10), T("TEXCOORDINDEX", 11), TF("BUMPENVLSCALE", 22), TF("BUMPENVLOFFSET", 23),
    T("TEXTURETRANSFORMFLAGS", 24), T("CONSTANT", 32),
    { "PATCHSEGMENTS", FX2_SC_NPATCH, 1, 1, FX2_T_FLOAT, 1, 0 },                        /* 121 */
    { "FVF", FX2_SC_FVF, 1, 1, FX2_SV_DWORD, 1, 0 },
    { "PROJECTIONTRANSFORM", FX2_SC_TRANSFORM, 4, 4, FX2_T_FLOAT, 1, 3 },               /* D3DTS_PROJECTION */
    { "VIEWTRANSFORM", FX2_SC_TRANSFORM, 4, 4, FX2_T_FLOAT, 1, 2 },                     /* D3DTS_VIEW */
    { "WORLDTRANSFORM", FX2_SC_TRANSFORM, 4, 4, FX2_T_FLOAT, 256, 256 },                /* D3DTS_WORLDMATRIX(i) */
    { "TEXTURETRANSFORM", FX2_SC_TRANSFORM, 4, 4, FX2_T_FLOAT, 8, 16 },                 /* D3DTS_TEXTURE0 + i */
    { "MATERIALDIFFUSE", FX2_SC_MATERIAL, 1, 4, FX2_T_FLOAT, 1, 0 },                    /* 127 */
    { "MATERIALAMBIENT", FX2_SC_MATERIAL, 1, 4, FX2_T_FLOAT, 1, 1 },
    { "MATERIALSPECULAR", FX2_SC_MATERIAL, 1, 4, FX2_T_FLOAT, 1, 2 },
    { "MATERIALEMISSIVE", FX2_SC_MATERIAL, 1, 4, FX2_T_FLOAT, 1, 3 },
    { "MATERIALPOWER", FX2_SC_MATERIAL, 1, 1, FX2_T_FLOAT, 1, 4 },
    L("LIGHTTYPE", 1, FX2_SV_DWORD, 0), L("LIGHTDIFFUSE", 4, FX2_T_FLOAT, 1),            /* 132.. */
    L("LIGHTSPECULAR", 4, FX2_T_FLOAT, 2), L("LIGHTAMBIENT", 4, FX2_T_FLOAT, 3),
    L("LIGHTPOSITION", 3, FX2_T_FLOAT, 4), L("LIGHTDIRECTION", 3, FX2_T_FLOAT, 5),
    L("LIGHTRANGE", 1, FX2_T_FLOAT, 6), L("LIGHTFALLOFF", 1, FX2_T_FLOAT, 7),
    L("LIGHTATTENUATION0", 1, FX2_T_FLOAT, 8), L("LIGHTATTENUATION1", 1, FX2_T_FLOAT, 9),
    L("LIGHTATTENUATION2", 1, FX2_T_FLOAT, 10), L("LIGHTTHETA", 1, FX2_T_FLOAT, 11),
    L("LIGHTPHI", 1, FX2_T_FLOAT, 12), L("LIGHTENABLE", 1, FX2_SV_DWORD, 13),
    { "VERTEXSHADER", FX2_SC_VSHADER, 1, 1, FX2_T_VERTEXSHADER, 1, 0 },                 /* 146 */
    { "PIXELSHADER", FX2_SC_PSHADER, 1, 1, FX2_T_PIXELSHADER, 1, 0 },
    C("VERTEXSHADERCONSTANTF", FX2_SC_VSCONST, FX2_T_FLOAT, 0, 0, 0),                    /* 148 */
    C("VERTEXSHADERCONSTANTB", FX2_SC_VSCONST, FX2_T_BOOL, 0, 0, 0),
    C("VERTEXSHADERCONSTANTI", FX2_SC_VSCONST, FX2_T_INT, 0, 0, 0),
    C("VERTEXSHADERCONSTANT", FX2_SC_VSCONST, FX2_T_FLOAT, 0, 0, 0),
    C("VERTEXSHADERCONSTANT1", FX2_SC_VSCONST, FX2_T_FLOAT, 1, 4, 1),
    C("VERTEXSHADERCONSTANT2", FX2_SC_VSCONST, FX2_T_FLOAT, 2, 4, 2),
    C("VERTEXSHADERCONSTANT3", FX2_SC_VSCONST, FX2_T_FLOAT, 3, 4, 3),
    C("VERTEXSHADERCONSTANT4", FX2_SC_VSCONST, FX2_T_FLOAT, 4, 4, 4),
    C("PIXELSHADERCONSTANTF", FX2_SC_PSCONST, FX2_T_FLOAT, 0, 0, 0),                     /* 156 */
    C("PIXELSHADERCONSTANTB", FX2_SC_PSCONST, FX2_T_BOOL, 0, 0, 0),
    C("PIXELSHADERCONSTANTI", FX2_SC_PSCONST, FX2_T_INT, 0, 0, 0),
    C("PIXELSHADERCONSTANT", FX2_SC_PSCONST, FX2_T_FLOAT, 0, 0, 0),
    C("PIXELSHADERCONSTANT1", FX2_SC_PSCONST, FX2_T_FLOAT, 1, 4, 1),
    C("PIXELSHADERCONSTANT2", FX2_SC_PSCONST, FX2_T_FLOAT, 2, 4, 2),
    C("PIXELSHADERCONSTANT3", FX2_SC_PSCONST, FX2_T_FLOAT, 3, 4, 3),
    C("PIXELSHADERCONSTANT4", FX2_SC_PSCONST, FX2_T_FLOAT, 4, 4, 4),
    { "TEXTURE", FX2_SC_TEXTURE, 1, 1, FX2_T_TEXTURE, 261, 0 },                         /* 164 */
    S("ADDRESSU", 1), S("ADDRESSV", 2), S("ADDRESSW", 3), S("BORDERCOLOR", 4),
    S("MAGFILTER", 5), S("MINFILTER", 6), S("MIPFILTER", 7),
    { "MIPMAPLODBIAS", FX2_SC_SAMPLERSTATE, 1, 1, FX2_T_FLOAT, 261, 8 },
    S("MAXMIPLEVEL", 9), S("MAXANISOTROPY", 10), S("SRGBTEXTURE", 11), S("ELEMENTINDEX", 12),
    S("DMAPOFFSET", 13),
    { "SAMPLER", FX2_SC_SAMPLER, 1, 1, FX2_T_SAMPLER, 261, 0 },                         /* 178 */
    { "STATEBLOCK", FX2_SC_STATEBLOCK, 1, 1, FX2_SV_STATEBLOCK, 1, 0 },                 /* 179..181: d3dx9_26 only */
    { "$PRESHADER_VS", FX2_SC_PRESHADER_VS, 1, 1, FX2_T_VERTEXSHADER, 1, 0 },
    { "$PRESHADER_PS", FX2_SC_PRESHADER_PS, 1, 1, FX2_T_PIXELSHADER, 1, 0 } };
#undef R
#undef RF
#undef T
#undef TF
#undef L
#undef C
#undef S
static const char *const k_sclasses[FX2_NSCLASSES] = {
    "RENDER", "STAGE", "NPATCH", "FVF", "TRANSFORM", "MATERIAL", "LIGHT", "VSHADER", "PSHADER", "VSCONST",
    "PSCONST", "TEXTURE", "SAMPLERSTATE", "SAMPLER", "STATEBLOCK", "PRESHADER_VS", "PRESHADER_PS" };

const char *fx2_type_name(uint32_t type){ return type < FX2_NTYPES ? k_types[type] : NULL; }
const char *fx2_class_name(uint32_t cls){ return cls < FX2_NCLASSES ? k_classes[cls] : NULL; }
const char *fx2_state_name(uint32_t op){ return op < FX2_NSTATES ? k_states[op].name : NULL; }
const fx2_state_info *fx2_state_get(uint32_t op){ return op < FX2_NSTATES ? &k_states[op] : NULL; }
const char *fx2_sclass_name(uint32_t cls){ return cls < FX2_NSCLASSES ? k_sclasses[cls] : NULL; }

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
