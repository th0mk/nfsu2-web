/* Offline WGSL translation of D3D9 shader binaries with the game's own
 * translator (runtime/vsemu.c built natively with -DVSEMU_WGSL), for checking
 * the emitter without running the game:
 *
 *   cc -O1 -DVSEMU_WGSL -DNFS_ENHANCED=0 -Iruntime -Iruntime/web -Iweb/tools \
 *      runtime/vsemu.c web/tools/wgsl-xl.c web/tools/psref.c -lm -o out/wgsl-xl
 *   out/wgsl-xl [--stages 8] <outdir> shader.bin...
 *   out/wgsl-xl [--stages 8] --pairs <dumpdir> <outdir>
 *   out/wgsl-xl [--stages 8] --fx <outdir> effect.fxo...
 *   node web/tools/shader-check.mjs --from <outdir>
 *
 * --stages 8 translates for 8 texture stages (gpu-abi.md section 11.2: the
 * modules Most Wanted needs).  --fx takes every shader of compiled effects
 * (Most Wanted's, from tools/fxres.py: each a size dword, then the program
 * from its version token to its end token), distinct by bytes.
 *
 * ps_2_x, ps_3_0 and vs_3_0 get vectors from web/tools/psref.c, the D3D9
 * reference (write_sm3_ps_vectors, write_sm3_vs_vectors): ps_<id>.p3vec and
 * vs_<id>.v3vec, which web/src/test/shadercheck.ts runs against the WGSL on
 * the GPU.  The other models keep the Mac-semantics oracles below (.vec,
 * .pvec: vs_run, ps_ref_run), which follow the 8-stage rules too: with 8
 * stages every module of every mode gets them ('VSV2', 'PSV2'); with 4 only
 * --pairs writes them, as before.
 *
 * --pairs takes an in-game dump (NFSU2_SHADER_DUMP) and translates every
 * distinct vertex shader object against every distinct vertex declaration the
 * game created that supplies all of the shader's inputs, mapped exactly as
 * com.c draw_up/gpuvs_id do (stream 0, vs_input_reg).  That covers the pairs a
 * single test drive never draws; the modules get ids from 1001 (index.txt
 * says which object and declaration each one is).  It also writes
 * differential test vectors (vs_<id>.vec, see write_vectors) for these
 * modules and for the game's own pairs in the dump, and pixel shader vectors
 * (ps_<id>.pvec, see write_ps_vectors) for every dumped pixel shader.
 *
 * The output uses the in-game dump's file names (vs_<n>.wgsl, ps_<n>.wgsl,
 * ps_<n+256>.wgsl for the cube twin, vs_0/ps_0/ps_256 built-ins,
 * pulltest.wgsl, index.txt).  A vertex shader gets a synthetic declaration
 * (one element per dcl'd input with a typical NFSU2 type), so this checks the
 * shader bodies; the real (shader, declaration) pairs come from the in-game
 * dump (NFSU2_SHADER_DUMP, docs/web/gpu-abi.md section 9). */
#include "vsemu.h"
#include "wgsl.h"
#include "psref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <math.h>

static char g_buf[256 * 1024];
/* the SM3-class version tokens, whose oracle is psref */
/* --fx with 8 stages: each pixel shader's depth variant too (every 2D stage a depth stage, gpu-abi.md 11.4):
 * translated and counted, not written (the game asks for them at its draws, gpu_web.c ps_depth) */
static int g_dv, g_dv_fail, g_dv_depth;
static int is_sm3(uint32_t v){ return v == PSREF_PS_2_X || v == PSREF_PS_3_0 || v == PSREF_VS_3_0; }
static int write_sm3_ps_vectors(const char *dir, int id, const uint32_t *tok, size_t ndw);
static int write_sm3_vs_vectors(const char *dir, int id, const uint32_t *tok, size_t ndw, const int dcl[16][2]);

static void put(const char *dir, const char *name, const char *text, size_t n){
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    fwrite(text, 1, n, f); fclose(f);
}

static unsigned char *slurp(const char *path, size_t *n){
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)L + 4);
    if (b && fread(b, 1, (size_t)L, f) != (size_t)L){ free(b); b = 0; }
    fclose(f); *n = (size_t)L;
    return b;
}


/* ---- differential test vectors: the CPU interpreter is the reference ----
 * vs_<id>.vec (little-endian): u32 magic 'VSV1', nverts, stride, writes_fog,
 * then nverts*stride vertex bytes, 256*4 f32 constants (K.c), and per vertex
 * 29 f32 expected outputs: pos, col0, col1, uv0..uv3 (TEXCOORDINDEX = identity),
 * fog.  The expected values come from vs_run on inputs decoded by a copy of
 * com.c decl_fetch, i.e. the Mac's CPU vertex path.  The shader check runs the
 * WGSL vertex function as a compute shader on the same bytes and compares.
 * With 8 stages the magic is 'VSV2' and each vertex has 45: uv4..uv7 (oT4..oT7,
 * which vs_run keeps apart) after the fog. */
static uint32_t g_rng;
static uint32_t rnd(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float rndf(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() >> 8) / 16777216.f; }

/* copy of com.c decl_fetch (static there), reading from a byte buffer */
static float rf(const unsigned char *b){ float f; memcpy(&f, b, 4); return f; }
static int16_t rs(const unsigned char *b){ return (int16_t)(b[0] | b[1] << 8); }
static uint16_t ru(const unsigned char *b){ return (uint16_t)(b[0] | b[1] << 8); }
static void decl_fetch(const unsigned char *a, unsigned type, float out[4]){
    out[0]=out[1]=out[2]=0.f; out[3]=1.f;
    switch (type){
    case 0: out[0]=rf(a); break;
    case 1: out[0]=rf(a); out[1]=rf(a+4); break;
    case 2: out[0]=rf(a); out[1]=rf(a+4); out[2]=rf(a+8); break;
    case 3: out[0]=rf(a); out[1]=rf(a+4); out[2]=rf(a+8); out[3]=rf(a+12); break;
    case 4: out[0]=a[2]/255.f; out[1]=a[1]/255.f; out[2]=a[0]/255.f; out[3]=a[3]/255.f; break;
    case 5: out[0]=a[0]; out[1]=a[1]; out[2]=a[2]; out[3]=a[3]; break;
    case 8: out[0]=a[0]/255.f; out[1]=a[1]/255.f; out[2]=a[2]/255.f; out[3]=a[3]/255.f; break;
    case 6: out[0]=rs(a); out[1]=rs(a+2); break;
    case 7: out[0]=rs(a); out[1]=rs(a+2); out[2]=rs(a+4); out[3]=rs(a+6); break;
    case 9: out[0]=rs(a)/32767.f; out[1]=rs(a+2)/32767.f; break;
    case 10:out[0]=rs(a)/32767.f; out[1]=rs(a+2)/32767.f; out[2]=rs(a+4)/32767.f; out[3]=rs(a+6)/32767.f; break;
    case 11:out[0]=ru(a)/65535.f; out[1]=ru(a+2)/65535.f; break;
    case 12:out[0]=ru(a)/65535.f; out[1]=ru(a+2)/65535.f; out[2]=ru(a+4)/65535.f; out[3]=ru(a+6)/65535.f; break;
    default: break;
    }
}
static const int DSIZE[13] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8 };

