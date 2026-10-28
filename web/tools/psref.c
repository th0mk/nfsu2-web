/* D3D9 reference interpreter for ps_2_x, ps_3_0 and vs_3_0: the interface is
 * psref.h; docs/web/psref.md lists the rules it follows, where each comes
 * from, and the choices it makes where Direct3D 9's documents leave room.
 *
 * Three passes at load: decode every token, check every instruction against
 * its model (opcode, operand kinds, register counts, modifiers, swizzles,
 * declarations), then the flow structure (blocks, functions, nesting depths
 * through the call graph, D3D9's gradient rule).  Running keeps an active
 * mask per lane (SIMT): if/else/endif, loops with per-lane breaks and calls
 * all run the quad in lock step, so dsx/dsy and the level of detail of an
 * implicit texld see their neighbours' values at the same point. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif
#include "psref.h"
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- D3D9 bytecode (d3d9types.h: D3DSHADER_INSTRUCTION_OPCODE_TYPE, D3DSHADER_PARAM_REGISTER_TYPE) ---- */
enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX,
    OP_SLT, OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3,
    OP_M3x2, OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN,
    OP_ABS, OP_NRM, OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC,
    OP_MOVA, OP_DEFB, OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL = 65, OP_TEX = 66, OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80, OP_DEF = 81,
    OP_CMP = 88, OP_BEM = 89, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92, OP_TEXLDD = 93, OP_SETP = 94,
    OP_TEXLDL = 95, OP_BREAKP = 96, OP_COUNT = 97
};
enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4, RT_ATTROUT = 5, RT_OUTPUT = 6,
    RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONST2 = 11, RT_CONST3 = 12,
    RT_CONST4 = 13, RT_CONSTBOOL = 14, RT_LOOP = 15, RT_TEMPF16 = 16, RT_MISC = 17, RT_LABEL = 18, RT_PRED = 19
};
enum { SM_NONE = 0, SM_NEG = 1, SM_ABS = 11, SM_ABSNEG = 12, SM_NOT = 13 };
enum { DM_SAT = 1, DM_PP = 2, DM_CENTROID = 4 };
enum { M_PS2X, M_PS3, M_VS3 };
enum { CMP_GT = 1, CMP_EQ, CMP_GE, CMP_LT, CMP_NE, CMP_LE };

#define B_P2 1u
#define B_P3 2u
#define B_V3 4u
#define B_PS (B_P2 | B_P3)
#define B_ALL 7u
/* what ps_2_x needs from D3DPSHADERCAPS2_0 */
#define F_STATIC 1u
#define F_DYN 2u
#define F_PRED 4u
#define F_GRAD 8u

typedef struct { const char *name; uint8_t models, nsrc, dst, feat; } OpInfo;
static const OpInfo OPS[OP_COUNT] = {
    [OP_NOP] = { "nop", B_ALL, 0, 0, 0 },          [OP_MOV] = { "mov", B_ALL, 1, 1, 0 },
    [OP_ADD] = { "add", B_ALL, 2, 1, 0 },          [OP_SUB] = { "sub", B_ALL, 2, 1, 0 },
    [OP_MAD] = { "mad", B_ALL, 3, 1, 0 },          [OP_MUL] = { "mul", B_ALL, 2, 1, 0 },
    [OP_RCP] = { "rcp", B_ALL, 1, 1, 0 },          [OP_RSQ] = { "rsq", B_ALL, 1, 1, 0 },
    [OP_DP3] = { "dp3", B_ALL, 2, 1, 0 },          [OP_DP4] = { "dp4", B_ALL, 2, 1, 0 },
    [OP_MIN] = { "min", B_ALL, 2, 1, 0 },          [OP_MAX] = { "max", B_ALL, 2, 1, 0 },
    [OP_SLT] = { "slt", B_V3, 2, 1, 0 },           [OP_SGE] = { "sge", B_V3, 2, 1, 0 },
    [OP_EXP] = { "exp", B_ALL, 1, 1, 0 },          [OP_LOG] = { "log", B_ALL, 1, 1, 0 },
    [OP_LIT] = { "lit", B_V3, 1, 1, 0 },           [OP_DST] = { "dst", B_V3, 2, 1, 0 },
    [OP_LRP] = { "lrp", B_ALL, 3, 1, 0 },          [OP_FRC] = { "frc", B_ALL, 1, 1, 0 },
    [OP_M4x4] = { "m4x4", B_ALL, 2, 1, 0 },        [OP_M4x3] = { "m4x3", B_ALL, 2, 1, 0 },
    [OP_M3x4] = { "m3x4", B_ALL, 2, 1, 0 },        [OP_M3x3] = { "m3x3", B_ALL, 2, 1, 0 },
    [OP_M3x2] = { "m3x2", B_ALL, 2, 1, 0 },        [OP_CALL] = { "call", B_ALL, 1, 0, F_STATIC },
    [OP_CALLNZ] = { "callnz", B_ALL, 2, 0, F_STATIC }, [OP_LOOP] = { "loop", B_P3 | B_V3, 2, 0, 0 },
    [OP_RET] = { "ret", B_ALL, 0, 0, F_STATIC },   [OP_ENDLOOP] = { "endloop", B_P3 | B_V3, 0, 0, 0 },
    [OP_LABEL] = { "label", B_ALL, 1, 0, F_STATIC }, [OP_DCL] = { "dcl", B_ALL, 0, 1, 0 },
    [OP_POW] = { "pow", B_ALL, 2, 1, 0 },          [OP_CRS] = { "crs", B_ALL, 2, 1, 0 },
    [OP_SGN] = { "sgn", B_V3, 3, 1, 0 },           [OP_ABS] = { "abs", B_ALL, 1, 1, 0 },
    [OP_NRM] = { "nrm", B_ALL, 1, 1, 0 },          [OP_SINCOS] = { "sincos", B_ALL, 1, 1, 0 },
    [OP_REP] = { "rep", B_ALL, 1, 0, F_STATIC },   [OP_ENDREP] = { "endrep", B_ALL, 0, 0, F_STATIC },
    [OP_IF] = { "if", B_ALL, 1, 0, 0 },            [OP_IFC] = { "if_comp", B_ALL, 2, 0, F_DYN },
    [OP_ELSE] = { "else", B_ALL, 0, 0, 0 },        [OP_ENDIF] = { "endif", B_ALL, 0, 0, 0 },
    [OP_BREAK] = { "break", B_ALL, 0, 0, F_STATIC }, [OP_BREAKC] = { "break_comp", B_ALL, 2, 0, F_DYN },
    [OP_MOVA] = { "mova", B_V3, 1, 1, 0 },         [OP_DEFB] = { "defb", B_ALL, 0, 1, 0 },
    [OP_DEFI] = { "defi", B_ALL, 0, 1, 0 },        [OP_TEXCOORD] = { "texcoord", 0, 0, 1, 0 },
    [OP_TEXKILL] = { "texkill", B_PS, 0, 1, 0 },   [OP_TEX] = { "texld", B_PS, 2, 1, 0 },
    [OP_EXPP] = { "expp", B_V3, 1, 1, 0 },         [OP_LOGP] = { "logp", B_V3, 1, 1, 0 },
    [OP_CND] = { "cnd", 0, 3, 1, 0 },              [OP_DEF] = { "def", B_ALL, 0, 1, 0 },
    [OP_CMP] = { "cmp", B_PS, 3, 1, 0 },           [OP_BEM] = { "bem", 0, 2, 1, 0 },
    [OP_DP2ADD] = { "dp2add", B_PS, 3, 1, 0 },     [OP_DSX] = { "dsx", B_PS, 1, 1, F_GRAD },
    [OP_DSY] = { "dsy", B_PS, 1, 1, F_GRAD },      [OP_TEXLDD] = { "texldd", B_PS, 4, 1, F_GRAD },
    [OP_SETP] = { "setp", B_ALL, 2, 1, F_PRED },   [OP_TEXLDL] = { "texldl", B_P3 | B_V3, 2, 1, 0 },
    [OP_BREAKP] = { "breakp", B_ALL, 1, 0, F_PRED },
};

const char *psref_opname(unsigned op){
    if (op < OP_COUNT && OPS[op].name) return OPS[op].name;
    return "?";
}

void psref_caps_sm3(PsrefCaps *c){
    memset(c, 0, sizeof *c);
    c->ps20_caps = PSREF_PS20_ARBITRARYSWIZZLE | PSREF_PS20_GRADIENTINSTRUCTIONS | PSREF_PS20_PREDICATION |
                   PSREF_PS20_NODEPENDENTREADLIMIT | PSREF_PS20_NOTEXINSTRUCTIONLIMIT;
    c->ps20_dynamic_depth = 24;
    c->ps20_num_temps = 32;
    c->ps20_static_depth = 4;
    c->max_vs_const = 256;
    c->max_executed = 65535;
    c->num_mrt = 4;
}

/* ---- the decoded program ---- */
typedef struct {
    uint8_t type, mod, mask, dmod, shift, rel, rtype, rcomp;
    uint8_t swz[4];
    uint16_t num, rnum;
} Opd;

typedef struct {
    uint16_t op;
    uint8_t ctrl, pred, has_dst, nsrc;
    uint8_t cw;          /* component-wise: source c is read for the components written */
    uint8_t need[4];     /* per source: the result components the instruction reads */
    Opd dst, prd, src[4];
    uint32_t imm[4];
    int match;           /* if -> else or endif, else -> endif, endif -> if; rep/loop <-> end */
    int tok;             /* token index */
} Ins;

#define MAXDCL 96
#define FRMAX 192

struct PsrefProg {
    uint32_t version;
    int model;
    PsrefCaps caps;
    Ins *ins;
    int n, main_end;
    size_t ndw;
    unsigned nconst;
    float (*def)[4];
    uint8_t *defd;
    int defi[16][4]; unsigned defim;
    int defb[16]; unsigned defbm;
    PsrefDcl dcl[MAXDCL]; int ndcl;
    uint8_t inm[16], tm[8], om[12], smp[16];
    uint8_t posm, face;
    int nlabel, *label_at;
    unsigned opcount[OP_COUNT];
};

uint32_t psref_version(const PsrefProg *p){ return p ? p->version : 0; }
size_t psref_ndw(const PsrefProg *p){ return p ? p->ndw : 0; }
unsigned psref_ninstr(const PsrefProg *p){ return p ? (unsigned)p->n : 0; }
unsigned psref_opcount(const PsrefProg *p, unsigned op){ return (p && op < OP_COUNT) ? p->opcount[op] : 0; }
int psref_ndcl(const PsrefProg *p){ return p ? p->ndcl : 0; }
int psref_dcl(const PsrefProg *p, int i, PsrefDcl *d){
    if (!p || i < 0 || i >= p->ndcl) return 0;
    *d = p->dcl[i];
    return 1;
}
void psref_free(PsrefProg *p){
    if (!p) return;
    free(p->ins); free(p->def); free(p->defd); free(p->label_at); free(p);
}

static void setf(char *err, size_t cap, const char *fmt, ...){
    if (!err || !cap) return;
    va_list a; va_start(a, fmt); vsnprintf(err, cap, fmt, a); va_end(a);
}

