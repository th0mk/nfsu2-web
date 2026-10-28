/* d3dxmath_cases.h -- the inputs of the D3DX math checks (TRACKER M2-08),
 * shared by web/test/d3dx_oracle_unit.c ("math": each case through the
 * oracle's export and through speed.exe's IAT slot) and web/test/
 * d3dxmath_unit.c (the native answers alone, no game files): the same cases
 * from the same seeds, so both print the same digest of the native answers.
 *
 * A case: a window of guest memory filled with inputs, the arguments (some
 * pointing into the window, aliased, overlapping or misaligned the ways a
 * caller can), and the caller's x87 control word.  Most inputs are ordinary
 * floats; some cases inject special values (zeros of both signs, infinities,
 * quiet and signalling NaNs, denormals, the extremes), some take random bit
 * patterns throughout, some are dense with NaNs (which of two NaN operands an
 * operation keeps).  Normalize's first cases put |v|^2 exactly on its 2^-46
 * threshold and one ulp either side (mc_norm_at).  Matrices are sometimes
 * affine (last column 0 0 0 1), sometimes singular. */
#ifndef D3DXMATH_CASES_H
#define D3DXMATH_CASES_H
#include <stdint.h>
#include <string.h>

#define MC_WIN   0x1000u                 /* the window, 16-byte aligned */
#define MC_CASES 12000                   /* per function (L-046: at least 10,000) */

typedef struct { uint32_t args[6]; int nargs; uint16_t fcw; } MathCase;

static uint32_t mc_s;
static uint32_t mc_rnd(void){ mc_s ^= mc_s << 13; mc_s ^= mc_s >> 17; mc_s ^= mc_s << 5; return mc_s; }
static uint32_t mc_fbits(float f){ uint32_t b; memcpy(&b, &f, 4); return b; }

/* an ordinary float: zeros, +-1, small integers, or about 2^-10 .. 2^10 with a random significand */
static uint32_t mc_ordinary(void){
    uint32_t r = mc_rnd();
    switch (r & 15u){
    case 0: return 0u;
    case 1: return 0x80000000u;
    case 2: return 0x3f800000u | (r & 0x80000000u);
    case 3: return mc_fbits((float)((int)((r >> 8) % 201u) - 100));
    default: return (r & 0x80000000u) | ((117u + (r >> 4) % 21u) << 23) | (mc_rnd() & 0x007fffffu);
    }
}
static const uint32_t mc_specials[] = {
    0x7f800000u, 0xff800000u,                        /* infinities */
    0x7fc00000u, 0xffc00000u, 0x7fc12345u, 0xffe54321u,  /* quiet NaNs (the default one among them) */
    0x7f800001u, 0xff812345u, 0x7fa00000u,           /* signalling NaNs */
    0x00000001u, 0x807fffffu, 0x00012345u,           /* denormals */
    0x00800000u, 0x7f7fffffu, 0xff7fffffu,           /* the smallest normal, the largest finite */
    0x80000000u, 0x00000000u, 0x5f000000u, 0x1f000000u, 0x3f800001u,
    0x28800000u, 0x287fffffu,                        /* 2^-46 and just below (as a component: a square near 2^-92) */
};
/* Normalize's cases 0..13: x, y, z whose s = (x x + y y) + z z, each step rounded to f32, is exactly
 * the kernel's 2^-46 (it normalises), one ulp below (the zero vector) or one ulp above, s built from
 * one, two or three components; then x one ulp either side of 2^-23 (s two ulps either side) */
