/* fxmodel_test.c -- unit tests for runtime/mw/fxmodel.c (the effect model)
 * and fx2.c's state table: docs/mw/fx-model.md §6.1 (conversions,
 * versions, lookups, CTAB, the state table), §6.2 (the pool) and §4.5.
 *
 *   clang -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
 *         -Iruntime/mw -o out/mw/fx/bin/fxmodel_test tools/mw/fxmodel_test.c runtime/mw/fxmodel.c runtime/mw/fx2.c
 *   out/mw/fx/bin/fxmodel_test [--mw] [--states] [--dll <d3dx9_26.dll>] [--conf <game.conf>] [<NAME>.fxo...]
 *
 * Always: the state table against the SDK's enums written below (§4.5 b),
 * and the synthetic effects this file builds in memory (what Most Wanted's
 * effects lack: BOOL, structs, strings, matrices of every shape, every
 * unmatched-constant reason, empty names, SAMPLER[i] = <sampler> pass
 * states, a creation the memory bound refuses with a pool).  With effect
 * files: lookups, the CTAB match,
 * the rows they use, and the pool test over all of them created in byte
 * order of NAME with one pool (every slot, every holder as the writer,
 * every holder reading back), then the lifetime checks.  --mw adds the
 * counts docs/mw/fx-model.md §3.6 and §7 give for the 31 effects and the
 * two pool parameters GAME §5.5 names; those two names are the only ones
 * of Most Wanted written here, everything else is derived from the files.
 *
 * The DLL part (§4.5 a) runs when --dll or the environment variable
 * NFSMW_D3DX names d3dx9_26.dll; otherwise it is skipped and says so.  The
 * file's SHA-256 must equal game.conf's D3DX_SHA256 (--conf, default
 * games/mw/game.conf); the table is found at run time and nothing of the
 * DLL is written here.  --states prints the table, one row per line.
 *
 * Each failure prints where and what; the exit status is 1 when any check
 * failed.
 */
#include "fxmodel.h"
#include "fxtext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long g_checks, g_fails;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)){ g_fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint32_t fb(float x){ uint32_t w; memcpy(&w, &x, 4); return w; }
static void *xalloc(size_t n){ void *p = calloc(1, n ? n : 1); if (!p){ fprintf(stderr, "fxmodel_test: out of memory\n"); exit(1); } return p; }

/* ======================= the state table (§4.5 b) ======================= */

/* d3d9types.h (S): D3DRENDERSTATETYPE, D3DTEXTURESTAGESTATETYPE, D3DSAMPLERSTATETYPE, D3DTS_* */
static const struct { const char *name; uint32_t v; } k_sdk[] = {
    { "D3DRS_ZENABLE", 7 }, { "D3DRS_FILLMODE", 8 }, { "D3DRS_SHADEMODE", 9 }, { "D3DRS_ZWRITEENABLE", 14 },
    { "D3DRS_ALPHATESTENABLE", 15 }, { "D3DRS_LASTPIXEL", 16 }, { "D3DRS_SRCBLEND", 19 }, { "D3DRS_DESTBLEND", 20 },
    { "D3DRS_CULLMODE", 22 }, { "D3DRS_ZFUNC", 23 }, { "D3DRS_ALPHAREF", 24 }, { "D3DRS_ALPHAFUNC", 25 },
    { "D3DRS_DITHERENABLE", 26 }, { "D3DRS_ALPHABLENDENABLE", 27 }, { "D3DRS_FOGENABLE", 28 },
    { "D3DRS_SPECULARENABLE", 29 }, { "D3DRS_FOGCOLOR", 34 }, { "D3DRS_FOGTABLEMODE", 35 }, { "D3DRS_FOGSTART", 36 },
    { "D3DRS_FOGEND", 37 }, { "D3DRS_FOGDENSITY", 38 }, { "D3DRS_RANGEFOGENABLE", 48 }, { "D3DRS_STENCILENABLE", 52 },
    { "D3DRS_STENCILFAIL", 53 }, { "D3DRS_STENCILZFAIL", 54 }, { "D3DRS_STENCILPASS", 55 }, { "D3DRS_STENCILFUNC", 56 },
    { "D3DRS_STENCILREF", 57 }, { "D3DRS_STENCILMASK", 58 }, { "D3DRS_STENCILWRITEMASK", 59 },
    { "D3DRS_TEXTUREFACTOR", 60 }, { "D3DRS_WRAP0", 128 }, { "D3DRS_WRAP1", 129 }, { "D3DRS_WRAP2", 130 },
    { "D3DRS_WRAP3", 131 }, { "D3DRS_WRAP4", 132 }, { "D3DRS_WRAP5", 133 }, { "D3DRS_WRAP6", 134 }, { "D3DRS_WRAP7", 135 },
    { "D3DRS_CLIPPING", 136 }, { "D3DRS_LIGHTING", 137 }, { "D3DRS_AMBIENT", 139 }, { "D3DRS_FOGVERTEXMODE", 140 },
    { "D3DRS_COLORVERTEX", 141 }, { "D3DRS_LOCALVIEWER", 142 }, { "D3DRS_NORMALIZENORMALS", 143 },
    { "D3DRS_DIFFUSEMATERIALSOURCE", 145 }, { "D3DRS_SPECULARMATERIALSOURCE", 146 },
    { "D3DRS_AMBIENTMATERIALSOURCE", 147 }, { "D3DRS_EMISSIVEMATERIALSOURCE", 148 }, { "D3DRS_VERTEXBLEND", 151 },
    { "D3DRS_CLIPPLANEENABLE", 152 }, { "D3DRS_POINTSIZE", 154 }, { "D3DRS_POINTSIZE_MIN", 155 },
    { "D3DRS_POINTSPRITEENABLE", 156 }, { "D3DRS_POINTSCALEENABLE", 157 }, { "D3DRS_POINTSCALE_A", 158 },
    { "D3DRS_POINTSCALE_B", 159 }, { "D3DRS_POINTSCALE_C", 160 }, { "D3DRS_MULTISAMPLEANTIALIAS", 161 },
    { "D3DRS_MULTISAMPLEMASK", 162 }, { "D3DRS_PATCHEDGESTYLE", 163 }, { "D3DRS_DEBUGMONITORTOKEN", 165 },
    { "D3DRS_POINTSIZE_MAX", 166 }, { "D3DRS_INDEXEDVERTEXBLENDENABLE", 167 }, { "D3DRS_COLORWRITEENABLE", 168 },
    { "D3DRS_TWEENFACTOR", 170 }, { "D3DRS_BLENDOP", 171 }, { "D3DRS_POSITIONDEGREE", 172 },
    { "D3DRS_NORMALDEGREE", 173 }, { "D3DRS_SCISSORTESTENABLE", 174 }, { "D3DRS_SLOPESCALEDEPTHBIAS", 175 },
    { "D3DRS_ANTIALIASEDLINEENABLE", 176 }, { "D3DRS_MINTESSELLATIONLEVEL", 178 }, { "D3DRS_MAXTESSELLATIONLEVEL", 179 },
    { "D3DRS_ADAPTIVETESS_X", 180 }, { "D3DRS_ADAPTIVETESS_Y", 181 }, { "D3DRS_ADAPTIVETESS_Z", 182 },
    { "D3DRS_ADAPTIVETESS_W", 183 }, { "D3DRS_ENABLEADAPTIVETESSELLATION", 184 }, { "D3DRS_TWOSIDEDSTENCILMODE", 185 },
    { "D3DRS_CCW_STENCILFAIL", 186 }, { "D3DRS_CCW_STENCILZFAIL", 187 }, { "D3DRS_CCW_STENCILPASS", 188 },
    { "D3DRS_CCW_STENCILFUNC", 189 }, { "D3DRS_COLORWRITEENABLE1", 190 }, { "D3DRS_COLORWRITEENABLE2", 191 },
    { "D3DRS_COLORWRITEENABLE3", 192 }, { "D3DRS_BLENDFACTOR", 193 }, { "D3DRS_SRGBWRITEENABLE", 194 },
    { "D3DRS_DEPTHBIAS", 195 }, { "D3DRS_WRAP8", 198 }, { "D3DRS_WRAP9", 199 }, { "D3DRS_WRAP10", 200 },
    { "D3DRS_WRAP11", 201 }, { "D3DRS_WRAP12", 202 }, { "D3DRS_WRAP13", 203 }, { "D3DRS_WRAP14", 204 },
    { "D3DRS_WRAP15", 205 }, { "D3DRS_SEPARATEALPHABLENDENABLE", 206 }, { "D3DRS_SRCBLENDALPHA", 207 },
    { "D3DRS_DESTBLENDALPHA", 208 }, { "D3DRS_BLENDOPALPHA", 209 },
    { "D3DTSS_COLOROP", 1 }, { "D3DTSS_COLORARG1", 2 }, { "D3DTSS_COLORARG2", 3 }, { "D3DTSS_ALPHAOP", 4 },
    { "D3DTSS_ALPHAARG1", 5 }, { "D3DTSS_ALPHAARG2", 6 }, { "D3DTSS_BUMPENVMAT00", 7 }, { "D3DTSS_BUMPENVMAT01", 8 },
    { "D3DTSS_BUMPENVMAT10", 9 }, { "D3DTSS_BUMPENVMAT11", 10 }, { "D3DTSS_TEXCOORDINDEX", 11 },
    { "D3DTSS_BUMPENVLSCALE", 22 }, { "D3DTSS_BUMPENVLOFFSET", 23 }, { "D3DTSS_TEXTURETRANSFORMFLAGS", 24 },
    { "D3DTSS_COLORARG0", 26 }, { "D3DTSS_ALPHAARG0", 27 }, { "D3DTSS_RESULTARG", 28 }, { "D3DTSS_CONSTANT", 32 },
    { "D3DSAMP_ADDRESSU", 1 }, { "D3DSAMP_ADDRESSV", 2 }, { "D3DSAMP_ADDRESSW", 3 }, { "D3DSAMP_BORDERCOLOR", 4 },
    { "D3DSAMP_MAGFILTER", 5 }, { "D3DSAMP_MINFILTER", 6 }, { "D3DSAMP_MIPFILTER", 7 }, { "D3DSAMP_MIPMAPLODBIAS", 8 },
    { "D3DSAMP_MAXMIPLEVEL", 9 }, { "D3DSAMP_MAXANISOTROPY", 10 }, { "D3DSAMP_SRGBTEXTURE", 11 },
    { "D3DSAMP_ELEMENTINDEX", 12 }, { "D3DSAMP_DMAPOFFSET", 13 },
    { "D3DTS_VIEW", 2 }, { "D3DTS_PROJECTION", 3 }, { "D3DTS_TEXTURE0", 16 }, { "D3DTS_WORLD", 256 } };
/* D3DMATERIAL9 and D3DLIGHT9 in member order (S) */
static const char *const k_material[] = { "DIFFUSE", "AMBIENT", "SPECULAR", "EMISSIVE", "POWER" };
static const char *const k_light[] = { "TYPE", "DIFFUSE", "SPECULAR", "AMBIENT", "POSITION", "DIRECTION", "RANGE",
                                       "FALLOFF", "ATTENUATION0", "ATTENUATION1", "ATTENUATION2", "THETA", "PHI" };

static int sdk(const char *name, uint32_t *v){
    for (size_t i = 0; i < sizeof k_sdk / sizeof k_sdk[0]; i++) if (!strcmp(k_sdk[i].name, name)){ *v = k_sdk[i].v; return 1; }
    return 0;
}
static int member(const char *const *list, size_t n, const char *field, uint32_t *v){
    for (size_t i = 0; i < n; i++) if (!strcmp(list[i], field)){ *v = (uint32_t)i; return 1; }
    return 0;
}
static int prefixed(const char *s, const char *pre){ return !strncmp(s, pre, strlen(pre)); }

/* §4.2's class of each row range, written apart from fx2.c's table */
static uint32_t class_of_row(uint32_t op){
    static const struct { uint32_t last, cls; } r[] = {
        { 102, FX2_SC_RENDER }, { 120, FX2_SC_STAGE }, { 121, FX2_SC_NPATCH }, { 122, FX2_SC_FVF }, { 126, FX2_SC_TRANSFORM },
        { 131, FX2_SC_MATERIAL }, { 145, FX2_SC_LIGHT }, { 146, FX2_SC_VSHADER }, { 147, FX2_SC_PSHADER },
        { 155, FX2_SC_VSCONST }, { 163, FX2_SC_PSCONST }, { 164, FX2_SC_TEXTURE }, { 177, FX2_SC_SAMPLERSTATE },
        { 178, FX2_SC_SAMPLER }, { 179, FX2_SC_STATEBLOCK }, { 180, FX2_SC_PRESHADER_VS }, { 181, FX2_SC_PRESHADER_PS } };
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++) if (op <= r[i].last) return r[i].cls;
    return FX2_NSCLASSES;
}

/* the SDK value of row op by its name (§4.5 b); 0 when the row is unmapped */
static int sdk_row(uint32_t op, uint32_t *v){
    const fx2_state_info *si = fx2_state_get(op);
    char buf[64];
    *v = 0;
    switch (class_of_row(op)){
    case FX2_SC_RENDER: snprintf(buf, sizeof buf, "D3DRS_%s", si->name); return sdk(buf, v);
    case FX2_SC_STAGE: snprintf(buf, sizeof buf, "D3DTSS_%s", si->name); return sdk(buf, v);
    case FX2_SC_SAMPLERSTATE: snprintf(buf, sizeof buf, "D3DSAMP_%s", si->name); return sdk(buf, v);
    case FX2_SC_TRANSFORM: {
        size_t n = strlen(si->name);
        if (n < 10 || strcmp(si->name + n - 9, "TRANSFORM")) return 0;
        snprintf(buf, sizeof buf, "D3DTS_%.*s%s", (int)(n - 9), si->name, !strncmp(si->name, "TEXTURE", 7) ? "0" : "");
        return sdk(buf, v); }
    case FX2_SC_MATERIAL:
        return prefixed(si->name, "MATERIAL") && member(k_material, 5, si->name + 8, v);
    case FX2_SC_LIGHT:
        if (!strcmp(si->name, "LIGHTENABLE")){ *v = 13; return 1; }      /* the DLL's convention (fx-model §4.2) */
        return prefixed(si->name, "LIGHT") && member(k_light, 13, si->name + 5, v);
    case FX2_SC_VSCONST: case FX2_SC_PSCONST: {
        char last = si->name[strlen(si->name) - 1];
        *v = last >= '1' && last <= '4' ? (uint32_t)(last - '0') : 0;
        return 1; }
    default: return 1;                                                  /* no D3D9 number: 0 */
    }
}

static int in_list(uint32_t op, const uint32_t *l, size_t n){ for (size_t i = 0; i < n; i++) if (l[i] == op) return 1; return 0; }

static void test_state_table(void){
    for (uint32_t op = 0; op < FX2_NSTATES; op++){
        const fx2_state_info *si = fx2_state_get(op);
        uint32_t v;
        CHECK(si && si->name && !strcmp(si->name, fx2_state_name(op)), "row %u: name", op);
        CHECK(si->cls == class_of_row(op), "row %u %s: class %u, §4.2 gives %u", op, si->name, si->cls, class_of_row(op));
        CHECK(sdk_row(op, &v), "row %u %s: no SDK value by its name", op, si->name);
        CHECK(si->d3d == v, "row %u %s: d3d %u, the SDK gives %u", op, si->name, si->d3d, v);
        CHECK(fx2_sclass_name(si->cls) != NULL, "row %u: class name", op);
    }
    CHECK(fx2_state_get(FX2_NSTATES) == NULL && fx2_sclass_name(FX2_NSCLASSES) == NULL, "past the tables");
    /* §4.2's value types, sizes and index counts */
    static const uint32_t rfloat[] = { 18, 19, 20, 60, 61, 62, 65, 66, 67, 74, 79, 81, 82, 83, 84, 85, 86, 98 };
    static const uint32_t sfloat[] = { 112, 113, 114, 115, 117, 118 };
    for (uint32_t op = 0; op < FX2_NSTATES; op++){
        const fx2_state_info *si = fx2_state_get(op);
        uint32_t ty = FX2_SV_DWORD, r = 1, c = 1, ni = 1;
        switch (si->cls){
        case FX2_SC_RENDER: if (in_list(op, rfloat, 18)) ty = FX2_T_FLOAT; break;
        case FX2_SC_STAGE: ni = 8; if (in_list(op, sfloat, 6)) ty = FX2_T_FLOAT; break;
        case FX2_SC_NPATCH: ty = FX2_T_FLOAT; break;
        case FX2_SC_TRANSFORM: ty = FX2_T_FLOAT; r = c = 4; ni = op == 125 ? 256 : op == 126 ? 8 : 1; break;
        case FX2_SC_MATERIAL: ty = FX2_T_FLOAT; c = op == 131 ? 1 : 4; break;
        case FX2_SC_LIGHT: ni = 0;
            if (op != 132 && op != 145){ ty = FX2_T_FLOAT; c = op <= 135 ? 4 : op <= 137 ? 3 : 1; }
            break;
        case FX2_SC_VSHADER: case FX2_SC_PRESHADER_VS: ty = FX2_T_VERTEXSHADER; break;
        case FX2_SC_PSHADER: case FX2_SC_PRESHADER_PS: ty = FX2_T_PIXELSHADER; break;
        case FX2_SC_VSCONST: case FX2_SC_PSCONST: {
            uint32_t k = (op - 148) % 8;
            ni = 0; ty = k == 1 ? FX2_T_BOOL : k == 2 ? FX2_T_INT : FX2_T_FLOAT;
            r = k >= 4 ? k - 3 : 0; c = k >= 4 ? 4 : 0;
            break; }
        case FX2_SC_TEXTURE: ty = FX2_T_TEXTURE; ni = 261; break;
        case FX2_SC_SAMPLERSTATE: ni = 261; if (op == 172) ty = FX2_T_FLOAT; break;
        case FX2_SC_SAMPLER: ty = FX2_T_SAMPLER; ni = 261; break;
        case FX2_SC_STATEBLOCK: ty = FX2_SV_STATEBLOCK; break;
        default: break;
        }
        CHECK(si->vtype == ty && si->rows == r && si->cols == c && si->nindex == ni,
              "row %u %s: type 0x%x %ux%u index %u, §4.2 gives 0x%x %ux%u index %u", op, si->name, si->vtype, si->rows,
              si->cols, si->nindex, ty, r, c, ni);
    }
    /* the explicit ones: WRAP8..15 right after WRAP7, no off-by-8 */
    for (uint32_t k = 0; k < 8; k++){
        char w[16];
        snprintf(w, sizeof w, "WRAP%u", k);
        CHECK(!strcmp(fx2_state_get(31 + k)->name, w) && fx2_state_get(31 + k)->d3d == 128 + k, "WRAP%u: row %u, 128 + %u", k, 31 + k, k);
        snprintf(w, sizeof w, "WRAP%u", k + 8);
        CHECK(!strcmp(fx2_state_get(39 + k)->name, w) && fx2_state_get(39 + k)->d3d == 198 + k, "WRAP%u: row %u, 198 + %u", k + 8, 39 + k, k);
    }
    CHECK(!strcmp(fx2_state_get(65)->name, "POINTSCALE_A") && fx2_state_get(65)->d3d == 158, "row 65 is POINTSCALE_A = 158");
    CHECK(fx2_state_get(65)->d3d != 168, "row 65 is not COLORWRITEENABLE (the old kit's off-by-8)");
    CHECK(!strcmp(fx2_state_get(73)->name, "COLORWRITEENABLE") && fx2_state_get(73)->d3d == 168, "row 73 is COLORWRITEENABLE = 168");
    CHECK(fx2_state_get(89)->d3d == 186 && fx2_state_get(102)->d3d == 209, "rows 89 and 102 hold the SDK's 186 and 209");
}