static const char *rname(int model, int type, int num){
    switch (type){
    case RT_TEMP: return "r";
    case RT_INPUT: return "v";
    case RT_CONST: return "c";
    case RT_ADDR: return model == M_VS3 ? "a" : "t";
    case RT_OUTPUT: return "o";
    case RT_CONSTINT: return "i";
    case RT_COLOROUT: return "oC";
    case RT_DEPTHOUT: return "oDepth";
    case RT_SAMPLER: return "s";
    case RT_CONSTBOOL: return "b";
    case RT_LOOP: return "aL";
    case RT_MISC: return num == 0 ? "vPos" : num == 1 ? "vFace" : "misc";
    case RT_LABEL: return "l";
    case RT_PRED: return "p";
    default: return "?";
    }
}
static int rnamed(int type){ return type != RT_DEPTHOUT && type != RT_LOOP && type != RT_MISC; }
static void comps(char *b, unsigned m){
    int k = 0;
    for (int c = 0; c < 4; c++) if (m >> c & 1u) b[k++] = "xyzw"[c];
    b[k] = 0;
}

/* ---- load-time errors ---- */
typedef struct { PsrefProg *p; char *err; size_t cap; int q; } L;
static int lerr(L *l, const char *fmt, ...){
    char m[256];
    va_list a; va_start(a, fmt); vsnprintf(m, sizeof m, fmt, a); va_end(a);
    if (l->q >= 0 && l->q < l->p->n)
        setf(l->err, l->cap, "psref: instruction %d (%s, token %d): %s", l->q, psref_opname(l->p->ins[l->q].op),
             l->p->ins[l->q].tok, m);
    else setf(l->err, l->cap, "psref: %s", m);
    return 0;
}

/* ---- pass 1: decode ---- */
static int param(const uint32_t *tok, size_t end, size_t *i, Opd *o, int isdst){
    if (*i >= end) return 0;
    uint32_t t = tok[(*i)++];
    memset(o, 0, sizeof *o);
    if (!(t >> 31) || (t & 0xC000u)) return -1;           /* bit 31 set, bits 14-15 reserved */
    o->num = (uint16_t)(t & 0x7FFu);
    o->type = (uint8_t)(((t >> 28) & 7u) | ((t >> 8) & 0x18u));
    o->rel = (uint8_t)((t >> 13) & 1u);
    if (isdst){
        o->mask = (uint8_t)((t >> 16) & 0xFu);
        o->dmod = (uint8_t)((t >> 20) & 0xFu);
        o->shift = (uint8_t)((t >> 24) & 0xFu);
        for (int c = 0; c < 4; c++) o->swz[c] = (uint8_t)c;
    } else {
        for (int c = 0; c < 4; c++) o->swz[c] = (uint8_t)((t >> (16 + 2 * c)) & 3u);
        o->mod = (uint8_t)((t >> 24) & 0xFu);
    }
    if (o->rel){
        if (*i >= end) return 0;
        uint32_t r = tok[(*i)++];
        if (!(r >> 31)) return -1;
        o->rtype = (uint8_t)(((r >> 28) & 7u) | ((r >> 8) & 0x18u));
        o->rnum = (uint16_t)(r & 0x7FFu);
        unsigned sw = (r >> 16) & 0xFFu;
        o->rcomp = (uint8_t)(sw & 3u);
        if (sw != (unsigned)o->rcomp * 0x55u) return -2;   /* the address component: a replicate swizzle */
        if ((r >> 24) & 0xFu) return -2;
        if (!((o->rtype == RT_ADDR || o->rtype == RT_LOOP) && o->rnum == 0)) return -2;
    }
    return 1;
}

static int decode(L *l, const uint32_t *tok, size_t ndw){
    PsrefProg *p = l->p;
    size_t cap = 64, i = 1;
    p->ins = malloc(cap * sizeof *p->ins);
    if (!p->ins) return lerr(l, "out of memory");
    for (;;){
        if (i >= ndw) return lerr(l, "no end token in %zu dwords", ndw);
        uint32_t t = tok[i];
        unsigned op = t & 0xFFFFu;
        if (op == 0xFFFFu){ if (t != 0x0000FFFFu) return lerr(l, "bad end token 0x%08x", t); p->ndw = i + 1; break; }
        if (op == 0xFFFEu){
            size_t len = (t >> 16) & 0x7FFFu;
            if (t >> 31) return lerr(l, "bad comment token 0x%08x at %zu", t, i);
            if (i + 1 + len > ndw) return lerr(l, "comment at token %zu runs past the program", i);
            i += 1 + len; continue;
        }
        if ((size_t)p->n == cap){
            cap *= 2;
            Ins *ni = realloc(p->ins, cap * sizeof *ni);
            if (!ni) return lerr(l, "out of memory");
            p->ins = ni;
        }
        Ins *x = &p->ins[p->n];
        memset(x, 0, sizeof *x);
        x->tok = (int)i; x->op = (uint16_t)op; x->match = -1;
        l->q = p->n; p->n++;
        size_t len = (t >> 24) & 0xFu, b = i + 1, end = b + len;
        if (t & 0xA0000000u) return lerr(l, "instruction token 0x%08x: bit 31 or 29 set", t);
        if (t & 0x40000000u) return lerr(l, "co-issue is ps_1_x only");
        if (end > ndw) return lerr(l, "runs past the program");
        if (op >= OP_COUNT || !OPS[op].name) return lerr(l, "opcode %u is not in ps_2_x, ps_3_0 or vs_3_0", op);
        x->ctrl = (uint8_t)((t >> 16) & 0xFFu);
        x->pred = (uint8_t)((t >> 28) & 1u);
        p->opcount[op]++;
        size_t j = b;
        int r;
        if (op == OP_DCL){
            if (len != 2) return lerr(l, "dcl with %zu operand tokens (2)", len);
            x->imm[0] = tok[j++];
            if ((r = param(tok, end, &j, &x->dst, 1)) != 1) return lerr(l, "bad dcl register token");
            x->has_dst = 1;
        } else if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB){
            size_t want = op == OP_DEFB ? 2 : 5;
            if (len != want) return lerr(l, "%zu operand tokens (%zu)", len, want);
            if ((r = param(tok, end, &j, &x->dst, 1)) != 1 || x->dst.rel) return lerr(l, "bad register token");
            x->has_dst = 1;
            for (size_t k = 0; k + 1 < want; k++) x->imm[k] = tok[j++];
        } else {
            if (OPS[op].dst){
                if ((r = param(tok, end, &j, &x->dst, 1)) != 1)
                    return lerr(l, r == -2 ? "bad relative address token" : "bad destination token");
                x->has_dst = 1;
            }
            if (x->pred){
                if (!x->has_dst) return lerr(l, "a predicate on an instruction without a destination");
                if ((r = param(tok, end, &j, &x->prd, 0)) != 1 || x->prd.rel)
                    return lerr(l, "bad predicate token");
            }
            while (j < end){
                if (x->nsrc == 4) return lerr(l, "more than 4 source operands");
                if ((r = param(tok, end, &j, &x->src[x->nsrc], 0)) != 1)
                    return lerr(l, r == -2 ? "bad relative address token" : "bad source token");
                x->nsrc++;
            }
        }
        if (j != end) return lerr(l, "operands do not fill the instruction's %zu tokens", len);
        i = end;
    }
    l->q = -1;
    return 1;
}

/* ---- pass 2: each instruction against its model ---- */
static int reg_count(const PsrefProg *p, int type){
    int m = p->model;
    switch (type){
    case RT_TEMP: return m == M_PS2X ? p->caps.ps20_num_temps : 32;
    case RT_INPUT: return m == M_PS2X ? 2 : m == M_PS3 ? 10 : 16;
    case RT_CONST: return (int)p->nconst;
    case RT_ADDR: return m == M_PS2X ? 8 : m == M_VS3 ? 1 : 0;
    case RT_OUTPUT: return m == M_VS3 ? 12 : 0;
    case RT_CONSTINT: case RT_CONSTBOOL: return 16;
    case RT_COLOROUT: return m == M_VS3 ? 0 : p->caps.num_mrt;
    case RT_DEPTHOUT: return m == M_VS3 ? 0 : 1;
    case RT_SAMPLER: return m == M_VS3 ? 4 : 16;
    case RT_LOOP: return m == M_PS2X ? 0 : 1;
    case RT_MISC: return m == M_PS3 ? 2 : 0;
    case RT_LABEL: return m == M_PS2X ? 16 : 2048;
    case RT_PRED: return (m != M_PS2X || (p->caps.ps20_caps & PSREF_PS20_PREDICATION)) ? 1 : 0;
    default: return 0;
    }
}
static int in_range(L *l, const Opd *o){
    int n = reg_count(l->p, o->type);
    if (n <= 0) return lerr(l, "register type %s is not in this model", rname(l->p->model, o->type, o->num));
    if (o->num >= n){
        if (rnamed(o->type)) return lerr(l, "%s%d: the model has %s0..%s%d", rname(l->p->model, o->type, o->num),
                                         o->num, rname(l->p->model, o->type, 0), rname(l->p->model, o->type, 0), n - 1);
        return lerr(l, "%s register %d is not in this model", rname(l->p->model, o->type, o->num), o->num);
    }
    return 1;
}
static int is_replicate(const Opd *o){ return o->swz[0] == o->swz[1] && o->swz[1] == o->swz[2] && o->swz[2] == o->swz[3]; }
static int is_identity(const Opd *o){ return o->swz[0] == 0 && o->swz[1] == 1 && o->swz[2] == 2 && o->swz[3] == 3; }
/* ps_2_0's swizzles: .xyzw, a replicate, .yzxw, .zxyw, .wzyx (ps_2_x without D3DPS20CAPS_ARBITRARYSWIZZLE) */
static int ps20_swizzle(const Opd *o){
    static const uint8_t ok[3][4] = { { 1, 2, 0, 3 }, { 2, 0, 1, 3 }, { 3, 2, 1, 0 } };
    if (is_identity(o) || is_replicate(o)) return 1;
    for (int k = 0; k < 3; k++) if (!memcmp(o->swz, ok[k], 4)) return 1;
    return 0;
}

/* a value source of an arithmetic, texture coordinate or comparison operand */
static int chk_val(L *l, const Opd *o, int face_ok){
    PsrefProg *p = l->p;
    int m = p->model;
    switch (o->type){
    case RT_TEMP: case RT_INPUT: case RT_CONST: case RT_ADDR: case RT_LOOP: break;
    case RT_MISC:
        if (m == M_PS3 && o->num == 1 && !face_ok)
            return lerr(l, "vFace is only a condition of setp, if_comp and break_comp");
        break;
    default: return lerr(l, "%s%d cannot be a value source", rname(m, o->type, o->num), o->num);
    }
    if (!in_range(l, o)) return 0;
    if (o->mod != SM_NONE && o->mod != SM_NEG){
        if ((o->mod == SM_ABS || o->mod == SM_ABSNEG) && m != M_PS2X) {}
        else return lerr(l, "source modifier %d is not allowed in this model", o->mod);
    }
    if (o->rel){
        int ok = (m == M_PS3 && o->type == RT_INPUT && o->rtype == RT_LOOP) ||
                 (m == M_VS3 && (o->type == RT_INPUT || o->type == RT_CONST));
        if (!ok) return lerr(l, "relative addressing of %s is not in this model", rname(m, o->type, o->num));
        if (o->rtype == RT_ADDR && m != M_VS3) return lerr(l, "a0 is vs only");
    }
    if (m == M_PS2X && !(p->caps.ps20_caps & PSREF_PS20_ARBITRARYSWIZZLE) && !ps20_swizzle(o))
        return lerr(l, "arbitrary swizzle without D3DPS20CAPS_ARBITRARYSWIZZLE");
    if (o->type == RT_ADDR && m == M_VS3 && o->rel) return lerr(l, "relative a0");
    return 1;
}
/* b# or p0 as the condition of if, callnz; p0 for breakp */
static int chk_cond(L *l, const Opd *o, int bool_ok){
    if (o->type == RT_CONSTBOOL && bool_ok){
        if (!in_range(l, o)) return 0;
    } else if (o->type == RT_PRED){
        if (!in_range(l, o)) return 0;
        if (!is_replicate(o)) return lerr(l, "the predicate needs a replicate swizzle");
    } else return lerr(l, "the condition must be %sp0", bool_ok ? "b# or " : "");
    if (o->mod != SM_NONE && o->mod != SM_NOT) return lerr(l, "modifier %d on a condition", o->mod);
    if (o->rel) return lerr(l, "relative condition");
    return 1;
}
static int chk_plain(L *l, const Opd *o, int type){
    if (o->type != type) return lerr(l, "operand must be %s#", rname(l->p->model, type, 0));
    if (!in_range(l, o)) return 0;
    if (o->mod || o->rel) return lerr(l, "modifier or relative address on %s", rname(l->p->model, type, 0));
    return 1;
}

