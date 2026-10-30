/* comrec.c -- the device-call recorder: NFSU2_DXREC=<file>[,<from>[,<to>]] writes every
 * IDirect3DDevice9 call the guest makes in Presents <from>..<to> (all by default), the state blocks'
 * Capture, Apply and Release, and the game's context lines (comrec_marker) to <file>, one line each
 * (docs/web/dxrec.md has the format).  What two runs must agree on is what the device was told, so
 * nothing that differs between two equal runs is written as it is:
 *   - a COM object is named by its kind and creation number ("tex#12": the 12th texture made), a shader
 *     by a hash of its code ("sh:1a2b3c4d"), the device as "dev";
 *   - data a call points at is written instead of the pointer: constants, matrices, viewports,
 *     materials, lights and rectangles as hex dwords (a hash past 32 dwords), vertices and indices
 *     of the UP draws and a shader's code as a hash;
 *   - an output pointer is written as the object or value the call left there, after "->".
 * A call's result follows "=" when it is not 0.  The calls a state block's Apply makes are not written
 * (Apply is).  "F <n>" starts Present n's calls (n = Presents so far).
 * web/tools/dxrec-diff.mjs compares two files call by call, and replays each into the device state
 * every draw sees.  Off, the cost is one test per COM call (com.c). */
#include "cpu.h"
#include "comrec.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_comrec_on;
static FILE *g_f;
static uint32_t g_frame, g_from, g_to = 0xFFFFFFFFu;
static atomic_flag g_lock = ATOMIC_FLAG_INIT;

/* ---- objects: creation numbers by pointer (com_new calls comrec_new) ---- */
#define OBJ_BITS 18
typedef struct { uint32_t p, iface, serial; } ObjEnt;
static ObjEnt *g_obj;
static uint32_t g_serial[64];
static uint32_t oslot(uint32_t p){ return (p >> 4) * 2654435761u >> (32 - OBJ_BITS); }
void comrec_new(uint32_t obj, uint32_t iface){
    if (!g_obj) return;
    uint32_t i = oslot(obj), mask = (1u << OBJ_BITS) - 1u;
    for (uint32_t k = 0; k <= mask; k++, i = (i + 1u) & mask)
        if (!g_obj[i].p || g_obj[i].p == obj){
            g_obj[i].p = obj; g_obj[i].iface = iface; g_obj[i].serial = ++g_serial[iface & 63u]; return; }
}
static const ObjEnt *ofind(uint32_t p){
    if (!g_obj || !p) return 0;
    uint32_t i = oslot(p), mask = (1u << OBJ_BITS) - 1u;
    for (uint32_t k = 0; k <= mask && g_obj[i].p; k++, i = (i + 1u) & mask) if (g_obj[i].p == p) return &g_obj[i];
    return 0;
}
static uint32_t fnv(uint32_t h, uint32_t p, uint32_t n){
    for (uint32_t i = 0; i < n; i++){ h ^= MR8(p + i); h *= 16777619u; }
    return h;
}
static const char *short_name(const char *ifn){
    static const char *const map[][2] = {
        {"IDirect3D9","d3d"},{"IDirect3DDevice9","dev"},{"IDirect3DTexture9","tex"},{"IDirect3DVertexBuffer9","vb"},
        {"IDirect3DIndexBuffer9","ib"},{"IDirect3DSurface9","surf"},{"IDirect3DSimple","obj"},
        {"IDirect3DVertexDeclaration9","decl"},{"IDirect3DStateBlock9","sb"},{"IDirect3DSwapChain9","swap"},
        {"IDirect3DQuery9","q"},{"ID3DXEffect","fx"},{"ID3DXEffectPool","pool"} };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(ifn, map[i][0])) return map[i][1];
    return ifn;
}
void comrec_obj(uint32_t v, char *buf, int n){
    int i = v ? com_obj_iface(v) : 0;
    if (!i){ snprintf(buf, (size_t)n, "%x", v); return; }
    const char *s = short_name(com_iface_name(i));
    uint32_t p, sz;
    if (com_shader_code(v, &p, &sz)){ snprintf(buf, (size_t)n, "sh:%08x", p && sz ? fnv(2166136261u, p, sz) : 0u); return; }
    if (!strcmp(s, "dev")){ snprintf(buf, (size_t)n, "dev"); return; }
    const ObjEnt *e = ofind(v);
    if (e && e->iface == (uint32_t)i) snprintf(buf, (size_t)n, "%s#%u", s, e->serial);
    else snprintf(buf, (size_t)n, "%s?", s);
}