static void write_vectors(const char *dir, int id, const VSProg *p, const int dcl[16][2]){
    enum { NV = 64 };
    int stride = 4;
    for (int i = 0; i < 16; i++)
        if (dcl[i][0] >= 0 && dcl[i][1] >= 0 && dcl[i][1] < 13 && dcl[i][0] + DSIZE[dcl[i][1]] > stride)
            stride = dcl[i][0] + DSIZE[dcl[i][1]];
    stride = (stride + 3) & ~3;
    g_rng = 0x9E3779B9u ^ (uint32_t)id * 2654435761u;
    unsigned char *vb = calloc(NV, (size_t)stride);
    /* plausible values per element type: floats in [0, 4) keep relative
     * constant indices (a0 = floor(v) * 4 + n in WORLDBONE) inside c0..c255,
     * where the interpreter (out of range -> 0) and the GPU (clamp) agree */
    for (int v = 0; v < NV; v++)
        for (int i = 0; i < 16; i++){
            if (dcl[i][0] < 0) continue;
            unsigned char *a = vb + (size_t)v * stride + dcl[i][0];
            int t = dcl[i][1];
            if (t <= 3) for (int c = 0; c <= t; c++){ float f = rndf(0.f, 4.f); memcpy(a + 4*c, &f, 4); }
            else for (int c = 0; c < DSIZE[t]; c++) a[c] = (unsigned char)rnd();
        }
    static VSConst k; memset(&k, 0, sizeof k);
    for (int r = 0; r < 256; r++) for (int c = 0; c < 4; c++) k.f[r][c] = rndf(-1.f, 1.f);
    int wf = vs_writes_fog(p), s8 = wgsl_get_stages() == 8, no = s8 ? 45 : 29;
    char path[1024]; snprintf(path, sizeof path, "%s/vs_%d.vec", dir, id);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    uint32_t hdr[4] = { s8 ? 0x32565356u : 0x31565356u, NV, (uint32_t)stride, (uint32_t)wf };
    fwrite(hdr, 4, 4, f);
    fwrite(vb, (size_t)stride, NV, f);
    fwrite(k.f, 16, 256, f);
    for (int v = 0; v < NV; v++){
        float in[16][4];
        for (int r = 0; r < 16; r++){ in[r][0] = in[r][1] = in[r][2] = 0.f; in[r][3] = 1.f; }
        for (int i = 0; i < 16; i++)
            if (dcl[i][0] >= 0) decl_fetch(vb + (size_t)v * stride + dcl[i][0], (unsigned)dcl[i][1], in[i]);
        VSOut o; vs_run(p, &k, (const float (*)[4])in, &o);
        float e[45];
        memcpy(e, o.pos, 16); memcpy(e + 4, o.col[0], 16); memcpy(e + 8, o.col[1], 16);
        memcpy(e + 12, o.tex[0], 64);
        e[28] = wf ? o.fog : 1.f;          /* GPU semantics: unwritten oFog = 1 (no fog) */
        memcpy(e + 29, o.tex[4], 64);      /* 8 stages: oT4..oT7 (vs_run keeps all eight) */
        fwrite(e, 4, (size_t)no, f);
    }
    fclose(f); free(vb);
}


/* ---- pixel shader vectors: ps_<id>.pvec, oracle = ps_ref_run ----
 * u32 magic 'PSV1', npix, cubemask, 0; the 560-byte PSPar block; per pixel 25
 * f32 inputs (col0, col1, uv0..uv3, fog); per pixel 5 f32 expected outputs
 * (rgba after alpha test and fog, killed 0/1).  Textures are an analytic
 * function of the coordinate (ftex below; the shader check substitutes the
 * same function for textureSample).  With 8 stages the magic is 'PSV2', each
 * pixel has 41 inputs (uv4..uv7 after the fog), and the shader check takes
 * each binding's kind from the module (ps_ref_run adds ps_2_0's declared cubes
 * to cubemask itself). */
