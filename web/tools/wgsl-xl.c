/* Offline WGSL translation of D3D9 shader binaries with the game's own
 * translator (runtime/vsemu.c built natively with -DVSEMU_WGSL), for checking
 * the emitter without running the game:
 *
 *   cc -O1 -DVSEMU_WGSL -DNFS_ENHANCED=0 -Iruntime -Iruntime/web \
 *      runtime/vsemu.c web/tools/wgsl-xl.c -lm -o out/wgsl-xl
 *   out/wgsl-xl <outdir> shader.bin...
 *   out/wgsl-xl --pairs <dumpdir> <outdir>
 *   node web/tools/shader-check.mjs --from <outdir>
 *
 * --pairs takes an in-game dump (NFSU2_SHADER_DUMP) and translates every
 * distinct vertex shader object against every distinct vertex declaration the
 * game created that supplies all of the shader's inputs, mapped exactly as
 * com.c draw_up/gpuvs_id do (stream 0, vs_input_reg).  That covers the pairs a
 * single test drive never draws; the modules get ids from 1001 (index.txt
 * says which object and declaration each one is).  It also writes
 * differential test vectors (vs_<id>.vec, see write_vectors) for these
 * modules and for the game's own pairs in the dump, and pixel shader vectors
 * (ps_<id>.pvec, see write_ps_vectors) for every dumped pixel shader.
 *
 * The output uses the in-game dump's file names (vs_<n>.wgsl, ps_<n>.wgsl,
 * ps_<n+256>.wgsl for the cube twin, vs_0/ps_0/ps_256 built-ins,
 * pulltest.wgsl, index.txt).  A vertex shader gets a synthetic declaration
 * (one element per dcl'd input with a typical NFSU2 type), so this checks the
 * shader bodies; the real (shader, declaration) pairs come from the in-game
 * dump (NFSU2_SHADER_DUMP, docs/web/gpu-abi.md section 9). */
#include "vsemu.h"
#include "wgsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <math.h>

static char g_buf[256 * 1024];

static void put(const char *dir, const char *name, const char *text, size_t n){
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    fwrite(text, 1, n, f); fclose(f);
}

static unsigned char *slurp(const char *path, size_t *n){
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)L + 4);
    if (b && fread(b, 1, (size_t)L, f) != (size_t)L){ free(b); b = 0; }
    fclose(f); *n = (size_t)L;
    return b;
}


/* ---- differential test vectors: the CPU interpreter is the reference ----
 * vs_<id>.vec (little-endian): u32 magic 'VSV1', nverts, stride, writes_fog,
 * then nverts*stride vertex bytes, 256*4 f32 constants (K.c), and per vertex
 * 29 f32 expected outputs: pos, col0, col1, uv0..uv3 (TEXCOORDINDEX = identity),
 * fog.  The expected values come from vs_run on inputs decoded by a copy of
 * com.c decl_fetch, i.e. the Mac's CPU vertex path.  The shader check runs the
 * WGSL vertex function as a compute shader on the same bytes and compares. */
static uint32_t g_rng;
static uint32_t rnd(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float rndf(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() >> 8) / 16777216.f; }

/* copy of com.c decl_fetch (static there), reading from a byte buffer */
static float rf(const unsigned char *b){ float f; memcpy(&f, b, 4); return f; }
static int16_t rs(const unsigned char *b){ return (int16_t)(b[0] | b[1] << 8); }
static uint16_t ru(const unsigned char *b){ return (uint16_t)(b[0] | b[1] << 8); }
static void decl_fetch(const unsigned char *a, unsigned type, float out[4]){
    out[0]=out[1]=out[2]=0.f; out[3]=1.f;
    switch (type){
    case 0: out[0]=rf(a); break;
    case 1: out[0]=rf(a); out[1]=rf(a+4); break;
    case 2: out[0]=rf(a); out[1]=rf(a+4); out[2]=rf(a+8); break;
    case 3: out[0]=rf(a); out[1]=rf(a+4); out[2]=rf(a+8); out[3]=rf(a+12); break;
    case 4: out[0]=a[2]/255.f; out[1]=a[1]/255.f; out[2]=a[0]/255.f; out[3]=a[3]/255.f; break;
    case 5: out[0]=a[0]; out[1]=a[1]; out[2]=a[2]; out[3]=a[3]; break;
    case 8: out[0]=a[0]/255.f; out[1]=a[1]/255.f; out[2]=a[2]/255.f; out[3]=a[3]/255.f; break;
    case 6: out[0]=rs(a); out[1]=rs(a+2); break;
    case 7: out[0]=rs(a); out[1]=rs(a+2); out[2]=rs(a+4); out[3]=rs(a+6); break;
    case 9: out[0]=rs(a)/32767.f; out[1]=rs(a+2)/32767.f; break;
    case 10:out[0]=rs(a)/32767.f; out[1]=rs(a+2)/32767.f; out[2]=rs(a+4)/32767.f; out[3]=rs(a+6)/32767.f; break;
    case 11:out[0]=ru(a)/65535.f; out[1]=ru(a+2)/65535.f; break;
    case 12:out[0]=ru(a)/65535.f; out[1]=ru(a+2)/65535.f; out[2]=ru(a+4)/65535.f; out[3]=ru(a+6)/65535.f; break;
    default: break;
    }
}
static const int DSIZE[13] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8 };