static const uint32_t mc_norm_at[][3] = {
    { 0x34000000u, 0, 0 }, { 0, 0x34000000u, 0 }, { 0, 0, 0xb4000000u },          /* s = 2^-46 */
    { 0x33b504f4u, 0x33b504f3u, 0 }, { 0x3393cd2eu, 0x3393cd3bu, 0x3393cd45u },
    { 0x33ffffffu, 0x2e000000u, 0x2e000000u },
    { 0x33ffffffu, 0x2e000000u, 0 }, { 0x2e000000u, 0, 0x33ffffffu },              /* s = 0x287fffff */
    { 0x3393cd2eu, 0x3393cd39u, 0x3393cd46u },
    { 0x33ffffffu, 0x2e800000u, 0 }, { 0x2e800000u, 0, 0xb3ffffffu },              /* s = 0x28800001 */
    { 0x3393cd2eu, 0x3393cd3cu, 0x3393cd46u },
    { 0x33ffffffu, 0, 0 }, { 0x34000001u, 0, 0 },                                  /* s = 0x287ffffe, 0x28800002 */
};
#define MC_NNORM_AT (int)(sizeof mc_norm_at / sizeof mc_norm_at[0])
#define MC_NSPEC (sizeof mc_specials / sizeof mc_specials[0])
static int mc_mode;                                  /* 0 ordinary, 1 some specials, 2 random bits, 3 NaN-dense */
static uint32_t mc_value(void){
    if (mc_mode == 2) return mc_rnd();
    if (mc_mode == 3 && mc_rnd() % 3u == 0) return 0x7f800000u | (mc_rnd() & 0x807fffffu) | 1u;   /* a NaN, either kind */
    if (mc_mode == 1 && mc_rnd() % 24u == 0) return mc_specials[mc_rnd() % MC_NSPEC];
    return mc_ordinary();
}
static void mc_put(uint32_t a, uint32_t v){ memcpy(GMEM_ + a, &v, 4); }
static uint32_t mc_get(uint32_t a){ uint32_t v; memcpy(&v, GMEM_ + a, 4); return v; }
/* a matrix at m: affine (column 4 = 0 0 0 1) in a third of the cases, singular (row 1 = row 0) sometimes */
static void mc_matrix(uint32_t m, int singular){
    if (mc_mode != 2 && mc_rnd() % 3u == 0){
        mc_put(m + 12u, 0); mc_put(m + 28u, 0); mc_put(m + 44u, 0); mc_put(m + 60u, 0x3f800000u);
    }
    if (singular) for (uint32_t k = 0; k < 16u; k += 4u) mc_put(m + 16u + k, mc_get(m + k));
}
static uint32_t mc_al(void){ return mc_rnd() & 1u ? 0u : 4u * (mc_rnd() % 4u); }   /* 16-byte aligned or not */
static float mc_range(float lo, float hi){ return lo + (hi - lo) * (float)(mc_rnd() >> 8) * 0x1p-24f; }

/* the x87 functions' callers: mostly the game thread's word (PC=24, nearest), then the CRT's and directed rounding */
static const uint16_t mc_fcws[] = { 0x007f, 0x007f, 0x007f, 0x007f, 0x027f, 0x037f, 0x047f, 0x087f, 0x0c7f, 0x0e7f };