static void ftex(int k, const float *c, int n, float o[4]){
    float x = c[0], y = c[1], f = (float)k;
    if (n == 3){ x = c[0] + c[2] * 0.3f; y = c[1] - c[2] * 0.2f; }
    o[0] = 0.5f + 0.4f * sinf(x * 3.1f + f * 0.37f);
    o[1] = 0.5f + 0.4f * cosf(y * 2.3f + f * 0.5f);
    o[2] = 0.5f + 0.4f * sinf((x + y) * 1.7f + f);
    o[3] = 0.5f + 0.4f * cosf(x * y * 5.3f + 0.2f * f);
}
static int write_ps_vectors(const char *dir, int id, const VSProg *p, unsigned cubemask){
    enum { NP = 256 };
    int s8 = wgsl_get_stages() == 8, nin = s8 ? 41 : 25;
    g_rng = 0x85EBCA6Bu ^ (uint32_t)id * 2246822519u;
    static PSRefPar P; memset(&P, 0, sizeof P);
    P.aref = rndf(0.f, 0.5f);
    for (int k = 0; k < 4; k++) P.proj[k] = (float)(k + 16 * (int)(rnd() % 3u));
    /* 8 stages (Most Wanted): material constants positive, as for psref's vectors; its shaders raise a saturated
     * dot product to c2.x, and a power <= 0 of 0 is infinite (WGSL leaves it open) */
    for (int r = 0; r < 32; r++) for (int c = 0; c < 4; c++) P.c[r][c] = s8 ? rndf(0.05f, 1.f) : rndf(-1.f, 1.f);
    for (int c = 0; c < 3; c++) P.fog[c] = rndf(0.f, 1.f);
    P.fog[3] = (float)(id & 1);
    char path[1024]; snprintf(path, sizeof path, "%s/ps_%d.pvec", dir, id);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    uint32_t hdr[4] = { s8 ? 0x32565350u : 0x31565350u, NP, cubemask, 0 };
    fwrite(hdr, 4, 4, f);
    float blk[140] = { 0 };                    /* PSPar: aref usec pad pad, proj, c[32], fog */
    blk[0] = P.aref; memcpy(blk + 4, P.proj, 16); memcpy(blk + 8, P.c, 512); memcpy(blk + 136, P.fog, 16);
    fwrite(blk, 4, 140, f);
    static PSRefIn in[NP];
    for (int i = 0; i < NP; i++){
        /* colours a little outside [0, 1] (the Mac does not clamp oD0/oD1)
         * so _sat, cmp and cnd see both sides of their thresholds */
        for (int c = 0; c < 4; c++){ in[i].col[0][c] = rndf(-0.25f, 1.25f); in[i].col[1][c] = rndf(-0.25f, 1.25f); }
        for (int k = 0; k < 4; k++){
            in[i].uv[k][0] = rndf(0.f, 1.f); in[i].uv[k][1] = rndf(0.f, 1.f);
            in[i].uv[k][2] = rndf(0.5f, 2.f); in[i].uv[k][3] = rndf(0.5f, 2.f);
        }
        in[i].fog = rndf(-0.2f, 1.2f);
        for (int k = 4; k < (s8 ? 8 : 4); k++){
            in[i].uv[k][0] = rndf(0.f, 1.f); in[i].uv[k][1] = rndf(0.f, 1.f);
            in[i].uv[k][2] = rndf(0.5f, 2.f); in[i].uv[k][3] = rndf(0.5f, 2.f);
        }
        float row[41];
        memcpy(row, in[i].col, 32); memcpy(row + 8, in[i].uv, 64); row[24] = in[i].fog;
        memcpy(row + 25, in[i].uv[4], 64);
        fwrite(row, 4, (size_t)nin, f);
    }
    int ok = 1;
    for (int i = 0; i < NP; i++){
        float o[5] = { 0, 0, 0, 0, 0 }; int killed = 0;
        if (!ps_ref_run(p, &P, &in[i], ftex, cubemask, o, &killed)) ok = 0;   /* refused (ps_2_x, ps_3_0): zeros */
        o[4] = (float)killed;
        fwrite(o, 4, 5, f);
    }
    fclose(f);
    return ok;
}

/* ---- ps_2_x, ps_3_0 and vs_3_0: oracle = psref (web/tools/psref.c) ----
 * The texture is an analytic function of the coordinate and the level of
 * detail; shadercheck.ts puts the same function (in f32) in place of each
 * textureSample* call.  It uses only products, sums and floor, which a GPU
 * rounds as C does (its sin is allowed an error of 2^-11, enough to flip a
 * shadow shader's depth comparison), and a bump that is continuous where its
 * floor jumps.  lod: textureSampleLevel's level; else from the gradients
 * (implicit: the coordinate's fine derivatives; texldd: its own), log2 of 256
 * times their largest x or y component (-16 for none), plus texldb's bias. */
static float bump(float t){ float u = t - floorf(t); return 4.f * u * (1.f - u); }
static int sm3_tex(void *user, const PsrefSample *s, float o[4]){
    (void)user;
    float x = s->c[0], y = s->c[1], f = (float)s->stage, lod;
    if (s->mode == PSREF_LOD_LEVEL) lod = s->lod;
    else {
        float g = fmaxf(fmaxf(fabsf(s->dx[0]), fabsf(s->dx[1])), fmaxf(fabsf(s->dy[0]), fabsf(s->dy[1]))) * 256.f;
        lod = g > 0.f ? log2f(g) : -16.f;
        if (s->mode == PSREF_LOD_BIAS) lod += s->lod;
    }
    if (s->type == 3){ x = s->c[0] + s->c[2] * 0.3f; y = s->c[1] - s->c[2] * 0.2f; }
    o[0] = 0.1f + 0.8f * bump(x * 3.1f + f * 0.37f + 0.05f * lod);
    o[1] = 0.1f + 0.8f * bump(y * 2.3f + f * 0.5f);
    o[2] = 0.1f + 0.8f * bump((x + y) * 1.7f + f * 0.13f);
    o[3] = 0.1f + 0.8f * bump(x * y * 5.3f + 0.2f * f);
    return 1;
}
static uint32_t fnv(const void *b, size_t n){
    const unsigned char *c = b; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++){ h ^= c[i]; h *= 16777619u; }
    return h;
}
/* the VOut field a declaration's usage maps to: 0 pos, 1 col0, 2 col1, 3 + k uvk, 11 fog; -1 none */
static int sm3_field(int usage, int index){
    if (usage == 0 && index == 0) return 0;
    if (usage == 10 && index < 2) return 1 + index;
    if (usage == 5 && index < 8) return 3 + index;
    if (usage == 11 && index == 0) return 11;
    return -1;
}

/* Ill-conditioned results: a comparison (cmp, slt, setp, if_comp, the alpha
 * test, texkill) can sit closer to its threshold than the rounding of a GPU's
 * division, sin or log2, and then flip there (the 16-tap shadow shaders
 * compare a depth difference with 0: one tap moves the light by 1/16).  So
 * psref runs each case twice more with every input moved by 1e-5 of itself
 * (at least 1e-7), at random: when its own result then moves by more than the
 * test's tolerance, the case is marked fragile, and shadercheck.ts counts a
 * mismatch there apart from the failures.  A smooth result does not move that
 * far, so everything else must match. */
static float nudge(float x){
    float s = (rnd() & 1u) ? 1.f : -1.f, m = fabsf(x) > 0.01f ? fabsf(x) : 0.01f;
    return x + s * 1e-5f * m;
}
static int differs(const float *a, const float *b, int n, float tol){
    for (int i = 0; i < n; i++){
        if (a[i] == b[i] || (isnan(a[i]) && isnan(b[i]))) continue;
        float d = fabsf(a[i] - b[i]) / fmaxf(1.f, fabsf(b[i]));
        if (!(d <= tol)) return 1;
    }
    return 0;
}