/* the result components instruction x reads from source si (bit c = component c after the swizzle) */
static unsigned need_of(const PsrefProg *p, const Ins *x, int si){
    unsigned m = x->has_dst ? x->dst.mask : 0;
    switch (x->op){
    case OP_MOV: case OP_ADD: case OP_SUB: case OP_MUL: case OP_MAD: case OP_MIN: case OP_MAX: case OP_SLT:
    case OP_SGE: case OP_FRC: case OP_ABS: case OP_LRP: case OP_CMP: case OP_DSX: case OP_DSY: case OP_SETP:
    case OP_MOVA:
        return m;
    case OP_SGN: return si == 0 ? m : 0;
    case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_EXPP: case OP_LOGP: case OP_POW: return 1;
    case OP_SINCOS: return si == 0 ? 1 : 0;
    case OP_DP3: return 7;
    case OP_DP4: return 15;
    case OP_DP2ADD: return si == 2 ? 1 : 3;
    case OP_NRM: return 7 | m;
    case OP_CRS: return ((m & 1) ? 6u : 0) | ((m & 2) ? 5u : 0) | ((m & 4) ? 3u : 0);
    case OP_LIT: return ((m & 2) ? 1u : 0) | ((m & 4) ? 11u : 0);
    case OP_DST: return si == 0 ? (((m & 2) ? 2u : 0) | ((m & 4) ? 4u : 0)) : (((m & 2) ? 2u : 0) | ((m & 8) ? 8u : 0));
    case OP_M4x4: case OP_M4x3: return si == 0 ? 15 : 0;
    case OP_M3x4: case OP_M3x3: case OP_M3x2: return si == 0 ? 7 : 0;
    case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: {
        int ty = x->nsrc > 1 && x->src[1].num < 16 ? p->smp[x->src[1].num] : 2;
        unsigned c = ty == 2 ? 3u : 7u;
        if (si == 0) return c | ((x->op == OP_TEXLDL || (x->op == OP_TEX && x->ctrl)) ? 8u : 0);
        return si >= 2 ? c : 0;
    }
    case OP_IFC: case OP_BREAKC: return 1;
    default: return 0;
    }
}
static int is_cw(unsigned op){
    switch (op){
    case OP_MOV: case OP_ADD: case OP_SUB: case OP_MUL: case OP_MAD: case OP_MIN: case OP_MAX: case OP_SLT:
    case OP_SGE: case OP_FRC: case OP_ABS: case OP_LRP: case OP_CMP: case OP_SETP: case OP_MOVA: case OP_SGN:
        return 1;
    default: return 0;
    }
}

/* a read of an input, a texture coordinate register, vPos or vFace must stay inside its declaration */
static int chk_decl_read(L *l, const Opd *o, unsigned need){
    PsrefProg *p = l->p;
    unsigned rc = 0;
    for (int c = 0; c < 4; c++) if (need >> c & 1u) rc |= 1u << o->swz[c];
    unsigned dm;
    char b[8];
    switch (o->type){
    case RT_INPUT:
        if (o->rel) return 1;                                  /* checked when it runs */
        dm = p->inm[o->num];
        if (!dm) return lerr(l, "v%d is not declared", o->num);
        if (p->model == M_PS3 && (rc & ~dm)){ comps(b, rc & ~dm); return lerr(l, "reads v%d.%s outside its declaration", o->num, b); }
        return 1;
    case RT_ADDR:
        if (p->model != M_PS2X) return 1;
        dm = p->tm[o->num];
        if (!dm) return lerr(l, "t%d is not declared", o->num);
        if (rc & ~dm){ comps(b, rc & ~dm); return lerr(l, "reads t%d.%s outside its declaration", o->num, b); }
        return 1;
    case RT_MISC:
        dm = o->num == 0 ? p->posm : (p->face ? 0xFu : 0);
        if (!dm) return lerr(l, "%s is not declared", rname(p->model, o->type, o->num));
        if (rc & ~dm){ comps(b, rc & ~dm); return lerr(l, "reads vPos.%s outside its declaration", b); }
        return 1;
    default: return 1;
    }
}

static int chk_dst(L *l, const Ins *x){
    PsrefProg *p = l->p;
    const Opd *d = &x->dst;
    int m = p->model;
    if (!d->mask) return lerr(l, "empty write mask");
    if (d->shift) return lerr(l, "a result shift is ps_1_x only");
    if (d->dmod & ~(unsigned)(DM_SAT | DM_PP)) return lerr(l, "result modifier 0x%x", d->dmod);
    switch (d->type){
    case RT_TEMP: break;
    case RT_COLOROUT: case RT_DEPTHOUT:
        if (m == M_VS3) return lerr(l, "%s in a vertex shader", rname(m, d->type, d->num));
        if (d->type == RT_DEPTHOUT && d->mask != 1 && d->mask != 0xF) return lerr(l, "oDepth takes .x");
        break;
    case RT_OUTPUT:
        if (m != M_VS3) return lerr(l, "o# in a pixel shader");
        break;
    case RT_PRED:
        if (x->op != OP_SETP) return lerr(l, "only setp writes p0");
        break;
    case RT_ADDR:
        if (m != M_VS3 || x->op != OP_MOVA) return lerr(l, "only mova writes a0");
        break;
    default: return lerr(l, "%s cannot be a destination", rname(m, d->type, d->num));
    }
    if (x->op == OP_SETP && d->type != RT_PRED) return lerr(l, "setp writes p0");
    if (x->op == OP_MOVA && d->type != RT_ADDR) return lerr(l, "mova writes a0");
    if (!in_range(l, d)) return 0;
    if (d->rel && !(m == M_VS3 && d->type == RT_OUTPUT && d->rtype == RT_LOOP))
        return lerr(l, "relative destination (vs_3_0 o[aL] only)");
    if ((d->dmod & DM_SAT) && (d->type == RT_PRED || d->type == RT_ADDR)) return lerr(l, "_sat on %s", rname(m, d->type, 0));
    if (d->type == RT_OUTPUT && !d->rel){
        if (!p->om[d->num]) return lerr(l, "o%d is not declared", d->num);
        if (d->mask & ~p->om[d->num]){ char b[8]; comps(b, d->mask & ~p->om[d->num]); return lerr(l, "writes o%d.%s outside its declaration", d->num, b); }
    }
    return 1;
}

static int chk_dcl(L *l, Ins *x){
    PsrefProg *p = l->p;
    uint32_t u = x->imm[0];
    const Opd *d = &x->dst;
    int m = p->model;
    if (!(u >> 31)) return lerr(l, "dcl usage token without bit 31");
    int usage = (int)(u & 0x1Fu), index = (int)((u >> 16) & 0xFu), st = (int)((u >> 27) & 0xFu);
    if (d->shift || d->rel || (d->dmod & ~(unsigned)(DM_PP | DM_CENTROID))) return lerr(l, "bad dcl register token");
    if (!in_range(l, d)) return 0;
    if (p->ndcl == MAXDCL) return lerr(l, "more than %d declarations", MAXDCL);
    PsrefDcl *D = &p->dcl[p->ndcl];
    D->regtype = d->type; D->reg = d->num; D->usage = usage; D->index = index; D->mask = d->mask; D->smptype = 0;
    uint8_t *slot = 0;
    int packable = 0;
    switch (d->type){
    case RT_SAMPLER:
        if (st < 2 || st > 4) return lerr(l, "sampler type %d (2D 2, cube 3, volume 4)", st);
        if (p->smp[d->num]) return lerr(l, "s%d declared twice", d->num);
        p->smp[d->num] = (uint8_t)st; D->smptype = st; D->mask = 0xF;
        p->ndcl++;
        return 1;
    case RT_INPUT:
        if (m != M_PS2X && usage > 13) return lerr(l, "usage %d", usage);
        slot = &p->inm[d->num]; packable = m == M_PS3;
        break;
    case RT_ADDR:
        if (m != M_PS2X) return lerr(l, "dcl of %s", rname(m, d->type, d->num));
        slot = &p->tm[d->num];
        break;
    case RT_OUTPUT:
        if (usage > 13) return lerr(l, "usage %d", usage);
        slot = &p->om[d->num]; packable = 1;
        break;
    case RT_MISC:
        if (d->num == 0){
            if (d->mask != 1 && d->mask != 2 && d->mask != 3) return lerr(l, "vPos takes .x, .y or .xy");
            slot = &p->posm;
        } else {
            if (p->face) return lerr(l, "vFace declared twice");
            p->face = 1; D->mask = 0xF;
            p->ndcl++;
            return 1;
        }
        break;
    default: return lerr(l, "dcl of %s", rname(m, d->type, d->num));
    }
    if (!d->mask) return lerr(l, "empty dcl mask");
    if (*slot && (!packable || (*slot & d->mask))) return lerr(l, "%s%d declared twice", rname(m, d->type, d->num), d->num);
    *slot |= d->mask;
    p->ndcl++;
    return 1;
}

static int chk_def(L *l, Ins *x){
    PsrefProg *p = l->p;
    const Opd *d = &x->dst;
    int want = x->op == OP_DEF ? RT_CONST : x->op == OP_DEFI ? RT_CONSTINT : RT_CONSTBOOL;
    if (d->type != want || d->mask != 0xF || d->dmod || d->shift) return lerr(l, "bad %s register", psref_opname(x->op));
    if (!in_range(l, d)) return 0;
    if (x->op == OP_DEF){
        if (p->defd[d->num]) return lerr(l, "c%d defined twice", d->num);
        p->defd[d->num] = 1;
        for (int c = 0; c < 4; c++) memcpy(&p->def[d->num][c], &x->imm[c], 4);
    } else if (x->op == OP_DEFI){
        if (p->defim >> d->num & 1u) return lerr(l, "i%d defined twice", d->num);
        p->defim |= 1u << d->num;
        for (int c = 0; c < 4; c++) p->defi[d->num][c] = (int)x->imm[c];
    } else {
        if (p->defbm >> d->num & 1u) return lerr(l, "b%d defined twice", d->num);
        p->defbm |= 1u << d->num;
        p->defb[d->num] = x->imm[0] != 0;
    }
    return 1;
}

