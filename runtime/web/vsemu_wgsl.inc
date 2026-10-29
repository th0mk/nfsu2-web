/* D3D9 shader bytecode -> WGSL: the WebGPU twin of vs_to_msl / ps_to_msl_ex2.
 *
 * Included at the end of vsemu.c (wasm builds, or native with -DVSEMU_WGSL for
 * offline tools) so it shares the parser's private VSProg/VSIns and token
 * macros.  The authoritative contract with the renderer is docs/web/gpu-abi.md:
 * module layout, bindings, structs, override constants.
 *
 * Semantics are the Mac backends' on purpose (the Mac build is the reference
 * look), instruction for instruction: same register masking, same defaults,
 * same edge cases (rcp/rsq/log of 0), same skipped ps_1_x texture ops, same
 * ps_2_0 t# quirk (behind an opt-in override).  Where WGSL forces a different
 * spelling the comment says so:
 *   - no masked swizzle assignment: writes are expanded per component;
 *   - no implicit float->int: a0 writes convert explicitly (truncation, as the
 *     MSL implicit conversion; mova rounds half away from zero, as MSL round);
 *   - constant expressions that overflow (1.0/0.0, log2(0.0) ...) are shader
 *     creation errors, so `def` constants are function-scope `let`s and every
 *     register read is a runtime value: nothing can ever be const-folded into
 *     an error;
 *   - no preprocessor: the MSL macros (SMPk, TSEL, LDF ...) become helpers.
 * Every module is complete text (no prelude added by the renderer) with one
 * entry point. */
#include <stdarg.h>
#include "wgsl.h"
#include "gpu_proto.h"
#define WSTR_(x) #x
#define WSTR(x) WSTR_(x)

typedef struct { char *p; size_t cap, n; int ok; } WBuf;
static void wb_init(WBuf *b, char *p, size_t cap){
    b->p = p; b->cap = cap; b->n = 0; b->ok = (p && cap > 0);
    if (b->ok) p[0] = 0;
}
static void wb_put(WBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void wb_put(WBuf *b, const char *fmt, ...){
    if (!b->ok) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->cap - b->n){ b->ok = 0; b->p[b->n] = 0; return; }
    b->n += (size_t)w;
}

/* Texture stages of every module (gpu-abi.md section 11.2, GwCtl.stages): 4
 * (UG2, the default; the modules are exactly what they were before 8 stages
 * existed) or 8: VOut gains uv4..uv7, a vertex shader's oT4..oT7 are their
 * own registers, ps_2_0 reads t0..t7 and s0..s7 with the declared sampler
 * types, and every module's first line says "8 stages".  Set once, before
 * the first translation. */
static unsigned g_wstages = 4;
void wgsl_set_stages(unsigned n){ g_wstages = n == 8 ? 8u : 4u; }
unsigned wgsl_get_stages(void){ return g_wstages; }
/* binding kinds (GW_BK_*, 2 bits per stage) of the last pixel shader translated */
static unsigned g_wkinds;
unsigned wgsl_ps_kinds(void){ return g_wkinds; }
/* the stages (bit k) whose texture is a depth texture, for the pixel shaders
 * translated until it is set again (gpu-abi.md 11.4: a draw's depth variant);
 * 8 stages only, 2D stages only */
static unsigned g_wdepth;
void wgsl_set_depth_stages(unsigned mask){ g_wdepth = g_wstages == 8 ? mask & 0xFFu : 0u; }

/* ---- shared module text (gpu-abi.md sections 2-5) ---- */
static const char W_VOUT[] =
    "struct VOut {\n"
    "  @builtin(position) pos: vec4f,\n"
    "  @location(0) col0: vec4f, @location(1) col1: vec4f,\n"
    "  @location(2) uv0: vec4f, @location(3) uv1: vec4f, @location(4) uv2: vec4f, @location(5) uv3: vec4f,\n"
    "  @location(6) fog: f32,\n"
    "}\n";
static const char W_VOUT8[] =
    "struct VOut {\n"
    "  @builtin(position) pos: vec4f,\n"
    "  @location(0) col0: vec4f, @location(1) col1: vec4f,\n"
    "  @location(2) uv0: vec4f, @location(3) uv1: vec4f, @location(4) uv2: vec4f, @location(5) uv3: vec4f,\n"
    "  @location(6) fog: f32,\n"
    "  @location(7) uv4: vec4f, @location(8) uv5: vec4f, @location(9) uv6: vec4f, @location(10) uv7: vec4f,\n"
    "}\n";
static const char *w_vout(void){ return g_wstages == 8 ? W_VOUT8 : W_VOUT; }
static const char *w_st8(void){ return g_wstages == 8 ? ", 8 stages" : ""; }
/* K is a 64 KB window of the frame's VS constant arena, bound once for many
 * draws; a draw's block {stride, tsel, base, pad; c[0..255]} starts at row
 * kb = instance_index (the draw's firstInstance), so a draw no longer needs a
 * bind group of its own (gpu-abi.md section 3). */
static const char W_VSPAR[] =
    "struct VSWin { c: array<vec4f, " WSTR(GW_KWIN_ROWS) "> }\n"
    "@group(0) @binding(0) var<storage, read> VB: array<u32>;\n"
    "@group(0) @binding(1) var<uniform> K: VSWin;\n";
/* Byte-addressed little-endian loads from the vertex buffer at any alignment
 * (a misaligned u32 combines two words).  The same text is used by the pull
 * test module, so the compute test checks exactly what the VS modules run. */
static const char W_PULL[] =
    "fn ld32(a: u32) -> u32 {\n"
    "  let w = a >> 2u;\n"
    "  let s = (a & 3u) << 3u;\n"
    "  if (s == 0u) { return VB[w]; }\n"
    "  return (VB[w] >> s) | (VB[w + 1u] << (32u - s));\n"
    "}\n"
    "fn ld16(a: u32) -> u32 {\n"
    "  let s = (a & 3u) << 3u;\n"
    "  if (s <= 16u) { return (VB[a >> 2u] >> s) & 0xFFFFu; }\n"
    "  return ld32(a) & 0xFFFFu;\n"
    "}\n"
    "fn ldf(a: u32) -> f32 { return bitcast<f32>(ld32(a)); }\n"
    "fn lds(a: u32) -> f32 { return f32(bitcast<i32>(ld16(a) << 16u) >> 16u); }\n"
    "fn ldu(a: u32) -> f32 { return f32(ld16(a)); }\n"
    "fn ldub4(a: u32) -> vec4f {\n"
    "  let u = ld32(a);\n"
    "  return vec4f(f32(u & 0xFFu), f32((u >> 8u) & 0xFFu), f32((u >> 16u) & 0xFFu), f32(u >> 24u));\n"
    "}\n"
    /* x / y rounded exactly like the C division in decl_fetch: the
     * reciprocal product corrected by one FMA residual step (r = f32(1/y)).
     * A plain WGSL division is not correctly rounded on the GPU (measured in
     * Chrome on Metal: 126 of the 256 byte values of b/255 differ by 1 ulp);
     * this form matched all 256 + 65536 + 65536 cases of /255, /32767, /65535. */
    "fn cdiv(x: vec4f, y: f32, r: f32) -> vec4f {\n"
    "  let q = x * r;\n"
    "  return fma(fma(-q, vec4f(y), x), vec4f(r), q);\n"
    "}\n"
    "fn n8(x: vec4f) -> vec4f { return cdiv(x, 255.0, 0x1.010102p-8); }\n"
    "fn s16n(x: vec4f) -> vec4f { return cdiv(x, 32767.0, 0x1.0002p-15); }\n"
    "fn u16n(x: vec4f) -> vec4f { return cdiv(x, 65535.0, 0x1.0001p-16); }\n"
    "fn ldcolor(a: u32) -> vec4f { return n8(ldub4(a).zyxw); }\n";
