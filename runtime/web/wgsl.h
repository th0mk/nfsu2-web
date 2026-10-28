/* WGSL backend of the D3D9 shader translator (runtime/web/vsemu_wgsl.inc,
 * compiled as part of vsemu.c) and the shader dump used to validate it.
 * The contract with the WebGPU renderer is docs/web/gpu-abi.md. */
#ifndef WGSL_H
#define WGSL_H
#include <stddef.h>
#include <stdint.h>
#include "vsemu.h"

/* Same signatures and return convention as the MSL emitters (length of the
 * text, 0 = cannot translate).  Entry points: vs_<id> / ps_<id>. */
int vs_to_wgsl(const VSProg *p, int id, const int decl[16][2], char *out, size_t cap);
int ps_to_wgsl(const VSProg *p, int id, char *out, size_t cap);
int ps_to_wgsl_ex(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask);
int ps_to_wgsl_ex2(const VSProg *p, int id, char *out, size_t cap, unsigned cubemask, int reflk);
/* Built-in modules for draws without a game shader: vs_0 (TVert pass-through)
 * and ps_<id> = col0 * tex0 (id 0, cube twin 256 with cubemask 1). */
int wgsl_tvert_module(char *out, size_t cap);
int wgsl_ff_module(int id, char *out, size_t cap, unsigned cubemask);
/* Compute module checking the vertex-pulling helpers (shadercheck test). */
int wgsl_pull_test_module(char *out, size_t cap);

#ifdef VSEMU_WGSL
/* Native test tools only: ps_1_x/ps_2_0 reference with the Mac's semantics
 * (runtime/web/vsemu_psref.inc), the oracle of web/tools/wgsl-xl.c; it refuses
 * ps_2_x and ps_3_0, whose reference is web/tools/psref.h (D3D9's rules). */
typedef struct { float col[2][4], uv[4][4], fog; } PSRefIn;
typedef struct { float aref, proj[4], c[32][4], fog[4]; } PSRefPar;
typedef void (*PSRefTex)(int stage, const float *coord, int ncoord, float out[4]);
int ps_ref_run(const VSProg *p, const PSRefPar *P, const PSRefIn *in, PSRefTex tex,
               unsigned cubemask, float out[4], int *killed);
#endif

/* NFSU2_SHADER_DUMP=<dir>: write every shader the game creates and every
 * module registered with the renderer (runtime/web/shader_dump.c). */
int  wgsl_dump_on(void);
void wgsl_dump_obj(const char *kind, int idx, const void *tok, size_t bytes, const VSProg *p);
void wgsl_dump_vs(int gid, int vsobj, uint32_t decl, const int dcl[16][2], const char *wgsl, int len);
void wgsl_dump_ps(int id, int psobj, const char *wgsl, int len);
/* CreateVertexDeclaration: the D3DVERTEXELEMENT9 array, keyed by the COM
 * object address that SetVertexDeclaration (and the vs index line) uses. */
void wgsl_dump_decl(uint32_t obj, const void *elems, size_t bytes);
#endif
