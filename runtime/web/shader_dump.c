/* NFSU2_SHADER_DUMP=<dir> (e.g. /save/shaders): write every shader the game
 * creates and every WGSL module the translator hands the renderer, so the
 * whole set can be validated in Chrome (web/tools/shader-check.mjs).
 *
 *   vsobj_<n>.bin/.asm   CreateVertexShader bytecode + disassembly (n = com.c index)
 *   psobj_<id>.bin/.asm  CreatePixelShader bytecode + disassembly
 *   vs_<gid>.wgsl        one module per (vertex shader, declaration) pair
 *   ps_<id>.wgsl         pixel shader module; ps_<id+256>.wgsl = stage-0 cube twin
 *   decl_<obj>.bin      CreateVertexDeclaration elements (obj = COM address, hex)
 *   vs_0, ps_0, ps_256   built-in TVert / fixed-function modules
 *   pulltest.wgsl        vertex-pulling compute test
 *   index.txt            one line per object / module, with the declarations
 *
 * Game-derived data: the dump goes to OPFS or out/, never into the repo. */
#include "wgsl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static int g_on = -1;
static char g_dir[512];
static FILE *g_ix;

static void put(const char *name, const void *data, size_t n){
    char path[640]; snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f){ fprintf(stderr, "[shaderdump] %s: %s\n", path, strerror(errno)); return; }
    fwrite(data, 1, n, f); fclose(f);
}

int wgsl_dump_on(void){
    if (g_on >= 0) return g_on;
    const char *d = getenv("NFSU2_SHADER_DUMP");
    g_on = (d && *d) ? 1 : 0;
    if (!g_on) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", d);
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST){
        fprintf(stderr, "[shaderdump] cannot create %s: %s\n", g_dir, strerror(errno));
        g_on = 0; return 0;
    }
    char path[640]; snprintf(path, sizeof path, "%s/index.txt", g_dir);
    g_ix = fopen(path, "w");
    static char buf[64 * 1024];
    int n;
    if ((n = wgsl_tvert_module(buf, sizeof buf)) > 0) put("vs_0.wgsl", buf, (size_t)n);
    if ((n = wgsl_ff_module(0, buf, sizeof buf, 0u)) > 0) put("ps_0.wgsl", buf, (size_t)n);
    if ((n = wgsl_ff_module(256, buf, sizeof buf, 1u)) > 0) put("ps_256.wgsl", buf, (size_t)n);
    if ((n = wgsl_pull_test_module(buf, sizeof buf)) > 0) put("pulltest.wgsl", buf, (size_t)n);
    if (g_ix){ fprintf(g_ix, "builtin vs_0 ps_0 ps_256 pulltest\n"); fflush(g_ix); }
    fprintf(stderr, "[shaderdump] writing shaders to %s\n", g_dir);
    return 1;
}

void wgsl_dump_obj(const char *kind, int idx, const void *tok, size_t bytes, const VSProg *p){
    if (!wgsl_dump_on()) return;
    char name[64];
    snprintf(name, sizeof name, "%sobj_%d.bin", kind, idx);
    put(name, tok, bytes);
    snprintf(name, sizeof name, "%sobj_%d.asm", kind, idx);
    char path[640]; snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "w");
    if (f){ vs_disasm(p, f); fclose(f); }
    if (g_ix){
        unsigned v = vs_version(p);
        fprintf(g_ix, "obj %s %d %s_%u_%u bytes=%zu instr=%u unknown=%u\n", kind, idx, kind,
                v >> 8, v & 0xFF, bytes, vs_ninstr(p), vs_unknown(p));
        fflush(g_ix);
    }
}

static const char *const DECLTYPE[] = { "FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4",
    "SHORT2", "SHORT4", "UBYTE4N", "SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N", "UDEC3", "DEC3N",
    "FLOAT16_2", "FLOAT16_4", "UNUSED" };

void wgsl_dump_vs(int gid, int vsobj, uint32_t decl, const int dcl[16][2], const char *wgsl, int len){
    if (!wgsl_dump_on()) return;
    char name[64];
    if (len > 0){ snprintf(name, sizeof name, "vs_%d.wgsl", gid); put(name, wgsl, (size_t)len); }
    if (g_ix){
        fprintf(g_ix, "vs %d obj=%d decl=0x%08x in=", gid, vsobj, decl);
        int first = 1;
        for (int i = 0; i < 16; i++){
            if (dcl[i][0] < 0) continue;
            unsigned t = (unsigned)dcl[i][1];
            fprintf(g_ix, "%sv%d@%d:%s", first ? "" : ",", i, dcl[i][0], t < 18 ? DECLTYPE[t] : "?");
            first = 0;
        }
        fprintf(g_ix, " %s\n", len > 0 ? "ok" : "REJECTED");
        fflush(g_ix);
    }
}

void wgsl_dump_ps(int id, int psobj, const char *wgsl, int len){ wgsl_dump_ps_why(id, psobj, wgsl, len, NULL); }
void wgsl_dump_ps_why(int id, int psobj, const char *wgsl, int len, const char *why){
    if (!wgsl_dump_on()) return;
    char name[64];
    if (len > 0){ snprintf(name, sizeof name, "ps_%d.wgsl", id); put(name, wgsl, (size_t)len); }
    if (g_ix){
        int r = len <= 0 && why && *why;
        fprintf(g_ix, "ps %d obj=%d %s%s%s\n", id, psobj, len > 0 ? "ok" : "REJECTED", r ? ": " : "", r ? why : "");
        fflush(g_ix);
    }
}

void wgsl_dump_decl(uint32_t obj, const void *elems, size_t bytes){
    if (!wgsl_dump_on()) return;
    char name[64]; snprintf(name, sizeof name, "decl_%08x.bin", obj);
    put(name, elems, bytes);
    if (g_ix){ fprintf(g_ix, "decl 0x%08x elements=%zu\n", obj, bytes / 8); fflush(g_ix); }
}
