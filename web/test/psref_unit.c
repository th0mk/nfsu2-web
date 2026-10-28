/* Unit tests of the D3D9 reference for ps_2_x, ps_3_0 and vs_3_0
 * (web/tools/psref.c, docs/web/psref.md), one group per instruction class:
 * arithmetic and modifiers, dynamic flow control, loops with aL, predicates,
 * dsx/dsy, texld/texldp/texldb/texldl/texldd, vFace and vPos, each model's
 * register limits and features, undefined reads, texkill and outputs.  Each
 * program is assembled here token by token; the expected values are worked
 * out by hand from the D3D9 instruction pages.
 *
 *   psref_unit                 the unit tests
 *   psref_unit --emit <dir>    the unit tests, and every distinct program they load
 *                              written to <dir> (the translator's corpus)
 *   psref_unit --fx <fxo>...   every ps_2_x, ps_3_0 and vs_3_0 blob of these
 *                              compiled effects, loaded and run (web/test/psref.test.mjs
 *                              gives it Most Wanted's 31) */
#include "psref.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D3DSIO opcodes and register types, written out again here (not shared with psref.c) */
enum { NOP = 0, MOV = 1, ADD = 2, SUB = 3, MAD = 4, MUL = 5, RCP = 6, RSQ = 7, DP3 = 8, DP4 = 9, MIN = 10, MAX = 11,
       SLT = 12, SGE = 13, EXP = 14, LOG = 15, LIT = 16, DST = 17, LRP = 18, FRC = 19, M4x4 = 20, M3x2 = 24,
       CALL = 25, CALLNZ = 26, LOOP = 27, RET = 28, ENDLOOP = 29, LABEL = 30, DCL = 31, POW = 32, CRS = 33,
       SGN = 34, ABS = 35, NRM = 36, SINCOS = 37, REP = 38, ENDREP = 39, IF = 40, IFC = 41, ELSE = 42, ENDIF = 43,
       BREAK = 44, BREAKC = 45, MOVA = 46, DEFB = 47, DEFI = 48, TEXKILL = 65, TEX = 66, EXPP = 78, LOGP = 79,
       DEF = 81, CMP = 88, DP2ADD = 90, DSX = 91, DSY = 92, TEXLDD = 93, SETP = 94, TEXLDL = 95, BREAKP = 96 };
enum { R = 0, V = 1, C = 2, A = 3, O = 6, I = 7, OC = 8, OD = 9, SMP = 10, B = 14, AL = 15, MISC = 17, LBL = 18, P = 19 };
enum { GT = 1, EQ = 2, GE = 3, LT = 4, NE = 5, LE = 6 };
enum { U_POSITION = 0, U_NORMAL = 3, U_TEXCOORD = 5, U_COLOR = 10 };

static int checks, failed;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                              printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a token assembler ---- */
static uint32_t T[8192];
static int nT;
static void begin(uint32_t ver){ nT = 0; T[nT++] = ver; }
static void end(void){ T[nT++] = 0x0000FFFFu; }
static uint32_t rtb(int t){ return ((uint32_t)(t & 7) << 28) | ((uint32_t)((t >> 3) & 3) << 11); }
static int ci(char ch){ return ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : 3; }
static uint32_t S(int t, int n, const char *sw){
    uint32_t r = 0; size_t k = strlen(sw);
    for (int c = 0; c < 4; c++) r |= (uint32_t)ci(sw[(size_t)c < k ? (size_t)c : k - 1]) << (2 * c);
    return 0x80000000u | rtb(t) | (uint32_t)n | (r << 16);
}
static uint32_t D(int t, int n, const char *mask){
    uint32_t m = 0;
    for (const char *c = mask; *c; c++) m |= 1u << ci(*c);
    return 0x80000000u | rtb(t) | (uint32_t)n | (m << 16);
}
static uint32_t MODS(uint32_t s, int mod){ return s | ((uint32_t)mod << 24); }
#define NEG(s) MODS(s, 1)
#define ABSM(s) MODS(s, 11)
#define NOT(s) MODS(s, 13)
static uint32_t SAT(uint32_t d){ return d | (1u << 20); }
static uint32_t REL(uint32_t s){ return s | 0x2000u; }
static void ins(int op, int ctrl, int pred, int n, const uint32_t *a){
    T[nT++] = (uint32_t)op | ((uint32_t)ctrl << 16) | ((uint32_t)n << 24) | ((uint32_t)pred << 28);
    for (int i = 0; i < n; i++) T[nT++] = a[i];
}
#define IN(op, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; ins(op, 0, 0, (int)(sizeof a_ / 4), a_); } while (0)
#define INC(op, ctrl, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; ins(op, ctrl, 0, (int)(sizeof a_ / 4), a_); } while (0)
#define INP(op, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; ins(op, 0, 1, (int)(sizeof a_ / 4), a_); } while (0)
#define IN0(op) ins(op, 0, 0, 0, NULL)
static uint32_t fu(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static void def(int n, float a, float b, float c, float d){ IN(DEF, D(C, n, "xyzw"), fu(a), fu(b), fu(c), fu(d)); }
static void defi(int n, int a, int b, int c, int d){ IN(DEFI, D(I, n, "xyzw"), (uint32_t)a, (uint32_t)b, (uint32_t)c, (uint32_t)d); }
static void defb(int n, int v){ IN(DEFB, D(B, n, "xyzw"), (uint32_t)v); }
static void dcl(int usage, int index, uint32_t reg){ IN(DCL, 0x80000000u | (uint32_t)usage | ((uint32_t)index << 16), reg); }
static void dcls(int type, int n){ IN(DCL, 0x80000000u | ((uint32_t)type << 27), D(SMP, n, "xyzw")); }
static void dclt(int n, const char *m){ dcl(0, 0, D(A, n, m)); }                /* ps_2_x dcl t# */

/* ---- loading and running ---- */
static PsrefCaps CAPS;
static char err[1024];
/* --emit <dir>: every distinct program the tests load, as <dir>/u<line>_<n>.bin, a corpus for the WGSL
 * translator's differential test (web/tools/wgsl-xl.c, shadercheck.ts) */
static const char *g_emit;
static void emit(int line){
    static uint32_t seen[1024]; static int nseen;
    uint32_t h = 2166136261u;
    for (int i = 0; i < nT; i++){ h ^= T[i]; h *= 16777619u; }
    for (int i = 0; i < nseen; i++) if (seen[i] == h) return;
    if (nseen < 1024) seen[nseen++] = h;
    char path[1024]; snprintf(path, sizeof path, "%s/u%04d_%d.bin", g_emit, line, nseen);
    FILE *f = fopen(path, "wb");
    if (f){ fwrite(T, 4, (size_t)nT, f); fclose(f); }
}
static PsrefProg *ld(int line){
    PsrefProg *p = psref_load(T, (size_t)nT, &CAPS, err, sizeof err);
    checks++;
    if (!p){ failed++; printf("FAIL %s:%d: refused: %s\n", __FILE__, line, err); }
    else if (g_emit) emit(line);
    return p;
}
#define LD() ld(__LINE__)
static void refused(int line, const char *sub){
    PsrefProg *p = psref_load(T, (size_t)nT, &CAPS, err, sizeof err);
    checks++;
    if (p || !strstr(err, sub)){
        failed++;
        printf("FAIL %s:%d: %s (wanted a refusal with \"%s\")\n", __FILE__, line, p ? "loaded" : err, sub);
    }
    psref_free(p);
}
#define REFUSED(sub) refused(__LINE__, sub)

static PsrefPsIn Q[4];
static PsrefPsOut OUT[4];
static float K[1024][4];
static PsrefConsts KC;
static PsrefSample SMPL[32];
static int nsmpl;
static void reset(void){
    memset(Q, 0, sizeof Q);
    for (int l = 0; l < 4; l++){ Q[l].pos[0] = 10.f + (float)(l & 1); Q[l].pos[1] = 20.f + (float)(l >> 1); Q[l].face = 1.f; }
    memset(K, 0, sizeof K);
    memset(&KC, 0, sizeof KC);
    KC.c = (const float (*)[4])K; KC.nc = 1024;
    psref_caps_sm3(&CAPS);
}
/* the test texture: (c.x + 10 * stage, c.y, c.z, mode * 100 + lod), every lookup recorded */
static int tex(void *u, const PsrefSample *s, float o[4]){
    (void)u;
    if (nsmpl < 32) SMPL[nsmpl] = *s;
    nsmpl++;
    o[0] = s->c[0] + 10.f * (float)s->stage; o[1] = s->c[1]; o[2] = s->c[2]; o[3] = (float)s->mode * 100.f + s->lod;
    return 1;
}
static int psrun(int line, PsrefProg *p){
    nsmpl = 0;
    int ok = p && psref_ps_run(p, &KC, Q, tex, NULL, OUT, err, sizeof err);
    checks++;
    if (!ok){ failed++; printf("FAIL %s:%d: run: %s\n", __FILE__, line, p ? err : "no program"); }
    return ok;
}
#define PSRUN(p) psrun(__LINE__, p)
static void psbad(int line, PsrefProg *p, const char *sub){
    nsmpl = 0;
    int ok = psref_ps_run(p, &KC, Q, tex, NULL, OUT, err, sizeof err);
    checks++;
    if (ok || !strstr(err, sub)){ failed++; printf("FAIL %s:%d: %s (wanted a run error with \"%s\")\n", __FILE__, line, ok ? "ran" : err, sub); }
}
#define PSBAD(p, sub) psbad(__LINE__, p, sub)
static float VIN[16][4], VO[12][4];
static unsigned VOM[12];
static int vsrun(int line, PsrefProg *p){
    nsmpl = 0;
    int ok = p && psref_vs_run(p, &KC, (const float (*)[4])VIN, tex, NULL, VO, VOM, err, sizeof err);
    checks++;
    if (!ok){ failed++; printf("FAIL %s:%d: run: %s\n", __FILE__, line, p ? err : "no program"); }
    return ok;
}
#define VSRUN(p) vsrun(__LINE__, p)
static void vsbad(int line, PsrefProg *p, const char *sub){
    nsmpl = 0;
    int ok = psref_vs_run(p, &KC, (const float (*)[4])VIN, tex, NULL, VO, VOM, err, sizeof err);
    checks++;
    if (ok || !strstr(err, sub)){ failed++; printf("FAIL %s:%d: %s (wanted a run error with \"%s\")\n", __FILE__, line, ok ? "ran" : err, sub); }
}
#define VSBAD(p, sub) vsbad(__LINE__, p, sub)
static int same(float a, float b){ return a == b || (isnan(a) && isnan(b)); }
#define EQ4(v, a, b, c, d) CHECK(same((v)[0], a) && same((v)[1], b) && same((v)[2], c) && same((v)[3], d), \
    "%s = (%g, %g, %g, %g), want (%g, %g, %g, %g)", #v, (double)(v)[0], (double)(v)[1], (double)(v)[2], (double)(v)[3], \
    (double)(a), (double)(b), (double)(c), (double)(d))
#define PS3 PSREF_PS_3_0
#define PS2X PSREF_PS_2_X
#define VS3 PSREF_VS_3_0