/* ps_<id>.p3vec (little-endian): u32 'P3V1', nquads, 0, 0; the 560-byte PSPar
 * block; per pixel 42 f32 inputs (col0, col1, uv0..uv7, fog, face), per pixel
 * 8 f32 expected: rgba after the alpha test (ALPHA_FUNC 7) and, ps_2_x, the
 * fixed-function fog; killed 0/1, or -1 when psref refused the quad; depth;
 * fragile 0/1; 0.  Quad q's lane l is the pixel (2 (q % 8) + (l & 1),
 * 2 (q / 8) + (l >> 1)) of a 16-pixel-wide target; inside a quad every input
 * is affine (base + dx gx + dy gy), so the derivatives on both sides are
 * differences of the same values. */
enum { P3_NQ = 64, P3_NIN = 42, P3_NOUT = 8 };
typedef struct {
    const PsrefProg *p; PsrefConsts k; const PSRefPar *P; int ps3;
    int vf[10][4];              /* each v# component's VOut field: 1 col0, 2 col1, 3 + k uvk, 11 fog; -1 none */
} P3Gen;
/* psref on one quad of inputs (in: 4 x P3_NIN), then the alpha test and fog: w (4 x P3_NOUT); 0 = refused */
static int p3_expect(const P3Gen *g, int q, float in[4][P3_NIN], float w[4][P3_NOUT], char *err, size_t ec){
    PsrefPsIn pin[4]; PsrefPsOut pout[4];
    memset(pin, 0, sizeof pin);
    for (int l = 0; l < 4; l++){
        const float *v = in[l];
        for (int r = 0; r < 10; r++) for (int c = 0; c < 4; c++){
            int f = g->vf[r][c];
            pin[l].v[r][c] = f == 1 || f == 2 ? v[(f - 1) * 4 + c] : f >= 3 && f < 11 ? v[8 + (f - 3) * 4 + c] : f == 11 ? v[40] : 0.f;
        }
        for (int t = 0; t < 8; t++) for (int c = 0; c < 4; c++) pin[l].t[t][c] = v[8 + t * 4 + c];
        pin[l].pos[0] = (float)(2 * (q % 8) + (l & 1)); pin[l].pos[1] = (float)(2 * (q / 8) + (l >> 1));
        pin[l].face = v[41];
    }
    if (!psref_ps_run(g->p, &g->k, pin, sm3_tex, NULL, pout, err, ec)) return 0;
    for (int l = 0; l < 4; l++){
        float *o = w[l];
        const float *res = pout[l].oC[0];
        float fog = in[l][40];
        int killed = pout[l].killed;
        if (res[3] < g->P->aref) killed = 1;                    /* ALPHA_FUNC 7: !(a < aref) passes */
        float f = fog < 0.f ? 0.f : (fog > 1.f ? 1.f : fog), m = 1.f + (f - 1.f) * g->P->fog[3];
        for (int c = 0; c < 3; c++) o[c] = g->ps3 ? res[c] : g->P->fog[c] + (res[c] - g->P->fog[c]) * m;
        o[3] = res[3]; o[4] = (float)killed; o[5] = pout[l].depth_written ? pout[l].depth : 0.f; o[6] = 0.f; o[7] = 0.f;
    }
    return 1;
}
static int write_sm3_ps_vectors(const char *dir, int id, const uint32_t *tok, size_t ndw){
    char err[512];
    PsrefCaps caps; psref_caps_sm3(&caps);
    PsrefProg *p = psref_load(tok, ndw, &caps, err, sizeof err);
    if (!p){ fprintf(stderr, "ps_%d: psref refuses it: %s\n", id, err); return 0; }
    g_rng = fnv(tok, ndw * 4) | 1u;
    static PSRefPar P; memset(&P, 0, sizeof P);
    P.aref = rndf(0.f, 0.5f);
    /* material constants are positive (a power <= 0 of a zero is infinite in D3D9 too: no use as a check) */
    for (int r = 0; r < 32; r++) for (int c = 0; c < 4; c++) P.c[r][c] = rndf(0.05f, 1.f);
    for (int c = 0; c < 3; c++) P.fog[c] = rndf(0.f, 1.f);
    P.fog[3] = (float)(rnd() & 1u);
    P3Gen g; memset(&g, 0, sizeof g);
    g.p = p; g.P = &P; g.ps3 = psref_version(p) == PSREF_PS_3_0;
    g.k.c = (const float (*)[4])P.c; g.k.nc = 32;
    /* each v# of a ps_3_0 from the fields of its declarations; ps_2_x's v0, v1 are col0, col1 */
    for (int r = 0; r < 10; r++) for (int c = 0; c < 4; c++) g.vf[r][c] = -1;
    for (int i = 0; i < psref_ndcl(p); i++){
        PsrefDcl d;
        if (!psref_dcl(p, i, &d) || d.regtype != PSREF_DCL_INPUT || d.reg >= 10) continue;
        int f = g.ps3 ? sm3_field(d.usage, d.index) : 1 + d.reg;
        for (int c = 0; c < 4; c++) if (d.mask >> c & 1u) g.vf[d.reg][c] = f;
    }
    static float in[P3_NQ * 4][P3_NIN], want[P3_NQ * 4][P3_NOUT];
    int skipped = 0, fragile = 0;
    for (int q = 0; q < P3_NQ; q++){
        float base[P3_NIN], gx[P3_NIN], gy[P3_NIN];
        for (int i = 0; i < P3_NIN; i++){
            int uvzw = i >= 8 && i < 40 && (i & 3) >= 2;
            base[i] = i < 8 ? rndf(0.f, 1.f) : uvzw ? rndf(0.5f, 2.f) : i < 40 ? rndf(0.f, 1.f) : rndf(-0.2f, 1.2f);
            gx[i] = rndf(-0.01f, 0.01f); gy[i] = rndf(-0.01f, 0.01f);
        }
        float face = (rnd() & 1u) ? 1.f : -1.f;
        float (*qi)[P3_NIN] = &in[q * 4], (*qw)[P3_NOUT] = &want[q * 4];
        for (int l = 0; l < 4; l++){
            float dx = (float)(l & 1), dy = (float)(l >> 1);
            for (int i = 0; i < 41; i++) qi[l][i] = base[i] + gx[i] * dx + gy[i] * dy;
            qi[l][41] = face;
        }
        if (!p3_expect(&g, q, qi, qw, err, sizeof err)){
            if (!skipped) fprintf(stderr, "ps_%d quad %d: psref: %s\n", id, q, err);
            skipped++;
            for (int l = 0; l < 4; l++){ memset(qw[l], 0, sizeof qw[l]); qw[l][4] = -1.f; }
            continue;
        }
        for (int t = 0; t < 2; t++){
            float ni[4][P3_NIN], nw[4][P3_NOUT];
            for (int l = 0; l < 4; l++){ for (int i = 0; i < 41; i++) ni[l][i] = nudge(qi[l][i]); ni[l][41] = face; }
            int ok = p3_expect(&g, q, ni, nw, err, sizeof err);
            for (int l = 0; l < 4; l++)
                if (!ok || nw[l][4] != qw[l][4] || (!qw[l][4] && differs(nw[l], qw[l], 6, 1e-3f))) qw[l][6] = 1.f;
        }
        for (int l = 0; l < 4; l++) fragile += qw[l][6] != 0.f;
    }
    psref_free(p);
    /* psref refused every quad (an undefined read on every input, say): nothing to compare, no file */
    if (skipped == P3_NQ){ fprintf(stderr, "ps_%d: psref refused all %d quads: no vectors\n", id, P3_NQ); return 0; }
    char path[1024]; snprintf(path, sizeof path, "%s/ps_%d.p3vec", dir, id);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    uint32_t hdr[4] = { 0x31563350u, P3_NQ, 0, 0 };
    fwrite(hdr, 4, 4, f);
    float blk[140] = { 0 };                    /* PSPar: aref usec pad pad, proj, c[32], fog */
    blk[0] = P.aref; memcpy(blk + 8, P.c, 512); memcpy(blk + 136, P.fog, 16);
    fwrite(blk, 4, 140, f);
    fwrite(in, sizeof in, 1, f);
    fwrite(want, sizeof want, 1, f);
    fclose(f);
    if (skipped) fprintf(stderr, "ps_%d: psref refused %d of %d quads\n", id, skipped, P3_NQ);
    if (fragile) fprintf(stderr, "ps_%d: %d of %d pixels fragile\n", id, fragile, P3_NQ * 4);
    return 1;
}