static int chk_ins(L *l, Ins *x){
    PsrefProg *p = l->p;
    int m = p->model;
    unsigned mb = m == M_PS2X ? B_P2 : m == M_PS3 ? B_P3 : B_V3;
    const OpInfo *I = &OPS[x->op];
    if (!(I->models & mb)) return lerr(l, "%s is not in this model", I->name);
    if (m == M_PS2X){
        unsigned f = I->feat;
        if (x->pred) f |= F_PRED;
        if (x->op == OP_IF) f |= (x->nsrc == 1 && x->src[0].type == RT_PRED) ? F_PRED | F_DYN : F_STATIC;
        if (x->op == OP_CALLNZ && x->nsrc == 2 && x->src[1].type == RT_PRED) f |= F_PRED | F_DYN;
        if (x->op == OP_BREAKP) f |= F_DYN;
        if ((f & F_STATIC) && p->caps.ps20_static_depth <= 0) return lerr(l, "%s needs StaticFlowControlDepth > 0", I->name);
        if ((f & F_DYN) && p->caps.ps20_dynamic_depth <= 0) return lerr(l, "%s needs DynamicFlowControlDepth > 0", I->name);
        if ((f & F_PRED) && !(p->caps.ps20_caps & PSREF_PS20_PREDICATION)) return lerr(l, "%s needs D3DPS20CAPS_PREDICATION", x->pred ? "predication" : I->name);
        if ((f & F_GRAD) && !(p->caps.ps20_caps & PSREF_PS20_GRADIENTINSTRUCTIONS)) return lerr(l, "%s needs D3DPS20CAPS_GRADIENTINSTRUCTIONS", I->name);
    }
    if (x->op == OP_DCL || x->op == OP_DEF || x->op == OP_DEFI || x->op == OP_DEFB){
        if (x->pred) return lerr(l, "predicated %s", I->name);
        return 1;                                             /* done in pass 1b */
    }
    int want = I->nsrc;
    if (x->op == OP_SINCOS) want = m == M_PS2X ? 3 : 1;
    if (x->nsrc != want) return lerr(l, "%d source operands (%d)", x->nsrc, want);
    if (x->op == OP_TEX && x->ctrl > 2) return lerr(l, "texld control %d", x->ctrl);
    if ((x->op == OP_IFC || x->op == OP_BREAKC || x->op == OP_SETP) && (x->ctrl < CMP_GT || x->ctrl > CMP_LE))
        return lerr(l, "comparison %d", x->ctrl);
    if (x->op != OP_TEX && x->op != OP_IFC && x->op != OP_BREAKC && x->op != OP_SETP && x->ctrl)
        return lerr(l, "instruction controls 0x%x", x->ctrl);
    if (x->pred){                               /* (p0.swizzle) or (!p0.swizzle): any swizzle, one per component */
        const Opd *pr = &x->prd;
        if (pr->type != RT_PRED || !in_range(l, pr) || pr->rel || (pr->mod != SM_NONE && pr->mod != SM_NOT))
            return lerr(l, "bad predicate operand");
        if (x->op == OP_TEXKILL) return lerr(l, "a predicated texkill");
        if (x->op == OP_SINCOS && m == M_PS2X && !is_replicate(pr)) return lerr(l, "ps_2_x sincos takes a replicate predicate");
    }
    if (x->has_dst && x->op != OP_TEXKILL && !chk_dst(l, x)) return 0;
    for (int s = 0; s < x->nsrc; s++) x->need[s] = (uint8_t)need_of(p, x, s);
    x->cw = (uint8_t)is_cw(x->op);

    switch (x->op){
    case OP_CALL: case OP_LABEL:
        return chk_plain(l, &x->src[0], RT_LABEL);
    case OP_CALLNZ:
        return chk_plain(l, &x->src[0], RT_LABEL) && chk_cond(l, &x->src[1], 1);
    case OP_IF:
        return chk_cond(l, &x->src[0], 1);
    case OP_BREAKP:
        return chk_cond(l, &x->src[0], 0);
    case OP_REP:
        return chk_plain(l, &x->src[0], RT_CONSTINT);
    case OP_LOOP:
        return chk_plain(l, &x->src[0], RT_LOOP) && chk_plain(l, &x->src[1], RT_CONSTINT);
    case OP_RET: case OP_ENDLOOP: case OP_ENDREP: case OP_ELSE: case OP_ENDIF: case OP_BREAK: case OP_NOP:
        return 1;
    case OP_IFC: case OP_BREAKC:
        for (int s = 0; s < 2; s++){
            if (!chk_val(l, &x->src[s], 1) || !chk_decl_read(l, &x->src[s], 1)) return 0;
            if (!is_replicate(&x->src[s])) return lerr(l, "the comparison needs replicate swizzles");
        }
        return 1;
    case OP_TEXKILL: {
        const Opd *d = &x->dst;
        if (d->dmod || d->shift || d->rel || !d->mask) return lerr(l, "bad texkill register");
        if (!(d->type == RT_TEMP || (m == M_PS2X && d->type == RT_ADDR) || (m == M_PS3 && d->type == RT_INPUT)))
            return lerr(l, "texkill of %s", rname(m, d->type, d->num));
        if (!in_range(l, d)) return 0;
        Opd s = *d; s.mod = 0;
        return chk_decl_read(l, &s, d->mask);
    }
    case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: {
        const Opd *sm = &x->src[1];
        if (!chk_plain(l, sm, RT_SAMPLER)) return 0;
        if (!p->smp[sm->num]) return lerr(l, "s%d is not declared", sm->num);
        if (m == M_PS2X && !is_identity(sm)) return lerr(l, "a sampler swizzle is ps_3_0 only");
        if (m != M_VS3 && x->dst.type != RT_TEMP) return lerr(l, "a texture load writes a temporary");
        if (m == M_PS2X && x->op == OP_TEX){
            if (x->dst.mask != 0xF) return lerr(l, "ps_2_x texld writes .xyzw");
            const Opd *c = &x->src[0];
            if (c->type != RT_TEMP && c->type != RT_ADDR) return lerr(l, "ps_2_x texld reads t# or r#");
            if (c->mod || !is_identity(c)) return lerr(l, "ps_2_x texld coordinate without modifier or swizzle");
        }
        for (int s = 0; s < x->nsrc; s++){
            if (s == 1) continue;
            if (!chk_val(l, &x->src[s], 0) || !chk_decl_read(l, &x->src[s], x->need[s])) return 0;
            if (x->src[s].type == RT_MISC && x->src[s].num == 1) return lerr(l, "vFace as a coordinate");
        }
        return 1;
    }
    default: break;
    }
    /* arithmetic */
    int nval = x->nsrc;
    if (x->op == OP_SINCOS && m == M_PS2X){
        nval = 1;
        for (int s = 1; s < 3; s++) if (!chk_plain(l, &x->src[s], RT_CONST)) return 0;
        if (x->src[1].num == x->src[2].num) return lerr(l, "sincos needs two different constants");
    }
    if (x->op == OP_SGN){
        nval = 1;
        for (int s = 1; s < 3; s++) if (!chk_plain(l, &x->src[s], RT_TEMP)) return 0;
        if (x->src[1].num == x->src[2].num) return lerr(l, "sgn needs two different temporaries");
    }
    int ismat = x->op >= OP_M4x4 && x->op <= OP_M3x2;
    for (int s = 0; s < nval; s++){
        const Opd *o = &x->src[s];
        if (!chk_val(l, o, x->op == OP_SETP)) return 0;
        if (ismat && s == 1){
            if (o->mod || !is_identity(o)) return lerr(l, "the matrix operand takes no modifier or swizzle");
            if (o->type != RT_CONST && o->type != RT_TEMP && o->type != RT_INPUT) return lerr(l, "the matrix is c#, r# or v#");
            int rows = (x->op == OP_M4x4 || x->op == OP_M3x4) ? 4 : (x->op == OP_M3x2) ? 2 : 3;
            if (!o->rel && o->num + rows > reg_count(p, o->type)) return lerr(l, "the matrix runs past %s%d", rname(m, o->type, 0), reg_count(p, o->type) - 1);
            for (int r = 0; r < rows && !o->rel; r++){
                Opd row = *o; row.num = (uint16_t)(o->num + r);
                if (!chk_decl_read(l, &row, (x->op == OP_M4x4 || x->op == OP_M4x3) ? 15 : 7)) return 0;
            }
            continue;
        }
        if (!chk_decl_read(l, o, x->need[s])) return 0;
    }
    switch (x->op){
    case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_EXPP: case OP_LOGP: case OP_POW: case OP_SINCOS:
        for (int s = 0; s < (x->op == OP_POW ? 2 : 1); s++)
            if (!is_replicate(&x->src[s])) return lerr(l, "a scalar source needs a replicate swizzle");
        break;
    case OP_DP2ADD:
        if (!is_replicate(&x->src[2])) return lerr(l, "dp2add's third source needs a replicate swizzle");
        break;
    default: break;
    }
    if (x->op == OP_SINCOS){
        if (x->dst.type != RT_TEMP || (x->dst.mask != 1 && x->dst.mask != 2 && x->dst.mask != 3))
            return lerr(l, "sincos writes r#.x, .y or .xy");
    }
    if (x->op == OP_NRM || x->op == OP_CRS){
        if (x->dst.type != RT_TEMP) return lerr(l, "%s writes a temporary", I->name);
        for (int s = 0; s < x->nsrc; s++)
            if (x->src[s].type == RT_TEMP && x->src[s].num == x->dst.num) return lerr(l, "%s: destination and source are one register", I->name);
        if (x->op == OP_CRS && (x->dst.mask & 8)) return lerr(l, "crs writes a subset of .xyz");
    }
    if (ismat){
        unsigned want_m = (x->op == OP_M4x4 || x->op == OP_M3x4) ? 0xFu : x->op == OP_M3x2 ? 3u : 7u;
        if (x->dst.mask != want_m) return lerr(l, "%s writes .%s", I->name, want_m == 0xF ? "xyzw" : want_m == 7 ? "xyz" : "xy");
    }
    if (x->op == OP_SGN)
        for (int s = 1; s < 3; s++) if (x->dst.type == RT_TEMP && x->src[s].num == x->dst.num) return lerr(l, "sgn's scratch registers must differ from the destination");
    return 1;
}

/* ---- pass 3: blocks, functions, nesting through calls, the gradient rule ---- */
typedef struct { int loop, dyn, stat, call; } Depth;
typedef struct { int label; Depth at; } Site;

