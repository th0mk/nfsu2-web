/* Most Wanted: the D3DX math speed.exe imports from d3dx9_26.dll, native
 * (PLAN D-10).  Eleven functions; D3DXMatrixInverse is among them although
 * its only caller (0x6BDE4C) is never reached.  d3dx9_26.dll is not in the
 * game folder and is never shipped (GAME.md 5.1, D-09), so these are MW's
 * answers in every build; in a D3DX oracle build the oracle keeps the two
 * effect imports (module.c module_load_imports).
 *
 * What they reproduce: what d3dx9_26.dll 9.07.239.0000 runs on a CPU with SSE
 * and without 3DNow!.  Its math exports jump through a table its first call
 * fills by CPU (IsProcessorFeaturePresent 7, then 6): seven of these have an
 * SSE kernel there, D3DXMatrixTranslation and D3DXMatrixTranspose keep the
 * table's x87 default, and D3DXMatrixOrthoLH and D3DXMatrixPerspectiveLH are
 * x87 code outside the table.  Each answer does the kernel's operations in
 * the kernel's order, operand order included, and reads and writes guest
 * memory in the kernel's order, so an output that overlaps an input comes out
 * as it does there:
 *   - SSE: IEEE f32, round to nearest, denormals kept: MXCSR's default, the
 *     only mode the lift models (ldmxcsr is read and ignored; speed.exe's
 *     lift has none).  A NaN result takes the lift's SSE rule (cpu.h
 *     sse_r2f: the first operand's NaN, then the second's, else the default
 *     NaN).  rcpss/rcpps and rsqrtss are taken as the lift takes them
 *     (tools/lift.py): 1/x and 1/sqrt(x) rounded to f32, not a CPU's 12-bit
 *     estimate, which differs between Intel and AMD; the Newton step after
 *     them is the kernel's.
 *     The packed kernels run as f32x4 vectors and, when an output is a NaN,
 *     once more lane by lane with the NaN rule: an operation's result is the
 *     same either way unless it is a NaN, so outputs without a NaN are the
 *     NaN rule's too.
 *   - x87: the lift's model (cpu.h x87_r2pc): each result rounded to the
 *     caller's precision control, so PC=24 (the game thread after
 *     CreateDevice) and PC=53/64 give what the lifted DLL gives.  fld/fstp
 *     moves are written as the lift writes them, (float)(double): a
 *     signalling NaN keeps its bits through them, as in the lifted DLL (an
 *     x87 would quiet it).
 * Not reproduced: the xmm registers, ecx, edx and the flags the kernels leave
 * behind (volatile in the 32-bit Windows ABI; no caller reads them), and the
 * x87 registers below the stack top (empty).
 *
 * The binding: each import's IAT slot holds the address of the exe's own
 * thunk for it (0x7E7D58-0x7E7DA0, `jmp [slot]`), and that thunk's dispatch
 * entry is the answer here (dispatch_patch_take): the game's call goes thunk
 * -> one table lookup -> the answer, as fast as a call into lifted code.  A
 * patch the table does not take leaves the slot as it was: bound to its own
 * thunk, the lifted `jmp [slot]` would call itself forever.
 * NFSMW_D3DX_MATH=oracle in an oracle build leaves them to the oracle (A/B).
 *
 * Checked against the oracle's lifted kernels on 10,000+ inputs each
 * (web/test/d3dx-math.test.mjs, web/test/d3dx_oracle_unit.c "math"). */
#include "cpu.h"
#include "d3dxmath_mw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma clang fp contract(off)
#pragma STDC FP_CONTRACT OFF

int dispatch_patch_take(uint32_t a, void (*f)(CPU *));
uint64_t g_d3dxmath_exact;                     /* outputs with a NaN: the lane-by-lane pass ran (tests) */

typedef float v4 __attribute__((vector_size(16), aligned(4)));
#define AI static inline __attribute__((always_inline))

/* ---- guest memory (GMEM_: the base cpu.h's MR32 uses) ---- */
AI v4 ld4(uint32_t a){ v4 v; memcpy(&v, GMEM_ + a, 16); return v; }
AI void st4(uint32_t a, v4 v){ memcpy(GMEM_ + a, &v, 16); }
AI void st2(uint32_t a, v4 v){ memcpy(GMEM_ + a, &v, 8); }          /* movlps: lanes 0, 1 */
AI void st2h(uint32_t a, v4 v){ memcpy(GMEM_ + a, (const float *)&v + 2, 8); }   /* movhps: lanes 2, 3 */
AI float ldf(uint32_t a){ float f; memcpy(&f, GMEM_ + a, 4); return f; }
AI void stf(uint32_t a, float f){ memcpy(GMEM_ + a, &f, 4); }
AI void ld16(float m[16], uint32_t a){ memcpy(m, GMEM_ + a, 64); }
AI uint32_t arg(CPU *c, int i){ return MR32(c->esp + 4u + 4u * (uint32_t)i); }
/* stdcall: the callee pops its n arguments; the result in eax */
AI void ret(CPU *c, int n, uint32_t eax){ POP32(c->eip); c->esp += 4u * (uint32_t)n; c->eax = eax; }