static const char W_TPICK[] =
    "fn tpick(s: u32, a: vec4f, b: vec4f, c: vec4f, d: vec4f) -> vec4f {\n"
    "  return select(select(a, b, s == 1u), select(c, d, s == 3u), s >= 2u);\n"
    "}\n";
static const char W_PSPAR[] =
    "struct PSPar { aref: f32, usec: f32, pad0: f32, pad1: f32, proj: vec4f, c: array<vec4f, 32>, fog: vec4f }\n"
    "@group(0) @binding(2) var<uniform> P: PSPar;\n"
    "override ALPHA_FUNC: u32 = 7u;\n"
    "override PS20_TEXCOORD: bool = true;\n"
    /* D3DCMP against P.aref; 7 is spelled !(a < r) so a NaN alpha passes, as
     * the Mac's "if (a < aref) discard" does. */
    "fn alpha_pass(a: f32, r: f32) -> bool {\n"
    "  switch ALPHA_FUNC {\n"
    "    case 1u: { return false; }\n"
    "    case 2u: { return a < r; }\n"
    "    case 3u: { return a == r; }\n"
    "    case 4u: { return a <= r; }\n"
    "    case 5u: { return a > r; }\n"
    "    case 6u: { return a != r; }\n"
    "    case 7u: { return !(a < r); }\n"
    "    default: { return true; }\n"
    "  }\n"
    "}\n"
    /* Alpha test, then D3D9 vertex fog C = f*Cs + (1-f)*Cfog on rgb only;
     * P.fog.a = FOGENABLE.  ALPHA_FUNC 8 folds the discard away (the Mac's
     * no-discard twin, which keeps hidden-surface removal on Apple GPUs). */
    "fn ps_end(c: vec4f, fog: f32) -> vec4f {\n"
    "  if (ALPHA_FUNC != 8u) { if (!alpha_pass(c.a, P.aref)) { discard; } }\n"
    "  return vec4f(mix(P.fog.rgb, c.rgb, mix(1.0, saturate(fog), P.fog.a)), c.a);\n"
    "}\n"
    /* ps_1_x coordinate of stage k: P.proj[k] = coordIndex + 16*projMode,
     * mode 1 divides by z, 2 by w (texproj_push in com.c).  select(), not a
     * branch, keeps the sample in uniform control flow. */
    "fn tcoord(q: vec4f, sel: f32) -> vec2f {\n"
    "  let m = i32(sel) >> 4u;\n"
    "  return q.xy / select(select(1.0, q.z, m == 1), q.w, m == 2);\n"
    "}\n";

/* a stage's texture and sampler types: a depth stage (wgsl_set_depth_stages, never
 * with 4 stages) is texture_depth_2d with a comparison sampler */
static const char *w_textype(unsigned cubemask, unsigned depthmask, int k){
    return ((depthmask >> k) & 1u) ? "texture_depth_2d" : ((cubemask >> k) & 1u) ? "texture_cube<f32>" : "texture_2d<f32>";
}
static const char *w_smptype(unsigned depthmask, int k){ return ((depthmask >> k) & 1u) ? "sampler_comparison" : "sampler"; }
static void w_textures(WBuf *b, unsigned cubemask, unsigned depthmask){
    for (int k = 0; k < 4; k++)
        wb_put(b, "@group(1) @binding(%d) var tex%d: %s;\n", k, k, w_textype(cubemask, depthmask, k));
    for (int k = 0; k < 4; k++)
        wb_put(b, "@group(1) @binding(%d) var smp%d: %s;\n", 4 + k, k, w_smptype(depthmask, k));
    /* 8 stages: stages 4..7 after them (texBinding/smpBinding in shader-abi.ts) */
    if (g_wstages == 8){
        for (int k = 4; k < 8; k++)
            wb_put(b, "@group(1) @binding(%d) var tex%d: %s;\n", k + 4, k, w_textype(cubemask, depthmask, k));
        for (int k = 4; k < 8; k++)
            wb_put(b, "@group(1) @binding(%d) var smp%d: %s;\n", k + 8, k, w_smptype(depthmask, k));
    }
}

/* f32 literal that survives a round trip (9 significant digits) and always
 * reads as a float.  Non-finite values have no WGSL literal: reject (the Mac's
 * %g would print "inf"/"nan", which does not compile in MSL either). */
static int w_float(char *o, size_t cap, float f){
    if (!isfinite(f)) return 0;
    char t[48]; snprintf(t, sizeof t, "%.9g", (double)f);
    if (!strpbrk(t, ".e")) snprintf(o, cap, "%s.0", t);
    else snprintf(o, cap, "%s", t);
    return 1;
}

/* ---- register naming (msl_reg with WGSL names) ---- */
typedef struct {
    const VSProg *p;
    int      vs;            /* 1 = vertex shader (K.c, r0..r31, v0..v15, a0) */
    uint32_t r, v, t, ci;   /* registers used (bit n) */
    uint32_t cdef[8];       /* def'd constants used */
    int      a0, aL, oC, Z, dummy;
    int      bad;           /* text overflow or unsupported operand: reject */
} WCtx;

static void w_reg(WCtx *x, char *o, size_t cap, unsigned tok, unsigned rel){
    const VSProg *p = x->p;
    int ty = RT(tok), n = RN(tok), vs = x->vs;
    switch (ty){
    case R_TEMP:  n &= vs ? 31 : 15; x->r |= 1u << n; snprintf(o, cap, "r%d", n); return;
    case R_INPUT: n &= vs ? 15 : 1;  x->v |= 1u << n; snprintf(o, cap, "v%d", n); return;
    case R_ADDR:
        if (vs){ x->a0 = 1; snprintf(o, cap, "vec4f(a0)"); }
        else   { n &= (g_wstages == 8 && p->version >= 0x0200) ? 7 : 3; x->t |= 1u << n; snprintf(o, cap, "t%d", n); }
        return;
    case R_LOOP:     x->aL = 1; snprintf(o, cap, "vec4f(f32(aL))"); return;
    case R_COLOROUT: x->oC = 1; snprintf(o, cap, "oC"); return;
    case R_CONSTINT:
        if (n >= 0 && n < 16){ x->ci |= 1u << n; snprintf(o, cap, "i%d", n); }
        else { x->Z = 1; snprintf(o, cap, "Z"); }
        return;
    case R_CONST: case R_CONST2: case R_CONST3: case R_CONST4: {
        int idx = n;
        if (ty == R_CONST2) idx += 2048; else if (ty == R_CONST3) idx += 4096;
        else if (ty == R_CONST4) idx += 6144;
        /* relative addressing (vertex only, as on the Mac): c[a0.<comp> + idx],
         * clamped to the 256-entry array */
        if (RELADDR(tok) && vs){
            char comp = 'x';
            if (rel){
                if (RT(rel) == R_LOOP) comp = 0;
                else comp = SWCH[SWIZ(rel) & 3u];
            }
            if (comp){ x->a0 = 1; snprintf(o, cap, "K.c[kb + 1u + u32(clamp(a0.%c + %d, 0, 255))]", comp, idx); }
            else     { x->aL = 1; snprintf(o, cap, "K.c[kb + 1u + u32(clamp(aL + %d, 0, 255))]", idx); }
            return;
        }
        if (idx >= 0 && idx < 256 && (p->defmask[idx >> 5] & (1u << (idx & 31)))){
            x->cdef[idx >> 5] |= 1u << (idx & 31);
            snprintf(o, cap, "c%d", idx);
        } else if (vs) snprintf(o, cap, "K.c[kb + %du]", (idx & 255) + 1);   /* row 0 of the block is its header */
        else snprintf(o, cap, "P.c[%d]", idx & 31);
        return; }
    default: x->Z = 1; snprintf(o, cap, "Z"); return;
    }
}

