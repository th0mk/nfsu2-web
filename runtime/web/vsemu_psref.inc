/* Reference pixel shader interpreter for the WGSL differential test
 * (web/tools/wgsl-xl --pairs, web/src/test/shadercheck.ts).  Native test
 * builds only (-DVSEMU_WGSL); the game does not carry it.
 *
 * It executes the D3D9 bytecode directly with the semantics the Mac's
 * ps_to_msl_ex2 gives it (and therefore the WGSL backend must give it):
 * register masks (r & 15, v & 1, t & 3, c & 31), def constants, source
 * modifiers, destination shift then _sat then write mask, tex/texld coordinate
 * rules with the projective divide, skipped unsupported ops, result r0 or oC,
 * alpha test ">= aref" and vertex fog.  One exception: the ps_2_0 t#
 * registers hold the coordinates, as in D3D (the Mac leaves them at 0).
 * Texture sampling goes through a callback so the test can use the same
 * analytic "texture" on both sides.
 *
 * ps_1_x and ps_2_0 only: ps_2_x and ps_3_0 are refused (return 0).  Their
 * reference is web/tools/psref.c, which follows D3D9 (docs/web/psref.md);
 * this interpreter's shortcuts (r & 15, t & 3, no projective divide in
 * texldp) would agree with a translator that copied them, and 10 of Most
 * Wanted's 18 ps_2_x shaders use r16 and above.
 *
 * With 8 stages (wgsl_set_stages), as the translator: ps_2_0 reads t0..t7
 * and samples s0..s7, each by its declared type (a cube adds to cubemask, a
 * volume is refused); ps_1_x keeps the Mac's four. */
/* PSRefIn, PSRefPar and PSRefTex are declared in wgsl.h. */
typedef struct {
    const VSProg *p; const PSRefPar *P; const PSRefIn *in;
    float r[16][4], t[8][4], v[2][4], oC[4];
    int tm;                                 /* t# and s# mask: 7 (ps_2_0, 8 stages) or 3 */
} PSRefExec;

static void psr_reg(PSRefExec *e, unsigned tok, float o[4]){
    const VSProg *p = e->p;
    int ty = RT(tok), n = RN(tok);
    const float *b = 0; static const float z[4] = { 0, 0, 0, 0 };
    switch (ty){
    case R_TEMP:     b = e->r[n & 15]; break;
    case R_INPUT:    b = e->v[n & 1]; break;
    case R_ADDR:     b = e->t[n & e->tm]; break;
    case R_COLOROUT: b = e->oC; break;
    case R_CONSTINT:
        if (n >= 0 && n < 16){ for (int i = 0; i < 4; i++) o[i] = (float)p->defi[n][i]; return; }
        b = z; break;
    case R_CONST: case R_CONST2: case R_CONST3: case R_CONST4: {
        int idx = n;
        if (ty == R_CONST2) idx += 2048; else if (ty == R_CONST3) idx += 4096;
        else if (ty == R_CONST4) idx += 6144;
        if (idx >= 0 && idx < 256 && (p->defmask[idx >> 5] & (1u << (idx & 31)))) b = p->def[idx];
        else b = e->P->c[idx & 31];
        break; }
    default: b = z; break;
    }
    memcpy(o, b, 16);
}

static void psr_src(PSRefExec *e, unsigned tok, float o[4]){
    float b[4]; psr_reg(e, tok, b);
    unsigned sw = SWIZ(tok);
    for (int i = 0; i < 4; i++) o[i] = b[(sw >> (2*i)) & 3u];
    for (int i = 0; i < 4; i++){
        float x = o[i];
        switch (SMOD(tok)){
        case 1: x = -x; break;            case 2: x = x - 0.5f; break;
        case 3: x = -(x - 0.5f); break;   case 4: x = 2.f*x - 1.f; break;
        case 5: x = -(2.f*x - 1.f); break; case 6: x = 1.f - x; break;
        case 7: x = 2.f*x; break;          case 8: x = -2.f*x; break;
        case 11: x = fabsf(x); break;      case 12: x = -fabsf(x); break;
        default: break;
        }
        o[i] = x;
    }
}