static int link(L *l){
    PsrefProg *p = l->p;
    int n = p->n;
    for (int q = 0; q < n; q++) p->ins[q].match = -1;
    int st[64], ifq[64], sp = 0;              /* open blocks; an else's if */
    int fn_of_main = 1;                        /* still in main */
    int fn_open = 1;                           /* a function body is open (main or a subroutine) */
    p->main_end = n;
    for (int q = 0; q < n; q++){
        Ins *x = &p->ins[q];
        l->q = q;
        if (!fn_open){
            if (x->op == OP_LABEL) { fn_open = 1; fn_of_main = 0; }
            else return lerr(l, "outside main and every subroutine");
            int lb = x->src[0].num;
            if (p->label_at[lb] >= 0) return lerr(l, "l%d defined twice", lb);
            p->label_at[lb] = q;
            continue;
        }
        switch (x->op){
        case OP_LABEL:
            if (sp) return lerr(l, "a label inside a block");
            if (fn_of_main){ p->main_end = q; fn_of_main = 0; }
            else return lerr(l, "a label inside a subroutine (no ret before it)");
            if (p->label_at[x->src[0].num] >= 0) return lerr(l, "l%d defined twice", x->src[0].num);
            p->label_at[x->src[0].num] = q;
            break;
        case OP_RET:
            if (sp) return lerr(l, "ret inside a block (the reference takes ret at a function's end only)");
            if (fn_of_main) p->main_end = q;
            fn_open = 0;
            break;
        case OP_IF: case OP_IFC: case OP_REP: case OP_LOOP:
            if (sp == 64) return lerr(l, "blocks nested deeper than 64");
            ifq[sp] = q; st[sp++] = q;
            break;
        case OP_ELSE:                            /* if -> else; the open block becomes the else */
            if (!sp || (p->ins[st[sp - 1]].op != OP_IF && p->ins[st[sp - 1]].op != OP_IFC))
                return lerr(l, "else without its if");
            p->ins[st[sp - 1]].match = q;
            st[sp - 1] = q;
            break;
        case OP_ENDIF: {                         /* if or else -> endif; endif -> if */
            if (!sp) return lerr(l, "endif without if");
            Ins *o = &p->ins[st[sp - 1]];
            if (o->op != OP_ELSE && o->op != OP_IF && o->op != OP_IFC) return lerr(l, "endif closes a %s", psref_opname(o->op));
            o->match = q;
            x->match = ifq[sp - 1];
            sp--;
            break; }
        case OP_ENDREP: case OP_ENDLOOP: {
            if (!sp) return lerr(l, "%s without its start", psref_opname(x->op));
            Ins *o = &p->ins[st[sp - 1]];
            if ((x->op == OP_ENDREP) != (o->op == OP_REP) || (o->op != OP_REP && o->op != OP_LOOP))
                return lerr(l, "%s closes a %s", psref_opname(x->op), psref_opname(o->op));
            o->match = q; x->match = st[sp - 1];
            sp--;
            break; }
        case OP_BREAK: case OP_BREAKC: case OP_BREAKP: {
            int in_loop = 0;
            for (int k = 0; k < sp; k++) if (p->ins[st[k]].op == OP_REP || p->ins[st[k]].op == OP_LOOP) in_loop = 1;
            if (!in_loop) return lerr(l, "break outside a loop");
            break; }
        default: break;
        }
        /* aL: only inside a loop block of main (a subroutine's aL is checked when it runs) */
        int uses_al = 0;
        for (int s = 0; s < x->nsrc; s++) if ((x->src[s].type == RT_LOOP && x->op != OP_LOOP) || (x->src[s].rel && x->src[s].rtype == RT_LOOP)) uses_al = 1;
        if (x->has_dst && x->dst.rel && x->dst.rtype == RT_LOOP) uses_al = 1;
        if (uses_al && fn_of_main){
            int in_loop = 0;
            for (int k = 0; k < sp; k++) if (p->ins[st[k]].op == OP_LOOP) in_loop = 1;
            if (!in_loop) return lerr(l, "aL outside a loop block");
        }
    }
    l->q = -1;
    if (sp){ l->q = st[sp - 1]; return lerr(l, "block not closed"); }
    if (!fn_of_main && fn_open) return lerr(l, "the last subroutine has no ret");

    /* calls: every target defined; depths per function, then through the call graph */
    for (int q = 0; q < n; q++){
        Ins *x = &p->ins[q];
        if ((x->op == OP_CALL || x->op == OP_CALLNZ) && p->label_at[x->src[0].num] < 0){
            l->q = q; return lerr(l, "l%d is not defined", x->src[0].num);
        }
    }
    /* functions: index 0 = main, then each label in program order */
    int nf = 1, *fstart = calloc((size_t)n + 1, sizeof *fstart), *fend = calloc((size_t)n + 1, sizeof *fend);
    Depth *fmax = calloc((size_t)n + 1, sizeof *fmax), *ftot = calloc((size_t)n + 1, sizeof *ftot);
    int *fstate = calloc((size_t)n + 1, sizeof *fstate);
    if (!fstart || !fend || !fmax || !ftot || !fstate){ free(fstart); free(fend); free(fmax); free(ftot); free(fstate); return lerr(l, "out of memory"); }
    fstart[0] = 0; fend[0] = p->main_end;
    for (int q = p->main_end; q < n; q++)
        if (p->ins[q].op == OP_LABEL){
            fstart[nf] = q + 1;
            int e = q + 1;
            while (e < n && p->ins[e].op != OP_RET) e++;      /* ret is at the function's top level */
            fend[nf++] = e;
        }
    int ok = 1;
    /* local depths */
    for (int f = 0; f < nf && ok; f++){
        Depth d = { 0, 0, 0, 0 }, mx = { 0, 0, 0, 0 };
        int kind[64], ks = 0;                  /* 1 loop, 2 dyn if, 3 static if */
        for (int q = fstart[f]; q < fend[f] && ok; q++){
            const Ins *x = &p->ins[q];
            switch (x->op){
            case OP_REP: case OP_LOOP: kind[ks++] = 1; d.loop++; break;
            case OP_IFC: kind[ks++] = 2; d.dyn++; break;
            case OP_IF: if (x->src[0].type == RT_PRED){ kind[ks++] = 2; d.dyn++; } else { kind[ks++] = 3; d.stat++; } break;
            case OP_ENDREP: case OP_ENDLOOP: case OP_ENDIF:
                if (ks){ int k = kind[--ks]; if (k == 1) d.loop--; else if (k == 2) d.dyn--; else d.stat--; }
                break;
            default: break;
            }
            Depth here = d;
            if (x->op == OP_BREAKC) here.dyn++;
            if (here.loop > mx.loop) mx.loop = here.loop;
            if (here.dyn > mx.dyn) mx.dyn = here.dyn;
            if (here.stat > mx.stat) mx.stat = here.stat;
        }
        fmax[f] = mx;
    }
    /* totals through the call graph (depth-first; recursion is an error) */
    int stack[256], ssp = 0;
    for (int root = 0; root < nf && ok; root++){
        if (fstate[root] == 2) continue;
        stack[ssp++] = root; fstate[root] = 1;
        while (ssp && ok){
            int f = stack[ssp - 1], pending = 0;
            Depth tot = fmax[f];
            Depth d = { 0, 0, 0, 0 };
            int kind[64], ks = 0;
            for (int q = fstart[f]; q < fend[f]; q++){
                const Ins *x = &p->ins[q];
                switch (x->op){
                case OP_REP: case OP_LOOP: kind[ks++] = 1; d.loop++; break;
                case OP_IFC: kind[ks++] = 2; d.dyn++; break;
                case OP_IF: if (x->src[0].type == RT_PRED){ kind[ks++] = 2; d.dyn++; } else { kind[ks++] = 3; d.stat++; } break;
                case OP_ENDREP: case OP_ENDLOOP: case OP_ENDIF:
                    if (ks){ int k = kind[--ks]; if (k == 1) d.loop--; else if (k == 2) d.dyn--; else d.stat--; }
                    break;
                default: break;
                }
                if (x->op != OP_CALL && x->op != OP_CALLNZ) continue;
                int lq = p->label_at[x->src[0].num], g = 0;
                for (int k = 1; k < nf; k++) if (fstart[k] == lq + 1) g = k;
                if (fstate[g] == 1){ l->q = q; ok = lerr(l, "recursive call of l%d", x->src[0].num); break; }
                if (fstate[g] == 0){ if (ssp == 256){ ok = lerr(l, "call graph too deep"); break; } stack[ssp++] = g; fstate[g] = 1; pending = 1; break; }
                int pdyn = (x->op == OP_CALLNZ && x->src[1].type == RT_PRED) ? 1 : 0;
                if (d.loop + ftot[g].loop > tot.loop) tot.loop = d.loop + ftot[g].loop;
                if (d.dyn + pdyn + ftot[g].dyn > tot.dyn) tot.dyn = d.dyn + pdyn + ftot[g].dyn;
                if (d.stat + ftot[g].stat > tot.stat) tot.stat = d.stat + ftot[g].stat;
                if (1 + ftot[g].call > tot.call) tot.call = 1 + ftot[g].call;
            }
            if (!ok || pending) continue;
            ftot[f] = tot; fstate[f] = 2; ssp--;
        }
    }
    if (ok){
        Depth t = ftot[0];
        int ml, md, ms, mc;
        if (p->model == M_PS2X){
            ml = mc = p->caps.ps20_static_depth; md = p->caps.ps20_dynamic_depth; ms = p->caps.ps20_static_depth > 0 ? 24 : 0;
        } else { ml = 4; mc = 4; md = 24; ms = 24; }
        l->q = -1;
        if (t.loop > ml) ok = lerr(l, "loop/rep nesting %d (the model allows %d)", t.loop, ml);
        else if (t.dyn > md) ok = lerr(l, "dynamic flow control nesting %d (the model allows %d)", t.dyn, md);
        else if (t.stat > ms) ok = lerr(l, "static flow control nesting %d (the model allows %d)", t.stat, ms);
        else if (t.call > mc) ok = lerr(l, "call nesting %d (the model allows %d)", t.call, mc);
    }
    /* D3D9's gradient rule: a gradient of a temporary (dsx, dsy; texld, texldp, texldb with an r# coordinate)
     * is illegal inside flow control that may vary across pixels: a dynamic if, a loop that a per-pixel
     * break may leave early, or under a predicate */
    for (int f = 0; f < nf && ok; f++){
        int bs[64], bk = 0;
        for (int q = fstart[f]; q < fend[f] && ok; q++){
            const Ins *x = &p->ins[q];
            switch (x->op){
            case OP_REP: case OP_LOOP: case OP_IF: case OP_IFC: if (bk < 64) bs[bk++] = q; break;
            case OP_ENDREP: case OP_ENDLOOP: case OP_ENDIF: if (bk) bk--; break;
            default: break;
            }
            int grad = ((x->op == OP_DSX || x->op == OP_DSY) && x->src[0].type == RT_TEMP) ||
                       (x->op == OP_TEX && p->model != M_VS3 && x->src[0].type == RT_TEMP);
            if (!grad) continue;
            int divergent = x->pred;
            for (int k = 0; k < bk && !divergent; k++){
                const Ins *b = &p->ins[bs[k]];
                if (b->op == OP_IFC || (b->op == OP_IF && b->src[0].type == RT_PRED)) divergent = 1;
                if (b->op == OP_REP || b->op == OP_LOOP){
                    /* a break of this loop that only some pixels may take */
                    int depth = 0, dynif = 0;
                    int ifs[64], is = 0;
                    for (int r = bs[k] + 1; r < b->match && !divergent; r++){
                        const Ins *y = &p->ins[r];
                        if (y->op == OP_REP || y->op == OP_LOOP) depth++;
                        else if (y->op == OP_ENDREP || y->op == OP_ENDLOOP) depth--;
                        else if (y->op == OP_IF || y->op == OP_IFC){ int d = y->op == OP_IFC || y->src[0].type == RT_PRED; if (is < 64) ifs[is++] = d; dynif += d; }
                        else if (y->op == OP_ENDIF){ if (is) dynif -= ifs[--is]; }
                        else if (depth == 0 && (y->op == OP_BREAKC || y->op == OP_BREAKP || (y->op == OP_BREAK && dynif))) divergent = 1;
                    }
                }
            }
            if (divergent){ l->q = q; ok = lerr(l, "a gradient of a temporary inside flow control that may differ per pixel (D3D9 forbids it)"); }
        }
    }
    free(fstart); free(fend); free(fmax); free(ftot); free(fstate);
    return ok;
}

