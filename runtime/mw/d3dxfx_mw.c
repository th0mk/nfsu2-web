/* Most Wanted's D3DX effects, native (TRACKER M5-05, M5-06; PLAN D-09): D3DXCreateEffectPool,
 * D3DXCreateEffectFromResourceA, and the ID3DXEffect and ID3DXEffectPool objects they make, so speed.exe
 * runs without d3dx9_26.dll (a dev oracle that never ships, GAME.md 5.1).  The objects are com.c's
 * (com_new_d3dx): their vtables call d3dxfx_call through game.h's d3dx_call, and their last Release
 * d3dxfx_free.
 *
 * What an effect is: the exe's resource (RT_RCDATA by name; speed.exe passes hModule NULL), parsed in
 * place (fx2.c), its model (fxmodel.c: nodes, values, versions, the pool's shared parameters, the CTAB
 * matches; docs/mw/fx-model.md) and its preshaders (fxpres.c; docs/mw/fx-preshader.md).  This file adds
 * what D3DX does with them on the device (docs/mw/fx-hle.md): the shaders it creates with the effect,
 * the handles the game holds (fx-model 2.8), Begin's saved state, the states and constants BeginPass
 * applies, what CommitChanges applies again, End's restore, and the lost-device rules.  Every device call
 * goes through the device's own vtable (call_guest), as the DLL's do, so the device-call recorder
 * (runtime/comrec.c, NFSU2_DXREC) sees the HLE's calls as it sees the oracle's.
 *
 * Handles: an effect object holds a block of 4 x (nodes + 1) bytes after its native index; node id's
 * handle is block + 4 id, never 0 (fx-model 2.8).  A handle outside the block is a name (a guest
 * string), as D3DX takes one.  The game never inspects a handle's value.
 *
 * Off by default in an oracle build (the oracle keeps the two imports); NFSMW_D3DX_FX=hle there binds
 * them here.  A build without the oracle always binds them. */
#include "cpu.h"
#include "game.h"
#include "comrec.h"
#include "d3dxfx_mw.h"
#include "fx2.h"
#include "fxmodel.h"
#include "fxpres.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);   /* com.c */
uint32_t com_new_d3dx(int pool, uint32_t extra);                             /* com.c */
uint32_t com_obj_addref(CPU *c, uint32_t o);
uint32_t com_obj_release(CPU *c, uint32_t o);
uint32_t com_scratch(uint32_t n);
uint32_t res_find(uint32_t lpName, uint32_t lpType, int wide);               /* res.c */
uint32_t res_data(uint32_t hrsrc);
uint32_t res_size(uint32_t hrsrc);
int dispatch_patch_take(uint32_t a, void (*f)(CPU *));

#define HR_OK          0x00000000u
#define HR_INVALIDCALL 0x8876086Cu    /* D3DERR_INVALIDCALL */
#define HR_INVALIDDATA 0x88760B59u    /* D3DXERR_INVALIDDATA */
#define HR_NOTIMPL     0x80004001u
#define HR_FAIL        0x80004005u
#define HR_OOM         0x8007000Eu
#define DONOTSAVESTATE 1u             /* D3DXFX_DONOTSAVESTATE */
#define VTEX0          257u           /* D3DVERTEXTEXTURESAMPLER0 */

/* IDirect3DDevice9's slots (d3d9_tables.h M_DEVICE) */
enum { D_ADDREF = 1, D_RELEASE = 2, D_SETTRANSFORM = 44, D_SETMATERIAL = 49, D_SETLIGHT = 51, D_LIGHTENABLE = 53,
       D_SETRS = 57, D_BEGINSB = 60, D_ENDSB = 61, D_SETTEXTURE = 65, D_SETTSS = 67, D_SETSS = 69, D_NPATCH = 79,
       D_SETFVF = 89, D_CREATEVS = 91, D_SETVS = 92, D_SETVSCF = 94, D_CREATEPS = 106, D_SETPS = 107,
       D_SETPSCF = 109 };
enum { SB_RELEASE = 2, SB_CAPTURE = 4, SB_APPLY = 5 };         /* IDirect3DStateBlock9 */

/* ---- the objects ---- */
typedef struct { fxm_pool *p; uint32_t obj; int effects; } Pool;
typedef struct {                 /* a shader program's registers as D3DX last uploaded them */
    uint32_t n;                  /* registers */
    uint32_t *r;                 /* 4 dwords each */
} Regs;
typedef struct {
    uint32_t obj, dev, pool_obj;
    Pool *pool;
    int id;                      /* the game's list row, -1 until known */
    char name[48];
    fx2_effect fx;
    fxm_effect *m;
    fxp_effect *pres;
    uint32_t n;                  /* nodes */
    uint32_t *res_shader;        /* by resource index: the shader object D3DX made for it */
    Regs *regs;                  /* by program index (fxm_progs) */
    const fxm_prog *progs; uint32_t nprogs;
    uint32_t tech;               /* the current technique's node */
    int started, pass;           /* Begin seen; the active pass's index, -1 none */
    uint32_t flags;              /* Begin's */
    uint32_t *saved;             /* by technique index: the state block Begin captures */
    uint64_t *stamp;             /* by node id (passes): the clock when the pass was applied */
    uint32_t light_upd, mat_upd;
    uint32_t light[8][26], mat[17];
    uint32_t scratch;            /* guest bytes for the data the device calls point at */
} Fx;

#define MAXFX 512
static Fx *g_fx[MAXFX];
#define MAXPOOL 64
static Pool *g_pool[MAXPOOL];