static const char *vtype_name(uint32_t t){
    return t == FX2_SV_DWORD ? "DWORD" : t == FX2_SV_STATEBLOCK ? "STATEBLOCK" : fx2_type_name(t);
}

static void print_states(void){
    for (uint32_t op = 0; op < FX2_NSTATES; op++){
        const fx2_state_info *si = fx2_state_get(op);
        printf("%u %s %s %s %ux%u %u %u\n", op, si->name, fx2_sclass_name(si->cls), vtype_name(si->vtype), si->rows,
               si->cols, si->nindex, si->d3d);
    }
}

/* ---- §4.5 a: against d3dx9_26.dll's own table ---- */

static uint8_t *slurp(const char *path, uint32_t *size){
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0, got;
    do {
        if (n == cap){
            cap = cap ? cap * 2 : 65536;
            uint8_t *b = cap <= 0x7FFFFFFFu ? realloc(buf, cap) : NULL;
            if (!b){ free(buf); fclose(in); return NULL; }
            buf = b;
        }
        got = fread(buf + n, 1, cap - n, in);
        n += got;
    } while (got);
    fclose(in);
    *size = (uint32_t)n;
    return buf;
}

typedef struct { const uint8_t *b; uint32_t n, base, nsec; const uint8_t *sec; } pe;
/* the file offset of n bytes at virtual address va, or -1 */
static int64_t pe_off(const pe *p, uint64_t va, uint32_t n){
    if (va < p->base) return -1;
    uint64_t rva = va - p->base;
    for (uint32_t i = 0; i < p->nsec; i++){
        const uint8_t *s = p->sec + 40 * i;
        uint32_t sva = fx2_rd32(s + 12), raw = fx2_rd32(s + 16), ptr = fx2_rd32(s + 20);
        if (rva >= sva && rva + n <= (uint64_t)sva + raw && (uint64_t)ptr + raw <= p->n) return (int64_t)(ptr + (rva - sva));
    }
    return -1;
}
static int pe_open(pe *p, const uint8_t *b, uint32_t n){
    memset(p, 0, sizeof *p);
    if (n < 0x40) return -1;
    uint32_t lf = fx2_rd32(b + 0x3C);
    if ((uint64_t)lf + 24 > n || memcmp(b + lf, "PE\0\0", 4)) return -1;
    const uint8_t *coff = b + lf + 4, *opt = coff + 20;
    uint32_t optsz = fx2_rd16(coff + 16);
    p->nsec = fx2_rd16(coff + 2);
    if ((uint64_t)lf + 24 + optsz + 40ull * p->nsec > n || optsz < 32 || fx2_rd16(opt) != 0x10B) return -1;
    p->b = b; p->n = n; p->base = fx2_rd32(opt + 28); p->sec = opt + optsz;
    return 0;
}
/* the NUL-terminated string at va, or NULL */
static const char *pe_str(const pe *p, uint32_t va){
    int64_t o = pe_off(p, va, 1);
    if (o < 0) return NULL;
    for (int64_t i = o; i < p->n; i++) if (!p->b[i]) return (const char *)p->b + o;
    return NULL;
}

static int read_conf(const char *path, const char *key, char *out, size_t n){
    FILE *f = fopen(path, "r");
    char line[512];
    size_t kn = strlen(key);
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kn) && line[kn] == '='){
            snprintf(out, n, "%s", line + kn + 1);
            out[strcspn(out, "\r\n")] = 0;
            fclose(f);
            return 1;
        }
    fclose(f);
    return 0;
}

static void test_dll(const char *path, const char *conf){
    uint32_t n;
    uint8_t *b = slurp(path, &n);
    CHECK(b != NULL, "%s: cannot read", path);
    if (!b) return;
    char want[80] = "", got[65];
    sha256_hex(b, n, got);
    CHECK(read_conf(conf, "D3DX_SHA256", want, sizeof want), "%s: no D3DX_SHA256", conf);
    CHECK(!strcmp(want, got), "%s: SHA-256 %s, %s says %s", path, got, conf, want);
    pe p;
    CHECK(!pe_open(&p, b, n), "%s: not a PE32 file", path);
    if (strcmp(want, got) || pe_open(&p, b, n)){ free(b); return; }

    /* the string ZENABLE (NUL on both sides), then every 4-aligned dword holding its VA */
    uint32_t zva = 0;
    for (uint32_t i = 0; i < p.nsec && !zva; i++){
        const uint8_t *s = p.sec + 40 * i;
        uint32_t raw = fx2_rd32(s + 16), ptr = fx2_rd32(s + 20), sva = fx2_rd32(s + 12);
        for (uint32_t k = 0; ptr + (uint64_t)raw <= n && k + 9 <= raw; k++)
            if (!memcmp(b + ptr + k, "\0ZENABLE\0", 9)){ zva = p.base + sva + k + 1; break; }
    }
    CHECK(zva != 0, "no ZENABLE string in the DLL");
    uint32_t copies[8], nc = 0;
    for (uint32_t i = 0; i < p.nsec && zva; i++){
        const uint8_t *s = p.sec + 40 * i;
        uint32_t raw = fx2_rd32(s + 16), ptr = fx2_rd32(s + 20), sva = fx2_rd32(s + 12);
        for (uint32_t k = (4 - sva % 4) % 4; ptr + (uint64_t)raw <= n && k + 4 <= raw; k += 4)
            if (fx2_rd32(b + ptr + k) == zva && nc < 8) copies[nc++] = p.base + sva + k;
    }
    CHECK(nc == 4, "%u copies of the state table, 4 expected", nc);
    if (nc != 4){ free(b); return; }
    const uint8_t *t[4];
    for (int c = 0; c < 4; c++){
        int64_t o = pe_off(&p, copies[c], (FX2_NSTATES + 1) * 32);
        CHECK(o >= 0, "copy %d is not whole in the file", c);
        if (o < 0){ free(b); return; }
        t[c] = b + o;
    }
    for (int c = 1; c < 4; c++)
        for (uint32_t r = 0; r < FX2_NSTATES; r++)
            CHECK(!memcmp(t[0] + 32 * r, t[c] + 32 * r, 28), "copy %d row %u differs in +0..+27", c, r);
    for (int c = 0; c < 4; c++) CHECK(fx2_rd32(t[c] + 32 * FX2_NSTATES) == 0, "copy %d: row 182's name is not 0", c);

    uint32_t dword = 0, sblock = 0, map[FX2_NSCLASSES], inv[256];
    memset(map, 0xFF, sizeof map); memset(inv, 0xFF, sizeof inv);
    static const uint32_t xform[4] = { 3, 2, 256, 16 };       /* the table's transform number to D3DTS_* (W, S) */
    for (uint32_t op = 0; op < FX2_NSTATES; op++){
        const uint8_t *row = t[0] + 32 * op;
        const fx2_state_info *si = fx2_state_get(op);
        const char *nm = pe_str(&p, fx2_rd32(row));
        uint32_t ty = fx2_rd32(row + 4), rows = fx2_rd32(row + 12), cols = fx2_rd32(row + 16), ni = fx2_rd32(row + 20);
        uint32_t cv = fx2_rd32(row + 24), dc = cv >> 24, val = cv & 0xFFFFFF;
        CHECK(nm && !strcmp(nm, si->name), "row %u: the DLL's name is %s, ours %s", op, nm ? nm : "(none)", si->name);
        CHECK(rows == si->rows && cols == si->cols && ni == si->nindex, "row %u %s: the DLL's size %ux%u index %u, ours %ux%u %u",
              op, si->name, rows, cols, ni, si->rows, si->cols, si->nindex);
        if (si->vtype == FX2_SV_DWORD){
            if (!dword) dword = ty;
            CHECK(ty == dword && ty >= 20, "row %u %s: DWORD rows share one code >= 20", op, si->name);
        } else if (si->vtype == FX2_SV_STATEBLOCK){
            sblock = ty;
            CHECK(ty >= 20, "row %u: the state block's code is >= 20", op);
        } else CHECK(ty == si->vtype, "row %u %s: the DLL's type %u, ours %u", op, si->name, ty, si->vtype);
        if (map[si->cls] == 0xFFFFFFFFu) map[si->cls] = dc;
        CHECK(map[si->cls] == dc, "row %u %s: class %s has two DLL codes", op, si->name, fx2_sclass_name(si->cls));
        if (inv[dc & 0xFF] == 0xFFFFFFFFu) inv[dc & 0xFF] = si->cls;
        CHECK(inv[dc & 0xFF] == si->cls, "row %u %s: one DLL class code for two of our classes", op, si->name);
        switch (si->cls){
        case FX2_SC_RENDER: case FX2_SC_STAGE: case FX2_SC_SAMPLERSTATE: case FX2_SC_MATERIAL: case FX2_SC_LIGHT:
            if (op == 89) CHECK(val == 53 && si->d3d == 186, "row 89: the DLL holds 53 and we 186");
            else if (op == 102) CHECK(val == 171 && si->d3d == 209, "row 102: the DLL holds 171 and we 209");
            else CHECK(val == si->d3d, "row %u %s: the DLL's value %u, ours %u", op, si->name, val, si->d3d);
            break;
        case FX2_SC_TRANSFORM:
            CHECK(val < 4 && xform[val] == si->d3d, "row %u %s: transform %u", op, si->name, val);
            break;
        case FX2_SC_VSCONST: case FX2_SC_PSCONST:
            CHECK((val & 0xFFFF) == si->d3d, "row %u %s: register count %u, ours %u", op, si->name, val & 0xFFFF, si->d3d);
            CHECK(((val >> 16 & 0xFF) != 0) == (si->vtype == FX2_T_BOOL || si->vtype == FX2_T_INT),
                  "row %u %s: the B/I mark", op, si->name);
            break;
        default:
            CHECK(val == 0, "row %u %s: the DLL's value %u, 0 expected", op, si->name, val);
        }
    }
    CHECK(sblock && sblock != dword, "the state block's code differs from the DWORD code");
    printf("dll: %s matches game.conf; 4 copies of the 182-row table, every row agrees (89 and 102 hold 53 and 171)\n", path);
    free(b);
}

/* ======================= synthetic effects (F§2) ======================= */

