/* D3D9 reference interpreter for the shader models Most Wanted adds to UG2's:
 * ps_2_x, ps_3_0 and vs_3_0 (docs/web/psref.md).  Native test tools only;
 * the game does not carry it (it lives here, not in runtime/web, whose every
 * .c file goes into the game's module).
 *
 * It decodes the bytecode itself (it shares nothing with vsemu.c, so a
 * decoder bug in the translator cannot hide in both), refuses a program the
 * D3D9 rules for its model do not allow, and runs a pixel shader on a 2x2
 * quad (dsx/dsy and the implicit level of detail of texld come from the
 * neighbours) and a vertex shader on one vertex.  Whatever D3D9 leaves
 * undefined (a temporary read before it is written, a loop count outside
 * [0, 255], a gradient in divergent flow, ...) is an error, not a value.
 * ps_1_x and ps_2_0 stay with ps_ref_run (runtime/web/vsemu_psref.inc). */
#ifndef PSREF_H
#define PSREF_H
#include <stddef.h>
#include <stdint.h>

#define PSREF_PS_2_X 0xFFFF0201u
#define PSREF_PS_3_0 0xFFFF0300u
#define PSREF_VS_3_0 0xFFFE0300u

/* D3DPSHADERCAPS2_0.Caps bits */
#define PSREF_PS20_ARBITRARYSWIZZLE    0x01u
#define PSREF_PS20_GRADIENTINSTRUCTIONS 0x02u
#define PSREF_PS20_PREDICATION         0x04u
#define PSREF_PS20_NODEPENDENTREADLIMIT 0x08u
#define PSREF_PS20_NOTEXINSTRUCTIONLIMIT 0x10u

/* What the device reports (D3DCAPS9), which a program is validated and run against. */
typedef struct {
    unsigned ps20_caps;      /* D3DPSHADERCAPS2_0.Caps (PSREF_PS20_*) */
    int ps20_dynamic_depth;  /* .DynamicFlowControlDepth, 0..24 */
    int ps20_num_temps;      /* .NumTemps, 12..32 */
    int ps20_static_depth;   /* .StaticFlowControlDepth, 0..4 */
    unsigned max_vs_const;   /* MaxVertexShaderConst, >= 256 */
    unsigned max_executed;   /* Max[PV]ShaderInstructionsExecuted, >= 65535 for 3_0 */
    int num_mrt;             /* NumSimultaneousRTs, 1..4 (oC0..oC3) */
} PsrefCaps;
/* An SM3 device: every ps_2_x cap at its maximum, 256 vertex shader
 * constants, 65535 instructions executed, 4 render targets. */
void psref_caps_sm3(PsrefCaps *c);

typedef struct PsrefProg PsrefProg;
/* tok: the program from its version token; ndw: dwords that may be read.
 * NULL with the reason in err when the program is not valid for its model. */
PsrefProg *psref_load(const uint32_t *tok, size_t ndw, const PsrefCaps *caps, char *err, size_t errcap);
void psref_free(PsrefProg *p);
uint32_t psref_version(const PsrefProg *p);
size_t psref_ndw(const PsrefProg *p);            /* dwords up to and including the end token */
unsigned psref_ninstr(const PsrefProg *p);       /* instructions, dcl and def included */
unsigned psref_opcount(const PsrefProg *p, unsigned op);   /* instructions with this D3DSIO opcode */
const char *psref_opname(unsigned op);           /* "texldp" etc. ("?" for an unknown opcode) */

/* Declarations: v# (ps_3_0, vs_3_0 with usage), t# and v# (ps_2_x), o# (vs_3_0), s#, vPos, vFace. */
enum { PSREF_DCL_INPUT = 1, PSREF_DCL_TEXCOORD = 3, PSREF_DCL_OUTPUT = 6, PSREF_DCL_SAMPLER = 10,
       PSREF_DCL_MISC = 17 };
typedef struct { int regtype, reg, usage, index; unsigned mask; int smptype; } PsrefDcl;
int psref_ndcl(const PsrefProg *p);
int psref_dcl(const PsrefProg *p, int i, PsrefDcl *d);

/* Constants as the application set them (Set*ShaderConstantF/I/B); a
 * program's own def, defi and defb take precedence in its constant space. */
typedef struct {
    const float (*c)[4]; unsigned nc;
    int i[16][4];
    int b[16];
} PsrefConsts;

/* One texture lookup.  type: 2 = 2D, 3 = cube, 4 = volume (the dcl);
 * c: the coordinate after the instruction's divide (texldp: x, y, z by w),
 * cmask: which of its components the shader defined (D3D9 needs x, y and
 * for cube and volume z, plus w for the projective, bias and level forms;
 * a depth texture's compare reads z of a 2D lookup too).  lod: texldb's bias
 * or texldl's level (src0.w).  dx, dy: AUTO and BIAS: the derivative of c
 * across the quad (fine: the neighbour in the same row or column); GRAD:
 * texldd's src2 and src3.  Return 0 to stop the run (an error). */
enum { PSREF_LOD_AUTO, PSREF_LOD_BIAS, PSREF_LOD_LEVEL, PSREF_LOD_GRAD };
typedef struct {
    int stage, type, mode, vertex;
    float c[4]; unsigned cmask;
    float lod;
    float dx[4], dy[4];
} PsrefSample;
typedef int (*PsrefTex)(void *user, const PsrefSample *s, float out[4]);

/* Pixel shaders run on a quad: lane 0 is pixel (x, y), 1 (x+1, y), 2 (x, y+1),
 * 3 (x+1, y+1).  Inputs per lane: v# (ps_2_x: v0, v1 colours; ps_3_0: v0..v9
 * by their declarations), t# (ps_2_x), vPos and vFace.  Components outside a
 * declaration: ps_2_x colours take the (0, 0, 0, 1) default, everything else
 * is undefined (an error when read). */
typedef struct {
    float v[10][4];
    float t[8][4];
    float pos[2];
    float face;
} PsrefPsIn;
typedef struct {
    float oC[4][4]; unsigned oCmask[4];   /* oC0..oC3 and the components written */
    float depth; int depth_written;
    int killed;                           /* texkill */
} PsrefPsOut;
/* 1 = ran; 0 = error (err).  The result is the shader's; alpha test and the
 * fixed-function fog of ps_2_x are the pipeline's, after it. */
int psref_ps_run(const PsrefProg *p, const PsrefConsts *k, const PsrefPsIn in[4], PsrefTex tex, void *user,
                 PsrefPsOut out[4], char *err, size_t errcap);

/* Vertex shaders: v0..v15 by their declarations, o0..o11 out with the
 * components written in omask (texldl samples with vertex = 1). */
int psref_vs_run(const PsrefProg *p, const PsrefConsts *k, const float v[16][4], PsrefTex tex, void *user,
                 float o[12][4], unsigned omask[12], char *err, size_t errcap);
#endif
