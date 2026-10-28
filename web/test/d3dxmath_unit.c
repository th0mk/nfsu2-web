/* Most Wanted's native D3DX math (runtime/mw/d3dxmath_mw.c, TRACKER M2-08)
 * alone: no game files, no oracle (web/test/d3dx-math.test.mjs).
 *
 *   d3dxmath_unit.wasm
 *
 * Each answer is called as speed.exe's thunk calls it (stdcall, the return
 * address at esp).  Two parts:
 *   - known answers: results that follow from the kernels' operations by hand
 *     (exact products, the default NaN of 0 x inf, which NaN operand an SSE
 *     operation keeps, the operand order D3DXVec3TransformNormal takes by the
 *     matrix's alignment, Normalize's threshold and its +0 zeros,
 *     Translation's bit copies, Transpose in place, an exact inverse, a
 *     singular matrix, the 1/w step of D3DXVec3TransformCoordArray on both of
 *     its paths);
 *   - the cases of web/test/d3dxmath_cases.h: each call returns to its
 *     caller with its arguments popped and the caller's registers kept, and a
 *     digest of the answers per function.  web/test/d3dx_oracle_unit.c "math"
 *     prints the same digests where it has proved every case equal to the
 *     D3DX oracle's, so equal digests here carry that proof to builds without
 *     the oracle.
 * Results are "T" lines; a failed check prints "FAIL"; the end is
 * "d3dxmath_unit: <n> passed, <m> failed". */
#include "cpu.h"
#include "mw/d3dxmath_mw.h"
#include "d3dxmath_cases.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int pass, fail;
#define CHECK(cond, ...) do { if (cond) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } } while (0)
static void tr(const char *fmt, ...){
    char b[400]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("T %s\n", b);
}

/* ---- what d3dxmath_mw.c needs of the runtime ---- */
void dispatch_patch(uint32_t a, void (*f)(CPU *)){ (void)a; (void)f; }

/* guest memory: linear memory below the program's data (0x28000000, web/tools/win32unit.mjs) */
#define WIN     0x07F80000u              /* the cases' window: d3dx_oracle_unit.c's MATH_W (eax is in the digest) */
#define KA      0x01100000u              /* known answers: inputs and outputs */
#define STACK   0x02000000u
#define RA      0x00401000u

static CPU g_cpu;
static const D3dxMathFn *fn_named(const char *n){
    for (int i = 0; i < D3DXMATH_COUNT; i++) if (!strcmp(d3dxmath_fns[i].name, n)) return &d3dxmath_fns[i];
    return 0;
}
/* the call as the thunk makes it; 1 = returned to the caller with the arguments popped and ebx, esi,
 * edi, ebp, the control word and the x87 top kept */
static int call(const D3dxMathFn *f, const uint32_t *args, uint16_t fcw, uint32_t *eax){
    CPU *c = &g_cpu;
    c->esp = STACK; c->eax = 0xA1A1A1A1u; c->ebx = 0x0B0B0B0Bu; c->esi = 0x05050505u; c->edi = 0x0D0D0D0Du;
    c->ebp = 0x0E0E0E0Eu; c->fcw = fcw; c->top = 5;
    for (int k = f->nargs - 1; k >= 0; k--) PUSH32(args[k]);
    PUSH32(RA);
    f->fn(c);
    if (eax) *eax = c->eax;
    return c->esp == STACK && c->eip == RA && c->ebx == 0x0B0B0B0Bu && c->esi == 0x05050505u && c->edi == 0x0D0D0D0Du
        && c->ebp == 0x0E0E0E0Eu && c->fcw == fcw && c->top == 5;
}
static uint32_t fb(float f){ uint32_t b; memcpy(&b, &f, 4); return b; }
static void putf(uint32_t a, float f){ MRF32(a) = f; }
static void putm(uint32_t a, const float m[16]){ for (int k = 0; k < 16; k++) MRF32(a + 4u * (uint32_t)k) = m[k]; }