typedef struct { uint8_t *p; uint32_t n, cap; } bb;
static void bb_put(bb *b, const void *s, uint32_t n){
    if (b->n + n > b->cap){ b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); if (!b->p) exit(1); }
    memcpy(b->p + b->n, s, n); b->n += n;
}
static void bb_u32(bb *b, uint32_t v){ uint8_t q[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; bb_put(b, q, 4); }
static void bb_u16(bb *b, uint32_t v){ uint8_t q[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; bb_put(b, q, 2); }
static void bb_pad(bb *b, uint8_t with){ while (b->n % 4) bb_put(b, &with, 1); }
static void bb_set32(bb *b, uint32_t at, uint32_t v){ b->p[at] = (uint8_t)v; b->p[at + 1] = (uint8_t)(v >> 8); b->p[at + 2] = (uint8_t)(v >> 16); b->p[at + 3] = (uint8_t)(v >> 24); }

typedef struct ts {               /* a typedef to write */
    uint32_t type, cls;
    const char *name, *sem;
    uint32_t elements, rows, cols, nm;
    const struct ts *m;
} ts;

typedef struct { bb d, t; } fxb;  /* the data area (from body +0) and the table area */
static void fxb_init(fxb *x){ memset(x, 0, sizeof *x); bb_u32(&x->d, 0); }
static uint32_t fxb_name(fxb *x, const char *s){
    if (!s) return 0;
    uint32_t off = x->d.n, n = (uint32_t)strlen(s) + 1;
    bb_u32(&x->d, n); bb_put(&x->d, s, n); bb_pad(&x->d, 0xA5);   /* pad bytes are not zero (F§1) */
    return off;
}
static uint32_t fxb_dw(fxb *x, const uint32_t *v, uint32_t n){ uint32_t off = x->d.n; for (uint32_t i = 0; i < n; i++) bb_u32(&x->d, v[i]); return off; }
static void tnames(fxb *x, const ts *t, uint32_t *o, uint32_t *k){
    o[(*k)++] = fxb_name(x, t->name); o[(*k)++] = fxb_name(x, t->sem);
    for (uint32_t m = 0; m < t->nm; m++) tnames(x, &t->m[m], o, k);
}
static void tbody(fxb *x, const ts *t, const uint32_t *o, uint32_t *k){
    bb_u32(&x->d, t->type); bb_u32(&x->d, t->cls); bb_u32(&x->d, o[(*k)++]); bb_u32(&x->d, o[(*k)++]); bb_u32(&x->d, t->elements);
    if (t->cls == FX2_C_VECTOR){ bb_u32(&x->d, t->cols); bb_u32(&x->d, t->rows); }
    else if (t->cls <= FX2_C_MATRIX_COLUMNS){ bb_u32(&x->d, t->rows); bb_u32(&x->d, t->cols); }
    else if (t->cls == FX2_C_STRUCT){ bb_u32(&x->d, t->nm); for (uint32_t m = 0; m < t->nm; m++) tbody(x, &t->m[m], o, k); }
}
/* a typedef: its names first, then its body with the member typedefs right after it */
static uint32_t fxb_type(fxb *x, const ts *t){
    uint32_t o[128], k = 0;
    tnames(x, t, o, &k);
    uint32_t off = x->d.n;
    k = 0;
    tbody(x, t, o, &k);
    return off;
}
static uint8_t *fxb_file(fxb *x, uint32_t *size){
    *size = 8 + x->d.n + x->t.n;
    uint8_t *f = xalloc(*size);
    f[0] = 0x01; f[1] = 0x09; f[2] = 0xFF; f[3] = 0xFE;
    f[4] = (uint8_t)x->d.n; f[5] = (uint8_t)(x->d.n >> 8); f[6] = (uint8_t)(x->d.n >> 16); f[7] = (uint8_t)(x->d.n >> 24);
    memcpy(f + 8, x->d.p, x->d.n); memcpy(f + 8 + x->d.n, x->t.p, x->t.n);
    free(x->d.p); free(x->t.p);
    return f;
}

/* one CTAB entry to write */
typedef struct { const char *name; uint32_t set, reg, count, cls, type, rows, cols, elements, members; int outside; } cs;

/* a CTAB comment (token, FOURCC, payload) into o: header, type infos, strings, then
 * the entries last, so that one entry more than written (extra) lies outside */
static void ctab_comment(bb *o, uint32_t version, const char *target, const cs *c, uint32_t n, uint32_t extra){
    bb pl = { 0 };
    for (int i = 0; i < 28; i++){ uint8_t z = 0; bb_put(&pl, &z, 1); }
    uint32_t ti = pl.n;
    for (uint32_t k = 0; k < n; k++){
        bb_u16(&pl, c[k].cls); bb_u16(&pl, c[k].type); bb_u16(&pl, c[k].rows); bb_u16(&pl, c[k].cols);
        bb_u16(&pl, c[k].elements); bb_u16(&pl, c[k].members); bb_u32(&pl, 0);
    }
    uint32_t creator = pl.n;
    bb_put(&pl, "fxmodel_test", 13);
    uint32_t tgt = pl.n;
    bb_put(&pl, target, (uint32_t)strlen(target) + 1);
    uint32_t names[16];
    for (uint32_t k = 0; k < n; k++){ names[k] = c[k].name ? pl.n : 0; if (c[k].name) bb_put(&pl, c[k].name, (uint32_t)strlen(c[k].name) + 1); }
    bb_pad(&pl, 0);
    uint32_t ci = pl.n;
    for (uint32_t k = 0; k < n; k++){
        bb_u32(&pl, names[k]); bb_u16(&pl, c[k].set); bb_u16(&pl, c[k].reg); bb_u16(&pl, c[k].count); bb_u16(&pl, 0);
        bb_u32(&pl, c[k].outside ? 0xFFFF0 : ti + 16 * k); bb_u32(&pl, 0);
    }
    bb_set32(&pl, 0, 28); bb_set32(&pl, 4, creator); bb_set32(&pl, 8, version); bb_set32(&pl, 12, n + extra);
    bb_set32(&pl, 16, ci); bb_set32(&pl, 20, 0x100); bb_set32(&pl, 24, tgt);
    bb_u32(o, 0xFFFE | (1 + pl.n / 4) << 16);
    bb_u32(o, FX2_CTAB);
    bb_put(o, pl.p, pl.n);
    free(pl.p);
}
static void shader_blob(bb *o, uint32_t version, const char *target, const cs *c, uint32_t n, uint32_t extra){
    bb_u32(o, version);
    ctab_comment(o, version, target, c, n, extra);
    bb_u32(o, FX2_END);
}
static void pres_blob(bb *o, const cs *c, uint32_t n){
    bb_u32(o, FX2_PRES_VERSION);
    ctab_comment(o, FX2_PRES_VERSION, "tx", c, n, 0);
    bb_u32(o, 0xFFFE | 2 << 16); bb_u32(o, FX2_CLIT); bb_u32(o, 0);                         /* no literals */
    bb_u32(o, 0xFFFE | 4 << 16); bb_u32(o, FX2_FXLC); bb_u32(o, 0); bb_u32(o, 0xF0F0F0F0u); bb_u32(o, 0x0F0F0F0Fu);
    bb_u32(o, FX2_END);
}
static void res_entry(fxb *x, uint32_t tech, uint32_t index, uint32_t element, uint32_t state, uint32_t usage, const void *data, uint32_t size){
    bb_u32(&x->t, tech); bb_u32(&x->t, index); bb_u32(&x->t, element); bb_u32(&x->t, state); bb_u32(&x->t, usage);
    bb_u32(&x->t, size); bb_put(&x->t, data, size); bb_pad(&x->t, 0x5A);
}
/* a state entry: operation, index and a literal of type t with value v (n dwords) */
static void state_entry(fxb *x, bb *to, uint32_t op, uint32_t index, const ts *t, const uint32_t *v, uint32_t n){
    uint32_t toff = fxb_type(x, t), voff = fxb_dw(x, v, n);
    bb_u32(to, op); bb_u32(to, index); bb_u32(to, toff); bb_u32(to, voff);
}

#define T_(ty, cl, nm, sm, el, r, c) { FX2_T_##ty, FX2_C_##cl, nm, sm, el, r, c, 0, NULL }
static const ts k_int1 = T_(INT, MATRIX_ROWS, NULL, NULL, 0, 1, 1);

/* ---- the conversion effect: one parameter of each shape (indices below) ---- */
enum { P_B, P_I, P_F, P_V2, P_V3, P_V4, P_M33, P_M43, P_M44, P_MC44, P_I5, P_V4A, P_M44A, P_ST, P_SA, P_TEX0, P_TEXA,
       P_SMP, P_SHI, P_C31, P_STR, P_BV4, P_NP };

static const ts k_st_m[] = { T_(FLOAT, VECTOR, "pos", "POSITION", 0, 1, 3), T_(INT, SCALAR, "n", NULL, 0, 1, 1),
                             T_(BOOL, SCALAR, "flags", NULL, 2, 1, 1) };
static const ts k_sa_m[] = { T_(FLOAT, SCALAR, "x", NULL, 0, 1, 1), T_(TEXTURE, OBJECT, "tex", NULL, 0, 0, 0) };
static const ts k_conv_t[P_NP] = {
    T_(BOOL, SCALAR, "b", NULL, 0, 1, 1), T_(INT, SCALAR, "i", "INDEX", 0, 1, 1), T_(FLOAT, SCALAR, "f", "Factor", 0, 1, 1),
    T_(FLOAT, VECTOR, "v2", NULL, 0, 1, 2), T_(FLOAT, VECTOR, "v3", NULL, 0, 1, 3), T_(FLOAT, VECTOR, "v4", NULL, 0, 1, 4),
    T_(FLOAT, MATRIX_ROWS, "m33", NULL, 0, 3, 3), T_(FLOAT, MATRIX_ROWS, "m43", NULL, 0, 4, 3),
    T_(FLOAT, MATRIX_ROWS, "m44", "XFORM", 0, 4, 4), T_(FLOAT, MATRIX_COLUMNS, "mc44", NULL, 0, 4, 4),
    T_(INT, SCALAR, "i5", NULL, 5, 1, 1), T_(FLOAT, VECTOR, "v4a", NULL, 3, 1, 4), T_(FLOAT, MATRIX_ROWS, "m44a", NULL, 2, 4, 4),
    { FX2_T_VOID, FX2_C_STRUCT, "st", NULL, 0, 0, 0, 3, k_st_m }, { FX2_T_VOID, FX2_C_STRUCT, "sa", NULL, 2, 0, 0, 2, k_sa_m },
    T_(TEXTURE, OBJECT, "albedo_tex", "ALBEDO", 0, 0, 0), T_(TEXTURE, OBJECT, "texa", NULL, 2, 0, 0),
    T_(SAMPLER, OBJECT, "smp", NULL, 0, 0, 0), T_(INT, SCALAR, "shared_i", NULL, 0, 1, 1),
    T_(FLOAT, MATRIX_ROWS, "c31", NULL, 0, 3, 1), T_(STRING, OBJECT, "str", NULL, 0, 0, 0), T_(BOOL, VECTOR, "bv4", NULL, 0, 1, 4) };

/* objects: 1 albedo_tex, 2-3 texa, 4-5 sa[].tex, 6 str, 7 the string annotation, 8 the sampler's
 * TEXTURE state, 9 the vertex shader, 10 the pixel shader, 11 the pass's TEXTURE[0] */
#define CONV_OBJECTS 12
static uint8_t *make_conv(uint32_t *size){
    fxb x;
    fxb_init(&x);
    uint32_t toff[P_NP], voff[P_NP];
    static const uint32_t z16[16];
    uint32_t def[64];
    for (int p = 0; p < P_NP; p++){
        toff[p] = fxb_type(&x, &k_conv_t[p]);
        switch (p){
        case P_I: def[0] = 7; voff[p] = fxb_dw(&x, def, 1); break;
        case P_F: def[0] = fb(1.5f); voff[p] = fxb_dw(&x, def, 1); break;
        case P_V2: def[0] = fb(1); def[1] = fb(2); voff[p] = fxb_dw(&x, def, 2); break;
        case P_V3: voff[p] = fxb_dw(&x, z16, 3); break;
        case P_V4: voff[p] = fxb_dw(&x, z16, 4); break;
        case P_M33: voff[p] = fxb_dw(&x, z16, 9); break;
        case P_M43: voff[p] = fxb_dw(&x, z16, 12); break;
        case P_M44: case P_MC44: voff[p] = fxb_dw(&x, z16, 16); break;
        case P_I5: for (int k = 0; k < 5; k++) def[k] = (uint32_t)k + 1; voff[p] = fxb_dw(&x, def, 5); break;
        case P_V4A: for (int k = 0; k < 12; k++) def[k] = fb((float)k); voff[p] = fxb_dw(&x, def, 12); break;
        case P_M44A: voff[p] = fxb_dw(&x, z16, 16); fxb_dw(&x, z16, 16); break;
        case P_ST: def[0] = fb(1); def[1] = fb(2); def[2] = fb(3); def[3] = 9; def[4] = 1; def[5] = 0; voff[p] = fxb_dw(&x, def, 6); break;
        case P_SA: def[0] = fb(0.25f); def[1] = 4; def[2] = fb(0.5f); def[3] = 5; voff[p] = fxb_dw(&x, def, 4); break;
        case P_TEX0: def[0] = 1; voff[p] = fxb_dw(&x, def, 1); break;
        case P_TEXA: def[0] = 2; def[1] = 3; voff[p] = fxb_dw(&x, def, 2); break;
        case P_SMP: {                                         /* a sampler_state block: TEXTURE = <albedo_tex>, MINFILTER = 2 */
            static const ts tex = T_(TEXTURE, OBJECT, NULL, NULL, 0, 0, 0);
            bb st = { 0 };
            uint32_t eight = 8, two = 2;
            state_entry(&x, &st, 164, 256, &tex, &eight, 1);
            state_entry(&x, &st, 170, 256, &k_int1, &two, 1);
            voff[p] = x.d.n;
            bb_u32(&x.d, 2); bb_put(&x.d, st.p, st.n);
            free(st.p);
            break; }
        case P_SHI: def[0] = 3; voff[p] = fxb_dw(&x, def, 1); break;
        case P_C31: voff[p] = fxb_dw(&x, z16, 3); break;
        case P_STR: def[0] = 6; voff[p] = fxb_dw(&x, def, 1); break;
        case P_BV4: voff[p] = fxb_dw(&x, z16, 4); break;
        default: voff[p] = fxb_dw(&x, z16, 1);
        }
    }
    /* annotations of parameter b: a string, an INT and a FLOAT[2] */
    static const ts a_str = T_(STRING, OBJECT, "Name", NULL, 0, 0, 0), a_int = T_(INT, SCALAR, "level", NULL, 0, 1, 1),
                    a_f2 = T_(FLOAT, SCALAR, "range", NULL, 2, 1, 1);
    uint32_t seven = 7, three = 3, f2[2] = { fb(-1), fb(2.5f) }, two = 2;
    uint32_t a0t = fxb_type(&x, &a_str), a0v = fxb_dw(&x, &seven, 1), a1t = fxb_type(&x, &a_int), a1v = fxb_dw(&x, &three, 1);
    uint32_t a2t = fxb_type(&x, &a_f2), a2v = fxb_dw(&x, f2, 2);
    uint32_t t0n = fxb_name(&x, "t0"), p0n = fxb_name(&x, "p0"), tat = fxb_type(&x, &a_int), tav = fxb_dw(&x, &two, 1);
    /* the pass's states (their typedefs and values in the data area) */
    static const ts vsh = T_(VERTEXSHADER, OBJECT, NULL, NULL, 0, 0, 0), psh = T_(PIXELSHADER, OBJECT, NULL, NULL, 0, 0, 0),
                    tex = T_(TEXTURE, OBJECT, NULL, NULL, 0, 0, 0);
    bb ps = { 0 };
    uint32_t nine = 9, ten = 10, zero = 0, one = 1, eleven = 11;
    state_entry(&x, &ps, 146, 0, &vsh, &nine, 1);
    state_entry(&x, &ps, 147, 0, &psh, &ten, 1);
    state_entry(&x, &ps, 13, 0, &k_int1, &zero, 1);
    state_entry(&x, &ps, 164, 0, &tex, &eleven, 1);
    state_entry(&x, &ps, 0, 0, &k_int1, &one, 1);

    bb *t = &x.t;
    bb_u32(t, P_NP); bb_u32(t, 1); bb_u32(t, 3); bb_u32(t, CONV_OBJECTS);
    for (int p = 0; p < P_NP; p++){
        bb_u32(t, toff[p]); bb_u32(t, voff[p]); bb_u32(t, p == P_SHI ? 1 : 0); bb_u32(t, p == P_B ? 3 : 0);
        if (p == P_B){ bb_u32(t, a0t); bb_u32(t, a0v); bb_u32(t, a1t); bb_u32(t, a1v); bb_u32(t, a2t); bb_u32(t, a2v); }
    }
    bb_u32(t, t0n); bb_u32(t, 1); bb_u32(t, 1); bb_u32(t, tat); bb_u32(t, tav);
    bb_u32(t, p0n); bb_u32(t, 0); bb_u32(t, 5); bb_put(t, ps.p, ps.n);
    free(ps.p);
    bb_u32(t, 2); bb_u32(t, 5);                                 /* object data, resources */
    bb_u32(t, 6); bb_u32(t, 6); bb_put(t, "hello", 6); bb_pad(t, 0x77);
    bb_u32(t, 7); bb_u32(t, 5); bb_put(t, "text", 5); bb_pad(t, 0x77);
    /* the vertex shader's constants: three that match, then every reason, then one entry outside (6) */
    static const cs vsc[] = {
        { "m44", 2, 0, 4, FX2_C_MATRIX_COLUMNS, FX2_T_FLOAT, 4, 4, 1, 0, 0 },
        { "v4", 2, 4, 1, FX2_C_VECTOR, FX2_T_FLOAT, 1, 4, 1, 0, 0 },
        { "v4a", 2, 5, 2, FX2_C_VECTOR, FX2_T_FLOAT, 1, 4, 3, 0, 0 },
        { "smp", 3, 1, 1, FX2_C_OBJECT, FX2_T_SAMPLER2D, 1, 1, 1, 0, 0 },
        { "nope", 2, 9, 1, FX2_C_SCALAR, FX2_T_FLOAT, 1, 1, 1, 0, 0 },           /* 1: no such parameter */
        { "i5[1]", 2, 9, 1, FX2_C_SCALAR, FX2_T_INT, 1, 1, 1, 0, 0 },            /* 1: an element, not top level */
        { "smp", 2, 9, 1, FX2_C_SCALAR, FX2_T_FLOAT, 1, 1, 1, 0, 0 },            /* 2: a sampler under class SCALAR (§8) */
        { "v3", 2, 9, 1, FX2_C_VECTOR, FX2_T_FLOAT, 1, 4, 1, 0, 0 },             /* 3: 1x4 against 1x3 */
        { "i5", 2, 9, 4, FX2_C_SCALAR, FX2_T_INT, 1, 1, 4, 0, 0 },               /* 4: 4 elements against 5 */
        { "f", 2, 9, 1, FX2_C_SCALAR, FX2_T_FLOAT, 1, 1, 1, 0, 1 },              /* 5: type info outside */
        { "st", 2, 9, 3, FX2_C_STRUCT, FX2_T_VOID, 1, 1, 1, 3, 0 },              /* 7: a struct */
        { NULL, 2, 9, 1, FX2_C_SCALAR, FX2_T_FLOAT, 1, 1, 1, 0, 0 } };           /* 1: no name */
    /* the pixel shader's: W's element and class rules (fx-model §3.2 steps 4 and 5).  Reason 2 for a sampler
     * under a class other than OBJECT is the model's (W takes it as an input without registers and applies its
     * states there, §3.2 and §8); a numeric parameter under class OBJECT W refuses */
    static const cs psc[] = { { "smp", 3, 0, 1, FX2_C_OBJECT, FX2_T_SAMPLER2D, 1, 1, 1, 0, 0 },
        { "smp", 3, 1, 1, FX2_C_OBJECT, FX2_T_SAMPLER2D, 1, 1, 4, 0, 0 },   /* 0: no element check on a sampler */
        { "smp", 3, 2, 1, FX2_C_SCALAR, FX2_T_SAMPLER2D, 1, 1, 1, 0, 0 },   /* 2: set s, class SCALAR (§8) */
        { "f", 2, 3, 1, FX2_C_SCALAR, FX2_T_FLOAT, 1, 1, 0, 0, 0 },         /* 0: a non-array takes Elements 0 */
        { "f", 2, 4, 1, FX2_C_SCALAR, FX2_T_FLOAT, 1, 1, 2, 0, 0 },         /* 4: ... but not 2 */
        { "f", 2, 5, 1, FX2_C_OBJECT, FX2_T_TEXTURE, 1, 1, 1, 0, 0 },       /* 2: a numeric parameter, class OBJECT */
        { "f", 2, 6, 1, FX2_C_SCALAR, FX2_T_FLOAT, 1, 1, 1, 2, 0 },         /* 4: StructMembers 2 on a numeric one */
        { "v4a", 2, 7, 3, FX2_C_VECTOR, FX2_T_FLOAT, 1, 4, 3, 1, 0 } };     /* 4: the same on an array (each element) */
    static const cs prc[] = { { "i5", 2, 0, 2, FX2_C_SCALAR, FX2_T_INT, 1, 1, 5, 0, 0 },
                              { "smp", 3, 0, 1, FX2_C_OBJECT, FX2_T_SAMPLER2D, 1, 1, 1, 0, 0 } };   /* 2: set s in a preshader (§8) */
    bb blob = { 0 };
    shader_blob(&blob, 0xFFFE0101u, "vs_1_1", vsc, sizeof vsc / sizeof vsc[0], 1);
    res_entry(&x, 0, 0, FX2_NONE, 0, 0, blob.p, blob.n); blob.n = 0;
    shader_blob(&blob, 0xFFFF0200u, "ps_2_0", psc, sizeof psc / sizeof psc[0], 0);
    res_entry(&x, 0, 0, FX2_NONE, 1, 0, blob.p, blob.n); blob.n = 0;
    pres_blob(&blob, prc, 2);
    res_entry(&x, 0, 0, FX2_NONE, 2, 0, blob.p, blob.n);
    free(blob.p);
    res_entry(&x, 0, 0, FX2_NONE, 3, 1, "texa[1]", 8);
    res_entry(&x, FX2_NONE, P_SMP, 0, 0, 1, "albedo_tex", 11);
    return fxb_file(&x, size);
}

/* an effect of parameters only: each with its typedef, flags and file value (dwords as stored) */
typedef struct { ts t; uint32_t flags; uint32_t n; const uint32_t *v; } pspec;
static uint8_t *make_params(const pspec *ps, uint32_t np, uint32_t nobjects, uint32_t *size){
    fxb x;
    fxb_init(&x);
    uint32_t toff[32], voff[32];
    for (uint32_t p = 0; p < np; p++){ toff[p] = fxb_type(&x, &ps[p].t); voff[p] = fxb_dw(&x, ps[p].v, ps[p].n); }
    bb_u32(&x.t, np); bb_u32(&x.t, 0); bb_u32(&x.t, 0); bb_u32(&x.t, nobjects);
    for (uint32_t p = 0; p < np; p++){ bb_u32(&x.t, toff[p]); bb_u32(&x.t, voff[p]); bb_u32(&x.t, ps[p].flags); bb_u32(&x.t, 0); }
    bb_u32(&x.t, 0); bb_u32(&x.t, 0);
    return fxb_file(&x, size);
}

/* §2.3's empty names: an unnamed FLOAT4 with an unnamed annotation and one named "k", a struct st with an
 * unnamed member and a member y, a texture; a pass whose vertex shader has one CTAB constant named "" (a
 * string at a non-zero offset) and whose TEXTURE[0] is a usage-1 reference to ""; a second technique, unnamed,
 * with one unnamed pass.  Objects 1-3 */
static uint8_t *make_unnamed(uint32_t *size){
    fxb x;
    fxb_init(&x);
    static const ts v4 = T_(FLOAT, VECTOR, NULL, NULL, 0, 1, 4), tex = T_(TEXTURE, OBJECT, "tex", NULL, 0, 0, 0);
    static const ts st_m[] = { T_(FLOAT, SCALAR, NULL, NULL, 0, 1, 1), T_(FLOAT, SCALAR, "y", NULL, 0, 1, 1) };
    static const ts st = { FX2_T_VOID, FX2_C_STRUCT, "st", NULL, 0, 0, 0, 2, st_m };
    static const ts a_none = T_(INT, SCALAR, NULL, NULL, 0, 1, 1), a_k = T_(INT, SCALAR, "k", NULL, 0, 1, 1);
    static const ts vsh = T_(VERTEXSHADER, OBJECT, NULL, NULL, 0, 0, 0), otex = T_(TEXTURE, OBJECT, NULL, NULL, 0, 0, 0);
    static const uint32_t z4[4], stv[2] = { 0x3F800000u, 0x40000000u };
    uint32_t one = 1, two = 2, three = 3, five = 5, six = 6;
    uint32_t t0 = fxb_type(&x, &v4), v0 = fxb_dw(&x, z4, 4), t1 = fxb_type(&x, &st), v1 = fxb_dw(&x, stv, 2);
    uint32_t t2 = fxb_type(&x, &tex), v2 = fxb_dw(&x, &one, 1);
    uint32_t a0t = fxb_type(&x, &a_none), a0v = fxb_dw(&x, &five, 1), a1t = fxb_type(&x, &a_k), a1v = fxb_dw(&x, &six, 1);
    uint32_t tn = fxb_name(&x, "t"), pn = fxb_name(&x, "p");
    bb ps = { 0 };
    state_entry(&x, &ps, 146, 0, &vsh, &two, 1);
    state_entry(&x, &ps, 164, 0, &otex, &three, 1);
    bb *t = &x.t;
    bb_u32(t, 3); bb_u32(t, 2); bb_u32(t, 0); bb_u32(t, 4);
    bb_u32(t, t0); bb_u32(t, v0); bb_u32(t, 0); bb_u32(t, 2); bb_u32(t, a0t); bb_u32(t, a0v); bb_u32(t, a1t); bb_u32(t, a1v);
    bb_u32(t, t1); bb_u32(t, v1); bb_u32(t, 0); bb_u32(t, 0);
    bb_u32(t, t2); bb_u32(t, v2); bb_u32(t, 0); bb_u32(t, 0);
    bb_u32(t, tn); bb_u32(t, 0); bb_u32(t, 1);
    bb_u32(t, pn); bb_u32(t, 0); bb_u32(t, 2); bb_put(t, ps.p, ps.n);
    free(ps.p);
    bb_u32(t, 0); bb_u32(t, 0); bb_u32(t, 1);                  /* the unnamed technique and its unnamed pass */
    bb_u32(t, 0); bb_u32(t, 0); bb_u32(t, 0);
    bb_u32(t, 0); bb_u32(t, 2);                                 /* object data, resources */
    static const cs c[] = { { "", 2, 0, 1, FX2_C_VECTOR, FX2_T_FLOAT, 1, 4, 1, 0, 0 } };
    bb blob = { 0 };
    shader_blob(&blob, 0xFFFE0101u, "vs_1_1", c, 1, 0);
    res_entry(&x, 0, 0, FX2_NONE, 0, 0, blob.p, blob.n);
    free(blob.p);
    res_entry(&x, 0, 0, FX2_NONE, 1, 1, "", 1);
    return fxb_file(&x, size);
}

/* §3.5's row 178: textures ta, tb, tc; a sampler smp whose block's TEXTURE is <ta>; a sampler array smpa[2]
 * whose element blocks take <tb> and <tc>; a texture td and a sampler smpb whose block takes <td>; a pass with
 * SAMPLER[0] = <smp>, SAMPLER[1] = <smpa> and TEXTURE[2] = <smpb> (another row), all usage-1 references.
 * Objects 1-9 */
static uint8_t *make_sampler_ref(uint32_t *size){
    fxb x;
    fxb_init(&x);
    static const ts tex = T_(TEXTURE, OBJECT, NULL, NULL, 0, 0, 0), smp = T_(SAMPLER, OBJECT, NULL, NULL, 0, 0, 0);
    static const ts pt[7] = { T_(TEXTURE, OBJECT, "ta", NULL, 0, 0, 0), T_(TEXTURE, OBJECT, "tb", NULL, 0, 0, 0),
                              T_(TEXTURE, OBJECT, "tc", NULL, 0, 0, 0), T_(SAMPLER, OBJECT, "smp", NULL, 0, 0, 0),
                              T_(SAMPLER, OBJECT, "smpa", NULL, 2, 0, 0), T_(TEXTURE, OBJECT, "td", NULL, 0, 0, 0),
                              T_(SAMPLER, OBJECT, "smpb", NULL, 0, 0, 0) };
    uint32_t toff[7], voff[7], zero = 0, seven = 7, nine = 9;
    for (uint32_t p = 0; p < 3; p++){ uint32_t o = p + 1; toff[p] = fxb_type(&x, &pt[p]); voff[p] = fxb_dw(&x, &o, 1); }
    toff[5] = fxb_type(&x, &pt[5]); voff[5] = fxb_dw(&x, &seven, 1);
    for (uint32_t p = 3; p < 7; p++){                          /* one TEXTURE state per block: objects 4, 5, 6, 8 */
        if (p == 5) continue;
        uint32_t ne = p == 4 ? 2 : 1;
        bb st[2] = { { 0 }, { 0 } };
        for (uint32_t k = 0; k < ne; k++){ uint32_t o = p == 6 ? 8 : p + 1 + k; state_entry(&x, &st[k], 164, 256, &tex, &o, 1); }
        toff[p] = fxb_type(&x, &pt[p]);
        voff[p] = x.d.n;                                       /* the elements' blocks one after another */
        for (uint32_t k = 0; k < ne; k++){ bb_u32(&x.d, 1); bb_put(&x.d, st[k].p, st[k].n); free(st[k].p); }
    }
    uint32_t tn = fxb_name(&x, "t"), pn = fxb_name(&x, "p");
    bb ps = { 0 };
    state_entry(&x, &ps, 178, 0, &smp, &zero, 1);              /* a sampler value: an empty block */
    state_entry(&x, &ps, 178, 1, &smp, &zero, 1);
    state_entry(&x, &ps, 164, 2, &tex, &nine, 1);
    bb *t = &x.t;
    bb_u32(t, 7); bb_u32(t, 1); bb_u32(t, 0); bb_u32(t, 10);
    for (uint32_t p = 0; p < 7; p++){ bb_u32(t, toff[p]); bb_u32(t, voff[p]); bb_u32(t, 0); bb_u32(t, 0); }
    bb_u32(t, tn); bb_u32(t, 0); bb_u32(t, 1);
    bb_u32(t, pn); bb_u32(t, 0); bb_u32(t, 3); bb_put(t, ps.p, ps.n);
    free(ps.p);
    bb_u32(t, 0); bb_u32(t, 7);                                 /* object data, resources */
    res_entry(&x, FX2_NONE, 3, 0, 0, 1, "ta", 3);
    res_entry(&x, FX2_NONE, 4, 0, 0, 1, "tb", 3);
    res_entry(&x, FX2_NONE, 4, 1, 0, 1, "tc", 3);
    res_entry(&x, FX2_NONE, 6, 0, 0, 1, "td", 3);
    res_entry(&x, 0, 0, FX2_NONE, 0, 1, "smp", 4);
    res_entry(&x, 0, 0, FX2_NONE, 1, 1, "smpa", 5);
    res_entry(&x, 0, 0, FX2_NONE, 2, 1, "smpb", 5);
    return fxb_file(&x, size);
}

/* a creation the model's memory bound refuses (fxmodel.h): a shared FLOAT s, a FLOAT[4096] big and a vertex
 * shader whose CTAB lists big k times (one TypeInfo, one name), so the leaves need k x 64 KB.  Objects 1 */
static uint8_t *make_blowup(uint32_t k, uint32_t *size){
    fxb x;
    fxb_init(&x);
    static const ts s = T_(FLOAT, SCALAR, "s", NULL, 0, 1, 1), big = T_(FLOAT, SCALAR, "big", NULL, 4096, 1, 1),
                    vsh = T_(VERTEXSHADER, OBJECT, NULL, NULL, 0, 0, 0);
    uint32_t zero = 0, one = 1, *zb = xalloc(4096 * 4);
    uint32_t t0 = fxb_type(&x, &s), v0 = fxb_dw(&x, &zero, 1), t1 = fxb_type(&x, &big), v1 = fxb_dw(&x, zb, 4096);
    free(zb);
    uint32_t tn = fxb_name(&x, "t"), pn = fxb_name(&x, "p");
    bb ps = { 0 };
    state_entry(&x, &ps, 146, 0, &vsh, &one, 1);
    bb *t = &x.t;
    bb_u32(t, 2); bb_u32(t, 1); bb_u32(t, 0); bb_u32(t, 2);
    bb_u32(t, t0); bb_u32(t, v0); bb_u32(t, FXM_SHARED); bb_u32(t, 0);
    bb_u32(t, t1); bb_u32(t, v1); bb_u32(t, 0); bb_u32(t, 0);
    bb_u32(t, tn); bb_u32(t, 0); bb_u32(t, 1);
    bb_u32(t, pn); bb_u32(t, 0); bb_u32(t, 1); bb_put(t, ps.p, ps.n);
    free(ps.p);
    bb_u32(t, 0); bb_u32(t, 1);                                 /* object data, resources */
    bb pl = { 0 }, blob = { 0 };
    for (int i = 0; i < 28; i++){ uint8_t z = 0; bb_put(&pl, &z, 1); }
    uint32_t ti = pl.n;
    bb_u16(&pl, FX2_C_SCALAR); bb_u16(&pl, FX2_T_FLOAT); bb_u16(&pl, 1); bb_u16(&pl, 1); bb_u16(&pl, 4096); bb_u16(&pl, 0); bb_u32(&pl, 0);
    uint32_t nm = pl.n;
    bb_put(&pl, "big", 4);
    uint32_t tgt = pl.n;
    bb_put(&pl, "vs_1_1", 7);
    bb_pad(&pl, 0);
    uint32_t ci = pl.n;
    for (uint32_t j = 0; j < k; j++){ bb_u32(&pl, nm); bb_u16(&pl, 2); bb_u16(&pl, 0); bb_u16(&pl, 1); bb_u16(&pl, 0); bb_u32(&pl, ti); bb_u32(&pl, 0); }
    bb_set32(&pl, 0, 28); bb_set32(&pl, 4, tgt); bb_set32(&pl, 8, 0xFFFE0101u); bb_set32(&pl, 12, k);
    bb_set32(&pl, 16, ci); bb_set32(&pl, 20, 0x100); bb_set32(&pl, 24, tgt);
    bb_u32(&blob, 0xFFFE0101u);
    bb_u32(&blob, 0xFFFE | (1 + pl.n / 4) << 16); bb_u32(&blob, FX2_CTAB); bb_put(&blob, pl.p, pl.n);
    bb_u32(&blob, FX2_END);
    res_entry(&x, 0, 0, FX2_NONE, 0, 0, blob.p, blob.n);
    free(pl.p); free(blob.p);
    return fxb_file(&x, size);
}

/* ---- one effect: its bytes, parse tree and model ---- */
typedef struct { char name[256]; uint8_t *buf; uint32_t size; fx2_effect fx; fxm_effect *m; } eff;

static int eff_parse(eff *e, uint8_t *buf, uint32_t size, const char *name){
    char err[256];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->buf = buf; e->size = size;
    if (fx2_parse(&e->fx, buf, size, err, sizeof err)){ CHECK(0, "%s: %s", name, err); return -1; }
    return 0;
}
static int eff_model(eff *e, fxm_pool *pool){
    char err[256];
    if (fxm_effect_new(&e->m, &e->fx, pool, err, sizeof err)){ CHECK(0, "%s: %s", e->name, err); return -1; }
    return 0;
}

static uint32_t id_of(const fxm_effect *m, const char *path){ return fxm_by_name(m, 0, (const uint8_t *)path, (uint32_t)strlen(path)); }

typedef struct { uint32_t n; uint32_t p[64]; } droplog;
static void drop_rec(void *ctx, uint32_t ptr){ droplog *d = ctx; if (d->n < 64) d->p[d->n] = ptr; d->n++; }

/* ======================= §6.1: conversions, versions, lookups ======================= */

static void test_conv(void){
    eff E;
    uint32_t size;
    uint8_t *buf = make_conv(&size);
    if (eff_parse(&E, buf, size, "conv")){ free(buf); return; }
    fxm_pool *pool = fxm_pool_new();
    if (eff_model(&E, pool)){ fx2_free(&E.fx); free(buf); fxm_pool_free(pool); return; }
    fxm_effect *m = E.m;
    uint32_t id[P_NP], w, v[64], m16[16];
    int ch;
    for (int p = 0; p < P_NP; p++) id[p] = fxm_param(m, 0, (uint32_t)p);

    /* §2.2: ids, order and shapes */
    CHECK(fxm_param(m, 0, P_NP) == 0 && fxm_element(m, 0, P_NP) == 0, "past the parameters");
    CHECK(id[P_B] == 1 && fxm_annot(m, id[P_B], 0) == id[P_B] + 1 && id[P_I] == id[P_B] + 4, "a parameter, then its annotations");
    CHECK(id_of(m, "i5[3]") == id[P_I5] + 4 && fxm_element(m, id[P_I5], 3) == id[P_I5] + 4 && fxm_element(m, id[P_I5], 5) == 0,
          "elements follow their array");
    CHECK(fxm_param(m, id[P_I5], 0) == 0, "fxm_param on an array gives 0");
    uint32_t pos = id_of(m, "st.pos"), fl = id_of(m, "st.flags"), fl1 = id_of(m, "st.flags[1]");
    CHECK(pos == id[P_ST] + 1 && id_of(m, "st.n") == id[P_ST] + 2 && fl == id[P_ST] + 3 && fl1 == id[P_ST] + 5,
          "a struct's members depth first");
    CHECK(fxm_param(m, id[P_ST], 1) == id[P_ST] + 2 && fxm_param(m, id[P_ST], 3) == 0 && fxm_element(m, fl, 1) == fl1, "members, elements");
    CHECK(fxm_by_name(m, id[P_ST], (const uint8_t *)"flags[1]", 8) == fl1 && fxm_by_name(m, id[P_ST], (const uint8_t *)"pos", 3) == pos,
          "names under a parent");
    uint32_t sa1 = id_of(m, "sa[1]"), sa1x = id_of(m, "sa[1].x"), sa1t = id_of(m, "sa[1].tex");
    CHECK(sa1 && fxm_by_name(m, sa1, (const uint8_t *)"x", 1) == sa1x && sa1t == sa1x + 1, "an array of structs");
    const fxm_node *n;
    n = fxm_node_get(m, id[P_ST]);
    CHECK(n->bytes == 24 && n->members == 3 && fxm_node_get(m, fl1)->bytes == 4, "struct bytes");
    n = fxm_node_get(m, id[P_SA]);
    CHECK(n->bytes == 16 && fxm_node_get(m, sa1)->bytes == 8 && fxm_node_get(m, sa1)->elements == 0, "struct array bytes");
    CHECK(fxm_node_get(m, sa1t)->object == 5 && fxm_data(m, sa1t)[0] == 0 && fxm_data(m, sa1x)[0] == fb(0.5f), "struct defaults");
    CHECK(fxm_node_get(m, id[P_SMP])->bytes == 0 && fxm_node_get(m, id[P_SMP])->block && fxm_data(m, id[P_SMP]) == NULL, "a sampler has no storage");
    CHECK(fxm_node_get(m, id[P_TEXA])->bytes == 8 && fxm_node_get(m, id_of(m, "texa[1]"))->object == 3, "a texture array");
    n = fxm_node_get(m, id_of(m, "i5[2]"));
    CHECK(n->kind == FXM_K_ELEMENT && n->top == id[P_I5] && n->elements == 0 && n->name.n == 2 && !memcmp(n->path.p, "i5[2]", 5),
          "an element: its array's name, its own path");
    n = fxm_node_get(m, fxm_annot(m, id[P_B], 0));
    CHECK(n->kind == FXM_K_ANNOT && n->flags == FXM_ANNOTATION && n->top == 0 && n->object == 7, "an annotation");
    CHECK(fxm_annot_by_name(m, id[P_B], (const uint8_t *)"level", 5) == id[P_B] + 2, "annotation by name");
    CHECK(fxm_get_int(m, id[P_B] + 2, &w) == FXM_OK && w == 3, "GetInt on an INT annotation");
    CHECK(fxm_get_array(m, id[P_B] + 3, FX2_T_FLOAT, v, 2) == FXM_OK && v[0] == fb(-1) && v[1] == fb(2.5f), "an annotation array");
    CHECK(fxm_get_object(m, id[P_B] + 1, &w) == FXM_OK && w == 0, "a string annotation reads its 0 pointer");

    /* §2.3: lookups */
    CHECK(!id_of(m, "i5[03]") && !id_of(m, "i5[5]") && !id_of(m, "i5[]") && !id_of(m, "i5[ 3]") && !id_of(m, "I5") && !id_of(m, "x"),
          "no match for leading zeros, out of range, empty, spaces, case or a member name alone");
    CHECK(fxm_by_semantic(m, 0, (const uint8_t *)"albedo", 6) == id[P_TEX0] && fxm_by_semantic(m, 0, (const uint8_t *)"FACTOR", 6) == id[P_F],
          "semantics compare without case");
    CHECK(fxm_by_semantic(m, 0, NULL, 0) == id[P_B], "NULL: the first without a semantic");
    CHECK(fxm_by_semantic(m, id[P_ST], (const uint8_t *)"position", 8) == pos && fxm_by_semantic(m, 0, (const uint8_t *)"position", 8) == 0,
          "a struct parent: its direct members");
    CHECK(fxm_by_semantic(m, id[P_SA], NULL, 0) == id_of(m, "sa[0]") && fxm_by_semantic(m, id[P_I5], NULL, 0) == 0,
          "an array of structs: W's members[] are its elements, with the array's semantic; a numeric array: none");
    uint32_t t0 = fxm_tech(m, 0), p0 = fxm_pass(m, t0, 0);
    CHECK(t0 && fxm_tech_by_name(m, (const uint8_t *)"t0", 2) == t0 && !fxm_tech(m, 1) && fxm_pass_by_name(m, t0, (const uint8_t *)"p0", 2) == p0,
          "techniques and passes");
    CHECK(fxm_annot_by_name(m, t0, (const uint8_t *)"level", 5) == t0 + 1 && fxm_get_int(m, t0 + 1, &w) == FXM_OK && w == 2, "a technique annotation");
    CHECK(p0 == t0 + 2 && fxm_nodes(m) == p0, "the pass is the last node");

    /* §2.5: conversions, exact dwords */
    /* SetFloat into INT is d3dx9_26's _ftol2: 64-bit truncation, the low dword; NaN and the infinities 0 (O) */
    struct { float in; uint32_t out; } f2i[] = { { 1.9f, 1 }, { -1.9f, 0xFFFFFFFFu }, { 3e9f, 0xB2D05E00u }, { -3e9f, 0x4D2FA200u },
                                                { 2147483520.0f, 2147483520u }, { -2147483648.0f, 0x80000000u }, { -0.0f, 0 },
                                                { 4294967296.0f, 0 }, { 1e19f, 0 }, { 5e9f, 0x2A05F200u }, { -5e9f, 0xD5FA0E00u } };
    for (size_t k = 0; k < sizeof f2i / sizeof f2i[0]; k++){
        CHECK(fxm_set_float(m, id[P_I], fb(f2i[k].in), NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == f2i[k].out,
              "SetFloat(%g) into INT: 0x%08x, want 0x%08x", f2i[k].in, fxm_data(m, id[P_I])[0], f2i[k].out);
    }
    CHECK(fxm_set_float(m, id[P_I], 0x7FC00000u, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0, "NaN into INT: 0");
    CHECK(fxm_set_float(m, id[P_I], 0xFF800000u, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0, "-inf into INT: 0");
    v[0] = fb(-3e9f);
    CHECK(fxm_set_array(m, id[P_I], FX2_T_FLOAT, v, 1, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0x4D2FA200u,
          "SetFloatArray into INT: _ftol2 too");
    CHECK(fxm_set_float(m, id[P_B], 0x80000000u, NULL) == FXM_OK && fxm_data(m, id[P_B])[0] == 0, "-0.0 into BOOL is FALSE (fucompp)");
    CHECK(fxm_set_float(m, id[P_B], 0x7FC00000u, NULL) == FXM_OK && fxm_data(m, id[P_B])[0] == 1, "NaN into BOOL is TRUE");
    /* SetFloat into FLOAT and SetInt into FLOAT compare values, not bytes: -0 does not replace +0 (O) */
    {
        uint32_t z = 0, nz = 0x80000000u, vv[1] = { 0 };
        int ch = 9;
        CHECK(fxm_set_value(m, id[P_F], vv, 4, NULL) == FXM_OK && fxm_set_float(m, id[P_F], nz, &ch) == FXM_OK && !ch &&
              fxm_data(m, id[P_F])[0] == z, "SetFloat(-0) on +0: nothing written");
        uint64_t ver = fxm_version(m, id[P_F]);
        CHECK(fxm_set_float(m, id[P_F], 0x7FC00000u, &ch) == FXM_OK && ch && fxm_set_float(m, id[P_F], 0x7FC00000u, &ch) == FXM_OK &&
              !ch && fxm_version(m, id[P_F]) == ver + 2, "SetFloat(NaN) writes every time: the version rises");
        vv[0] = nz;
        CHECK(fxm_set_value(m, id[P_F], vv, 4, NULL) == FXM_OK && fxm_set_int(m, id[P_F], 0, &ch) == FXM_OK && !ch &&
              fxm_data(m, id[P_F])[0] == nz, "SetInt(0) on -0.0: nothing written");
        vv[0] = 0x4B800000u;                                    /* 16777216.0 */
        ver = fxm_version(m, id[P_F]);
        CHECK(fxm_set_value(m, id[P_F], vv, 4, NULL) == FXM_OK && fxm_set_int(m, id[P_F], 16777217u, &ch) == FXM_OK && !ch &&
              fxm_version(m, id[P_F]) == ver + 2, "SetInt(2^24 + 1) on 2^24: the same float written, the version rises");
    }
    CHECK(fxm_set_bool(m, id[P_B], 5, NULL) == FXM_OK && fxm_data(m, id[P_B])[0] == 5, "SetBool(5) on BOOL is stored raw");
    CHECK(fxm_get_bool(m, id[P_B], &w) == FXM_OK && w == 5, "GetBool on BOOL: raw (set_number with equal types; W)");
    CHECK(fxm_get_value(m, id[P_B], v, 4) == FXM_OK && v[0] == 1, "GetValue normalises BOOL");
    CHECK(fxm_get_int(m, id[P_B], &w) == FXM_OK && w == 1 && fxm_get_float(m, id[P_B], &w) == FXM_OK && w == fb(1), "BOOL to INT and FLOAT");
    CHECK(fxm_set_bool(m, id[P_F], 7, NULL) == FXM_OK && fxm_data(m, id[P_F])[0] == fb(1), "SetBool into FLOAT gives 1.0");
    CHECK(fxm_set_int(m, id[P_F], (uint32_t)-3, NULL) == FXM_OK && fxm_data(m, id[P_F])[0] == fb(-3), "SetInt into FLOAT");
    CHECK(fxm_get_bool(m, id[P_F], &w) == FXM_OK && w == 1, "FLOAT to BOOL");
    v[0] = 5;
    CHECK(fxm_set_array(m, id[P_F], FX2_T_BOOL, v, 1, NULL) == FXM_OK && fxm_data(m, id[P_F])[0] == fb(5), "SetBoolArray(5) on FLOAT gives 5.0");
    CHECK(fxm_set_array(m, id[P_B], FX2_T_BOOL, v, 1, NULL) == FXM_OK && fxm_data(m, id[P_B])[0] == 1, "SetBoolArray(5) on BOOL: INT to BOOL");
    /* the SetInt colour form and GetInt back */
    float k255 = 1.0f / 255.0f;
    CHECK(fxm_set_int(m, id[P_V4], 0x80FF4020u, NULL) == FXM_OK, "SetInt colour on float4");
    const uint32_t *d4 = fxm_data(m, id[P_V4]);
    CHECK(d4[0] == fb(255 * k255) && d4[1] == fb(64 * k255) && d4[2] == fb(32 * k255) && d4[3] == fb(128 * k255), "colour: bytes 2, 1, 0, 3 x 1/255");
    CHECK(fxm_get_int(m, id[P_V4], &w) == FXM_OK && w == 0x80FF4020u, "GetInt colour back: 0x%08x", w);
    CHECK(fxm_set_int(m, id[P_V3], 0x80FF4020u, NULL) == FXM_OK && fxm_get_int(m, id[P_V3], &w) == FXM_OK && w == 0x00FF4020u, "colour on float3");
    CHECK(fxm_set_int(m, id[P_C31], 0x00102030u, NULL) == FXM_OK && fxm_data(m, id[P_C31])[0] == fb(16 * k255), "colour on a 3x1 matrix");
    uint32_t vec[4] = { fb(2), fb(-1), 0x7FC00000u, fb(0.5f) };
    CHECK(fxm_set_vector(m, id[P_V4], vec, NULL) == FXM_OK && fxm_get_int(m, id[P_V4], &w) == FXM_OK && w == 0x7FFF0000u,
          "GetInt colour clamps: 0x%08x", w);
    /* SetVector */
    /* SetVector into INT 1x1: each channel rounded, x * 255 + 0.5 at the caller's x87 precision (O) */
    uint32_t vin[4] = { fb(1), fb(0.5f), fb(-3), fb(2) };
    CHECK(fxm_set_vector(m, id[P_I], vin, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0xFFFF8000u,
          "SetVector into INT 1x1 packs: 0x%08x", fxm_data(m, id[P_I])[0]);
    CHECK(fxm_get_vector(m, id[P_I], v) == FXM_OK && v[0] == fb(255 * k255) && v[1] == fb(128 * k255) && v[2] == 0 && v[3] == fb(255 * k255),
          "GetVector from INT 1x1");
    uint32_t snan[4] = { 0x7F800001u, 0xFF800001u, 0x7FBFFFFFu, fb(0.5f) };
    CHECK(fxm_set_vector(m, id[P_I], snan, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0x80000000u,
          "SetVector into INT 1x1: NaNs give 0 (0x%08x)", fxm_data(m, id[P_I])[0]);
    uint32_t amb[4] = { fb(0.5f), fb(0.25f), fb(0.999f), fb(0.0039f) };
    CHECK(fxm_set_vector(m, id[P_I], amb, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0x018040FFu,
          "SetVector(0.5, 0.25, 0.999, 0.0039): d3dx9_26's 0x018040FF, not truncation's 0x007F3FFE");
    uint32_t half[4] = { 0x3F010101u, 0, 0, 0 };              /* 128.5 / 255 as a float */
    CHECK(fxm_set_vector(m, id[P_I], half, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0x00810000u, "128.5/255 at 24 bits: 129");
    fxm_set_x87_pc(m, 53);
    CHECK(fxm_set_vector(m, id[P_I], half, NULL) == FXM_OK && fxm_data(m, id[P_I])[0] == 0x00800000u, "128.5/255 at 53 bits: 128");
    fxm_set_x87_pc(m, 24);
    uint32_t v1234[4] = { fb(1), fb(2), fb(3), fb(4) };
    CHECK(fxm_set_vector(m, id[P_V2], v1234, NULL) == FXM_OK && fxm_node_get(m, id[P_V2])->bytes == 8 &&
          fxm_data(m, id[P_V2])[0] == fb(1) && fxm_data(m, id[P_V2])[1] == fb(2), "SetVector into 1x2 writes 2");
    CHECK(fxm_get_vector(m, id[P_V2], v) == FXM_OK && v[2] == 0 && v[3] == 0, "GetVector: the rest 0.0");
    CHECK(fxm_set_vector(m, id[P_F], v1234, NULL) == FXM_OK && fxm_data(m, id[P_F])[0] == fb(1), "SetVector on a FLOAT scalar");
    /* matrices */
    for (int k = 0; k < 16; k++) m16[k] = fb((float)k);
    CHECK(fxm_set_matrix(m, id[P_M33], m16, 0, NULL) == FXM_OK, "SetMatrix 3x3");
    const uint32_t *dm = fxm_data(m, id[P_M33]);
    int okm = 1;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) okm &= dm[i * 3 + k] == m16[i * 4 + k];
    CHECK(okm, "SetMatrix 3x3: data[i*3+k] = m[i][k]");
    CHECK(fxm_set_matrix(m, id[P_M43], m16, 1, NULL) == FXM_OK, "SetMatrixTranspose 4x3");
    dm = fxm_data(m, id[P_M43]); okm = 1;
    for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) okm &= dm[i * 3 + k] == m16[k * 4 + i];
    CHECK(okm, "SetMatrixTranspose 4x3: data[i*3+k] = m[k][i]");
    CHECK(fxm_get_matrix(m, id[P_M43], v, 0) == FXM_OK && v[3] == 0 && v[4] == m16[1] && v[15] == 0, "GetMatrix fills 0.0 outside");
    CHECK(fxm_get_matrix(m, id[P_V4], v, 1) == FXM_OK && fxm_get_matrix(m, id[P_V4], v, 0) == FXM_INVALIDCALL, "GetMatrixTranspose takes a vector");
    CHECK(fxm_set_matrix_array(m, id[P_M44A], m16, 3, 0, NULL) == FXM_INVALIDCALL, "SetMatrixArray count > elements");
    uint32_t m32[32];
    for (int k = 0; k < 32; k++) m32[k] = fb((float)(100 + k));
    CHECK(fxm_set_matrix_array(m, id[P_M44A], m32, 2, 0, NULL) == FXM_OK && fxm_data(m, id[P_M44A])[31] == m32[31], "SetMatrixArray 2");
    CHECK(fxm_set_vector_array(m, id[P_V4A], m32, 4, NULL) == FXM_INVALIDCALL, "SetVectorArray count > elements");
    CHECK(fxm_set_vector_array(m, id[P_V4A], m32, 2, NULL) == FXM_OK && fxm_data(m, id[P_V4A])[7] == m32[7] &&
          fxm_data(m, id[P_V4A])[8] == fb(8), "SetVectorArray 2 of 3");
    CHECK(fxm_get_vector_array(m, id[P_V4A], v, 3) == FXM_OK && v[11] == fb(11), "GetVectorArray");
    /* W's shape checks (fx-model §2.5) */
    uint64_t vm = fxm_version(m, id[P_M44]);
    CHECK(fxm_set_matrix_array(m, id[P_M44], m16, 0, 0, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_M44]) > vm,
          "SetMatrixArray count 0 on a non-array: elements >= count, OK and the version rises");
    CHECK(fxm_set_vector_array(m, id[P_V4], m32, 0, NULL) == FXM_INVALIDCALL, "SetVectorArray needs an array, even for count 0");
    CHECK(fxm_get_vector_array(m, 0, NULL, 0) == FXM_OK && fxm_get_matrix_array(m, id[P_B], NULL, 0, 1) == FXM_OK,
          "Get{Vector,Matrix}Array count 0: OK whatever the handle");
    CHECK(fxm_get_array(m, id[P_I], FX2_T_INT, NULL, 0) == FXM_INVALIDCALL && fxm_get_array(m, id[P_I], FX2_T_INT, v, 0) == FXM_OK,
          "Get{Bool,Int,Float}Array: a NULL pointer is refused even for count 0 (W tests it first)");
    CHECK(fxm_get_matrix(m, id[P_V4], v, 1) == FXM_OK && v[0] == fxm_data(m, id[P_V4])[0] && v[3] == fxm_data(m, id[P_V4])[3] && v[4] == 0,
          "GetMatrixTranspose of a vector: row 0, not transposed (W get_matrix(FALSE))");
    /* SetIntArray on INT[5] */
    uint32_t nine[9] = { 10, 11, 12, 13, 14, 15, 16, 17, 18 };
    CHECK(fxm_set_array(m, id[P_I5], FX2_T_INT, nine, 3, NULL) == FXM_OK && fxm_data(m, id[P_I5])[2] == 12 && fxm_data(m, id[P_I5])[3] == 4,
          "SetIntArray 3 of 5");
    CHECK(fxm_set_array(m, id[P_I5], FX2_T_INT, nine, 9, NULL) == FXM_OK && fxm_data(m, id[P_I5])[4] == 14, "SetIntArray 9 writes 5");
    CHECK(fxm_get_array(m, id[P_MC44], FX2_T_FLOAT, v, 16) == FXM_OK && fxm_set_array(m, id[P_MC44], FX2_T_FLOAT, v, 16, NULL) == FXM_INVALIDCALL,
          "MATRIX_COLUMNS: Get*Array only");
    /* BOOL storage through SetValue and the arrays */
    uint32_t braw[4] = { 5, 0, 0x80000000u, 1 };
    CHECK(fxm_set_value(m, id[P_BV4], braw, 16, NULL) == FXM_OK && fxm_data(m, id[P_BV4])[0] == 5, "SetValue BOOL raw");
    CHECK(fxm_get_value(m, id[P_BV4], v, 16) == FXM_OK && v[0] == 1 && v[1] == 0 && v[2] == 1 && v[3] == 1, "GetValue BOOL 0/1");
    CHECK(fxm_get_array(m, id[P_BV4], FX2_T_BOOL, v, 4) == FXM_OK && v[0] == 5 && v[2] == 0x80000000u, "GetBoolArray on BOOL: raw");
    CHECK(fxm_get_array(m, id[P_BV4], FX2_T_INT, v, 4) == FXM_OK && v[0] == 1 && v[2] == 1, "GetIntArray on BOOL: 0/1");
    uint32_t sraw[6] = { fb(4), fb(5), fb(6), 77, 3, 0 };
    CHECK(fxm_set_value(m, id[P_ST], sraw, 24, NULL) == FXM_OK && fxm_get_value(m, id[P_ST], v, 24) == FXM_OK && v[3] == 77 && v[4] == 3 && v[5] == 0,
          "a struct: SetValue raw, GetValue raw too (W switches on the struct's own type, VOID)");
    CHECK(fxm_get_value(m, fl, v, 8) == FXM_OK && v[0] == 1 && v[1] == 0, "GetValue on the BOOL member itself normalises");
    CHECK(fxm_set_value(m, id[P_ST], sraw, 20, NULL) == FXM_INVALIDCALL, "SetValue with too few bytes");
    CHECK(fxm_set_value(m, fl1, sraw + 3, 4, NULL) == FXM_OK && fxm_data(m, id[P_ST])[5] == 77, "an element writes into its top's block");
    /* objects */
    CHECK(fxm_set_object(m, id[P_TEX0], 0x1000, &w, NULL) == FXM_OK && w == 0 && fxm_get_object(m, id[P_TEX0], &w) == FXM_OK && w == 0x1000, "SetTexture");
    CHECK(fxm_set_object(m, id[P_TEXA], 0x2000, &w, NULL) == FXM_INVALIDCALL && fxm_set_object(m, id_of(m, "texa[1]"), 0x2000, &w, NULL) == FXM_OK,
          "SetTexture: one element, not the array");
    CHECK(fxm_set_object(m, sa1t, 0x3000, NULL, NULL) == FXM_OK && fxm_data(m, id[P_SA])[3] == 0x3000, "SetTexture on a struct member");
    CHECK(fxm_set_object(m, id[P_STR], 0x4000, NULL, NULL) == FXM_OK, "SetString");
    /* refused calls change nothing */
    uint32_t before = fxm_data(m, id[P_I])[0];
    uint64_t c0 = fxm_clock(m);
    ch = 9;
    CHECK(fxm_set_int(m, 0, 1, &ch) == FXM_INVALIDCALL && ch == 0 && fxm_set_int(m, fxm_nodes(m) + 1, 1, NULL) == FXM_INVALIDCALL, "id 0 or past N");
    CHECK(fxm_set_int(m, id[P_V2], 1, NULL) == FXM_INVALIDCALL && fxm_set_float(m, id[P_V4], 0, NULL) == FXM_INVALIDCALL &&
          fxm_set_vector(m, id[P_M44], vin, NULL) == FXM_INVALIDCALL && fxm_set_matrix(m, id[P_V4], m16, 0, NULL) == FXM_INVALIDCALL &&
          fxm_set_vector_array(m, id[P_V4], m32, 1, NULL) == FXM_INVALIDCALL && fxm_set_object(m, id[P_I], 1, NULL, NULL) == FXM_INVALIDCALL &&
          fxm_set_bool(m, id[P_I5], 1, NULL) == FXM_INVALIDCALL && fxm_set_array(m, id[P_ST], FX2_T_INT, nine, 1, NULL) == FXM_INVALIDCALL &&
          fxm_set_array(m, id[P_I], FX2_T_STRING, nine, 1, NULL) == FXM_INVALIDCALL && fxm_set_matrix_array(m, id[P_M44], m16, 1, 0, NULL) == FXM_INVALIDCALL,
          "every refused shape");
    uint32_t an = fxm_annot(m, id[P_B], 1);
    CHECK(fxm_set_int(m, an, 1, NULL) == FXM_INVALIDCALL && fxm_set_value(m, an, nine, 4, NULL) == FXM_INVALIDCALL &&
          fxm_set_int(m, t0, 1, NULL) == FXM_INVALIDCALL && fxm_set_value(m, p0, nine, 64, NULL) == FXM_INVALIDCALL &&
          fxm_get_value(m, t0, v, 64) == FXM_INVALIDCALL, "Set* on an annotation, a technique or a pass");
    CHECK(fxm_set_value(m, id[P_SMP], nine, 64, NULL) == FXM_FAIL && fxm_get_value(m, id[P_SMP], v, 64) == FXM_FAIL, "SetValue on a sampler: E_FAIL");
    CHECK(fxm_data(m, id[P_I])[0] == before && fxm_clock(m) == c0, "refused calls changed nothing");

    /* §2.5's last column and §2.6: versions and changed */
    uint64_t v0;
    fxm_set_int(m, id[P_I], 42, NULL);
    v0 = fxm_version(m, id[P_I]);
    CHECK(fxm_set_int(m, id[P_I], 42, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_I]) == v0, "SetInt, same value: no version");
    CHECK(fxm_set_int(m, id[P_I], 43, &ch) == FXM_OK && ch == 1 && fxm_version(m, id[P_I]) > v0, "SetInt, new value: version");
    v0 = fxm_version(m, id[P_F]); fxm_get_float(m, id[P_F], &w);
    CHECK(fxm_set_float(m, id[P_F], w, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_F]) == v0, "SetFloat, same: no version");
    v0 = fxm_version(m, id[P_I5]);
    CHECK(fxm_set_array(m, id[P_I5], FX2_T_INT, nine, 5, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_I5]) > v0, "SetIntArray, same: version");
    v0 = fxm_version(m, id[P_V2]);
    CHECK(fxm_set_vector(m, id[P_V2], v1234, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_V2]) > v0, "SetVector, same: version");
    v0 = fxm_version(m, id[P_M33]);
    CHECK(fxm_set_matrix(m, id[P_M33], m16, 0, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_M33]) > v0, "SetMatrix, same: version");
    v0 = fxm_version(m, id[P_TEX0]);
    CHECK(fxm_set_object(m, id[P_TEX0], 0x1000, NULL, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_TEX0]) == v0, "SetTexture, same: no version");
    CHECK(fxm_set_object(m, id[P_TEX0], 0x1001, NULL, &ch) == FXM_OK && ch == 1 && fxm_version(m, id[P_TEX0]) > v0, "SetTexture, new: version");
    v0 = fxm_version(m, id[P_STR]);
    CHECK(fxm_set_object(m, id[P_STR], 0x4000, NULL, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_STR]) > v0, "SetString, same: version");
    v0 = fxm_version(m, id[P_B]);
    CHECK(fxm_set_bool(m, id[P_B], 1, &ch) == FXM_OK && fxm_version(m, id[P_B]) > v0, "SetBool: version");
    v0 = fxm_version(m, id[P_BV4]);
    CHECK(fxm_set_value(m, id[P_BV4], braw, 16, &ch) == FXM_OK && ch == 0 && fxm_version(m, id[P_BV4]) > v0, "SetValue, same: version");
    v0 = fxm_version(m, id[P_I5]);
    CHECK(fxm_set_int(m, id_of(m, "i5[2]"), 99, &ch) == FXM_OK && ch == 1 && fxm_version(m, id[P_I5]) > v0 &&
          fxm_version(m, id_of(m, "i5[2]")) == fxm_version(m, id[P_I5]), "an element raises its top's version");
    uint64_t mx = 0;
    for (int p = 0; p < P_NP; p++) if (fxm_version(m, id[p]) > mx) mx = fxm_version(m, id[p]);
    CHECK(mx == fxm_clock(m) && fxm_version(m, t0) == 0 && fxm_version(m, an) == 0, "the clock is the newest version");

    /* §3 on the synthetic CTABs: every reason, each kept in the list */
    uint32_t np;
    const fxm_prog *pr = fxm_progs(m, &np);
    CHECK(np == 3, "3 programs, got %u", np);
    if (np == 3){
        static const uint32_t why[] = { 0, 0, 0, 0, 1, 1, 2, 3, 4, 5, 7, 1 };
        CHECK(pr[0].kind == FXM_P_SHADER && pr[0].ctab && pr[0].nconsts == 13 && pr[0].nread == 12, "vs: 13 stored, 12 read (reason 6 ends it)");
        for (uint32_t k = 0; k < pr[0].nread && k < 12; k++)
            CHECK(pr[0].consts[k].why == why[k] && pr[0].consts[k].k == k, "vs const %u: reason %u, want %u", k, pr[0].consts[k].why, why[k]);
        const fxm_const *q = pr[0].consts;
        CHECK(q[0].id == id[P_M44] && q[0].layout == FXM_L_COLUMNS && q[0].per == 4 && q[0].used == 1 && q[0].nleaves == 1 &&
              q[0].leaves[0].reg == 0 && q[0].leaves[0].count == 4 && q[0].from == FX2_T_FLOAT && q[0].to == FX2_T_FLOAT, "m44: columns");
        CHECK(q[1].layout == FXM_L_VECTOR && q[1].per == 1 && q[1].leaves[0].reg == 4, "v4: vector");
        CHECK(q[2].nelem == 3 && q[2].used == 2 && q[2].leaves[1].reg == 6 && q[2].leaves[1].count == 1 && q[2].leaves[2].count == 0 &&
              q[2].leaves[2].id == id_of(m, "v4a[2]"), "v4a: 2 of 3 elements, leaves are elements");
        CHECK(q[3].layout == FXM_L_SAMPLER && q[3].stage == 258 && q[3].used == 1, "a vertex shader's s1: stage 257 + 1");
        CHECK(q[11].name.p == NULL && q[11].c.name == 0, "no name: reason 1");
        CHECK(pr[1].consts[0].layout == FXM_L_SAMPLER && pr[1].consts[0].stage == 0 && pr[1].consts[0].id == id[P_SMP], "a pixel shader's s0");
        static const uint32_t pwhy[] = { 0, 0, 2, 0, 4, 2, 4, 4 };
        CHECK(pr[1].nread == 8, "ps: 8 constants");
        for (uint32_t k = 0; k < pr[1].nread && k < 8; k++)
            CHECK(pr[1].consts[k].why == pwhy[k], "ps const %u: reason %u, want %u", k, pr[1].consts[k].why, pwhy[k]);
        CHECK(pr[1].consts[1].stage == 1 && pr[1].consts[1].used == 1 && pr[1].consts[3].id == id[P_F] && pr[1].consts[3].nelem == 1,
              "the sampler with Elements 4 binds stage 1; f with Elements 0 has one leaf");
        CHECK(pr[2].kind == FXM_P_PRESHADER && pr[2].nread == 2, "the preshader");
        q = pr[2].consts;
        CHECK(q[0].id == id[P_I5] && q[0].from == FX2_T_INT && q[0].to == FX2_T_FLOAT && q[0].layout == FXM_L_SCALAR && q[0].used == 2 &&
              q[0].nelem == 5 && q[0].leaves[1].reg == 1, "i5 int>float scalar 2/5");
        CHECK(q[1].why == 2, "set s in a preshader: reason 2");
    }
    /* §3.5: state sources, deps, versions */
    const fx2_pass *ps = &E.fx.techs[0].passes[0];
    fxm_source s3 = fxm_state_source(m, &ps->states[3]), s4 = fxm_state_source(m, &ps->states[4]), s0 = fxm_state_source(m, &ps->states[0]);
    CHECK(s0.src == FXM_S_SHADER && s0.prog == &pr[0] && s0.nres == 1, "a shader state");
    CHECK(s3.src == FXM_S_PARAM && s3.id == id_of(m, "texa[1]") && s4.src == FXM_S_LITERAL, "usage 1, literal");
    fxm_source sb = fxm_state_source(m, &fxm_node_get(m, id[P_SMP])->block->states[0]);
    CHECK(sb.src == FXM_S_PARAM && sb.id == id[P_TEX0], "a sampler block's usage 1");
    const uint32_t *deps;
    uint32_t nd = fxm_pass_deps(m, p0, &deps);
    uint32_t want[] = { id[P_F], id[P_V4], id[P_M44], id[P_I5], id[P_V4A], id[P_TEX0], id[P_TEXA] };
    int okd = nd == 7;
    for (uint32_t k = 0; okd && k < 7; k++) okd = deps[k] == want[k];
    CHECK(okd, "deps: f v4 m44 i5 v4a albedo_tex (through the sampler) texa (through texa[1]); got %u", nd);
    uint32_t vs, pss;
    fxm_tech_versions(m, t0, &vs, &pss);
    CHECK(vs == 0xFFFE0101u && pss == 0xFFFF0200u, "technique versions");
    CHECK(fxm_shared_slot(m, id[P_SHI]) == 1 && fxm_shared_slot(m, id[P_I]) == 0 && fxm_pool_slots(pool) == 1, "the shared parameter");

    /* without a pool nothing is shared and versions come from the effect */
    fxm_effect *solo;
    char err[128];
    CHECK(!fxm_effect_new(&solo, &E.fx, NULL, err, sizeof err), "%s", err);
    CHECK(fxm_shared_slot(solo, id[P_SHI]) == 0 && fxm_clock(solo) == (uint64_t)P_NP, "no pool: no slot, a clock of its own");
    fxm_effect_free(solo, NULL, NULL);
    droplog dl = { 0 };
    fxm_effect_free(m, drop_rec, &dl);
    CHECK(dl.n == 4 && dl.p[0] == 0x3000 && dl.p[1] == 0x1001 && dl.p[2] == 0x2000 && dl.p[3] == 0x4000,
          "free drops every object pointer of its own, in id order (%u drops)", dl.n);
    fxm_pool_free(pool);
    fx2_free(&E.fx);
    free(buf);
}