/* ======================================================================== */
static void t_arith(void){
    /* modifiers, swizzles, _sat, and the special values the instruction pages give */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw"));
    def(0, 1.f, -2.f, 0.5f, 4.f);
    IN(ADD, D(R, 0, "xyzw"), S(V, 0, "xyzw"), NEG(S(C, 0, "wzyx")));           /* v0 - c0.wzyx */
    IN(MUL, SAT(D(R, 1, "xyzw")), ABSM(S(R, 0, "xyzw")), S(C, 0, "x"));         /* sat(|r0|) */
    IN(RCP, D(R, 2, "x"), S(C, 1, "x"));                                        /* rcp 0 = FLT_MAX */
    IN(RCP, D(R, 2, "y"), S(C, 0, "x"));                                        /* rcp 1 = 1 */
    IN(RSQ, D(R, 2, "z"), NEG(S(C, 0, "w")));                                   /* rsq |-4| = 0.5 */
    IN(LOG, D(R, 2, "w"), S(C, 1, "x"));                                        /* log 0 = -FLT_MAX */
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    IN(MOV, D(OC, 1, "xyzw"), S(R, 1, "xyzw"));
    IN(MOV, D(OC, 2, "xyzw"), S(R, 2, "xyzw"));
    end();
    PsrefProg *p = LD();
    for (int l = 0; l < 4; l++){ Q[l].v[0][0] = 0.25f + (float)l; Q[l].v[0][1] = 3.f; Q[l].v[0][2] = -1.f; Q[l].v[0][3] = 2.f; }
    if (PSRUN(p)){
        EQ4(OUT[2].oC[0], 0.25f + 2.f - 4.f, 2.5f, 1.f, 1.f);
        EQ4(OUT[2].oC[1], 1.f, 1.f, 1.f, 1.f);
        EQ4(OUT[0].oC[1], 1.f, 1.f, 1.f, 1.f);
        EQ4(OUT[3].oC[2], FLT_MAX, 1.f, 0.5f, -FLT_MAX);
        CHECK(OUT[1].oCmask[0] == 0xF && OUT[1].oCmask[3] == 0, "oC masks %x %x", OUT[1].oCmask[0], OUT[1].oCmask[3]);
    }
    psref_free(p);

    /* nrm, crs, dp2add, cmp, lrp, frc, pow, exp, dp3, dp4, sincos, m4x4: one program, each register in turn to oC0
     * (c0 = (3, 0, 4, 10), c1 = (1, 2, 3, -1), c2 = (0, 1, 0, 0), c3 = (0.5, -0.5, 2, 8)) */
    struct { const char *what; float want[4]; } want[] = {
        { "nrm", { 3.f / 5.f, 0.f, 4.f / 5.f, 10.f / 5.f } },
        { "crs", { -3.f, 0.f, 1.f, 8.f } },
        { "dp2add/cmp", { 7.5f, -0.5f, 0.5f, 0.5f } },
        { "lrp", { 2.f, 1.f, 3.5f, 4.5f } },
        { "frc", { 0.5f, 0.f, 0.f, 0.5f } },
        { "pow/exp/dp3/dp4", { 8.f, 8.f, 14.f, -2.5f } },
        { "sincos", { 1.f, 0.f, 2.f, 8.f } },
        { "m4x4", { 1.f * 3.f + 3.f * 4.f - 10.f, 1.f * 1.f + 2.f * 2.f + 3.f * 3.f - 1.f * -1.f,
                    0.f * 1.f + 1.f * 2.f + 0.f + 0.f, 0.5f - 1.f + 6.f - 8.f } },
    };
    for (int r = 0; r < 8; r++){
        reset();
        begin(PS3);
        def(0, 3.f, 0.f, 4.f, 10.f); def(1, 1.f, 2.f, 3.f, -1.f); def(2, 0.f, 1.f, 0.f, 0.f); def(3, 0.5f, -0.5f, 2.f, 8.f);
        IN(NRM, D(R, 0, "xyzw"), S(C, 0, "xyzw"));
        IN(CRS, D(R, 1, "xyz"), S(C, 1, "xyzw"), S(C, 2, "xyzw"));
        IN(MOV, D(R, 1, "w"), S(C, 3, "w"));
        IN(DP2ADD, D(R, 2, "x"), S(C, 1, "xyzw"), S(C, 3, "xyzw"), S(C, 3, "w"));
        IN(CMP, D(R, 2, "yzw"), S(C, 1, "wwxx"), S(C, 3, "xxxx"), S(C, 3, "yyyy"));
        IN(LRP, D(R, 3, "xyzw"), S(C, 3, "x"), S(C, 1, "xyzw"), S(C, 0, "xyzw"));
        IN(FRC, D(R, 4, "xyzw"), S(C, 3, "yzwx"));
        IN(POW, D(R, 5, "x"), NEG(S(C, 3, "z")), S(C, 0, "x"));
        IN(EXP, D(R, 5, "y"), S(C, 1, "z"));
        IN(DP3, D(R, 5, "z"), S(C, 1, "xyzw"), S(C, 1, "xyzw"));
        IN(DP4, D(R, 5, "w"), S(C, 1, "xyzw"), S(C, 3, "xyzw"));
        IN(SINCOS, D(R, 6, "xy"), S(C, 2, "x"));
        IN(MOV, D(R, 6, "zw"), S(C, 3, "xyzw"));
        IN(M4x4, D(R, 7, "xyzw"), S(C, 1, "xyzw"), S(C, 0, "xyzw"));
        IN(MOV, D(OC, 0, "xyzw"), S(R, r, "xyzw"));
        end();
        p = LD();
        if (p && PSRUN(p))
            CHECK(same(OUT[1].oC[0][0], want[r].want[0]) && same(OUT[1].oC[0][1], want[r].want[1]) &&
                  same(OUT[1].oC[0][2], want[r].want[2]) && same(OUT[1].oC[0][3], want[r].want[3]),
                  "%s: (%g, %g, %g, %g), want (%g, %g, %g, %g)", want[r].what, (double)OUT[1].oC[0][0], (double)OUT[1].oC[0][1],
                  (double)OUT[1].oC[0][2], (double)OUT[1].oC[0][3], (double)want[r].want[0], (double)want[r].want[1],
                  (double)want[r].want[2], (double)want[r].want[3]);
        psref_free(p);
    }

    /* min and max as the pages write them: min = s0 < s1 ? s0 : s1, max = s0 >= s1 ? s0 : s1 (a NaN takes s1) */
    reset();
    begin(PS3);
    IN(MIN, D(R, 0, "xy"), S(C, 0, "xy"), S(C, 1, "xy"));
    IN(MAX, D(R, 0, "zw"), S(C, 0, "xyxy"), S(C, 1, "xyxy"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    p = LD();
    K[0][0] = NAN; K[0][1] = 2.f; K[1][0] = 1.f; K[1][1] = NAN;
    if (PSRUN(p)) EQ4(OUT[0].oC[0], 1.f, NAN, 1.f, NAN);
    psref_free(p);
    /* nrm of a zero vector: FLT_MAX times each component (nrm page), w included */
    reset();
    begin(PS3);
    IN(NRM, D(R, 0, "xyzw"), S(C, 0, "xyzw"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    p = LD();
    K[0][3] = 0.5f;
    if (PSRUN(p)) EQ4(OUT[0].oC[0], 0.f, 0.f, 0.f, 0.5f * FLT_MAX);
    psref_free(p);
    /* cmp takes src1 at exactly 0 (and -0); log takes |x|; the six comparisons of setp at equal and unequal values */
    reset();
    begin(PS3);
    def(1, 1.f, 2.f, 3.f, 4.f);
    def(2, 5.f, 6.f, 7.f, 8.f);
    IN(CMP, D(R, 0, "xyzw"), S(C, 0, "xyzw"), S(C, 1, "xyzw"), S(C, 2, "xyzw"));
    IN(LOG, D(R, 1, "x"), S(C, 3, "x"));
    for (int k = 0; k < 6; k++){
        INC(SETP, GT + k, D(P, 0, "xyzw"), S(C, 4, "xyzw"), S(C, 5, "xyzw"));
        IN(MOV, D(R, 2 + k, "xyzw"), S(C, 1, "x"));
        INP(MOV, D(R, 2 + k, "xyzw"), S(P, 0, "xyzw"), S(C, 1, "y"));
    }
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    IN(MOV, D(OC, 1, "xyzw"), S(R, 1, "x"));
    end();
    p = LD();
    K[0][0] = 0.f; K[0][1] = -0.f; K[0][2] = -1e-30f; K[0][3] = NAN;
    K[3][0] = -8.f;
    K[4][0] = 1.f; K[4][1] = 2.f; K[4][2] = 3.f; K[4][3] = NAN;
    K[5][0] = 1.f; K[5][1] = 1.f; K[5][2] = 4.f; K[5][3] = 0.f;
    if (PSRUN(p)){
        EQ4(OUT[0].oC[0], 1.f, 2.f, 7.f, 8.f);
        EQ4(OUT[0].oC[1], 3.f, 3.f, 3.f, 3.f);
    }
    psref_free(p);
    /* the six comparisons, one register each: (1 vs 1, 2 vs 1, 3 vs 4, NaN vs 0) -> 2 where true, else 1 */
    static const float cmpwant[6][4] = {
        { 1, 2, 1, 1 },     /* gt */
        { 2, 1, 1, 1 },     /* eq */
        { 2, 2, 1, 1 },     /* ge */
        { 1, 1, 2, 1 },     /* lt */
        { 1, 2, 2, 2 },     /* ne: NaN != 0 */
        { 2, 1, 2, 1 },     /* le */
    };
    for (int k = 0; k < 6; k++){
        reset();
        begin(PS3);
        def(1, 1.f, 2.f, 3.f, 4.f);
        INC(SETP, GT + k, D(P, 0, "xyzw"), S(C, 4, "xyzw"), S(C, 5, "xyzw"));
        IN(MOV, D(R, 0, "xyzw"), S(C, 1, "x"));
        INP(MOV, D(R, 0, "xyzw"), S(P, 0, "xyzw"), S(C, 1, "y"));
        INC(IFC, GT + k, S(C, 4, "x"), S(C, 5, "x"));
          IN(MOV, D(R, 1, "xyzw"), S(C, 1, "y"));
        IN0(ELSE);
          IN(MOV, D(R, 1, "xyzw"), S(C, 1, "x"));
        IN0(ENDIF);
        IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
        IN(MOV, D(OC, 1, "xyzw"), S(R, 1, "xyzw"));
        end();
        p = LD();
        K[4][0] = 1.f; K[4][1] = 2.f; K[4][2] = 3.f; K[4][3] = NAN;
        K[5][0] = 1.f; K[5][1] = 1.f; K[5][2] = 4.f; K[5][3] = 0.f;
        if (PSRUN(p)){
            EQ4(OUT[0].oC[0], cmpwant[k][0], cmpwant[k][1], cmpwant[k][2], cmpwant[k][3]);
            CHECK(OUT[0].oC[1][0] == cmpwant[k][0], "if_comp %d: %g", k + 1, (double)OUT[0].oC[1][0]);
        }
        psref_free(p);
    }
    /* def wins over the application's value of the same constant; a vs_3_0 relative read lands on def'd ones too */
    reset();
    begin(PS3);
    def(0, 1.f, 2.f, 3.f, 4.f);
    IN(MOV, D(OC, 0, "xyzw"), S(C, 0, "xyzw"));
    end();
    p = LD();
    K[0][0] = 9.f;
    if (PSRUN(p)) EQ4(OUT[0].oC[0], 1.f, 2.f, 3.f, 4.f);
    psref_free(p);
    reset();
    begin(VS3);
    dcl(U_POSITION, 0, D(V, 0, "xyzw"));
    dcl(U_POSITION, 0, D(O, 0, "xyzw"));
    def(8, 5.f, 6.f, 7.f, 8.f);
    IN(MOVA, D(A, 0, "x"), S(V, 0, "xyzw"));
    IN(MOV, D(O, 0, "xyzw"), REL(S(C, 5, "xyzw")), S(A, 0, "xxxx"));
    end();
    p = LD();
    for (int k = 0; k < 16; k++) K[k][0] = (float)k;
    VIN[0][0] = 3.f;
    if (VSRUN(p)) EQ4(VO[0], 5.f, 6.f, 7.f, 8.f);
    VIN[0][0] = 2.f;
    if (VSRUN(p)) EQ4(VO[0], 7.f, 0.f, 0.f, 0.f);
    psref_free(p);

    /* ps_2_x sincos: three operands, .z undefined afterwards (reading it is an error) */
    reset();
    begin(PS2X);
    def(0, 0.25f, 0.f, 0.f, 0.f);
    def(1, -1.5500992e-006f, -2.1701389e-005f, 0.0026041667f, 0.00026041668f);
    def(2, -0.020833334f, -0.12500000f, 1.0f, 0.50000000f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "yyyy"));
    IN(SINCOS, D(R, 0, "x"), S(C, 0, "x"), S(C, 1, "xyzw"), S(C, 2, "xyzw"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xxxw"));
    end();
    p = LD();
    if (PSRUN(p)) EQ4(OUT[0].oC[0], (float)cos(0.25), (float)cos(0.25), (float)cos(0.25), 0.f);
    psref_free(p);
    nT -= 3 + 1;                                 /* drop "mov oC0, r0.xxxw" and the end token */
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    p = LD();
    PSBAD(p, "reads r0.yz before it is written");
    psref_free(p);
    /* sincos outside [-pi, pi] is undefined */
    reset();
    begin(PS3);
    def(0, 4.f, 0.f, 0.f, 0.f);
    IN(SINCOS, D(R, 0, "xy"), S(C, 0, "x"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyxy"));
    end();
    p = LD();
    PSBAD(p, "outside [-pi, pi]");
    psref_free(p);
}

/* ======================================================================== */
static void t_flow(void){
    /* if_comp/else/endif nested, lanes of the quad taking different ways */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xy"));
    def(0, 0.5f, 1.f, 2.f, 3.f);
    INC(IFC, GT, S(V, 0, "x"), S(C, 0, "x"));
      IN(MOV, D(R, 0, "xyzw"), S(C, 0, "y"));
    IN0(ELSE);
      INC(IFC, LT, S(V, 0, "y"), S(C, 0, "x"));
        IN(MOV, D(R, 0, "xyzw"), S(C, 0, "z"));
      IN0(ELSE);
        IN(MOV, D(R, 0, "xyzw"), S(C, 0, "w"));
      IN0(ENDIF);
    IN0(ENDIF);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    PsrefProg *p = LD();
    float vin[4][2] = { { 0.9f, 0.f }, { 0.1f, 0.2f }, { 0.2f, 0.9f }, { 0.7f, 0.9f } };
    for (int l = 0; l < 4; l++){ Q[l].v[0][0] = vin[l][0]; Q[l].v[0][1] = vin[l][1]; }
    if (PSRUN(p)){
        CHECK(OUT[0].oC[0][0] == 1.f && OUT[1].oC[0][0] == 2.f && OUT[2].oC[0][0] == 3.f && OUT[3].oC[0][0] == 1.f,
              "if/else lanes: %g %g %g %g", (double)OUT[0].oC[0][0], (double)OUT[1].oC[0][0], (double)OUT[2].oC[0][0], (double)OUT[3].oC[0][0]);
    }
    psref_free(p);

    /* break_comp in rep: each lane leaves after its own number of rounds */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "x"));
    defi(0, 10, 0, 0, 0);
    def(0, 0.f, 1.f, 0.f, 0.f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "x"));
    IN(REP, S(I, 0, "xyzw"));
      INC(BREAKC, GE, S(R, 0, "x"), S(V, 0, "x"));
      IN(ADD, D(R, 0, "x"), S(R, 0, "x"), S(C, 0, "y"));
    IN0(ENDREP);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "x"));
    end();
    p = LD();
    float rounds[4] = { 1.f, 3.f, 7.f, 20.f };
    for (int l = 0; l < 4; l++) Q[l].v[0][0] = rounds[l];
    if (PSRUN(p))
        CHECK(OUT[0].oC[0][0] == 1.f && OUT[1].oC[0][0] == 3.f && OUT[2].oC[0][0] == 7.f && OUT[3].oC[0][0] == 10.f,
              "break_comp rounds: %g %g %g %g", (double)OUT[0].oC[0][0], (double)OUT[1].oC[0][0], (double)OUT[2].oC[0][0], (double)OUT[3].oC[0][0]);
    psref_free(p);
    /* break inside a dynamic if, and the loop's lanes rejoining after it */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "x"));
    defi(0, 5, 0, 0, 0);
    def(0, 0.f, 1.f, 100.f, 0.f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "x"));
    IN(REP, S(I, 0, "xyzw"));
      INC(IFC, GE, S(R, 0, "x"), S(V, 0, "x"));
        IN0(BREAK);
      IN0(ENDIF);
      IN(ADD, D(R, 0, "x"), S(R, 0, "x"), S(C, 0, "y"));
    IN0(ENDREP);
    IN(ADD, D(R, 0, "y"), S(R, 0, "x"), S(C, 0, "z"));                         /* every lane again */
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyyy"));
    end();
    p = LD();
    float rr[4] = { 0.f, 2.f, 4.f, 9.f };
    for (int l = 0; l < 4; l++) Q[l].v[0][0] = rr[l];
    if (PSRUN(p))
        CHECK(OUT[0].oC[0][1] == 100.f && OUT[1].oC[0][1] == 102.f && OUT[2].oC[0][1] == 104.f && OUT[3].oC[0][1] == 105.f,
              "break in if: %g %g %g %g", (double)OUT[0].oC[0][1], (double)OUT[1].oC[0][1], (double)OUT[2].oC[0][1], (double)OUT[3].oC[0][1]);
    psref_free(p);

    /* call, callnz with a predicate, ret; if b# with defb and with the application's b# */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "x"));
    def(0, 1.f, 2.f, 0.f, 4.f);
    defb(1, 1);
    INC(SETP, GT, D(P, 0, "x"), S(V, 0, "x"), S(C, 0, "z"));
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "z"));
    IN(CALLNZ, S(LBL, 1, "xyzw"), S(P, 0, "x"));
    IN(CALL, S(LBL, 2, "xyzw"));
    IN(IF, S(B, 0, "xyzw"));
      IN(ADD, D(R, 0, "z"), S(R, 0, "z"), S(C, 0, "x"));
    IN0(ENDIF);
    IN(IF, NOT(S(B, 1, "xyzw")));
      IN(ADD, D(R, 0, "z"), S(R, 0, "z"), S(C, 0, "w"));
    IN0(ELSE);
      IN(ADD, D(R, 0, "w"), S(R, 0, "w"), S(C, 0, "w"));
    IN0(ENDIF);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    IN0(RET);
    IN(LABEL, S(LBL, 1, "xyzw"));
      IN(ADD, D(R, 0, "x"), S(R, 0, "x"), S(C, 0, "x"));
    IN0(RET);
    IN(LABEL, S(LBL, 2, "xyzw"));
      IN(ADD, D(R, 0, "y"), S(R, 0, "y"), S(C, 0, "y"));
    IN0(RET);
    end();
    p = LD();
    float sg[4] = { 1.f, -1.f, 2.f, -2.f };
    for (int l = 0; l < 4; l++) Q[l].v[0][0] = sg[l];
    KC.b[0] = 1;
    if (PSRUN(p)){
        EQ4(OUT[0].oC[0], 1.f, 2.f, 1.f, 4.f);
        EQ4(OUT[1].oC[0], 0.f, 2.f, 1.f, 4.f);
    }
    KC.b[0] = 0;
    if (PSRUN(p)) EQ4(OUT[2].oC[0], 1.f, 2.f, 0.f, 4.f);
    psref_free(p);

    /* structure: an else without if, a block left open, a break outside a loop, a missing label, recursion */
    reset(); begin(PS3); IN0(ELSE); end(); REFUSED("else without its if");
    reset(); begin(PS3); def(0, 0, 0, 0, 0); INC(IFC, GT, S(C, 0, "x"), S(C, 0, "y")); end(); REFUSED("block not closed");
    reset(); begin(PS3); IN0(BREAK); end(); REFUSED("break outside a loop");
    reset(); begin(PS3); IN(CALL, S(LBL, 3, "xyzw")); end(); REFUSED("l3 is not defined");
    reset(); begin(PS3); IN(CALL, S(LBL, 0, "xyzw")); IN0(RET); IN(LABEL, S(LBL, 0, "xyzw")); IN(CALL, S(LBL, 0, "xyzw")); IN0(RET); end();
    REFUSED("recursive call");
    /* dynamic flow control in ps_2_x needs DynamicFlowControlDepth, static flow needs StaticFlowControlDepth */
    reset(); begin(PS2X); def(0, 0, 0, 0, 0); INC(IFC, GT, S(C, 0, "x"), S(C, 0, "y")); IN0(ENDIF); IN(MOV, D(OC, 0, "xyzw"), S(C, 0, "xyzw")); end();
    PsrefProg *q = LD(); psref_free(q);
    CAPS.ps20_dynamic_depth = 0; REFUSED("needs DynamicFlowControlDepth");
    reset(); begin(PS2X); defi(0, 2, 0, 0, 0); IN(REP, S(I, 0, "xyzw")); IN0(ENDREP); end();
    q = LD(); psref_free(q);
    CAPS.ps20_static_depth = 0; REFUSED("needs StaticFlowControlDepth");
}