/* ---- SSE operations: ex = 0 plain vectors, ex = 1 lane by lane with the lift's NaN rule ---- */
AI v4 bc(float x){ return (v4){ x, x, x, x }; }
AI float smul(int ex, float a, float b){ return ex ? sse_r2f(a, b, a * b) : a * b; }
AI float sadd(int ex, float a, float b){ return ex ? sse_r2f(a, b, a + b) : a + b; }
AI float ssub(int ex, float a, float b){ return ex ? sse_r2f(a, b, a - b) : a - b; }
AI v4 vmul(int ex, v4 a, v4 b){
    if (!ex) return a * b;
    v4 r; for (int k = 0; k < 4; k++) r[k] = sse_r2f(a[k], b[k], a[k] * b[k]);
    return r;
}
AI v4 vadd(int ex, v4 a, v4 b){
    if (!ex) return a + b;
    v4 r; for (int k = 0; k < 4; k++) r[k] = sse_r2f(a[k], b[k], a[k] + b[k]);
    return r;
}
AI v4 vsub(int ex, v4 a, v4 b){
    if (!ex) return a - b;
    v4 r; for (int k = 0; k < 4; k++) r[k] = sse_r2f(a[k], b[k], a[k] - b[k]);
    return r;
}
AI v4 vrcp(v4 a){ return (v4){ 1.0f / a[0], 1.0f / a[1], 1.0f / a[2], 1.0f / a[3] }; }   /* rcpps as lifted */
/* shufps d, s, imm */
#define SHUF(d, s, imm) ((v4){ (d)[(imm) & 3], (d)[((imm) >> 2) & 3], (s)[((imm) >> 4) & 3], (s)[((imm) >> 6) & 3] })
AI int nan4(v4 v){ __typeof__(v != v) n = v != v; return (n[0] | n[1] | n[2] | n[3]) != 0; }
AI int nan3(v4 v){ return v[0] != v[0] || v[1] != v[1] || v[2] != v[2]; }

/* ---- D3DXMatrixMultiply(out, m1, m2), SSE kernel ----
 * m2's rows in four registers, m1's elements broadcast; every read before the first write */
AI void mmul_k(int ex, const float a[16], v4 b0, v4 b1, v4 b2, v4 b3, v4 o[4]){
    o[0] = vadd(ex, vadd(ex, vmul(ex, bc(a[0]), b0), vmul(ex, bc(a[1]), b1)), vadd(ex, vmul(ex, bc(a[2]), b2), vmul(ex, bc(a[3]), b3)));
    o[1] = vadd(ex, vadd(ex, vadd(ex, vmul(ex, bc(a[4]), b0), vmul(ex, bc(a[5]), b1)), vmul(ex, bc(a[6]), b2)), vmul(ex, bc(a[7]), b3));
    o[2] = vadd(ex, vadd(ex, vadd(ex, vmul(ex, bc(a[8]), b0), vmul(ex, bc(a[9]), b1)), vmul(ex, bc(a[10]), b2)), vmul(ex, bc(a[11]), b3));
    o[3] = vadd(ex, vadd(ex, vmul(ex, b0, bc(a[12])), vmul(ex, b1, bc(a[13]))), vadd(ex, vmul(ex, b2, bc(a[14])), vmul(ex, b3, bc(a[15]))));
}
static void d3dx_matrix_multiply(CPU *c){
    uint32_t out = arg(c, 0), A = arg(c, 1), B = arg(c, 2);
    v4 b0 = ld4(B), b1 = ld4(B + 16u), b2 = ld4(B + 32u), b3 = ld4(B + 48u), o[4];
    float a[16]; ld16(a, A);
    mmul_k(0, a, b0, b1, b2, b3, o);
    if (nan4(o[0]) | nan4(o[1]) | nan4(o[2]) | nan4(o[3])){ g_d3dxmath_exact++; mmul_k(1, a, b0, b1, b2, b3, o); }
    st4(out, o[0]); st4(out + 16u, o[1]); st4(out + 32u, o[2]); st4(out + 48u, o[3]);
    ret(c, 3, out);
}