/* ---- the line being written ---- */
typedef struct { char b[4096]; int n; uint32_t a[12]; int nargs, kind; unsigned m; uint32_t outs; } Line;
static _Thread_local Line t_line[4];
static _Thread_local int t_depth;
static void put(Line *L, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Line *L, const char *fmt, ...){
    if (L->n >= (int)sizeof L->b - 2) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(L->b + L->n, sizeof L->b - (size_t)L->n, fmt, ap);
    va_end(ap);
    if (k > 0) L->n += k;
    if (L->n > (int)sizeof L->b - 2) L->n = (int)sizeof L->b - 2;
}
static void put_obj(Line *L, uint32_t v){ char s[32]; comrec_obj(v, s, sizeof s); put(L, " %s", s); }
static int readable(uint32_t p, uint32_t n){ return p && mem_is_committed(p) && mem_is_committed(p + (n ? n - 1u : 0u)); }
/* ndw dwords at p: written out up to 32 of them, else as a hash and a count */
static void put_dw(Line *L, uint32_t p, uint32_t ndw){
    if (!p){ put(L, " null"); return; }
    if (ndw > 0x100000u || !readable(p, 4u * ndw)){ put(L, " bad"); return; }
    if (ndw > 32u){ put(L, " #%08x/%u", fnv(2166136261u, p, 4u * ndw), ndw); return; }
    put(L, " [");
    for (uint32_t i = 0; i < ndw; i++) put(L, i ? ",%x" : "%x", MR32(p + 4u * i));
    put(L, "]");
}
static void put_hash(Line *L, uint32_t p, uint32_t n){
    if (!p){ put(L, " null"); return; }
    if (n > 0x4000000u || !readable(p, n)){ put(L, " bad"); return; }
    put(L, " #%08x/%u", fnv(2166136261u, p, n), n);
}
static uint32_t prim_verts(uint32_t type, uint32_t n){
    switch (type){ case 1: return n; case 2: return 2u * n; case 3: return n + 1u; case 4: return 3u * n;
                   case 5: case 6: return n + 2u; default: return 0; }
}

/* the arguments of IDirect3DDevice9 method m (com.c's d3d9_tables.h order); 'this' is a[0] */
static void dev_args(Line *L, unsigned m, const uint32_t *a, int nargs){
    switch (m){
    case 17: for (int i = 1; i <= 2; i++) put_dw(L, a[i], 4); put(L, " %x", a[3]); put_dw(L, a[4], 0); return;  /* Present */
    case 34: put_obj(L, a[1]); put_dw(L, a[2], 4); put_obj(L, a[3]); put_dw(L, a[4], 4); put(L, " %x", a[5]); return;
    case 35: put_obj(L, a[1]); put_dw(L, a[2], 4); put(L, " %x", a[3]); return;                       /* ColorFill */
    case 30: put_obj(L, a[1]); put_dw(L, a[2], 4); put_obj(L, a[3]); put_dw(L, a[4], 2); return;      /* UpdateSurface */
    case 43: put(L, " %x", a[1]); put_dw(L, a[1] ? a[2] : 0u, 4u * a[1]); put(L, " %x %x %x %x", a[3], a[4], a[5], a[6]); return;
    case 44: case 46: put(L, " %x", a[1]); put_dw(L, a[2], 16); return;      /* SetTransform, MultiplyTransform */
    case 47: put_dw(L, a[1], 6); return;                                      /* SetViewport */
    case 49: put_dw(L, a[1], 17); return;                                     /* SetMaterial */
    case 51: put(L, " %x", a[1]); put_dw(L, a[2], 26); return;                /* SetLight */
    case 55: put(L, " %x", a[1]); put_dw(L, a[2], 4); return;                 /* SetClipPlane */
    case 62: put_dw(L, a[1], 2); return;                                      /* SetClipStatus */
    case 75: put_dw(L, a[1], 4); return;                                      /* SetScissorRect */
    case 21: put(L, " %x %x", a[1], a[2]); put_hash(L, a[3], 1536); return;  /* SetGammaRamp */
    case 16: put_hash(L, a[1], 56); return;                                   /* Reset */
    case 83: {                                                                /* DrawPrimitiveUP */
        put(L, " %x %x", a[1], a[2]); put_hash(L, a[3], prim_verts(a[1], a[2]) * a[4]); put(L, " %x", a[4]); return; }
    case 84: {                                                                /* DrawIndexedPrimitiveUP */
        put(L, " %x %x %x %x", a[1], a[2], a[3], a[4]);
        put_hash(L, a[5], prim_verts(a[1], a[4]) * (a[6] == 101u ? 2u : 4u)); put(L, " %x", a[6]);
        put_hash(L, a[7], (a[2] + a[3]) * a[8]); put(L, " %x", a[8]); return; }
    case 86: {                                                                /* CreateVertexDeclaration */
        uint32_t n = 0; while (n < 64u && readable(a[1] + 8u * n, 8) && MR16(a[1] + 8u * n) != 0xFFu) n++;
        put_dw(L, a[1], 2u * (n + 1u)); L->outs = 1u << 2; return; }
    case 91: case 106: {                                                      /* Create{Vertex,Pixel}Shader */
        uint32_t n = 0; while (n < 65536u && readable(a[1] + 4u * n, 4)){ n++; if (MR32(a[1] + 4u * (n - 1u)) == 0xFFFFu) break; }
        put_hash(L, a[1], 4u * n); L->outs = 1u << 2; return; }
    case 94: case 109: put(L, " %x", a[1]); put_dw(L, a[2], 4u * a[3]); put(L, " %x", a[3]); return;  /* Set*ShaderConstantF */
    case 96: case 111: put(L, " %x", a[1]); put_dw(L, a[2], 4u * a[3]); put(L, " %x", a[3]); return;  /* ...I */
    case 98: case 113: put(L, " %x", a[1]); put_dw(L, a[2], a[3]); put(L, " %x", a[3]); return;       /* ...B */
    default: break;
    }
    /* the rest: outputs by name (Get*, Create*, EndStateBlock), objects by name, values as they are */
    uint32_t outs = 0;
    switch (m){
    case 6: case 7: case 8: case 9: case 19: case 22: case 45: case 48: case 50: case 52: case 54: case 56:
    case 58: case 63: case 64: case 66: case 68: case 70: case 72: case 74: case 76: case 88: case 90: case 93:
    case 95: case 97: case 99: case 101: case 103: case 105: case 108: case 110: case 112: case 114:
    case 38: case 40: case 14: case 18:
        outs = 1u << (nargs - 1); break;                                      /* the last one is the output */
    case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 36:  /* Create*: the object, a handle */
        outs = 1u << (nargs - 2); break;
    case 13: case 59: case 118: outs = 1u << 2; break;
    case 61: outs = 1u << 1; break;                                           /* EndStateBlock */
    default: break;
    }
    if (m == 101) outs |= 1u << 3 | 1u << 4;                                 /* GetStreamSource: offset, stride */
    for (int i = 1; i < nargs; i++){
        if (outs & (1u << i)) put(L, " out");
        else if (((m >= 23 && m <= 29) || m == 36) && i == nargs - 1) put(L, " -");   /* pSharedHandle */
        else put_obj(L, a[i]);
    }
    L->outs = outs;
}