PsrefProg *psref_load(const uint32_t *tok, size_t ndw, const PsrefCaps *caps, char *err, size_t errcap){
    if (err && errcap) err[0] = 0;
    if (!tok || ndw < 2){ setf(err, errcap, "psref: no program"); return NULL; }
    int model;
    switch (tok[0]){
    case PSREF_PS_2_X: model = M_PS2X; break;
    case PSREF_PS_3_0: model = M_PS3; break;
    case PSREF_VS_3_0: model = M_VS3; break;
    default: setf(err, errcap, "psref: version token 0x%08x is not ps_2_x, ps_3_0 or vs_3_0", tok[0]); return NULL;
    }
    PsrefProg *p = calloc(1, sizeof *p);
    if (!p){ setf(err, errcap, "psref: out of memory"); return NULL; }
    p->version = tok[0]; p->model = model;
    if (caps) p->caps = *caps; else psref_caps_sm3(&p->caps);
    L l = { p, err, errcap, -1 };
    const PsrefCaps *c = &p->caps;
    if (model == M_PS2X && (c->ps20_num_temps < 12 || c->ps20_num_temps > 32 || c->ps20_dynamic_depth < 0 ||
                            c->ps20_dynamic_depth > 24 || c->ps20_static_depth < 0 || c->ps20_static_depth > 4)){
        lerr(&l, "D3DPSHADERCAPS2_0 outside D3D9's ranges"); psref_free(p); return NULL;
    }
    if (c->num_mrt < 1 || c->num_mrt > 4 || c->max_executed < 1 || (model == M_VS3 && (c->max_vs_const < 256 || c->max_vs_const > 8192))){
        lerr(&l, "caps outside D3D9's ranges"); psref_free(p); return NULL;
    }
    p->nconst = model == M_PS2X ? 32 : model == M_PS3 ? 224 : c->max_vs_const;
    p->def = calloc(p->nconst, sizeof *p->def);
    p->defd = calloc(p->nconst, 1);
    p->nlabel = model == M_PS2X ? 16 : 2048;
    p->label_at = malloc((size_t)p->nlabel * sizeof *p->label_at);
    if (!p->def || !p->defd || !p->label_at){ lerr(&l, "out of memory"); psref_free(p); return NULL; }
    for (int i = 0; i < p->nlabel; i++) p->label_at[i] = -1;
    if (!decode(&l, tok, ndw)){ psref_free(p); return NULL; }
    /* 1b: declarations and definitions first, wherever they are, so instructions can be checked against them */
    for (int q = 0; q < p->n; q++){
        Ins *x = &p->ins[q];
        l.q = q;
        if (x->op == OP_DCL && !chk_dcl(&l, x)){ psref_free(p); return NULL; }
        if ((x->op == OP_DEF || x->op == OP_DEFI || x->op == OP_DEFB) && !chk_def(&l, x)){ psref_free(p); return NULL; }
    }
    for (int q = 0; q < p->n; q++){
        l.q = q;
        if (!chk_ins(&l, &p->ins[q])){ psref_free(p); return NULL; }
    }
    l.q = -1;
    if (!link(&l)){ psref_free(p); return NULL; }
    return p;
}

/* ---- running ---- */
enum { FR_IF, FR_LOOP, FR_CALL };
typedef struct {
    int kind, start, isloop;
    unsigned entry, cond, broken;
    int count, iter, al, step;
} Frame;

typedef struct {
    const PsrefProg *p;
    const PsrefConsts *k;
    int nl;
    PsrefTex tex; void *user;
    float r[4][32][4]; uint8_t rdef[4][32];
    float pr[4][4]; uint8_t pdef[4];
    int a0[4][4]; uint8_t adef[4];
    float in[4][16][4]; uint8_t indef[16];
    float t[4][8][4];
    float pos[4][4]; float face[4];
    float oc[4][4][4]; uint8_t ocm[4][4];
    float od[4]; uint8_t odw[4];
    float o[12][4]; uint8_t om[12];
    uint8_t killed[4];
    Frame fr[FRMAX]; int nfr;
    unsigned active, all;
    unsigned long steps;
    int pc, lane;
    char *err; size_t errcap;
} Ex;

static int xfail(Ex *e, const char *fmt, ...){
    char m[256];
    va_list a; va_start(a, fmt); vsnprintf(m, sizeof m, fmt, a); va_end(a);
    const Ins *x = &e->p->ins[e->pc];
    if (e->nl > 1) setf(e->err, e->errcap, "psref: instruction %d (%s, token %d), lane %d: %s", e->pc, psref_opname(x->op), x->tok, e->lane, m);
    else setf(e->err, e->errcap, "psref: instruction %d (%s, token %d): %s", e->pc, psref_opname(x->op), x->tok, m);
    return 0;
}

static unsigned dead(const Ex *e){
    unsigned d = 0;
    for (int i = 0; i < e->nfr; i++) if (e->fr[i].kind == FR_LOOP) d |= e->fr[i].broken;
    return d;
}
static int cur_al(Ex *e, int *al){
    for (int i = e->nfr - 1; i >= 0; i--) if (e->fr[i].kind == FR_LOOP && e->fr[i].isloop){ *al = e->fr[i].al; return 1; }
    return xfail(e, "aL outside a loop");
}

static void cval(const Ex *e, int n, float v[4]){
    const PsrefProg *p = e->p;
    if (n < 0 || (unsigned)n >= p->nconst){ v[0] = v[1] = v[2] = v[3] = 0.f; return; }   /* D3D9: (0, 0, 0, 0) */
    if (p->defd[n]) memcpy(v, p->def[n], 16);
    else if (e->k && e->k->c && (unsigned)n < e->k->nc) memcpy(v, e->k->c[n], 16);
    else v[0] = v[1] = v[2] = v[3] = 0.f;
}
static void ival(const Ex *e, int n, int v[4]){
    if (e->p->defim >> n & 1u) memcpy(v, e->p->defi[n], 16);
    else if (e->k) memcpy(v, e->k->i[n], 16);
    else v[0] = v[1] = v[2] = v[3] = 0;
}
static int bval(const Ex *e, int n){
    if (e->p->defbm >> n & 1u) return e->p->defb[n];
    return e->k ? e->k->b[n] != 0 : 0;
}

/* register number after relative addressing */
static int eff_num(Ex *e, const Opd *o, int lane, int *n){
    *n = o->num;
    if (!o->rel) return 1;
    int off = 0;
    if (o->rtype == RT_LOOP){ if (!cur_al(e, &off)) return 0; }
    else {
        if (!(e->adef[lane] >> o->rcomp & 1u)) return xfail(e, "a0.%c read before mova writes it", "xyzw"[o->rcomp]);
        off = e->a0[lane][o->rcomp];
    }
    *n += off;
    return 1;
}

/* the register's value and its defined components */
static int fetch(Ex *e, const Opd *o, int lane, float v[4], unsigned *d){
    const PsrefProg *p = e->p;
    int n;
    if (!eff_num(e, o, lane, &n)) return 0;
    switch (o->type){
    case RT_TEMP: memcpy(v, e->r[lane][n], 16); *d = e->rdef[lane][n]; return 1;
    case RT_INPUT:
        if (n < 0 || n >= reg_count(p, RT_INPUT)) return xfail(e, "v[%d]: outside v0..v%d", n, reg_count(p, RT_INPUT) - 1);
        if (!e->indef[n]) return xfail(e, "v%d is not declared", n);
        memcpy(v, e->in[lane][n], 16); *d = e->indef[n]; return 1;
    case RT_CONST: cval(e, n, v); *d = 0xF; return 1;
    case RT_ADDR:
        if (p->model == M_VS3){ for (int c = 0; c < 4; c++) v[c] = (float)e->a0[lane][c]; *d = e->adef[lane]; return 1; }
        memcpy(v, e->t[lane][n], 16); *d = p->tm[n]; return 1;
    case RT_LOOP: { int al = 0; if (!cur_al(e, &al)) return 0; v[0] = v[1] = v[2] = v[3] = (float)al; *d = 0xF; return 1; }
    case RT_MISC:
        if (n == 0){ memcpy(v, e->pos[lane], 16); *d = p->posm; return 1; }
        v[0] = v[1] = v[2] = v[3] = e->face[lane]; *d = 0xF; return 1;
    default: return xfail(e, "register type %d read", o->type);
    }
}

/* a source operand: swizzle, then the modifier; need = result components that must be defined */
static int opv(Ex *e, const Opd *o, int lane, unsigned need, float out[4], unsigned *defm){
    float v[4]; unsigned d = 0;
    e->lane = lane;
    if (!fetch(e, o, lane, v, &d)) return 0;
    unsigned od = 0;
    for (int c = 0; c < 4; c++){ out[c] = v[o->swz[c]]; od |= ((d >> o->swz[c]) & 1u) << c; }
    if (need & ~od){
        unsigned miss = 0; char b[8];
        for (int c = 0; c < 4; c++) if ((need & ~od) >> c & 1u) miss |= 1u << o->swz[c];
        comps(b, miss);
        int n = o->num; eff_num(e, o, lane, &n);
        if (o->type == RT_TEMP) return xfail(e, "reads r%d.%s before it is written (undefined in D3D9)", n, b);
        if (o->type == RT_ADDR && e->p->model == M_VS3) return xfail(e, "reads a0.%s before mova writes it", b);
        return xfail(e, "reads %s%d.%s outside its declaration", rname(e->p->model, o->type, n), n, b);
    }
    for (int c = 0; c < 4; c++){
        float x = out[c];
        switch (o->mod){
        case SM_NEG: x = -x; break;
        case SM_ABS: x = fabsf(x); break;
        case SM_ABSNEG: x = -fabsf(x); break;
        default: break;
        }
        out[c] = x;
    }
    if (defm) *defm = od;
    return 1;
}

static int cmpf(int how, float a, float b){
    switch (how){
    case CMP_GT: return a > b;
    case CMP_EQ: return a == b;
    case CMP_GE: return a >= b;
    case CMP_LT: return a < b;
    case CMP_NE: return a != b;
    default: return a <= b;
    }
}

/* a condition: b# (same for every lane) or p0.c with an optional NOT */
static int cond(Ex *e, const Opd *o, int lane, int *b){
    int v;
    e->lane = lane;
    if (o->type == RT_CONSTBOOL) v = bval(e, o->num);
    else {
        int c = o->swz[0];
        if (!(e->pdef[lane] >> c & 1u)) return xfail(e, "reads p0.%c before setp writes it", "xyzw"[c]);
        v = e->pr[lane][c] != 0.f;
    }
    *b = o->mod == SM_NOT ? !v : v;
    return 1;
}

/* the components a predicated instruction writes in this lane */
static int pmask(Ex *e, const Ins *x, int lane, unsigned *m){
    *m = x->dst.mask;
    if (!x->pred) return 1;
    unsigned r = 0;
    for (int c = 0; c < 4; c++){
        if (!(x->dst.mask >> c & 1u)) continue;
        int pc = x->prd.swz[c];
        e->lane = lane;
        if (!(e->pdef[lane] >> pc & 1u)) return xfail(e, "the predicate reads p0.%c before setp writes it", "xyzw"[pc]);
        int b = e->pr[lane][pc] != 0.f;
        if (x->prd.mod == SM_NOT) b = !b;
        if (b) r |= 1u << c;
    }
    *m = r;
    return 1;
}