/* ---- D3DXVec4Transform(out, v, m), SSE kernel: (r0 x + r1 y) + (r2 z + r3 w), the row first ---- */
AI v4 v4t_k(int ex, v4 v, v4 r0, v4 r1, v4 r2, v4 r3){
    v4 hi = vadd(ex, vmul(ex, r2, bc(v[2])), vmul(ex, r3, bc(v[3])));
    return vadd(ex, vadd(ex, vmul(ex, r0, bc(v[0])), vmul(ex, r1, bc(v[1]))), hi);
}
static void d3dx_vec4_transform(CPU *c){
    uint32_t out = arg(c, 0), V = arg(c, 1), M = arg(c, 2);
    v4 v = ld4(V), r0 = ld4(M), r1 = ld4(M + 16u), r2 = ld4(M + 32u), r3 = ld4(M + 48u);
    v4 o = v4t_k(0, v, r0, r1, r2, r3);
    if (nan4(o)){ g_d3dxmath_exact++; o = v4t_k(1, v, r0, r1, r2, r3); }
    st4(out, o);
    ret(c, 3, out);
}

/* ---- D3DXVec3Transform(out4, v3, m), SSE kernel: (r0 x + r1 y) + (r2 z + r3) ---- */
AI v4 v3t_k(int ex, float x, float y, float z, v4 r0, v4 r1, v4 r2, v4 r3){
    v4 hi = vadd(ex, vmul(ex, r2, bc(z)), r3);
    return vadd(ex, vadd(ex, vmul(ex, r0, bc(x)), vmul(ex, r1, bc(y))), hi);
}
static void d3dx_vec3_transform(CPU *c){
    uint32_t out = arg(c, 0), V = arg(c, 1), M = arg(c, 2);
    float x = ldf(V), y = ldf(V + 4u), z = ldf(V + 8u);
    v4 r0 = ld4(M), r1 = ld4(M + 16u), r2 = ld4(M + 32u), r3 = ld4(M + 48u);
    v4 o = v3t_k(0, x, y, z, r0, r1, r2, r3);
    if (nan4(o)){ g_d3dxmath_exact++; o = v3t_k(1, x, y, z, r0, r1, r2, r3); }
    st4(out, o);
    ret(c, 3, out);
}

/* ---- D3DXVec3TransformNormal(out3, v3, m), SSE kernel: (x r0 + y r1) + z r2.  With m 16-byte aligned
 * the kernel multiplies component by row, otherwise row by component (only a NaN pair can tell) ---- */
AI v4 v3n_k(int ex, int al, float x, float y, float z, v4 r0, v4 r1, v4 r2){
    v4 px = al ? vmul(ex, bc(x), r0) : vmul(ex, r0, bc(x));
    v4 py = al ? vmul(ex, bc(y), r1) : vmul(ex, r1, bc(y));
    v4 pz = al ? vmul(ex, bc(z), r2) : vmul(ex, r2, bc(z));
    return vadd(ex, vadd(ex, px, py), pz);
}
static void d3dx_vec3_transform_normal(CPU *c){
    uint32_t out = arg(c, 0), V = arg(c, 1), M = arg(c, 2);
    int al = (M & 15u) == 0;
    float x = ldf(V), y = ldf(V + 4u), z = ldf(V + 8u);
    v4 r0 = ld4(M), r1 = ld4(M + 16u), r2 = ld4(M + 32u);
    v4 o = v3n_k(0, al, x, y, z, r0, r1, r2);
    if (nan3(o)){ g_d3dxmath_exact++; o = v3n_k(1, al, x, y, z, r0, r1, r2); }
    st2(out, o); stf(out + 8u, o[2]);
    ret(c, 3, out);
}

/* ---- D3DXVec3Normalize(out, v), SSE kernel: s = (x x + y y) + z z; below 2^-46 (or a NaN) the
 * zero vector, else v * k with r = rsqrt(s), k = (3 - (r s) r) (r 0.5) ---- */