/* Case i of function f (the order of d3dxmath_fns), the window at W */
static void mc_case(int f, int i, uint32_t W, MathCase *mc){
    mc_s = 0x9E3779B9u * (uint32_t)(i + 1) ^ 0x85EBCA6Bu * (uint32_t)(f + 7) ^ 0x2545F491u;
    if (!mc_s) mc_s = 1;
    for (int k = 0; k < 4; k++) mc_rnd();
    uint32_t r = mc_rnd() % 20u;
    mc_mode = r < 14u ? 0 : r < 17u ? 1 : r < 19u ? 2 : 3;
    for (uint32_t k = 0; k < MC_WIN; k += 4u) mc_put(W + k, mc_value());
    memset(mc, 0, sizeof *mc);
    mc->fcw = 0x007f;
    uint32_t A = W + mc_al(), B = W + 0x100u + mc_al(), O = W + 0x200u + mc_al(), V = W + 0x300u + mc_al();
    uint32_t *a = mc->args;
    switch (f){
    case 0: {                                        /* D3DXMatrixMultiply(out, m1, m2) */
        mc_matrix(A, 0); mc_matrix(B, 0);
        uint32_t q = mc_rnd() % 8u;
        a[0] = q == 0 ? A : q == 1 ? B : q == 2 ? A + 4u * (1u + mc_rnd() % 15u) : O;
        a[1] = A; a[2] = mc_rnd() % 16u == 0 ? A : B; mc->nargs = 3; break; }
    case 1: case 2: case 9: {                        /* D3DXVec4Transform, D3DXVec3Transform, D3DXVec3TransformNormal (out, v, m) */
        mc_matrix(A, 0);
        a[0] = mc_rnd() % 5u == 0 ? V : O; a[1] = V; a[2] = A; mc->nargs = 3; break; }
    case 3: {                                        /* D3DXMatrixInverse(out, det, m) */
        mc_matrix(A, mc_rnd() % 10u == 0);
        a[0] = mc_rnd() % 5u == 0 ? A : O; a[1] = mc_rnd() % 4u == 0 ? 0u : W + 0x380u; a[2] = A; mc->nargs = 3; break; }
    case 4: case 5: {                                /* D3DXMatrixOrthoLH, D3DXMatrixPerspectiveLH(out, w, h, zn, zf) */
        a[0] = O; mc->nargs = 5; mc->fcw = mc_fcws[mc_rnd() % (sizeof mc_fcws / sizeof mc_fcws[0])];
        if (mc_mode == 0 && mc_rnd() % 2u){
            float zn = mc_range(0.01f, 10.0f);
            a[1] = mc_fbits(mc_range(0.5f, 4096.0f)); a[2] = mc_fbits(mc_range(0.5f, 4096.0f));
            a[3] = mc_fbits(zn); a[4] = mc_fbits(zn + mc_range(0.001f, 10000.0f));
        } else for (int k = 1; k < 5; k++) a[k] = mc_value();
        if (mc_rnd() % 16u == 0) a[4] = a[3];          /* zn == zf */
        break; }
    case 6:                                          /* D3DXMatrixTranslation(out, x, y, z) */
        a[0] = O; for (int k = 1; k < 4; k++) a[k] = mc_value(); mc->nargs = 4; break;
    case 7: {                                        /* D3DXVec3TransformCoordArray(out, os, v, vs, m, n) */
        static const uint32_t vss[] = { 12, 16, 20, 24, 32 }, oss[] = { 12, 16, 20, 24, 32, 12, 16, 4, 8, 0 };
        mc_matrix(A, 0);
        uint32_t n = mc_rnd() % 4u == 0 ? mc_rnd() % 13u : 13u + mc_rnd() % 28u;
        uint32_t vs = vss[mc_rnd() % 5u], os = oss[mc_rnd() % 10u], v = V, o = W + 0x880u + mc_al();
        if (mc_rnd() % 5u == 0){ o = v; os = vs; }  /* in place */
        else if (mc_rnd() % 12u == 0) o = v + 4u * (1u + mc_rnd() % 6u);   /* overlapping */
        a[0] = o; a[1] = os; a[2] = v; a[3] = vs; a[4] = A; a[5] = n; mc->nargs = 6; break; }
    case 8: {                                        /* D3DXVec3Normalize(out, v) */
        if (mc_rnd() % 8u == 0)                      /* near the threshold: |v|^2 around 2^-46 */
            for (uint32_t k = 0; k < 3; k++) mc_put(V + 4u * k, mc_fbits(mc_range(-1.5f, 1.5f) * 0x1p-24f));
        if (i < MC_NNORM_AT)                         /* on the threshold and either side of it */
            for (uint32_t k = 0; k < 3; k++) mc_put(V + 4u * k, mc_norm_at[i][k]);
        a[0] = mc_rnd() % 4u == 0 ? V : O; a[1] = V; mc->nargs = 2; break; }
    case 10: {                                       /* D3DXMatrixTranspose(out, m) */
        uint32_t q = mc_rnd() % 8u;
        a[0] = q < 2u ? A : q == 2u ? A + 4u * (1u + mc_rnd() % 15u) : O; a[1] = A; mc->nargs = 2; break; }
    }
}

/* FNV-1a, 64 bits */
static uint64_t mc_fnv(uint64_t h, const void *p, size_t n){
    const uint8_t *b = (const uint8_t *)p;
    for (size_t k = 0; k < n; k++){ h ^= b[k]; h *= 0x100000001b3ull; }
    return h;
}
#define MC_FNV0 0xcbf29ce484222325ull
#endif