static void write_vectors(const char *dir, int id, const VSProg *p, const int dcl[16][2]){
    enum { NV = 64 };
    int stride = 4;
    for (int i = 0; i < 16; i++)
        if (dcl[i][0] >= 0 && dcl[i][1] >= 0 && dcl[i][1] < 13 && dcl[i][0] + DSIZE[dcl[i][1]] > stride)
            stride = dcl[i][0] + DSIZE[dcl[i][1]];
    stride = (stride + 3) & ~3;
    g_rng = 0x9E3779B9u ^ (uint32_t)id * 2654435761u;
    unsigned char *vb = calloc(NV, (size_t)stride);
    /* plausible values per element type: floats in [0, 4) keep relative
     * constant indices (a0 = floor(v) * 4 + n in WORLDBONE) inside c0..c255,
     * where the interpreter (out of range -> 0) and the GPU (clamp) agree */
    for (int v = 0; v < NV; v++)
        for (int i = 0; i < 16; i++){
            if (dcl[i][0] < 0) continue;
            unsigned char *a = vb + (size_t)v * stride + dcl[i][0];
            int t = dcl[i][1];
            if (t <= 3) for (int c = 0; c <= t; c++){ float f = rndf(0.f, 4.f); memcpy(a + 4*c, &f, 4); }
            else for (int c = 0; c < DSIZE[t]; c++) a[c] = (unsigned char)rnd();
        }
    static VSConst k; memset(&k, 0, sizeof k);
    for (int r = 0; r < 256; r++) for (int c = 0; c < 4; c++) k.f[r][c] = rndf(-1.f, 1.f);
    int wf = vs_writes_fog(p);
    char path[1024]; snprintf(path, sizeof path, "%s/vs_%d.vec", dir, id);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    uint32_t hdr[4] = { 0x31565356u, NV, (uint32_t)stride, (uint32_t)wf };
    fwrite(hdr, 4, 4, f);
    fwrite(vb, (size_t)stride, NV, f);
    fwrite(k.f, 16, 256, f);
    for (int v = 0; v < NV; v++){
        float in[16][4];
        for (int r = 0; r < 16; r++){ in[r][0] = in[r][1] = in[r][2] = 0.f; in[r][3] = 1.f; }
        for (int i = 0; i < 16; i++)
            if (dcl[i][0] >= 0) decl_fetch(vb + (size_t)v * stride + dcl[i][0], (unsigned)dcl[i][1], in[i]);
        VSOut o; vs_run(p, &k, (const float (*)[4])in, &o);
        float e[29];
        memcpy(e, o.pos, 16); memcpy(e + 4, o.col[0], 16); memcpy(e + 8, o.col[1], 16);
        memcpy(e + 12, o.tex[0], 64);
        e[28] = wf ? o.fog : 1.f;          /* GPU semantics: unwritten oFog = 1 (no fog) */
        fwrite(e, 4, 29, f);
    }
    fclose(f); free(vb);
}


/* ---- pixel shader vectors: ps_<id>.pvec, oracle = ps_ref_run ----
 * u32 magic 'PSV1', npix, cubemask, 0; the 560-byte PSPar block; per pixel 25
 * f32 inputs (col0, col1, uv0..uv3, fog); per pixel 5 f32 expected outputs
 * (rgba after alpha test and fog, killed 0/1).  Textures are an analytic
 * function of the coordinate (ftex below; the shader check substitutes the
 * same function for textureSample). */