#define NORM_MIN 0x1p-46f                        /* the kernel's comiss constant */
AI void norm_k(int ex, float x, float y, float z, float s, float o[3]){
    float r = 1.0f / sqrtf(s);                   /* rsqrtss as lifted */
    float t = smul(ex, smul(ex, r, s), r);
    float k = smul(ex, ssub(ex, 3.0f, t), smul(ex, r, 0.5f));
    o[0] = smul(ex, x, k); o[1] = smul(ex, y, k); o[2] = smul(ex, z, k);
}
static void d3dx_vec3_normalize(CPU *c){
    uint32_t out = arg(c, 0), V = arg(c, 1);
    float x = ldf(V), y = ldf(V + 4u), z = ldf(V + 8u);
    /* plain operations: s is a NaN for the lane-by-lane rule exactly when it is one here, and only
     * then do its bits differ, but a NaN takes the zero branch (comiss: unordered) */
    float s = (x * x + y * y) + z * z;
    if (!(s >= NORM_MIN)){ MR32(out) = 0; MR32(out + 4u) = 0; MR32(out + 8u) = 0; }
    else {
        float o[3];
        norm_k(0, x, y, z, s, o);
        if (o[0] != o[0] || o[1] != o[1] || o[2] != o[2]){ g_d3dxmath_exact++; norm_k(1, x, y, z, s, o); }
        stf(out, o[0]); stf(out + 4u, o[1]); stf(out + 8u, o[2]);
    }
    ret(c, 2, out);
}

/* ---- D3DXVec3TransformCoordArray(out, out_stride, v, v_stride, m, n), SSE kernel ----
 * n <= 12: each vector by the SSE D3DXVec3TransformCoord kernel; more: groups of four in one pass
 * (the matrix read once, before the first group, its sums in another order), then the rest one by
 * one.  1/w is the kernel's: r = rcp(w), (r + r) - (r w) r. */
AI v4 tc1_k(int ex, float x, float y, float z, v4 r0, v4 r1, v4 r2, v4 r3){
    v4 p = vadd(ex, vadd(ex, vmul(ex, r0, bc(x)), vmul(ex, r1, bc(y))), vadd(ex, vmul(ex, r2, bc(z)), r3));
    v4 r = vrcp(p);
    v4 inv = vsub(ex, vadd(ex, r, r), vmul(ex, vmul(ex, r, p), r));
    return vmul(ex, p, bc(inv[3]));
}
static void tc1(uint32_t out, uint32_t V, uint32_t M){        /* D3DXVec3TransformCoord, SSE */
    float x = ldf(V), y = ldf(V + 4u), z = ldf(V + 8u);
    v4 r0 = ld4(M), r1 = ld4(M + 16u), r2 = ld4(M + 32u), r3 = ld4(M + 48u);
    v4 o = tc1_k(0, x, y, z, r0, r1, r2, r3);
    if (nan3(o)){ g_d3dxmath_exact++; o = tc1_k(1, x, y, z, r0, r1, r2, r3); }
    st2(out, o); stf(out + 8u, o[2]);
}
/* four vectors: X, Y, Z hold component k of vector k (lane k); m[e] is element e broadcast */
AI void tc4_k(int ex, v4 X, v4 Y, v4 Z, const float m[16], v4 *ox, v4 *oy, v4 *oz){
    v4 w = vadd(ex, vadd(ex, vadd(ex, vmul(ex, X, bc(m[3])), vmul(ex, Y, bc(m[7]))), vmul(ex, Z, bc(m[11]))), bc(m[15]));
    v4 r = vrcp(w);
    v4 inv = vsub(ex, vadd(ex, r, r), vmul(ex, vmul(ex, r, w), r));
    v4 tx = vadd(ex, vadd(ex, vadd(ex, vmul(ex, X, bc(m[0])), vmul(ex, Y, bc(m[4]))), vmul(ex, Z, bc(m[8]))), bc(m[12]));
    v4 tz = vadd(ex, vadd(ex, vadd(ex, vmul(ex, X, bc(m[2])), vmul(ex, Y, bc(m[6]))), vmul(ex, Z, bc(m[10]))), bc(m[14]));
    v4 ty = vadd(ex, vadd(ex, vadd(ex, vmul(ex, X, bc(m[1])), vmul(ex, Y, bc(m[5]))), vmul(ex, Z, bc(m[9]))), bc(m[13]));
    *ox = vmul(ex, tx, inv); *oy = vmul(ex, ty, inv); *oz = vmul(ex, tz, inv);
}
static void d3dx_vec3_transform_coord_array(CPU *c){
    uint32_t out = arg(c, 0), os = arg(c, 1), V = arg(c, 2), vs = arg(c, 3), M = arg(c, 4), n = arg(c, 5);
    uint32_t groups = n > 12u ? (n & ~3u) >> 2 : 0u, rest = n - 4u * groups, o = out, v = V;
    if (groups){
        float m[16]; ld16(m, M);
        for (uint32_t g = 0; g < groups; g++){
            /* the reads (lanes: vector 0..3), then the writes in the kernel's order */
            float x0 = ldf(v), y0 = ldf(v + 4u), x1 = ldf(v + vs), y1 = ldf(v + vs + 4u);
            float x2 = ldf(v + 2u * vs), y2 = ldf(v + 2u * vs + 4u), z0 = ldf(v + 8u);
            float x3 = ldf(v + 3u * vs), y3 = ldf(v + 3u * vs + 4u);
            float z2 = ldf(v + 2u * vs + 8u), z3 = ldf(v + 3u * vs + 8u), z1 = ldf(v + vs + 8u);
            v4 X = { x0, x1, x2, x3 }, Y = { y0, y1, y2, y3 }, Z = { z0, z1, z2, z3 }, ox, oy, oz;
            tc4_k(0, X, Y, Z, m, &ox, &oy, &oz);
            if (nan4(ox) | nan4(oy) | nan4(oz)){ g_d3dxmath_exact++; tc4_k(1, X, Y, Z, m, &ox, &oy, &oz); }
            st2(o, (v4){ ox[0], oy[0], 0, 0 }); st2(o + os, (v4){ ox[1], oy[1], 0, 0 });
            stf(o + 8u, oz[0]); stf(o + os + 8u, oz[1]);
            st2(o + 2u * os, (v4){ ox[2], oy[2], 0, 0 }); st2(o + 3u * os, (v4){ ox[3], oy[3], 0, 0 });
            stf(o + 2u * os + 8u, oz[2]); stf(o + 3u * os + 8u, oz[3]);
            v += 4u * vs; o += 4u * os;
        }
    }
    for (uint32_t k = 0; k < rest; k++){ tc1(o, v, M); v += vs; o += os; }
    ret(c, 6, out);
}