/* vs_<id>.v3vec: u32 'V3V1', nverts, stride, 0; the vertex bytes; 256 x 4 f32
 * constants (K.c); 45 f32 "compare" flags (1 where an output declaration
 * covers the VOut component: pos, col0, col1, uv0..uv7, fog); per vertex 46
 * f32: the 45 expected from psref on the inputs decoded as com.c decl_fetch
 * does, then fragile 0/1 (nudge above). */
enum { V3_NOUT = 46 };
static int v3_expect(const PsrefProg *p, const PsrefConsts *k, const float in[16][4], const int of[12][4], float w[V3_NOUT],
                     char *err, size_t ec){
    float o[12][4]; unsigned om[12];
    for (int c = 0; c < V3_NOUT; c++) w[c] = 0.f;
    w[44] = 1.f;
    if (!psref_vs_run(p, k, in, sm3_tex, NULL, o, om, err, ec)) return 0;
    for (int r = 0; r < 12; r++) for (int c = 0; c < 4; c++) if (of[r][c] >= 0) w[of[r][c]] = o[r][c];
    return 1;
}
static int write_sm3_vs_vectors(const char *dir, int id, const uint32_t *tok, size_t ndw, const int dcl[16][2]){
    enum { NV = 64 };
    char err[512];
    PsrefCaps caps; psref_caps_sm3(&caps);
    PsrefProg *p = psref_load(tok, ndw, &caps, err, sizeof err);
    if (!p){ fprintf(stderr, "vs_%d: psref refuses it: %s\n", id, err); return 0; }
    int stride = 4;
    for (int i = 0; i < 16; i++)
        if (dcl[i][0] >= 0 && dcl[i][1] >= 0 && dcl[i][1] < 13 && dcl[i][0] + DSIZE[dcl[i][1]] > stride)
            stride = dcl[i][0] + DSIZE[dcl[i][1]];
    stride = (stride + 3) & ~3;
    g_rng = fnv(tok, ndw * 4) ^ (uint32_t)id * 2654435761u;
    unsigned char *vb = calloc(NV, (size_t)stride);
    for (int v = 0; v < NV; v++)
        for (int i = 0; i < 16; i++){
            if (dcl[i][0] < 0) continue;
            unsigned char *a = vb + (size_t)v * stride + dcl[i][0];
            int t = dcl[i][1];
            if (t <= 3) for (int c = 0; c <= t; c++){ float fl = rndf(-1.f, 1.f); memcpy(a + 4 * c, &fl, 4); }
            else for (int c = 0; c < DSIZE[t]; c++) a[c] = (unsigned char)rnd();
        }
    static float K[256][4];
    for (int r = 0; r < 256; r++) for (int c = 0; c < 4; c++) K[r][c] = rndf(0.05f, 1.f);
    PsrefConsts k; memset(&k, 0, sizeof k); k.c = (const float (*)[4])K; k.nc = 256;
    /* where each output register's components go */
    int of[12][4]; float care[45] = { 0 };
    for (int r = 0; r < 12; r++) for (int c = 0; c < 4; c++) of[r][c] = -1;
    for (int i = 0; i < psref_ndcl(p); i++){
        PsrefDcl d;
        if (!psref_dcl(p, i, &d) || d.regtype != PSREF_DCL_OUTPUT || d.reg >= 12) continue;
        int fld = sm3_field(d.usage, d.index);
        if (fld < 0) continue;
        for (int c = 0; c < 4; c++){
            if (!(d.mask >> c & 1u)) continue;
            if (fld == 11){ of[d.reg][c] = 44; care[44] = 1.f; break; }    /* fog: the first component */
            of[d.reg][c] = fld * 4 + c; care[fld * 4 + c] = 1.f;
        }
    }
    static float want[NV][V3_NOUT];
    int bad = 0, fragile = 0;
    for (int v = 0; v < NV; v++){
        float in[16][4];
        for (int r = 0; r < 16; r++){ in[r][0] = in[r][1] = in[r][2] = 0.f; in[r][3] = 1.f; }
        for (int i = 0; i < 16; i++)
            if (dcl[i][0] >= 0) decl_fetch(vb + (size_t)v * stride + dcl[i][0], (unsigned)dcl[i][1], in[i]);
        if (!v3_expect(p, &k, (const float (*)[4])in, (const int (*)[4])of, want[v], err, sizeof err)){
            if (!bad) fprintf(stderr, "vs_%d vertex %d: psref: %s\n", id, v, err);
            bad++;
            continue;
        }
        for (int t = 0; t < 2; t++){
            /* only what the stream supplies moves: a type's default components (FLOAT3's w = 1 ...) are exact on the GPU too */
            static const int NC[13] = { 1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4 };
            float ni[16][4], nw[V3_NOUT], a[45], b[45];
            for (int r = 0; r < 16; r++) for (int c = 0; c < 4; c++)
                ni[r][c] = dcl[r][0] >= 0 && dcl[r][1] >= 0 && dcl[r][1] < 13 && c < NC[dcl[r][1]] ? nudge(in[r][c]) : in[r][c];
            int ok = v3_expect(p, &k, (const float (*)[4])ni, (const int (*)[4])of, nw, err, sizeof err);
            int n = 0;
            for (int c = 0; c < 45; c++) if (care[c]){ a[n] = nw[c]; b[n] = want[v][c]; n++; }
            if (!ok || differs(a, b, n, 1e-4f)) want[v][45] = 1.f;
        }
        fragile += want[v][45] != 0.f;
    }
    psref_free(p);
    if (bad){ free(vb); fprintf(stderr, "vs_%d: psref refused %d of %d vertices: no vectors\n", id, bad, NV); return 0; }
    char path[1024]; snprintf(path, sizeof path, "%s/vs_%d.v3vec", dir, id);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    uint32_t hdr[4] = { 0x31563356u, NV, (uint32_t)stride, 0 };
    fwrite(hdr, 4, 4, f);
    fwrite(vb, (size_t)stride, NV, f);
    fwrite(K, sizeof K, 1, f);
    fwrite(care, sizeof care, 1, f);
    fwrite(want, sizeof want, 1, f);
    fclose(f); free(vb);
    if (fragile) fprintf(stderr, "vs_%d: %d of %d vertices fragile\n", id, fragile, NV);
    return 1;
}