static void ftex(int k, const float *c, int n, float o[4]){
    float x = c[0], y = c[1], f = (float)k;
    if (n == 3){ x = c[0] + c[2] * 0.3f; y = c[1] - c[2] * 0.2f; }
    o[0] = 0.5f + 0.4f * sinf(x * 3.1f + f * 0.37f);
    o[1] = 0.5f + 0.4f * cosf(y * 2.3f + f * 0.5f);
    o[2] = 0.5f + 0.4f * sinf((x + y) * 1.7f + f);
    o[3] = 0.5f + 0.4f * cosf(x * y * 5.3f + 0.2f * f);
}
static int write_ps_vectors(const char *dir, int id, const VSProg *p, unsigned cubemask){
    enum { NP = 256 };
    g_rng = 0x85EBCA6Bu ^ (uint32_t)id * 2246822519u;
    static PSRefPar P; memset(&P, 0, sizeof P);
    P.aref = rndf(0.f, 0.5f);
    for (int k = 0; k < 4; k++) P.proj[k] = (float)(k + 16 * (int)(rnd() % 3u));
    for (int r = 0; r < 32; r++) for (int c = 0; c < 4; c++) P.c[r][c] = rndf(-1.f, 1.f);
    for (int c = 0; c < 3; c++) P.fog[c] = rndf(0.f, 1.f);
    P.fog[3] = (float)(id & 1);
    char path[1024]; snprintf(path, sizeof path, "%s/ps_%d.pvec", dir, id);
    FILE *f = fopen(path, "wb");
    if (!f){ perror(path); exit(1); }
    uint32_t hdr[4] = { 0x31565350u, NP, cubemask, 0 };
    fwrite(hdr, 4, 4, f);
    float blk[140] = { 0 };                    /* PSPar: aref usec pad pad, proj, c[32], fog */
    blk[0] = P.aref; memcpy(blk + 4, P.proj, 16); memcpy(blk + 8, P.c, 512); memcpy(blk + 136, P.fog, 16);
    fwrite(blk, 4, 140, f);
    static PSRefIn in[NP];
    for (int i = 0; i < NP; i++){
        /* colours a little outside [0, 1] (the Mac does not clamp oD0/oD1)
         * so _sat, cmp and cnd see both sides of their thresholds */
        for (int c = 0; c < 4; c++){ in[i].col[0][c] = rndf(-0.25f, 1.25f); in[i].col[1][c] = rndf(-0.25f, 1.25f); }
        for (int k = 0; k < 4; k++){
            in[i].uv[k][0] = rndf(0.f, 1.f); in[i].uv[k][1] = rndf(0.f, 1.f);
            in[i].uv[k][2] = rndf(0.5f, 2.f); in[i].uv[k][3] = rndf(0.5f, 2.f);
        }
        in[i].fog = rndf(-0.2f, 1.2f);
        float row[25];
        memcpy(row, in[i].col, 32); memcpy(row + 8, in[i].uv, 64); row[24] = in[i].fog;
        fwrite(row, 4, 25, f);
    }
    int ok = 1;
    for (int i = 0; i < NP; i++){
        float o[5] = { 0, 0, 0, 0, 0 }; int killed = 0;
        if (!ps_ref_run(p, &P, &in[i], ftex, cubemask, o, &killed)) ok = 0;   /* refused (ps_2_x, ps_3_0): zeros */
        o[4] = (float)killed;
        fwrite(o, 4, 5, f);
    }
    fclose(f);
    return ok;
}

/* every pixel shader object of the dump, and its stage-0 cube twin */
static int game_ps_vectors(const char *dump, const char *dir){
    int n = 0;
    for (int id = 1; id < 256; id++){
        char bp[1024]; snprintf(bp, sizeof bp, "%s/psobj_%d.bin", dump, id);
        size_t bn; unsigned char *b = slurp(bp, &bn);
        if (!b) continue;
        VSProg *p = vs_compile((const uint32_t *)b, bn / 4);
        if (p){ n += write_ps_vectors(dir, id, p, 0u); n += write_ps_vectors(dir, id + 256, p, 1u); vs_free(p); }
        free(b);
    }
    return n;
}