/* ======================================================================== */
static void t_loops(void){
    /* loop aL, i#: count, start and step; v[aL + n] in ps_3_0 */
    reset();
    begin(PS3);
    for (int r = 0; r < 9; r++) dcl(U_TEXCOORD, r, D(V, r, "xyzw"));
    defi(0, 3, 0, 3, 0);
    def(0, 0.f, 0.f, 0.f, 0.f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "xyzw"));
    IN(LOOP, S(AL, 0, "xyzw"), S(I, 0, "xyzw"));
      IN(ADD, D(R, 0, "xyzw"), S(R, 0, "xyzw"), REL(S(V, 1, "xyzw")), S(AL, 0, "xxxx"));
    IN0(ENDLOOP);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    PsrefProg *p = LD();
    for (int l = 0; l < 4; l++) for (int r = 0; r < 9; r++) for (int c = 0; c < 4; c++) Q[l].v[r][c] = (float)(r * 10 + c + l * 1000);
    if (PSRUN(p)) EQ4(OUT[1].oC[0], 120.f + 3000.f, 123.f + 3000.f, 126.f + 3000.f, 129.f + 3000.f);   /* v1 + v4 + v7 */
    psref_free(p);

    /* nested loops in vs_3_0: aL is the inner loop's, then the outer one's again; c[aL + n] */
    reset();
    begin(VS3);
    dcl(U_POSITION, 0, D(O, 0, "xyzw"));
    defi(0, 2, 0, 1, 0);
    defi(1, 2, 10, 10, 0);
    def(0, 0.f, 0.f, 0.f, 0.f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "xyzw"));
    IN(LOOP, S(AL, 0, "xyzw"), S(I, 0, "xyzw"));
      IN(LOOP, S(AL, 0, "xyzw"), S(I, 1, "xyzw"));
        IN(ADD, D(R, 0, "x"), S(R, 0, "x"), REL(S(C, 0, "xxxx")), S(AL, 0, "xxxx"));
      IN0(ENDLOOP);
      IN(ADD, D(R, 0, "y"), S(R, 0, "y"), REL(S(C, 1, "xxxx")), S(AL, 0, "xxxx"));
    IN0(ENDLOOP);
    IN(MOV, D(O, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    p = LD();
    for (int k = 0; k < 64; k++) K[k][0] = (float)k;
    if (VSRUN(p)) EQ4(VO[0], 60.f, 3.f, 0.f, 0.f);                /* 2 x (c10 + c20); c1 + c2 */
    psref_free(p);

    /* o[aL] written in a loop; a0 from mova (to nearest, halves away from zero); c[a0.x + n] out of range reads 0 */
    reset();
    begin(VS3);
    dcl(U_POSITION, 0, D(V, 0, "xyzw"));
    dcl(U_POSITION, 0, D(O, 0, "xyzw"));
    for (int r = 1; r <= 4; r++) dcl(U_TEXCOORD, r - 1, D(O, r, "xyzw"));
    dcl(U_TEXCOORD, 4, D(O, 5, "xyzw"));
    defi(0, 4, 1, 1, 0);
    IN(LOOP, S(AL, 0, "xyzw"), S(I, 0, "xyzw"));
      IN(MOV, REL(D(O, 0, "xyzw")), S(AL, 0, "xxxx"), REL(S(C, 0, "xyzw")), S(AL, 0, "xxxx"));
    IN0(ENDLOOP);
    IN(MOVA, D(A, 0, "xy"), S(V, 0, "xyzw"));
    IN(MOV, D(O, 0, "xy"), REL(S(C, 5, "xyzw")), S(A, 0, "xxxx"));
    IN(MOV, D(O, 0, "zw"), REL(S(C, 5, "xyzw")), S(A, 0, "yyyy"));
    IN(MOV, D(O, 5, "xyzw"), S(A, 0, "xyxy"));
    end();
    p = LD();
    for (int k = 0; k < 64; k++) for (int c = 0; c < 4; c++) K[k][c] = (float)(k * 4 + c);
    VIN[0][0] = 2.5f; VIN[0][1] = 300.f;
    if (VSRUN(p)){
        EQ4(VO[1], 4.f, 5.f, 6.f, 7.f);
        EQ4(VO[4], 16.f, 17.f, 18.f, 19.f);
        EQ4(VO[0], 32.f, 33.f, 0.f, 0.f);                          /* a0.x = 3: c8; a0.y = 300: c305 is out of range */
        EQ4(VO[5], 3.f, 300.f, 3.f, 300.f);
        CHECK(VOM[0] == 0xF && VOM[4] == 0xF && VOM[6] == 0, "o masks %x %x %x", VOM[0], VOM[4], VOM[6]);
    }
    VIN[0][0] = -2.5f;
    if (VSRUN(p)) CHECK(VO[5][0] == -3.f, "mova -2.5 = %g", (double)VO[5][0]);
    psref_free(p);

    /* rep with a count of 0 skips its body; a count outside [0, 255], a step outside [-128, 127] and i.w != 0 are errors */
    reset();
    begin(PS3);
    def(0, 1.f, 0.f, 0.f, 0.f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "xyzw"));
    IN(REP, S(I, 0, "xyzw"));
      IN(ADD, D(R, 0, "xyzw"), S(R, 0, "xyzw"), S(C, 0, "xyzw"));
    IN0(ENDREP);
    IN(LOOP, S(AL, 0, "xyzw"), S(I, 1, "xyzw"));
    IN0(ENDLOOP);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    p = LD();
    KC.i[1][0] = 1;
    if (PSRUN(p)) EQ4(OUT[0].oC[0], 1.f, 0.f, 0.f, 0.f);
    KC.i[0][0] = 256; PSBAD(p, "outside [0, 255]");
    KC.i[0][0] = 0; KC.i[1][2] = 128; PSBAD(p, "step outside [-128, 127]");
    KC.i[1][2] = 0; KC.i[1][3] = 1; PSBAD(p, "must be 0");
    psref_free(p);
    /* aL outside a loop block (rep has none); loop is not ps_2_x */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); defi(0, 1, 0, 0, 0);
    IN(REP, S(I, 0, "xyzw")); IN(MOV, D(OC, 0, "xyzw"), REL(S(V, 0, "xyzw")), S(AL, 0, "xxxx")); IN0(ENDREP); end();
    REFUSED("aL outside a loop block");
    reset(); begin(PS2X); defi(0, 1, 0, 0, 0); IN(LOOP, S(AL, 0, "xyzw"), S(I, 0, "xyzw")); IN0(ENDLOOP); end();
    REFUSED("loop is not in this model");
    /* relative c# in a pixel shader, relative v# by a0 in ps_3_0 */
    reset(); begin(PS3); defi(0, 1, 0, 0, 0); IN(LOOP, S(AL, 0, "xyzw"), S(I, 0, "xyzw"));
    IN(MOV, D(OC, 0, "xyzw"), REL(S(C, 0, "xyzw")), S(AL, 0, "xxxx")); IN0(ENDLOOP); end();
    REFUSED("relative addressing of c");
    /* v[aL] past v9 at run time */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); defi(0, 2, 9, 1, 0); IN(LOOP, S(AL, 0, "xyzw"), S(I, 0, "xyzw"));
    IN(MOV, D(OC, 0, "xyzw"), REL(S(V, 0, "xyzw")), S(AL, 0, "xxxx")); IN0(ENDLOOP); end();
    p = LD(); PSBAD(p, "v9 is not declared"); psref_free(p);

    /* the executed-instruction limit */
    reset();
    begin(PS3);
    defi(0, 255, 0, 0, 0);
    def(0, 1.f, 0.f, 0.f, 0.f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "yyyy"));
    IN(REP, S(I, 0, "xyzw"));
      IN(REP, S(I, 0, "xyzw"));
        IN(ADD, D(R, 0, "x"), S(R, 0, "x"), S(C, 0, "x"));
      IN0(ENDREP);
    IN0(ENDREP);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    end();
    p = LD();
    PSBAD(p, "more than 65535 instructions executed");
    psref_free(p);
    CAPS.max_executed = 1000000;
    p = LD();
    if (PSRUN(p)) CHECK(OUT[0].oC[0][0] == 65025.f, "255 x 255 = %g", (double)OUT[0].oC[0][0]);
    psref_free(p);
}

/* ======================================================================== */
static void t_pred(void){
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw"));
    def(0, 0.5f, 0.f, 9.f, 0.f);
    def(2, 1.f, 2.f, 3.f, 4.f);
    INC(SETP, GT, D(P, 0, "xyzw"), S(V, 0, "xyzw"), S(C, 0, "x"));
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "y"));
    IN(MOV, D(R, 1, "xyzw"), S(C, 0, "z"));
    INP(ADD, D(R, 0, "xyzw"), S(P, 0, "xyzw"), S(R, 0, "xyzw"), S(C, 2, "xyzw"));
    INP(MOV, D(R, 1, "xyzw"), NOT(S(P, 0, "yxwz")), S(C, 2, "xyzw"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    IN(MOV, D(OC, 1, "xyzw"), S(R, 1, "xyzw"));
    IN(IF, NOT(S(P, 0, "w")));
      IN(MOV, D(OC, 2, "xyzw"), S(C, 2, "xyzw"));
    IN0(ELSE);
      IN(MOV, D(OC, 2, "xyzw"), S(C, 0, "xyzw"));
    IN0(ENDIF);
    end();
    PsrefProg *p = LD();
    float pv[4][4] = { { 1, 0, 1, 0 }, { 0, 1, 0, 1 }, { 1, 1, 1, 1 }, { 0, 0, 0, 0 } };
    for (int l = 0; l < 4; l++) memcpy(Q[l].v[0], pv[l], 16);
    if (PSRUN(p)){
        EQ4(OUT[0].oC[0], 1.f, 0.f, 3.f, 0.f);
        EQ4(OUT[0].oC[1], 1.f, 9.f, 3.f, 9.f);
        EQ4(OUT[1].oC[1], 9.f, 2.f, 9.f, 4.f);
        EQ4(OUT[0].oC[2], 1.f, 2.f, 3.f, 4.f);                     /* p0.w false: !p0.w */
        EQ4(OUT[1].oC[2], 0.5f, 0.f, 9.f, 0.f);
    }
    psref_free(p);
    /* breakp per lane */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "x"));
    defi(0, 6, 0, 0, 0);
    def(0, 0.f, 1.f, 0.f, 0.f);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "x"));
    IN(REP, S(I, 0, "xyzw"));
      INC(SETP, GE, D(P, 0, "x"), S(R, 0, "x"), S(V, 0, "x"));
      IN(BREAKP, S(P, 0, "x"));
      IN(ADD, D(R, 0, "x"), S(R, 0, "x"), S(C, 0, "y"));
    IN0(ENDREP);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "x"));
    end();
    p = LD();
    float lim[4] = { 2.f, 0.f, 5.f, 9.f };
    for (int l = 0; l < 4; l++) Q[l].v[0][0] = lim[l];
    if (PSRUN(p)) CHECK(OUT[0].oC[0][0] == 2.f && OUT[1].oC[0][0] == 0.f && OUT[2].oC[0][0] == 5.f && OUT[3].oC[0][0] == 6.f,
                        "breakp: %g %g %g %g", (double)OUT[0].oC[0][0], (double)OUT[1].oC[0][0], (double)OUT[2].oC[0][0], (double)OUT[3].oC[0][0]);
    psref_free(p);
    /* p0 read before setp writes it */
    reset(); begin(PS3); def(0, 1, 1, 1, 1);
    INP(MOV, D(OC, 0, "xyzw"), S(P, 0, "xyzw"), S(C, 0, "xyzw")); end();
    p = LD(); PSBAD(p, "before setp writes it"); psref_free(p);
    /* ps_2_x: predication needs D3DPS20CAPS_PREDICATION; p0 is no value source; a predicated texkill */
    reset(); begin(PS2X); def(0, 1, 1, 1, 1);
    INC(SETP, GT, D(P, 0, "x"), S(C, 0, "x"), S(C, 0, "y"));
    INP(MOV, D(OC, 0, "xyzw"), S(P, 0, "xxxx"), S(C, 0, "xyzw")); end();
    p = LD(); psref_free(p);
    CAPS.ps20_caps &= ~PSREF_PS20_PREDICATION; REFUSED("PREDICATION");
    reset(); begin(PS3); IN(MOV, D(OC, 0, "xyzw"), S(P, 0, "xyzw")); end(); REFUSED("cannot be a value source");
    reset(); begin(PS3); def(0, 1, 1, 1, 1); INC(SETP, GT, D(P, 0, "x"), S(C, 0, "x"), S(C, 0, "y"));
    INP(TEXKILL, D(R, 0, "xyzw"), S(P, 0, "xxxx")); end(); REFUSED("predicated texkill");
    /* only setp writes p0 */
    reset(); begin(PS3); def(0, 1, 1, 1, 1); IN(MOV, D(P, 0, "x"), S(C, 0, "x")); end(); REFUSED("only setp writes p0");
}