/* Source operand: register + swizzle + source modifier (msl_src). */
static void w_src(WCtx *x, char *o, size_t cap, unsigned tok, unsigned rel){
    char reg[96]; w_reg(x, reg, sizeof reg, tok, rel);
    unsigned sw = SWIZ(tok);
    char sws[8];
    if (sw == 0xE4u) sws[0] = 0;
    else { sws[0] = '.'; for (int i = 0; i < 4; i++) sws[1+i] = SWCH[(sw >> (2*i)) & 3u]; sws[5] = 0; }
    char body[128];
    snprintf(body, sizeof body, "%s%s", reg, sws);
    int w;
    switch (SMOD(tok)){
    case 1:  w = snprintf(o, cap, "(-%s)", body); break;                    /* NEG  */
    case 2:  w = snprintf(o, cap, "(%s - 0.5)", body); break;               /* BIAS */
    case 3:  w = snprintf(o, cap, "(-(%s - 0.5))", body); break;
    case 4:  w = snprintf(o, cap, "(2.0 * %s - 1.0)", body); break;         /* SIGN (bx2) */
    case 5:  w = snprintf(o, cap, "(-(2.0 * %s - 1.0))", body); break;
    case 6:  w = snprintf(o, cap, "(1.0 - %s)", body); break;               /* COMP */
    case 7:  w = snprintf(o, cap, "(2.0 * %s)", body); break;               /* X2   */
    case 8:  w = snprintf(o, cap, "(-2.0 * %s)", body); break;
    case 11: w = snprintf(o, cap, "abs(%s)", body); break;                  /* ABS  */
    case 12: w = snprintf(o, cap, "(-abs(%s))", body); break;
    default: w = snprintf(o, cap, "%s", body); break;
    }
    if (w < 0 || (size_t)w >= cap) x->bad = 1;
}

/* Write `val` (a vec4f expression) to `name` under the destination's write
 * mask.  WGSL has no masked swizzle assignment, so a partial mask evaluates
 * once into a let and stores component by component.  `toint` converts for
 * the integer destination a0 (truncation toward zero, as MSL's implicit
 * float -> int conversion). */
static void w_store(WBuf *b, const char *name, unsigned mask, const char *val, int toint){
    if (mask == 0u || mask == 0xFu){
        if (toint) wb_put(b, "  %s = vec4i(%s);\n", name, val);
        else       wb_put(b, "  %s = %s;\n", name, val);
        return;
    }
    wb_put(b, "  { let e = %s;", val);
    for (int c = 0; c < 4; c++)
        if (mask & (1u << c)){
            if (toint) wb_put(b, " %s.%c = i32(e.%c);", name, SWCH[c], SWCH[c]);
            else       wb_put(b, " %s.%c = e.%c;", name, SWCH[c], SWCH[c]);
        }
    wb_put(b, " }\n");
}

static void w_decls(WBuf *b, const WCtx *x, int nr){
    if (x->Z) wb_put(b, "  let Z = vec4f(0.0);\n");
    for (int i = 0; i < nr; i++) if (x->r & (1u << i)) wb_put(b, "  var r%d = vec4f(0.0);\n", i);
    for (int i = 0; i < 256; i++){
        if (!(x->cdef[i >> 5] & (1u << (i & 31)))) continue;
        char f[4][48];
        for (int c = 0; c < 4; c++) w_float(f[c], sizeof f[c], x->p->def[i][c]);
        wb_put(b, "  let c%d = vec4f(%s, %s, %s, %s);\n", i, f[0], f[1], f[2], f[3]);
    }
    for (int i = 0; i < 16; i++) if (x->ci & (1u << i))
        wb_put(b, "  let i%d = vec4f(%d.0, %d.0, %d.0, %d.0);\n", i, x->p->defi[i][0],
               x->p->defi[i][1], x->p->defi[i][2], x->p->defi[i][3]);
    if (x->a0) wb_put(b, "  var a0 = vec4i(0);\n");
    if (x->aL) wb_put(b, "  var aL: i32 = 0;\n");
    if (x->dummy) wb_put(b, "  var dummy = vec4f(0.0);\n");
}

static int w_defs_finite(const VSProg *p){
    for (int i = 0; i < 256; i++)
        if (p->defmask[i >> 5] & (1u << (i & 31)))
            for (int c = 0; c < 4; c++) if (!isfinite(p->def[i][c])) return 0;
    return 1;
}

/* The other cars' headlights (runtime/headlights.c, gpu_proto.h GwHL).  The
 * worldheadlight pixel shaders (ps_1_1) light a surface with
 *     mul rA.rgb, t1, t2          headlight texture (stage 1, at uv1.xy / uv1.w)
 *                                 x falloff ramp (stage 2)
 *     mad rA.rgb, rA, c0, v0      x HEADLIGHTCOLOR + the vertex light
 * before the texture multiply; after that mad, every other car's light is
 * added the same way, from its coordinate uv1 * t and its own colour. */
static const char W_HL[] =
    "struct HLight { t: mat4x4f, col: vec4f }\n"
    "struct HLPar { n: u32, s: f32, o: f32, pad: f32, l: array<HLight, " WSTR(GW_HL_MAX) "> }\n"
    "@group(0) @binding(3) var<uniform> H: HLPar;\n"
    "fn hl_more(q: vec4f, v: f32) -> vec3f {\n"
    "  var a = vec3f(0.0);\n"
    "  for (var k = 0u; k < min(H.n, " WSTR(GW_HL_MAX) "u); k++) {\n"
    "    let p = q * H.l[k].t;\n"
    "    let l1 = textureSample(tex1, smp1, p.xy / p.w).rgb;\n"
    "    let l2 = textureSample(tex2, smp2, vec2f(H.s * p.w + H.o, v)).rgb;\n"
    "    a += select(vec3f(0.0), l1 * l2 * H.l[k].col.rgb, p.w > 0.0);\n"
    "  }\n"
    "  return a;\n"
    "}\n";
static int w_plain(unsigned tok, int ty, int n){
    return RT(tok) == ty && RN(tok) == n && SWIZ(tok) == 0xE4u && SMOD(tok) == 0;
}

/* ====================================================================
 * Pixel shader (ps_to_msl_ex2)
 * ==================================================================== */