/* §6.2's last point: matching on synthetic effects */
static void test_pool_match(void){
    static const ts m1[] = { T_(FLOAT, SCALAR, "a", NULL, 0, 1, 1) };
    static const ts m2[] = { T_(FLOAT, SCALAR, "a", NULL, 0, 1, 1), T_(FLOAT, SCALAR, "b", NULL, 0, 1, 1) };
    static const uint32_t one = 1, two[2] = { 0, 0 }, three[3] = { 1, 2, 3 }, four[4] = { 0, 0, 0, 0 }, f15 = 0x3FC00000u, f25 = 0x40200000u, zero = 0;
    const pspec A[] = {
        { T_(FLOAT, SCALAR, "s1", "S", 0, 1, 1), 1, 1, &f15 }, { T_(INT, SCALAR, "s2", NULL, 3, 1, 1), 1, 3, three },
        { { FX2_T_VOID, FX2_C_STRUCT, "s3", NULL, 0, 0, 0, 1, m1 }, 1, 1, &zero }, { T_(SAMPLER, OBJECT, "smp", NULL, 0, 0, 0), 1, 1, &zero },
        { T_(TEXTURE, OBJECT, "tx", NULL, 0, 0, 0), 1, 1, &one } };
    const pspec B[] = {
        { T_(INT, SCALAR, "s1", "S", 0, 1, 1), 1, 1, &one }, { T_(INT, SCALAR, "s2", NULL, 4, 1, 1), 1, 4, four },
        { { FX2_T_VOID, FX2_C_STRUCT, "s3", NULL, 0, 0, 0, 2, m2 }, 1, 2, two }, { T_(SAMPLER, OBJECT, "smp", NULL, 0, 0, 0), 1, 1, &zero },
        { T_(TEXTURE, OBJECT, "tx", "OTHER SEMANTIC", 0, 0, 0), 1, 1, &one } };
    const pspec C[] = { { T_(FLOAT, SCALAR, "s1", "s", 0, 1, 1), 1, 1, &f25 }, { T_(FLOAT, SCALAR, "s1", NULL, 0, 1, 1), 0, 1, &f25 } };
    uint32_t sa, sb, sc;
    uint8_t *ba = make_params(A, 5, 2, &sa), *bbuf = make_params(B, 5, 2, &sb), *bc = make_params(C, 2, 1, &sc);
    eff ea, eb, ec;
    if (eff_parse(&ea, ba, sa, "A") || eff_parse(&eb, bbuf, sb, "B") || eff_parse(&ec, bc, sc, "C")) return;
    fxm_pool *pool = fxm_pool_new();
    eff_model(&ea, pool); eff_model(&eb, pool);
    if (!ea.m || !eb.m) return;
    uint64_t va0 = fxm_version(ea.m, fxm_param(ea.m, 0, 0));
    eff_model(&ec, pool);
    if (!ec.m) return;
    uint32_t a[5], b[5], c[2];
    for (uint32_t i = 0; i < 5; i++){ a[i] = fxm_param(ea.m, 0, i); b[i] = fxm_param(eb.m, 0, i); }
    c[0] = fxm_param(ec.m, 0, 0); c[1] = fxm_param(ec.m, 0, 1);
    CHECK(fxm_shared_slot(ea.m, a[0]) == 1 && fxm_shared_slot(ea.m, a[1]) == 2 && fxm_shared_slot(ea.m, a[2]) == 3, "A opens slots 0, 1, 2");
    CHECK(fxm_shared_slot(ea.m, a[3]) == 0 && fxm_shared_slot(eb.m, b[3]) == 0, "a shared sampler gets no slot");
    CHECK(fxm_shared_slot(ea.m, a[4]) == 4 && fxm_shared_slot(eb.m, b[4]) == 4, "the texture is one slot (semantics are not compared)");
    CHECK(fxm_shared_slot(eb.m, b[0]) == 5 && fxm_shared_slot(eb.m, b[1]) == 6 && fxm_shared_slot(eb.m, b[2]) == 7,
          "another type, element count or member count: other slots");
    CHECK(fxm_shared_slot(ec.m, c[0]) == 1 && fxm_data(ec.m, c[0])[0] == f15 && fxm_shared_slot(ec.m, c[1]) == 0 && fxm_data(ec.m, c[1])[0] == f25,
          "a joiner takes the slot's value, not its default; a twin without the flag keeps its own");
    CHECK(fxm_version(ea.m, a[0]) > va0 && fxm_version(ea.m, a[0]) == fxm_clock(ec.m) - 1 && fxm_version(ec.m, c[0]) == fxm_version(ea.m, a[0]),
          "joining a live slot raises its version to ++counter, in parameter order (W: set_dirty)");
    CHECK(fxm_set_float(ec.m, c[0], f25, NULL) == FXM_OK && fxm_data(ea.m, a[0])[0] == f25 && fxm_version(ea.m, a[0]) == fxm_version(ec.m, c[0]),
          "a write through one holder reaches the other");
    /* lifetime and drop on the texture slot: dropped once, when its last holder goes */
    droplog l1 = { 0 }, l2 = { 0 }, l3 = { 0 }, l4 = { 0 };
    CHECK(fxm_set_object(eb.m, b[4], 0xABC0, NULL, NULL) == FXM_OK && fxm_data(ea.m, a[4])[0] == 0xABC0, "SetTexture on a shared texture");
    fxm_effect_free(ea.m, drop_rec, &l1); ea.m = NULL;
    CHECK(l1.n == 0 && fxm_data(eb.m, b[4])[0] == 0xABC0, "the other holder keeps the pointer; nothing dropped");
    CHECK(fxm_data(ec.m, c[0])[0] == f25 && fxm_pool_slots(pool) == 7, "slot 0 lives on in C");
    eff_model(&ea, pool);
    if (!ea.m) return;
    CHECK(fxm_shared_slot(ea.m, a[1]) == 2 && fxm_data(ea.m, a[1])[2] == 3 && fxm_data(ea.m, a[0])[0] == f25,
          "A again: slot number 1 reopens with its default, slot 0 kept its value");
    CHECK(fxm_data(ea.m, a[4])[0] == 0xABC0, "A again joins the live texture slot and takes its pointer");
    fxm_effect_free(eb.m, drop_rec, &l2); eb.m = NULL;
    CHECK(l2.n == 0, "B goes: A still holds the texture");
    fxm_effect_free(ea.m, drop_rec, &l3);
    CHECK(l3.n == 1 && l3.p[0] == 0xABC0, "the last holder goes: the pointer is dropped once (%u drops)", l3.n);
    fxm_effect_free(ec.m, drop_rec, &l4);
    CHECK(l4.n == 0, "no object pointers in C");
    fxm_pool_free(pool);
    fx2_free(&ea.fx); fx2_free(&eb.fx); fx2_free(&ec.fx);
    free(ba); free(bbuf); free(bc);
}