/* The game's own pairs: index.txt "vs <gid> obj=<n> decl=0x.. in=v0@0:FLOAT3,..." */
static int game_vectors(const char *dump, const char *dir){
    static const char *const TN[] = { "FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4",
        "SHORT2", "SHORT4", "UBYTE4N", "SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N" };
    char path[1024]; snprintf(path, sizeof path, "%s/index.txt", dump);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048]; int n = 0;
    while (fgets(line, sizeof line, f)){
        int gid, obj; char in[1500];
        if (sscanf(line, "vs %d obj=%d decl=%*s in=%1499s", &gid, &obj, in) != 3) continue;
        int dcl[16][2];
        for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
        for (char *t = strtok(in, ","); t; t = strtok(0, ",")){
            int r, off; char ty[32];
            if (sscanf(t, "v%d@%d:%31s", &r, &off, ty) != 3 || r < 0 || r > 15) continue;
            for (int q = 0; q < 13; q++) if (!strcmp(ty, TN[q])){ dcl[r][0] = off; dcl[r][1] = q; }
        }
        char bp[1024]; snprintf(bp, sizeof bp, "%s/vsobj_%d.bin", dump, obj);
        size_t bn; unsigned char *b = slurp(bp, &bn);
        if (!b) continue;
        VSProg *p = vs_compile((const uint32_t *)b, bn / 4);
        if (p){ write_vectors(dir, gid, p, (const int (*)[2])dcl); n++; vs_free(p); }
        free(b);
    }
    fclose(f);
    return n;
}

static int pairs(const char *dump, const char *dir){
    DIR *d = opendir(dump);
    if (!d){ perror(dump); return 1; }
    enum { MAXO = 512 };
    static struct { char name[64]; unsigned char *b; size_t n; } vs[MAXO], dc[MAXO];
    int nvs = 0, ndc = 0;
    struct dirent *e;
    while ((e = readdir(d))){
        int isvs = !strncmp(e->d_name, "vsobj_", 6) && strstr(e->d_name, ".bin");
        int isdc = !strncmp(e->d_name, "decl_", 5) && strstr(e->d_name, ".bin");
        if (!isvs && !isdc) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dump, e->d_name);
        size_t n; unsigned char *b = slurp(p, &n);
        if (!b) continue;
        /* distinct contents only */
        int dup = 0, cnt = isvs ? nvs : ndc;
        for (int i = 0; i < cnt && !dup; i++){
            if (isvs ? (vs[i].n == n && !memcmp(vs[i].b, b, n)) : (dc[i].n == n && !memcmp(dc[i].b, b, n))) dup = 1;
        }
        if (dup || cnt >= MAXO){ free(b); continue; }
        if (isvs){ snprintf(vs[nvs].name, 64, "%s", e->d_name); vs[nvs].b = b; vs[nvs].n = n; nvs++; }
        else     { snprintf(dc[ndc].name, 64, "%s", e->d_name); dc[ndc].b = b; dc[ndc].n = n; ndc++; }
    }
    closedir(d);
    FILE *ix; { char p[1024]; snprintf(p, sizeof p, "%s/pairs.txt", dir); ix = fopen(p, "w"); }
    if (!ix){ perror("pairs.txt"); return 1; }
    int id = 1000, ok = 0, fail = 0, nopair = 0;
    for (int v = 0; v < nvs; v++){
        VSProg *p = vs_compile((const uint32_t *)vs[v].b, vs[v].n / 4);
        if (!p){ fprintf(ix, "%s: parse failed\n", vs[v].name); fail++; continue; }
        int paired = 0;
        for (int k = 0; k < ndc; k++){
            const unsigned char *el = dc[k].b;
            int dcl[16][2], have[14][16];
            memset(have, 0, sizeof have);
            for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
            int nel = 0;
            for (size_t q = 0; q + 8 <= dc[k].n; q += 8){
                unsigned st = el[q] | el[q+1] << 8, of = el[q+2] | el[q+3] << 8;
                unsigned ty = el[q+4], us = el[q+6], ui = el[q+7];
                if (st == 0xFFu && ty == 17u) break;
                if (st != 0) continue;
                if (us < 14 && ui < 16) have[us][ui] = 1;
                int rg = vs_input_reg(p, (int)us, (int)ui);
                if (rg >= 0 && nel < 16){ dcl[rg][0] = (int)of; dcl[rg][1] = (int)ty; nel++; }
            }
            int covered = nel > 0;
            for (int us = 0; us < 14 && covered; us++)
                for (int ui = 0; ui < 16; ui++)
                    if (vs_input_reg(p, us, ui) >= 0 && !have[us][ui]){ covered = 0; break; }
            if (!covered) continue;
            paired = 1;
            int n = vs_to_wgsl(p, ++id, (const int (*)[2])dcl, g_buf, sizeof g_buf);
            char name[64]; snprintf(name, sizeof name, "vs_%d.wgsl", id);
            if (n > 0){ put(dir, name, g_buf, (size_t)n); write_vectors(dir, id, p, (const int (*)[2])dcl); ok++; }
            else fail++;
            fprintf(ix, "vs %d = %s x %s %s\n", id, vs[v].name, dc[k].name, n > 0 ? "ok" : "REJECTED");
        }
        if (!paired){ nopair++; fprintf(ix, "%s: no declaration supplies all inputs\n", vs[v].name); }
        vs_free(p);
    }
    fclose(ix);
    int gv = game_vectors(dump, dir), pv = game_ps_vectors(dump, dir);
    printf("%d distinct vertex shaders x %d distinct declarations: %d modules, %d rejected, %d shaders unpaired; "
           "test vectors for %d game pairs and %d pixel shader modules\n", nvs, ndc, ok, fail, nopair, gv, pv);
    return fail ? 1 : 0;
}