static float sat(float x){ return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

static int wr(Ex *e, const Ins *x, int lane, const float v[4], unsigned m){
    const Opd *d = &x->dst;
    const PsrefProg *p = e->p;
    if (!m) return 1;
    float w[4];
    for (int c = 0; c < 4; c++) w[c] = (d->dmod & DM_SAT) ? sat(v[c]) : v[c];
    int n;
    e->lane = lane;
    if (!eff_num(e, d, lane, &n)) return 0;
    switch (d->type){
    case RT_TEMP:
        for (int c = 0; c < 4; c++) if (m >> c & 1u) e->r[lane][n][c] = w[c];
        e->rdef[lane][n] |= (uint8_t)m;
        return 1;
    case RT_COLOROUT:
        for (int c = 0; c < 4; c++) if (m >> c & 1u) e->oc[lane][n][c] = w[c];
        e->ocm[lane][n] |= (uint8_t)m;
        return 1;
    case RT_DEPTHOUT:
        if (m & 1u){ e->od[lane] = w[0]; e->odw[lane] = 1; }
        return 1;
    case RT_OUTPUT:
        if (n < 0 || n >= 12) return xfail(e, "o[%d]: outside o0..o11", n);
        if (m & ~(unsigned)p->om[n]){ char b[8]; comps(b, m & ~(unsigned)p->om[n]); return xfail(e, "writes o%d.%s outside its declaration", n, b); }
        for (int c = 0; c < 4; c++) if (m >> c & 1u) e->o[n][c] = w[c];
        e->om[n] |= (uint8_t)m;
        return 1;
    default: return xfail(e, "write to register type %d", d->type);
    }
}

static float f_rcp(float f){                    /* rcp - vs pseudo-code */
    if (f == 0.f) return FLT_MAX;
    if (f != 1.f) return 1.f / f;
    return f;
}
static float f_rsq(float f){                    /* rsq - vs pseudo-code */
    f = fabsf(f);
    if (f == 0.f) return FLT_MAX;
    if (f != 1.f) return (float)(1.0 / (double)(float)sqrt((double)f));
    return f;
}
static float f_log(float f){                    /* log - ps/vs pseudo-code */
    float v = fabsf(f);
    if (v != 0.f) return (float)(log((double)v) / log(2.0));
    return -FLT_MAX;
}
static float f_exp(float f){ return (float)pow(2.0, (double)f); }

/* the quad's other lane in this row (dx) or column (dy) */
static int lo_x(int l){ return l & ~1; }
static int hi_x(int l){ return l | 1; }
static int lo_y(int l){ return l & ~2; }
static int hi_y(int l){ return l | 2; }

static int ex_tex(Ex *e, const Ins *x){
    const PsrefProg *p = e->p;
    int stage = x->src[1].num, type = p->smp[stage];
    int proj = x->op == OP_TEX && x->ctrl == 1, bias = x->op == OP_TEX && x->ctrl == 2;
    int implicit = x->op == OP_TEX;
    float c[4][4]; unsigned cm[4] = { 0, 0, 0, 0 };
    float q[4][4];
    /* the coordinate: every lane when the level of detail comes from the quad */
    unsigned lanes = implicit ? e->all : e->active;
    if (implicit && e->active != e->all && x->src[0].type == RT_TEMP)
        return xfail(e, "an implicit level of detail of a temporary in divergent flow (D3D9 forbids it)");
    for (int l = 0; l < e->nl; l++){
        if (!(lanes >> l & 1u)) continue;
        if (!opv(e, &x->src[0], l, x->need[0], c[l], &cm[l])) return 0;
        memcpy(q[l], c[l], 16);
        if (proj) for (int k = 0; k < 3; k++) q[l][k] = c[l][k] / c[l][3];
    }
    for (int l = 0; l < e->nl; l++){
        if (!(e->active >> l & 1u)) continue;
        unsigned m;
        if (!pmask(e, x, l, &m)) return 0;
        if (!m) continue;
        PsrefSample s;
        memset(&s, 0, sizeof s);
        s.stage = stage; s.type = type; s.vertex = p->model == M_VS3;
        memcpy(s.c, q[l], 16); s.cmask = cm[l];
        e->lane = l;
        if (implicit){
            s.mode = bias ? PSREF_LOD_BIAS : PSREF_LOD_AUTO;
            for (int k = 0; k < 4; k++){ s.dx[k] = q[hi_x(l)][k] - q[lo_x(l)][k]; s.dy[k] = q[hi_y(l)][k] - q[lo_y(l)][k]; }
            if (bias){
                s.lod = c[l][3];
                float lo = p->model == M_PS2X ? -3.f : -16.f, hi = p->model == M_PS2X ? 3.f : 15.f;
                if (!(s.lod >= lo && s.lod <= hi)) return xfail(e, "texldb bias %g outside [%g, %g] (undefined in D3D9)", (double)s.lod, (double)lo, (double)hi);
            }
        } else if (x->op == OP_TEXLDL){
            s.mode = PSREF_LOD_LEVEL; s.lod = c[l][3];
        } else {
            s.mode = PSREF_LOD_GRAD;
            if (!opv(e, &x->src[2], l, x->need[2], s.dx, 0) || !opv(e, &x->src[3], l, x->need[3], s.dy, 0)) return 0;
        }
        float t[4], r[4];
        if (!e->tex){ e->lane = l; return xfail(e, "no texture function"); }
        if (!e->tex(e->user, &s, t)){ e->lane = l; return xfail(e, "the texture function refused s%d", stage); }
        for (int k = 0; k < 4; k++) r[k] = t[x->src[1].swz[k]];
        if (!wr(e, x, l, r, m)) return 0;
    }
    return 1;
}

static int ex_grad(Ex *e, const Ins *x){
    float v[4][4];
    if (e->active != e->all && x->src[0].type == RT_TEMP)
        return xfail(e, "%s of a temporary in divergent flow (D3D9 forbids it)", psref_opname(x->op));
    for (int l = 0; l < e->nl; l++)
        if (!opv(e, &x->src[0], l, x->need[0], v[l], 0)) return 0;
    for (int l = 0; l < e->nl; l++){
        if (!(e->active >> l & 1u)) continue;
        unsigned m;
        if (!pmask(e, x, l, &m)) return 0;
        float d[4];
        for (int c = 0; c < 4; c++)
            d[c] = x->op == OP_DSX ? v[hi_x(l)][c] - v[lo_x(l)][c] : v[hi_y(l)][c] - v[lo_y(l)][c];
        if (!wr(e, x, l, d, m)) return 0;
    }
    return 1;
}

static int ex_lane(Ex *e, const Ins *x, int l){
    const PsrefProg *p = e->p;
    unsigned m;
    if (!pmask(e, x, l, &m)) return 0;
    if (x->op == OP_TEXKILL){
        Opd s = x->dst; s.mod = 0;
        for (int c = 0; c < 4; c++) s.swz[c] = (uint8_t)c;
        float v[4];
        if (!opv(e, &s, l, x->dst.mask, v, 0)) return 0;
        for (int c = 0; c < 4; c++) if ((x->dst.mask >> c & 1u) && v[c] < 0.f) e->killed[l] = 1;
        return 1;
    }
    if (!m) return 1;
    float s[4][4], d[4] = { 0, 0, 0, 0 };
    int ismat = x->op >= OP_M4x4 && x->op <= OP_M3x2;
    int nval = (x->op == OP_SINCOS || x->op == OP_SGN) ? 1 : ismat ? 1 : x->nsrc;
    for (int i = 0; i < nval; i++)
        if (!opv(e, &x->src[i], l, x->cw ? m : x->need[i], s[i], 0)) return 0;
    switch (x->op){
    case OP_MOV: memcpy(d, s[0], 16); break;
    case OP_ADD: for (int c = 0; c < 4; c++) d[c] = s[0][c] + s[1][c]; break;
    case OP_SUB: for (int c = 0; c < 4; c++) d[c] = s[0][c] - s[1][c]; break;
    case OP_MUL: for (int c = 0; c < 4; c++) d[c] = s[0][c] * s[1][c]; break;
    case OP_MAD: for (int c = 0; c < 4; c++){ float t = s[0][c] * s[1][c]; d[c] = t + s[2][c]; } break;
    case OP_MIN: for (int c = 0; c < 4; c++) d[c] = s[0][c] < s[1][c] ? s[0][c] : s[1][c]; break;
    case OP_MAX: for (int c = 0; c < 4; c++) d[c] = s[0][c] >= s[1][c] ? s[0][c] : s[1][c]; break;
    case OP_SLT: for (int c = 0; c < 4; c++) d[c] = s[0][c] < s[1][c] ? 1.f : 0.f; break;
    case OP_SGE: for (int c = 0; c < 4; c++) d[c] = s[0][c] >= s[1][c] ? 1.f : 0.f; break;
    case OP_FRC: for (int c = 0; c < 4; c++) d[c] = s[0][c] - floorf(s[0][c]); break;
    case OP_ABS: for (int c = 0; c < 4; c++) d[c] = fabsf(s[0][c]); break;
    case OP_SGN: for (int c = 0; c < 4; c++) d[c] = s[0][c] < 0.f ? -1.f : (s[0][c] == 0.f ? 0.f : 1.f); break;
    case OP_LRP: for (int c = 0; c < 4; c++){ float t = s[1][c] - s[2][c]; float u = s[0][c] * t; d[c] = u + s[2][c]; } break;
    case OP_CMP: for (int c = 0; c < 4; c++) d[c] = s[0][c] >= 0.f ? s[1][c] : s[2][c]; break;
    case OP_RCP: d[0] = d[1] = d[2] = d[3] = f_rcp(s[0][0]); break;
    case OP_RSQ: d[0] = d[1] = d[2] = d[3] = f_rsq(s[0][0]); break;
    case OP_EXP: case OP_EXPP: d[0] = d[1] = d[2] = d[3] = f_exp(s[0][0]); break;
    case OP_LOG: case OP_LOGP: d[0] = d[1] = d[2] = d[3] = f_log(s[0][0]); break;
    case OP_POW: d[0] = d[1] = d[2] = d[3] = (float)pow((double)fabsf(s[0][0]), (double)s[1][0]); break;
    case OP_DP3: { float a = s[0][0] * s[1][0], b = s[0][1] * s[1][1], c2 = s[0][2] * s[1][2];
                   float r = a + b; r = r + c2; d[0] = d[1] = d[2] = d[3] = r; } break;
    case OP_DP4: { float a = s[0][0] * s[1][0], b = s[0][1] * s[1][1], c2 = s[0][2] * s[1][2], w = s[0][3] * s[1][3];
                   float r = a + b; r = r + c2; r = r + w; d[0] = d[1] = d[2] = d[3] = r; } break;
    case OP_DP2ADD: { float a = s[0][0] * s[1][0], b = s[0][1] * s[1][1];
                      float r = a + b; r = r + s[2][0]; d[0] = d[1] = d[2] = d[3] = r; } break;
    case OP_NRM: { float a = s[0][0] * s[0][0], b = s[0][1] * s[0][1], c2 = s[0][2] * s[0][2];
                   float f = a + b; f = f + c2;
                   f = f != 0.f ? (float)(1.0 / sqrt((double)f)) : FLT_MAX;
                   for (int c = 0; c < 4; c++) d[c] = s[0][c] * f; } break;
    case OP_CRS: { float a, b;
                   a = s[0][1] * s[1][2]; b = s[0][2] * s[1][1]; d[0] = a - b;
                   a = s[0][2] * s[1][0]; b = s[0][0] * s[1][2]; d[1] = a - b;
                   a = s[0][0] * s[1][1]; b = s[0][1] * s[1][0]; d[2] = a - b; } break;
    case OP_LIT: { float pw = s[0][3]; const float MAXP = 127.9961f;
                   if (pw < -MAXP) pw = -MAXP; else if (pw > MAXP) pw = MAXP;
                   d[0] = 1.f; d[1] = 0.f; d[2] = 0.f; d[3] = 1.f;
                   if (s[0][0] > 0.f){ d[1] = s[0][0]; if (s[0][1] > 0.f) d[2] = (float)pow((double)s[0][1], (double)pw); } } break;
    case OP_DST: d[0] = 1.f; d[1] = s[0][1] * s[1][1]; d[2] = s[0][2]; d[3] = s[1][3]; break;
    case OP_SINCOS: {
        float a = s[0][0];
        if (!(a >= -(float)3.14159265358979323846 && a <= (float)3.14159265358979323846))
            return xfail(e, "sincos of %g, outside [-pi, pi] (undefined in D3D9)", (double)a);
        d[0] = (float)cos((double)a); d[1] = (float)sin((double)a); d[2] = d[3] = 0.f;
        if (!wr(e, x, l, d, m)) return 0;
        if (p->model == M_PS2X){                /* ps_2_x leaves .z (and the unwritten one of .x/.y) undefined */
            int n = x->dst.num;
            unsigned und = 4u | ((x->dst.mask == 1) ? 2u : (x->dst.mask == 2) ? 1u : 0u);
            e->rdef[l][n] &= (uint8_t)~und;
        }
        return 1; }
    case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
        int rows = (x->op == OP_M4x4 || x->op == OP_M3x4) ? 4 : (x->op == OP_M3x2) ? 2 : 3;
        int cols = (x->op == OP_M4x4 || x->op == OP_M4x3) ? 4 : 3;
        for (int r = 0; r < rows; r++){
            Opd row = x->src[1]; row.num = (uint16_t)(row.num + r);
            float v[4];
            if (!opv(e, &row, l, cols == 4 ? 15u : 7u, v, 0)) return 0;
            float acc = s[0][0] * v[0];
            for (int c = 1; c < cols; c++){ float t = s[0][c] * v[c]; acc = acc + t; }
            d[r] = acc;
        }
        break; }
    case OP_MOVA:
        for (int c = 0; c < 4; c++){
            if (!(m >> c & 1u)) continue;
            float v = s[0][c];
            if (!(fabsf(v) < 16777216.f)) return xfail(e, "mova of %g", (double)v);
            e->a0[l][c] = (int)roundf(v);         /* to nearest; halves away from zero */
        }
        e->adef[l] |= (uint8_t)m;
        return 1;
    case OP_SETP:
        for (int c = 0; c < 4; c++) if (m >> c & 1u) e->pr[l][c] = cmpf(x->ctrl, s[0][c], s[1][c]) ? 1.f : 0.f;
        e->pdef[l] |= (uint8_t)m;
        return 1;
    default:
        return xfail(e, "%s is not implemented", psref_opname(x->op));
    }
    if (!wr(e, x, l, d, m)) return 0;
    if (x->op == OP_SGN)                          /* sgn's scratch registers are undefined after it */
        for (int i = 1; i < 3; i++) e->rdef[l][x->src[i].num] = 0;
    return 1;
}