/* §2.3: "" names no parameter and no annotation, and NULL no annotation (W: get_parameter_by_name and
 * get_annotation_by_name return NULL), so a CTAB constant or a usage-1 reference named "" matches nothing
 * either; GetParameterByName(parent, NULL) gives the parent itself (W returns its handle before any lookup);
 * a technique or pass by NULL is none, by "" the unnamed one (W: get_technique_by_name, GetPassByName) */
static void test_unnamed(void){
    eff E;
    uint32_t size, np;
    uint8_t *buf = make_unnamed(&size);
    if (eff_parse(&E, buf, size, "unnamed")){ free(buf); return; }
    if (eff_model(&E, NULL)){ fx2_free(&E.fx); free(buf); return; }
    fxm_effect *m = E.m;
    uint32_t p0 = fxm_param(m, 0, 0), st = fxm_param(m, 0, 1), m0 = fxm_param(m, st, 0), pass = fxm_pass(m, fxm_tech(m, 0), 0);
    CHECK(fxm_node_get(m, p0)->path.n == 0 && fxm_node_get(m, m0)->path.n == 3, "unnamed: the paths \"\" and \"st.\"");
    CHECK(!fxm_by_name(m, 0, (const uint8_t *)"", 0) && !fxm_by_name(m, 0, NULL, 0), "GetParameterByName \"\" or NULL: none");
    CHECK(!fxm_by_name(m, st, (const uint8_t *)"", 0), "GetParameterByName \"\" under a parent: none");
    uint32_t an0 = fxm_annot(m, p0, 0), t1 = fxm_tech(m, 1);
    CHECK(fxm_by_name(m, st, NULL, 0) == st && fxm_by_name(m, m0, NULL, 0) == m0 && fxm_by_name(m, an0, NULL, 0) == an0 &&
          !fxm_by_name(m, pass, NULL, 0) && !fxm_by_name(m, 0, NULL, 0), "GetParameterByName NULL: the parent itself; none under a pass or with parent 0");
    CHECK(t1 && !fxm_tech_by_name(m, NULL, 0) && fxm_tech_by_name(m, (const uint8_t *)"", 0) == t1 && !fxm_pass_by_name(m, t1, NULL, 0) &&
          fxm_pass_by_name(m, t1, (const uint8_t *)"", 0) == fxm_pass(m, t1, 0), "a technique and a pass by NULL: none; by \"\": the unnamed ones");
    CHECK(fxm_by_name(m, 0, (const uint8_t *)"st.", 3) == m0 && fxm_by_name(m, st, (const uint8_t *)"y", 1) == m0 + 1,
          "the unnamed member by its path, y under its parent");
    CHECK(!fxm_annot_by_name(m, p0, (const uint8_t *)"", 0) && !fxm_annot_by_name(m, p0, NULL, 0) &&
          fxm_annot_by_name(m, p0, (const uint8_t *)"k", 1) == fxm_annot(m, p0, 1), "GetAnnotationByName \"\" or NULL: none");
    const fxm_prog *pr = fxm_progs(m, &np);
    CHECK(np == 1 && pr[0].nread == 1 && pr[0].consts[0].name.p && !pr[0].consts[0].name.n && pr[0].consts[0].why == 1,
          "a CTAB constant named \"\" matches no parameter (reason 1)");
    fxm_source s1 = fxm_state_source(m, &E.fx.techs[0].passes[0].states[1]);
    CHECK(s1.src == FXM_S_PARAM && s1.id == 0 && fxm_pass_deps(m, pass, NULL) == 0, "a usage-1 name \"\" resolves to none; no deps");
    fxm_effect_free(m, NULL, NULL);
    fx2_free(&E.fx);
    free(buf);
}