/* ======================================================================== */
static void t_grad(void){
    /* dsx/dsy of an input: fine derivatives, each row and column of the quad on its own */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw"));
    IN(DSX, D(R, 0, "xyzw"), S(V, 0, "xyzw"));
    IN(DSY, D(R, 1, "xyzw"), S(V, 0, "xyzw"));
    IN(MUL, D(R, 2, "xyzw"), S(V, 0, "xyzw"), S(V, 0, "xyzw"));
    IN(DSX, D(R, 3, "xyzw"), S(R, 2, "xyzw"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    IN(MOV, D(OC, 1, "xyzw"), S(R, 1, "xyzw"));
    IN(MOV, D(OC, 2, "xyzw"), S(R, 3, "xyzw"));
    end();
    PsrefProg *p = LD();
    for (int l = 0; l < 4; l++){
        float x = Q[l].pos[0], y = Q[l].pos[1];
        Q[l].v[0][0] = 2.f * x + 3.f * y; Q[l].v[0][1] = x * x; Q[l].v[0][2] = x * y; Q[l].v[0][3] = 1.f;
    }
    if (PSRUN(p)){
        EQ4(OUT[0].oC[0], 2.f, 21.f, 20.f, 0.f);                   /* row y = 20 */
        EQ4(OUT[3].oC[0], 2.f, 21.f, 21.f, 0.f);                   /* row y = 21 */
        EQ4(OUT[1].oC[1], 3.f, 0.f, 11.f, 0.f);                    /* column x = 11 */
        float a = Q[1].v[0][0] * Q[1].v[0][0] - Q[0].v[0][0] * Q[0].v[0][0];
        CHECK(OUT[0].oC[2][0] == a && OUT[1].oC[2][0] == a, "dsx of a temporary: %g %g, want %g", (double)OUT[0].oC[2][0], (double)OUT[1].oC[2][0], (double)a);
    }
    psref_free(p);
    /* a gradient of a temporary under a dynamic if: refused at load (D3D9's validator does) */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw"));
    IN(MOV, D(R, 0, "xyzw"), S(V, 0, "xyzw"));
    INC(IFC, GT, S(V, 0, "x"), S(V, 0, "y")); IN(DSX, D(R, 1, "xyzw"), S(R, 0, "xyzw")); IN0(ENDIF);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    REFUSED("gradient of a temporary");
    /* of an input it is allowed anywhere */
    nT = 0; begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); IN(MOV, D(R, 1, "xyzw"), S(V, 0, "xyzw"));
    INC(IFC, GT, S(V, 0, "x"), S(V, 0, "y")); IN(DSX, D(R, 1, "xyzw"), S(V, 0, "xyzw")); IN0(ENDIF);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 1, "xyzw")); end();
    p = LD();
    for (int l = 0; l < 4; l++){ Q[l].v[0][0] = (float)(l & 1) * 5.f; Q[l].v[0][1] = 1.f; Q[l].v[0][2] = Q[l].v[0][3] = 0.f; }
    if (PSRUN(p)){ CHECK(OUT[1].oC[0][0] == 5.f && OUT[0].oC[0][0] == 0.f, "dsx in a branch: %g %g", (double)OUT[1].oC[0][0], (double)OUT[0].oC[0][0]); }
    psref_free(p);
    /* a loop that a per-pixel break may leave early */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); defi(0, 2, 0, 0, 0);
    IN(MOV, D(R, 0, "xyzw"), S(V, 0, "xyzw"));
    IN(REP, S(I, 0, "xyzw")); INC(BREAKC, GT, S(V, 0, "x"), S(V, 0, "y")); IN(DSY, D(R, 0, "xyzw"), S(R, 0, "xyzw")); IN0(ENDREP);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    REFUSED("gradient of a temporary");
    /* ps_2_x needs D3DPS20CAPS_GRADIENTINSTRUCTIONS; vs_3_0 has none */
    reset(); begin(PS2X); dclt(0, "xyzw"); IN(DSX, D(R, 0, "xyzw"), S(A, 0, "xyzw")); IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    p = LD(); psref_free(p);
    CAPS.ps20_caps &= ~PSREF_PS20_GRADIENTINSTRUCTIONS; REFUSED("GRADIENTINSTRUCTIONS");
    reset(); begin(VS3); dcl(U_POSITION, 0, D(V, 0, "xyzw")); IN(DSX, D(R, 0, "xyzw"), S(V, 0, "xyzw")); end();
    REFUSED("dsx is not in this model");
}