static int push(Ex *e, Frame f){
    if (e->nfr == FRMAX) return xfail(e, "too many open blocks");
    e->fr[e->nfr++] = f;
    return 1;
}
static Frame *innermost_loop(Ex *e){
    for (int i = e->nfr - 1; i >= 0; i--){
        if (e->fr[i].kind == FR_CALL) return NULL;
        if (e->fr[i].kind == FR_LOOP) return &e->fr[i];
    }
    return NULL;
}

static int run(Ex *e){
    const PsrefProg *p = e->p;
    int ncall = 0;
    e->active = e->all;
    e->pc = 0;
    for (;;){
        if (!ncall && e->pc >= p->main_end) return 1;
        if (e->pc >= p->n) return xfail(e, "ran past the program");
        const Ins *x = &p->ins[e->pc];
        if (x->op != OP_DCL && x->op != OP_DEF && x->op != OP_DEFI && x->op != OP_DEFB &&
            ++e->steps > p->caps.max_executed) return xfail(e, "more than %u instructions executed", p->caps.max_executed);
        int next = e->pc + 1;
        e->lane = 0;
        switch (x->op){
        case OP_NOP: case OP_DCL: case OP_DEF: case OP_DEFI: case OP_DEFB: case OP_LABEL: break;
        case OP_IF: case OP_IFC: {
            if (!e->active){
                int q = x->match;
                if (p->ins[q].op == OP_ELSE) q = p->ins[q].match;
                next = q + 1; break;
            }
            unsigned c = 0;
            for (int l = 0; l < e->nl; l++){
                if (!(e->active >> l & 1u)) continue;
                int b;
                if (x->op == OP_IF){ if (!cond(e, &x->src[0], l, &b)) return 0; }
                else {
                    float a[4], bb[4];
                    if (!opv(e, &x->src[0], l, 1, a, 0) || !opv(e, &x->src[1], l, 1, bb, 0)) return 0;
                    b = cmpf(x->ctrl, a[0], bb[0]);
                }
                if (b) c |= 1u << l;
            }
            Frame f = { FR_IF, e->pc, 0, e->active, c, 0, 0, 0, 0, 0 };
            if (!push(e, f)) return 0;
            e->active = c;
            break; }
        case OP_ELSE: {
            Frame *f = &e->fr[e->nfr - 1];
            e->active = f->entry & ~f->cond & ~dead(e);
            break; }
        case OP_ENDIF: {
            Frame f = e->fr[--e->nfr];
            e->active = f.entry & ~dead(e);
            break; }
        case OP_REP: case OP_LOOP: {
            if (!e->active){ next = x->match + 1; break; }
            int iv[4], ir = x->op == OP_REP ? x->src[0].num : x->src[1].num;
            ival(e, ir, iv);
            if (iv[0] < 0 || iv[0] > 255) return xfail(e, "i%d.x = %d: a loop count outside [0, 255] (undefined in D3D9)", ir, iv[0]);
            Frame f = { FR_LOOP, e->pc, x->op == OP_LOOP, e->active, 0, 0, iv[0], 0, 0, 0 };
            if (x->op == OP_LOOP){
                if (iv[1] < 0 || iv[1] > 255) return xfail(e, "i%d.y = %d: aL's start outside [0, 255]", ir, iv[1]);
                if (iv[2] < -128 || iv[2] > 127) return xfail(e, "i%d.z = %d: aL's step outside [-128, 127]", ir, iv[2]);
                if (iv[3] != 0) return xfail(e, "i%d.w = %d: must be 0", ir, iv[3]);
                f.al = iv[1]; f.step = iv[2];
            }
            if (!iv[0]){ next = x->match + 1; break; }
            if (!push(e, f)) return 0;
            break; }
        case OP_ENDREP: case OP_ENDLOOP: {
            Frame *f = &e->fr[e->nfr - 1];
            f->iter++;
            unsigned cont = f->entry & ~dead(e);
            if (f->iter < f->count && cont){ e->active = cont; f->al += f->step; next = f->start + 1; }
            else { unsigned en = f->entry; e->nfr--; e->active = en & ~dead(e); }
            break; }
        case OP_BREAK: case OP_BREAKC: case OP_BREAKP: {
            unsigned lanes = 0;
            for (int l = 0; l < e->nl; l++){
                if (!(e->active >> l & 1u)) continue;
                int b = 1;
                if (x->op == OP_BREAKC){
                    float a[4], bb[4];
                    if (!opv(e, &x->src[0], l, 1, a, 0) || !opv(e, &x->src[1], l, 1, bb, 0)) return 0;
                    b = cmpf(x->ctrl, a[0], bb[0]);
                } else if (x->op == OP_BREAKP){ if (!cond(e, &x->src[0], l, &b)) return 0; }
                if (b) lanes |= 1u << l;
            }
            Frame *f = innermost_loop(e);
            if (!f) return xfail(e, "break outside a loop");
            f->broken |= lanes;
            e->active &= ~lanes;
            break; }
        case OP_CALL: case OP_CALLNZ: {
            unsigned lanes = 0;
            for (int l = 0; l < e->nl; l++){
                if (!(e->active >> l & 1u)) continue;
                int b = 1;
                if (x->op == OP_CALLNZ && !cond(e, &x->src[1], l, &b)) return 0;
                if (b) lanes |= 1u << l;
            }
            if (!lanes) break;
            int lim = p->model == M_PS2X ? p->caps.ps20_static_depth : 4;
            if (ncall >= lim) return xfail(e, "calls nested deeper than %d", lim);
            Frame f = { FR_CALL, e->pc + 1, 0, e->active, 0, 0, 0, 0, 0, 0 };
            if (!push(e, f)) return 0;
            ncall++;
            e->active = lanes;
            next = p->label_at[x->src[0].num] + 1;
            break; }
        case OP_RET: {
            if (!ncall) return 1;
            Frame f = e->fr[--e->nfr];
            if (f.kind != FR_CALL) return xfail(e, "ret inside a block");
            ncall--;
            e->active = f.entry & ~dead(e);
            next = f.start;
            break; }
        case OP_TEX: case OP_TEXLDL: case OP_TEXLDD:
            if (e->active && !ex_tex(e, x)) return 0;
            break;
        case OP_DSX: case OP_DSY:
            if (e->active && !ex_grad(e, x)) return 0;
            break;
        default:
            for (int l = 0; l < e->nl; l++)
                if ((e->active >> l & 1u) && !ex_lane(e, x, l)) return 0;
            break;
        }
        e->pc = next;
    }
}

static void ex_init(Ex *e, const PsrefProg *p, const PsrefConsts *k, int nl, PsrefTex tex, void *user, char *err, size_t errcap){
    memset(e, 0, sizeof *e);
    e->p = p; e->k = k; e->nl = nl; e->tex = tex; e->user = user;
    e->all = (1u << nl) - 1u;
    e->err = err; e->errcap = errcap;
}

int psref_ps_run(const PsrefProg *p, const PsrefConsts *k, const PsrefPsIn in[4], PsrefTex tex, void *user,
                 PsrefPsOut out[4], char *err, size_t errcap){
    if (err && errcap) err[0] = 0;
    if (!p || !in || !out){ setf(err, errcap, "psref: no program"); return 0; }
    if (p->model == M_VS3){ setf(err, errcap, "psref: a vertex shader given to psref_ps_run"); return 0; }
    Ex *e = malloc(sizeof *e);
    if (!e){ setf(err, errcap, "psref: out of memory"); return 0; }
    ex_init(e, p, k, 4, tex, user, err, errcap);
    int nin = reg_count(p, RT_INPUT);
    for (int r = 0; r < nin; r++){
        unsigned dm = p->inm[r];
        if (!dm) continue;
        e->indef[r] = p->model == M_PS2X ? 0xF : (uint8_t)dm;
        for (int l = 0; l < 4; l++)
            for (int c = 0; c < 4; c++)                 /* ps_2_x colours: (0, 0, 0, 1) outside the declaration */
                e->in[l][r][c] = (p->model == M_PS2X && !(dm >> c & 1u)) ? (c == 3 ? 1.f : 0.f) : in[l].v[r][c];
    }
    for (int l = 0; l < 4; l++){
        memcpy(e->t[l], in[l].t, sizeof in[l].t);
        e->pos[l][0] = in[l].pos[0]; e->pos[l][1] = in[l].pos[1];
        e->face[l] = in[l].face;
    }
    int ok = run(e);
    if (ok){
        for (int l = 0; l < 4; l++){
            memset(&out[l], 0, sizeof out[l]);
            memcpy(out[l].oC, e->oc[l], sizeof out[l].oC);
            for (int r = 0; r < 4; r++) out[l].oCmask[r] = e->ocm[l][r];
            out[l].depth = e->od[l]; out[l].depth_written = e->odw[l];
            out[l].killed = e->killed[l];
        }
    }
    free(e);
    return ok;
}

int psref_vs_run(const PsrefProg *p, const PsrefConsts *k, const float v[16][4], PsrefTex tex, void *user,
                 float o[12][4], unsigned omask[12], char *err, size_t errcap){
    if (err && errcap) err[0] = 0;
    if (!p || !v || !o || !omask){ setf(err, errcap, "psref: no program"); return 0; }
    if (p->model != M_VS3){ setf(err, errcap, "psref: a pixel shader given to psref_vs_run"); return 0; }
    Ex *e = malloc(sizeof *e);
    if (!e){ setf(err, errcap, "psref: out of memory"); return 0; }
    ex_init(e, p, k, 1, tex, user, err, errcap);
    for (int r = 0; r < 16; r++){
        if (!p->inm[r]) continue;
        e->indef[r] = 0xF;                              /* the stream gives all four (D3D9: partial (0, 0, 0, 1)) */
        memcpy(e->in[0][r], v[r], 16);
    }
    int ok = run(e);
    if (ok){
        memcpy(o, e->o, sizeof e->o);
        for (int r = 0; r < 12; r++) omask[r] = e->om[r];
    }
    free(e);
    return ok;
}