/* §3.5: SAMPLER[i] = <sampler> (row 178, usage 1) applies that sampler's states, so what they read is in the
 * pass's deps: smp's block (ta) and element 0's block of the array smpa (tb: the model's choice, W reads the
 * array's NULL data pointer), not tc; TEXTURE[2] = <smpb> applies none of smpb's states, so not td either */
static void test_sampler_ref(void){
    eff E;
    uint32_t size;
    uint8_t *buf = make_sampler_ref(&size);
    if (eff_parse(&E, buf, size, "sampler_ref")){ free(buf); return; }
    if (eff_model(&E, NULL)){ fx2_free(&E.fx); free(buf); return; }
    fxm_effect *m = E.m;
    const uint32_t *deps;
    uint32_t nd = fxm_pass_deps(m, fxm_pass(m, fxm_tech(m, 0), 0), &deps);
    fxm_source s0 = fxm_state_source(m, &E.fx.techs[0].passes[0].states[0]);
    CHECK(s0.src == FXM_S_PARAM && s0.id && s0.id == id_of(m, "smp") && fxm_node_get(m, s0.id)->block->nstates == 1, "SAMPLER[0] = <smp>");
    fxm_source s2 = fxm_state_source(m, &E.fx.techs[0].passes[0].states[2]);
    CHECK(s2.src == FXM_S_PARAM && s2.id && s2.id == id_of(m, "smpb"), "TEXTURE[2] = <smpb>");
    CHECK(nd == 2 && deps[0] == id_of(m, "ta") && deps[1] == id_of(m, "tb"),
          "SAMPLER[i] = <sampler>: the deps are ta (smp's block) and tb (smpa[0]'s), not tc, not td (TEXTURE[2]); got %u", nd);
    fxm_effect_free(m, NULL, NULL);
    fx2_free(&E.fx);
    free(buf);
}

/* §2.6: a creation the memory bound refuses leaves the pool as it was: its slots, their versions and the
 * counter (the storage and the pool come last, as W syncs the pool at the end of d3dx_parse_effect) */
static void test_refused(void){
    static const uint32_t zero = 0;
    const pspec X[] = { { T_(FLOAT, SCALAR, "s", NULL, 0, 1, 1), FXM_SHARED, 1, &zero } };
    uint32_t sx, sy;
    uint8_t *bx = make_params(X, 1, 0, &sx), *by = make_blowup(2000, &sy);
    eff ex, ey;
    if (eff_parse(&ex, bx, sx, "X")){ free(bx); free(by); return; }
    if (eff_parse(&ey, by, sy, "blowup")){ fx2_free(&ex.fx); free(bx); free(by); return; }
    fxm_pool *pool = fxm_pool_new();
    if (!eff_model(&ex, pool)){
        uint32_t s = fxm_param(ex.m, 0, 0);
        uint64_t v = fxm_version(ex.m, s), c = fxm_clock(ex.m);
        uint32_t slots = fxm_pool_slots(pool);
        char err[160];
        fxm_effect *y = NULL;
        int r = fxm_effect_new(&y, &ey.fx, pool, err, sizeof err);
        CHECK(r == -1 && !y && !strncmp(err, "out of memory", 13), "the blown-up effect is refused as out of memory (%d: %s)", r, err);
        CHECK(fxm_version(ex.m, s) == v && fxm_clock(ex.m) == c && fxm_pool_slots(pool) == slots && fxm_shared_slot(ex.m, s) == 1,
              "a refused creation leaves the slot's version (%llu, was %llu) and the counter (%llu, was %llu) alone",
              (unsigned long long)fxm_version(ex.m, s), (unsigned long long)v, (unsigned long long)fxm_clock(ex.m), (unsigned long long)c);
        if (y) fxm_effect_free(y, NULL, NULL);
        fxm_effect_free(ex.m, NULL, NULL);
    }
    fxm_pool_free(pool);
    fx2_free(&ex.fx); fx2_free(&ey.fx);
    free(bx); free(by);
}

/* ======================= effect files ======================= */

static int eff_by_name(const void *a, const void *b){ return strcmp(((const eff *)a)->name, ((const eff *)b)->name); }

/* ---- the rows the effects use (§4.5) ---- */
static unsigned long g_past;          /* states whose operation is past the table */
static void rows_state(const fx2_state *st, uint8_t *used){
    const fx2_state_info *si = fx2_state_get(st->op);
    uint32_t v;
    if (!si){ g_past++; return; }
    used[st->op] = 1;
    CHECK(sdk_row(st->op, &v) && v == si->d3d, "row %u %s: no SDK value", st->op, si->name);
    CHECK(!si->nindex || st->index < si->nindex, "row %u %s: index %u, the table allows %u", st->op, si->name, st->index, si->nindex);
}
static void rows_value(const fx2_value *v, uint8_t *used){
    if (v->kind == FX2_V_SAMPLER)
        for (uint32_t e = 0; e < v->n; e++) for (uint32_t k = 0; k < v->blocks[e].nstates; k++) rows_state(&v->blocks[e].states[k], used);
    if (v->kind == FX2_V_STRUCT)
        for (uint64_t i = 0; i < (uint64_t)v->n * v->t->nmembers; i++) rows_value(&v->members[i], used);
}
static void test_rows(const eff *x, uint32_t n, int mw){
    uint8_t used[FX2_NSTATES] = { 0 };
    for (uint32_t i = 0; i < n; i++){
        const fx2_effect *fx = &x[i].fx;
        for (uint32_t p = 0; p < fx->nparams; p++) rows_value(&fx->params[p].v, used);
        for (uint32_t t = 0; t < fx->ntechs; t++)
            for (uint32_t j = 0; j < fx->techs[t].npasses; j++)
                for (uint32_t k = 0; k < fx->techs[t].passes[j].nstates; k++) rows_state(&fx->techs[t].passes[j].states[k], used);
    }
    uint32_t nu = 0;
    for (uint32_t op = 0; op < FX2_NSTATES; op++) nu += used[op];
    printf("rows: the effects use %u distinct state rows, each with its SDK value and an index the row allows; %lu operations past the table\n",
           nu, g_past);
    if (mw) CHECK(nu == 46 && !g_past, "%u distinct rows and %lu past the table, fx-model §4.5 says 46 and none", nu, g_past);
}