#define WEX 4096
/* ps_2_x, ps_3_0 and vs_3_0: vsemu_sm3.inc (D3D9's semantics) */
static int ps_to_wgsl_sm3(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask);
static int vs_to_wgsl_sm3(const VSProg *p, int id, const int decl[16][2], char *out, size_t cap);
int ps_to_wgsl_ex2(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask, int reflk){
    g_wkinds = 0;
    if (!p || !out || cap < 512) return 0;
    if (reflk >= 0) return 0;                   /* HD reflection variants: Mac-only */
    if (p->raw) return ps_to_wgsl_sm3(p, id, out, cap, cubemask);
    if (!w_defs_finite(p)) return 0;
    /* 8 stages: ps_2_0 samples s0..s7 as they are declared (a cube is a
     * texture_cube, a volume is refused), as ps_2_x does */
    unsigned smask = (g_wstages == 8 && p->version >= 0x0200) ? 7u : 3u;
    if (smask == 7u)
        for (int k = 0; k < 8; k++){
            if (p->smptype[k] == 4) return 0;
            if (p->smptype[k] == 3){ cubemask |= 1u << k; g_wkinds |= GW_BK_CUBE << (2 * k); }
        }
    /* the depth stages of a draw's depth variant (wgsl_set_depth_stages; 8 stages only) */
    const unsigned dmask = g_wdepth & ~cubemask & (smask == 7u ? 0xFFu : 0xFu);
    for (int k = 0; k < 8; k++) if ((dmask >> k) & 1u) g_wkinds |= GW_BK_DEPTH << (2 * k);
    /* ps_2_0's texldp divides its coordinate by w (8 stages: D3D9's rule; with 4 the Mac's, which reads xy) */
    const int pdiv = g_wstages == 8 && p->version >= 0x0200;
    WCtx x; memset(&x, 0, sizeof x); x.p = p; x.vs = 0;
    char *bodybuf = malloc(cap);
    if (!bodybuf) return 0;
    WBuf bb; wb_init(&bb, bodybuf, cap);
    char *ex = malloc(WEX), *s0 = malloc(WEX), *s1 = malloc(WEX), *s2 = malloc(WEX), *val = malloc(WEX);
    int rc = 0;
    if (!ex || !s0 || !s1 || !s2 || !val) goto done;

    /* ps_2_0: `dcl tN` coordinate registers, loaded as D3D does.  The Mac
     * never loads them (they read 0, gfx-d3d9.md section 9 #1): then depth
     * of field (the scenes before and after a race) samples one texel and
     * leaves only the light glows on a flat colour.  PS20_TEXCOORD = false
     * gives the Mac's look (A/B). */
    if (p->version >= 0x0200){
        for (unsigned q = 0; q < p->n; q++){
            const VSIns *y = &p->ins[q];
            if (y->op != O_DCL || RT(y->dst) != R_ADDR) continue;
            int k = RN(y->dst) & (int)smask;
            x.t |= 1u << k;
            wb_put(&bb, "  t%d = select(vec4f(0.0), i.uv%d, PS20_TEXCOORD);\n", k, k);
        }
    }
    /* Coordinates only for stages that are sampled through tex / texld tM
     * (same rule as the Mac: tcuse). */
    unsigned tcuse = 0;
    for (unsigned q = 0; q < p->n; q++){
        const VSIns *y = &p->ins[q];
        if (y->op != O_TEX && y->op != O_TEXLDD && y->op != O_TEXLDL) continue;
        if (y->nsrc == 1 && RT(y->src[0]) == R_ADDR) tcuse |= 1u << (RN(y->src[0]) & 3);
        else if (y->nsrc == 0)                       tcuse |= 1u << (RN(y->dst) & 3);
    }
    for (int k = 0; k < 4; k++)
        if (tcuse & (1u << k)) wb_put(&bb, "  let tc%d = tcoord(i.uv%d, P.proj[%d]);\n", k, k, k);

    int hl_mul = -1, hl_used = 0;
    for (unsigned q = 0; q < p->n && bb.ok && !x.bad; q++){
        const VSIns *y = &p->ins[q];
        if (y->nsrc > 0) w_src(&x, s0, WEX, y->src[0], y->rel[0]);
        if (y->nsrc > 1) w_src(&x, s1, WEX, y->src[1], y->rel[1]);
        if (y->nsrc > 2) w_src(&x, s2, WEX, y->src[2], y->rel[2]);
        int w = 0;
        ex[0] = 0;
        switch (y->op){
        case O_NOP: case O_DCL: case O_DEF: case O_DEFI: case O_DEFB:
        case O_LABEL: case O_END:
            continue;
        case O_TEX: case O_TEXLDD: case O_TEXLDL: {
            /* ps_1_x "tex tN": sampler N, coordinate N (projective)
             * ps_1_4 "texld rN, tM": sampler N, coordinate M
             * ps_1_4 "texld rN, rM": dependent read at rM.xy
             * ps_2_0 "texld d, src, sN": sampler N at src.xy
             * A cube stage samples the 3-component vector (no divide). */
            int dn = RN(y->dst) & 3, smp, crd = -1;
            const char *cs = 0;
            if (y->nsrc >= 2){ smp = RN(y->src[1]) & (int)smask; cs = s0; }
            else if (y->nsrc == 1 && RT(y->src[0]) == R_ADDR){ smp = dn; crd = RN(y->src[0]) & 3; }
            else if (y->nsrc == 1){ smp = dn; cs = s0; }
            else { smp = dn; crd = dn; }
            int cube = (cubemask >> smp) & 1u;
            int proj = pdiv && y->op == O_TEX && y->cmp == 1u && y->nsrc >= 2;     /* ps_2_0 texldp */
            if ((dmask >> smp) & 1u){
                /* a depth texture (gpu-abi.md 11.4): the coordinate after the divide the
                 * instruction asks for, the comparison result in all four components */
                if (crd >= 0)
                    w = snprintf(ex, WEX, "vec4f(textureSampleCompareLevel(tex%d, smp%d, tc%d, i.uv%d.z / "
                                 "select(select(1.0, i.uv%d.z, (i32(P.proj[%d]) >> 4u) == 1), i.uv%d.w, (i32(P.proj[%d]) >> 4u) == 2)))",
                                 smp, smp, crd, crd, crd, crd, crd, crd);
                else if (proj)
                    w = snprintf(ex, WEX, "vec4f(textureSampleCompareLevel(tex%d, smp%d, (%s).xy / (%s).w, (%s).z / (%s).w))",
                                 smp, smp, cs, cs, cs, cs);
                else
                    w = snprintf(ex, WEX, "vec4f(textureSampleCompareLevel(tex%d, smp%d, (%s).xy, (%s).z))", smp, smp, cs, cs);
            } else if (crd >= 0){
                if (cube) w = snprintf(ex, WEX, "textureSample(tex%d, smp%d, i.uv%d.xyz)", smp, smp, crd);
                else      w = snprintf(ex, WEX, "textureSample(tex%d, smp%d, tc%d)", smp, smp, crd);
            } else if (proj){
                w = snprintf(ex, WEX, "textureSample(tex%d, smp%d, (%s).%s / (%s).w)", smp, smp, cs, cube ? "xyz" : "xy", cs);
            } else {
                w = snprintf(ex, WEX, "textureSample(tex%d, smp%d, (%s).%s)", smp, smp, cs, cube ? "xyz" : "xy");
            }
            break; }
        case O_TEXCOORD: {
            /* texcoord tN / texcrd: t[N] = uvN (the Mac uses the destination
             * number for both, also for ps_1_4 texcrd rN, tM) */
            int dn = RN(y->dst) & 3;
            x.t |= 1u << dn;
            wb_put(&bb, "  t%d = i.uv%d;\n", dn, dn);
            continue; }
        case O_TEXKILL: {
            /* texkill's register is its destination token (no sources).
             * D3D9: ps_1_x tests the first three components of texture
             * coordinate set N (texkill tN) or, ps_1_4, of a temp (rN);
             * ps_2_0 and later all four components of the register (as
             * Wine's shader_glsl_texkill).  The Mac's MSL reads the unset
             * source operand instead (vsemu.c), a bug not carried over;
             * NFSU2's shaders have no texkill. */
            int kty = RT(y->dst);
            char kr[64];
            if (kty == R_ADDR && p->version < 0x0200) snprintf(kr, sizeof kr, "i.uv%d", RN(y->dst) & 3);
            else if (kty == R_TEMP || kty == R_ADDR) w_reg(&x, kr, sizeof kr, y->dst, 0);
            else { x.bad = 1; continue; }
            if (p->version >= 0x0200) wb_put(&bb, "  if (any(%s < vec4f(0.0))) { discard; }\n", kr);
            else                      wb_put(&bb, "  if (any(%s.xyz < vec3f(0.0))) { discard; }\n", kr);
            continue; }
        case O_MOV:    w = snprintf(ex, WEX, "%s", s0); break;
        case O_ADD:    w = snprintf(ex, WEX, "(%s) + (%s)", s0, s1); break;
        case O_SUB:    w = snprintf(ex, WEX, "(%s) - (%s)", s0, s1); break;
        case O_MUL:    w = snprintf(ex, WEX, "(%s) * (%s)", s0, s1); break;
        case O_MAD:    w = snprintf(ex, WEX, "(%s) * (%s) + (%s)", s0, s1, s2); break;
        case O_LRP:    w = snprintf(ex, WEX, "mix((%s), (%s), (%s))", s2, s1, s0); break;
        case O_MIN:    w = snprintf(ex, WEX, "min((%s), (%s))", s0, s1); break;
        case O_MAX:    w = snprintf(ex, WEX, "max((%s), (%s))", s0, s1); break;
        case O_DP3:    w = snprintf(ex, WEX, "vec4f(dot((%s).xyz, (%s).xyz))", s0, s1); break;
        case O_DP4:    w = snprintf(ex, WEX, "vec4f(dot((%s), (%s)))", s0, s1); break;
        case O_DP2ADD: w = snprintf(ex, WEX, "vec4f(dot((%s).xy, (%s).xy) + (%s).x)", s0, s1, s2); break;
        case O_RCP:    w = snprintf(ex, WEX, "vec4f(select(1.0 / (%s).x, 3.402823466e+38, (%s).x == 0.0))", s0, s0); break;
        case O_RSQ:    w = snprintf(ex, WEX, "vec4f(select(inverseSqrt(abs((%s).x)), 3.402823466e+38, abs((%s).x) == 0.0))", s0, s0); break;
        case O_FRC:    w = snprintf(ex, WEX, "fract(%s)", s0); break;
        case O_ABS:    w = snprintf(ex, WEX, "abs(%s)", s0); break;
        case O_CMP:    w = snprintf(ex, WEX, "select((%s), (%s), (%s) >= vec4f(0.0))", s2, s1, s0); break;
        case O_CND:    w = snprintf(ex, WEX, "select((%s), (%s), (%s) > vec4f(0.5))", s2, s1, s0); break;
        case O_POW:    w = snprintf(ex, WEX, "vec4f(pow(abs((%s).x), (%s).x))", s0, s1); break;
        case O_NRM:    w = snprintf(ex, WEX, "vec4f(select(normalize((%s).xyz), vec3f(0.0), length((%s).xyz) == 0.0), 1.0)", s0, s0); break;
        case O_EXP:    w = snprintf(ex, WEX, "vec4f(exp2((%s).x))", s0); break;
        /* expp: (2^floor(x), fract(x), 2^x, 1) */
        case O_EXPP:   w = snprintf(ex, WEX, "vec4f(exp2(floor((%s).x)), fract((%s).x), exp2((%s).x), 1.0)", s0, s0, s0); break;
        case O_LOGP:   w = snprintf(ex, WEX,
                           "vec4f(floor(log2(max(abs((%s).x), 1e-38))),"
                           " abs((%s).x) * exp2(-floor(log2(max(abs((%s).x), 1e-38)))),"
                           " log2(max(abs((%s).x), 1e-38)), 1.0)", s0, s0, s0, s0); break;
        case O_LOG:    w = snprintf(ex, WEX, "vec4f(select(log2(abs((%s).x)), -3.4e+38, (%s).x == 0.0))", s0, s0); break;
        default:
            continue;                   /* unsupported: skipped, as on the Mac */
        }
        if (w < 0 || w >= WEX){ x.bad = 1; break; }
        if (!y->has_dst) continue;
        /* destination: same register map as the sources (msl_dst); anything
         * else would not compile on the Mac either -> reject */
        int dty = RT(y->dst);
        if (dty != R_TEMP && dty != R_INPUT && dty != R_ADDR && dty != R_COLOROUT){ x.bad = 1; break; }
        char d[64]; w_reg(&x, d, sizeof d, y->dst, 0);
        int sh = DSHIFT(y->dst);
        char k[48] = "";
        if (sh) w_float(k, sizeof k, ldexpf(1.f, sh));
        if (sh && SATUR(y->dst)) w = snprintf(val, WEX, "saturate((%s) * %s)", ex, k);
        else if (sh)             w = snprintf(val, WEX, "((%s) * %s)", ex, k);
        else if (SATUR(y->dst))  w = snprintf(val, WEX, "saturate(%s)", ex);
        else                     w = snprintf(val, WEX, "%s", ex);
        if (w < 0 || w >= WEX){ x.bad = 1; break; }
        w_store(&bb, d, WMASK(y->dst), val, 0);
        /* the headlight term (W_HL) */
        int dn = RN(y->dst);
        if (p->version < 0x0200 && dty == R_TEMP && (WMASK(y->dst) & 7u) == 7u && !sh && !SATUR(y->dst)){
            if (y->op == O_MUL && ((w_plain(y->src[0], R_ADDR, 1) && w_plain(y->src[1], R_ADDR, 2)) ||
                                   (w_plain(y->src[0], R_ADDR, 2) && w_plain(y->src[1], R_ADDR, 1)))){
                hl_mul = dn; continue; }
            if (y->op == O_MAD && dn == hl_mul && w_plain(y->src[0], R_TEMP, dn) && w_plain(y->src[1], R_CONST, 0)
                && (tcuse & 6u) == 6u){
                wb_put(&bb, "  r%d = vec4f(r%d.xyz + hl_more(i.uv1, tc2.y), r%d.w);\n", dn, dn, dn);
                hl_used = 1; hl_mul = -1; continue; }
        }
        if (dty == R_TEMP && dn == hl_mul) hl_mul = -1;
    }
    if (x.bad || !bb.ok) goto done;

    int usec = ps_uses_colorout(p);
    if (usec) x.oC = 1; else x.r |= 1u;          /* the result register */
    WBuf ob; wb_init(&ob, out, cap);
    wb_put(&ob, "// ps_%d: %s_%u_%u, %u instructions%s%s%s", id, "ps", p->version >> 8, p->version & 0xFF,
           p->n, (cubemask & 1u) ? ", stage 0 cube" : "", hl_used ? ", other cars' headlights" : "", w_st8());
    if (g_wkinds) wb_put(&ob, ", kinds 0x%x", g_wkinds);
    wb_put(&ob, "\n%s%s", w_vout(), W_PSPAR);
    w_textures(&ob, cubemask, dmask);
    if (hl_used) wb_put(&ob, "%s", W_HL);
    wb_put(&ob, "@fragment fn ps_%d(i: VOut) -> @location(0) vec4f {\n", id);
    w_decls(&ob, &x, 16);
    if (x.v & 1u) wb_put(&ob, "  var v0 = i.col0;\n");
    if (x.v & 2u) wb_put(&ob, "  var v1 = i.col1;\n");
    for (int t = 0; t < 8; t++) if (x.t & (1u << t)) wb_put(&ob, "  var t%d = vec4f(0.0);\n", t);
    if (x.oC) wb_put(&ob, "  var oC = vec4f(0.0);\n");
    wb_put(&ob, "%s", bodybuf);
    wb_put(&ob, "  return ps_end(%s, i.fog);\n}\n", usec ? "oC" : "r0");
    if (ob.ok) rc = (int)ob.n;
done:
    free(bodybuf); free(ex); free(s0); free(s1); free(s2); free(val);
    return rc;
}
int ps_to_wgsl_ex(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask){
    return ps_to_wgsl_ex2(p, id, out, cap, cubemask, -1);
}
int ps_to_wgsl(const VSProg *p, int id, char *out, size_t cap){
    return ps_to_wgsl_ex(p, id, out, cap, 0u);
}