/* ======================================================================== */
static void t_tex(void){
    /* texld (implicit level of detail: the quad's derivatives), texldp (x, y, z by w), texldb, texldl, texldd */
    reset();
    begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw"));
    dcl(U_TEXCOORD, 1, D(V, 1, "xyzw"));
    dcls(2, 0); dcls(3, 1); dcls(2, 2);
    IN(TEX, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw"));
    INC(TEX, 1, D(R, 1, "xyzw"), S(V, 1, "xyzw"), S(SMP, 1, "xyzw"));
    INC(TEX, 2, D(R, 2, "xyzw"), S(V, 0, "xyzw"), S(SMP, 2, "wzyx"));
    IN(TEXLDL, D(R, 3, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw"));
    IN(TEXLDD, D(R, 4, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw"), S(V, 1, "xyzw"), NEG(S(V, 1, "wzyx")));
    for (int r = 0; r < 4; r++) IN(MOV, D(OC, r, "xyzw"), S(R, r, "xyzw"));
    IN(MOV, D(OD, 0, "x"), S(R, 4, "w"));
    end();
    PsrefProg *p = LD();
    for (int l = 0; l < 4; l++){
        float x = (float)(l & 1), y = (float)(l >> 1);
        Q[l].v[0][0] = 0.25f + x * 0.5f; Q[l].v[0][1] = 0.5f + y * 0.25f; Q[l].v[0][2] = 3.f; Q[l].v[0][3] = 1.5f;
        Q[l].v[1][0] = 2.f; Q[l].v[1][1] = 4.f; Q[l].v[1][2] = 6.f; Q[l].v[1][3] = 2.f + x;
    }
    if (PSRUN(p)){
        /* the lookups come instruction by instruction, lanes 0..3 within each: lane 0 of instruction k is SMPL[4k] */
        CHECK(nsmpl == 20, "%d lookups (20)", nsmpl);
        const PsrefSample *s = &SMPL[0];                           /* texld */
        CHECK(s->mode == PSREF_LOD_AUTO && s->stage == 0 && s->type == 2 && s->dx[0] == 0.5f && s->dy[1] == 0.25f && s->dx[1] == 0.f,
              "texld: mode %d stage %d type %d dx %g dy %g", s->mode, s->stage, s->type, (double)s->dx[0], (double)s->dy[1]);
        s = &SMPL[4];                                              /* texldp on a cube */
        CHECK(s->type == 3 && s->c[0] == 1.f && s->c[1] == 2.f && s->c[2] == 3.f && s->c[3] == 2.f &&
              s->dx[0] == 2.f / 3.f - 1.f, "texldp: (%g %g %g %g) dx %g", (double)s->c[0], (double)s->c[1], (double)s->c[2], (double)s->c[3], (double)s->dx[0]);
        s = &SMPL[8];
        CHECK(s->mode == PSREF_LOD_BIAS && s->lod == 1.5f, "texldb: mode %d lod %g", s->mode, (double)s->lod);
        EQ4(OUT[0].oC[2], 100.f + 1.5f, 3.f, 0.5f, 20.25f);       /* the sampler's .wzyx */
        s = &SMPL[12];
        CHECK(s->mode == PSREF_LOD_LEVEL && s->lod == 1.5f && s->dx[0] == 0.f && !s->vertex, "texldl: mode %d lod %g", s->mode, (double)s->lod);
        s = &SMPL[16];
        CHECK(s->mode == PSREF_LOD_GRAD && s->dx[0] == 2.f && s->dy[0] == -2.f && s->dy[1] == -6.f, "texldd: dx %g dy %g %g",
              (double)s->dx[0], (double)s->dy[0], (double)s->dy[1]);
        CHECK(OUT[2].depth_written && OUT[2].depth == 300.f, "oDepth %d %g", OUT[2].depth_written, (double)OUT[2].depth);
        EQ4(OUT[3].oC[0], 0.75f, 0.75f, 3.f, 0.f);
    }
    /* texldb's bias outside [-16, 15] in ps_3_0 */
    Q[2].v[0][3] = 15.5f;
    PSBAD(p, "outside [-16, 15]");
    psref_free(p);
    /* ps_2_x: bias within [-3, 3]; no sampler swizzle; texldl is not in it; texldd needs GRADIENTINSTRUCTIONS */
    reset(); begin(PS2X); dclt(0, "xyzw"); dcls(2, 0);
    INC(TEX, 2, D(R, 0, "xyzw"), S(A, 0, "xyzw"), S(SMP, 0, "xyzw")); IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    p = LD();
    for (int l = 0; l < 4; l++) Q[l].t[0][3] = 3.f;
    PSRUN(p);
    Q[1].t[0][3] = 3.25f; PSBAD(p, "outside [-3, 3]");
    psref_free(p);
    reset(); begin(PS2X); dclt(0, "xyzw"); dcls(2, 0);
    IN(TEX, D(R, 0, "xyzw"), S(A, 0, "xyzw"), S(SMP, 0, "yyyy")); end(); REFUSED("sampler swizzle");
    reset(); begin(PS2X); dclt(0, "xyzw"); dcls(2, 0);
    IN(TEXLDL, D(R, 0, "xyzw"), S(A, 0, "xyzw"), S(SMP, 0, "xyzw")); end(); REFUSED("texldl is not in this model");
    reset(); begin(PS2X); dclt(0, "xyzw"); dcls(2, 0);
    IN(TEXLDD, D(R, 0, "xyzw"), S(A, 0, "xyzw"), S(SMP, 0, "xyzw"), S(A, 0, "xyzw"), S(A, 0, "xyzw")); IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    p = LD(); psref_free(p);
    CAPS.ps20_caps &= ~PSREF_PS20_GRADIENTINSTRUCTIONS; REFUSED("GRADIENTINSTRUCTIONS");
    /* ps_2_x texld: .xyzw into r#, the coordinate t# or r# without swizzle */
    reset(); begin(PS2X); dclt(0, "xyzw"); dcls(2, 0);
    IN(TEX, D(R, 0, "xy"), S(A, 0, "xyzw"), S(SMP, 0, "xyzw")); end(); REFUSED("writes .xyzw");
    reset(); begin(PS2X); dclt(0, "xyzw"); dcls(2, 0);
    IN(TEX, D(R, 0, "xyzw"), S(A, 0, "yxzw"), S(SMP, 0, "xyzw")); end(); REFUSED("without modifier or swizzle");
    /* an undeclared sampler; a 2D lookup needs .xy, a cube .xyz of a declared coordinate */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xy"));
    IN(TEX, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw")); end(); REFUSED("s0 is not declared");
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xy")); dcls(2, 0);
    IN(TEX, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw")); IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    p = LD();
    if (PSRUN(p)) CHECK(SMPL[0].cmask == 3, "2D cmask %x", SMPL[0].cmask);
    psref_free(p);
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xy")); dcls(3, 0);
    IN(TEX, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw")); end(); REFUSED("reads v0.z outside its declaration");
    /* implicit lookups of a temporary in a dynamic branch are refused; texldl and texldd are fine there */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); dcls(2, 0);
    IN(MOV, D(R, 0, "xyzw"), S(V, 0, "xyzw"));
    INC(IFC, GT, S(V, 0, "x"), S(V, 0, "y")); INC(TEX, 1, D(R, 1, "xyzw"), S(R, 0, "xyzw"), S(SMP, 0, "xyzw")); IN0(ENDIF); end();
    REFUSED("gradient of a temporary");
    /* (r0 is written only in the lanes that take the branch: texldl reads no other lane) */
    nT = 0; begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); dcls(2, 0);
    IN(MOV, D(R, 1, "xyzw"), S(V, 0, "xyzw"));
    INC(IFC, GT, S(V, 0, "x"), S(V, 0, "y")); IN(MOV, D(R, 0, "xyzw"), S(V, 0, "xyzw"));
    IN(TEXLDL, D(R, 1, "xyzw"), S(R, 0, "xyzw"), S(SMP, 0, "xyzw")); IN0(ENDIF);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 1, "xyzw")); end();
    p = LD();
    for (int l = 0; l < 4; l++){ Q[l].v[0][0] = (float)l; Q[l].v[0][1] = 1.5f; Q[l].v[0][2] = 0.f; Q[l].v[0][3] = 0.f; }
    if (PSRUN(p)) CHECK(nsmpl == 2 && OUT[2].oC[0][3] == 200.f && OUT[0].oC[0][3] == 0.f, "texldl in a branch: %d lookups", nsmpl);
    psref_free(p);
    /* an implicit lookup of an input inside a branch: the level of detail still comes from all four pixels */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); dcls(2, 0); def(0, 0, 0, 0, 0);
    IN(MOV, D(R, 0, "xyzw"), S(C, 0, "xyzw"));
    INC(IFC, GT, S(V, 0, "w"), S(C, 0, "x")); IN(TEX, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw")); IN0(ENDIF);
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    p = LD();
    for (int l = 0; l < 4; l++){ Q[l].v[0][0] = (float)l * 0.125f; Q[l].v[0][1] = (float)(l * l) * 0.25f; Q[l].v[0][2] = 0.f; Q[l].v[0][3] = l == 3 ? 1.f : -1.f; }
    if (PSRUN(p)) CHECK(nsmpl == 1 && SMPL[0].dx[0] == 0.125f && SMPL[0].dy[1] == 2.f, "texld in a branch: %d lookups, dx %g dy %g",
                        nsmpl, (double)SMPL[0].dx[0], (double)SMPL[0].dy[1]);
    psref_free(p);
    /* a vertex texture fetch */
    reset(); begin(VS3); dcl(U_POSITION, 0, D(V, 0, "xyzw")); dcl(U_POSITION, 0, D(O, 0, "xyzw")); dcls(2, 3);
    IN(TEXLDL, D(O, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 3, "xyzw")); end();
    p = LD();
    VIN[0][0] = 0.5f; VIN[0][1] = 0.25f; VIN[0][2] = 0.f; VIN[0][3] = 2.f;
    if (VSRUN(p)){ CHECK(SMPL[0].vertex == 1 && SMPL[0].stage == 3, "vertex fetch %d %d", SMPL[0].vertex, SMPL[0].stage); EQ4(VO[0], 30.5f, 0.25f, 0.f, 202.f); }
    psref_free(p);
    reset(); begin(VS3); dcl(U_POSITION, 0, D(V, 0, "xyzw")); dcls(2, 4); end(); REFUSED("s4: the model has s0..s3");
    reset(); begin(VS3); dcl(U_POSITION, 0, D(V, 0, "xyzw")); dcls(2, 0);
    IN(TEX, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw")); end(); REFUSED("texld is not in this model");
}

/* ======================================================================== */
static void t_face_pos(void){
    reset();
    begin(PS3);
    IN(DCL, 0x80000000u, D(MISC, 1, "xyzw"));                      /* dcl vFace */
    IN(DCL, 0x80000000u, D(MISC, 0, "xy"));                        /* dcl vPos.xy */
    def(0, 0.f, 1.f, -1.f, 0.f);
    INC(IFC, GT, S(MISC, 1, "x"), S(C, 0, "x"));
      IN(MOV, D(R, 0, "xyzw"), S(C, 0, "y"));
    IN0(ELSE);
      IN(MOV, D(R, 0, "xyzw"), S(C, 0, "z"));
    IN0(ENDIF);
    INC(SETP, LT, D(P, 0, "x"), S(MISC, 1, "x"), S(C, 0, "x"));
    IN(MOV, D(R, 0, "y"), S(C, 0, "x"));
    INP(MOV, D(R, 0, "y"), S(P, 0, "x"), S(C, 0, "y"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyyy"));
    IN(MOV, D(OC, 1, "xy"), S(MISC, 0, "xyzw"));
    IN(DSX, D(R, 1, "x"), S(MISC, 0, "x"));
    IN(DSY, D(R, 1, "y"), S(MISC, 0, "y"));
    IN(MOV, D(OC, 2, "xyzw"), S(R, 1, "xyxy"));
    end();
    PsrefProg *p = LD();
    float faces[4] = { 1.f, -1.f, 0.5f, -3.f };
    for (int l = 0; l < 4; l++) Q[l].face = faces[l];
    if (PSRUN(p)){
        EQ4(OUT[0].oC[0], 1.f, 0.f, 0.f, 0.f);
        EQ4(OUT[1].oC[0], -1.f, 1.f, 1.f, 1.f);
        CHECK(OUT[3].oC[1][0] == 11.f && OUT[3].oC[1][1] == 21.f && OUT[3].oCmask[1] == 3, "vPos (%g, %g)", (double)OUT[3].oC[1][0], (double)OUT[3].oC[1][1]);
        EQ4(OUT[2].oC[2], 1.f, 1.f, 1.f, 1.f);
    }
    psref_free(p);
    reset(); begin(PS3); IN(DCL, 0x80000000u, D(MISC, 1, "xyzw")); def(0, 0, 0, 0, 0);
    IN(ADD, D(R, 0, "xyzw"), S(MISC, 1, "x"), S(C, 0, "xyzw")); end(); REFUSED("vFace is only a condition");
    reset(); begin(PS3); def(0, 0, 0, 0, 0); INC(IFC, GT, S(MISC, 1, "x"), S(C, 0, "x")); IN0(ENDIF); end(); REFUSED("vFace is not declared");
    reset(); begin(PS3); IN(DCL, 0x80000000u, D(MISC, 0, "xy")); IN(MOV, D(OC, 0, "xyzw"), S(MISC, 0, "xyzw")); end();
    REFUSED("reads vPos.zw outside its declaration");
    reset(); begin(PS3); IN(DCL, 0x80000000u, D(MISC, 0, "xyz")); end(); REFUSED("vPos takes .x, .y or .xy");
    reset(); begin(PS2X); IN(DCL, 0x80000000u, D(MISC, 1, "xyzw")); end(); REFUSED("not in this model");
}

/* ======================================================================== */
/* one register, read or written, at the edge of the model's count */
static void lim(int line, uint32_t ver, int type, int n, int ok){
    begin(ver);
    int ps = ver != VS3;
    uint32_t out0 = ps ? D(OC, 0, "xyzw") : D(O, 0, "xyzw");
    if (!ps) dcl(U_POSITION, 0, D(O, 0, "xyzw"));
    switch (type){
    case R: IN(MOV, D(R, n, "xyzw"), S(C, 0, "xyzw")); IN(MOV, out0, S(R, n, "xyzw")); break;
    case C: IN(MOV, out0, S(C, n, "xyzw")); break;
    case V: if (ver == PS2X) dcl(0, 0, D(V, n, "xyzw")); else dcl(U_TEXCOORD, 0, D(V, n, "xyzw")); IN(MOV, out0, S(V, n, "xyzw")); break;
    case A: if (ver == PS2X){ dclt(n, "xyzw"); IN(MOV, out0, S(A, n, "xyzw")); }
            else { dcl(U_POSITION, 0, D(V, 0, "xyzw")); IN(MOVA, D(A, n, "x"), S(V, 0, "xyzw")); } break;
    case O: dcl(U_TEXCOORD, 0, D(O, n, "xyzw")); IN(MOV, D(O, n, "xyzw"), S(C, 0, "xyzw")); break;
    case OC: IN(MOV, D(OC, n, "xyzw"), S(C, 0, "xyzw")); break;
    case SMP: dcls(2, n); break;
    case I: IN(REP, S(I, n, "xyzw")); IN0(ENDREP); break;
    case B: IN(IF, S(B, n, "xyzw")); IN0(ENDIF); break;
    case P: IN(SETP, D(P, n, "x"), S(C, 0, "x"), S(C, 0, "y")); break;
    case LBL: IN(CALL, S(LBL, n, "xyzw")); IN0(RET); IN(LABEL, S(LBL, n, "xyzw")); IN0(RET); break;
    default: break;
    }
    end();
    if (type == P) T[nT - 5] |= (uint32_t)GT << 16;              /* setp's comparison */
    PsrefProg *p = psref_load(T, (size_t)nT, &CAPS, err, sizeof err);
    checks++;
    if (!!p != ok){ failed++; printf("FAIL %s:%d: register type %d #%d in 0x%08x: %s\n", __FILE__, line, type, n, ver, p ? "loaded" : err); }
    psref_free(p);
}
#define LIM(ver, type, n, ok) lim(__LINE__, ver, type, n, ok)

static void t_limits(void){
    reset();
    LIM(PS2X, R, 31, 1); LIM(PS2X, R, 32, 0);
    CAPS.ps20_num_temps = 12; LIM(PS2X, R, 11, 1); LIM(PS2X, R, 12, 0); CAPS.ps20_num_temps = 32;
    LIM(PS2X, C, 31, 1); LIM(PS2X, C, 32, 0);
    LIM(PS2X, V, 1, 1); LIM(PS2X, V, 2, 0);
    LIM(PS2X, A, 7, 1); LIM(PS2X, A, 8, 0);
    LIM(PS2X, SMP, 15, 1); LIM(PS2X, SMP, 16, 0);
    LIM(PS2X, OC, 3, 1); LIM(PS2X, OC, 4, 0);
    CAPS.num_mrt = 2; LIM(PS2X, OC, 1, 1); LIM(PS2X, OC, 2, 0); CAPS.num_mrt = 4;
    LIM(PS2X, I, 15, 1); LIM(PS2X, I, 16, 0);
    LIM(PS2X, B, 15, 1); LIM(PS2X, B, 16, 0);
    LIM(PS2X, P, 0, 1); LIM(PS2X, P, 1, 0);
    LIM(PS2X, LBL, 15, 1); LIM(PS2X, LBL, 16, 0);
    LIM(PS3, R, 31, 1); LIM(PS3, R, 32, 0);
    LIM(PS3, C, 223, 1); LIM(PS3, C, 224, 0);
    LIM(PS3, V, 9, 1); LIM(PS3, V, 10, 0);
    LIM(PS3, A, 0, 0);                                              /* no t# in ps_3_0 */
    LIM(PS3, SMP, 15, 1); LIM(PS3, SMP, 16, 0);
    LIM(PS3, OC, 3, 1); LIM(PS3, OC, 4, 0);
    LIM(PS3, I, 15, 1); LIM(PS3, I, 16, 0);
    LIM(PS3, B, 15, 1); LIM(PS3, B, 16, 0);
    LIM(PS3, P, 0, 1); LIM(PS3, P, 1, 0);
    LIM(PS3, LBL, 2047, 1);
    LIM(VS3, R, 31, 1); LIM(VS3, R, 32, 0);
    LIM(VS3, C, 255, 1); LIM(VS3, C, 256, 0);
    CAPS.max_vs_const = 512; LIM(VS3, C, 511, 1); LIM(VS3, C, 512, 0); CAPS.max_vs_const = 256;
    LIM(VS3, V, 15, 1); LIM(VS3, V, 16, 0);
    LIM(VS3, O, 11, 1); LIM(VS3, O, 12, 0);
    LIM(VS3, A, 0, 1); LIM(VS3, A, 1, 0);
    LIM(VS3, SMP, 3, 1); LIM(VS3, SMP, 4, 0);
    LIM(VS3, I, 15, 1); LIM(VS3, I, 16, 0);
    LIM(VS3, B, 15, 1); LIM(VS3, B, 16, 0);
    LIM(VS3, P, 0, 1);
    LIM(VS3, OC, 0, 0);
    LIM(VS3, LBL, 2047, 1);

    /* modifiers: abs is 3_0 only; ps_1_x's (bias, bx2, x2, comp, shift) are in none of these models */
    reset(); begin(PS2X); def(0, 1, 1, 1, 1); IN(MOV, D(OC, 0, "xyzw"), ABSM(S(C, 0, "xyzw"))); end(); REFUSED("source modifier 11");
    reset(); begin(PS3); def(0, 1, 1, 1, 1); IN(MOV, D(OC, 0, "xyzw"), MODS(S(C, 0, "xyzw"), 4)); end(); REFUSED("source modifier 4");
    reset(); begin(PS3); def(0, 1, 1, 1, 1); IN(MOV, D(OC, 0, "xyzw") | (1u << 24), S(C, 0, "xyzw")); end(); REFUSED("result shift");
    /* ps_2_x swizzles without D3DPS20CAPS_ARBITRARYSWIZZLE: identity, replicate, .yzxw, .zxyw, .wzyx */
    reset(); CAPS.ps20_caps &= ~PSREF_PS20_ARBITRARYSWIZZLE;
    begin(PS2X); def(0, 1, 2, 3, 4); IN(MOV, D(R, 0, "xyzw"), S(C, 0, "yzxw")); IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "wzyx")); end();
    PsrefProg *p = LD(); psref_free(p);
    begin(PS2X); def(0, 1, 2, 3, 4); IN(MOV, D(OC, 0, "xyzw"), S(C, 0, "xzyw")); end(); REFUSED("arbitrary swizzle");
    CAPS.ps20_caps |= PSREF_PS20_ARBITRARYSWIZZLE; p = LD(); psref_free(p);
    /* instructions of other models */
    reset(); begin(VS3); def(0, 1, 1, 1, 1); IN(CMP, D(R, 0, "xyzw"), S(C, 0, "xyzw"), S(C, 0, "xyzw"), S(C, 0, "xyzw")); end(); REFUSED("cmp is not in this model");
    reset(); begin(PS3); def(0, 1, 1, 1, 1); IN(SLT, D(R, 0, "xyzw"), S(C, 0, "xyzw"), S(C, 0, "xyzw")); end(); REFUSED("slt is not in this model");
    reset(); begin(VS3); IN(TEXKILL, D(R, 0, "xyzw")); end(); REFUSED("texkill is not in this model");
    reset(); begin(PS3); IN(80, D(R, 0, "xyzw"), S(C, 0, "x"), S(C, 0, "x"), S(C, 0, "x")); end(); REFUSED("cnd is not in this model");
    reset(); begin(PSREF_PS_3_0 & 0xFFFF0200u); end(); REFUSED("is not ps_2_x, ps_3_0 or vs_3_0");
    /* nesting: loop/rep 4 (ps_2_x: StaticFlowControlDepth), dynamic 24, calls 4 */
    for (int depth = 4; depth <= 5; depth++){
        reset(); begin(PS3); defi(0, 1, 0, 0, 0);
        for (int k = 0; k < depth; k++) IN(REP, S(I, 0, "xyzw"));
        for (int k = 0; k < depth; k++) IN0(ENDREP);
        end();
        if (depth == 4){ p = LD(); psref_free(p); } else REFUSED("loop/rep nesting 5");
    }
    reset(); CAPS.ps20_static_depth = 1; begin(PS2X); defi(0, 1, 0, 0, 0);
    IN(REP, S(I, 0, "xyzw")); IN(REP, S(I, 0, "xyzw")); IN0(ENDREP); IN0(ENDREP); end(); REFUSED("loop/rep nesting 2");
    for (int depth = 24; depth <= 25; depth++){
        reset(); begin(PS3); def(0, 1, 1, 1, 1);
        for (int k = 0; k < depth; k++) INC(IFC, GT, S(C, 0, "x"), S(C, 0, "y"));
        for (int k = 0; k < depth; k++) IN0(ENDIF);
        end();
        if (depth == 24){ p = LD(); psref_free(p); } else REFUSED("dynamic flow control nesting 25");
    }
    for (int depth = 4; depth <= 5; depth++){
        reset(); begin(PS3);
        IN(CALL, S(LBL, 0, "xyzw")); IN0(RET);
        for (int k = 0; k < depth; k++){
            IN(LABEL, S(LBL, k, "xyzw"));
            if (k + 1 < depth) IN(CALL, S(LBL, k + 1, "xyzw"));
            IN0(RET);
        }
        end();
        if (depth == 4){ p = LD(); psref_free(p); } else REFUSED("call nesting 5");
    }
    /* a loop in main around a call: the callee's loops count on top of the caller's */
    reset(); begin(PS3); defi(0, 1, 0, 0, 0);
    IN(REP, S(I, 0, "xyzw")); IN(REP, S(I, 0, "xyzw")); IN(CALL, S(LBL, 0, "xyzw")); IN0(ENDREP); IN0(ENDREP); IN0(RET);
    IN(LABEL, S(LBL, 0, "xyzw")); IN(REP, S(I, 0, "xyzw")); IN(REP, S(I, 0, "xyzw")); IN(REP, S(I, 0, "xyzw")); IN0(ENDREP); IN0(ENDREP); IN0(ENDREP); IN0(RET);
    end(); REFUSED("loop/rep nesting 5");
    /* declarations: twice, outside the model, packed with overlapping masks */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xy")); dcl(U_TEXCOORD, 1, D(V, 0, "zw")); end(); p = LD(); psref_free(p);
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xy")); dcl(U_TEXCOORD, 1, D(V, 0, "yz")); end(); REFUSED("declared twice");
    reset(); begin(PS2X); dclt(0, "xy"); dclt(0, "zw"); end(); REFUSED("declared twice");
    reset(); begin(VS3); dcl(U_TEXCOORD, 0, D(O, 0, "xy")); IN(MOV, D(O, 0, "xyz"), S(C, 0, "xyzw")); end(); REFUSED("writes o0.z outside its declaration");
    reset(); begin(VS3); IN(MOV, D(O, 0, "xyzw"), S(C, 0, "xyzw")); end(); REFUSED("o0 is not declared");
    reset(); begin(PS3); IN(MOV, D(OC, 0, "xyzw"), S(V, 0, "xyzw")); end(); REFUSED("v0 is not declared");
    /* bytecode errors */
    reset(); begin(PS3); end(); T[nT - 1] = 0x0001FFFFu; REFUSED("bad end token");
    reset(); begin(PS3); IN(MOV, D(OC, 0, "xyzw"), S(C, 0, "xyzw")); REFUSED("no end token");
    reset(); begin(PS3); T[nT++] = 0xFFFEu | (40u << 16); end(); REFUSED("runs past the program");
    reset(); begin(PS3); IN(77, D(R, 0, "xyzw")); end(); REFUSED("opcode 77 is not in ps_2_x, ps_3_0 or vs_3_0");
    reset(); begin(PS3); IN(MOV, D(OC, 0, "xyzw"), S(C, 0, "xyzw")); T[1] |= 0x40000000u; end(); REFUSED("co-issue");
}

/* ======================================================================== */
static void t_misc(void){
    /* a temporary read before it is written is an error, per component and per lane */
    reset(); begin(PS3); def(0, 1, 1, 1, 1);
    IN(MOV, D(R, 0, "xyz"), S(C, 0, "xyzw")); IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw")); end();
    PsrefProg *p = LD(); PSBAD(p, "reads r0.w before it is written"); psref_free(p);
    /* ps_2_x colours: (0, 0, 0, 1) outside the declaration, whatever the interpolator held */
    reset(); begin(PS2X); dcl(0, 0, D(V, 0, "xyz")); IN(MOV, D(OC, 0, "xyzw"), S(V, 0, "xyzw")); end();
    p = LD();
    for (int l = 0; l < 4; l++){ Q[l].v[0][0] = 0.5f; Q[l].v[0][1] = 0.25f; Q[l].v[0][2] = 0.125f; Q[l].v[0][3] = 7.f; }
    if (PSRUN(p)) EQ4(OUT[0].oC[0], 0.5f, 0.25f, 0.125f, 1.f);
    psref_free(p);
    /* ps_2_x t#: no default */
    reset(); begin(PS2X); dclt(0, "xy"); IN(MOV, D(OC, 0, "xyzw"), S(A, 0, "xyzw")); end(); REFUSED("reads t0.zw outside its declaration");
    /* texkill: the components in its mask; the pixel keeps running */
    reset(); begin(PS3); dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); def(0, 1, 2, 3, 4);
    IN(MOV, D(R, 0, "xyzw"), S(V, 0, "xyzw"));
    IN(TEXKILL, D(R, 0, "xy"));
    IN(MOV, D(OC, 0, "xyzw"), S(C, 0, "xyzw"));
    end();
    p = LD();
    float kv[4][4] = { { 1, 1, -1, -1 }, { -0.5f, 1, 1, 1 }, { 1, -0.f, 1, 1 }, { 1, -1e-9f, 0, 0 } };
    for (int l = 0; l < 4; l++) memcpy(Q[l].v[0], kv[l], 16);
    if (PSRUN(p)){
        CHECK(!OUT[0].killed && OUT[1].killed && !OUT[2].killed && OUT[3].killed, "killed %d %d %d %d", OUT[0].killed, OUT[1].killed, OUT[2].killed, OUT[3].killed);
        EQ4(OUT[1].oC[0], 1.f, 2.f, 3.f, 4.f);
    }
    psref_free(p);
    /* vs_3_0: slt, sge, sgn (its scratch registers are undefined after it), lit, dst, expp, logp, frc */
    reset(); begin(VS3); dcl(U_POSITION, 0, D(V, 0, "xyzw"));
    for (int r = 0; r < 4; r++) dcl(U_TEXCOORD, r, D(O, r, "xyzw"));
    def(0, 0.5f, 2.f, -3.f, 0.f);
    def(1, 4.f, 0.f, 0.f, 0.f);
    IN(SLT, D(R, 0, "xy"), S(V, 0, "xyzw"), S(C, 0, "xyzw"));
    IN(SGE, D(R, 0, "zw"), S(V, 0, "xyzw"), S(C, 0, "xyzw"));
    IN(MOV, D(R, 2, "xyzw"), S(C, 0, "xyzw"));
    IN(MOV, D(R, 3, "xyzw"), S(C, 0, "xyzw"));
    IN(SGN, D(R, 1, "xyzw"), S(C, 0, "xzwy"), S(R, 2, "xyzw"), S(R, 3, "xyzw"));
    IN(MOV, D(O, 0, "xyzw"), S(R, 0, "xyzw"));
    IN(MOV, D(O, 1, "xyzw"), S(R, 1, "xyzw"));
    IN(LIT, D(O, 2, "xyzw"), S(C, 0, "yxzy"));                     /* (2, 0.5, -3, 2): 0.5^2 */
    IN(LIT, D(R, 4, "xyzw"), S(C, 0, "yzzy"));                     /* (2, -3, -3, 2): N.H <= 0, z stays 0 */
    IN(DST, D(O, 3, "xy"), S(C, 0, "xyzw"), S(C, 1, "xxxx"));
    IN(EXPP, D(O, 3, "z"), S(C, 1, "x"));
    IN(LOGP, D(O, 3, "w"), S(C, 1, "x"));
    IN(ADD, D(O, 0, "z"), S(R, 0, "z"), S(R, 4, "z"));            /* 0 + lit's z */
    end();
    p = LD();
    VIN[0][0] = 0.f; VIN[0][1] = 2.f; VIN[0][2] = -4.f; VIN[0][3] = 1.f;
    if (VSRUN(p)){
        EQ4(VO[0], 1.f, 0.f, 0.f, 1.f);
        EQ4(VO[1], 1.f, -1.f, 0.f, 1.f);
        EQ4(VO[2], 1.f, 2.f, 0.25f, 1.f);
        EQ4(VO[3], 1.f, 8.f, 16.f, 2.f);
    }
    nT -= 1;                                                        /* sgn's scratch r2 read afterwards */
    IN(MOV, D(O, 3, "xyzw"), S(R, 2, "xyzw"));
    end();
    psref_free(p);
    p = LD(); VSBAD(p, "reads r2.xyzw before it is written"); psref_free(p);
}

/* ======================================================================== */
/* --fx: every SM3-class blob of these compiled effects (size-prefixed, ending in 0x0000FFFF), loaded with an
 * SM3 device's caps (ps_2_x also with ps_2_b's: no flow control, no arbitrary swizzle, 32 temporaries) and run
 * on 16 quads or 64 vertices of made-up input with an analytic texture */
static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float rndf(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() >> 8) / 16777216.f; }
static int mwtex(void *u, const PsrefSample *s, float o[4]){
    (void)u;
    float x = s->c[0], y = s->c[1], f = (float)s->stage, lod;
    if (s->mode == PSREF_LOD_LEVEL) lod = s->lod;
    else {
        float g = fmaxf(fmaxf(fabsf(s->dx[0]), fabsf(s->dx[1])), fmaxf(fabsf(s->dy[0]), fabsf(s->dy[1]))) * 256.f;
        lod = g > 0.f ? log2f(g) : -16.f;
        if (s->mode == PSREF_LOD_BIAS) lod += s->lod;
    }
    if (s->type == 3){ x = s->c[0] + s->c[2] * 0.3f; y = s->c[1] - s->c[2] * 0.2f; }
    o[0] = 0.5f + 0.4f * sinf(x * 3.1f + f * 0.37f + 0.05f * lod);
    o[1] = 0.5f + 0.4f * cosf(y * 2.3f + f * 0.5f);
    o[2] = 0.5f + 0.4f * sinf((x + y) * 1.7f + f);
    o[3] = 0.5f + 0.4f * cosf(x * y * 5.3f + 0.2f * f);
    return 1;
}
static uint32_t fnv(uint32_t h, const void *b, size_t n){
    const unsigned char *c = b;
    for (size_t i = 0; i < n; i++){ h ^= c[i]; h *= 16777619u; }
    return h;
}
static int fx_mode(int argc, char **argv){
    int nver[3] = { 0, 0, 0 }, loaded = 0, ran = 0, bad = 0, ps2b = 0, nps2x = 0;
    static float KF[256][4];
    PsrefConsts kc; memset(&kc, 0, sizeof kc); kc.c = (const float (*)[4])KF; kc.nc = 256;
    for (int a = 2; a < argc; a++){
        FILE *f = fopen(argv[a], "rb");
        if (!f){ perror(argv[a]); return 2; }
        fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
        uint32_t *w = malloc((size_t)L + 4);
        if (!w || fread(w, 1, (size_t)L, f) != (size_t)L){ fclose(f); return 2; }
        fclose(f);
        size_t n = (size_t)L / 4;
        const char *name = strrchr(argv[a], '/'); name = name ? name + 1 : argv[a];
        int k = 0;
        for (size_t i = 0; i + 2 < n; i++){
            uint32_t size = w[i];
            if (size < 8 || (size & 3) || i + 1 + size / 4 > n) continue;
            uint32_t v = w[i + 1];
            int kind = v == PS2X ? 0 : v == PS3 ? 1 : v == VS3 ? 2 : -1;
            if (kind < 0 || w[i + size / 4] != 0x0000FFFFu) continue;
            const uint32_t *b = &w[i + 1];
            size_t ndw = size / 4;
            nver[kind]++;
            /* inputs and constants from the blob's own bytes: the same blob always gets the same digest.  The
             * constants are positive, as material values are (a specular power <= 0 makes pow(0, p) infinite,
             * which is D3D9's answer but no use as a check) */
            g_rng = fnv(2166136261u, b, ndw * 4) | 1u;
            for (int r = 0; r < 256; r++) for (int c = 0; c < 4; c++) KF[r][c] = rndf(0.05f, 1.f);
            const char *vn = kind == 0 ? "ps_2_x" : kind == 1 ? "ps_3_0" : "vs_3_0";
            PsrefCaps caps; psref_caps_sm3(&caps);
            PsrefProg *p = psref_load(b, ndw, &caps, err, sizeof err);
            if (!p){ printf("blob %s #%d %s: REFUSED %s\n", name, k++, vn, err); bad++; continue; }
            loaded++;
            if (kind == 0){
                nps2x++;
                PsrefCaps b2; psref_caps_sm3(&b2);
                b2.ps20_caps = PSREF_PS20_NODEPENDENTREADLIMIT; b2.ps20_dynamic_depth = 0; b2.ps20_static_depth = 0; b2.ps20_num_temps = 32;
                PsrefProg *q = psref_load(b, ndw, &b2, err, sizeof err);
                if (q) ps2b++; else printf("blob %s #%d ps_2_b caps: REFUSED %s\n", name, k, err);
                psref_free(q);
            }
            uint32_t h = 2166136261u;
            int ok = 1;
            unsigned omask_all = 0;
            if (kind < 2){
                PsrefPsIn in[4]; PsrefPsOut o[4];
                for (int qd = 0; qd < 16 && ok; qd++){
                    memset(in, 0, sizeof in);
                    float base[18][4], gx[18][4], gy[18][4];
                    for (int r = 0; r < 18; r++) for (int c = 0; c < 4; c++){
                        int col = (kind == 0 && r < 2);
                        base[r][c] = c < 2 || col ? rndf(0.f, 1.f) : rndf(0.5f, 2.f);
                        gx[r][c] = rndf(-0.01f, 0.01f); gy[r][c] = rndf(-0.01f, 0.01f);
                    }
                    for (int l = 0; l < 4; l++){
                        float dx = (float)(l & 1), dy = (float)(l >> 1);
                        for (int r = 0; r < 10; r++) for (int c = 0; c < 4; c++) in[l].v[r][c] = base[r][c] + gx[r][c] * dx + gy[r][c] * dy;
                        for (int r = 0; r < 8; r++) for (int c = 0; c < 4; c++) in[l].t[r][c] = base[10 + r][c] + gx[10 + r][c] * dx + gy[10 + r][c] * dy;
                        in[l].pos[0] = 100.f + dx; in[l].pos[1] = 50.f + dy; in[l].face = 1.f;
                    }
                    if (!psref_ps_run(p, &kc, in, mwtex, NULL, o, err, sizeof err)){ printf("blob %s #%d %s: RUN %s\n", name, k, vn, err); ok = 0; break; }
                    for (int l = 0; l < 4; l++){
                        omask_all |= o[l].oCmask[0];
                        h = fnv(h, o[l].oC, sizeof o[l].oC);
                        for (int c = 0; c < 4; c++) if (!isfinite(o[l].oC[0][c])){ printf("blob %s #%d %s: oC0 not finite\n", name, k, vn); ok = 0; }
                        if (o[l].killed) h = fnv(h, "k", 1);
                    }
                }
                if (ok && omask_all != 0xF){ printf("blob %s #%d %s: oC0 mask %x\n", name, k, vn, omask_all); ok = 0; }
            } else {
                PsrefDcl d; unsigned want[12] = { 0 };
                for (int j = 0; j < psref_ndcl(p); j++) if (psref_dcl(p, j, &d) && d.regtype == PSREF_DCL_OUTPUT) want[d.reg] |= d.mask;
                for (int vx = 0; vx < 64 && ok; vx++){
                    float vin[16][4], vo[12][4]; unsigned om[12];
                    for (int r = 0; r < 16; r++) for (int c = 0; c < 4; c++) vin[r][c] = c == 3 && r == 0 ? 1.f : rndf(-1.f, 1.f);
                    if (!psref_vs_run(p, &kc, (const float (*)[4])vin, mwtex, NULL, vo, om, err, sizeof err)){ printf("blob %s #%d %s: RUN %s\n", name, k, vn, err); ok = 0; break; }
                    for (int r = 0; r < 12; r++){
                        if (om[r] != want[r]){ printf("blob %s #%d %s: o%d written .%x, declared .%x\n", name, k, vn, r, om[r], want[r]); ok = 0; }
                        for (int c = 0; c < 4; c++) if ((om[r] >> c & 1u) && !isfinite(vo[r][c])){ printf("blob %s #%d %s: o%d not finite\n", name, k, vn, r); ok = 0; }
                    }
                    h = fnv(h, vo, sizeof vo);
                }
            }
            if (ok) ran++; else bad++;
            printf("blob %s #%d %s: %zu dwords, %u instructions, bytes %08x, %s, digest %08x\n", name, k, vn, ndw,
                   psref_ninstr(p), fnv(2166136261u, b, ndw * 4), ok ? "ran" : "FAILED", h);
            psref_free(p);
            k++;
        }
        free(w);
    }
    int total = nver[0] + nver[1] + nver[2];
    printf("mw: %d SM3-class blobs: %d ps_2_x, %d ps_3_0, %d vs_3_0; %d loaded, %d ran, %d failed\n",
           total, nver[0], nver[1], nver[2], loaded, ran, bad);
    printf("mw: with ps_2_b caps %d of %d ps_2_x load\n", ps2b, nps2x);
    return bad ? 1 : 0;
}

/* ======================================================================== */
/* Programs for the WGSL translator's differential test (--emit), which runs them on random inputs (web/tools/wgsl-xl.c:
 * colours and coordinates in [0, 1], texture coordinate z and w in [0.5, 2], vertex elements in [-1, 1], constants in
 * [0.05, 1]): every class of instruction, defined on all of those inputs, and the special values of rcp, rsq, log,
 * nrm, cmp and pow (0, -0 and 1 made from an input, and from def); one colour output (the module ABI has one target)
 * and a POSITION0 output for the vertex shader.  Here they only have to load and run. */
static void t_corpus(void){
    enum { U_FOG = 11 };
    /* ps_3_0: arithmetic, predicates, if p0 / b# / if_comp, vPos, vFace, oDepth */
    reset(); begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); dcl(U_TEXCOORD, 1, D(V, 1, "xyzw")); dcl(U_COLOR, 0, D(V, 2, "xyzw"));
    IN(DCL, 0x80000000u, D(MISC, 0, "xy")); IN(DCL, 0x80000000u, D(MISC, 1, "xyzw"));
    def(10, 0.5f, -0.25f, 2.f, 0.75f); def(11, 0.f, 1.f, 0.6f, 0.3f); defb(0, 1); defb(1, 0);
    IN(MUL, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(V, 1, "wzyx"));
    IN(RCP, D(R, 1, "x"), S(V, 0, "w")); IN(RSQ, D(R, 1, "y"), NEG(S(V, 1, "z")));
    IN(LOG, D(R, 1, "z"), S(V, 2, "x")); IN(EXP, D(R, 1, "w"), NEG(S(V, 1, "x")));
    INC(SETP, LT, D(P, 0, "xyzw"), S(V, 0, "xyzw"), S(C, 11, "z"));
    IN(MOV, D(R, 2, "xyzw"), S(C, 10, "xyzw"));
    INP(ADD, D(R, 2, "xyzw"), S(P, 0, "xyzw"), S(R, 2, "xyzw"), S(V, 1, "xyzw"));
    INP(MOV, D(R, 2, "yw"), NOT(S(P, 0, "wzyx")), S(V, 2, "xyzw"));
    INC(SETP, EQ, D(P, 0, "x"), S(R, 1, "x"), S(R, 1, "x"));
    INC(SETP, NE, D(P, 0, "y"), S(V, 0, "y"), S(C, 11, "z"));
    INC(SETP, LE, D(P, 0, "z"), S(V, 1, "z"), S(C, 10, "z"));
    IN(MOV, D(R, 3, "xyzw"), S(C, 11, "x"));
    IN(IF, S(P, 0, "z")); IN(ADD, D(R, 3, "xyzw"), S(R, 3, "xyzw"), S(V, 0, "xyzw"));
    IN0(ELSE); IN(SUB, D(R, 3, "xyzw"), S(R, 3, "xyzw"), S(V, 1, "yzwx")); IN0(ENDIF);
    IN(IF, NOT(S(P, 0, "x"))); IN(MOV, D(R, 3, "x"), S(C, 10, "y")); IN0(ENDIF);
    INC(IFC, EQ, S(V, 0, "x"), S(V, 0, "x")); IN(MAD, D(R, 3, "y"), S(R, 3, "y"), S(C, 10, "z"), S(V, 2, "w")); IN0(ENDIF);
    INC(IFC, LE, S(V, 1, "y"), S(C, 10, "x")); IN(ADD, D(R, 3, "z"), S(R, 3, "z"), S(C, 11, "y")); IN0(ENDIF);
    INC(IFC, NE, S(V, 2, "y"), S(C, 11, "w")); IN(MUL, D(R, 3, "w"), S(R, 3, "w"), S(C, 10, "w")); IN0(ENDIF);
    IN(IF, S(B, 0, "x")); IN(ADD, D(R, 3, "xyzw"), S(R, 3, "xyzw"), S(C, 11, "z")); IN0(ENDIF);
    IN(IF, NOT(S(B, 1, "x"))); IN(MUL, D(R, 3, "xyzw"), S(R, 3, "xyzw"), S(C, 10, "w")); IN0(ENDIF);
    IN(MOV, D(R, 4, "xyzw"), S(C, 11, "xyzw"));
    INC(IFC, GT, S(MISC, 1, "x"), S(C, 11, "x")); IN(MOV, D(R, 4, "x"), S(C, 10, "z")); IN0(ENDIF);
    IN(MAD, D(R, 4, "yz"), S(MISC, 0, "xyxy"), S(C, 11, "w"), S(C, 10, "y"));
    IN(FRC, D(R, 4, "w"), S(R, 4, "y"));
    IN(DP2ADD, D(R, 5, "x"), S(R, 4, "yz"), S(C, 10, "xy"), S(C, 10, "z"));
    IN(LRP, D(R, 5, "y"), S(V, 2, "x"), S(R, 0, "y"), S(R, 1, "z"));
    IN(CMP, D(R, 5, "zw"), S(R, 2, "xyzw"), S(R, 3, "zwxy"), NEG(S(R, 4, "xyzw")));
    IN(NRM, D(R, 6, "xyz"), S(R, 0, "xyzw"));
    IN(POW, D(R, 6, "w"), ABSM(S(V, 0, "x")), S(C, 10, "z"));
    IN(MIN, D(R, 7, "xy"), S(R, 2, "xy"), S(R, 3, "zw")); IN(MAX, D(R, 7, "zw"), S(R, 1, "xy"), S(R, 4, "zw"));
    IN(SINCOS, D(R, 8, "xy"), S(V, 1, "x"));
    IN(CRS, D(R, 9, "xyz"), S(R, 6, "xyzw"), S(V, 2, "xyzw")); IN(ABS, D(R, 9, "w"), S(R, 3, "x"));
    IN(ADD, D(R, 10, "xyzw"), S(R, 2, "xyzw"), S(R, 3, "xyzw"));
    IN(MAD, D(R, 10, "xyzw"), S(R, 5, "xyzw"), S(C, 10, "w"), S(R, 10, "xyzw"));
    IN(MAD, D(R, 10, "xyzw"), S(R, 6, "xyzw"), S(R, 7, "wzyx"), S(R, 10, "xyzw"));
    IN(MAD, D(R, 10, "xy"), S(R, 8, "xy"), S(R, 9, "zw"), S(R, 10, "xy"));
    IN(ADD, D(OC, 0, "xyzw"), S(R, 10, "xyzw"), ABSM(S(R, 1, "xyzw")));
    IN(MOV, SAT(D(OD, 0, "x")), S(R, 5, "x"));
    end();
    PsrefProg *p = LD();
    for (int l = 0; l < 4; l++) for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) Q[l].v[r][c] = 0.25f + 0.1f * (float)(r + c + l);
    PSRUN(p); psref_free(p);

    /* ps_3_0: oDepth with the full write mask (how an assembler may encode `mov oDepth, ...`): component x */
    reset(); begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw"));
    def(10, 0.25f, 0.5f, 0.125f, 1.f);
    IN(MAD, D(R, 0, "xyzw"), S(V, 0, "wzyx"), S(C, 10, "xyzw"), S(C, 10, "zyxw"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 0, "xyzw"));
    IN(MUL, SAT(D(OD, 0, "xyzw")), S(R, 0, "yxwz"), S(C, 10, "y"));
    end();
    p = LD();
    for (int l = 0; l < 4; l++) for (int c = 0; c < 4; c++) Q[l].v[0][c] = 0.2f + 0.1f * (float)(c + l);
    if (PSRUN(p)) CHECK(OUT[1].depth_written && fabsf(OUT[1].depth - 0.375f) < 1e-6f, "oDepth.xyzw: %g", (double)OUT[1].depth);  /* (v0.z/2 + 1/2)/2 */
    psref_free(p);

    /* ps_3_0: the special values, made at run time (0, -0 and 1 from an input, which no compiler can fold away)
     * and given by def: rcp and rsq of 1 are exactly 1 (their difference from 1, times 2^20, shows one ulp), of 0
     * and -0 FLT_MAX; log of 0 and -0 -FLT_MAX, of a negative log |x|; nrm of a zero vector FLT_MAX times each
     * component (w included); cmp at 0 and -0; pow of 0 and -0 to a positive power and of a negative base.
     * FLT_MAX times 2^-120 is about 256, where an infinity would stay infinite. */
    reset(); begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw"));
    def(10, -1.f, 1.f, 0.f, -0.f); def(11, 1048576.f, 0x1p-120f, 0.5f, -4.f); def(12, 2.f, 0.25f, 3.f, 1.f);
    IN(ADD, D(R, 12, "xyzw"), S(V, 0, "xyzw"), S(C, 10, "y"));                      /* v0 + 1 > 0 */
    IN(CMP, D(R, 0, "xyzw"), S(R, 12, "xyzw"), S(C, 10, "y"), S(C, 10, "z"));      /* 1 */
    IN(CMP, D(R, 1, "xyzw"), S(R, 12, "xyzw"), S(C, 10, "z"), S(C, 10, "y"));      /* 0 */
    IN(MUL, D(R, 2, "xyzw"), S(R, 1, "xyzw"), S(C, 10, "x"));                       /* -0 */
    IN(RCP, D(R, 3, "x"), S(R, 0, "x")); IN(RSQ, D(R, 3, "y"), S(R, 0, "y"));
    IN(RCP, D(R, 3, "z"), S(C, 10, "y")); IN(RSQ, D(R, 3, "w"), NEG(S(C, 10, "y")));
    IN(ADD, D(R, 3, "xyzw"), S(R, 3, "xyzw"), NEG(S(C, 10, "y")));                  /* 0 when exact */
    IN(NRM, D(R, 6, "xyz"), S(R, 1, "xyzw")); IN(MOV, D(R, 6, "w"), S(R, 2, "w"));  /* (0, 0, 0, -0) */
    IN(DP4, D(R, 11, "x"), ABSM(S(R, 3, "xyzw")), S(C, 12, "w"));
    IN(DP4, D(R, 11, "y"), ABSM(S(R, 6, "xyzw")), S(C, 12, "w"));
    IN(MAD, D(R, 11, "x"), S(R, 11, "x"), S(C, 11, "x"), S(R, 11, "y"));           /* 0 */
    IN(RCP, D(R, 4, "x"), S(R, 1, "x")); IN(RSQ, D(R, 4, "y"), S(R, 2, "x"));
    IN(RCP, D(R, 4, "z"), S(C, 10, "w")); IN(RSQ, D(R, 4, "w"), S(C, 10, "z"));
    IN(MOV, D(R, 7, "xyzw"), S(R, 2, "xyzw")); IN(MOV, D(R, 7, "w"), S(R, 0, "w")); /* (-0, -0, -0, 1) */
    IN(NRM, D(R, 8, "xyzw"), S(R, 7, "xyzw"));                                      /* (-0, -0, -0, FLT_MAX) */
    IN(MUL, D(R, 4, "xyzw"), S(R, 4, "xyzw"), S(C, 11, "y")); IN(MUL, D(R, 8, "xyzw"), S(R, 8, "xyzw"), S(C, 11, "y"));
    IN(DP4, D(R, 11, "y"), S(R, 4, "xyzw"), S(C, 12, "w")); IN(ADD, D(R, 11, "y"), S(R, 11, "y"), S(R, 8, "w"));   /* 5 x 256 */
    IN(LOG, D(R, 5, "x"), S(R, 1, "y")); IN(LOG, D(R, 5, "y"), S(R, 2, "z"));
    IN(LOG, D(R, 5, "z"), S(C, 10, "z")); IN(LOG, D(R, 5, "w"), S(C, 11, "w"));
    IN(MUL, D(R, 5, "xyz"), S(R, 5, "xyzw"), S(C, 11, "y"));
    IN(DP4, D(R, 11, "z"), S(R, 5, "xyzw"), S(C, 12, "w"));                         /* 3 x -256, + 2 */
    IN(CMP, D(R, 9, "x"), S(R, 1, "x"), S(C, 12, "x"), S(C, 12, "y"));
    IN(CMP, D(R, 9, "y"), S(R, 2, "x"), S(C, 12, "x"), S(C, 12, "y"));
    IN(CMP, D(R, 9, "z"), S(C, 10, "w"), S(C, 12, "z"), S(C, 12, "y"));
    IN(CMP, D(R, 9, "w"), S(C, 10, "x"), S(C, 12, "y"), S(C, 12, "w"));             /* (2, 2, 3, 1) */
    IN(POW, D(R, 10, "x"), S(R, 1, "x"), S(C, 12, "x")); IN(POW, D(R, 10, "y"), S(R, 2, "x"), S(C, 12, "y"));
    IN(POW, D(R, 10, "z"), S(C, 11, "w"), S(C, 11, "z")); IN(POW, D(R, 10, "w"), S(C, 10, "z"), S(C, 12, "z"));
    IN(ADD, D(R, 9, "xyzw"), S(R, 9, "xyzw"), S(R, 10, "xyzw"));                    /* + (0, 0, 2, 0) */
    IN(DP4, D(R, 11, "w"), S(R, 9, "xyzw"), S(C, 12, "w"));
    IN(MOV, D(OC, 0, "xyzw"), S(R, 11, "xyzw"));
    end();
    p = LD();
    for (int l = 0; l < 4; l++) for (int c = 0; c < 4; c++) Q[l].v[0][c] = 0.3f * (float)(c + l);
    if (PSRUN(p)) for (int l = 0; l < 4; l++){
        const float *o = OUT[l].oC[0];
        CHECK(o[0] == 0.f && fabsf(o[1] - 1280.f) < 0.01f && fabsf(o[2] + 766.f) < 0.01f && o[3] == 10.f,
              "special values, lane %d: (%g, %g, %g, %g)", l, (double)o[0], (double)o[1], (double)o[2], (double)o[3]);
    }
    psref_free(p);

    /* ps_3_0: lookups of every kind, the same where the flow differs per pixel (of inputs: hoisted
     * derivatives), dsx/dsy, loop with aL and v[aL] and a per-pixel break, rep with breakp, call, callnz, texkill */
    reset(); begin(PS3);
    dcl(U_TEXCOORD, 0, D(V, 0, "xyzw")); dcl(U_TEXCOORD, 1, D(V, 1, "xyzw")); dcl(U_TEXCOORD, 2, D(V, 2, "xyzw"));
    dcls(2, 0); dcls(3, 1); dcls(2, 2);
    def(10, 0.5f, 0.25f, 1.f, 0.f); def(11, 0.3f, 0.1f, 0.7f, 2.f); defi(0, 5, 0, 0, 0); defi(1, 2, 0, 1, 0); defb(0, 0);
    IN(TEX, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(SMP, 0, "xyzw"));
    INC(TEX, 1, D(R, 1, "xyzw"), S(V, 1, "xyzw"), S(SMP, 2, "wzyx"));
    INC(TEX, 2, D(R, 2, "xyzw"), S(V, 2, "xyzw"), S(SMP, 1, "xyzw"));
    IN(TEXLDL, D(R, 3, "xyzw"), S(V, 0, "xyzw"), S(SMP, 2, "xyzw"));
    IN(TEXLDD, D(R, 4, "xyzw"), S(V, 1, "xyzw"), S(SMP, 0, "xyzw"), S(V, 2, "xyzw"), S(R, 0, "xyzw"));
    IN(DSX, D(R, 5, "xyzw"), S(V, 0, "xyzw")); IN(DSY, D(R, 6, "xyzw"), S(R, 0, "xyzw"));
    INC(IFC, GT, S(V, 0, "x"), S(C, 10, "x"));
      IN(TEX, D(R, 7, "xyzw"), S(V, 1, "xyzw"), S(SMP, 0, "xyzw"));
      INC(TEX, 1, D(R, 8, "xyzw"), S(V, 2, "xyzw"), S(SMP, 2, "xyzw"));
      IN(DSY, D(R, 9, "xyzw"), NEG(S(V, 2, "yxwz")));
      INC(TEX, 2, D(R, 10, "xyzw"), S(V, 0, "xyzw"), S(SMP, 1, "xyzw"));
    IN0(ELSE);
      IN(MOV, D(R, 7, "xyzw"), S(C, 10, "xyzw")); IN(MOV, D(R, 8, "xyzw"), S(C, 11, "xyzw"));
      IN(MOV, D(R, 9, "xyzw"), S(C, 10, "wzyx")); IN(MOV, D(R, 10, "xyzw"), S(C, 11, "wzyx"));
    IN0(ENDIF);
    IN(MOV, D(R, 11, "xyzw"), S(C, 10, "w"));
    IN(LOOP, S(AL, 0, "xyzw"), S(I, 1, "xyzw"));
      IN(ADD, D(R, 11, "xyzw"), S(R, 11, "xyzw"), REL(S(V, 0, "xyzw")), S(AL, 0, "xxxx"));
      INC(BREAKC, GT, S(R, 11, "x"), S(C, 11, "z"));
    IN0(ENDLOOP);
    IN(MOV, D(R, 12, "xyzw"), S(C, 10, "w"));
    INC(SETP, GT, D(P, 0, "x"), S(V, 1, "y"), S(C, 10, "x"));
    IN(REP, S(I, 0, "xyzw"));
      IN(ADD, D(R, 12, "xyzw"), S(R, 12, "xyzw"), S(C, 11, "xyzw"));
      IN(BREAKP, S(P, 0, "x"));
    IN0(ENDREP);
    IN(MOV, D(R, 13, "xyzw"), S(C, 10, "w"));
    IN(CALL, S(LBL, 0, "xyzw"));
    IN(CALLNZ, S(LBL, 1, "xyzw"), S(P, 0, "x"));
    IN(CALLNZ, S(LBL, 1, "xyzw"), NOT(S(B, 0, "x")));
    IN(SUB, D(R, 14, "xyzw"), S(V, 2, "xyzw"), S(C, 11, "x"));
    IN(TEXKILL, D(R, 14, "xy"));
    IN(ADD, D(R, 15, "xyzw"), S(R, 0, "xyzw"), S(R, 1, "xyzw"));
    IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 2, "xyzw")); IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 3, "xyzw"));
    IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 4, "xyzw")); IN(MAD, D(R, 15, "xyzw"), S(R, 5, "xyzw"), S(C, 10, "z"), S(R, 15, "xyzw"));
    IN(MAD, D(R, 15, "xyzw"), S(R, 6, "xyzw"), S(C, 11, "y"), S(R, 15, "xyzw")); IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 7, "xyzw"));
    IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 8, "xyzw")); IN(MAD, D(R, 15, "xyzw"), S(R, 9, "xyzw"), S(C, 11, "z"), S(R, 15, "xyzw"));
    IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 10, "xyzw")); IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 11, "xyzw"));
    IN(ADD, D(R, 15, "xyzw"), S(R, 15, "xyzw"), S(R, 12, "xyzw"));
    IN(MAD, D(OC, 0, "xyzw"), S(R, 13, "xyzw"), S(C, 11, "y"), S(R, 15, "xyzw"));
    IN0(RET);
    IN(LABEL, S(LBL, 0, "xyzw")); IN(MAD, D(R, 13, "xyzw"), S(V, 1, "xyzw"), S(C, 11, "w"), S(R, 13, "xyzw")); IN0(RET);
    IN(LABEL, S(LBL, 1, "xyzw")); IN(ADD, D(R, 13, "xyzw"), S(R, 13, "xyzw"), S(C, 10, "zzzx")); IN0(RET);
    end();
    p = LD();
    for (int l = 0; l < 4; l++) for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++)
        Q[l].v[r][c] = (c < 2 ? 0.2f : 0.7f) + 0.11f * (float)(r + l) + 0.05f * (float)c;
    PSRUN(p); psref_free(p);

    /* ps_2_x: t#, colours partly declared, texldp/texldb on a cube, sincos with its constants, predicates,
     * rep with break_comp, dsx/dsy of a coordinate */
    reset(); begin(PS2X);
    dcl(0, 0, D(V, 0, "xyz")); dcl(0, 0, D(V, 1, "xyzw"));
    dclt(0, "xyzw"); dclt(1, "xy"); dclt(4, "xyzw"); dclt(7, "xyzw");
    dcls(2, 0); dcls(3, 1); dcls(2, 4);
    def(0, -1.55009923e-6f, -2.17013894e-5f, 0.00260416674f, 0.00026041668f);
    def(1, -0.020833334f, -0.125f, 1.f, 0.5f);
    def(2, 0.5f, 0.25f, 2.f, 0.1f); defi(0, 4, 0, 0, 0);
    IN(TEX, D(R, 0, "xyzw"), S(A, 0, "xyzw"), S(SMP, 0, "xyzw"));
    INC(TEX, 1, D(R, 1, "xyzw"), S(A, 4, "xyzw"), S(SMP, 4, "xyzw"));
    INC(TEX, 2, D(R, 2, "xyzw"), S(A, 7, "xyzw"), S(SMP, 1, "xyzw"));
    INC(TEX, 1, D(R, 3, "xyzw"), S(A, 7, "xyzw"), S(SMP, 1, "xyzw"));
    IN(MOV, D(R, 4, "xyzw"), S(V, 0, "xyzw"));
    IN(SINCOS, D(R, 5, "xy"), S(A, 1, "x"), S(C, 0, "xyzw"), S(C, 1, "xyzw"));
    IN(DSX, D(R, 6, "xyzw"), S(A, 4, "xyzw")); IN(DSY, D(R, 7, "xyzw"), S(A, 4, "wzyx"));
    INC(SETP, GT, D(P, 0, "xyzw"), S(V, 1, "xyzw"), S(C, 2, "x"));
    IN(MOV, D(R, 8, "xyzw"), S(C, 2, "xyzw"));
    INP(MUL, D(R, 8, "xyzw"), S(P, 0, "xyzw"), S(R, 8, "xyzw"), S(V, 1, "xyzw"));
    IN(MOV, D(R, 9, "xyzw"), S(C, 2, "w"));
    IN(REP, S(I, 0, "xyzw"));
      IN(MAD, D(R, 9, "xyzw"), S(R, 9, "xyzw"), S(C, 2, "z"), S(A, 0, "xyzw"));
      INC(BREAKC, GE, S(R, 9, "x"), S(C, 1, "z"));
    IN0(ENDREP);
    IN(CMP, D(R, 10, "xyzw"), S(R, 0, "xyzw"), S(R, 1, "xyzw"), S(R, 2, "xyzw"));
    IN(DP2ADD, D(R, 10, "x"), S(R, 5, "xy"), S(C, 2, "xy"), S(C, 2, "w"));
    IN(LRP, D(R, 11, "xyzw"), S(R, 4, "wwww"), S(R, 3, "xyzw"), S(R, 8, "xyzw"));
    IN(ADD, D(R, 11, "xyzw"), S(R, 11, "xyzw"), S(R, 9, "xyzw"));
    IN(MAD, D(R, 11, "xyzw"), S(R, 6, "xyzw"), S(C, 2, "z"), S(R, 11, "xyzw"));
    IN(MAD, D(R, 11, "xyzw"), S(R, 7, "xyzw"), S(C, 2, "z"), S(R, 11, "xyzw"));
    IN(ADD, SAT(D(OC, 0, "xyzw")), S(R, 11, "xyzw"), S(R, 10, "xyzw"));
    end();
    p = LD();
    for (int l = 0; l < 4; l++){
        for (int r = 0; r < 2; r++) for (int c = 0; c < 4; c++) Q[l].v[r][c] = 0.3f + 0.1f * (float)(r + c + l);
        for (int t = 0; t < 8; t++) for (int c = 0; c < 4; c++) Q[l].t[t][c] = c < 2 ? 0.2f + 0.05f * (float)(t + l) : 1.f;
    }
    PSRUN(p); psref_free(p);

    /* vs_3_0: the vertex instructions, a0 and aL relative constants (one past c255: (0, 0, 0, 0)), o[aL],
     * packed outputs, fog, static and dynamic ifs, setp and predicates, callnz p0 */
    reset(); begin(VS3);
    dcl(U_POSITION, 0, D(V, 0, "xyzw")); dcl(U_NORMAL, 0, D(V, 1, "xyzw")); dcl(U_TEXCOORD, 0, D(V, 2, "xyzw"));
    dcl(U_POSITION, 0, D(O, 0, "xyzw")); dcl(U_TEXCOORD, 0, D(O, 1, "xyzw")); dcl(U_TEXCOORD, 1, D(O, 2, "xyzw"));
    dcl(U_COLOR, 0, D(O, 3, "xyzw")); dcl(U_COLOR, 1, D(O, 4, "xy")); dcl(U_TEXCOORD, 2, D(O, 4, "zw"));
    dcl(U_FOG, 0, D(O, 5, "x"));
    def(20, 0.5f, 2.f, -1.f, 0.25f); def(200, 1.f, 0.f, 3.f, 0.125f); defi(0, 3, 0, 1, 0); defb(0, 1);
    IN(M4x4, D(O, 0, "xyzw"), S(V, 0, "xyzw"), S(C, 0, "xyzw"));
    IN(SLT, D(R, 0, "xyzw"), S(V, 0, "xyzw"), S(V, 1, "xyzw")); IN(SGE, D(R, 1, "xyzw"), S(V, 1, "xyzw"), S(V, 2, "zwxy"));
    IN(MOV, D(R, 2, "xyzw"), S(C, 200, "xyzw")); IN(MOV, D(R, 3, "xyzw"), S(C, 200, "xyzw"));
    IN(SGN, D(R, 4, "xyzw"), S(V, 1, "xyzw"), S(R, 2, "xyzw"), S(R, 3, "xyzw"));
    IN(LIT, D(R, 5, "xyzw"), S(V, 2, "xyxy")); IN(DST, D(R, 6, "xyzw"), S(V, 0, "xyzw"), S(V, 1, "xyzw"));
    IN(EXPP, D(R, 7, "x"), S(V, 1, "x")); IN(LOGP, D(R, 7, "y"), S(V, 2, "y"));
    IN(LOG, D(R, 7, "z"), S(V, 0, "z")); IN(FRC, D(R, 7, "w"), S(V, 1, "y"));
    IN(MOVA, D(A, 0, "xy"), S(V, 2, "xyzw"));
    IN(ADD, D(R, 8, "xyzw"), S(R, 0, "xyzw"), REL(S(C, 5, "xyzw")), S(A, 0, "xxxx"));
    IN(ADD, D(R, 8, "xyzw"), S(R, 8, "xyzw"), REL(S(C, 255, "xyzw")), S(A, 0, "yyyy"));
    IN(ADD, D(R, 8, "xyzw"), S(R, 8, "xyzw"), REL(S(C, 20, "xyzw")), S(A, 0, "xxxx"));
    IN(MOV, D(R, 9, "xyzw"), S(C, 200, "w"));
    IN(LOOP, S(AL, 0, "xyzw"), S(I, 0, "xyzw"));
      IN(ADD, D(R, 9, "xyzw"), S(R, 9, "xyzw"), REL(S(C, 10, "xyzw")), S(AL, 0, "xxxx"));
      IN(MUL, REL(D(O, 1, "xyzw")), S(AL, 0, "xxxx"), S(R, 9, "xyzw"), S(V, 2, "xyzw"));
    IN0(ENDLOOP);
    IN(MOV, D(R, 10, "xyzw"), S(C, 200, "xyzw"));
    IN(IF, S(B, 0, "x")); IN(ADD, D(R, 10, "xyzw"), S(R, 10, "xyzw"), S(V, 0, "xyzw")); IN0(ELSE); IN(MOV, D(R, 10, "xyzw"), S(V, 1, "xyzw")); IN0(ENDIF);
    INC(IFC, LT, S(V, 0, "x"), S(V, 1, "x")); IN(MUL, D(R, 10, "xyzw"), S(R, 10, "xyzw"), S(C, 20, "x")); IN0(ENDIF);
    INC(SETP, GE, D(P, 0, "xyzw"), S(V, 1, "xyzw"), S(V, 2, "xyzw"));
    INP(ADD, D(R, 10, "xyzw"), S(P, 0, "xyzw"), S(R, 10, "xyzw"), S(C, 200, "zzzz"));
    IN(CALLNZ, S(LBL, 2, "xyzw"), S(P, 0, "x"));
    IN(NRM, D(R, 11, "xyz"), S(V, 1, "xyzw")); IN(CRS, D(R, 12, "xyz"), S(V, 1, "xyzw"), S(V, 2, "xyzw"));
    IN(MOV, D(R, 11, "w"), S(C, 20, "w")); IN(MOV, D(R, 12, "w"), S(C, 20, "y"));
    IN(MAD, D(O, 2, "xyzw"), S(R, 5, "xyzw"), S(R, 6, "xyzw"), S(R, 8, "xyzw"));
    IN(MAD, SAT(D(O, 3, "xyzw")), S(R, 4, "xyzw"), S(R, 7, "xyzw"), S(R, 10, "xyzw"));
    IN(ADD, D(O, 4, "xy"), S(R, 11, "xy"), S(R, 1, "zw"));
    IN(MAD, D(O, 4, "zw"), S(R, 12, "zzzw"), S(C, 20, "y"), S(R, 1, "xyxy"));
    IN(MAX, D(O, 5, "x"), S(R, 9, "x"), S(R, 11, "w"));
    IN0(RET);
    IN(LABEL, S(LBL, 2, "xyzw")); IN(MAD, D(R, 10, "xyzw"), S(R, 10, "xyzw"), S(C, 20, "x"), S(V, 2, "xyzw")); IN0(RET);
    end();
    p = LD();
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) VIN[r][c] = 0.3f - 0.15f * (float)(r + c);
    VSRUN(p); psref_free(p);
}

int main(int argc, char **argv){
    if (argc > 1 && !strcmp(argv[1], "--fx")) return fx_mode(argc, argv);
    if (argc > 2 && !strcmp(argv[1], "--emit")) g_emit = argv[2];
    t_arith();
    t_flow();
    t_loops();
    t_pred();
    t_grad();
    t_tex();
    t_face_pos();
    t_limits();
    t_misc();
    t_corpus();
    printf("psref_unit: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
