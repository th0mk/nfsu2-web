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
/* Texture stages of every module, 4 (the default) or 8 (gpu-abi.md section
 * 11.2): set once, before the first translation. */
void wgsl_set_stages(unsigned n);
unsigned wgsl_get_stages(void);
/* The binding kinds (GW_BK_*, 2 bits per stage) of the last pixel shader
 * translated: how its module declares each stage (GW_SHADER_PS a[2]). */
unsigned wgsl_ps_kinds(void);
/* The stages (bit k) that sample a depth texture with a depth compare in the
 * pixel shaders translated until it is set again: a draw's depth variant
 * (gpu-abi.md section 11.4).  8 stages only; 0 = none (the default). */
void wgsl_set_depth_stages(unsigned mask);
/* Why the last ps_2_x, ps_3_0 or vs_3_0 translation returned 0 ("" when it
 * did not; runtime/web/vsemu_sm3.inc). */
const char *wgsl_refusal(void);

#ifdef VSEMU_WGSL
/* Native test tools only: ps_1_x/ps_2_0 reference with the Mac's semantics
 * (runtime/web/vsemu_psref.inc), the oracle of web/tools/wgsl-xl.c; it refuses
 * ps_2_x and ps_3_0, whose reference is web/tools/psref.h (D3D9's rules). */
typedef struct { float col[2][4], uv[8][4], fog; } PSRefIn;     /* uv4..uv7: 8 stages only */
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
/* the same with the translator's reason for a refusal (len <= 0), written after REJECTED */
void wgsl_dump_ps_why(int id, int psobj, const char *wgsl, int len, const char *why);
/* CreateVertexDeclaration: the D3DVERTEXELEMENT9 array, keyed by the COM
 * object address that SetVertexDeclaration (and the vs index line) uses. */
void wgsl_dump_decl(uint32_t obj, const void *elems, size_t bytes);
#endif