/* every pixel shader object of the dump, and its stage-0 cube twin */
static int game_ps_vectors(const char *dump, const char *dir){
    int n = 0;
    for (int id = 1; id < 256; id++){
        char bp[1024]; snprintf(bp, sizeof bp, "%s/psobj_%d.bin", dump, id);
        size_t bn; unsigned char *b = slurp(bp, &bn);
        if (!b) continue;
        if (bn >= 4 && is_sm3(((const uint32_t *)b)[0])) n += write_sm3_ps_vectors(dir, id, (const uint32_t *)b, bn / 4);
        else {
            VSProg *p = vs_compile((const uint32_t *)b, bn / 4);
            if (p){ n += write_ps_vectors(dir, id, p, 0u); n += write_ps_vectors(dir, id + 256, p, 1u); vs_free(p); }
        }
        free(b);
    }
    return n;
}

/* The game's own pairs: index.txt "vs <gid> obj=<n> decl=0x.. in=v0@0:FLOAT3,..." */
static int game_vectors(const char *dump, const char *dir){
    static const char *const TN[] = { "FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4",
        "SHORT2", "SHORT4", "UBYTE4N", "SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N" };
    char path[1024]; snprintf(path, sizeof path, "%s/index.txt", dump);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048]; int n = 0;
    while (fgets(line, sizeof line, f)){
        int gid, obj; char in[1500];
        if (sscanf(line, "vs %d obj=%d decl=%*s in=%1499s", &gid, &obj, in) != 3) continue;
        int dcl[16][2];
        for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
        for (char *t = strtok(in, ","); t; t = strtok(0, ",")){
            int r, off; char ty[32];
            if (sscanf(t, "v%d@%d:%31s", &r, &off, ty) != 3 || r < 0 || r > 15) continue;
            for (int q = 0; q < 13; q++) if (!strcmp(ty, TN[q])){ dcl[r][0] = off; dcl[r][1] = q; }
        }
        char bp[1024]; snprintf(bp, sizeof bp, "%s/vsobj_%d.bin", dump, obj);
        size_t bn; unsigned char *b = slurp(bp, &bn);
        if (!b) continue;
        if (bn >= 4 && is_sm3(((const uint32_t *)b)[0])) n += write_sm3_vs_vectors(dir, gid, (const uint32_t *)b, bn / 4, (const int (*)[2])dcl);
        else {
            VSProg *p = vs_compile((const uint32_t *)b, bn / 4);
            if (p){ write_vectors(dir, gid, p, (const int (*)[2])dcl); n++; vs_free(p); }
        }
        free(b);
    }
    fclose(f);
    return n;
}