static int g_log_once[64];
static void say_once(int k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say_once(int k, const char *fmt, ...){
    if (k < 0 || k >= 64 || g_log_once[k]++) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

static Fx *fx_of(uint32_t obj){
    if (!obj || !mem_is_committed(obj + 7u)) return 0;
    uint32_t i = MR32(obj + 4u);
    return i >= 1 && i <= MAXFX && g_fx[i - 1] && g_fx[i - 1]->obj == obj ? g_fx[i - 1] : 0;
}
static Pool *pool_of(uint32_t obj){
    if (!obj || !mem_is_committed(obj + 7u)) return 0;
    uint32_t i = MR32(obj + 4u);
    return i >= 1 && i <= MAXPOOL && g_pool[i - 1] && g_pool[i - 1]->obj == obj ? g_pool[i - 1] : 0;
}

/* ---- the game's list (GAME.md 5.4): wrapper i at [0x93DE78 + 4i], its effect at wrapper +0x48 ---- */
int fx_wrapper_id(uint32_t effect){
    for (uint32_t i = 0; i < 31u; i++){
        uint32_t w = MR32(0x0093de78u + 4u * i);
        if (w && mem_is_committed(w + 0x4Bu) && MR32(w + 0x48u) == effect) return (int)i;
    }
    return -1;
}

/* ---- the recorder's context lines ---- */
static const char *const k_slot[78] = {
    "QueryInterface","AddRef","Release","GetDesc","GetParameterDesc","GetTechniqueDesc","GetPassDesc",
    "GetFunctionDesc","GetParameter","GetParameterByName","GetParameterBySemantic","GetParameterElement",
    "GetTechnique","GetTechniqueByName","GetPass","GetPassByName","GetFunction","GetFunctionByName",
    "GetAnnotation","GetAnnotationByName","SetValue","GetValue","SetBool","GetBool","SetBoolArray",
    "GetBoolArray","SetInt","GetInt","SetIntArray","GetIntArray","SetFloat","GetFloat","SetFloatArray",
    "GetFloatArray","SetVector","GetVector","SetVectorArray","GetVectorArray","SetMatrix","GetMatrix",
    "SetMatrixArray","GetMatrixArray","SetMatrixPointerArray","GetMatrixPointerArray","SetMatrixTranspose",
    "GetMatrixTranspose","SetMatrixTransposeArray","GetMatrixTransposeArray","SetMatrixTransposePointerArray",
    "GetMatrixTransposePointerArray","SetString","GetString","SetTexture","GetTexture","GetPixelShader",
    "GetVertexShader","SetArrayRange","GetPool","SetTechnique","GetCurrentTechnique","ValidateTechnique",
    "FindNextValidTechnique","IsParameterUsed","Begin","BeginPass","CommitChanges","EndPass","End",
    "GetDevice","OnLostDevice","OnResetDevice","SetStateManager","GetStateManager","BeginParameterBlock",
    "EndParameterBlock","ApplyParameterBlock","DeleteParameterBlock","CloneEffect" };
const char *fxmark_name(int slot){ return slot >= 0 && slot < 78 ? k_slot[slot] : "?"; }
void fxmark(int id, const char *method, const char *arg, uint32_t hr){
    if (!g_comrec_on) return;
    char ids[8]; if (id < 0) snprintf(ids, sizeof ids, "-"); else snprintf(ids, sizeof ids, "%d", id);
    if (hr) comrec_marker("E%s %s%s%s =%x", ids, method, arg ? " " : "", arg ? arg : "", hr);
    else comrec_marker("E%s %s%s%s", ids, method, arg ? " " : "", arg ? arg : "");
}

/* ---- the device, through its vtable ---- */
static uint32_t dcall(CPU *c, uint32_t obj, int slot, int n, ...){
    uint32_t a[10]; a[0] = obj;
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 9; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return call_guest(c, MR32(MR32(obj) + 4u * (uint32_t)slot), n + 1, a);
}
#define DEV(c, f, slot, n, ...) dcall((c), (f)->dev, (slot), (n), __VA_ARGS__)

/* the x87 precision the caller runs at, for the preshaders and SetVector's colour form (fx-preshader 3.4) */
static int caller_pc(CPU *c){
    int pc = fxp_pc_from_cw(c->fcw);
    if (!pc){ say_once(0, "[d3dxfx] x87 control word 0x%04x is not one the preshaders model: taken as 24 bits\n", c->fcw); pc = 24; }
    return pc;
}

/* ---- handles (fx-model 2.8) ---- */
static uint32_t h_of(const Fx *f, uint32_t id){ return id ? f->obj + 8u + 4u * id : 0u; }
static uint32_t h_node(const Fx *f, uint32_t h){
    uint32_t b = f->obj + 8u;
    return h > b && h <= b + 4u * f->n && !((h - b) & 3u) ? (h - b) / 4u : 0u;
}
/* a guest string, at most cap - 1 bytes: its length, -1 when it cannot be read */
static int gstring(uint32_t p, char *buf, int cap){
    if (!p) return -1;
    int n = 0;
    for (; n < cap - 1; n++){
        if (!mem_is_committed(p + (uint32_t)n)) return -1;
        if (!(buf[n] = (char)MR8(p + (uint32_t)n))) return n;
    }
    buf[n] = 0;
    return n;
}
static int is_kind(const Fx *f, uint32_t id, int k1, int k2, int k3, int k4){
    const fxm_node *d = fxm_node_get(f->m, id);
    return d && ((int)d->kind == k1 || (int)d->kind == k2 || (int)d->kind == k3 || (int)d->kind == k4);
}
/* a parameter handle (or name): its node, 0 for none; annotations too where allow_annot */
static uint32_t h_param(const Fx *f, uint32_t h, int allow_annot){
    if (!h) return 0;
    uint32_t id = h_node(f, h);
    if (id) return is_kind(f, id, FXM_K_PARAM, FXM_K_ELEMENT, FXM_K_MEMBER, allow_annot ? FXM_K_ANNOT : FXM_K_PARAM) ? id : 0u;
    char s[256]; int n = gstring(h, s, sizeof s);
    return n > 0 ? fxm_by_name(f->m, 0, (const uint8_t *)s, (uint32_t)n) : 0u;
}
static uint32_t h_tech(const Fx *f, uint32_t h){
    if (!h) return 0;
    uint32_t id = h_node(f, h);
    if (id) return is_kind(f, id, FXM_K_TECH, FXM_K_TECH, FXM_K_TECH, FXM_K_TECH) ? id : 0u;
    char s[256]; int n = gstring(h, s, sizeof s);
    return n >= 0 ? fxm_tech_by_name(f->m, (const uint8_t *)s, (uint32_t)n) : 0u;
}
static uint32_t h_pass(const Fx *f, uint32_t h){
    uint32_t id = h_node(f, h);
    return id && is_kind(f, id, FXM_K_PASS, FXM_K_PASS, FXM_K_PASS, FXM_K_PASS) ? id : 0u;
}
static uint32_t gaddr(const void *p){ return p ? (uint32_t)((const uint8_t *)p - (const uint8_t *)GMEM_) : 0u; }
static uint32_t gname(fx2_str s){ return s.p ? gaddr(s.p) : 0u; }

static const fx2_pass *pass_of(const Fx *f, uint32_t pass){
    const fxm_node *p = fxm_node_get(f->m, pass), *t = p ? fxm_node_get(f->m, p->parent) : 0;
    if (!p || !t || t->index >= f->fx.ntechs || p->index >= f->fx.techs[t->index].npasses) return 0;
    return &f->fx.techs[t->index].passes[p->index];
}
static uint32_t prog_index(const Fx *f, const fxm_prog *pr){
    return pr && pr >= f->progs && pr < f->progs + f->nprogs ? (uint32_t)(pr - f->progs) : 0xFFFFFFFFu;
}

/* ---- applying a pass (docs/mw/fx-hle.md 3) ---- */
static void apply_state(CPU *c, Fx *f, const fx2_state *s, uint32_t unit, int all, uint64_t stamp);

/* a state's value: n dwords in v (at most 32); 0, or 1 when it has none to set */
static int state_value(CPU *c, Fx *f, const fx2_state *s, const fxm_source *src, uint32_t *v, uint32_t *n){
    *n = 0;
    switch (src->src){
    case FXM_S_LITERAL: {
        const fx2_type *t = s->t;
        uint32_t k = t ? (t->rows ? t->rows : 1u) * (t->cols ? t->cols : 1u) : 1u;
        if (s->v.kind == FX2_V_OBJECT) k = 1;
        if (k > 32) k = 32;
        if (!s->v.data || s->v.size < 4u * k) k = s->v.data ? s->v.size / 4u : 0u;
        for (uint32_t i = 0; i < k; i++) v[i] = fx2_rd32(s->v.data + 4u * i);
        *n = k;
        return 0; }
    case FXM_S_PRESHADER: {
        int e = fxp_state_value(f->pres, s, caller_pc(c), v, 32, n);
        if (e){ say_once(1, "[d3dxfx] %s: a preshader stopped (%s); its state is left as it was\n", f->name, fxp_err_name(e)); return 1; }
        return 0; }
    case FXM_S_PARAM: {
        const uint32_t *d = src->id ? fxm_data(f->m, src->id) : 0;
        const fxm_node *nd = fxm_node_get(f->m, src->id);
        if (!d || !nd){ say_once(2, "[d3dxfx] %s: a state names a parameter that has no value\n", f->name); return 1; }
        uint32_t k = nd->bytes / 4u; if (k > 32) k = 32;
        for (uint32_t i = 0; i < k; i++) v[i] = d[i];
        *n = k;
        return 0; }
    case FXM_S_SHADER: {
        uint32_t r = src->res ? (uint32_t)(src->res - f->fx.res) : 0xFFFFFFFFu;
        v[0] = r < f->fx.nres ? f->res_shader[r] : 0u;
        *n = 1;
        return 0; }
    default:
        say_once(3, "[d3dxfx] %s: a state of a kind not modelled (source %u) is left as it was\n", f->name, src->src);
        return 1;
    }
}
static int state_dirty(const Fx *f, const fxm_source *src, uint64_t stamp){
    if (src->src == FXM_S_PARAM) return src->id && fxm_version(f->m, src->id) > stamp;
    if (src->src == FXM_S_PRESHADER && src->prog){
        for (uint32_t k = 0; k < src->prog->nconsts && k < src->prog->nread; k++){
            const fxm_const *q = &src->prog->consts[k];
            if (!q->why && q->id && fxm_version(f->m, q->id) > stamp) return 1;
        }
    }
    return 0;
}

static uint32_t f2bits(float x){ uint32_t w; memcpy(&w, &x, 4); return w; }
static float bits2f(uint32_t w){ float x; memcpy(&x, &w, 4); return x; }
static uint32_t to_set(uint32_t to, uint32_t from, uint32_t w){
    if (to == from) return w;
    if (to == FX2_T_FLOAT){
        if (from == FX2_T_INT) return f2bits((float)(int32_t)w);
        if (from == FX2_T_BOOL) return f2bits(w ? 1.0f : 0.0f);
    }
    if (to == FX2_T_INT && from == FX2_T_FLOAT){ float x = bits2f(w); return (uint32_t)(int32_t)x; }
    if (to == FX2_T_BOOL) return from == FX2_T_FLOAT ? ((w & 0x7FFFFFFFu) != 0) : (w != 0);
    return w;
}

/* one shader program's constants (fx-model 3.3), the numeric ones that are dirty (all of them with all),
 * then its samplers' states (fx-model 3.4) */
static void shader_consts(CPU *c, Fx *f, const fxm_prog *pr, int vs, int all, uint64_t stamp){
    uint32_t pi = prog_index(f, pr);
    if (pi == 0xFFFFFFFFu) return;
    Regs *R = &f->regs[pi];
    for (uint32_t k = 0; k < pr->nconsts && k < pr->nread; k++){
        const fxm_const *q = &pr->consts[k];
        if (q->why || q->layout == FXM_L_SAMPLER || !q->id) continue;
        if (!all && fxm_version(f->m, q->id) <= stamp) continue;
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t l = 0; l < q->nleaves; l++){
            const fxm_leaf *L = &q->leaves[l];
            if (!L->count) continue;
            const uint32_t *d = fxm_data(f->m, L->id);
            const fxm_node *nd = fxm_node_get(f->m, L->id);
            if (!d || !nd) continue;
            if (L->reg + L->count > R->n){
                uint32_t nn = L->reg + L->count;
                uint32_t *r = realloc(R->r, 16u * nn);
                if (!r) continue;
                memset(r + 4u * R->n, 0, 16u * (nn - R->n));
                R->r = r; R->n = nn;
            }
            uint32_t rows = nd->rows ? nd->rows : 1u, cols = nd->cols ? nd->cols : 1u;
            for (uint32_t r = 0; r < L->count; r++){
                uint32_t *g = R->r + 4u * (L->reg + r);
                switch (L->layout){
                case FXM_L_SCALAR:  g[0] = to_set(q->to, q->from, d[r]); break;
                case FXM_L_VECTOR:  for (uint32_t i = 0; i < cols && i < 4u; i++) g[i] = to_set(q->to, q->from, d[i]); break;
                case FXM_L_ROWS:    for (uint32_t i = 0; i < cols && i < 4u; i++) g[i] = to_set(q->to, q->from, d[r * cols + i]); break;
                case FXM_L_COLUMNS: for (uint32_t i = 0; i < rows && i < 4u; i++) g[i] = to_set(q->to, q->from, d[i * cols + r]); break;
                default: break;
                }
            }
            if (L->reg < lo) lo = L->reg;
            if (L->reg + L->count > hi) hi = L->reg + L->count;
        }
        if (lo < hi){
            uint32_t bytes = 16u * (hi - lo);
            if (bytes > 4096u) bytes = 4096u, hi = lo + 256u;
            memcpy(GMEM_ + f->scratch, R->r + 4u * lo, bytes);
            DEV(c, f, vs ? D_SETVSCF : D_SETPSCF, 3, lo, f->scratch, hi - lo);
        }
    }
    for (uint32_t k = 0; k < pr->nconsts && k < pr->nread; k++){
        const fxm_const *q = &pr->consts[k];
        if (q->why || q->layout != FXM_L_SAMPLER) continue;
        for (uint32_t j = 0; j < q->used && j < q->nleaves; j++){
            const fxm_node *nd = fxm_node_get(f->m, q->leaves[j].id);
            const fx2_block *b = nd ? nd->block : 0;
            if (!b) continue;
            for (uint32_t i = 0; i < b->nstates; i++)
                apply_state(c, f, &b->states[i], q->stage + j + (vs ? VTEX0 : 0u), all, stamp);
        }
    }
}

static void apply_state(CPU *c, Fx *f, const fx2_state *s, uint32_t unit, int all, uint64_t stamp){
    const fx2_state_info *in = fx2_state_get(s->op);
    if (!in) return;
    fxm_source src = fxm_state_source(f->m, s);
    int dirty = all || state_dirty(f, &src, stamp);
    int shader = in->cls == FX2_SC_VSHADER || in->cls == FX2_SC_PSHADER;
    if (!dirty && !shader) return;
    uint32_t v[32]; uint32_t n = 0;
    if (state_value(c, f, s, &src, v, &n) || !n) return;
    uint32_t idx = unit != 0xFFFFFFFFu ? unit : s->index;
    switch (in->cls){
    case FX2_SC_RENDER:       DEV(c, f, D_SETRS, 2, in->d3d, v[0]); break;
    case FX2_SC_STAGE:        DEV(c, f, D_SETTSS, 3, s->index, in->d3d, v[0]); break;
    case FX2_SC_SAMPLERSTATE: DEV(c, f, D_SETSS, 3, idx, in->d3d, v[0]); break;
    case FX2_SC_TEXTURE:      DEV(c, f, D_SETTEXTURE, 2, idx, v[0]); break;
    case FX2_SC_FVF:          DEV(c, f, D_SETFVF, 1, v[0]); break;
    case FX2_SC_NPATCH:       DEV(c, f, D_NPATCH, 1, v[0]); break;
    case FX2_SC_VSHADER: case FX2_SC_PSHADER: {
        int vs = in->cls == FX2_SC_VSHADER;
        if (dirty) DEV(c, f, vs ? D_SETVS : D_SETPS, 1, v[0]);
        if (v[0] && src.src == FXM_S_SHADER && src.prog) shader_consts(c, f, src.prog, vs, dirty, stamp);
        break; }
    case FX2_SC_TRANSFORM: {
        uint32_t m[16] = {0};
        for (uint32_t i = 0; i < 16 && i < n; i++) m[i] = v[i];
        memcpy(GMEM_ + f->scratch, m, 64);
        DEV(c, f, D_SETTRANSFORM, 2, in->d3d + s->index, f->scratch);
        break; }
    case FX2_SC_LIGHT:
        if (s->index < 8){
            if (in->d3d == 13){ DEV(c, f, D_LIGHTENABLE, 2, s->index, v[0]); break; }
            static const uint8_t off[13] = { 0, 1, 5, 9, 13, 16, 19, 22, 23, 24, 25, 26, 27 };   /* D3DLIGHT9 dwords */
            static const uint8_t cnt[13] = { 1, 4, 4, 4, 3, 3, 1, 1, 1, 1, 1, 1, 1 };
            if (in->d3d < 13 && off[in->d3d] < 26)
                for (uint32_t i = 0; i < cnt[in->d3d] && i < n && off[in->d3d] + i < 26; i++) f->light[s->index][off[in->d3d] + i] = v[i];
            f->light_upd |= 1u << s->index;
        }
        break;
    case FX2_SC_MATERIAL: {
        static const uint8_t off[5] = { 0, 4, 8, 12, 16 }, cnt[5] = { 4, 4, 4, 4, 1 };   /* D3DMATERIAL9 dwords */
        if (in->d3d < 5) for (uint32_t i = 0; i < cnt[in->d3d] && i < n; i++) f->mat[off[in->d3d] + i] = v[i];
        f->mat_upd = 1;
        break; }
    default:
        say_once(4, "[d3dxfx] %s: state %s (class %s) is not applied\n", f->name, in->name, fx2_sclass_name(in->cls));
        break;
    }
}

static void apply_pass(CPU *c, Fx *f, uint32_t pass, int all){
    const fx2_pass *P = pass_of(f, pass);
    if (!P) return;
    uint64_t stamp = f->stamp[pass];
    for (uint32_t i = 0; i < P->nstates; i++) apply_state(c, f, &P->states[i], 0xFFFFFFFFu, all, stamp);
    for (uint32_t i = 0; i < 8; i++)
        if (f->light_upd & (1u << i)){
            memcpy(GMEM_ + f->scratch, f->light[i], 104);
            DEV(c, f, D_SETLIGHT, 2, i, f->scratch);
        }
    f->light_upd = 0;
    if (f->mat_upd){ memcpy(GMEM_ + f->scratch, f->mat, 68); DEV(c, f, D_SETMATERIAL, 1, f->scratch); }
    f->mat_upd = 0;
    f->stamp[pass] = fxm_clock(f->m);
}

/* ---- creation ---- */
static void drop_ref(void *ctx, uint32_t ptr){ com_obj_release((CPU *)ctx, ptr); }

static void fx_destroy(CPU *c, Fx *f){
    if (!f) return;
    for (uint32_t t = 0; f->saved && t < f->fx.ntechs; t++)
        if (f->saved[t]) dcall(c, f->saved[t], SB_RELEASE, 0);
    for (uint32_t r = 0; f->res_shader && r < f->fx.nres; r++)
        if (f->res_shader[r]) dcall(c, f->res_shader[r], D_RELEASE, 0);
    if (f->m) fxm_effect_free(f->m, drop_ref, c);
    if (f->pres) fxp_effect_free(f->pres);
    fx2_free(&f->fx);
    for (uint32_t i = 0; f->regs && i < f->nprogs; i++) free(f->regs[i].r);
    free(f->regs); free(f->res_shader); free(f->saved); free(f->stamp);
    if (f->pool){ f->pool->effects--; if (f->pool_obj) com_obj_release(c, f->pool_obj); }
    if (f->dev) dcall(c, f->dev, D_RELEASE, 0);
    free(f);
}

/* D3DXCreateEffectPool(ppPool) */
static void hle_create_pool(CPU *c){
    uint32_t pp = MR32(c->esp + 4u), r = HR_OOM;
    fxmark(-1, "D3DXCreateEffectPool", 0, 0);
    int slot = -1;
    for (int i = 0; i < MAXPOOL; i++) if (!g_pool[i]){ slot = i; break; }
    Pool *P = slot >= 0 ? calloc(1, sizeof *P) : 0;
    if (P && (P->p = fxm_pool_new()) && (P->obj = com_new_d3dx(1, 4u))){
        MR32(P->obj + 4u) = (uint32_t)slot + 1u;
        g_pool[slot] = P;
        if (pp) MR32(pp) = P->obj;
        r = HR_OK;
    } else {
        if (P) fxm_pool_free(P->p);
        free(P);
        if (!pp) r = HR_INVALIDCALL;
    }
    POP32(c->eip); c->esp += 4u; c->eax = r;
}

/* D3DXCreateEffectFromResourceA(pDevice, hSrcModule, pSrcResource, pDefines, pInclude, Flags, pPool,
 * ppEffect, ppCompilationErrors) */
static void hle_create_effect(CPU *c){
    uint32_t a[9];
    for (int i = 0; i < 9; i++) a[i] = MR32(c->esp + 4u + 4u * (uint32_t)i);
    char name[48] = "?";
    if (gstring(a[2], name, sizeof name) < 0) snprintf(name, sizeof name, "#%u", a[2]);
    fxmark(-1, "D3DXCreateEffectFromResourceA", name, 0);
    uint32_t r = HR_OK;
    Fx *f = 0;
    if (a[8]) MR32(a[8]) = 0;
    if (!a[0] || !a[2] || !a[7]){ r = HR_INVALIDCALL; goto out; }
    if (a[1]) say_once(5, "[d3dxfx] D3DXCreateEffectFromResourceA with module 0x%08x: the exe's resources are searched\n", a[1]);
    uint32_t hr = res_find(a[2], 10u, 0), va = res_data(hr), size = res_size(hr);
    if (!hr || !va || size < 8u){ fprintf(stderr, "[d3dxfx] %s: no such RT_RCDATA resource\n", name); r = HR_INVALIDDATA; goto out; }
    int slot = -1;
    for (int i = 0; i < MAXFX; i++) if (!g_fx[i]){ slot = i; break; }
    if (slot < 0 || !(f = calloc(1, sizeof *f))){ r = HR_OOM; goto out; }
    snprintf(f->name, sizeof f->name, "%s", name);
    f->id = -1; f->pass = -1;
    char err[256];
    if (fx2_parse(&f->fx, GMEM_ + va, size, err, sizeof err)){
        fprintf(stderr, "[d3dxfx] %s: %s\n", name, err); free(f); f = 0; r = HR_INVALIDDATA; goto out; }
    if (f->fx.tag != FX2_TAG){
        fprintf(stderr, "[d3dxfx] %s: not an fx_2_0 effect (0x%08x)\n", name, f->fx.tag); fx2_free(&f->fx); free(f); f = 0;
        r = HR_INVALIDDATA; goto out; }
    Pool *P = a[6] ? pool_of(a[6]) : 0;
    if (a[6] && !P) say_once(6, "[d3dxfx] %s: a pool that is not one of ours (0x%08x): none taken\n", name, a[6]);
    if (fxm_effect_new(&f->m, &f->fx, P ? P->p : 0, err, sizeof err)){
        fprintf(stderr, "[d3dxfx] %s: %s\n", name, err); fx2_free(&f->fx); free(f); f = 0; r = HR_OOM; goto out; }
    int pe = fxp_effect_new(&f->pres, f->m);
    if (pe){
        fprintf(stderr, "[d3dxfx] %s: a preshader D3DX refuses (%s)\n", name, fxp_err_name(pe));
        fxm_effect_free(f->m, 0, 0); fx2_free(&f->fx); free(f); f = 0; r = HR_FAIL; goto out; }
    f->n = fxm_nodes(f->m);
    f->progs = fxm_progs(f->m, &f->nprogs);
    f->res_shader = calloc(f->fx.nres ? f->fx.nres : 1u, sizeof *f->res_shader);
    f->regs = calloc(f->nprogs ? f->nprogs : 1u, sizeof *f->regs);
    f->saved = calloc(f->fx.ntechs ? f->fx.ntechs : 1u, sizeof *f->saved);
    f->stamp = calloc(f->n + 1u, sizeof *f->stamp);
    f->obj = com_new_d3dx(0, 4u + 4u * (f->n + 1u));
    f->scratch = com_scratch(4096);
    if (!f->res_shader || !f->regs || !f->saved || !f->stamp || !f->obj || !f->scratch){
        fx_destroy(c, f); f = 0; r = HR_OOM; goto out; }
    MR32(f->obj + 4u) = (uint32_t)slot + 1u;
    g_fx[slot] = f;
    f->dev = a[0];
    DEV(c, f, D_ADDREF, 0, 0);
    if (P){ f->pool = P; f->pool_obj = P->obj; P->effects++; com_obj_addref(c, P->obj); }
    fxm_set_x87_pc(f->m, caller_pc(c));
    /* the shaders, one per shader program, in resource order (fx-model 3.1) */
    for (uint32_t i = 0; i < f->nprogs; i++){
        const fxm_prog *pr = &f->progs[i];
        if (pr->kind != FXM_P_SHADER || !pr->res) continue;
        uint32_t ri = (uint32_t)(pr->res - f->fx.res);
        uint32_t hi = pr->version >> 16, out = f->scratch + 4092u;
        if (ri >= f->fx.nres || (hi != 0xFFFEu && hi != 0xFFFFu)) continue;
        MR32(out) = 0;
        uint32_t h = DEV(c, f, hi == 0xFFFEu ? D_CREATEVS : D_CREATEPS, 2, gaddr(pr->res->data), out);
        if (!h) f->res_shader[ri] = MR32(out);
        else fprintf(stderr, "[d3dxfx] %s: resource %u's shader was refused (0x%08x)\n", name, ri, h);
    }
    /* then each shader's device, let go at once: the oracle makes one device Release per shader, after them all
     * (a GetDevice of the shader's, which the recorder does not write, before each) */
    for (uint32_t i = 0; i < f->fx.nres; i++){
        if (!f->res_shader[i]) continue;
        uint32_t out = f->scratch + 4092u; MR32(out) = 0;
        if (!dcall(c, f->res_shader[i], 3, 1, out) && MR32(out)) dcall(c, MR32(out), D_RELEASE, 0);
    }
    MR32(a[7]) = f->obj;
    {
        static int said;
        if (!said++) fprintf(stderr, "[d3dxfx] the effects are the HLE's (runtime/mw/d3dxfx_mw.c)\n");
    }
out:
    if (r && a[7]) MR32(a[7]) = 0;
    POP32(c->eip); c->esp += 36u; c->eax = r;
}

/* ---- the methods ---- */
static uint32_t desc_param(const Fx *f, uint32_t id, uint32_t p){
    const fxm_node *d = fxm_node_get(f->m, id);
    if (!d || !p || (d->kind != FXM_K_PARAM && d->kind != FXM_K_ELEMENT && d->kind != FXM_K_MEMBER && d->kind != FXM_K_ANNOT))
        return HR_INVALIDCALL;
    MR32(p) = gname(d->name); MR32(p + 4u) = gname(d->sem);
    MR32(p + 8u) = d->cls; MR32(p + 12u) = d->type; MR32(p + 16u) = d->rows; MR32(p + 20u) = d->cols;
    MR32(p + 24u) = d->elements; MR32(p + 28u) = d->nannots; MR32(p + 32u) = d->members;
    MR32(p + 36u) = d->flags; MR32(p + 40u) = d->bytes;
    return HR_OK;
}

/* the pass's shaders' code for GetPassDesc: the applied shader of row 146 and 147 */
static uint32_t pass_code(const Fx *f, const fx2_pass *P, int vs){
    for (uint32_t i = 0; P && i < P->nstates; i++){
        const fx2_state_info *in = fx2_state_get(P->states[i].op);
        if (!in || in->cls != (vs ? FX2_SC_VSHADER : FX2_SC_PSHADER)) continue;
        fxm_source s = fxm_state_source(f->m, &P->states[i]);
        if (s.src == FXM_S_SHADER && s.res) return gaddr(s.res->data);
    }
    return 0;
}

static uint32_t m_begin(CPU *c, Fx *f, uint32_t pp, uint32_t flags){
    if (!f->tech) return HR_INVALIDCALL;
    const fxm_node *t = fxm_node_get(f->m, f->tech);
    if (!t) return HR_INVALIDCALL;
    if (!(flags & DONOTSAVESTATE) && t->index < f->fx.ntechs){
        uint32_t *sb = &f->saved[t->index];
        if (!*sb){
            DEV(c, f, D_BEGINSB, 0, 0);
            for (uint32_t i = 0; i < f->fx.techs[t->index].npasses; i++){
                uint32_t p = fxm_pass(f->m, f->tech, i);
                if (p) apply_pass(c, f, p, 1);
            }
            uint32_t out = f->scratch + 4092u; MR32(out) = 0;
            if (!DEV(c, f, D_ENDSB, 1, out)) *sb = MR32(out);
        }
        if (*sb) dcall(c, *sb, SB_CAPTURE, 0);
    }
    if (pp) MR32(pp) = f->fx.techs[t->index].npasses;
    f->started = 1; f->flags = flags; f->pass = -1;
    return HR_OK;
}

static uint32_t fx_method(CPU *c, Fx *f, unsigned m, const uint32_t *a){
    fxm_effect *M = f->m;
    switch (m){
    case 3: {                                                        /* GetDesc */
        uint32_t p = a[1]; if (!p) return HR_INVALIDCALL;
        uint32_t np = 0; while (fxm_param(M, 0, np)) np++;
        MR32(p) = 0; MR32(p + 4u) = np; MR32(p + 8u) = f->fx.ntechs; MR32(p + 12u) = 0;
        return HR_OK; }
    case 4: return desc_param(f, h_param(f, a[1], 1), a[2]);        /* GetParameterDesc */
    case 5: {                                                        /* GetTechniqueDesc */
        uint32_t id = h_tech(f, a[1]); const fxm_node *d = fxm_node_get(M, id);
        if (!d || !a[2]) return HR_INVALIDCALL;
        MR32(a[2]) = gname(d->name); MR32(a[2] + 4u) = f->fx.techs[d->index].npasses; MR32(a[2] + 8u) = d->nannots;
        return HR_OK; }
    case 6: {                                                        /* GetPassDesc */
        uint32_t id = h_pass(f, a[1]); const fxm_node *d = fxm_node_get(M, id);
        if (!d || !a[2]) return HR_INVALIDCALL;
        const fx2_pass *P = pass_of(f, id);
        MR32(a[2]) = gname(d->name); MR32(a[2] + 4u) = d->nannots;
        MR32(a[2] + 8u) = pass_code(f, P, 1); MR32(a[2] + 12u) = pass_code(f, P, 0);
        return HR_OK; }
    case 8: {                                                        /* GetParameter(parent, index) */
        uint32_t par = a[1] ? h_param(f, a[1], 0) : 0;
        if (a[1] && !par) return 0;
        return h_of(f, fxm_param(M, par, a[2])); }
    case 9: case 10: {                                               /* GetParameterByName, BySemantic */
        uint32_t par = a[1] ? h_param(f, a[1], 1) : 0;
        if (a[1] && !par) return 0;
        char s[256]; int n = gstring(a[2], s, sizeof s);
        if (m == 9) return n < 0 ? (par ? h_of(f, fxm_by_name(M, par, 0, 0)) : 0u) : h_of(f, fxm_by_name(M, par, (const uint8_t *)s, (uint32_t)n));
        return h_of(f, fxm_by_semantic(M, par, n < 0 ? 0 : (const uint8_t *)s, n < 0 ? 0u : (uint32_t)n)); }
    case 11: {                                                       /* GetParameterElement */
        uint32_t par = a[1] ? h_param(f, a[1], 0) : 0;
        if (a[1] && !par) return 0;
        return h_of(f, fxm_element(M, par, a[2])); }
    case 12: return h_of(f, fxm_tech(M, a[1]));                     /* GetTechnique */
    case 13: { char s[256]; int n = gstring(a[1], s, sizeof s);     /* GetTechniqueByName */
        return n < 0 ? 0u : h_of(f, fxm_tech_by_name(M, (const uint8_t *)s, (uint32_t)n)); }
    case 14: return h_of(f, fxm_pass(M, h_tech(f, a[1]), a[2]));    /* GetPass */
    case 15: { char s[256]; int n = gstring(a[2], s, sizeof s);     /* GetPassByName */
        return n < 0 ? 0u : h_of(f, fxm_pass_by_name(M, h_tech(f, a[1]), (const uint8_t *)s, (uint32_t)n)); }
    case 16: case 17: return 0;                                     /* GetFunction*: no functions */
    case 18: case 19: {                                              /* GetAnnotation, GetAnnotationByName */
        uint32_t o = h_node(f, a[1]);
        if (!o && a[1]){ o = h_param(f, a[1], 0); if (!o) o = h_tech(f, a[1]); }
        if (!o) return 0;
        if (m == 18) return h_of(f, fxm_annot(M, o, a[2]));
        char s[256]; int n = gstring(a[2], s, sizeof s);
        return n < 0 ? 0u : h_of(f, fxm_annot_by_name(M, o, (const uint8_t *)s, (uint32_t)n)); }
    case 20: case 21: {                                              /* SetValue, GetValue(h, p, bytes) */
        uint32_t id = h_param(f, a[1], m == 21);
        if (!id || !a[2] || !mem_is_committed(a[2]) || !mem_is_committed(a[2] + (a[3] ? a[3] - 1u : 0u))) return HR_INVALIDCALL;
        if (m == 20) return fxm_set_value(M, id, (const uint32_t *)(GMEM_ + a[2]), a[3], 0);
        return fxm_get_value(M, id, (uint32_t *)(GMEM_ + a[2]), a[3]); }
    case 22: { uint32_t id = h_param(f, a[1], 0); return id ? fxm_set_bool(M, id, a[2], 0) : HR_INVALIDCALL; }
    case 23: { uint32_t id = h_param(f, a[1], 1), v; uint32_t r = id && a[2] ? fxm_get_bool(M, id, &v) : HR_INVALIDCALL; if (!r) MR32(a[2]) = v; return r; }
    case 26: { uint32_t id = h_param(f, a[1], 0); return id ? fxm_set_int(M, id, a[2], 0) : HR_INVALIDCALL; }
    case 27: { uint32_t id = h_param(f, a[1], 1), v; uint32_t r = id && a[2] ? fxm_get_int(M, id, &v) : HR_INVALIDCALL; if (!r) MR32(a[2]) = v; return r; }
    case 30: { uint32_t id = h_param(f, a[1], 0); return id ? fxm_set_float(M, id, a[2], 0) : HR_INVALIDCALL; }
    case 31: { uint32_t id = h_param(f, a[1], 1), v; uint32_t r = id && a[2] ? fxm_get_float(M, id, &v) : HR_INVALIDCALL; if (!r) MR32(a[2]) = v; return r; }
    case 24: case 25: case 28: case 29: case 32: case 33: {         /* Set/Get{Bool,Int,Float}Array(h, p, count) */
        uint32_t ty = m < 26 ? FX2_T_BOOL : m < 30 ? FX2_T_INT : FX2_T_FLOAT;
        uint32_t id = h_param(f, a[1], !(m & 1) ? 0 : 1);
        if (!id || !a[2] || a[3] > 0x100000u || !mem_is_committed(a[2]) || !mem_is_committed(a[2] + 4u * (a[3] ? a[3] : 1u) - 1u)) return HR_INVALIDCALL;
        if (!(m & 1)) return fxm_set_array(M, id, ty, (const uint32_t *)(GMEM_ + a[2]), a[3], 0);
        return fxm_get_array(M, id, ty, (uint32_t *)(GMEM_ + a[2]), a[3]); }
    case 34: case 35: {                                              /* SetVector, GetVector */
        uint32_t id = h_param(f, a[1], m == 35);
        if (!id || !a[2] || !mem_is_committed(a[2] + 15u)) return HR_INVALIDCALL;
        if (m == 34){ fxm_set_x87_pc(M, caller_pc(c)); return fxm_set_vector(M, id, (const uint32_t *)(GMEM_ + a[2]), 0); }
        return fxm_get_vector(M, id, (uint32_t *)(GMEM_ + a[2])); }
    case 36: case 37: {                                              /* Set/GetVectorArray */
        uint32_t id = h_param(f, a[1], m == 37);
        if (!id || !a[2] || a[3] > 0x40000u || !mem_is_committed(a[2] + 16u * (a[3] ? a[3] : 1u) - 1u)) return HR_INVALIDCALL;
        if (m == 36) return fxm_set_vector_array(M, id, (const uint32_t *)(GMEM_ + a[2]), a[3], 0);
        return fxm_get_vector_array(M, id, (uint32_t *)(GMEM_ + a[2]), a[3]); }
    case 38: case 39: case 44: case 45: {                            /* Set/GetMatrix, Set/GetMatrixTranspose */
        uint32_t id = h_param(f, a[1], (m & 1));
        if (!id || !a[2] || !mem_is_committed(a[2] + 63u)) return HR_INVALIDCALL;
        int tr = m >= 44;
        if (!(m & 1)) return fxm_set_matrix(M, id, (const uint32_t *)(GMEM_ + a[2]), tr, 0);
        return fxm_get_matrix(M, id, (uint32_t *)(GMEM_ + a[2]), tr); }
    case 40: case 41: case 46: case 47: {                            /* Set/GetMatrix(Transpose)Array */
        uint32_t id = h_param(f, a[1], (m & 1));
        if (!id || !a[2] || a[3] > 0x10000u || !mem_is_committed(a[2] + 64u * (a[3] ? a[3] : 1u) - 1u)) return HR_INVALIDCALL;
        int tr = m >= 46;
        if (!(m & 1)) return fxm_set_matrix_array(M, id, (const uint32_t *)(GMEM_ + a[2]), a[3], tr, 0);
        return fxm_get_matrix_array(M, id, (uint32_t *)(GMEM_ + a[2]), a[3], tr); }
    case 50: case 52: {                                              /* SetString, SetTexture */
        uint32_t id = h_param(f, a[1], 0), old = 0; int ch = 0;
        if (!id) return HR_INVALIDCALL;
        const fxm_node *d = fxm_node_get(M, id);
        if (!d || (m == 52 && (d->type < FX2_T_TEXTURE || d->type > FX2_T_TEXTURECUBE || d->elements))
               || (m == 50 && d->type != FX2_T_STRING)) return HR_INVALIDCALL;
        uint32_t r = fxm_set_object(M, id, a[2], &old, &ch);
        if (r || m == 50 || old == a[2]) return r;
        if (a[2]) com_obj_addref(c, a[2]);
        if (old) com_obj_release(c, old);
        return HR_OK; }
    case 51: case 53: {                                              /* GetString, GetTexture */
        uint32_t id = h_param(f, a[1], m == 51), v = 0;
        if (!id || !a[2]) return HR_INVALIDCALL;
        uint32_t r = fxm_get_object(M, id, &v);
        if (!r){ MR32(a[2]) = v; if (m == 53 && v) com_obj_addref(c, v); }
        return r; }
    case 56: return HR_OK;                                          /* SetArrayRange */
    case 57: if (!a[1]) return HR_INVALIDCALL;                      /* GetPool */
        MR32(a[1]) = f->pool_obj; if (f->pool_obj) com_obj_addref(c, f->pool_obj); return HR_OK;
    case 58: {                                                       /* SetTechnique */
        uint32_t t = h_tech(f, a[1]);
        if (!t) return HR_INVALIDCALL;
        f->tech = t; return HR_OK; }
    case 59: return h_of(f, f->tech);                               /* GetCurrentTechnique */
    case 60: {                                                       /* ValidateTechnique */
        uint32_t t = h_tech(f, a[1]); const fxm_node *d = fxm_node_get(M, t);
        if (!d) return HR_INVALIDCALL;
        for (uint32_t p = 0; p < f->fx.techs[d->index].npasses; p++){   /* a shader the device refused fails it */
            const fx2_pass *P = &f->fx.techs[d->index].passes[p];
            for (uint32_t i = 0; i < P->nstates; i++){
                fxm_source s = fxm_state_source(M, &P->states[i]);
                if (s.src == FXM_S_SHADER && s.res && !f->res_shader[s.res - f->fx.res]) return HR_FAIL;
            }
        }
        return HR_OK; }
    case 61: {                                                       /* FindNextValidTechnique(h, &next) */
        uint32_t t = a[1] ? h_tech(f, a[1]) : 0, from = 0;
        if (t){ const fxm_node *d = fxm_node_get(M, t); from = d ? d->index + 1u : 0u; }
        for (uint32_t i = from; i < f->fx.ntechs; i++){
            uint32_t h = h_of(f, fxm_tech(M, i));
            uint32_t sv[2] = { f->obj, h };
            if (!fx_method(c, f, 60, sv)){ if (a[2]) MR32(a[2]) = h; return HR_OK; }
        }
        if (a[2]) MR32(a[2]) = 0;
        return HR_FAIL; }
    case 62: {                                                       /* IsParameterUsed(h, tech) */
        uint32_t id = h_param(f, a[1], 0), t = h_tech(f, a[2]);
        const fxm_node *d = fxm_node_get(M, id), *td = fxm_node_get(M, t);
        if (!d || !td) return 0;
        uint32_t top = d->top;
        for (uint32_t p = 0; p < f->fx.techs[td->index].npasses; p++){
            const uint32_t *deps; uint32_t nd = fxm_pass_deps(M, fxm_pass(M, t, p), &deps);
            for (uint32_t i = 0; i < nd; i++) if (deps[i] == top) return 1;
        }
        return 0; }
    case 63: return m_begin(c, f, a[1], a[2]);                      /* Begin */
    case 64: {                                                       /* BeginPass */
        if (!f->started || !f->tech) return HR_INVALIDCALL;
        uint32_t p = fxm_pass(M, f->tech, a[1]);
        if (!p) return HR_INVALIDCALL;
        f->pass = (int)a[1];
        apply_pass(c, f, p, 1);
        return HR_OK; }
    case 65: {                                                       /* CommitChanges */
        if (!f->started || f->pass < 0) return HR_OK;
        uint32_t p = fxm_pass(M, f->tech, (uint32_t)f->pass);
        if (p) apply_pass(c, f, p, 0);
        return HR_OK; }
    case 66: f->pass = -1; return HR_OK;                            /* EndPass */
    case 67: {                                                       /* End */
        if (!f->started) return HR_OK;
        f->started = 0; f->pass = -1;
        const fxm_node *t = fxm_node_get(M, f->tech);
        if (!(f->flags & DONOTSAVESTATE) && t && t->index < f->fx.ntechs && f->saved[t->index])
            dcall(c, f->saved[t->index], SB_APPLY, 0);
        return HR_OK; }
    case 68: if (!a[1]) return HR_INVALIDCALL;                      /* GetDevice */
        MR32(a[1]) = f->dev; DEV(c, f, D_ADDREF, 0, 0); return HR_OK;
    case 69:                                                         /* OnLostDevice: the saved states go */
        for (uint32_t t = 0; t < f->fx.ntechs; t++)
            if (f->saved[t]){ dcall(c, f->saved[t], SB_RELEASE, 0); f->saved[t] = 0; }
        return HR_OK;
    case 70: return HR_OK;                                          /* OnResetDevice */
    default:
        say_once(8 + (int)(m & 31), "[d3dxfx] ID3DXEffect::%s is not answered (E_NOTIMPL)\n", fxmark_name((int)m));
        return HR_NOTIMPL;
    }
}

uint32_t d3dxfx_call(CPU *c, int pool, unsigned method, const uint32_t *args, int nargs){
    (void)nargs;
    if (pool){ say_once(7, "[d3dxfx] ID3DXEffectPool method %u is not answered\n", method); return HR_NOTIMPL; }
    Fx *f = fx_of(args[0]);
    if (!f) return HR_INVALIDCALL;
    if (f->id < 0) f->id = fx_wrapper_id(f->obj);
    if (g_comrec_on){
        char arg[16] = "";
        if (method == 63) snprintf(arg, sizeof arg, "%x", args[2]);
        else if (method == 64) snprintf(arg, sizeof arg, "%u", args[1]);
        fxmark(f->id, fxmark_name((int)method), arg[0] ? arg : 0, 0);
    }
    return fx_method(c, f, method, args);
}

void d3dxfx_free(CPU *c, int pool, uint32_t obj){
    if (pool){
        Pool *P = pool_of(obj);
        if (!P) return;
        for (int i = 0; i < MAXPOOL; i++) if (g_pool[i] == P) g_pool[i] = 0;
        if (P->effects) say_once(9, "[d3dxfx] a pool freed while %d effects use it: kept\n", P->effects);
        else fxm_pool_free(P->p);
        free(P);
        return;
    }
    Fx *f = fx_of(obj);
    if (!f) return;
    for (int i = 0; i < MAXFX; i++) if (g_fx[i] == f) g_fx[i] = 0;
    fx_destroy(c, f);
}

/* ---- the binding (as d3dxmath_mw.c's): the IAT slot to the exe's own `jmp [slot]` thunk, the thunk's
 * dispatch entry to the answer ---- */
static const struct { const char *name; uint32_t iat, thunk; void (*fn)(CPU *); } k_imp[2] = {
    { "D3DXCreateEffectFromResourceA", 0x008904c0u, 0x007e7d76u, hle_create_effect },
    { "D3DXCreateEffectPool",          0x008904c4u, 0x007e7d7cu, hle_create_pool },
};
int d3dxfx_install(int oracle_linked){
    const char *e = getenv("NFSMW_D3DX_FX");
    if (oracle_linked && !(e && !strcmp(e, "hle"))) return 0;
    int n = 0;
    for (int i = 0; i < 2; i++){
        if (MR8(k_imp[i].thunk) != 0xFFu || MR8(k_imp[i].thunk + 1u) != 0x25u || MR32(k_imp[i].thunk + 2u) != k_imp[i].iat){
            fprintf(stderr, "[d3dxfx] %s: no `jmp [0x%08x]` at 0x%08x, left unanswered\n", k_imp[i].name, k_imp[i].iat, k_imp[i].thunk);
            continue;
        }
        if (!dispatch_patch_take(k_imp[i].thunk, k_imp[i].fn)) continue;
        MR32(k_imp[i].iat) = k_imp[i].thunk;
        n++;
    }
    fprintf(stderr, "[d3dxfx] %d of 2 D3DX effect imports answered natively%s\n", n, oracle_linked ? " (NFSMW_D3DX_FX=hle)" : "");
    return n;
}