/* ====================================================================
 * Vertex shader (vs_to_msl)
 * ==================================================================== */

/* D3DDECLTYPE -> WGSL expression reading at byte address B + off.  Must match
 * com.c decl_fetch (the CPU path) bit for bit -- the Mac's decl_read_msl only
 * gets within an ulp, its divisions run under Metal fast math -- and reject
 * the same types, so the draw takes the CPU path. */
static int decl_read_wgsl(char *o, size_t cap, unsigned type, unsigned off){
    switch (type){
    case 0: snprintf(o,cap,"vec4f(ldf(B + %uu), 0.0, 0.0, 1.0)",off); break;                       /* FLOAT1 */
    case 1: snprintf(o,cap,"vec4f(ldf(B + %uu), ldf(B + %uu), 0.0, 1.0)",off,off+4); break;          /* FLOAT2 */
    case 2: snprintf(o,cap,"vec4f(ldf(B + %uu), ldf(B + %uu), ldf(B + %uu), 1.0)",
                     off,off+4,off+8); break;                                                         /* FLOAT3 */
    case 3: snprintf(o,cap,"vec4f(ldf(B + %uu), ldf(B + %uu), ldf(B + %uu), ldf(B + %uu))",
                     off,off+4,off+8,off+12); break;                                                  /* FLOAT4 */
    case 4: snprintf(o,cap,"ldcolor(B + %uu)",off); break;                                           /* D3DCOLOR */
    case 5: snprintf(o,cap,"ldub4(B + %uu)",off); break;                                             /* UBYTE4 */
    case 8: snprintf(o,cap,"n8(ldub4(B + %uu))",off); break;                                        /* UBYTE4N */
    case 6: snprintf(o,cap,"vec4f(lds(B + %uu), lds(B + %uu), 0.0, 1.0)",off,off+2); break;          /* SHORT2 */
    case 7: snprintf(o,cap,"vec4f(lds(B + %uu), lds(B + %uu), lds(B + %uu), lds(B + %uu))",
                     off,off+2,off+4,off+6); break;                                                   /* SHORT4 */
    case 9: snprintf(o,cap,"vec4f(s16n(vec4f(lds(B + %uu), lds(B + %uu), 0.0, 0.0)).xy, 0.0, 1.0)",
                     off,off+2); break;                                                               /* SHORT2N */
    case 10:snprintf(o,cap,"s16n(vec4f(lds(B + %uu), lds(B + %uu), lds(B + %uu), lds(B + %uu)))",
                     off,off+2,off+4,off+6); break;                                                   /* SHORT4N */
    case 11:snprintf(o,cap,"vec4f(u16n(vec4f(ldu(B + %uu), ldu(B + %uu), 0.0, 0.0)).xy, 0.0, 1.0)",
                     off,off+2); break;                                                               /* USHORT2N */
    case 12:snprintf(o,cap,"u16n(vec4f(ldu(B + %uu), ldu(B + %uu), ldu(B + %uu), ldu(B + %uu)))",
                     off,off+2,off+4,off+6); break;                                                   /* USHORT4N */
    default: return 0;
    }
    return 1;
}