/* ---- D3DXMatrixInverse(out, det, m), SSE kernel (Cramer's rule on transposed pairs, Intel's
 * AP-928 scheme), transcribed instruction by instruction; lane by lane only (never reached in MW).
 * A determinant of 0 or a NaN: NULL, nothing written. ---- */
AI float rcp1(float x){ return 1.0f / x; }        /* rcpss as lifted */
static void d3dx_matrix_inverse(CPU *c){
    uint32_t out = arg(c, 0), pdet = arg(c, 1), M = arg(c, 2);
    const int ex = 1;
    v4 x0 = { 0, 0, 0, 0 }, x1, x2, x3, x4, x5, x6, x7;
    v4 s_d0, s_c0, s_b0, s_a0, s_90, s_80, s_70, s_60, s_50, s_40, s_30, s_20, s_e0;
#define LOHI(a, b) ((v4){ ldf(M + (a)), ldf(M + (a) + 4u), ldf(M + (b)), ldf(M + (b) + 4u) })
    x2 = LOHI(0x08u, 0x18u); x4 = LOHI(0x28u, 0x38u); x3 = LOHI(0x20u, 0x30u); x1 = LOHI(0x00u, 0x10u);
#undef LOHI
    x5 = x2; x5 = SHUF(x5, x4, 0x88); x4 = SHUF(x4, x2, 0xdd);
    x2 = x4; x2 = vmul(ex, x2, x5); x2 = SHUF(x2, x2, 0xb1);
    x6 = x2; x6 = SHUF(x6, x6, 0x4e);
    x7 = x3; x3 = SHUF(x3, x1, 0xdd); x1 = SHUF(x1, x7, 0x88);
    x7 = x3; x3 = vmul(ex, x3, x6); x6 = vmul(ex, x6, x1); s_d0 = x6;
    x6 = x7; x7 = vmul(ex, x7, x2); x2 = vmul(ex, x2, x1); x3 = vsub(ex, x3, x7);
    x7 = x6; x7 = vmul(ex, x7, x5); x5 = SHUF(x5, x5, 0x4e); x7 = SHUF(x7, x7, 0xb1); s_c0 = x2;
    x2 = x4; x2 = vmul(ex, x2, x7); x2 = vadd(ex, x2, x3);
    x3 = x7; x7 = SHUF(x7, x7, 0x4e); x3 = vmul(ex, x3, x1); s_b0 = x3;
    x3 = x4; x3 = vmul(ex, x3, x7); x7 = vmul(ex, x7, x1); x2 = vsub(ex, x2, x3);
    x3 = x6; x3 = SHUF(x3, x3, 0x4e); x3 = vmul(ex, x3, x4); x3 = SHUF(x3, x3, 0xb1); s_a0 = x7;
    x7 = x5; x5 = vmul(ex, x5, x3); x5 = vadd(ex, x5, x2);
    x2 = x3; x3 = SHUF(x3, x3, 0x4e); x2 = vmul(ex, x2, x1); s_90 = x4;
    x4 = x7; x7 = vmul(ex, x7, x3); x3 = vmul(ex, x3, x1); x5 = vsub(ex, x5, x7); x3 = vsub(ex, x3, x2);
    x2 = x1; x1 = vmul(ex, x1, x5); x3 = SHUF(x3, x3, 0x4e);
    x7 = x1; x1 = SHUF(x1, x1, 0x4e); s_e0 = x5; x1 = vadd(ex, x1, x7);
    x5 = x1; x1 = SHUF(x1, x1, 0xb1); x1[0] = sadd(ex, x1[0], x5[0]);
    x5 = x6; x5 = vmul(ex, x5, x2); x5 = SHUF(x5, x5, 0xb1);
    x7 = x5; x5 = SHUF(x5, x5, 0x4e); s_80 = x4; x4 = s_90; s_70 = x6;
    x6 = x4; x6 = vmul(ex, x6, x7); x6 = vadd(ex, x6, x3);
    x3 = x4; x3 = vmul(ex, x3, x5); x3 = vsub(ex, x3, x6);
    x6 = x4; x6 = vmul(ex, x6, x2); x6 = SHUF(x6, x6, 0xb1); s_60 = x5; x5 = s_70; s_50 = x7;
    x7 = x6; x7 = vmul(ex, x7, x5); x7 = vadd(ex, x7, x3);
    x3 = x6; x3 = SHUF(x3, x3, 0x4e); s_40 = x4;
    x4 = x5; x5 = vmul(ex, x5, x3); s_30 = x4;
    x4 = x6; x6 = x7; x6 = vsub(ex, x6, x5);
    x5 = s_d0; x7 = s_c0; x5 = vsub(ex, x5, x7); x5 = SHUF(x5, x5, 0x4e);
    x7 = s_80; x4 = vmul(ex, x4, x7); x3 = vmul(ex, x3, x7); x5 = vsub(ex, x5, x4);
    x2 = vmul(ex, x2, x7); x3 = vadd(ex, x3, x5); x2 = SHUF(x2, x2, 0xb1);
    x4 = x2; x4 = SHUF(x4, x4, 0x4e);
    x5 = s_40; s_20 = x6; x6 = x5; x5 = vmul(ex, x5, x2); x6 = vmul(ex, x6, x4); x5 = vadd(ex, x5, x3);
    x3 = x4; x4 = x5; x4 = vsub(ex, x4, x6);
    x5 = s_a0; x6 = s_b0; x5 = vsub(ex, x5, x6); x5 = SHUF(x5, x5, 0x4e);
    x6 = s_50; x6 = vmul(ex, x6, x7); x6 = vsub(ex, x6, x5);
    x5 = s_60; x7 = vmul(ex, x7, x5); x6 = vsub(ex, x6, x7);
    x5 = s_30; x2 = vmul(ex, x2, x5); x5 = vmul(ex, x5, x3); x6 = vsub(ex, x6, x2);
    x2 = x5; x2 = vadd(ex, x2, x6);
    float det = x1[0];
    if (det == x0[0] || det != det){ ret(c, 3, 0); return; }    /* comiss: equal or unordered */
    x6 = s_20;
    if (pdet) stf(pdet, det);
    x0 = x1; x1 = s_e0;
    x3 = x0; x0[0] = rcp1(x0[0]); x5 = x0;
    x0[0] = smul(ex, x0[0], x0[0]); x3[0] = smul(ex, x3[0], x0[0]);
    x5[0] = sadd(ex, x5[0], x5[0]); x5[0] = ssub(ex, x5[0], x3[0]); x5 = SHUF(x5, x5, 0);
    x1 = vmul(ex, x1, x5); st2(out, x1); st2h(out + 8u, x1);
    x4 = vmul(ex, x4, x5); st2(out + 16u, x4); st2h(out + 24u, x4);
    x6 = vmul(ex, x6, x5); st2(out + 32u, x6); st2h(out + 40u, x6);
    x5 = vmul(ex, x5, x2); st2(out + 48u, x5); st2h(out + 56u, x5);
    ret(c, 3, out);
}

