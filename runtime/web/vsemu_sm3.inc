/* Shader Model 3 class to WGSL: ps_2_x, ps_3_0 and vs_3_0 (included from
 * vsemu_wgsl.inc, whose module text and helpers it shares).
 *
 * The semantics are Direct3D 9's, as web/tools/psref.c runs them
 * (docs/web/psref.md), the oracle of the differential test (web/tools/wgsl-xl.c
 * writes the vectors, web/src/test/shadercheck.ts runs the modules on the
 * GPU); not the Mac port's shortcuts of the ps_1_x / ps_2_0 / vs_1_1 path.
 * This path decodes the bytecode itself (VSProg.raw: the tokens from the
 * version token to the end token) and translates instruction for
 * instruction, at full precision (_pp is a hint):
 *   - registers r0..r31; ps_2_x t0..t7 and v0/v1 (a colour outside its
 *     declaration reads (0, 0, 0, 1)); ps_3_0 v0..v9 and vs_3_0 o0..o11 by
 *     their declarations (packed ones too); vPos (the pixel's corner, as
 *     D3D9 puts pixel centres on integers) and vFace (+1 front, -1 back);
 *   - constants: the program's own def, defi and defb first, relative reads
 *     included; else P.c (pixel shaders, 32 rows) or the draw's K block
 *     (vertex shaders, 256 rows; a relative read outside them is
 *     (0, 0, 0, 0)).  The ABI has no integer or boolean constants
 *     (gpu-abi.md section 11.1): an i# or b# without its defi or defb is
 *     refused, so every loop count is a literal;
 *   - rcp and rsq of 0 = FLT_MAX and of 1 exactly 1, log of 0 = -FLT_MAX,
 *     pow of |x|, nrm of a zero vector = FLT_MAX times the vector (w
 *     included), lit's power clamped to +-127.9961, exp/expp = 2^x and
 *     log/logp = log2|x| (one component, as in every 2.0+ model), mova rounds
 *     halves away from zero, cmp tests >= 0, lrp is src0 * (src1 - src2) + src2;
 *     min/max with a NaN and pow(0, y <= 0) are the GPU's (WGSL leaves them
 *     open; psref.md says what D3D9's pages give);
 *   - flow control as WGSL blocks: if b#, if_comp, if p0, rep and loop with
 *     a literal count, break, break_comp, breakp; call and callnz inlined
 *     (D3D9 has no recursion); predicated writes per component; setp;
 *   - texld, texldp (x, y and z divided by w), texldb as textureSample and
 *     textureSampleBias, texldl as textureSampleLevel, texldd as
 *     textureSampleGrad, the sampler's swizzle on the result; dsx and dsy as
 *     fine derivatives (the neighbour in the same row or column, as psref);
 *   - texkill marks the pixel and it runs on (discard at the end); ps_2_x
 *     ends with the fixed-function alpha test and fog (ps_end), ps_3_0 with
 *     the alpha test only (D3D9 applies no fixed-function fog after it).
 * WGSL allows implicit derivatives only in uniform control flow, D3D9 those
 * of an input anywhere: inside flow that may differ per pixel (a dynamic if,
 * a loop a per-pixel break may leave, a callnz p0) a lookup with an implicit
 * level of detail whose coordinate is an input or a constant is made at the
 * top of the function, and dsx/dsy of one are taken there.  Of a temporary
 * there D3D9 forbids them, and the translation is refused.
 *
 * Refused (0; the reason in wgsl_refusal()): another model, a volume sampler,
 * MRT (oC1..oC3), vertex texture fetch (the ABI has no texture in the vertex
 * stage), c32+ in a pixel shader, an i# or b# the application sets, a
 * register the module ABI has no place for (a ps_3_0 input or vs_3_0 output
 * of another usage; a texture coordinate or sampler of stage 4..7 without 8
 * stages), and bytecode it cannot decode.  Nothing is skipped. */

static const char *s3_why;                  /* the last refusal's reason (static text) */
const char *wgsl_refusal(void){ return s3_why ? s3_why : ""; }

enum { S3_PS2X = 1, S3_PS3, S3_VS3 };
/* D3DSIO_* opcodes */
enum { S3_NOP = 0, S3_MOV, S3_ADD, S3_SUB, S3_MAD, S3_MUL, S3_RCP, S3_RSQ, S3_DP3, S3_DP4, S3_MIN, S3_MAX,
       S3_SLT, S3_SGE, S3_EXP, S3_LOG, S3_LIT, S3_DST, S3_LRP, S3_FRC, S3_M4x4, S3_M4x3, S3_M3x4, S3_M3x3,
       S3_M3x2, S3_CALL, S3_CALLNZ, S3_LOOP, S3_RET, S3_ENDLOOP, S3_LABEL, S3_DCL, S3_POW, S3_CRS, S3_SGN,
       S3_ABS, S3_NRM, S3_SINCOS, S3_REP, S3_ENDREP, S3_IF, S3_IFC, S3_ELSE, S3_ENDIF, S3_BREAK, S3_BREAKC,
       S3_MOVA, S3_DEFB, S3_DEFI, S3_TEXKILL = 65, S3_TEX = 66, S3_EXPP = 78, S3_LOGP = 79, S3_DEF = 81,
       S3_CMP = 88, S3_DP2ADD = 90, S3_DSX = 91, S3_DSY = 92, S3_TEXLDD = 93, S3_SETP = 94, S3_TEXLDL = 95,
       S3_BREAKP = 96 };
/* source modifiers (D3DSPSM_* >> 24) and the saturate result modifier (D3DSPDM_SATURATE >> 20) */
enum { S3M_NONE = 0, S3M_NEG = 1, S3M_ABS = 11, S3M_ABSNEG = 12, S3M_NOT = 13 };
#define S3D_SAT 1u
#define S3_FLTMAX "0x1.fffffep+127f"   /* FLT_MAX, exactly */

typedef struct {
    int type, num;
    uint8_t swz[4];             /* source: the register component of each result component */
    unsigned mask, mod;         /* destination write mask; source modifier or destination modifiers */
    int shift;
    int rel, rtype, rcomp;      /* relative addressing: by a0.<rcomp> or aL */
} S3Opd;
typedef struct {
    unsigned op, ctrl;
    int pred, hasdst, nsrc;
    S3Opd dst, prd, src[4];
    unsigned usage, uindex, smptype;    /* dcl */
    uint32_t imm[4];                    /* def, defi, defb */
    int match;                  /* if -> else or endif, else -> endif, rep/loop -> its end */
} S3Ins;
typedef struct { int reg, usage, index; unsigned mask; } S3Dcl;
typedef struct {
    int model;
    S3Ins *ins; int n;
    int main_end;               /* main is [0, main_end) */
    int label_at[2048];         /* a label's first instruction, 0 = not defined */
    float def[256][4]; uint8_t defd[256];
    int32_t defi[16][4]; unsigned defim;
    unsigned defb, defbm;
    S3Dcl din[16], dout[16]; int nin, nout;
    unsigned vmask[16];         /* declared components of v# */
    unsigned tmask[8];          /* ps_2_x: of t# */
    int smp[16];                /* sampler types: 2 2D, 3 cube, 4 volume */
    int vpos, vface;
} S3Prog;