/* Destination of a vertex shader write (vs_dst_msl): oT4..7 alias oT0..3 and
 * unknown registers go to a scratch variable, exactly as on the Mac. */
static int w_vsdst(WCtx *x, char *o, size_t cap, unsigned tok){
    int ty = RT(tok), n = RN(tok);
    switch (ty){
    case R_TEMP:    n &= 31; x->r |= 1u << n; snprintf(o, cap, "r%d", n); return 0;
    case R_ADDR:    x->a0 = 1; snprintf(o, cap, "a0"); return 1;
    case R_RASTOUT: if (n == 0){ snprintf(o, cap, "oPos"); return 0; }
                    if (n == 1){ snprintf(o, cap, "oFog"); return 0; }
                    x->dummy = 1; snprintf(o, cap, "dummy"); return 0;
    case R_ATTROUT: snprintf(o, cap, "oD%d", n & 1); return 0;
    case R_OUTPUT:  snprintf(o, cap, "oT%d", n & (int)(g_wstages - 1)); return 0;   /* 4 stages: oT4..7 alias oT0..3 */
    default:        x->dummy = 1; snprintf(o, cap, "dummy"); return 0;
    }
}

int vs_to_wgsl(const VSProg *p, int id, const int decl[16][2], char *out, size_t cap){
    if (!p || !out || cap < 1024) return 0;
    if (p->raw) return vs_to_wgsl_sm3(p, id, decl, out, cap);
    if (!w_defs_finite(p)) return 0;
    WCtx x; memset(&x, 0, sizeof x); x.p = p; x.vs = 1;
    char *bodybuf = malloc(cap);
    if (!bodybuf) return 0;
    WBuf bb; wb_init(&bb, bodybuf, cap);
    char *ex = malloc(WEX), *s0 = malloc(WEX), *s1 = malloc(WEX), *s2 = malloc(WEX), *val = malloc(WEX);
    int rc = 0;
    if (!ex || !s0 || !s1 || !s2 || !val) goto done;

    /* inputs from the declaration (baked offsets and types) */
    unsigned vdecl = 0;
    for (int i = 0; i < 16; i++){
        if (decl[i][0] < 0) continue;
        char e[256];
        if (!decl_read_wgsl(e, sizeof e, (unsigned)decl[i][1], (unsigned)decl[i][0])) goto done;
        wb_put(&bb, "  let v%d = %s;\n", i, e);
        vdecl |= 1u << i;
    }

    for (unsigned q = 0; q < p->n && bb.ok && !x.bad; q++){
        const VSIns *y = &p->ins[q];
        if (y->nsrc > 0) w_src(&x, s0, WEX, y->src[0], y->rel[0]);
        if (y->nsrc > 1) w_src(&x, s1, WEX, y->src[1], y->rel[1]);
        if (y->nsrc > 2) w_src(&x, s2, WEX, y->src[2], y->rel[2]);
        int w = 0;
        ex[0] = 0;
        switch (y->op){
        case O_NOP: case O_DCL: case O_DEF: case O_DEFI: case O_DEFB:
        case O_LABEL: case O_END: continue;
        case O_MOV:  w = snprintf(ex, WEX, "%s", s0); break;
        case O_ADD:  w = snprintf(ex, WEX, "(%s) + (%s)", s0, s1); break;
        case O_SUB:  w = snprintf(ex, WEX, "(%s) - (%s)", s0, s1); break;
        case O_MUL:  w = snprintf(ex, WEX, "(%s) * (%s)", s0, s1); break;
        case O_MAD:  w = snprintf(ex, WEX, "(%s) * (%s) + (%s)", s0, s1, s2); break;
        case O_MIN:  w = snprintf(ex, WEX, "min((%s), (%s))", s0, s1); break;
        case O_MAX:  w = snprintf(ex, WEX, "max((%s), (%s))", s0, s1); break;
        case O_SLT:  w = snprintf(ex, WEX, "select(vec4f(0.0), vec4f(1.0), (%s) < (%s))", s0, s1); break;
        case O_SGE:  w = snprintf(ex, WEX, "select(vec4f(0.0), vec4f(1.0), (%s) >= (%s))", s0, s1); break;
        case O_DP3:  w = snprintf(ex, WEX, "vec4f(dot((%s).xyz, (%s).xyz))", s0, s1); break;
        case O_DP4:  w = snprintf(ex, WEX, "vec4f(dot((%s), (%s)))", s0, s1); break;
        case O_LRP:  w = snprintf(ex, WEX, "mix((%s), (%s), (%s))", s2, s1, s0); break;
        case O_FRC:  w = snprintf(ex, WEX, "fract(%s)", s0); break;
        case O_ABS:  w = snprintf(ex, WEX, "abs(%s)", s0); break;
        case O_RCP:  w = snprintf(ex, WEX, "vec4f(select(1.0 / (%s).x, 3.402823466e+38, (%s).x == 0.0))", s0, s0); break;
        case O_RSQ:  w = snprintf(ex, WEX, "vec4f(select(inverseSqrt(abs((%s).x)), 3.402823466e+38, abs((%s).x) == 0.0))", s0, s0); break;
        case O_EXP:  w = snprintf(ex, WEX, "vec4f(exp2((%s).x))", s0); break;
        case O_EXPP: w = snprintf(ex, WEX, "vec4f(exp2(floor((%s).x)), fract((%s).x), exp2((%s).x), 1.0)", s0, s0, s0); break;
        /* vertex logp is the one-component log on the Mac */
        case O_LOG: case O_LOGP:
                     w = snprintf(ex, WEX, "vec4f(select(log2(abs((%s).x)), -3.4e+38, (%s).x == 0.0))", s0, s0); break;
        case O_POW:  w = snprintf(ex, WEX, "vec4f(pow(abs((%s).x), (%s).x))", s0, s1); break;
        case O_CRS:  w = snprintf(ex, WEX, "vec4f(cross((%s).xyz, (%s).xyz), 1.0)", s0, s1); break;
        case O_NRM:  w = snprintf(ex, WEX, "vec4f(select(normalize((%s).xyz), vec3f(0.0), length((%s).xyz) == 0.0), 1.0)", s0, s0); break;
        case O_SGN:  w = snprintf(ex, WEX, "sign(%s)", s0); break;
        case O_M4x4: case O_M4x3: case O_M3x3: case O_M3x4: case O_M3x2: {
            /* m?x? dst, src0, src1: rows are consecutive registers from src1
             * (as on the Mac: K.c rows ignore def and relative addressing) */
            int rows = (y->op==O_M4x4) ? 4 : (y->op==O_M4x3 ? 3 :
                       (y->op==O_M3x3 ? 3 : (y->op==O_M3x4 ? 4 : 2)));
            int vec3 = (y->op==O_M3x3 || y->op==O_M3x4 || y->op==O_M3x2);
            const char *sw2 = vec3 ? ".xyz" : "";
            WBuf eb; wb_init(&eb, ex, WEX);
            wb_put(&eb, "vec4f(");
            for (int rr = 0; rr < rows; rr++){
                unsigned tok2 = y->src[1];
                int ty2 = RT(tok2), nn2 = RN(tok2) + rr;
                if (ty2 == R_CONST2) nn2 += 2048; else if (ty2 == R_CONST3) nn2 += 4096;
                else if (ty2 == R_CONST4) nn2 += 6144;
                char row[48];
                if (ty2 == R_TEMP){ x.r |= 1u << (nn2 & 31); snprintf(row, sizeof row, "r%d", nn2 & 31); }
                else if (ty2 == R_INPUT){ x.v |= 1u << (nn2 & 15); snprintf(row, sizeof row, "v%d", nn2 & 15); }
                else snprintf(row, sizeof row, "K.c[kb + %du]", (nn2 & 255) + 1);
                wb_put(&eb, "%sdot((%s)%s, (%s)%s)", rr ? ", " : "", s0, sw2, row, sw2);
            }
            for (int rr = rows; rr < 4; rr++) wb_put(&eb, ", %s", rr == 3 ? "1.0" : "0.0");
            wb_put(&eb, ")");
            w = eb.ok ? (int)eb.n : -1;
            break; }
        case O_SINCOS: w = snprintf(ex, WEX, "vec4f(cos((%s).x), sin((%s).x), 0.0, 0.0)", s0, s0); break;
        case O_MOVA:   w = snprintf(ex, WEX, "%s", s0); break;
        case O_LIT:    w = snprintf(ex, WEX,
                           "vec4f(1.0, max((%s).x, 0.0), select(0.0, pow(max((%s).y, 0.0),"
                           " clamp((%s).w, -127.9, 127.9)), (%s).x > 0.0), 1.0)", s0, s0, s0, s0); break;
        case O_DST:    w = snprintf(ex, WEX, "vec4f(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", s0, s1, s0, s1); break;
        default:
            /* flow control, calls, ...: rejected, the draw keeps the CPU path */
            goto done;
        }
        if (w < 0 || w >= WEX){ x.bad = 1; break; }
        if (!y->has_dst) continue;
        if (y->op == O_MOVA){
            /* MSL round() rounds half away from zero; WGSL round() is
             * half-to-even, so spell the Mac's rounding out.  The mask is
             * ignored as on the Mac. */
            x.a0 = 1;
            wb_put(&bb, "  { let e = %s; a0 = vec4i(sign(e) * floor(abs(e) + 0.5)); }\n", ex);
            continue;
        }
        char d[64]; int toint = w_vsdst(&x, d, sizeof d, y->dst);
        if (SATUR(y->dst)) w = snprintf(val, WEX, "saturate(%s)", ex);
        else               w = snprintf(val, WEX, "%s", ex);
        if (w < 0 || w >= WEX){ x.bad = 1; break; }
        /* a0 writes: float -> int truncates toward zero, as the Mac's MSL
         * implicit conversion */
        w_store(&bb, d, WMASK(y->dst), val, toint);
    }
    if (x.bad || !bb.ok) goto done;

    WBuf ob; wb_init(&ob, out, cap);
    wb_put(&ob, "// vs_%d: vs_%u_%u, %u instructions, vs_kmax %d%s\n", id, p->version >> 8, p->version & 0xFF,
           p->n, vs_kmax(p), w_st8());
    wb_put(&ob, "%s%s%s%s", w_vout(), W_VSPAR, W_PULL, W_TPICK);
    wb_put(&ob, "@vertex fn vs_%d(@builtin(vertex_index) vid: u32, @builtin(instance_index) iid: u32) -> VOut {\n", id);
    /* kb <= GW_KWIN_ROWS - 257 (the renderer's window choice): every row
     * below, relative addressing's clamp included, stays inside the window */
    wb_put(&ob, "  let kb = iid;\n"
                "  let KH = bitcast<vec4u>(K.c[kb]);\n"
                "  let B = KH.z + vid * KH.x;\n");
    w_decls(&ob, &x, 32);
    for (int i = 0; i < 16; i++)
        if ((x.v & (1u << i)) && !(vdecl & (1u << i)))
            wb_put(&ob, "  let v%d = vec4f(0.0, 0.0, 0.0, 1.0);\n", i);
    /* oFog defaults to 1 = no fog when the shader does not write it */
    wb_put(&ob, "  var oPos = vec4f(0.0, 0.0, 0.0, 1.0);\n"
                "  var oD0 = vec4f(1.0); var oD1 = vec4f(0.0);\n"
                "  var oT0 = vec4f(0.0); var oT1 = vec4f(0.0); var oT2 = vec4f(0.0); var oT3 = vec4f(0.0);\n"
                "  var oFog = vec4f(1.0);\n");
    if (g_wstages == 8)
        wb_put(&ob, "  var oT4 = vec4f(0.0); var oT5 = vec4f(0.0); var oT6 = vec4f(0.0); var oT7 = vec4f(0.0);\n");
    wb_put(&ob, "%s", bodybuf);
    /* Epilogue as the Mac: no half-pixel offset, no colour clamp;
     * D3DTSS_TEXCOORDINDEX applied here so the pixel stage reads plain
     * interpolants. */
    wb_put(&ob, "  var o: VOut;\n"
                "  o.pos = oPos;\n"
                "  o.col0 = oD0; o.col1 = oD1;\n"
                "  o.uv0 = tpick(KH.y & 3u, oT0, oT1, oT2, oT3);\n"
                "  o.uv1 = tpick((KH.y >> 8u) & 3u, oT0, oT1, oT2, oT3);\n"
                "  o.uv2 = tpick((KH.y >> 16u) & 3u, oT0, oT1, oT2, oT3);\n"
                "  o.uv3 = tpick((KH.y >> 24u) & 3u, oT0, oT1, oT2, oT3);\n"
                "  o.fog = oFog.x;\n");
    /* 8 stages: uv4..uv7 are oT4..oT7 as they are (tsel covers stages 0-3) */
    if (g_wstages == 8) wb_put(&ob, "  o.uv4 = oT4; o.uv5 = oT5; o.uv6 = oT6; o.uv7 = oT7;\n");
    wb_put(&ob, "  return o;\n}\n");
    if (ob.ok) rc = (int)ob.n;
done:
    free(bodybuf); free(ex); free(s0); free(s1); free(s2); free(val);
    return rc;
}