/* ---- §6.1: lookups on every node ---- */
static int path_is(const fxm_effect *m, uint32_t id, const uint8_t *s, uint32_t n){
    const fxm_node *g = fxm_node_get(m, id);
    return g && g->path.n == n && (!n || !memcmp(g->path.p, s, n));
}
/* a query that names no node: 0, or a node whose path is exactly those bytes */
static int none_or_exact(const fxm_effect *m, const char *q, size_t n){
    uint32_t got = fxm_by_name(m, 0, (const uint8_t *)q, (uint32_t)n);
    return !got || path_is(m, got, (const uint8_t *)q, (uint32_t)n);
}
static int ieq_str(fx2_str a, fx2_str b){
    if (!a.p || !b.p || a.n != b.n) return 0;
    for (uint32_t i = 0; i < a.n; i++){
        uint8_t p = a.p[i], q = b.p[i];
        if (p >= 'a' && p <= 'z') p -= 32;
        if (q >= 'a' && q <= 'z') q -= 32;
        if (p != q) return 0;
    }
    return 1;
}
static int str_same(fx2_str a, fx2_str b){ return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }

static void test_lookups(const eff *x, uint32_t n, int mw){
    unsigned long kinds[8] = { 0 }, total = 0;
    char *q = xalloc(1 << 16);
    for (uint32_t i = 0; i < n; i++){
        const fxm_effect *m = x[i].m;
        const fx2_effect *fx = &x[i].fx;
        uint32_t nn = fxm_nodes(m);
        total += nn;
        for (uint32_t id = 1; id <= nn; id++){
            const fxm_node *nd = fxm_node_get(m, id);
            kinds[nd->kind]++;
            if (nd->kind > FXM_K_MEMBER || !nd->path.n) continue;            /* "" names nothing (§2.3) */
            uint32_t got = fxm_by_name(m, 0, nd->path.p, nd->path.n);
            CHECK(got && got <= id && path_is(m, got, nd->path.p, nd->path.n), "%s #%u: not found by its path", x[i].name, id);
            if (nd->kind == FXM_K_MEMBER && nd->name.n){
                uint32_t g2 = fxm_by_name(m, nd->parent, nd->name.p, nd->name.n);
                CHECK(g2 == got, "%s #%u: not found by its name under its parent", x[i].name, id);
            }
            if (nd->path.n + 32 > (1 << 16)) continue;
            memcpy(q, nd->path.p, nd->path.n);
            size_t pn = nd->path.n;
            if (nd->elements){
                CHECK(none_or_exact(m, q, pn + (size_t)snprintf(q + pn, 32, "[%u]", nd->elements)), "%s #%u: [n] out of range found", x[i].name, id);
                CHECK(none_or_exact(m, q, pn + (size_t)snprintf(q + pn, 32, "[]")), "%s #%u: [] found", x[i].name, id);
                CHECK(none_or_exact(m, q, pn + (size_t)snprintf(q + pn, 32, "[00]")), "%s #%u: a leading zero found", x[i].name, id);
                CHECK(none_or_exact(m, q, pn + (size_t)snprintf(q + pn, 32, "[ 0]")), "%s #%u: [ 0] found", x[i].name, id);
                for (uint32_t e = 0; e < nd->elements; e++){
                    uint32_t c = fxm_element(m, id, e);
                    const fxm_node *cn = fxm_node_get(m, c);
                    CHECK(cn && cn->kind == FXM_K_ELEMENT && cn->parent == id && cn->index == e, "%s #%u: element %u", x[i].name, id, e);
                }
                CHECK(!fxm_element(m, id, nd->elements) && !fxm_param(m, id, 0), "%s #%u: past the elements, or fxm_param on an array", x[i].name, id);
            }
            int letters = 0;
            for (size_t k = 0; k < pn; k++)
                if ((q[k] >= 'a' && q[k] <= 'z') || (q[k] >= 'A' && q[k] <= 'Z')){ q[k] ^= 0x20; letters = 1; }
            if (letters) CHECK(none_or_exact(m, q, pn), "%s #%u: found with its case changed", x[i].name, id);
            q[0] = 1; memcpy(q + 1, nd->path.p, pn);
            CHECK(none_or_exact(m, q, pn + 1), "%s #%u: an unknown name found", x[i].name, id);
        }
        for (uint32_t p = 0; p <= fx->nparams; p++){
            uint32_t want = p < fx->nparams ? fxm_param(m, 0, p) : 0;
            CHECK(fxm_element(m, 0, p) == want, "%s: fxm_element(0, %u)", x[i].name, p);
            if (p == fx->nparams) CHECK(!fxm_param(m, 0, p), "%s: fxm_param(0, P)", x[i].name);
            else {
                const fxm_node *nd = fxm_node_get(m, want);
                CHECK(nd && nd->kind == FXM_K_PARAM && nd->index == p && nd->t == fx->params[p].t, "%s: parameter %u", x[i].name, p);
            }
        }
        /* semantics: as stored, upper case and lower case give the first with that semantic (stricmp) */
        uint32_t nosem = 0;
        for (uint32_t p = 0; p < fx->nparams; p++){
            uint32_t id = fxm_param(m, 0, p);
            fx2_str sem = fxm_node_get(m, id)->sem;
            if (!sem.p){ if (!nosem) nosem = id; continue; }
            uint32_t first = 0;
            for (uint32_t r = 0; r < fx->nparams && !first; r++)
                if (ieq_str(fxm_node_get(m, fxm_param(m, 0, r))->sem, sem)) first = fxm_param(m, 0, r);
            if (sem.n + 1 > (1 << 16)) continue;
            CHECK(fxm_by_semantic(m, 0, sem.p, sem.n) == first, "%s #%u: by its semantic", x[i].name, id);
            for (uint32_t k = 0; k < sem.n; k++) q[k] = (char)(sem.p[k] >= 'a' && sem.p[k] <= 'z' ? sem.p[k] - 32 : sem.p[k]);
            CHECK(fxm_by_semantic(m, 0, (const uint8_t *)q, sem.n) == first, "%s #%u: by its semantic in upper case", x[i].name, id);
            for (uint32_t k = 0; k < sem.n; k++) q[k] = (char)(sem.p[k] >= 'A' && sem.p[k] <= 'Z' ? sem.p[k] + 32 : sem.p[k]);
            CHECK(fxm_by_semantic(m, 0, (const uint8_t *)q, sem.n) == first, "%s #%u: by its semantic in lower case", x[i].name, id);
        }
        CHECK(fxm_by_semantic(m, 0, NULL, 0) == nosem, "%s: NULL gives the first parameter without a semantic", x[i].name);
        /* techniques, passes and their annotations */
        for (uint32_t t = 0; t <= fx->ntechs; t++){
            uint32_t tid = fxm_tech(m, t);
            if (t == fx->ntechs){ CHECK(!tid && !fxm_tech_by_name(m, NULL, 0), "%s: past the techniques, or by NULL", x[i].name); break; }
            const fx2_tech *te = &fx->techs[t];
            uint32_t byname = 0;
            for (uint32_t r = 0; r < fx->ntechs && !byname; r++) if (str_same(fx->techs[r].name, te->name)) byname = fxm_tech(m, r);
            const uint8_t *tq = te->name.p ? te->name.p : (const uint8_t *)"";     /* offset 0 is found by "" (§2.3) */
            CHECK(tid && fxm_tech_by_name(m, tq, te->name.n) == byname, "%s technique %u: by name", x[i].name, t);
            for (uint32_t k = 0; k < te->nannots; k++){
                const fx2_annot *a = &te->annots[k];
                uint32_t want = 0;
                for (uint32_t r = 0; r < te->nannots && !want && a->t->name.n; r++)     /* "" names nothing (§2.3) */
                    if (str_same(te->annots[r].t->name, a->t->name)) want = fxm_annot(m, tid, r);
                CHECK(fxm_annot_by_name(m, tid, a->t->name.p, a->t->name.n) == want, "%s technique %u annotation %u: by name", x[i].name, t, k);
                uint32_t w;
                if (a->t->type == FX2_T_INT && a->t->cls <= FX2_C_MATRIX_COLUMNS && a->t->rows == 1 && a->t->cols == 1 && !a->t->elements)
                    CHECK(fxm_get_int(m, fxm_annot(m, tid, k), &w) == FXM_OK && w == fx2_rd32(a->v.data),
                          "%s technique %u annotation %u: GetInt gives the file's dword", x[i].name, t, k);
            }
            for (uint32_t j = 0; j <= te->npasses; j++){
                uint32_t pid = fxm_pass(m, tid, j);
                if (j == te->npasses){
                    CHECK(!pid && !fxm_pass_by_name(m, tid, NULL, 0), "%s technique %u: past the passes, or by NULL", x[i].name, t);
                    break;
                }
                uint32_t want = 0;
                for (uint32_t r = 0; r < te->npasses && !want; r++) if (str_same(te->passes[r].name, te->passes[j].name)) want = fxm_pass(m, tid, r);
                const fxm_node *pn = fxm_node_get(m, pid);
                CHECK(pn && pn->kind == FXM_K_PASS && pn->parent == tid && pn->index == j, "%s technique %u pass %u", x[i].name, t, j);
                const uint8_t *pq = te->passes[j].name.p ? te->passes[j].name.p : (const uint8_t *)"";
                CHECK(fxm_pass_by_name(m, tid, pq, te->passes[j].name.n) == want, "%s technique %u pass %u: by name", x[i].name, t, j);
            }
        }
    }
    free(q);
    printf("lookups: %lu nodes (%lu parameters, %lu elements, %lu members, %lu annotations, %lu techniques, %lu passes)\n", total,
           kinds[FXM_K_PARAM], kinds[FXM_K_ELEMENT], kinds[FXM_K_MEMBER], kinds[FXM_K_ANNOT], kinds[FXM_K_TECH], kinds[FXM_K_PASS]);
    if (mw) CHECK(total == 951 && kinds[FXM_K_PARAM] == 533 && kinds[FXM_K_ELEMENT] == 154 && kinds[FXM_K_MEMBER] == 0 &&
                  kinds[FXM_K_ANNOT] == 101 && kinds[FXM_K_TECH] == 76 && kinds[FXM_K_PASS] == 87, "fx-model §2.2's node counts");
}
/* ---- §6.1: the CTAB match over the effects, §3.6's table, deps and versions ---- */
typedef struct { uint32_t kind, set, from, to, layout; unsigned long n, cut; } cat;
static void test_ctab(const eff *x, uint32_t n, int mw){
    cat c[32];
    uint32_t nc = 0;
    unsigned long progs[2] = { 0 }, ctabs[2] = { 0 }, consts = 0, unmatched = 0, s2d = 0, scube = 0, pixel = 0;
    unsigned long deps = 0, passes = 0, maxdeps = 0;
    struct { uint32_t vs, ps; unsigned long n; } ver[16];
    uint32_t nv = 0;
    for (uint32_t i = 0; i < n; i++){
        const fxm_effect *m = x[i].m;
        uint32_t np;
        const fxm_prog *pr = fxm_progs(m, &np);
        for (uint32_t k = 0; k < np; k++){
            progs[pr[k].kind]++;
            ctabs[pr[k].kind] += pr[k].ctab != 0;
            CHECK(pr[k].res->target && !pr[k].res->usage, "%s program %u: not a usage-0 resource with a state", x[i].name, k);
            fxm_source src = fxm_state_source(m, pr[k].res->target);
            const fx2_res *last = pr[k].res;                                 /* §3.5: the last resource decides */
            while (last->next) last = last->next;
            CHECK(src.res == last && (src.prog == &pr[k]) == (last == pr[k].res), "%s program %u: its state's source",
                  x[i].name, k);
            for (uint32_t j = 0; j < pr[k].nread; j++){
                const fxm_const *q = &pr[k].consts[j];
                consts++;
                if (q->why){
                    unmatched++;
                    if (mw) CHECK(0, "%s program %u constant %u: reason %u", x[i].name, k, j, q->why);
                    CHECK(!q->id && !q->nleaves && q->why <= 7 && q->why != 6, "%s program %u constant %u: an unmatched entry", x[i].name, k, j);
                    continue;
                }
                const fxm_node *pn = fxm_node_get(m, q->id);
                CHECK(pn && pn->kind == FXM_K_PARAM && q->nleaves == q->nelem && q->nelem == (pn->elements ? pn->elements : 1),
                      "%s program %u constant %u: matched node", x[i].name, k, j);
                unsigned long got = 0;
                for (uint32_t l = 0; l < q->nleaves; l++){
                    got += q->leaves[l].count;
                    CHECK(q->leaves[l].id == (pn->elements ? fxm_element(m, q->id, l) : q->id), "%s: leaf %u", x[i].name, l);
                }
                unsigned long cnt = q->c.count, ce = q->c.elements ? q->c.elements : 1;
                if (q->layout == FXM_L_SAMPLER && cnt > ce) cnt = ce;                /* §3.4: cut to its own elements */
                CHECK(got == cnt || (got < cnt && got == (unsigned long)q->per * q->nelem), "%s: registers of a constant", x[i].name);
                if (q->layout == FXM_L_SAMPLER){
                    s2d += q->c.type == FX2_T_SAMPLER2D; scube += q->c.type == FX2_T_SAMPLERCUBE;
                    pixel += pr[k].version >> 16 == 0xFFFF && q->stage < 16;
                }
                uint32_t z = 0;
                while (z < nc && !(c[z].kind == pr[k].kind && c[z].set == q->c.set && c[z].from == q->from && c[z].to == q->to && c[z].layout == q->layout)) z++;
                if (z == nc && nc < 32){ c[nc].kind = pr[k].kind; c[nc].set = q->c.set; c[nc].from = q->from; c[nc].to = q->to; c[nc].layout = q->layout; c[nc].n = c[nc].cut = 0; nc++; }
                if (z < 32){ c[z].n++; c[z].cut += got < (unsigned long)q->per * q->nelem; }
            }
            CHECK(pr[k].nread <= pr[k].nconsts && (!mw || !pr[k].ctab || pr[k].nread == pr[k].nconsts),
                  "%s program %u: an entry outside the table", x[i].name, k);
        }
        for (uint32_t id = 1; id <= fxm_nodes(m); id++){
            const fxm_node *nd = fxm_node_get(m, id);
            if (nd->kind == FXM_K_PASS){
                const uint32_t *d;
                uint32_t k = fxm_pass_deps(m, id, &d);
                passes++; deps += k; if (k > maxdeps) maxdeps = k;
                for (uint32_t j = 0; j < k; j++){
                    const fxm_node *dn = fxm_node_get(m, d[j]);
                    CHECK(dn && dn->kind == FXM_K_PARAM && dn->bytes && (!j || d[j - 1] < d[j]), "%s pass #%u: deps sorted, top level, bytes", x[i].name, id);
                }
            } else if (nd->kind == FXM_K_TECH){
                uint32_t vs, ps, z = 0;
                fxm_tech_versions(m, id, &vs, &ps);
                while (z < nv && !(ver[z].vs == vs && ver[z].ps == ps)) z++;
                if (z == nv && nv < 16){ ver[nv].vs = vs; ver[nv].ps = ps; ver[nv].n = 0; nv++; }
                if (z < 16) ver[z].n++;
            }
        }
    }
    printf("ctab: %lu programs (%lu shaders, %lu preshaders), %lu and %lu with a CTAB; %lu constants, %lu unmatched\n",
           progs[0] + progs[1], progs[0], progs[1], ctabs[0], ctabs[1], consts, unmatched);
    static const char *const lay[] = { "scalar", "vector", "rows", "columns", "sampler" };
    static const char *const ty[] = { "-", "bool", "int", "float" };
    for (uint32_t z = 0; z < nc; z++)
        printf("  %s %c %s>%s %s: %lu, %lu with registers cut\n", c[z].kind ? "preshader" : "shader", "bics"[c[z].set & 3],
               ty[c[z].from & 3], ty[c[z].to & 3], lay[c[z].layout % 5], c[z].n, c[z].cut);
    printf("deps: %lu passes, %lu ids, at most %lu in one pass\n", passes, deps, maxdeps);
    char a1[32], a2[32];
    for (uint32_t z = 0; z < nv; z++)
        printf("  techniques %s/%s: %lu\n", vname(a1, sizeof a1, ver[z].vs) ? a1 : "-", vname(a2, sizeof a2, ver[z].ps) ? a2 : "-", ver[z].n);
    if (!mw) return;
    CHECK(progs[0] == 142 && progs[1] == 616 && ctabs[0] == 140 && ctabs[1] == 616, "§3.1's program counts");
    CHECK(consts == 1355 && unmatched == 0, "§3.2: 1,355 constants, all matched");
    static const struct { uint32_t kind, set, from, to, layout; unsigned long n, cut; } want[] = {
        { FXM_P_PRESHADER, 2, FX2_T_INT, FX2_T_FLOAT, FXM_L_SCALAR, 597, 288 },
        { FXM_P_PRESHADER, 2, FX2_T_FLOAT, FX2_T_FLOAT, FXM_L_COLUMNS, 7, 0 },
        { FXM_P_PRESHADER, 2, FX2_T_FLOAT, FX2_T_FLOAT, FXM_L_VECTOR, 6, 0 },
        { FXM_P_SHADER, 2, FX2_T_FLOAT, FX2_T_FLOAT, FXM_L_VECTOR, 365, 3 },
        { FXM_P_SHADER, 2, FX2_T_FLOAT, FX2_T_FLOAT, FXM_L_COLUMNS, 110, 26 },
        { FXM_P_SHADER, 2, FX2_T_FLOAT, FX2_T_FLOAT, FXM_L_SCALAR, 99, 0 },
        { FXM_P_SHADER, 3, 0, 0, FXM_L_SAMPLER, 171, 0 } };
    CHECK(nc == 7, "§3.6 has 7 kinds of constant, here %u", nc);
    for (size_t w = 0; w < sizeof want / sizeof want[0]; w++){
        uint32_t z = 0;
        while (z < nc && !(c[z].kind == want[w].kind && c[z].set == want[w].set && c[z].from == want[w].from && c[z].to == want[w].to && c[z].layout == want[w].layout)) z++;
        CHECK(z < nc && c[z].n == want[w].n && c[z].cut == want[w].cut, "§3.6 row %zu: %lu and %lu cut", w, z < nc ? c[z].n : 0, z < nc ? c[z].cut : 0);
    }
    CHECK(s2d == 165 && scube == 6 && pixel == 171, "§3.4: 165 SAMPLER2D, 6 SAMPLERCUBE, all in pixel shaders");
    CHECK(passes == 87 && deps == 1145 && maxdeps == 29, "§7: 87 passes, 1,145 deps, at most 29");
    static const struct { uint32_t vs, ps; unsigned long n; } vw[] = {
        { 0, 0, 16 }, { 0xFFFE0101u, 0xFFFF0101u, 21 }, { 0xFFFE0101u, 0xFFFF0104u, 1 }, { 0xFFFE0101u, 0xFFFF0200u, 18 },
        { 0xFFFE0101u, 0xFFFF0201u, 18 }, { 0xFFFE0300u, 0xFFFF0300u, 2 } };
    CHECK(nv == 6, "§7: 6 version pairs, here %u", nv);
    for (size_t w = 0; w < sizeof vw / sizeof vw[0]; w++){
        uint32_t z = 0;
        while (z < nv && !(ver[z].vs == vw[w].vs && ver[z].ps == vw[w].ps)) z++;
        CHECK(z < nv && ver[z].n == vw[w].n, "§7 version pair %zu", w);
    }
}

/* ---- §6.2: the pool test ---- */
typedef struct { uint32_t e, id; } hold;

/* the holders of slot s (number), in input order then id order */
static uint32_t holders(const eff *x, uint32_t n, uint32_t s, hold *h, uint32_t max){
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t p = 0; x[i].m && p < x[i].fx.nparams; p++){
            uint32_t id = fxm_param(x[i].m, 0, p);
            if (fxm_shared_slot(x[i].m, id) == s + 1){ if (k < max){ h[k].e = i; h[k].id = id; } k++; }
        }
    return k;
}
static uint32_t aparts(const eff *x, uint32_t n, uint32_t s, fx2_str name, hold *h, uint32_t max){
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t p = 0; x[i].m && p < x[i].fx.nparams; p++){
            uint32_t id = fxm_param(x[i].m, 0, p);
            if (fxm_shared_slot(x[i].m, id) != s + 1 && str_same(fxm_node_get(x[i].m, id)->name, name)){ if (k < max){ h[k].e = i; h[k].id = id; } k++; }
        }
    return k;
}