static int pairs(const char *dump, const char *dir){
    DIR *d = opendir(dump);
    if (!d){ perror(dump); return 1; }
    enum { MAXO = 512 };
    static struct { char name[64]; unsigned char *b; size_t n; } vs[MAXO], dc[MAXO];
    int nvs = 0, ndc = 0;
    struct dirent *e;
    while ((e = readdir(d))){
        int isvs = !strncmp(e->d_name, "vsobj_", 6) && strstr(e->d_name, ".bin");
        int isdc = !strncmp(e->d_name, "decl_", 5) && strstr(e->d_name, ".bin");
        if (!isvs && !isdc) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dump, e->d_name);
        size_t n; unsigned char *b = slurp(p, &n);
        if (!b) continue;
        /* distinct contents only */
        int dup = 0, cnt = isvs ? nvs : ndc;
        for (int i = 0; i < cnt && !dup; i++){
            if (isvs ? (vs[i].n == n && !memcmp(vs[i].b, b, n)) : (dc[i].n == n && !memcmp(dc[i].b, b, n))) dup = 1;
        }
        if (dup || cnt >= MAXO){ free(b); continue; }
        if (isvs){ snprintf(vs[nvs].name, 64, "%s", e->d_name); vs[nvs].b = b; vs[nvs].n = n; nvs++; }
        else     { snprintf(dc[ndc].name, 64, "%s", e->d_name); dc[ndc].b = b; dc[ndc].n = n; ndc++; }
    }
    closedir(d);
    FILE *ix; { char p[1024]; snprintf(p, sizeof p, "%s/pairs.txt", dir); ix = fopen(p, "w"); }
    if (!ix){ perror("pairs.txt"); return 1; }
    int id = 1000, ok = 0, fail = 0, nopair = 0;
    for (int v = 0; v < nvs; v++){
        VSProg *p = vs_compile((const uint32_t *)vs[v].b, vs[v].n / 4);
        if (!p){ fprintf(ix, "%s: parse failed\n", vs[v].name); fail++; continue; }
        int paired = 0;
        for (int k = 0; k < ndc; k++){
            const unsigned char *el = dc[k].b;
            int dcl[16][2], have[14][16];
            memset(have, 0, sizeof have);
            for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
            int nel = 0;
            for (size_t q = 0; q + 8 <= dc[k].n; q += 8){
                unsigned st = el[q] | el[q+1] << 8, of = el[q+2] | el[q+3] << 8;
                unsigned ty = el[q+4], us = el[q+6], ui = el[q+7];
                if (st == 0xFFu && ty == 17u) break;
                if (st != 0) continue;
                if (us < 14 && ui < 16) have[us][ui] = 1;
                int rg = vs_input_reg(p, (int)us, (int)ui);
                if (rg >= 0 && nel < 16){ dcl[rg][0] = (int)of; dcl[rg][1] = (int)ty; nel++; }
            }
            int covered = nel > 0;
            for (int us = 0; us < 14 && covered; us++)
                for (int ui = 0; ui < 16; ui++)
                    if (vs_input_reg(p, us, ui) >= 0 && !have[us][ui]){ covered = 0; break; }
            if (!covered) continue;
            paired = 1;
            int n = vs_to_wgsl(p, ++id, (const int (*)[2])dcl, g_buf, sizeof g_buf);
            char name[64]; snprintf(name, sizeof name, "vs_%d.wgsl", id);
            int sm3 = is_sm3(((const uint32_t *)vs[v].b)[0]);
            if (n > 0){
                put(dir, name, g_buf, (size_t)n); ok++;
                if (sm3) write_sm3_vs_vectors(dir, id, (const uint32_t *)vs[v].b, vs[v].n / 4, (const int (*)[2])dcl);
                else write_vectors(dir, id, p, (const int (*)[2])dcl);
            } else fail++;
            fprintf(ix, "vs %d = %s x %s %s%s%s\n", id, vs[v].name, dc[k].name, n > 0 ? "ok" : "REJECTED",
                    n > 0 || !sm3 ? "" : ": ", n > 0 || !sm3 ? "" : wgsl_refusal());
        }
        if (!paired){ nopair++; fprintf(ix, "%s: no declaration supplies all inputs\n", vs[v].name); }
        vs_free(p);
    }
    fclose(ix);
    int gv = game_vectors(dump, dir), pv = game_ps_vectors(dump, dir);
    printf("%d distinct vertex shaders x %d distinct declarations: %d modules, %d rejected, %d shaders unpaired; "
           "test vectors for %d game pairs and %d pixel shader modules\n", nvs, ndc, ok, fail, nopair, gv, pv);
    return fail ? 1 : 0;
}

/* one shader program: its module(s), its index line and (SM3 class) its vectors; 0 = a translation failed */
static int translate_one(const char *dir, FILE *ix, const uint32_t *tok, size_t dw, const char *base, int *nvs, int *nps){
    VSProg *p = vs_compile(tok, dw);
    if (!p){ fprintf(stderr, "%s: parse failed\n", base); return 0; }
    char name[64];
    int n, ok = 1, sm3 = is_sm3(tok[0]);
    if ((tok[0] >> 16) == 0xFFFFu){
        int id = ++*nps;
        n = ps_to_wgsl(p, id, g_buf, sizeof g_buf);
        char why[256]; snprintf(why, sizeof why, "%s", sm3 ? wgsl_refusal() : "");
        if (n > 0){ snprintf(name, sizeof name, "ps_%d.wgsl", id); put(dir, name, g_buf, (size_t)n); }
        else ok = 0;
        if (n > 0 && g_dv >= 0 && wgsl_get_stages() == 8){
            wgsl_set_depth_stages(0xFFu);
            int nd = ps_to_wgsl_ex(p, id | 0xFF << 9, g_buf, sizeof g_buf, 0u);
            wgsl_set_depth_stages(0);
            g_dv++;
            if (nd <= 0){ g_dv_fail++; fprintf(stderr, "%s: no depth variant\n", base); }
            else if (strstr(g_buf, "textureSampleCompareLevel")) g_dv_depth++;      /* it looks a depth stage up */
        }
        int nc = ps_to_wgsl_ex(p, id + 256, g_buf, sizeof g_buf, 1u);
        if (nc > 0){ snprintf(name, sizeof name, "ps_%d.wgsl", id + 256); put(dir, name, g_buf, (size_t)nc); }
        /* an SM3-class shader that looks up a 2D s0 has no stage-0 cube twin (D3D9 binds no cube there) */
        else if (!sm3 || n <= 0) ok = 0;
        fprintf(ix, "ps %d src=%s instr=%u unknown=%u len=%d cube=%d", id, base, vs_ninstr(p), vs_unknown(p), n, nc);
        if (sm3 && n <= 0) fprintf(ix, " REJECTED: %s", why);
        fprintf(ix, "\n");
        /* vectors where psref runs it (a program it refuses or cannot run on any input gets none: stderr says why);
         * the older models' from ps_ref_run, with 8 stages (with 4 the blob and effect modes write none, as before) */
        if (sm3 && n > 0 && write_sm3_ps_vectors(dir, id, tok, dw) && nc > 0) write_sm3_ps_vectors(dir, id + 256, tok, dw);
        if (!sm3 && wgsl_get_stages() == 8){
            if (n > 0 && !write_ps_vectors(dir, id, p, 0u)) fprintf(stderr, "ps_%d: ps_ref_run refused it\n", id);
            if (nc > 0 && !write_ps_vectors(dir, id + 256, p, 1u)) fprintf(stderr, "ps_%d: ps_ref_run refused it\n", id + 256);
        }
    } else {
        /* synthetic declaration: usage -> type, packed in register order */
        static const int ty_of_usage[14] = { 2, 3, 4, 2, 0, 1, 2, 2, 0, 3, 4, 0, 0, 0 };
        int dcl[16][2], off = 0;
        for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
        for (int us = 0; us < 14; us++)
            for (int ui = 0; ui < 8; ui++){
                int r = vs_input_reg(p, us, ui);
                if (r < 0) continue;
                int t = ty_of_usage[us];
                static const int sz[13] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8 };
                dcl[r][0] = off; dcl[r][1] = t; off += sz[t];
            }
        int id = ++*nvs;
        n = vs_to_wgsl(p, id, (const int (*)[2])dcl, g_buf, sizeof g_buf);
        if (n > 0){ snprintf(name, sizeof name, "vs_%d.wgsl", id); put(dir, name, g_buf, (size_t)n); }
        else ok = 0;
        fprintf(ix, "vs %d src=%s instr=%u unknown=%u kmax=%d stride=%d len=%d", id, base,
                vs_ninstr(p), vs_unknown(p), vs_kmax(p), off, n);
        if (sm3 && n <= 0) fprintf(ix, " REJECTED: %s", wgsl_refusal());
        fprintf(ix, "\n");
        if (sm3 && n > 0) write_sm3_vs_vectors(dir, id, tok, dw, (const int (*)[2])dcl);
        else if (n > 0 && wgsl_get_stages() == 8) write_vectors(dir, id, p, (const int (*)[2])dcl);
    }
    vs_free(p);
    return ok;
}