/* ====================================================================
 * TVert vertex stage and fixed-function pixel stage (metal.m kShaderSrc:
 * vmain / fmain / fmain_nd).  Same ABI as the game shaders.
 * ==================================================================== */
int wgsl_tvert_module(char *out, size_t cap){
    WBuf ob; wb_init(&ob, out, cap);
    /* 8 stages: uv4..uv7 stay 0 (a TVert holds four coordinates) */
    wb_put(&ob, "// vs_0: TVert pass-through (CPU-transformed vertices, metal.m vmain)%s\n", w_st8());
    wb_put(&ob, "%s%s", w_vout(), W_VSPAR);
    /* TVert (gfx.h): pos xyzw, oD0, oD1, t[4][4]: 28 f32 = 112 bytes.  The
     * renderer draws with firstVertex = MDraw.first from the start of the
     * TVert arena: no K block. */
    wb_put(&ob, "fn ld4(w: u32) -> vec4f { return bitcast<vec4f>(vec4u(VB[w], VB[w + 1u], VB[w + 2u], VB[w + 3u])); }\n");
    wb_put(&ob, "@vertex fn vs_0(@builtin(vertex_index) vid: u32) -> VOut {\n"
                "  let w = vid * 28u;\n"
                "  var o: VOut;\n"
                "  o.pos = ld4(w);\n"
                "  o.col0 = ld4(w + 4u); o.col1 = ld4(w + 8u);\n"
                "  o.uv0 = ld4(w + 12u); o.uv1 = ld4(w + 16u); o.uv2 = ld4(w + 20u); o.uv3 = ld4(w + 24u);\n"
                "  o.fog = 1.0;\n"
                "  return o;\n}\n");
    return ob.ok ? (int)ob.n : 0;
}