#define MAXH 512
static void test_pool(const eff *x, uint32_t n, const fxm_pool *pool, int mw){
    uint32_t ns = fxm_pool_slots(pool), live = 0, nhold = 0, napart = 0;
    hold *h = xalloc(MAXH * sizeof *h), *ap = xalloc(MAXH * sizeof *ap);
    uint32_t hist[MAXH] = { 0 };
    for (uint32_t s = 0; s < ns; s++){
        uint32_t nh = holders(x, n, s, h, MAXH);
        if (!nh) continue;
        CHECK(nh <= MAXH, "slot %u: %u holders", s, nh);
        if (nh > MAXH) continue;
        live++; nhold += nh; hist[nh]++;
        const fxm_effect *m0 = x[h[0].e].m;
        const fxm_node *nd = fxm_node_get(m0, h[0].id);
        uint32_t nw = nd->bytes / 4, na = aparts(x, n, s, nd->name, ap, MAXH);
        napart += na;
        uint32_t *exp = xalloc((nw + 4) * 4), *got = xalloc((nw + 4) * 4), *apv = xalloc(((size_t)na * 64 + 1) * 4);
        uint64_t *apver = xalloc(((size_t)na + 1) * 8);
        for (uint32_t a = 0; a < na && a < MAXH; a++){
            const fxm_node *an = fxm_node_get(x[ap[a].e].m, ap[a].id);
            apver[a] = fxm_version(x[ap[a].e].m, ap[a].id);
            if (an->bytes <= 256) fxm_get_value(x[ap[a].e].m, ap[a].id, apv + 64 * a, 256);
        }
        for (uint32_t r = 1; r < nh; r++){
            const fxm_node *rn = fxm_node_get(x[h[r].e].m, h[r].id);
            CHECK(rn->bytes == nd->bytes && rn->t->type == nd->t->type && fxm_version(x[h[r].e].m, h[r].id) == fxm_version(m0, h[0].id),
                  "slot %u: holder %u differs from the first", s, r);
        }
        for (uint32_t w = 0; w < nh; w++){
            fxm_effect *wm = x[h[w].e].m;
            for (int pass = 0; pass < 2; pass++){
                uint64_t before = fxm_version(wm, h[w].id);
                uint32_t rc = FXM_OK, v4[4];
                int typed = 1;
                fxm_get_value(wm, h[w].id, exp, nw * 4);
                if (pass == 1 || nd->cls > FX2_C_OBJECT) typed = 0;
                else if (nd->type == FX2_T_INT && nd->cls <= FX2_C_MATRIX_ROWS && nd->elements){
                    for (uint32_t k = 0; k < nw; k++) exp[k] = 0x5A000000u + 256 * s + w + k;
                    rc = fxm_set_array(wm, h[w].id, FX2_T_INT, exp, nw, NULL);
                } else if (nd->type == FX2_T_INT && nd->cls <= FX2_C_MATRIX_COLUMNS && nw == 1 && !nd->elements){
                    exp[0] = 0x5A000000u + 256 * s + w;
                    rc = fxm_set_int(wm, h[w].id, exp[0], NULL);
                } else if (nd->type == FX2_T_FLOAT && nd->cls <= FX2_C_MATRIX_COLUMNS && nw == 1 && !nd->elements){
                    exp[0] = fb((float)s + (float)w / 64 + 0.125f);
                    rc = fxm_set_float(wm, h[w].id, exp[0], NULL);
                } else if (nd->type == FX2_T_FLOAT && nd->cls == FX2_C_VECTOR && !nd->elements && nw <= 4){
                    for (uint32_t k = 0; k < 4; k++) v4[k] = fb((float)s + (float)w / 64 + 0.125f + (float)k);
                    memcpy(exp, v4, nw * 4);
                    rc = fxm_set_vector(wm, h[w].id, v4, NULL);
                } else if (nd->type == FX2_T_FLOAT && nd->cls <= FX2_C_MATRIX_ROWS){
                    for (uint32_t k = 0; k < nw; k++) exp[k] = fb((float)s + (float)w / 64 + 0.125f + (float)k);
                    rc = fxm_set_array(wm, h[w].id, FX2_T_FLOAT, exp, nw, NULL);
                } else if (nd->cls == FX2_C_OBJECT && nd->type >= FX2_T_TEXTURE && nd->type <= FX2_T_TEXTURECUBE && !nd->elements){
                    exp[0] = 0x13570000u + 16 * s + w;
                    rc = fxm_set_object(wm, h[w].id, exp[0], NULL, NULL);
                } else typed = 0;
                if (!typed){
                    for (uint32_t k = 0; k < nw; k++) exp[k] = 0x6B000000u + 256 * s + w + k + (uint32_t)pass;
                    rc = fxm_set_value(wm, h[w].id, exp, nw * 4, NULL);
                }
                CHECK(rc == FXM_OK, "slot %u writer %u: the setter refused (0x%08x)", s, w, rc);
                uint64_t after = fxm_version(wm, h[w].id);
                CHECK(after > before, "slot %u writer %u: the version did not rise", s, w);
                for (uint32_t r = 0; r < nh; r++){
                    const fxm_effect *rm = x[h[r].e].m;
                    memset(got, 0xEE, (nw + 4) * 4);
                    CHECK(fxm_get_value(rm, h[r].id, got, nw * 4) == FXM_OK && !memcmp(got, exp, nw * 4),
                          "slot %u writer %u (%s #%u): holder %u (%s #%u) reads another value", s, w, x[h[w].e].name, h[w].id, r,
                          x[h[r].e].name, h[r].id);
                    CHECK(fxm_version(rm, h[r].id) == after, "slot %u writer %u: holder %u's version", s, w, r);
                }
            }
        }
        for (uint32_t a = 0; a < na && a < MAXH; a++){
            const fxm_effect *am = x[ap[a].e].m;
            const fxm_node *an = fxm_node_get(am, ap[a].id);
            uint32_t now[64];
            CHECK(fxm_version(am, ap[a].id) == apver[a], "slot %u: apart %s #%u's version moved", s, x[ap[a].e].name, ap[a].id);
            if (an->bytes <= 256)
                CHECK(fxm_get_value(am, ap[a].id, now, 256) == FXM_OK && !memcmp(now, apv + 64 * a, an->bytes),
                      "slot %u: apart %s #%u's value moved", s, x[ap[a].e].name, ap[a].id);
        }
        free(exp); free(got); free(apv); free(apver);
    }
    printf("pool: %u live slots, %u holders, %u apart; every holder wrote, every holder read it back\n", live, nhold, napart);
    if (mw){
        CHECK(live == 20 && nhold == 156 && napart == 9, "§2.7: 20 slots, 156 holders, 9 apart");
        CHECK(hist[27] == 1 && hist[23] == 1 && hist[19] == 1 && hist[7] == 7 && hist[6] == 5 && hist[3] == 1 && hist[2] == 1 && hist[1] == 3,
              "§2.7's holders per slot");
        /* the two GAME §5.5 names: what 0x6D2ABA and 0x6C5ED2 write */
        static const char *const names[2] = { "ColorWriteMode", "BlendState" };
        static const uint32_t nh_want[2] = { 19, 23 }, na_want[2] = { 1, 3 };
        for (int k = 0; k < 2; k++){
            uint32_t s = 0, nh = 0;
            for (; s < ns; s++){
                nh = holders(x, n, s, h, MAXH);
                if (nh && fxm_node_get(x[h[0].e].m, h[0].id)->name.n == strlen(names[k]) &&
                    !memcmp(fxm_node_get(x[h[0].e].m, h[0].id)->name.p, names[k], strlen(names[k]))) break;
            }
            CHECK(s < ns, "no slot named %s", names[k]);
            if (s == ns) continue;
            fx2_str nm = fxm_node_get(x[h[0].e].m, h[0].id)->name;
            CHECK(nh == nh_want[k] && aparts(x, n, s, nm, ap, MAXH) == na_want[k], "%s: %u holders (%u), %u apart (%u)", names[k], nh,
                  nh_want[k], aparts(x, n, s, nm, ap, MAXH), na_want[k]);
            static const uint32_t bs[5] = { 0, 0, 0, 5, 6 };
            uint32_t got[5];
            const hold *wr = &h[nh / 2];
            uint32_t rc = k == 0 ? fxm_set_int(x[wr->e].m, wr->id, 0xF, NULL) : fxm_set_array(x[wr->e].m, wr->id, FX2_T_INT, bs, 5, NULL);
            CHECK(rc == FXM_OK, "%s: the write", names[k]);
            for (uint32_t r = 0; r < nh; r++)
                CHECK(fxm_get_value(x[h[r].e].m, h[r].id, got, k ? 20 : 4) == FXM_OK && (k ? !memcmp(got, bs, 20) : got[0] == 0xF),
                      "%s: %s does not read it back", names[k], x[h[r].e].name);
        }
    }
    free(h); free(ap);
}

/* ---- §6.2: lifetime; drop gets exactly the pointers that die ---- */
static void objects_of(const fxm_effect *m, uint32_t id, uint32_t *out, uint32_t *n, uint32_t max){
    const fxm_node *nd = fxm_node_get(m, id);
    if (nd->elements){ for (uint32_t e = 0; e < nd->elements; e++) objects_of(m, fxm_element(m, id, e), out, n, max); return; }
    if (nd->cls == FX2_C_STRUCT){ for (uint32_t k = 0; k < nd->members; k++) objects_of(m, fxm_param(m, id, k), out, n, max); return; }
    if (nd->cls == FX2_C_OBJECT && nd->bytes && fxm_data(m, id)[0]){       /* a sampler, INT, VOID ... has no storage */
        if (*n < max) out[*n] = fxm_data(m, id)[0];
        (*n)++;
    }
}
/* the pointers that die when the effects with gone[i] are freed */
static uint32_t dying(const eff *x, uint32_t n, const uint8_t *gone, const fxm_pool *pool, uint32_t *out, uint32_t max){
    uint32_t k = 0, ns = fxm_pool_slots(pool);
    hold *h = xalloc(MAXH * sizeof *h);
    for (uint32_t s = 0; s < ns; s++){
        uint32_t nh = holders(x, n, s, h, MAXH), all = nh > 0;
        for (uint32_t r = 0; r < nh && r < MAXH; r++) all &= gone[h[r].e];
        if (all) objects_of(x[h[0].e].m, h[0].id, out, &k, max);
    }
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t p = 0; gone[i] && x[i].m && p < x[i].fx.nparams; p++){
            uint32_t id = fxm_param(x[i].m, 0, p);
            if (!fxm_shared_slot(x[i].m, id)) objects_of(x[i].m, id, out, &k, max);
        }
    free(h);
    return k;
}
static int cmp32(const void *a, const void *b){ uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int same_set(uint32_t *a, uint32_t na, uint32_t *b, uint32_t nb){
    if (na != nb || na > 64) return 0;
    qsort(a, na, 4, cmp32); qsort(b, nb, 4, cmp32);
    return !na || !memcmp(a, b, na * 4);
}
static void free_set(eff *x, uint32_t n, const uint8_t *gone, fxm_pool *pool, const char *what, droplog *all){
    uint32_t want[64];
    uint32_t nw = dying(x, n, gone, pool, want, 64);
    droplog dl = { 0 };
    for (uint32_t i = 0; i < n; i++) if (gone[i] && x[i].m){ fxm_effect_free(x[i].m, drop_rec, &dl); x[i].m = NULL; }
    for (uint32_t k = 0; k < dl.n && k < 64; k++) drop_rec(all, dl.p[k]);
    CHECK(same_set(dl.p, dl.n, want, nw), "%s: %u pointers dropped, %u expected", what, dl.n, nw);
}

static void test_lifetime(eff *x, uint32_t n, fxm_pool *pool, int mw){
    uint32_t ns = fxm_pool_slots(pool), L = ns, best = 0, T;
    hold h[MAXH];
    droplog all = { 0 };
    for (uint32_t s = 0; s < ns; s++){
        uint32_t nh = holders(x, n, s, h, MAXH);
        const fxm_node *nd = nh ? fxm_node_get(x[h[0].e].m, h[0].id) : NULL;
        if (nh > best && (!mw || (nd->name.n == 10 && !memcmp(nd->name.p, "BlendState", 10)))){ best = nh; L = s; }
    }
    if (best < 2){ printf("lifetime: no slot with two holders, skipped\n"); return; }
    uint32_t nh = holders(x, n, L, h, MAXH), e0 = h[0].e, id0 = h[0].id;
    const fxm_node *nd = fxm_node_get(x[e0].m, id0);
    uint32_t nw = nd->bytes / 4, pat[64], got[64], def[64];
    if (nw > 64) return;
    for (uint32_t k = 0; k < nw; k++) pat[k] = 0x7C000000u + k;
    CHECK(fxm_set_value(x[h[1].e].m, h[1].id, pat, nw * 4, NULL) == FXM_OK, "lifetime: write through holder 1");
    uint8_t *gone = xalloc(n);
    gone[e0] = 1;
    free_set(x, n, gone, pool, "the first holder goes", &all);
    for (uint32_t r = 1; r < nh; r++)
        CHECK(fxm_get_value(x[h[r].e].m, h[r].id, got, nw * 4) == FXM_OK && !memcmp(got, pat, nw * 4), "lifetime: holder %u lost the value", r);
    char err[256];
    uint64_t lv = fxm_version(x[h[1].e].m, h[1].id);
    CHECK(!fxm_effect_new(&x[e0].m, &x[e0].fx, pool, err, sizeof err), "%s", err);
    CHECK(x[e0].m && fxm_shared_slot(x[e0].m, id0) == L + 1 && fxm_get_value(x[e0].m, id0, got, nw * 4) == FXM_OK && !memcmp(got, pat, nw * 4),
          "lifetime: made again, it reads the slot's value, not its default");
    CHECK(x[e0].m && fxm_version(x[e0].m, id0) > lv && fxm_version(x[h[1].e].m, h[1].id) == fxm_version(x[e0].m, id0),
          "lifetime: made again, it raised the slot's version (W: set_dirty on joining)");

    /* two pointers that die later: one in a texture slot, one in a texture of its own */
    for (T = 0; T < ns; T++){
        uint32_t th = holders(x, n, T, h, MAXH);
        const fxm_node *tn = th ? fxm_node_get(x[h[0].e].m, h[0].id) : NULL;
        if (tn && tn->cls == FX2_C_OBJECT && tn->type >= FX2_T_TEXTURE && tn->type <= FX2_T_TEXTURECUBE && !tn->elements) break;
    }
    if (T < ns) CHECK(fxm_set_object(x[h[0].e].m, h[0].id, 0x24680000u, NULL, NULL) == FXM_OK, "lifetime: SetTexture on slot %u", T);
    int own = 0;
    for (uint32_t i = 0; i < n && !own; i++)
        for (uint32_t p = 0; x[i].m && p < x[i].fx.nparams && !own; p++){
            uint32_t id = fxm_param(x[i].m, 0, p);
            const fxm_node *q = fxm_node_get(x[i].m, id);
            if (!fxm_shared_slot(x[i].m, id) && q->cls == FX2_C_OBJECT && q->type == FX2_T_TEXTURE && !q->elements)
                own = fxm_set_object(x[i].m, id, 0x13579000u, NULL, NULL) == FXM_OK;
        }
    if (mw) CHECK(T < ns && own, "lifetime: MW has a texture slot and a texture of its own");

    /* every holder of L goes: a new effect with that parameter starts from its default */
    nh = holders(x, n, L, h, MAXH);
    memset(gone, 0, n);
    for (uint32_t r = 0; r < nh; r++) gone[h[r].e] = 1;
    free_set(x, n, gone, pool, "every holder goes", &all);
    CHECK(holders(x, n, L, h, MAXH) == 0, "lifetime: the slot is free");
    CHECK(!fxm_effect_new(&x[e0].m, &x[e0].fx, pool, err, sizeof err), "%s", err);
    const fx2_value *v = &x[e0].fx.params[fxm_node_get(x[e0].m, id0)->index].v;
    for (uint32_t k = 0; k < nw; k++) def[k] = fx2_rd32(v->data + 4 * k);
    CHECK(fxm_get_value(x[e0].m, id0, got, nw * 4) == FXM_OK && !memcmp(got, def, nw * 4), "lifetime: a new effect starts from its default");

    memset(gone, 1, n);
    free_set(x, n, gone, pool, "everything goes", &all);
    int has1 = 0, has2 = 0;
    for (uint32_t k = 0; k < all.n && k < 64; k++){ has1 += all.p[k] == 0x24680000u; has2 += all.p[k] == 0x13579000u; }
    CHECK((T == ns || has1 == 1) && (!own || has2 == 1), "lifetime: each of the two pointers was dropped once (%d, %d)", has1, has2);
    free(gone);
    printf("lifetime: slot %u outlived its first holder, came back with its value, started again from its default; %u drops, each exact\n",
           L, all.n);
}

static void usage(void){
    fprintf(stderr, "usage: fxmodel_test [--mw] [--states] [--dll <d3dx9_26.dll>] [--conf <game.conf>] [<NAME>.fxo...]\n");
    exit(2);
}

int main(int argc, char **argv){
    int mw = 0, states = 0;
    const char *dll = getenv("NFSMW_D3DX"), *conf = "games/mw/game.conf";
    int nf = 0;
    char **files = xalloc((size_t)argc * sizeof *files);
    for (int i = 1; i < argc; i++){
        if (!strcmp(argv[i], "--mw")) mw = 1;
        else if (!strcmp(argv[i], "--states")) states = 1;
        else if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--conf") && i + 1 < argc) conf = argv[++i];
        else if (argv[i][0] == '-') usage();
        else files[nf++] = argv[i];
    }
    if (states){ print_states(); return 0; }
    test_state_table();
    if (dll && *dll) test_dll(dll, conf);
    else printf("dll: skipped (NFSMW_D3DX is not set and no --dll)\n");
    test_conv();
    test_pool_match();
    test_unnamed();
    test_sampler_ref();
    test_refused();
    if (nf){
        eff *x = xalloc((size_t)nf * sizeof *x);
        uint32_t n = 0;
        for (int i = 0; i < nf; i++){
            uint32_t size;
            uint8_t *b = slurp(files[i], &size);
            CHECK(b != NULL, "%s: cannot read", files[i]);
            if (!b) continue;
            const char *base = strrchr(files[i], '/');
            char name[256];
            snprintf(name, sizeof name, "%s", base ? base + 1 : files[i]);
            size_t nl = strlen(name);
            if (nl > 4 && !strcmp(name + nl - 4, ".fxo")) name[nl - 4] = 0;
            if (eff_parse(&x[n], b, size, name)) free(b); else n++;
        }
        qsort(x, n, sizeof *x, eff_by_name);
        if (mw) CHECK(n == 31, "%u effects, --mw wants the 31", n);
        fxm_pool *pool = fxm_pool_new();
        int ok = pool != NULL;
        for (uint32_t i = 0; i < n && ok; i++) ok = !eff_model(&x[i], pool);
        if (ok){
            test_rows(x, n, mw);
            test_lookups(x, n, mw);
            test_ctab(x, n, mw);
            test_pool(x, n, pool, mw);
            test_lifetime(x, n, pool, mw);
        }
        for (uint32_t i = 0; i < n; i++){ fxm_effect_free(x[i].m, NULL, NULL); fx2_free(&x[i].fx); free(x[i].buf); }
        fxm_pool_free(pool);
        free(x);
    }
    free(files);
    printf("fxmodel_test: %lu checks, %lu failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