/* ---- the x87 ones: the lift's model of each operation (cpu.h), with the caller's control word ---- */
#define X2(a, b, r) x87_r2pc(c->fcw, (a), (b), (r))
/* fld m32 / fstp m32, as the lifted code writes it */
AI void x87_move(uint32_t dst, uint32_t src){ MRF32(dst) = (float)(double)MRF32(src); }

/* D3DXMatrixTranslation(out, x, y, z), x87 default: fldz/fld1 stores, x y z copied as integers */
static void d3dx_matrix_translation(CPU *c){
    uint32_t out = arg(c, 0);
    static const uint8_t zero_at[] = { 0x2c, 0x24, 0x20, 0x1c, 0x18, 0x10, 0x0c, 0x08, 0x04 }, one_at[] = { 0x3c, 0x28, 0x14, 0x00 };
    MRF32(out + 0x2cu) = 0.0f;
    MR32(out + 0x30u) = arg(c, 1); MRF32(out + 0x24u) = 0.0f;
    MR32(out + 0x34u) = arg(c, 2); MRF32(out + 0x20u) = 0.0f;
    MR32(out + 0x38u) = arg(c, 3);
    for (unsigned k = 3; k < sizeof zero_at; k++) MRF32(out + zero_at[k]) = 0.0f;
    for (unsigned k = 0; k < sizeof one_at; k++) MRF32(out + one_at[k]) = 1.0f;
    ret(c, 4, out);
}