static float psr_sat(float x){ return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

int ps_ref_run(const VSProg *p, const PSRefPar *P, const PSRefIn *in, PSRefTex tex,
               unsigned cubemask, float out[4], int *killed){
    if (p->version > 0x0200) return 0;
    PSRefExec e; memset(&e, 0, sizeof e);
    e.p = p; e.P = P; e.in = in;
    const int tm = e.tm = (wgsl_get_stages() == 8 && p->version >= 0x0200) ? 7 : 3;
    if (tm == 7)
        for (int k = 0; k < 8; k++){
            if (p->smptype[k] == 4) return 0;
            if (p->smptype[k] == 3) cubemask |= 1u << k;
        }
    memcpy(e.v[0], in->col[0], 16); memcpy(e.v[1], in->col[1], 16);
    *killed = 0;
    float tc[8][2];                         /* ps_1_x reads 0..3; 4..7 only keep a bad index in bounds */
    for (int k = 0; k < 8; k++){
        int m = k < 4 ? ((int)P->proj[k]) >> 4 : 0;
        float d = m == 2 ? in->uv[k][3] : (m == 1 ? in->uv[k][2] : 1.f);
        tc[k][0] = in->uv[k][0] / d; tc[k][1] = in->uv[k][1] / d;
    }
    /* ps_2_0: the `dcl tN` coordinate registers hold uvN, as in D3D (the
     * emitter's PS20_TEXCOORD default) */
    if (p->version >= 0x0200)
        for (unsigned q = 0; q < p->n; q++)
            if (p->ins[q].op == O_DCL && RT(p->ins[q].dst) == R_ADDR)
                memcpy(e.t[RN(p->ins[q].dst) & tm], in->uv[RN(p->ins[q].dst) & tm], 16);
    for (unsigned q = 0; q < p->n; q++){
        const VSIns *x = &p->ins[q];
        float s[3][4], d[4] = { 0, 0, 0, 0 };
        for (unsigned i = 0; i < x->nsrc && i < 3; i++) psr_src(&e, x->src[i], s[i]);
        switch (x->op){
        case O_TEX: case O_TEXLDD: case O_TEXLDL: {
            int dn = RN(x->dst) & tm, smp;
            float c[3];
            if (x->nsrc >= 2){ smp = RN(x->src[1]) & tm; c[0] = s[0][0]; c[1] = s[0][1]; c[2] = s[0][2]; }
            else if (x->nsrc == 1 && RT(x->src[0]) == R_ADDR){
                int m = RN(x->src[0]) & 3; smp = dn;
                if ((cubemask >> smp) & 1u){ c[0] = in->uv[m][0]; c[1] = in->uv[m][1]; c[2] = in->uv[m][2]; }
                else { c[0] = tc[m][0]; c[1] = tc[m][1]; c[2] = 0.f; }
            } else if (x->nsrc == 1){ smp = dn; c[0] = s[0][0]; c[1] = s[0][1]; c[2] = s[0][2]; }
            else {
                smp = dn;
                if ((cubemask >> smp) & 1u){ c[0] = in->uv[dn][0]; c[1] = in->uv[dn][1]; c[2] = in->uv[dn][2]; }
                else { c[0] = tc[dn][0]; c[1] = tc[dn][1]; c[2] = 0.f; }
            }
            tex(smp, c, ((cubemask >> smp) & 1u) ? 3 : 2, d);
            break; }
        case O_TEXCOORD: memcpy(e.t[RN(x->dst) & tm], in->uv[RN(x->dst) & tm], 16); continue;
        case O_TEXKILL: {
            /* the register is the destination token (no sources); ps_1_x:
             * the first three components of coordinate set N (tN) or of rN,
             * ps_2_0+: all four (vsemu_wgsl.inc O_TEXKILL) */
            int kty = RT(x->dst);
            float k[4];
            if (kty == R_ADDR && p->version < 0x0200) memcpy(k, in->uv[RN(x->dst) & tm], 16);
            else if (kty == R_TEMP || kty == R_ADDR) psr_reg(&e, x->dst, k);
            else return 0;
            for (int i = 0; i < (p->version >= 0x0200 ? 4 : 3); i++) if (k[i] < 0.f) *killed = 1;
            continue; }
        case O_MOV: memcpy(d, s[0], 16); break;
        case O_ADD: for (int i = 0; i < 4; i++) d[i] = s[0][i] + s[1][i]; break;
        case O_SUB: for (int i = 0; i < 4; i++) d[i] = s[0][i] - s[1][i]; break;
        case O_MUL: for (int i = 0; i < 4; i++) d[i] = s[0][i] * s[1][i]; break;
        case O_MAD: for (int i = 0; i < 4; i++) d[i] = s[0][i] * s[1][i] + s[2][i]; break;
        case O_LRP: for (int i = 0; i < 4; i++) d[i] = s[2][i] + (s[1][i] - s[2][i]) * s[0][i]; break;
        case O_MIN: for (int i = 0; i < 4; i++) d[i] = fminf(s[0][i], s[1][i]); break;
        case O_MAX: for (int i = 0; i < 4; i++) d[i] = fmaxf(s[0][i], s[1][i]); break;
        case O_DP3: { float r = s[0][0]*s[1][0] + s[0][1]*s[1][1] + s[0][2]*s[1][2];
                      for (int i = 0; i < 4; i++) d[i] = r; } break;
        case O_DP4: { float r = s[0][0]*s[1][0] + s[0][1]*s[1][1] + s[0][2]*s[1][2] + s[0][3]*s[1][3];
                      for (int i = 0; i < 4; i++) d[i] = r; } break;
        case O_DP2ADD: { float r = s[0][0]*s[1][0] + s[0][1]*s[1][1] + s[2][0];
                         for (int i = 0; i < 4; i++) d[i] = r; } break;
        case O_RCP: { float r = s[0][0] == 0.f ? 3.402823466e38f : 1.f / s[0][0];
                      for (int i = 0; i < 4; i++) d[i] = r; } break;
        case O_RSQ: { float a = fabsf(s[0][0]), r = a == 0.f ? 3.402823466e38f : 1.f / sqrtf(a);
                      for (int i = 0; i < 4; i++) d[i] = r; } break;
        case O_FRC: for (int i = 0; i < 4; i++) d[i] = s[0][i] - floorf(s[0][i]); break;
        case O_ABS: for (int i = 0; i < 4; i++) d[i] = fabsf(s[0][i]); break;
        case O_CMP: for (int i = 0; i < 4; i++) d[i] = s[0][i] >= 0.f ? s[1][i] : s[2][i]; break;
        case O_CND: for (int i = 0; i < 4; i++) d[i] = s[0][i] > 0.5f ? s[1][i] : s[2][i]; break;
        case O_POW: { float r = powf(fabsf(s[0][0]), s[1][0]); for (int i = 0; i < 4; i++) d[i] = r; } break;
        case O_NRM: { float l = sqrtf(s[0][0]*s[0][0] + s[0][1]*s[0][1] + s[0][2]*s[0][2]);
                      for (int i = 0; i < 3; i++) d[i] = l == 0.f ? 0.f : s[0][i] / l; d[3] = 1.f; } break;
        case O_EXP: { float r = exp2f(s[0][0]); for (int i = 0; i < 4; i++) d[i] = r; } break;
        case O_EXPP: d[0] = exp2f(floorf(s[0][0])); d[1] = s[0][0] - floorf(s[0][0]); d[2] = exp2f(s[0][0]); d[3] = 1.f; break;
        case O_LOGP: { float a = fmaxf(fabsf(s[0][0]), 1e-38f), l = log2f(a);
                       d[0] = floorf(l); d[1] = fabsf(s[0][0]) * exp2f(-floorf(l)); d[2] = l; d[3] = 1.f; } break;
        case O_LOG: { float r = s[0][0] == 0.f ? -3.4e38f : log2f(fabsf(s[0][0])); for (int i = 0; i < 4; i++) d[i] = r; } break;
        default: continue;                      /* skipped, as on the Mac */
        }
        if (!x->has_dst) continue;
        int dty = RT(x->dst), dnum = RN(x->dst);
        float *dst = dty == R_TEMP ? e.r[dnum & 15] : dty == R_INPUT ? e.v[dnum & 1] :
                     dty == R_ADDR ? e.t[dnum & tm] : dty == R_COLOROUT ? e.oC : 0;
        if (!dst) return 0;
        int sh = DSHIFT(x->dst);
        unsigned m = WMASK(x->dst); if (!m) m = 0xFu;
        for (int i = 0; i < 4; i++){
            float val = sh ? d[i] * ldexpf(1.f, sh) : d[i];
            if (SATUR(x->dst)) val = psr_sat(val);
            if (m & (1u << i)) dst[i] = val;
        }
    }
    float res[4];
    memcpy(res, ps_uses_colorout(p) ? e.oC : e.r[0], 16);
    if (res[3] < P->aref) *killed = 1;                 /* ALPHA_FUNC 7: !(a < aref) passes */
    float f = in->fog < 0.f ? 0.f : (in->fog > 1.f ? 1.f : in->fog);
    float w = 1.f + (f - 1.f) * P->fog[3];             /* mix(1, saturate(fog), enable) */
    for (int i = 0; i < 3; i++) out[i] = P->fog[i] + (res[i] - P->fog[i]) * w;
    out[3] = res[3];
    return 1;
}