int main(int argc, char **argv){
    if (argc == 4 && !strcmp(argv[1], "--pairs")) return pairs(argv[2], argv[3]);
    if (argc < 3){ fprintf(stderr, "usage: %s <outdir> shader.bin... | --pairs <dumpdir> <outdir>\n", argv[0]); return 2; }
    const char *dir = argv[1];
    FILE *ix; { char p[1024]; snprintf(p, sizeof p, "%s/index.txt", dir); ix = fopen(p, "w"); }
    if (!ix){ perror("index.txt"); return 1; }
    int n;
    if ((n = wgsl_tvert_module(g_buf, sizeof g_buf)) > 0) put(dir, "vs_0.wgsl", g_buf, (size_t)n);
    if ((n = wgsl_ff_module(0, g_buf, sizeof g_buf, 0)) > 0) put(dir, "ps_0.wgsl", g_buf, (size_t)n);
    if ((n = wgsl_ff_module(256, g_buf, sizeof g_buf, 1)) > 0) put(dir, "ps_256.wgsl", g_buf, (size_t)n);
    if ((n = wgsl_pull_test_module(g_buf, sizeof g_buf)) > 0) put(dir, "pulltest.wgsl", g_buf, (size_t)n);
    int nvs = 0, nps = 0, fails = 0;
    for (int a = 2; a < argc; a++){
        FILE *f = fopen(argv[a], "rb");
        if (!f){ perror(argv[a]); return 1; }
        static uint32_t tok[16384];
        size_t dw = fread(tok, 4, 16384, f); fclose(f);
        VSProg *p = vs_compile(tok, dw);
        const char *base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
        if (!p){ fprintf(stderr, "%s: parse failed\n", base); fails++; continue; }
        char name[64];
        if ((tok[0] >> 16) == 0xFFFFu){
            int id = ++nps;
            n = ps_to_wgsl(p, id, g_buf, sizeof g_buf);
            if (n > 0){ snprintf(name, sizeof name, "ps_%d.wgsl", id); put(dir, name, g_buf, (size_t)n); }
            else fails++;
            int nc = ps_to_wgsl_ex(p, id + 256, g_buf, sizeof g_buf, 1u);
            if (nc > 0){ snprintf(name, sizeof name, "ps_%d.wgsl", id + 256); put(dir, name, g_buf, (size_t)nc); }
            else fails++;
            fprintf(ix, "ps %d src=%s instr=%u unknown=%u len=%d cube=%d\n", id, base, vs_ninstr(p),
                    vs_unknown(p), n, nc);
        } else {
            /* synthetic declaration: usage -> type, packed in register order */
            static const int ty_of_usage[14] = { 2, 3, 4, 2, 0, 1, 2, 2, 0, 3, 4, 0, 0, 0 };
            int dcl[16][2], off = 0;
            for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
            for (int us = 0; us < 14; us++)
                for (int ui = 0; ui < 8; ui++){
                    int r = vs_input_reg(p, us, ui);
                    if (r < 0) continue;
                    int t = ty_of_usage[us];
                    static const int sz[13] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8 };
                    dcl[r][0] = off; dcl[r][1] = t; off += sz[t];
                }
            int id = ++nvs;
            n = vs_to_wgsl(p, id, (const int (*)[2])dcl, g_buf, sizeof g_buf);
            if (n > 0){ snprintf(name, sizeof name, "vs_%d.wgsl", id); put(dir, name, g_buf, (size_t)n); }
            else fails++;
            fprintf(ix, "vs %d src=%s instr=%u unknown=%u kmax=%d stride=%d len=%d\n", id, base,
                    vs_ninstr(p), vs_unknown(p), vs_kmax(p), off, n);
        }
        vs_free(p);
    }
    fclose(ix);
    printf("%d vertex shaders, %d pixel shaders, %d translation failures\n", nvs, nps, fails);
    return fails ? 1 : 0;
}