static int s3_fail(const char *why){ if (!s3_why) s3_why = why; return 0; }
static int s3_pf(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int s3_pf(char *out, size_t cap, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return w >= 0 && (size_t)w < cap ? 1 : s3_fail("an expression too long");
}

/* ---- decoding ---- */
static int s3_opd(const uint32_t *t, size_t end, size_t *i, S3Opd *o, int dst){
    if (*i >= end) return 0;
    uint32_t v = t[(*i)++];
    if (!(v & 0x80000000u)) return 0;
    memset(o, 0, sizeof *o);
    o->type = (int)(((v >> 28) & 7u) | ((v >> 8) & 0x18u));
    o->num = (int)(v & 0x7FFu);
    if (dst){ o->mask = (v >> 16) & 0xFu; o->mod = (v >> 20) & 0xFu; o->shift = (int)((v >> 24) & 0xFu); }
    else {
        for (int c = 0; c < 4; c++) o->swz[c] = (uint8_t)((v >> (16 + 2 * c)) & 3u);
        o->mod = (v >> 24) & 0xFu;
    }
    if (v & 0x2000u){
        if (*i >= end) return 0;
        uint32_t r = t[(*i)++];
        if (!(r & 0x80000000u)) return 0;
        o->rel = 1;
        o->rtype = (int)(((r >> 28) & 7u) | ((r >> 8) & 0x18u));
        o->rcomp = (int)((r >> 16) & 3u);
        if (o->rtype != R_ADDR && o->rtype != R_LOOP) return 0;
    }
    return 1;
}

static int s3_nodst(unsigned op){
    switch (op){
    case S3_NOP: case S3_CALL: case S3_CALLNZ: case S3_LOOP: case S3_RET: case S3_ENDLOOP: case S3_LABEL:
    case S3_REP: case S3_ENDREP: case S3_IF: case S3_IFC: case S3_ELSE: case S3_ENDIF: case S3_BREAK:
    case S3_BREAKC: case S3_BREAKP:
        return 1;
    default: return 0;
    }
}

static void s3_free(S3Prog *p){ if (p){ free(p->ins); free(p); } }

static S3Prog *s3_decode(const uint32_t *tok, size_t ndw){
    if (!tok || ndw < 2){ s3_fail("no bytecode"); return 0; }
    uint32_t ver = tok[0];
    int model = ver == 0xFFFF0201u ? S3_PS2X : ver == 0xFFFF0300u ? S3_PS3 : ver == 0xFFFE0300u ? S3_VS3 : 0;
    if (!model){ s3_fail("not ps_2_x, ps_3_0 or vs_3_0"); return 0; }
    S3Prog *p = calloc(1, sizeof *p);
    if (!p){ s3_fail("out of memory"); return 0; }
    p->model = model;
    int cap = 64;
    p->ins = malloc((size_t)cap * sizeof *p->ins);
    if (!p->ins){ s3_free(p); s3_fail("out of memory"); return 0; }
    size_t i = 1;
    int ended = 0;
    while (i < ndw){
        uint32_t t = tok[i];
        unsigned op = t & 0xFFFFu;
        if (op == 0xFFFFu){ ended = 1; break; }
        if (op == 0xFFFEu){ i += 1 + ((t >> 16) & 0x7FFFu); continue; }
        if (t & 0x40000000u){ s3_free(p); s3_fail("a co-issued instruction"); return 0; }
        size_t body = i + 1, end = body + ((t >> 24) & 0xFu);
        if (end > ndw){ s3_free(p); s3_fail("an instruction runs past the program"); return 0; }
        if (p->n == cap){
            if (cap >= 32768){ s3_free(p); s3_fail("too many instructions"); return 0; }
            cap *= 2;
            S3Ins *ni = realloc(p->ins, (size_t)cap * sizeof *ni);
            if (!ni){ s3_free(p); s3_fail("out of memory"); return 0; }
            p->ins = ni;
        }
        S3Ins *x = &p->ins[p->n];
        memset(x, 0, sizeof *x);
        x->op = op; x->ctrl = (t >> 16) & 0xFFu; x->pred = (int)((t >> 28) & 1u); x->match = -1;
        size_t j = body;
        int ok = 1;
        if (op == S3_DCL){
            if (j + 2 > end) ok = 0;
            else {
                uint32_t u = tok[j++];
                x->usage = u & 0x1Fu; x->uindex = (u >> 16) & 0xFu; x->smptype = (u >> 27) & 0xFu;
                ok = s3_opd(tok, end, &j, &x->dst, 1); x->hasdst = 1;
            }
        } else if (op == S3_DEF || op == S3_DEFI || op == S3_DEFB){
            unsigned k = op == S3_DEFB ? 1u : 4u;
            ok = s3_opd(tok, end, &j, &x->dst, 1) && j + k <= end;
            if (ok){ memcpy(x->imm, &tok[j], k * 4u); j += k; }
            x->hasdst = 1;
        } else {
            /* destination, then a predicated instruction's predicate, then the sources */
            x->hasdst = !s3_nodst(op);
            if (x->pred && !x->hasdst){ s3_free(p); s3_fail("a predicated flow-control instruction"); return 0; }
            if (x->hasdst) ok = s3_opd(tok, end, &j, &x->dst, 1);
            if (ok && x->pred) ok = s3_opd(tok, end, &j, &x->prd, 0);
            while (ok && j < end){
                if (x->nsrc == 4){ ok = 0; break; }
                ok = s3_opd(tok, end, &j, &x->src[x->nsrc++], 0);
            }
        }
        if (!ok || j != end){ s3_free(p); s3_fail("an instruction's operands do not decode"); return 0; }
        i = end;
        p->n++;
    }
    if (!ended){ s3_free(p); s3_fail("no end token"); return 0; }
    return p;
}

/* declarations, constants, labels and blocks */
static int s3_scan(S3Prog *p){
    int stack[64], sp = 0;
    p->main_end = p->n;
    for (int q = 0; q < p->n; q++){
        S3Ins *x = &p->ins[q];
        const S3Opd *d = &x->dst;
        switch (x->op){
        case S3_DCL:
            if (d->type == R_SAMPLER){
                if (d->num >= 16) return s3_fail("a sampler past s15");
                p->smp[d->num] = (int)x->smptype;
            } else if (d->type == R_INPUT){
                if (d->num >= 16) return s3_fail("an input past v15");
                p->vmask[d->num] |= d->mask;
                if (p->model != S3_PS2X){
                    if (p->nin == 16) return s3_fail("too many input declarations");
                    p->din[p->nin++] = (S3Dcl){ d->num, (int)x->usage, (int)x->uindex, d->mask };
                }
            } else if (d->type == R_ADDR && p->model == S3_PS2X){
                if (d->num >= 8) return s3_fail("a texture coordinate past t7");
                p->tmask[d->num] |= d->mask;
            } else if (d->type == R_OUTPUT && p->model == S3_VS3){
                if (d->num >= 12) return s3_fail("an output past o11");
                if (p->nout == 16) return s3_fail("too many output declarations");
                p->dout[p->nout++] = (S3Dcl){ d->num, (int)x->usage, (int)x->uindex, d->mask };
            } else if (d->type == R_MISCTYPE && p->model == S3_PS3){
                if (d->num == 0) p->vpos = 1; else if (d->num == 1) p->vface = 1;
                else return s3_fail("an unknown misc register");
            } else return s3_fail("a declaration of an unknown register");
            break;
        case S3_DEF:
            if (d->type != R_CONST || d->num >= 256) return s3_fail("a def outside c0..c255");
            memcpy(p->def[d->num], x->imm, 16); p->defd[d->num] = 1;
            for (int c = 0; c < 4; c++) if (!isfinite(p->def[d->num][c])) return s3_fail("a def that is not finite");
            break;
        case S3_DEFI:
            if (d->type != R_CONSTINT || d->num >= 16) return s3_fail("a defi outside i0..i15");
            memcpy(p->defi[d->num], x->imm, 16); p->defim |= 1u << d->num;
            break;
        case S3_DEFB:
            if (d->type != R_CONSTBOOL || d->num >= 16) return s3_fail("a defb outside b0..b15");
            if (x->imm[0]) p->defb |= 1u << d->num;
            p->defbm |= 1u << d->num;
            break;
        case S3_LABEL:
            if (x->nsrc != 1 || x->src[0].type != R_LABEL || x->src[0].num >= 2048) return s3_fail("a bad label");
            if (sp) return s3_fail("a label inside a block");
            if (p->main_end == p->n) p->main_end = q;
            p->label_at[x->src[0].num] = q + 1;
            break;
        case S3_IF: case S3_IFC: case S3_REP: case S3_LOOP:
            if (sp == 64) return s3_fail("blocks nested too deep");
            stack[sp++] = q;
            break;
        case S3_ELSE:
            if (!sp || (p->ins[stack[sp - 1]].op != S3_IF && p->ins[stack[sp - 1]].op != S3_IFC))
                return s3_fail("an else without its if");
            p->ins[stack[sp - 1]].match = q;
            stack[sp - 1] = q;
            break;
        case S3_ENDIF: case S3_ENDREP: case S3_ENDLOOP: {
            if (!sp) return s3_fail("a block end without its start");
            int o = stack[--sp];
            unsigned so = p->ins[o].op;
            int want = x->op == S3_ENDIF ? (so == S3_IF || so == S3_IFC || so == S3_ELSE)
                     : x->op == S3_ENDREP ? so == S3_REP : so == S3_LOOP;
            if (!want) return s3_fail("blocks that straddle");
            p->ins[o].match = q;
            x->match = o;
            break; }
        default: break;
        }
    }
    if (sp) return s3_fail("a block left open");
    return 1;
}

static S3Prog *s3_load(const VSProg *p){
    if (!p->raw){ s3_fail("no bytecode kept"); return 0; }
    S3Prog *q = s3_decode(p->raw, p->nraw);
    if (q && !s3_scan(q)){ s3_free(q); return 0; }
    return q;
}

/* ---- emission ---- */
#define S3E 1024                /* the text of one source operand */
typedef struct {                /* working text, on the heap: the game translates on its own thread */
    char a[S3E], b[S3E], c[S3E], d[S3E];   /* sources */
    char pre[3 * S3E], val[4 * S3E];        /* an instruction's statements and value */
    char sv[5 * S3E], blk[9 * S3E], line[10 * S3E];
    char cond[3 * S3E];
} S3Txt;
typedef struct {
    const S3Prog *p;
    S3Txt *t;
    WBuf *b, *h;                /* body; derivatives taken at the top (hoisted) */
    int ind;                    /* indentation of the body */
    int div;                    /* open blocks whose flow may differ per pixel */
    int al[8], nal;             /* the aL variables of the open loops, innermost last */
    int nloop, nhoist;
    int calls[8], ncall;        /* labels being inlined */
    uint32_t r;                 /* temporaries used */
    unsigned vread, tread;      /* inputs read: v#, t# */
    unsigned suse;              /* samplers looked up */
    uint8_t cuse[256];          /* def'd constants read */
    int a0, p0, kill, odepth, kc, vin, orel;
} S3Ctx;

static int s3_line(S3Ctx *x, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int s3_line(S3Ctx *x, const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(x->t->line, sizeof x->t->line, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= sizeof x->t->line) return s3_fail("an expression too long");
    wb_put(x->b, "%*s%s\n", 2 * x->ind + 2, "", x->t->line);
    return x->b->ok ? 1 : s3_fail("the module text is too long");
}

/* the relative index: n + a0.<c> or n + aL */
static int s3_relidx(S3Ctx *x, const S3Opd *o, int n, char *out, size_t cap){
    if (o->rtype == R_LOOP){
        if (!x->nal) return s3_fail("aL outside a loop");
        return s3_pf(out, cap, "aL%d + %d", x->al[x->nal - 1], n);
    }
    if (x->p->model != S3_VS3) return s3_fail("a0 outside a vertex shader");
    x->a0 = 1;
    return s3_pf(out, cap, "a0.%c + %d", "xyzw"[o->rcomp], n);
}

/* a register as a vec4f */
static int s3_reg(S3Ctx *x, const S3Opd *o, char *out, size_t cap){
    const S3Prog *p = x->p;
    int n = o->num;
    char ri[64];
    switch (o->type){
    case R_TEMP:
        if (o->rel || n >= 32) return s3_fail("a temporary past r31");
        x->r |= 1u << n;
        return s3_pf(out, cap, "r%d", n);
    case R_INPUT:
        if (n >= 16 || (p->model == S3_PS2X && n >= 2) || (p->model == S3_PS3 && n >= 10))
            return s3_fail("an input past the model's");
        if (o->rel){
            if (p->model == S3_PS2X) return s3_fail("a relative input in ps_2_x");
            if (!s3_relidx(x, o, n, ri, sizeof ri)) return 0;
            x->vin = 1; x->vread = 0xFFFFu;
            return s3_pf(out, cap, "vin[u32(clamp(%s, 0, %d))]", ri, p->model == S3_PS3 ? 9 : 15);
        }
        if (!p->vmask[n]) return s3_fail("an input that is not declared");
        x->vread |= 1u << n;
        return s3_pf(out, cap, "v%d", n);
    case R_CONST: case R_CONST2: case R_CONST3: case R_CONST4:
        n += o->type == R_CONST2 ? 2048 : o->type == R_CONST3 ? 4096 : o->type == R_CONST4 ? 6144 : 0;
        if (o->rel){
            if (p->model != S3_VS3) return s3_fail("a relative constant in a pixel shader");
            if (!s3_relidx(x, o, n, ri, sizeof ri)) return 0;
            x->kc = 1;
            return s3_pf(out, cap, "kc(kb, %s)", ri);
        }
        if (n < 256 && p->defd[n]){ x->cuse[n] = 1; return s3_pf(out, cap, "c%d", n); }
        if (p->model == S3_VS3){
            if (n >= 256) return s3_fail("a constant past c255");
            return s3_pf(out, cap, "K.c[kb + %du]", n + 1);
        }
        if (n >= 32) return s3_fail("a pixel shader constant past c31 (PSPar holds 32)");
        return s3_pf(out, cap, "P.c[%d]", n);
    case R_ADDR:
        if (p->model == S3_VS3){ x->a0 = 1; return s3_pf(out, cap, "vec4f(a0)"); }
        if (p->model != S3_PS2X || o->rel || n >= 8) return s3_fail("a bad texture coordinate register");
        if (!p->tmask[n]) return s3_fail("a texture coordinate that is not declared");
        if (n >= (int)g_wstages) return s3_fail("texture coordinate 4..7 without 8 stages");
        x->tread |= 1u << n;
        return s3_pf(out, cap, "t%d", n);
    case R_LOOP:
        if (!x->nal) return s3_fail("aL outside a loop");
        return s3_pf(out, cap, "vec4f(f32(aL%d))", x->al[x->nal - 1]);
    case R_MISCTYPE:
        if (n == 0 && p->vpos) return s3_pf(out, cap, "vPos");
        if (n == 1 && p->vface) return s3_pf(out, cap, "vFace");
        return s3_fail("vPos or vFace without its declaration");
    default:
        return s3_fail("a source register this translator does not know");
    }
}

/* a source for the result components comps[0..n-1] (a scalar for n = 1,
 * else vecNf), with its modifier */
static int s3_src(S3Ctx *x, const S3Opd *o, const int *comps, int n, char *out, size_t cap){
    char reg[192], sw[8], body[256];
    if (!s3_reg(x, o, reg, sizeof reg)) return 0;
    int id = n == 4;
    for (int i = 0; i < n; i++){ sw[i] = "xyzw"[o->swz[comps[i]]]; if (o->swz[comps[i]] != comps[i]) id = 0; }
    sw[n] = 0;
    if (!(id ? s3_pf(body, sizeof body, "%s", reg) : s3_pf(body, sizeof body, "%s.%s", reg, sw))) return 0;
    switch (o->mod){
    case S3M_NONE:   return s3_pf(out, cap, "%s", body);
    case S3M_NEG:    return s3_pf(out, cap, "(-%s)", body);
    case S3M_ABS:    return s3_pf(out, cap, "abs(%s)", body);
    case S3M_ABSNEG: return s3_pf(out, cap, "(-abs(%s))", body);
    default:         return s3_fail("a source modifier of ps_1_x (or NOT on a value)");
    }
}
static const int S3_XYZW[4] = { 0, 1, 2, 3 };
/* buf: one of S3Txt's a, b, c, d (S3E bytes) */
#define S3SRC(o, nc, buf) s3_src(x, (o), S3_XYZW, (nc), (buf), S3E)

static const char *s3_vt(int n){ return n == 1 ? "f32" : n == 2 ? "vec2f" : n == 3 ? "vec3f" : "vec4f"; }
static int s3_comps(unsigned mask, int *comps){
    int n = 0;
    for (int c = 0; c < 4; c++) if (mask >> c & 1u) comps[n++] = c;
    return n;
}

/* the predicate of destination component c of a predicated instruction */
static void s3_pc(S3Ctx *x, const S3Ins *y, int c, char *out, size_t cap){
    x->p0 = 1;
    snprintf(out, cap, "%sp0.%c", y->prd.mod == S3M_NOT ? "!" : "", "xyzw"[y->prd.swz[c]]);
}

/* Write a float result to the destination under its mask, _sat and
 * predicate.  kind 0: val has the written components in mask order (a scalar
 * for one); 1: val is a scalar for every one of them; 2: val is a vec4f whose
 * component swz[c] (c without swz) goes to component c.  pre: statements
 * before the value, in the same block. */
static int s3_store(S3Ctx *x, const S3Ins *y, const char *pre, const char *val, int kind, const uint8_t *swz){
    const S3Prog *p = x->p;
    S3Txt *T = x->t;
    const S3Opd *d = &y->dst;
    int comps[4], n = s3_comps(d->mask, comps);
    if (d->shift) return s3_fail("a result shift (ps_1_x only)");
    if (d->mod & ~7u) return s3_fail("an unknown result modifier");
    if (!n) return 1;
    char name[96], ri[64];
    switch (d->type){
    case R_TEMP:
        if (d->rel || d->num >= 32) return s3_fail("a temporary past r31");
        x->r |= 1u << d->num; snprintf(name, sizeof name, "r%d", d->num); break;
    case R_COLOROUT:
        if (p->model == S3_VS3 || d->rel) return s3_fail("a colour output in a vertex shader");
        if (d->num != 0) return s3_fail("MRT: oC1..oC3 (the ABI has one colour target)");
        snprintf(name, sizeof name, "oC0"); break;
    case R_DEPTHOUT:
        if (p->model == S3_VS3) return s3_fail("oDepth in a vertex shader");
        /* .x or .xyzw (the assembler's full mask), as psref takes it; either writes component x */
        if (d->mask != 1u && d->mask != 0xFu) return s3_fail("oDepth with a write mask other than .x or .xyzw");
        x->odepth = 1; snprintf(name, sizeof name, "oDepth"); break;
    case R_OUTPUT:
        if (p->model != S3_VS3) return s3_fail("an output register in a pixel shader");
        if (d->rel){
            if (!s3_relidx(x, d, d->num, ri, sizeof ri)) return 0;
            snprintf(name, sizeof name, "o[u32(clamp(%s, 0, 11))]", ri);
        } else {
            if (d->num >= 12) return s3_fail("an output past o11");
            snprintf(name, sizeof name, x->orel ? "o[%d]" : "o%d", d->num);
        }
        break;
    default: return s3_fail("a destination register this translator does not know");
    }
    if (!((d->mod & S3D_SAT) ? s3_pf(T->sv, sizeof T->sv, "saturate(%s)", val) : s3_pf(T->sv, sizeof T->sv, "%s", val)))
        return 0;
    int ident = !swz || (swz[0] == 0 && swz[1] == 1 && swz[2] == 2 && swz[3] == 3);
    if (d->type == R_DEPTHOUT){
        /* a scalar register: component x of the value (kind 0 with .xyzw: the first of four; kind 2:
         * the component the swizzle puts at x) */
        char e[4] = "";
        if (kind == 2) snprintf(e, sizeof e, ".%c", "xyzw"[swz ? swz[0] & 3u : 0u]);
        else if (kind == 0 && n == 4) snprintf(e, sizeof e, ".x");
        char pc[32] = "";
        if (y->pred) s3_pc(x, y, 0, pc, sizeof pc);
        if (y->pred) return s3_line(x, "{ %s let e = (%s)%s; oDepth = select(oDepth, e, %s); }", pre ? pre : "", T->sv, e, pc);
        if (pre) return s3_line(x, "{ %s oDepth = (%s)%s; }", pre, T->sv, e);
        return s3_line(x, "oDepth = (%s)%s;", T->sv, e);
    }
    if (!y->pred && !pre){
        if (n == 4 && (kind == 0 || (kind == 2 && ident))) return s3_line(x, "%s = %s;", name, T->sv);
        if (n == 4 && kind == 1) return s3_line(x, "%s = vec4f(%s);", name, T->sv);
        if (n == 1 && kind != 2) return s3_line(x, "%s.%c = %s;", name, "xyzw"[comps[0]], T->sv);
    }
    if (!s3_pf(T->blk, sizeof T->blk, "{ %s%slet e = %s;", pre ? pre : "", pre ? " " : "", T->sv)) return 0;
    size_t o = strlen(T->blk);
    if (!y->pred && n == 4 && (kind != 2 || ident))
        return s3_line(x, "%s %s = %s; }", T->blk, name, kind == 1 ? "vec4f(e)" : "e");
    for (int k = 0; k < n; k++){
        int c = comps[k];
        char src[16], pc[32];
        if (kind == 1 || (kind == 0 && n == 1)) snprintf(src, sizeof src, "e");
        else if (kind == 0) snprintf(src, sizeof src, "e.%c", "xyzw"[k]);
        else snprintf(src, sizeof src, "e.%c", "xyzw"[swz ? swz[c] : c]);
        int ok;
        if (y->pred){
            s3_pc(x, y, c, pc, sizeof pc);
            ok = s3_pf(T->blk + o, sizeof T->blk - o, " %s.%c = select(%s.%c, %s, %s);", name, "xyzw"[c], name, "xyzw"[c], src, pc);
        } else ok = s3_pf(T->blk + o, sizeof T->blk - o, " %s.%c = %s;", name, "xyzw"[c], src);
        if (!ok) return 0;
        o += strlen(T->blk + o);
    }
    return s3_line(x, "%s }", T->blk);
}

static const char *s3_cmp(unsigned c){
    switch (c){ case 1: return ">"; case 2: return "=="; case 3: return ">="; case 4: return "<"; case 5: return "!="; case 6: return "<="; }
    return 0;
}

/* a condition: b# (its defb), p0.c (with NOT) */
static int s3_cond(S3Ctx *x, const S3Opd *o, char *out, size_t cap){
    if (o->type == R_CONSTBOOL){
        if (o->num >= 16 || !(x->p->defbm >> o->num & 1u)) return s3_fail("a b# the application sets (the ABI has no boolean constants)");
        int v = (int)(x->p->defb >> o->num & 1u);
        if (o->mod == S3M_NOT) v = !v;
        else if (o->mod) return s3_fail("a modifier on b# other than NOT");
        return s3_pf(out, cap, "%s", v ? "true" : "false");
    }
    if (o->type == R_PREDICATE){
        if (o->mod != S3M_NONE && o->mod != S3M_NOT) return s3_fail("a modifier on p0 other than NOT");
        x->p0 = 1;
        return s3_pf(out, cap, "%sp0.%c", o->mod == S3M_NOT ? "!" : "", "xyzw"[o->swz[0]]);
    }
    return s3_fail("a condition that is not b# or p0");
}
/* if_comp, break_comp: src0.x OP src1.x */
static int s3_cmpcond(S3Ctx *x, const S3Ins *y, char *out, size_t cap){
    const char *op = s3_cmp(y->ctrl & 7u);
    if (!op) return s3_fail("an unknown comparison");
    if (y->nsrc != 2 || !S3SRC(&y->src[0], 1, x->t->a) || !S3SRC(&y->src[1], 1, x->t->b)) return 0;
    return s3_pf(out, cap, "%s %s %s", x->t->a, op, x->t->b);
}

/* a source whose derivatives may be taken at the top of the function: an
 * input or a constant, without relative addressing */
static int s3_hoistable(const S3Prog *p, const S3Opd *o){
    if (o->rel) return 0;
    switch (o->type){
    case R_INPUT: case R_CONST: case R_CONST2: case R_CONST3: case R_CONST4: case R_MISCTYPE: return 1;
    case R_ADDR: return p->model == S3_PS2X;
    default: return 0;
    }
}

/* texld, texldp, texldb, texldl, texldd */
static int s3_tex(S3Ctx *x, const S3Ins *y){
    const S3Prog *p = x->p;
    S3Txt *T = x->t;
    if (y->nsrc < 2 || y->src[1].type != R_SAMPLER || y->src[1].num >= 16) return s3_fail("a lookup without its sampler");
    if (p->model == S3_VS3) return s3_fail("vertex texture fetch (the ABI has no texture in the vertex stage)");
    int s = y->src[1].num, ty = p->smp[s];
    if (ty == 4) return s3_fail("a volume texture");
    if (ty != 2 && ty != 3) return s3_fail("a lookup of an undeclared sampler");
    if (s >= (int)g_wstages) return s3_fail("a sampler of stage 4..7 without 8 stages");
    if (y->src[1].mod) return s3_fail("a modifier on a sampler");
    x->suse |= 1u << s;
    const char *cs = ty == 3 ? "xyz" : "xy";
    int proj = y->op == S3_TEX && (y->ctrl & 3u) == 1, bias = y->op == S3_TEX && (y->ctrl & 3u) == 2;
    if (y->op == S3_TEX && (y->ctrl & 3u) == 3) return s3_fail("an unknown texld variant");
    if (!S3SRC(&y->src[0], 4, T->a) || !s3_pf(T->pre, sizeof T->pre, "let q = %s;", T->a)) return 0;
    char crd[32];
    snprintf(crd, sizeof crd, proj ? "q.%s / q.w" : "q.%s", cs);
    int ok;
    if (y->op == S3_TEXLDL) ok = s3_pf(T->val, sizeof T->val, "textureSampleLevel(tex%d, smp%d, %s, q.w)", s, s, crd);
    else if (y->op == S3_TEXLDD){
        if (y->nsrc != 4 || !S3SRC(&y->src[2], 4, T->b) || !S3SRC(&y->src[3], 4, T->c)) return s3_fail("texldd without its gradients");
        ok = s3_pf(T->val, sizeof T->val, "textureSampleGrad(tex%d, smp%d, %s, (%s).%s, (%s).%s)", s, s, crd, T->b, cs, T->c, cs);
    } else if (!x->div){
        if (bias) ok = s3_pf(T->val, sizeof T->val, "textureSampleBias(tex%d, smp%d, %s, q.w)", s, s, crd);
        else      ok = s3_pf(T->val, sizeof T->val, "textureSample(tex%d, smp%d, %s)", s, s, crd);
    } else {
        /* An implicit level of detail where the flow may differ per pixel:
         * the lookup itself is made at the top of the function, in uniform
         * flow, where its coordinate (an input or a constant) holds the same
         * value, and the block uses the result.  Derivatives taken at the
         * top and a textureSampleGrad in the block are not enough: a shader
         * compiler may compute them again inside the block, where the
         * neighbouring pixels do not run (Chrome on an AMD RDNA 4 GPU gave a
         * cube texldb in an if_comp a level about 6 too high). */
        if (!s3_hoistable(p, &y->src[0]))
            return s3_fail("an implicit-LOD lookup of a temporary in flow that may differ per pixel (D3D9 forbids it)");
        int k = x->nhoist++;
        if (!(proj ? s3_pf(T->d, sizeof T->d, "(%s).%s / (%s).w", T->a, cs, T->a) : s3_pf(T->d, sizeof T->d, "(%s).%s", T->a, cs)))
            return 0;
        if (bias) wb_put(x->h, "  let h%d = textureSampleBias(tex%d, smp%d, %s, (%s).w);\n", k, s, s, T->d, T->a);
        else      wb_put(x->h, "  let h%d = textureSample(tex%d, smp%d, %s);\n", k, s, s, T->d);
        snprintf(T->val, sizeof T->val, "h%d", k);
        return s3_store(x, y, 0, T->val, 2, y->src[1].swz);
    }
    if (!ok) return 0;
    /* the sampler's swizzle applies to the result (always .xyzw in ps_2_x) */
    return s3_store(x, y, T->pre, T->val, 2, y->src[1].swz);
}

/* m4x4, m4x3, m3x4, m3x3, m3x2: the rows are consecutive registers from src1 */
static int s3_matrix(S3Ctx *x, const S3Ins *y){
    S3Txt *T = x->t;
    int rows = (y->op == S3_M4x4 || y->op == S3_M3x4) ? 4 : y->op == S3_M3x2 ? 2 : 3;
    int cols = (y->op == S3_M4x4 || y->op == S3_M4x3) ? 4 : 3;
    if (y->nsrc != 2 || y->src[1].mod || y->src[1].rel) return s3_fail("a matrix with a modifier or relative rows");
    if (!S3SRC(&y->src[0], cols, T->a) || !s3_pf(T->pre, sizeof T->pre, "let a = %s;", T->a)) return 0;
    size_t o = 0;
    if (!s3_pf(T->val, sizeof T->val, "vec4f(")) return 0;
    o = strlen(T->val);
    for (int r = 0; r < 4; r++){
        if (r < rows){
            S3Opd m = y->src[1]; m.num += r;
            if (!S3SRC(&m, cols, T->b) || !s3_pf(T->val + o, sizeof T->val - o, "%sdot(a, %s)", r ? ", " : "", T->b)) return 0;
        } else if (!s3_pf(T->val + o, sizeof T->val - o, ", 0.0")) return 0;
        o += strlen(T->val + o);
    }
    if (!s3_pf(T->val + o, sizeof T->val - o, ")")) return 0;
    return s3_store(x, y, T->pre, T->val, 2, 0);
}

/* one instruction that is not flow control */
static int s3_ins(S3Ctx *x, const S3Ins *y){
    const S3Prog *p = x->p;
    S3Txt *T = x->t;
    int comps[4], n = s3_comps(y->dst.mask, comps);
    const char *vt = s3_vt(n);
    char *A = T->a, *B = T->b, *C = T->c, *V = T->val, *PRE = T->pre;
    const size_t VC = sizeof T->val, PC = sizeof T->pre;
    #define SRCN(k, buf) s3_src(x, &y->src[k], comps, n, (buf), S3E)
    #define NEED(k) do { if (y->nsrc < (k)) return s3_fail("an instruction without its sources"); } while (0)
    switch (y->op){
    case S3_NOP: case S3_DCL: case S3_DEF: case S3_DEFI: case S3_DEFB: return 1;
    case S3_MOV: NEED(1); return SRCN(0, A) && s3_store(x, y, 0, A, 0, 0);
    case S3_ADD: case S3_SUB: case S3_MUL:
        NEED(2);
        return SRCN(0, A) && SRCN(1, B) &&
               s3_pf(V, VC, "%s %c %s", A, y->op == S3_ADD ? '+' : y->op == S3_SUB ? '-' : '*', B) && s3_store(x, y, 0, V, 0, 0);
    case S3_MAD:
        NEED(3);
        return SRCN(0, A) && SRCN(1, B) && SRCN(2, C) && s3_pf(V, VC, "%s * %s + %s", A, B, C) && s3_store(x, y, 0, V, 0, 0);
    case S3_MIN: case S3_MAX:
        NEED(2);
        return SRCN(0, A) && SRCN(1, B) && s3_pf(V, VC, "%s(%s, %s)", y->op == S3_MIN ? "min" : "max", A, B) && s3_store(x, y, 0, V, 0, 0);
    case S3_SLT: case S3_SGE:
        NEED(2);
        return SRCN(0, A) && SRCN(1, B) &&
               s3_pf(V, VC, "select(%s(0.0), %s(1.0), %s %s %s)", vt, vt, A, y->op == S3_SLT ? "<" : ">=", B) && s3_store(x, y, 0, V, 0, 0);
    case S3_FRC: NEED(1); return SRCN(0, A) && s3_pf(V, VC, "fract(%s)", A) && s3_store(x, y, 0, V, 0, 0);
    case S3_ABS: NEED(1); return SRCN(0, A) && s3_pf(V, VC, "abs(%s)", A) && s3_store(x, y, 0, V, 0, 0);
    case S3_SGN: NEED(1); return SRCN(0, A) && s3_pf(V, VC, "sign(%s)", A) && s3_store(x, y, 0, V, 0, 0);
    case S3_LRP:
        NEED(3);
        return SRCN(0, A) && SRCN(1, B) && SRCN(2, C) && s3_pf(PRE, PC, "let l = %s;", C) &&
               s3_pf(V, VC, "%s * (%s - l) + l", A, B) && s3_store(x, y, PRE, V, 0, 0);
    case S3_CMP:
        NEED(3);
        return SRCN(0, A) && SRCN(1, B) && SRCN(2, C) && s3_pf(V, VC, "select(%s, %s, %s >= %s(0.0))", C, B, A, vt) &&
               s3_store(x, y, 0, V, 0, 0);
    case S3_DSX: case S3_DSY: {
        NEED(1);
        const char *fn = y->op == S3_DSX ? "dpdxFine" : "dpdyFine";
        if (!x->div) return SRCN(0, A) && s3_pf(V, VC, "%s(%s)", fn, A) && s3_store(x, y, 0, V, 0, 0);
        if (!s3_hoistable(p, &y->src[0])) return s3_fail("dsx/dsy of a temporary in flow that may differ per pixel (D3D9 forbids it)");
        if (!S3SRC(&y->src[0], 4, T->a)) return 0;
        int k = x->nhoist++;
        wb_put(x->h, "  let gd%d = %s(%s);\n", k, fn, T->a);
        return s3_pf(V, VC, "gd%d", k) && s3_store(x, y, 0, V, 2, 0); }
    case S3_RCP: case S3_RSQ: case S3_LOG: case S3_LOGP:
        NEED(1);
        if (!S3SRC(&y->src[0], 1, A) || !s3_pf(PRE, PC, "let a = %s;", A)) return 0;
        /* of 1 exactly 1, as D3D9 requires (WGSL allows its division 2.5 ulp and inverseSqrt 2) */
        if (!s3_pf(V, VC, y->op == S3_RCP ? "select(select(1.0 / a, 1.0, a == 1.0), " S3_FLTMAX ", a == 0.0)"
                         : y->op == S3_RSQ ? "select(select(inverseSqrt(abs(a)), 1.0, abs(a) == 1.0), " S3_FLTMAX ", a == 0.0)"
                         : "select(log2(abs(a)), -" S3_FLTMAX ", a == 0.0)")) return 0;
        return s3_store(x, y, PRE, V, 1, 0);
    case S3_EXP: case S3_EXPP:
        NEED(1); return S3SRC(&y->src[0], 1, A) && s3_pf(V, VC, "exp2(%s)", A) && s3_store(x, y, 0, V, 1, 0);
    case S3_POW:
        NEED(2);
        return S3SRC(&y->src[0], 1, A) && S3SRC(&y->src[1], 1, B) && s3_pf(V, VC, "pow(abs(%s), %s)", A, B) && s3_store(x, y, 0, V, 1, 0);
    case S3_DP3: case S3_DP4: {
        NEED(2);
        int k = y->op == S3_DP3 ? 3 : 4;
        return S3SRC(&y->src[0], k, A) && S3SRC(&y->src[1], k, B) && s3_pf(V, VC, "dot(%s, %s)", A, B) && s3_store(x, y, 0, V, 1, 0); }
    case S3_DP2ADD:
        NEED(3);
        return S3SRC(&y->src[0], 2, A) && S3SRC(&y->src[1], 2, B) && S3SRC(&y->src[2], 1, C) &&
               s3_pf(V, VC, "dot(%s, %s) + %s", A, B, C) && s3_store(x, y, 0, V, 1, 0);
    case S3_NRM:
        /* every component times 1/sqrt(x^2 + y^2 + z^2); FLT_MAX for a zero vector */
        NEED(1);
        return S3SRC(&y->src[0], 4, A) && s3_pf(PRE, PC, "let s = %s; let d = dot(s.xyz, s.xyz);", A) &&
               s3_pf(V, VC, "s * select(inverseSqrt(d), " S3_FLTMAX ", d == 0.0)") && s3_store(x, y, PRE, V, 2, 0);
    case S3_CRS:
        NEED(2);
        return S3SRC(&y->src[0], 3, A) && S3SRC(&y->src[1], 3, B) && s3_pf(V, VC, "vec4f(cross(%s, %s), 0.0)", A, B) &&
               s3_store(x, y, 0, V, 2, 0);
    case S3_LIT:
        NEED(1);
        return S3SRC(&y->src[0], 4, A) && s3_pf(PRE, PC, "let s = %s;", A) &&
               s3_pf(V, VC, "vec4f(1.0, select(0.0, s.x, s.x > 0.0), select(0.0, pow(s.y, clamp(s.w, -127.9961, 127.9961)),"
                            " s.x > 0.0 && s.y > 0.0), 1.0)") && s3_store(x, y, PRE, V, 2, 0);
    case S3_DST:
        NEED(2);
        return S3SRC(&y->src[0], 4, A) && S3SRC(&y->src[1], 4, B) && s3_pf(PRE, PC, "let s = %s; let t = %s;", A, B) &&
               s3_store(x, y, PRE, "vec4f(1.0, s.y * t.y, s.z, t.w)", 2, 0);
    case S3_SINCOS:
        NEED(1);
        if (y->dst.mask & ~3u) return s3_fail("sincos writing past .xy");
        return S3SRC(&y->src[0], 1, A) && s3_pf(PRE, PC, "let a = %s;", A) &&
               s3_store(x, y, PRE, "vec4f(cos(a), sin(a), 0.0, 0.0)", 2, 0);
    case S3_M4x4: case S3_M4x3: case S3_M3x4: case S3_M3x3: case S3_M3x2:
        return s3_matrix(x, y);
    case S3_TEX: case S3_TEXLDL: case S3_TEXLDD:
        return s3_tex(x, y);
    case S3_MOVA: case S3_SETP: {
        /* a0 (vec4i) or p0 (vec4<bool>) */
        int mova = y->op == S3_MOVA;
        NEED(2 - mova);
        if (mova ? (p->model != S3_VS3 || y->dst.type != R_ADDR) : y->dst.type != R_PREDICATE)
            return s3_fail(mova ? "mova outside a vertex shader" : "setp of a register other than p0");
        if (!n) return 1;
        const char *op = mova ? 0 : s3_cmp(y->ctrl & 7u);
        if (!mova && !op) return s3_fail("an unknown comparison");
        if (!SRCN(0, A) || (!mova && !SRCN(1, B))) return 0;
        const char *it = n == 1 ? "i32" : n == 2 ? "vec2i" : n == 3 ? "vec3i" : "vec4i";
        if (mova){ x->a0 = 1; if (!s3_pf(T->blk, sizeof T->blk, "{ let f = %s; let e = %s(sign(f) * floor(abs(f) + 0.5));", A, it)) return 0; }
        else { x->p0 = 1; if (!s3_pf(T->blk, sizeof T->blk, "{ let e = %s %s %s;", A, op, B)) return 0; }
        const char *reg = mova ? "a0" : "p0";
        size_t o = strlen(T->blk);
        for (int k = 0; k < n; k++){
            char src[8], pc[32];
            snprintf(src, sizeof src, n == 1 ? "e" : "e.%c", "xyzw"[k]);
            int ok;
            if (y->pred){ s3_pc(x, y, comps[k], pc, sizeof pc);
                ok = s3_pf(T->blk + o, sizeof T->blk - o, " %s.%c = select(%s.%c, %s, %s);", reg, "xyzw"[comps[k]], reg, "xyzw"[comps[k]], src, pc); }
            else ok = s3_pf(T->blk + o, sizeof T->blk - o, " %s.%c = %s;", reg, "xyzw"[comps[k]], src);
            if (!ok) return 0;
            o += strlen(T->blk + o);
        }
        return s3_line(x, "%s }", T->blk); }
    case S3_TEXKILL: {
        /* the components of its write mask below zero mark the pixel; it runs on */
        if (y->pred) return s3_fail("a predicated texkill");
        S3Opd r = y->dst; r.mod = 0;
        for (int c = 0; c < 4; c++) r.swz[c] = (uint8_t)c;
        if (r.type != R_TEMP && r.type != R_INPUT && r.type != R_ADDR) return s3_fail("texkill of a register it cannot read");
        if (!n) return 1;
        if (!s3_src(x, &r, comps, n, A, S3E)) return 0;
        x->kill = 1;
        return n == 1 ? s3_line(x, "if (%s < 0.0) { kill = true; }", A) : s3_line(x, "if (any(%s < %s(0.0))) { kill = true; }", A, vt); }
    default:
        return s3_fail("an instruction this translator does not know");
    }
    #undef SRCN
    #undef NEED
}

/* a loop [q, end] that a per-pixel break may leave early: a break_comp or
 * breakp of its own, or a break of its own inside a dynamic if */
static int s3_dynloop(const S3Prog *p, int q, int end){
    int ifs[64], nif = 0, dyn = 0;
    for (int k = q + 1; k < end; k++){
        const S3Ins *y = &p->ins[k];
        switch (y->op){
        case S3_REP: case S3_LOOP: k = y->match; break;    /* an inner loop's breaks leave that loop */
        case S3_BREAKC: case S3_BREAKP: return 1;
        case S3_IF: case S3_IFC: {
            int d = y->op == S3_IFC || (y->nsrc && y->src[0].type == R_PREDICATE);
            if (nif < 64) ifs[nif++] = d;
            dyn += d;
            break; }
        case S3_ENDIF: if (nif) dyn -= ifs[--nif]; break;
        case S3_BREAK: if (dyn) return 1; break;
        default: break;
        }
    }
    return 0;
}

static int s3_range(S3Ctx *x, int from, int to);

/* inline the function at label l (call, callnz) */
static int s3_call(S3Ctx *x, int l){
    const S3Prog *p = x->p;
    if (l < 0 || l >= 2048 || !p->label_at[l]) return s3_fail("a call to an undefined label");
    for (int k = 0; k < x->ncall; k++) if (x->calls[k] == l) return s3_fail("a recursive call");
    if (x->ncall == 8) return s3_fail("calls nested too deep");
    int from = p->label_at[l], to = from;
    /* the function ends at the ret of its own level */
    for (; to < p->n; to++){
        const S3Ins *y = &p->ins[to];
        if (y->op == S3_RET) break;
        if (y->op == S3_LABEL) return s3_fail("a function without its ret");
        if (y->op == S3_IF || y->op == S3_IFC || y->op == S3_REP || y->op == S3_LOOP){
            int e = y->match;
            if (p->ins[e].op == S3_ELSE) e = p->ins[e].match;
            to = e;
        }
    }
    if (to >= p->n) return s3_fail("a function without its ret");
    x->calls[x->ncall++] = l;
    int ok = s3_range(x, from, to);
    x->ncall--;
    return ok;
}

static int s3_range(S3Ctx *x, int from, int to){
    const S3Prog *p = x->p;
    char *c = x->t->cond;
    const size_t cc = sizeof x->t->cond;
    for (int q = from; q < to; q++){
        const S3Ins *y = &p->ins[q];
        switch (y->op){
        case S3_IF: case S3_IFC: {
            int dyn = y->op == S3_IFC || (y->nsrc && y->src[0].type == R_PREDICATE);
            if (y->op == S3_IFC){ if (!s3_cmpcond(x, y, c, cc)) return 0; }
            else if (y->nsrc != 1 || !s3_cond(x, &y->src[0], c, cc)) return 0;
            int m = y->match, e = p->ins[m].op == S3_ELSE ? p->ins[m].match : m;
            if (!s3_line(x, "if (%s) {", c)) return 0;
            x->ind++; x->div += dyn;
            if (!s3_range(x, q + 1, m)) return 0;
            if (m != e){
                x->ind--;
                if (!s3_line(x, "} else {")) return 0;
                x->ind++;
                if (!s3_range(x, m + 1, e)) return 0;
            }
            x->ind--; x->div -= dyn;
            if (!s3_line(x, "}")) return 0;
            q = e;
            break; }
        case S3_REP: case S3_LOOP: {
            const S3Opd *io = &y->src[y->op == S3_REP ? 0 : 1];
            if (y->nsrc != (y->op == S3_REP ? 1 : 2) || io->type != R_CONSTINT || io->num >= 16)
                return s3_fail("a loop without its i#");
            if (!(p->defim >> io->num & 1u)) return s3_fail("a loop count the application sets (the ABI has no integer constants)");
            const int32_t *iv = p->defi[io->num];
            if (iv[0] < 0 || iv[0] > 255) return s3_fail("a loop count outside [0, 255]");
            int dyn = s3_dynloop(p, q, y->match), k = x->nloop++;
            if (y->op == S3_LOOP){
                if (iv[1] < 0 || iv[1] > 255 || iv[2] < -128 || iv[2] > 127 || iv[3] != 0)
                    return s3_fail("aL's start or step outside its range");
                if (x->nal == 8) return s3_fail("loops nested too deep");
                if (!s3_line(x, "var aL%d: i32 = %d;", k, iv[1])) return 0;
                x->al[x->nal++] = k;
            }
            if (!s3_line(x, "for (var lc%d = 0; lc%d < %d; lc%d++) {", k, k, iv[0], k)) return 0;
            x->ind++; x->div += dyn;
            if (!s3_range(x, q + 1, y->match)) return 0;
            if (y->op == S3_LOOP){ if (!s3_line(x, "aL%d += %d;", k, iv[2])) return 0; x->nal--; }
            x->ind--; x->div -= dyn;
            if (!s3_line(x, "}")) return 0;
            q = y->match;
            break; }
        case S3_BREAK: if (!s3_line(x, "break;")) return 0; break;
        case S3_BREAKC: if (!s3_cmpcond(x, y, c, cc) || !s3_line(x, "if (%s) { break; }", c)) return 0; break;
        case S3_BREAKP: if (y->nsrc != 1 || !s3_cond(x, &y->src[0], c, cc) || !s3_line(x, "if (%s) { break; }", c)) return 0; break;
        case S3_CALL:
            if (y->nsrc != 1 || y->src[0].type != R_LABEL) return s3_fail("a bad call");
            if (!s3_call(x, y->src[0].num)) return 0;
            break;
        case S3_CALLNZ: {
            if (y->nsrc != 2 || y->src[0].type != R_LABEL) return s3_fail("a bad callnz");
            if (!s3_cond(x, &y->src[1], c, cc)) return 0;
            int dyn = y->src[1].type == R_PREDICATE;
            if (!s3_line(x, "if (%s) {", c)) return 0;
            x->ind++; x->div += dyn;
            if (!s3_call(x, y->src[0].num)) return 0;
            x->ind--; x->div -= dyn;
            if (!s3_line(x, "}")) return 0;
            break; }
        case S3_RET:
            /* main's own ret ends it (a function's range stops before its ret) */
            if (x->ncall || x->ind) return s3_fail("ret inside a block");
            return 1;
        case S3_ELSE: case S3_ENDIF: case S3_ENDREP: case S3_ENDLOOP: case S3_LABEL:
            return s3_fail("a block end out of place");
        default:
            if (!s3_ins(x, y)) return 0;
            break;
        }
    }
    return 1;
}

/* the VOut field of a ps_3_0 input's declaration */
static int s3_vfield(int usage, int index, char *out, size_t cap){
    if (usage == 10 && index < 2) snprintf(out, cap, "i.col%d", index);                       /* COLOR0, COLOR1 */
    else if (usage == 5 && index < (int)g_wstages) snprintf(out, cap, "i.uv%d", index);       /* TEXCOORDn */
    else if (usage == 11 && index == 0) snprintf(out, cap, "vec4f(i.fog)");                   /* FOG */
    else return 0;
    return 1;
}

/* the binding kinds (GW_BK_*, 2 bits per stage) of the samplers looked up */
static unsigned s3_kinds(const S3Prog *p, unsigned used){
    unsigned k = 0;
    for (int s = 0; s < 8; s++) if ((used >> s & 1u) && p->smp[s] == 3) k |= GW_BK_CUBE << (2 * s);
    return k;
}

static void s3_decls(WBuf *ob, const S3Ctx *x){
    const S3Prog *p = x->p;
    for (int n = 0; n < 256; n++){
        if (!x->cuse[n]) continue;
        char f[4][48];
        for (int c = 0; c < 4; c++) w_float(f[c], sizeof f[c], p->def[n][c]);
        wb_put(ob, "  let c%d = vec4f(%s, %s, %s, %s);\n", n, f[0], f[1], f[2], f[3]);
    }
    for (int n = 0; n < 32; n++) if (x->r >> n & 1u) wb_put(ob, "  var r%d: vec4f;\n", n);
    if (x->a0) wb_put(ob, "  var a0: vec4i;\n");
    if (x->p0) wb_put(ob, "  var p0: vec4<bool>;\n");
}

/* a vertex shader's relative constant read: its own defs first, (0, 0, 0, 0) outside c0..c255 */
static void s3_kc(WBuf *ob, const S3Prog *p){
    wb_put(ob, "fn kc(kb: u32, n: i32) -> vec4f {\n  switch n {\n");
    for (int n = 0; n < 256; n++){
        if (!p->defd[n]) continue;
        char f[4][48];
        for (int c = 0; c < 4; c++) w_float(f[c], sizeof f[c], p->def[n][c]);
        wb_put(ob, "    case %d: { return vec4f(%s, %s, %s, %s); }\n", n, f[0], f[1], f[2], f[3]);
    }
    wb_put(ob, "    default: {}\n  }\n  if (n < 0 || n > 255) { return vec4f(0.0); }\n  return K.c[kb + 1u + u32(n)];\n}\n");
}

static void s3_textures(WBuf *ob, const S3Prog *p, unsigned used){
    for (int s = 0; s < 8; s++){
        if (!(used >> s & 1u)) continue;
        wb_put(ob, "@group(1) @binding(%d) var tex%d: %s;\n", s < 4 ? s : s + 4, s, p->smp[s] == 3 ? "texture_cube<f32>" : "texture_2d<f32>");
        wb_put(ob, "@group(1) @binding(%d) var smp%d: sampler;\n", s < 4 ? s + 4 : s + 8, s);
    }
}

static const char W_PSEND3[] =
    /* ps_3_0: the alpha test, no fixed-function fog */
    "fn ps_end3(c: vec4f) -> vec4f {\n"
    "  if (ALPHA_FUNC != 8u) { if (!alpha_pass(c.a, P.aref)) { discard; } }\n"
    "  return c;\n"
    "}\n";

/* the program, the context and its buffers for one translation */
typedef struct { S3Prog *p; S3Txt *t; char *bb, *hb; WBuf b, h; S3Ctx x; } S3Job;
static int s3_begin(S3Job *j, const VSProg *vp, size_t cap){
    memset(j, 0, sizeof *j);
    s3_why = 0;
    j->p = s3_load(vp);
    if (!j->p) return 0;
    j->t = malloc(sizeof *j->t); j->bb = malloc(cap); j->hb = malloc(cap / 4);
    if (!j->t || !j->bb || !j->hb) return s3_fail("out of memory");
    wb_init(&j->b, j->bb, cap); wb_init(&j->h, j->hb, cap / 4);
    S3Ctx *x = &j->x;
    x->p = j->p; x->t = j->t; x->b = &j->b; x->h = &j->h;
    return 1;
}
static void s3_end(S3Job *j){ free(j->t); free(j->bb); free(j->hb); s3_free(j->p); }

static int ps_to_wgsl_sm3(const VSProg *vp, int id, char *out, size_t cap, unsigned cubemask){
    S3Job j; int rc = 0;
    g_wkinds = 0;
    if (!s3_begin(&j, vp, cap)) goto done;
    S3Prog *p = j.p; S3Ctx *x = &j.x;
    if (p->model == S3_VS3){ s3_fail("a vertex shader given to the pixel translator"); goto done; }
    if (!s3_range(x, 0, p->main_end)) goto done;
    if (!j.b.ok || !j.h.ok){ s3_fail("the module text is too long"); goto done; }
    /* the cube twin (id + 256, for a cube bound at stage 0) is this module
     * when s0 is a cube or not looked up */
    if ((cubemask & 1u) && (x->suse & 1u) && p->smp[0] != 3){ s3_fail("the stage-0 cube twin of a shader whose s0 is 2D"); goto done; }

    WBuf ob; wb_init(&ob, out, cap);
    unsigned kinds = s3_kinds(p, x->suse);
    wb_put(&ob, "// ps_%d: %s, %d instructions%s", id, p->model == S3_PS3 ? "ps_3_0" : "ps_2_x", p->n, w_st8());
    if (kinds) wb_put(&ob, ", kinds 0x%x", kinds);
    wb_put(&ob, "\n%s%s", w_vout(), W_PSPAR);
    if (p->model == S3_PS3) wb_put(&ob, "%s", W_PSEND3);
    if (x->odepth) wb_put(&ob, "struct PSOut { @location(0) c: vec4f, @builtin(frag_depth) z: f32 }\n");
    s3_textures(&ob, p, x->suse);
    wb_put(&ob, "@fragment fn ps_%d(i: VOut%s) -> %s {\n", id, p->vface ? ", @builtin(front_facing) ff: bool" : "",
           x->odepth ? "PSOut" : "@location(0) vec4f");
    for (int n = 0; n < 16; n++){
        if (!(x->vread >> n & 1u) || !p->vmask[n]) continue;
        unsigned m = p->vmask[n];
        if (p->model == S3_PS2X){
            /* a colour outside its declaration reads (0, 0, 0, 1) */
            if (m == 0xFu) wb_put(&ob, "  let v%d = i.col%d;\n", n, n);
            else {
                wb_put(&ob, "  let v%d = vec4f(", n);
                for (int c = 0; c < 4; c++)
                    if (m >> c & 1u) wb_put(&ob, "%si.col%d.%c", c ? ", " : "", n, "xyzw"[c]);
                    else wb_put(&ob, "%s%s", c ? ", " : "", c == 3 ? "1.0" : "0.0");
                wb_put(&ob, ");\n");
            }
            continue;
        }
        /* ps_3_0: from its declarations (several packed with disjoint masks) */
        char f[4][48], fld[32]; int nd = 0, one = -1;
        for (int c = 0; c < 4; c++) snprintf(f[c], sizeof f[c], "0.0");
        for (int k = 0; k < p->nin; k++){
            const S3Dcl *d = &p->din[k];
            if (d->reg != n) continue;
            if (!s3_vfield(d->usage, d->index, fld, sizeof fld)){ s3_fail("a ps_3_0 input the module ABI has no place for"); goto done; }
            nd++; one = k;
            for (int c = 0; c < 4; c++) if (d->mask >> c & 1u) snprintf(f[c], sizeof f[c], "%s.%c", fld, "xyzw"[c]);
        }
        if (nd == 1){ s3_vfield(p->din[one].usage, p->din[one].index, fld, sizeof fld); wb_put(&ob, "  let v%d = %s;\n", n, fld); }
        else wb_put(&ob, "  let v%d = vec4f(%s, %s, %s, %s);\n", n, f[0], f[1], f[2], f[3]);
    }
    if (x->vin){
        wb_put(&ob, "  var vin = array<vec4f, 10>(");
        for (int n = 0; n < 10; n++){
            if (p->vmask[n]) wb_put(&ob, "%sv%d", n ? ", " : "", n);
            else wb_put(&ob, "%svec4f(0.0)", n ? ", " : "");
        }
        wb_put(&ob, ");\n");
    }
    for (int n = 0; n < 8; n++) if (x->tread >> n & 1u) wb_put(&ob, "  let t%d = i.uv%d;\n", n, n);
    if (p->vpos) wb_put(&ob, "  let vPos = vec4f(i.pos.xy - vec2f(0.5), 0.0, 0.0);\n");
    if (p->vface) wb_put(&ob, "  let vFace = vec4f(select(-1.0, 1.0, ff));\n");
    s3_decls(&ob, x);
    wb_put(&ob, "  var oC0: vec4f;\n");
    if (x->odepth) wb_put(&ob, "  var oDepth: f32;\n");
    if (x->kill) wb_put(&ob, "  var kill = false;\n");
    wb_put(&ob, "%s%s", j.hb, j.bb);
    if (x->kill) wb_put(&ob, "  if (kill) { discard; }\n");
    const char *end = p->model == S3_PS3 ? "ps_end3(oC0)" : "ps_end(oC0, i.fog)";
    if (x->odepth) wb_put(&ob, "  var po: PSOut;\n  po.c = %s;\n  po.z = oDepth;\n  return po;\n}\n", end);
    else wb_put(&ob, "  return %s;\n}\n", end);
    if (ob.ok){ rc = (int)ob.n; g_wkinds = kinds; }
    else s3_fail("the module text is too long");
done:
    s3_end(&j);
    return rc;
}

static int vs_to_wgsl_sm3(const VSProg *vp, int id, const int decl[16][2], char *out, size_t cap){
    S3Job j; int rc = 0;
    if (!s3_begin(&j, vp, cap)) goto done;
    S3Prog *p = j.p; S3Ctx *x = &j.x;
    if (p->model != S3_VS3){ s3_fail("a pixel shader given to the vertex translator"); goto done; }
    /* outputs by their declarations, each into a VOut field */
    int havepos = 0;
    for (int k = 0; k < p->nout; k++){
        const S3Dcl *d = &p->dout[k];
        if (d->usage == 0 && d->index == 0) havepos = 1;                        /* POSITION0 */
        else if (d->usage == 10 && d->index < 2) {}                              /* COLOR0, COLOR1 */
        else if (d->usage == 5 && d->index < (int)g_wstages) {}                  /* TEXCOORDn */
        else if (d->usage == 11 && d->index == 0) {}                             /* FOG */
        else if (d->usage == 4) {}                                               /* PSIZE: WebGPU draws 1-pixel points */
        else { s3_fail("a vs_3_0 output the module ABI has no place for"); goto done; }
    }
    if (!havepos){ s3_fail("a vs_3_0 without a POSITION0 output"); goto done; }
    /* relative output writes keep the outputs in an array (known before the body) */
    for (int q = 0; q < p->n; q++) if (p->ins[q].hasdst && p->ins[q].dst.type == R_OUTPUT && p->ins[q].dst.rel) x->orel = 1;
    if (!s3_range(x, 0, p->main_end)) goto done;
    if (!j.b.ok || !j.h.ok){ s3_fail("the module text is too long"); goto done; }

    WBuf ob; wb_init(&ob, out, cap);
    wb_put(&ob, "// vs_%d: vs_3_0, %d instructions, vs_kmax %d%s\n", id, p->n, vs_kmax(vp), w_st8());
    wb_put(&ob, "%s%s%s", w_vout(), W_VSPAR, W_PULL);
    if (x->kc) s3_kc(&ob, p);
    wb_put(&ob, "@vertex fn vs_%d(@builtin(vertex_index) vid: u32, @builtin(instance_index) iid: u32) -> VOut {\n", id);
    wb_put(&ob, "  let kb = iid;\n"
                "  let KH = bitcast<vec4u>(K.c[kb]);\n"
                "  let B = KH.z + vid * KH.x;\n");
    for (int n = 0; n < 16; n++){
        if (!(x->vread >> n & 1u) || !p->vmask[n]) continue;
        char e[256];
        if (decl[n][0] >= 0){
            if (!decl_read_wgsl(e, sizeof e, (unsigned)decl[n][1], (unsigned)decl[n][0])){ s3_fail("a vertex element type the GPU path does not read"); goto done; }
            wb_put(&ob, "  let v%d = %s;\n", n, e);
        } else wb_put(&ob, "  let v%d = vec4f(0.0, 0.0, 0.0, 1.0);\n", n);
    }
    if (x->vin){
        wb_put(&ob, "  var vin = array<vec4f, 16>(");
        for (int n = 0; n < 16; n++){
            if (p->vmask[n]) wb_put(&ob, "%sv%d", n ? ", " : "", n);
            else wb_put(&ob, "%svec4f(0.0, 0.0, 0.0, 1.0)", n ? ", " : "");
        }
        wb_put(&ob, ");\n");
    }
    s3_decls(&ob, x);
    if (x->orel) wb_put(&ob, "  var o: array<vec4f, 12>;\n");
    else for (int k = 0; k < 12; k++){
        int dcl = 0;
        for (int i = 0; i < p->nout; i++) if (p->dout[i].reg == k) dcl = 1;
        if (dcl) wb_put(&ob, "  var o%d: vec4f;\n", k);
    }
    wb_put(&ob, "%s", j.bb);
    /* the declared outputs into VOut; what no declaration covers stays 0 (fog 1: no fog) */
    wb_put(&ob, "  var vo: VOut;\n  vo.fog = 1.0;\n");
    for (int k = 0; k < p->nout; k++){
        const S3Dcl *d = &p->dout[k];
        char reg[16], fld[16];
        snprintf(reg, sizeof reg, x->orel ? "o[%d]" : "o%d", d->reg);
        if (d->usage == 4) continue;
        if (d->usage == 11){
            int c = 0;
            while (c < 3 && !(d->mask >> c & 1u)) c++;
            wb_put(&ob, "  vo.fog = %s.%c;\n", reg, "xyzw"[c]);
            continue;
        }
        if (d->usage == 0) snprintf(fld, sizeof fld, "pos");
        else if (d->usage == 10) snprintf(fld, sizeof fld, "col%d", d->index);
        else snprintf(fld, sizeof fld, "uv%d", d->index);
        if (d->mask == 0xFu) wb_put(&ob, "  vo.%s = %s;\n", fld, reg);
        else for (int c = 0; c < 4; c++) if (d->mask >> c & 1u) wb_put(&ob, "  vo.%s.%c = %s.%c;\n", fld, "xyzw"[c], reg, "xyzw"[c]);
    }
    wb_put(&ob, "  return vo;\n}\n");
    if (ob.ok) rc = (int)ob.n; else s3_fail("the module text is too long");
done:
    s3_end(&j);
    return rc;
}