/* D3DXMatrixTranspose(out, m), x87 default: the pairs swapped one by one (each pair's two reads before
 * its writes, so out == m works), half through the x87, half as integers; the diagonal copied unless
 * out == m */
static void d3dx_matrix_transpose(CPU *c){
    uint32_t out = arg(c, 0), m = arg(c, 1);
    float a = (float)(double)MRF32(m + 0x04u), b = (float)(double)MRF32(m + 0x10u);
    MRF32(out + 0x04u) = b; MRF32(out + 0x10u) = a;
    static const uint8_t pr[5][2] = { { 0x20, 0x08 }, { 0x30, 0x0c }, { 0x24, 0x18 }, { 0x34, 0x1c }, { 0x38, 0x2c } };
    for (int k = 0; k < 5; k++){                 /* edx = [m + p0]; fld [m + p1]; [out + p1] = edx; fstp [out + p0] */
        uint32_t i = MR32(m + pr[k][0]);
        float f = (float)(double)MRF32(m + pr[k][1]);
        MR32(out + pr[k][1]) = i; MRF32(out + pr[k][0]) = f;
    }
    if (out != m){
        MR32(out) = MR32(m); MR32(out + 0x14u) = MR32(m + 0x14u);
        MR32(out + 0x28u) = MR32(m + 0x28u); MR32(out + 0x3cu) = MR32(m + 0x3cu);
    }
    ret(c, 2, out);
}

/* D3DXMatrixOrthoLH(out, w, h, zn, zf): 2/w, 2/h, q = 1/(zf - zn), -(q zn), 1 */
static void d3dx_matrix_ortho_lh(CPU *c){
    uint32_t out = arg(c, 0), e = c->esp + 4u;
    double t = 2.0, s = (double)MRF32(e + 4u);
    t = X2(t, s, t / s); MRF32(out) = (float)t;
    for (uint32_t k = 1; k <= 4; k++) MRF32(out + 4u * k) = 0.0f;
    t = 2.0; s = (double)MRF32(e + 8u);
    t = X2(t, s, t / s); MRF32(out + 0x14u) = (float)t;
    for (uint32_t k = 6; k <= 9; k++) MRF32(out + 4u * k) = 0.0f;
    t = (double)MRF32(e + 16u); s = (double)MRF32(e + 12u);
    t = X2(t, s, t - s);
    s = 1.0; t = X2(t, s, s / t);                /* fdivr m32 */
    MRF32(out + 0x28u) = (float)t;
    for (uint32_t k = 11; k <= 13; k++) MRF32(out + 4u * k) = 0.0f;
    s = (double)MRF32(e + 12u); t = X2(t, s, t * s); t = -t;
    MRF32(out + 0x38u) = (float)t; MRF32(out + 0x3cu) = 1.0f;
    ret(c, 5, out);
}