int wgsl_ff_module(int id, char *out, size_t cap, unsigned cubemask){
    WBuf ob; wb_init(&ob, out, cap);
    int cube = cubemask & 1u;
    wb_put(&ob, "// ps_%d: fixed-function fragment col0 * tex0 (metal.m fmain)%s%s\n", id, cube ? ", stage 0 cube" : "", w_st8());
    wb_put(&ob, "%s%s", w_vout(), W_PSPAR);
    w_textures(&ob, cubemask, 0u);
    wb_put(&ob, "@fragment fn ps_%d(i: VOut) -> @location(0) vec4f {\n"
                "  return ps_end(i.col0 * textureSample(tex0, smp0, i.uv0.%s), i.fog);\n}\n",
           id, cube ? "xyz" : "xy");
    return ob.ok ? (int)ob.n : 0;
}

/* Compute module for the vertex-pulling test (web/src/test/shadercheck.ts):
 * for every supported declaration type t (0..12) and byte offset o (0..7) it
 * writes OUT[t * 8 + o] = decode(t, PT.base + o) with the same helpers and the
 * same decl_read_wgsl() text the vertex shaders use. */
int wgsl_pull_test_module(char *out, size_t cap){
    WBuf ob; wb_init(&ob, out, cap);
    wb_put(&ob, "// vertex-pulling test: OUT[type * 8 + offset] = decode(type, PT.x + offset)\n");
    wb_put(&ob, "@group(0) @binding(0) var<storage, read> VB: array<u32>;\n"
                "@group(0) @binding(1) var<storage, read_write> OUT: array<vec4f>;\n"
                "@group(0) @binding(2) var<uniform> PT: vec4u;\n");
    wb_put(&ob, "%s", W_PULL);
    wb_put(&ob, "@compute @workgroup_size(1) fn main() {\n  let B = PT.x;\n");
    for (unsigned t = 0; t <= 12; t++)
        for (unsigned o = 0; o < 8; o++){
            char e[256];
            if (decl_read_wgsl(e, sizeof e, t, o)) wb_put(&ob, "  OUT[%u] = %s;\n", t * 8 + o, e);
        }
    wb_put(&ob, "}\n");
    return ob.ok ? (int)ob.n : 0;
}

/* ps_2_x, ps_3_0 and vs_3_0 */
#include "vsemu_sm3.inc"

#ifdef VSEMU_WGSL
/* reference interpreter for the pixel shader differential test (tools only) */
#include "vsemu_psref.inc"
#endif