/* index.txt and the built-in modules of an output folder */
static FILE *builtins(const char *dir){
    FILE *ix; { char p[1024]; snprintf(p, sizeof p, "%s/index.txt", dir); ix = fopen(p, "w"); }
    if (!ix){ perror("index.txt"); exit(1); }
    int n;
    if ((n = wgsl_tvert_module(g_buf, sizeof g_buf)) > 0) put(dir, "vs_0.wgsl", g_buf, (size_t)n);
    if ((n = wgsl_ff_module(0, g_buf, sizeof g_buf, 0)) > 0) put(dir, "ps_0.wgsl", g_buf, (size_t)n);
    if ((n = wgsl_ff_module(256, g_buf, sizeof g_buf, 1)) > 0) put(dir, "ps_256.wgsl", g_buf, (size_t)n);
    if ((n = wgsl_pull_test_module(g_buf, sizeof g_buf)) > 0) put(dir, "pulltest.wgsl", g_buf, (size_t)n);
    return ix;
}

/* --fx: every shader program of compiled effects, distinct by bytes */
static int fx(const char *dir, int nf, char **files){
    FILE *ix = builtins(dir);
    enum { MAXB = 1024 };
    static struct { uint32_t h; size_t n; const uint32_t *w; } seen[MAXB];
    int ns = 0, nvs = 0, nps = 0, fails = 0, dups = 0;
    unsigned char **bufs = calloc((size_t)nf, sizeof *bufs);    /* kept to the end: seen[] points into them */
    if (!bufs) return 1;
    for (int a = 0; a < nf; a++){
        size_t L; unsigned char *b = bufs[a] = slurp(files[a], &L);
        if (!b){ perror(files[a]); return 1; }
        const uint32_t *w = (const uint32_t *)b;
        size_t n = L / 4;
        const char *base = strrchr(files[a], '/'); base = base ? base + 1 : files[a];
        int k = 0;
        for (size_t i = 0; i + 2 < n; i++){
            uint32_t size = w[i], v = w[i + 1];
            if (size < 8 || (size & 3) || i + 1 + size / 4 > n) continue;
            unsigned major = (v >> 8) & 0xFFu;
            if (((v >> 16) != 0xFFFEu && (v >> 16) != 0xFFFFu) || major < 1 || major > 3 || w[i + size / 4] != 0x0000FFFFu) continue;
            const uint32_t *t = &w[i + 1];
            size_t dw = size / 4;
            uint32_t h = fnv(t, dw * 4);
            int dup = 0;
            for (int s = 0; s < ns && !dup; s++) dup = seen[s].h == h && seen[s].n == dw && !memcmp(seen[s].w, t, dw * 4);
            i += dw;
            if (dup){ dups++; continue; }
            if (ns < MAXB){ seen[ns].h = h; seen[ns].n = dw; seen[ns].w = t; ns++; }
            char nm[160]; snprintf(nm, sizeof nm, "%s#%d", base, k++);
            if (!translate_one(dir, ix, t, dw, nm, &nvs, &nps)) fails++;
        }
    }
    for (int a = 0; a < nf; a++) free(bufs[a]);
    free(bufs);
    fclose(ix);
    printf("%d effects: %d distinct shaders (%d vertex, %d pixel), %d repeated; %d translation failures\n",
           nf, nvs + nps, nvs, nps, dups, fails);
    if (wgsl_get_stages() == 8)
        printf("depth variants: %d pixel shaders, %d with a depth lookup, %d failures\n", g_dv, g_dv_depth, g_dv_fail);
    return fails || g_dv_fail ? 1 : 0;
}

int main(int argc, char **argv){
    if (argc >= 3 && !strcmp(argv[1], "--stages")){
        unsigned s = (unsigned)atoi(argv[2]);
        if (s != 4 && s != 8){ fprintf(stderr, "--stages: 4 or 8\n"); return 2; }
        wgsl_set_stages(s);
        argc -= 2; argv += 2;
    }
    if (argc == 4 && !strcmp(argv[1], "--pairs")) return pairs(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "--fx")) return fx(argv[2], argc - 3, argv + 3);
    if (argc < 3){
        fprintf(stderr, "usage: %s [--stages 8] <outdir> shader.bin... | --pairs <dumpdir> <outdir> | --fx <outdir> effect.fxo...\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    FILE *ix = builtins(dir);
    int nvs = 0, nps = 0, fails = 0;
    for (int a = 2; a < argc; a++){
        FILE *f = fopen(argv[a], "rb");
        if (!f){ perror(argv[a]); return 1; }
        static uint32_t tok[16384];
        size_t dw = fread(tok, 4, 16384, f); fclose(f);
        const char *base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
        if (!translate_one(dir, ix, tok, dw, base, &nvs, &nps)) fails++;
    }
    fclose(ix);
    printf("%d vertex shaders, %d pixel shaders, %d translation failures\n", nvs, nps, fails);
    return fails ? 1 : 0;
}