/* D3DXMatrixPerspectiveLH(out, w, h, zn, zf): 2zn/w, 2zn/h, q = zf/(zf - zn), 1, -(q zn) */
static void d3dx_matrix_perspective_lh(CPU *c){
    uint32_t out = arg(c, 0), e = c->esp + 4u;
    double a = (double)MRF32(e + 12u);
    a = X2(a, a, a + a);                          /* fadd st0, st0: 2 zn */
    double b = (double)MRF32(e + 4u);
    b = X2(b, a, a / b);                          /* fdivr st0, st1 */
    MRF32(out) = (float)b;
    for (uint32_t k = 1; k <= 4; k++) MRF32(out + 4u * k) = 0.0f;
    double s = (double)MRF32(e + 8u);
    a = X2(a, s, a / s); MRF32(out + 0x14u) = (float)a;
    for (uint32_t k = 6; k <= 9; k++) MRF32(out + 4u * k) = 0.0f;
    double t = (double)MRF32(e + 16u); s = (double)MRF32(e + 12u);
    t = X2(t, s, t - s);
    s = (double)MRF32(e + 16u); t = X2(t, s, s / t);           /* fdivr m32 */
    MRF32(out + 0x28u) = (float)t; MRF32(out + 0x2cu) = 1.0f;
    MRF32(out + 0x30u) = 0.0f; MRF32(out + 0x34u) = 0.0f;
    s = (double)MRF32(e + 12u); t = X2(t, s, t * s); t = -t;
    MRF32(out + 0x38u) = (float)t; MRF32(out + 0x3cu) = 0.0f;
    ret(c, 5, out);
}
#undef X2

/* ---- the table: speed.exe's IAT slot and thunk of each, the arguments, the answer ---- */
const D3dxMathFn d3dxmath_fns[D3DXMATH_COUNT] = {
    { "D3DXMatrixMultiply",          0x008904b0u, 0x007e7d5eu, 3, d3dx_matrix_multiply },
    { "D3DXVec4Transform",           0x008904b4u, 0x007e7d64u, 3, d3dx_vec4_transform },
    { "D3DXVec3Transform",           0x008904b8u, 0x007e7d6au, 3, d3dx_vec3_transform },
    { "D3DXMatrixInverse",           0x008904bcu, 0x007e7d70u, 3, d3dx_matrix_inverse },
    { "D3DXMatrixOrthoLH",           0x008904c8u, 0x007e7d82u, 5, d3dx_matrix_ortho_lh },
    { "D3DXMatrixPerspectiveLH",     0x008904ccu, 0x007e7d88u, 5, d3dx_matrix_perspective_lh },
    { "D3DXMatrixTranslation",       0x008904d0u, 0x007e7d8eu, 4, d3dx_matrix_translation },
    { "D3DXVec3TransformCoordArray", 0x008904d4u, 0x007e7d94u, 6, d3dx_vec3_transform_coord_array },
    { "D3DXVec3Normalize",           0x008904d8u, 0x007e7d9au, 2, d3dx_vec3_normalize },
    { "D3DXVec3TransformNormal",     0x008904dcu, 0x007e7da0u, 3, d3dx_vec3_transform_normal },
    { "D3DXMatrixTranspose",         0x008904e0u, 0x007e7d58u, 2, d3dx_matrix_transpose },
};

/* game_mw.c's install, after win32_init (the slots hold IMP_MAGIC + index) and before module.c's
 * module_load_imports, which leaves a slot bound here to this answer.  Each thunk is checked first:
 * `jmp [slot]` (FF 25 <slot>).  A slot is bound only once the dispatch table took its thunk's patch;
 * one it did not take stays on IMP_MAGIC + index: the oracle's export where one is linked, else an
 * [hle] UNIMPLEMENTED line at its first call */
int d3dxmath_install(int oracle_linked){
    const char *e = getenv("NFSMW_D3DX_MATH");
    if (e && !strcmp(e, "oracle")){
        if (oracle_linked){
            fprintf(stderr, "[d3dx] NFSMW_D3DX_MATH=oracle: the %d math imports are left to the D3DX oracle\n", D3DXMATH_COUNT);
            return 0;
        }
        fprintf(stderr, "[d3dx] NFSMW_D3DX_MATH=oracle, but this build has no D3DX oracle: the native math answers\n");
    }
    int n = 0;
    for (int i = 0; i < D3DXMATH_COUNT; i++){
        const D3dxMathFn *f = &d3dxmath_fns[i];
        if (MR8(f->thunk) != 0xFFu || MR8(f->thunk + 1u) != 0x25u || MR32(f->thunk + 2u) != f->iat){
            fprintf(stderr, "[d3dx] %s: no `jmp [0x%08x]` at 0x%08x, left unanswered\n", f->name, f->iat, f->thunk);
            continue;
        }
        if (!dispatch_patch_take(f->thunk, f->fn)){
            fprintf(stderr, "[d3dx] %s: the dispatch table took no patch at 0x%08x, its import left unbound\n", f->name, f->thunk);
            continue;
        }
        MR32(f->iat) = f->thunk;
        n++;
    }
    fprintf(stderr, "[d3dx] %d of %d D3DX math imports answered natively\n", n, D3DXMATH_COUNT);
    return n;
}