int comrec_pre(CPU *c, int kind, unsigned m, const char *name, int nargs){
    if (!g_comrec_on || t_depth >= 4) return -1;
    Line *L = &t_line[t_depth];
    L->n = 0; L->outs = 0; L->kind = kind; L->m = m;
    L->nargs = nargs > 12 ? 12 : nargs;
    for (int i = 0; i < L->nargs; i++) L->a[i] = MR32(c->esp + 4u + 4u * (uint32_t)i);
    put(L, "%s", name);
    if (kind == CR_DEV) dev_args(L, m, L->a, L->nargs);
    else if (kind == CR_SB) put_obj(L, L->a[0]);
    else for (int i = 1; i < L->nargs; i++) put_obj(L, L->a[i]);
    return t_depth++;
}

static void emit(const char *s, int n){
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) ;
    if (g_f && g_frame >= g_from && g_frame <= g_to){ fwrite(s, 1, (size_t)n, g_f); fputc('\n', g_f); }
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

void comrec_post(CPU *c, int tok){
    if (tok < 0) return;
    t_depth = tok;
    Line *L = &t_line[tok];
    for (int i = 1; i < L->nargs; i++)
        if (L->outs & (1u << i)){
            uint32_t p = L->a[i];
            if (p && readable(p, 4)){ put(L, " ->"); put_obj(L, MR32(p)); }
        }
    if (c->eax) put(L, " =%x", c->eax);
    emit(L->b, L->n);
    int present = (L->kind == CR_DEV && L->m == 17u) || (L->kind == CR_SWAP && L->m == 3u);
    if (present){
        while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) ;
        g_frame++;
        if (g_f && g_frame >= g_from && g_frame <= g_to) fprintf(g_f, "F %u\n", g_frame);
        if (g_f) fflush(g_f);
        if (g_f && g_frame > g_to){ fclose(g_f); g_f = 0; g_comrec_on = 0; }
        atomic_flag_clear_explicit(&g_lock, memory_order_release);
    }
}

void comrec_marker(const char *fmt, ...){
    if (!g_comrec_on) return;
    char b[512]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof b) n = (int)sizeof b - 1;
    emit(b, n);
}

/* the file, whole: at the guest's ExitProcess (win32.c) and at the host's exit */
void comrec_close(void){
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) ;
    if (g_f){ fclose(g_f); g_f = 0; }
    g_comrec_on = 0;
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

__attribute__((constructor)) static void comrec_init(void){
    const char *e = getenv("NFSU2_DXREC");
    if (!e || !*e) return;
    char path[512]; size_t k = strcspn(e, ",");
    if (k >= sizeof path) return;
    memcpy(path, e, k); path[k] = 0;
    if (e[k] == ','){ char *q; g_from = (uint32_t)strtoul(e + k + 1, &q, 10); if (*q == ',') g_to = (uint32_t)strtoul(q + 1, 0, 10); }
    g_f = fopen(path, "w");
    if (!g_f){ fprintf(stderr, "[dxrec] cannot write %s: the recorder stays off\n", path); return; }
    setvbuf(g_f, 0, _IOFBF, 1u << 20);
    g_obj = calloc(1u << OBJ_BITS, sizeof *g_obj);
    if (!g_obj){ fclose(g_f); g_f = 0; return; }
    fprintf(g_f, "dxrec 1\nF 0\n");
    g_comrec_on = 1;
    atexit(comrec_close);
    fprintf(stderr, "[dxrec] device calls of Presents %u..%u into %s\n", g_from, g_to, path);
}