static void known(void){
    uint32_t eax;
    const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const float M[16] = { 1.5f, -2.25f, 3.0f, 0.5f, 7.0f, 0.125f, -1.0f, 2.0f, 9.5f, -3.5f, 4.0f, 0.75f, 10.0f, 20.0f, -30.0f, 1.0f };
    const D3dxMathFn *mul = fn_named("D3DXMatrixMultiply"), *tn = fn_named("D3DXVec3TransformNormal"),
        *nrm = fn_named("D3DXVec3Normalize"), *tr3 = fn_named("D3DXMatrixTranslation"), *tp = fn_named("D3DXMatrixTranspose"),
        *inv = fn_named("D3DXMatrixInverse"), *tca = fn_named("D3DXVec3TransformCoordArray"),
        *per = fn_named("D3DXMatrixPerspectiveLH"), *ort = fn_named("D3DXMatrixOrthoLH");
    const uint32_t A = KA, B = KA + 0x100u, O = KA + 0x200u, V = KA + 0x300u;

    /* MatrixMultiply: I x M = M exactly (no -0 or infinity in M); the result's address in eax */
    putm(A, I); putm(B, M);
    { uint32_t a[3] = { O, A, B }; CHECK(call(mul, a, 0x007f, &eax) && eax == O, "MatrixMultiply: stdcall, eax 0x%08x", eax); }
    int same = 1; for (int k = 0; k < 16; k++) same &= MRF32(O + 4u * (uint32_t)k) == M[k];
    CHECK(same, "MatrixMultiply: I x M == M");
    /* in place (out == m1): every read before the first write */
    putm(A, M); putm(B, I);
    { uint32_t a[3] = { A, A, B }; call(mul, a, 0x007f, 0); }
    same = 1; for (int k = 0; k < 16; k++) same &= MRF32(A + 4u * (uint32_t)k) == M[k];
    CHECK(same, "MatrixMultiply: M x I into M == M");
    /* 0 x infinity: the default NaN (0xFFC00000), kept through the sums */
    putm(A, I); putm(B, M); MR32(A) = 0x7f800000u; MR32(B) = 0;
    { uint32_t a[3] = { O, A, B }; call(mul, a, 0x007f, 0); }
    CHECK(MR32(O) == 0xffc00000u, "MatrixMultiply: inf x 0 -> 0x%08x (the default NaN)", MR32(O));
    /* two NaNs: the first operand's; rows 0-2 multiply m1's element by m2's row, row 3 m2's row by m1's element */
    putm(A, I); putm(B, M); MR32(A) = 0x7fc0aaaau; MR32(B) = 0x7fc0bbbbu; MR32(A + 48u) = 0x7fc0ccccu;
    { uint32_t a[3] = { O, A, B }; call(mul, a, 0x007f, 0); }
    CHECK(MR32(O) == 0x7fc0aaaau, "MatrixMultiply: row 0 keeps m1's NaN (0x%08x)", MR32(O));
    CHECK(MR32(O + 48u) == 0x7fc0bbbbu, "MatrixMultiply: row 3 keeps m2's NaN (0x%08x)", MR32(O + 48u));

    /* Vec3TransformNormal: both NaN, the matrix aligned -> the vector's NaN, not aligned -> the matrix's */
    for (int al = 0; al < 2; al++){
        uint32_t m = al ? A : A + 4u;
        putm(m, I); MR32(m) = 0x7fc0bbbbu;
        MR32(V) = 0x7fc0aaaau; putf(V + 4u, 1.0f); putf(V + 8u, 2.0f);
        uint32_t a[3] = { O, V, m }; call(tn, a, 0x007f, 0);
        CHECK(MR32(O) == (al ? 0x7fc0aaaau : 0x7fc0bbbbu), "TransformNormal, matrix %saligned: x = 0x%08x", al ? "" : "not ", MR32(O));
    }

    /* Normalize: (3, 4, 0) has length 1 to an ulp, z +0; below 2^-46 and NaN: three +0, whatever the signs */
    putf(V, 3.0f); putf(V + 4u, 4.0f); putf(V + 8u, 0.0f);
    { uint32_t a[2] = { O, V }; CHECK(call(nrm, a, 0x007f, &eax) && eax == O, "Normalize: stdcall"); }
    { float x = MRF32(O), y = MRF32(O + 4u); double l = sqrt((double)x * x + (double)y * y);
      CHECK(fabs(l - 1.0) < 2e-7 && fabs(x - 0.6) < 1e-7 && MR32(O + 8u) == 0u, "Normalize(3,4,0) = (%.9g, %.9g, 0x%08x)", x, y, MR32(O + 8u)); }
    const float tiny[3][3] = { { -0x1p-24f, 0, 0 }, { 0x1p-24f, 0x1p-24f, 0x1p-24f }, { NAN, 1, 1 } };
    for (int t = 0; t < 3; t++){
        for (int k = 0; k < 3; k++) putf(V + 4u * (uint32_t)k, tiny[t][k]);
        MR32(O) = MR32(O + 4u) = MR32(O + 8u) = 0xdeadbeefu;
        uint32_t a[2] = { O, V }; call(nrm, a, 0x007f, 0);
        CHECK(MR32(O) == 0 && MR32(O + 4u) == 0 && MR32(O + 8u) == 0, "Normalize: case %d -> +0 x3 (0x%08x 0x%08x 0x%08x)", t,
              MR32(O), MR32(O + 4u), MR32(O + 8u));
    }
    /* 2^-23 on one axis: |v|^2 = 2^-46, the threshold itself: normalised */
    putf(V, 0x1p-23f); putf(V + 4u, 0); putf(V + 8u, 0);
    { uint32_t a[2] = { O, V }; call(nrm, a, 0x007f, 0); }
    CHECK(MRF32(O) == 1.0f, "Normalize(2^-23, 0, 0) = %.9g", MRF32(O));

    /* Translation: x y z copied bit for bit (a signalling NaN too), zeros +0 */
    { uint32_t a[4] = { O, 0x7f800001u, 0x80000000u, fb(2.5f) };
      CHECK(call(tr3, a, 0x007f, &eax) && eax == O, "Translation: stdcall");
      CHECK(MR32(O + 48u) == 0x7f800001u && MR32(O + 52u) == 0x80000000u && MRF32(O + 56u) == 2.5f, "Translation: x y z copied");
      int ok = 1; for (int k = 0; k < 12; k++) ok &= MR32(O + 4u * (uint32_t)k) == ((k % 5 == 0) ? 0x3f800000u : 0u);
      CHECK(ok && MR32(O + 60u) == 0x3f800000u, "Translation: identity elsewhere, +0 zeros"); }

    /* Transpose in place and to another matrix */
    putm(A, M);
    { uint32_t a[2] = { O, A }; call(tp, a, 0x007f, 0); }
    { uint32_t a[2] = { A, A }; CHECK(call(tp, a, 0x007f, &eax) && eax == A, "Transpose: stdcall"); }
    same = 1; for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) same &= MRF32(A + 16u * (uint32_t)r + 4u * (uint32_t)k) == M[4 * k + r]
                                                                         && MRF32(O + 16u * (uint32_t)r + 4u * (uint32_t)k) == M[4 * k + r];
    CHECK(same, "Transpose: to another matrix and in place");

    /* Inverse: diag(2, 4, 8, 16) exactly (det 1024, 1/det exact), and a singular matrix: NULL, nothing written */
    { float D[16] = { 2,0,0,0, 0,4,0,0, 0,0,8,0, 0,0,0,16 }; putm(A, D); putf(V, -1.0f);
      uint32_t a[3] = { O, V, A }; CHECK(call(inv, a, 0x007f, &eax) && eax == O, "Inverse: stdcall, eax 0x%08x", eax);
      int ok = 1; for (int k = 0; k < 16; k++) ok &= MRF32(O + 4u * (uint32_t)k) == (k % 5 ? 0.0f : 1.0f / D[k]);
      CHECK(ok && MRF32(V) == 1024.0f, "Inverse of diag(2,4,8,16): diag(1/2,1/4,1/8,1/16), det %.9g", MRF32(V)); }
    { float S[16] = { 1,2,3,4, 1,2,3,4, 5,6,7,8, 9,1,2,3 }; putm(A, S); MR32(O) = 0x12345678u; putf(V, -1.0f);
      uint32_t a[3] = { O, V, A }; call(inv, a, 0x007f, &eax);
      CHECK(eax == 0 && MR32(O) == 0x12345678u && MRF32(V) == -1.0f, "Inverse of a singular matrix: NULL (0x%08x), nothing written", eax); }

    /* TransformCoordArray: the identity keeps points (w = 1: 1/w = 1 exactly) on both paths, in place too */
    for (uint32_t n = 5; n <= 17; n += 12){
        putm(A, I);
        for (uint32_t k = 0; k < n; k++){ putf(V + 16u * k, 1.0f + (float)k); putf(V + 16u * k + 4u, -2.0f * (float)k); putf(V + 16u * k + 8u, 0.25f); }
        uint32_t a[6] = { V, 16, V, 16, A, n };
        CHECK(call(tca, a, 0x007f, &eax) && eax == V, "TransformCoordArray(%u): stdcall", n);
        int ok = 1;
        for (uint32_t k = 0; k < n; k++) ok &= MRF32(V + 16u * k) == 1.0f + (float)k && MRF32(V + 16u * k + 4u) == -2.0f * (float)k
                                              && MRF32(V + 16u * k + 8u) == 0.25f;
        CHECK(ok, "TransformCoordArray(%u) with the identity, in place: the points kept", n);
    }

    /* PerspectiveLH(2, 2, 1, 101) and OrthoLH(2, 4, 1, 3) at the game thread's word */
    { uint32_t a[5] = { O, fb(2.0f), fb(2.0f), fb(1.0f), fb(101.0f) }; CHECK(call(per, a, 0x007f, &eax) && eax == O, "PerspectiveLH: stdcall");
      CHECK(MRF32(O) == 1.0f && MRF32(O + 20u) == 1.0f && MRF32(O + 40u) == 1.01f && MRF32(O + 44u) == 1.0f
            && MRF32(O + 56u) == -1.01f && MR32(O + 60u) == 0u, "PerspectiveLH(2, 2, 1, 101)"); }
    { uint32_t a[5] = { O, fb(2.0f), fb(4.0f), fb(1.0f), fb(3.0f) }; CHECK(call(ort, a, 0x007f, &eax) && eax == O, "OrthoLH: stdcall");
      CHECK(MRF32(O) == 1.0f && MRF32(O + 20u) == 0.5f && MRF32(O + 40u) == 0.5f && MRF32(O + 56u) == -0.5f
            && MRF32(O + 60u) == 1.0f && MR32(O + 44u) == 0u, "OrthoLH(2, 4, 1, 3)"); }
}

int main(void){
    known();
    for (int f = 0; f < D3DXMATH_COUNT; f++){
        const D3dxMathFn *fn = &d3dxmath_fns[f];
        uint64_t h = MC_FNV0; int regs_bad = 0;
        for (int i = 0; i < MC_CASES; i++){
            MathCase mc; uint32_t eax;
            mc_case(f, i, WIN, &mc);
            regs_bad += !call(fn, mc.args, mc.fcw, &eax);
            h = mc_fnv(mc_fnv(h, (const uint8_t *)GMEM_ + WIN, MC_WIN), &eax, 4);
        }
        tr("math %s: %d cases, digest %016llx", fn->name, MC_CASES, (unsigned long long)h);
        CHECK(regs_bad == 0, "%s: each call returns to its caller, stdcall, the caller's registers kept (%d not)", fn->name, regs_bad);
    }
    printf("d3dxmath_unit: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
