/* The Direct3D 9 device's rules (runtime/com.c, TRACKER M3-02, M3-03, M3-04;
 * GAME.md 5.7, L-085, L-087, L-088), built for wasm and called as lifted code
 * calls a COM method: arguments on the guest stack, the vtable entry's magic,
 * stdcall cleanup checked after every call.  The renderer's side (gpu_web.c)
 * is stubbed: texture creation hands out ids (or fails on request) and every
 * draw com.c hands it is recorded, so the vertices it draws can be checked
 * against a CPU reference of D3D9's primitive rules.
 *   -DD3D9_UNIT_MW  Most Wanted's answers: its real game table, runtime/mw/
 *                   game_mw.c linked as GAME=mw web/build.sh builds it (game.h
 *                   d3d_modes, d3d9_strict); without it UG2's, the GameOps
 *                   defaults (game_ug2.c leaves both fields 0:
 *                   web/test/game-table.test.mjs).
 * web/test/d3d9-wasm.test.mjs builds both (web/tools/win32unit.mjs) and runs
 * them; every check prints "ok <name>" or "FAIL <name>: why". */
#include "cpu.h"
#include "game.h"
#include "gfx.h"
#include "d3d9_tables.h"
#include "widescreen.h"
#include "enhance.h"
#include "texrep.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef D3D9_UNIT_MW
#define MW 1                                       /* g_game: game_mw.c's */
#else
const GameOps g_game = { .id = "ug2" };
#define MW 0
#endif

/* ---- the runtime around com.c ------------------------------------------- */
uint8_t g_cbits[1u << 15];
void mem_commit(uint32_t a, uint32_t n){ (void)a; (void)n; }
int g_gh_com = 1, g_gh_gbuf = 2, g_dx_in_pass = 0, g_perflog = 0;
uint64_t g_icall_hits = 0, g_timer_ticks = 0;
uint32_t g_image_base = 0x00400000u, g_image_size = 0x00100000u;
#define HEAP_LO 0x00800000u
#define HEAP_HI 0x02E00000u
static uint32_t g_bump = HEAP_LO;
static int g_fail_alloc = 0;                       /* the next allocations fail (a creator's out of memory) */
/* The buffer heap (Lock buffers) as heap.c's reuses it: a freed block goes back at once (the newest of its size
 * first) and is poisoned with 0xDD, so a pointer kept past a free reads garbage and its writes land in the next
 * owner's block.  The object heap is never reused, so a destroyed object stays readable (alive() below). */
#define HEAP_SZN ((HEAP_HI - HEAP_LO) >> 4)
static uint32_t *g_blk_sz;                         /* a live buffer block's size by (p - HEAP_LO) >> 4 */
#define FREEN 65536
static uint32_t g_free_p[FREEN], g_free_n[FREEN], g_nfree = 0;
uint32_t gheap_alloc_c(CPU *c, int heap, uint32_t n, int zero){
    (void)c;
    if (g_fail_alloc) return 0;
    if (!g_blk_sz && !(g_blk_sz = calloc(HEAP_SZN, 4))) return 0;
    n = (n + 15u) & ~15u; if (!n) n = 16u;
    uint32_t p = 0;
    if (heap == g_gh_gbuf)
        for (int i = (int)g_nfree - 1; i >= 0; i--) if (g_free_n[i] == n){
            p = g_free_p[i]; g_nfree--; g_free_p[i] = g_free_p[g_nfree]; g_free_n[i] = g_free_n[g_nfree]; break; }
    if (!p){ p = (g_bump + 15u) & ~15u; if (p + n > HEAP_HI) return 0; g_bump = p + n; }
    if (heap == g_gh_gbuf) g_blk_sz[(p - HEAP_LO) >> 4] = n;
    if (zero) memset(g_mem + p, 0, n);
    return p;
}
void gheap_free_c(CPU *c, int heap, uint32_t p){
    (void)c;
    if (heap != g_gh_gbuf || !g_blk_sz || p < HEAP_LO || p >= HEAP_HI) return;
    uint32_t n = g_blk_sz[(p - HEAP_LO) >> 4];
    if (!n) return;
    memset(g_mem + p, 0xDD, n);
    g_blk_sz[(p - HEAP_LO) >> 4] = 0;
    if (g_nfree < FREEN){ g_free_p[g_nfree] = p; g_free_n[g_nfree] = n; g_nfree++; }
}
int gheap_bump(void){ return 0; }
uint32_t gheap_usize(uint32_t p){ return (g_blk_sz && p >= HEAP_LO && p < HEAP_HI) ? g_blk_sz[(p - HEAP_LO) >> 4] : 0u; }
int gheap_check_block(CPU *c, uint32_t p, const char *w){ (void)c; (void)p; (void)w; return 0; }
int heap_census_line(char *b, size_t n){ return n ? (b[0] = 0) : 0; }   /* heap.c's [mem] line: none here */
void guest_backtrace(CPU *c, int n){ (void)c; (void)n; }
void dispatch_indirect(CPU *c, uint32_t a){ (void)c; (void)a; }
uint64_t host_us(void){ return 0; }
void vclock_tick(void){ }
void frame_pace(void){ }
void autotest_on_present(CPU *c){ (void)c; }
void watchdog_on_present(void){ }
void discord_on_present(void){ }
void dipad_present(CPU *c){ (void)c; }
void guest_cpu_mark(void){ }
void mprot_rearm(void){ }
void mprot_arm(uint32_t a){ (void)a; }
void watch_arm(uint32_t a){ (void)a; }
void perflog_guest_frame(void){ }
void texrep_frame(void){ }
void texrep_created(unsigned id, unsigned w, unsigned h, unsigned l, unsigned f, unsigned u, unsigned p){ (void)id; (void)w; (void)h; (void)l; (void)f; (void)u; (void)p; }
void texrep_uploaded(unsigned id, unsigned l, const uint8_t *d, unsigned p, unsigned r){ (void)id; (void)l; (void)d; (void)p; (void)r; }
void texrep_surface_written(unsigned id){ (void)id; }
int enh_rt_size(uint32_t eip, uint32_t bw, uint32_t bh, uint32_t *w, uint32_t *h){ (void)eip; (void)bw; (void)bh; (void)w; (void)h; return 0; }
void ws_set_mapping(float k){ (void)k; }
double ws_render_factor(void){ return 1.0; }
double ws_max_pixels(void){ return 0.0; }
float ws_ui_x(void){ return 1.f; }
float ws_ui_y(void){ return 1.f; }
float ws_persp_x(void){ return 1.f; }
float ws_persp_y(void){ return 1.f; }
int ws_fill_target(void){ return 0; }
int ws_take_border_quad(void){ return 0; }

/* the renderer: ids, and the draws it was handed */
static unsigned g_texid = 0;
static int g_fail_tex = 0;                         /* the renderer refuses the next textures */
int metal_available(void){ return 1; }
/* the renderer's format table (gpu_web.c fmt_blk): A8R8G8B8, X8R8G8B8, A8, L8 and DXT1/3/5, nothing else */
static unsigned stub_blk(unsigned f, unsigned *bpb){
    switch (f){
    case 21: case 22: *bpb = 4; return 1;
    case 28: case 41: *bpb = 1; return 1;
    case 0x31545844u: *bpb = 8; return 4;
    case 0x33545844u: case 0x35545844u: *bpb = 16; return 4;
    default: *bpb = 0; return 0;
    }
}
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned f){
    unsigned bpb, blk = stub_blk(f, &bpb), lw = w >> level;
    if (!blk) return 0;
    if (!lw) lw = 1;
    return blk == 1 ? lw * bpb : ((lw + blk - 1) / blk) * bpb;
}
unsigned metal_level_rows(unsigned h, unsigned level, unsigned f){
    unsigned bpb, blk = stub_blk(f, &bpb), lh = h >> level;
    if (!blk) return 0;
    if (!lh) lh = 1;
    return blk == 1 ? lh : (lh + blk - 1) / blk;
}
static unsigned g_uploads = 0;                     /* texture levels handed to the renderer (UnlockRect) */
void metal_upload_texture(unsigned id, unsigned level, const void *d, unsigned pitch, unsigned w, unsigned h, unsigned f){
    (void)id; (void)level; (void)d; (void)pitch; (void)w; (void)h; (void)f; g_uploads++; }
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned l, unsigned f, unsigned rt){
    unsigned b; (void)w; (void)h; (void)l; (void)rt; return g_fail_tex || !stub_blk(f, &b) ? 0u : ++g_texid; }
unsigned metal_create_texture_cube(unsigned e, unsigned l, unsigned f, unsigned rt){
    unsigned b; (void)e; (void)l; (void)rt; return g_fail_tex || !stub_blk(f, &b) ? 0u : ++g_texid; }
/* depth textures (8 stages, Most Wanted): D16, D24X8, D24S8 */
static unsigned char g_dtex[4096];
unsigned metal_create_texture_depth(unsigned w, unsigned h, unsigned f){
    (void)w; (void)h;
    if (g_fail_tex || (f != 75u && f != 77u && f != 80u)) return 0u;
    unsigned id = ++g_texid; if (id < sizeof g_dtex) g_dtex[id] = 1; return id; }
int metal_tex_is_depth(unsigned id){ return id < sizeof g_dtex ? g_dtex[id] : 0; }
void metal_set_depth_bias(unsigned b){ (void)b; }
static unsigned g_freed = 0;
void metal_free_texture(unsigned id){ (void)id; g_freed++; }
void metal_mirror_forget(uint32_t o){ (void)o; }
void metal_want_size(unsigned w, unsigned h){ (void)w; (void)h; }
void metal_resize(unsigned w, unsigned h){ (void)w; (void)h; }
void metal_reset_rts(void){ }
void metal_present(void){ }
void metal_begin_frame(void){ }
void metal_set_zsurf(unsigned z){ (void)z; }
void metal_z_forget(unsigned z){ (void)z; }
void metal_set_render_target(unsigned t){ (void)t; }
void metal_set_cull(unsigned c){ (void)c; }
void metal_set_depth(unsigned a){ (void)a; }
void metal_set_blend(unsigned k, float a){ (void)k; (void)a; }
void metal_set_blendcolor(uint32_t c){ (void)c; }
void metal_set_fog(unsigned a, unsigned b){ (void)a; (void)b; }
void metal_set_samplers(const unsigned k[4]){ (void)k; }
void metal_set_stage_tex(unsigned a, unsigned b, unsigned c){ (void)a; (void)b; (void)c; }
void metal_set_stage_hi(const unsigned t[4], const unsigned k[4]){ (void)t; (void)k; }
void metal_set_ps(int id, const float *c){ (void)id; (void)c; }
void metal_set_texproj(const float p[4]){ (void)p; }
void metal_set_ui_clip(float s){ (void)s; }
void metal_tex_size(unsigned id, unsigned *w, unsigned *h){ (void)id; if (w) *w = 1280; if (h) *h = 720; }
void metal_native_size(unsigned a, unsigned b, unsigned *w, unsigned *h){ (void)a; (void)b; if (w) *w = 1280; if (h) *h = 720; }
void metal_view_size(unsigned *w, unsigned *h){ if (w) *w = 1280; if (h) *h = 720; }
void metal_set_game_size(unsigned w, unsigned h){ (void)w; (void)h; }
void metal_set_ui_aspect(float a, float b){ (void)a; (void)b; }
#define MAXREC 40000
static TVert g_rec[MAXREC];
static unsigned g_recn = 0, g_rec_prim = 0, g_rec_calls = 0, g_rec_over = 0;
void metal_draw_tv(const TVert *tv, unsigned n, unsigned prim, unsigned tex){
    (void)tex;
    g_rec_calls++;
    if (g_rec_calls > 1 && prim != g_rec_prim) g_rec_over++;   /* one type per recorded draw */
    g_rec_prim = prim;
    for (unsigned i = 0; i < n && g_recn < MAXREC; i++) g_rec[g_recn++] = tv[i];
}
int metal_draw_raw(const void *s, unsigned n, unsigned st, unsigned p, unsigned t, int v, const float *k, unsigned nk){ (void)s; (void)n; (void)st; (void)p; (void)t; (void)v; (void)k; (void)nk; return 0; }

int com_call(CPU *c, uint32_t magic);
void com_init(void);
uint32_t com_direct3dcreate9(void);

/* ---- calling a method as the game does ------------------------------------ */
static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *name, const char *fmt, ...){
    if (ok){ g_pass++; printf("ok %s\n", name); return; }
    g_fail++;
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("FAIL %s: %s\n", name, b);
}
static CPU g_cpu;
#define STACK 0x02F80000u
static int slot_of(const Method *m, unsigned n, const char *name){
    for (unsigned i = 0; i < n; i++) if (!strcmp(m[i].name, name)) return (int)i;
    printf("FAIL table: no method %s\n", name); exit(2);
}
#define NELM(a) ((unsigned)(sizeof(a) / sizeof((a)[0])))
static const Method *tab_of(uint32_t obj, unsigned *n){
    uint32_t iface = (MR32(MR32(obj)) >> 16) & 0x1FFFu;
    switch (iface){
    case 1: *n = NELM(M_D3D9); return M_D3D9;
    case 2: *n = NELM(M_DEVICE); return M_DEVICE;
    case 3: *n = NELM(M_TEXTURE); return M_TEXTURE;
    case 4: *n = NELM(M_VBUF); return M_VBUF;
    case 5: *n = NELM(M_IBUF); return M_IBUF;
    case 6: *n = NELM(M_SURFACE); return M_SURFACE;
    default: printf("FAIL table: interface %u\n", iface); exit(2);
    }
}
static int g_stack_bad = 0;
/* method name, then its arguments after `this` */
static uint32_t call(uint32_t obj, const char *name, int n, ...){
    unsigned nm; const Method *m = tab_of(obj, &nm);
    int slot = slot_of(m, nm, name);
    uint32_t a[12]; va_list ap; va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    CPU *c = &g_cpu;
    c->esp = STACK;
    for (int i = n - 1; i >= 0; i--) PUSH32(a[i]);
    PUSH32(obj);
    PUSH32(0x00401234u);                       /* the return address com_call pops */
    com_call(c, MR32(MR32(obj) + 4u * (uint32_t)slot));
    /* stdcall: the callee took this and exactly the arguments pushed, never one more or one fewer (a cube's
     * GetCubeMapSurface, in the texture table's GetSurfaceLevel slot, is called with its own three) */
    if (c->esp != STACK && !g_stack_bad++)
        printf("FAIL stack: %s left esp 0x%08x (%d bytes off)\n", name, c->esp, (int)(c->esp - STACK));
    return c->eax;
}
static uint32_t addref(uint32_t o){ return call(o, "AddRef", 0); }
static uint32_t release(uint32_t o){ return call(o, "Release", 0); }
static int alive(uint32_t o){ return o && MR32(o) != 0; }      /* a destroyed object has its vtable cleared */

/* guest scratch: out pointers and structs */
#define OUT  0x00700000u
#define PP   0x00700100u                        /* D3DPRESENT_PARAMETERS, 56 bytes */
#define DESC 0x00700200u
#define VTX  0x00100000u
#define IDX  0x00200000u

static void present_params(uint32_t w, uint32_t h, uint32_t count){
    memset(g_mem + PP, 0, 56);
    MR32(PP) = w; MR32(PP + 4) = h; MR32(PP + 8) = 22u; MR32(PP + 12) = count;
    MR32(PP + 24) = 1u; MR32(PP + 32) = 1u; MR32(PP + 36) = 1u; MR32(PP + 40) = 75u;
}

#define E_OUTOFMEMORY      0x8007000Eu
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_DEVICELOST  0x88760868u
#define D3DERR_DEVICENOTRESET 0x88760869u

static uint32_t g_d3d, g_dev;

/* ---- M3-02: lifetimes --------------------------------------------------- */
static void t_lifetimes(void){
    /* the depth surface copied without AddRef (0x6BD7ED -> 0x982A2C -> 0x93DAC4): one Get, two Releases */
    uint32_t r = call(g_dev, "GetDepthStencilSurface", 1, OUT);
    uint32_t ds = MR32(OUT);
    check(r == 0 && alive(ds), "depth-get", "hr 0x%08x surface 0x%08x", r, ds);
    release(ds); release(ds);
    check(alive(ds), "depth-double-release", "the implicit depth surface was destroyed");
    r = call(g_dev, "SetDepthStencilSurface", 1, ds);
    MR32(OUT) = 0; call(g_dev, "GetDepthStencilSurface", 1, OUT);
    check(r == 0 && MR32(OUT) == ds, "depth-same-after", "hr 0x%08x, Get gave 0x%08x, not 0x%08x", r, MR32(OUT), ds);
    release(MR32(OUT));

    /* the cube (FUN_006BD4B0): CreateCubeTexture, then per face GetCubeMapSurface and Release of the cube */
    MR32(OUT) = 0;
    r = call(g_dev, "CreateCubeTexture", 7, 16u, 1u, 1u, 21u, 0u, OUT, 0u);
    uint32_t cube = MR32(OUT), face[6];
    check(r == 0 && alive(cube), "cube-create", "hr 0x%08x cube 0x%08x", r, cube);
    for (uint32_t f = 0; f < 6; f++){
        r = call(cube, "GetSurfaceLevel", 3, f, 0u, OUT + 4u + 4u*f);  /* the cube's slot: GetCubeMapSurface(face, level, pp) */
        face[f] = MR32(OUT + 4u + 4u*f);
        uint32_t left = release(cube);
        check(r == 0 && alive(face[f]) && left == 1u, "cube-face", "face %u: hr 0x%08x surface 0x%08x, the cube kept %u references", f, r, face[f], left);
    }
    uint32_t sd = 0;
    for (int f = 0; f < 6; f++) for (int g = f + 1; g < 6; g++) if (face[f] == face[g]) sd++;
    check(!sd, "cube-faces-distinct", "%u faces share a surface", sd);
    /* a face bound as the render target keeps its cube (the device's reference goes to the container) */
    call(g_dev, "SetRenderTarget", 2, 0u, face[5]);
    uint32_t left = release(cube);
    check(alive(cube) && alive(face[5]), "cube-kept-by-face", "the cube went while its face was bound (%u left)", left);
    /* the back buffer bound again (D3D9 refuses a NULL render target 0) */
    MR32(OUT) = 0;
    call(g_dev, "GetBackBuffer", 4, 0u, 0u, 0u, OUT);
    uint32_t bb = MR32(OUT);
    uint32_t rb = call(g_dev, "SetRenderTarget", 2, 0u, bb);
    release(bb);
    check(rb == 0u && alive(bb) && !alive(cube) && !alive(face[0]), "cube-freed",
          "SetRenderTarget(back buffer 0x%08x) 0x%08x: the cube outlived its last reference (%d) or the back buffer went (%d)",
          bb, rb, alive(cube), !alive(bb));

    /* reference counts in the guest wrapper: Create 1, AddRef 2, Release 1, a texture's surface shares the count */
    MR32(OUT) = 0;
    call(g_dev, "CreateTexture", 8, 64u, 64u, 3u, 0u, 21u, 1u, OUT, 0u);
    uint32_t t = MR32(OUT);
    uint32_t a1 = addref(t), r1 = release(t);
    call(t, "GetSurfaceLevel", 2, 1u, OUT + 32u);
    uint32_t lv = MR32(OUT + 32u), a2 = addref(lv);
    check(a1 == 2u && r1 == 1u && a2 == 3u, "refcount", "AddRef %u, Release %u, the level's AddRef %u", a1, r1, a2);
    release(lv); release(lv);
    unsigned before = g_freed;
    uint32_t last = release(t);
    check(last == 0u && !alive(t) && g_freed == before + 1u, "refcount-free", "last Release %u, alive %d, GPU frees %u",
          last, alive(t), g_freed - before);

    /* creators that made nothing say so, with a NULL pointer */
    struct { const char *name; int n; uint32_t a[9]; int outarg; } cr[] = {
        { "CreateTexture", 8, {16,16,1,0,21,1,OUT,0}, 6 },
        { "CreateCubeTexture", 7, {16,1,0,21,1,OUT,0}, 5 },
        { "CreateVolumeTexture", 9, {8,8,8,1,0,21,1,OUT,0}, 7 },
        { "CreateVertexBuffer", 6, {256,0,0,1,OUT,0}, 4 },
        { "CreateIndexBuffer", 6, {256,0,101,1,OUT,0}, 4 },
        { "CreateRenderTarget", 8, {64,64,21,0,0,0,OUT,0}, 6 },
        { "CreateDepthStencilSurface", 8, {64,64,75,0,0,0,OUT,0}, 6 },
        { "CreateOffscreenPlainSurface", 6, {64,64,21,2,OUT,0}, 4 },
    };
    for (unsigned k = 0; k < NELM(cr); k++){
        MR32(OUT) = 0xCDCDCDCDu;
        g_fail_alloc = 1;
        uint32_t a[9]; memcpy(a, cr[k].a, sizeof a);
        uint32_t hr = call(g_dev, cr[k].name, cr[k].n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
        g_fail_alloc = 0;
        char nm[64]; snprintf(nm, sizeof nm, "creator-%s", cr[k].name);
        check(hr == E_OUTOFMEMORY && MR32(OUT) == 0u, nm, "hr 0x%08x, pointer 0x%08x", hr, MR32(OUT));
    }
    /* a texture the renderer could not make: MW fails it (the game's fallback), UG2 keeps its empty texture */
    MR32(OUT) = 0xCDCDCDCDu; g_fail_tex = 1;
    uint32_t hr = call(g_dev, "CreateTexture", 8, 16u, 16u, 1u, 0u, 0x31545844u, 1u, OUT, 0u);
    g_fail_tex = 0;
    if (MW) check(hr == D3DERR_INVALIDCALL && MR32(OUT) == 0u, "creator-gpu-refused", "hr 0x%08x pointer 0x%08x", hr, MR32(OUT));
    else    check(hr == 0u && alive(MR32(OUT)), "creator-gpu-refused-ug2", "hr 0x%08x pointer 0x%08x", hr, MR32(OUT));
}

/* ---- M3-03: state rules -------------------------------------------------- */
static void t_state(void){
    /* a failed Reset reads as lost until a Reset works */
    uint32_t r0 = call(g_dev, "TestCooperativeLevel", 0);
    present_params(1280, 720, 5);                              /* more than D3DPRESENT_BACK_BUFFERS_MAX */
    uint32_t rf = call(g_dev, "Reset", 1, PP);
    uint32_t tcl = call(g_dev, "TestCooperativeLevel", 0), pr = call(g_dev, "Present", 4, 0u, 0u, 0u, 0u);
    rf = rf == D3DERR_INVALIDCALL && call(g_dev, "Reset", 1, 0u) == D3DERR_INVALIDCALL;   /* and no parameters */
    uint32_t tcl2 = call(g_dev, "TestCooperativeLevel", 0);
    present_params(1024, 768, 1);
    uint32_t rk = call(g_dev, "Reset", 1, PP);
    uint32_t tcl3 = call(g_dev, "TestCooperativeLevel", 0), pr3 = call(g_dev, "Present", 4, 0u, 0u, 0u, 0u);
    check(r0 == 0 && rf && tcl == D3DERR_DEVICENOTRESET && pr == D3DERR_DEVICELOST && tcl2 == D3DERR_DEVICENOTRESET,
          "reset-failed-lost", "TCL before 0x%08x, failed Resets %d, TCL 0x%08x, Present 0x%08x, TCL again 0x%08x", r0, rf, tcl, pr, tcl2);
    check(rk == 0 && tcl3 == 0 && pr3 == 0, "reset-ok-clears", "Reset 0x%08x, TCL 0x%08x, Present 0x%08x", rk, tcl3, pr3);
    present_params(9000, 9000, 1);
    uint32_t rg = call(g_dev, "Reset", 1, PP), tg = call(g_dev, "TestCooperativeLevel", 0);
    if (MW) check(rg == D3DERR_INVALIDCALL && tg == D3DERR_DEVICENOTRESET, "reset-size", "Reset 0x%08x TCL 0x%08x", rg, tg);
    else    check(rg == 0 && tg == 0, "reset-size-ug2", "Reset 0x%08x TCL 0x%08x (UG2 keeps its size)", rg, tg);
    present_params(1280, 720, 1); call(g_dev, "Reset", 1, PP);

    /* GetLevelCount is a count; the level descriptions */
    MR32(OUT) = 0;
    call(g_dev, "CreateTexture", 8, 256u, 64u, 4u, 1u, 21u, 0u, OUT, 0u);
    uint32_t t = MR32(OUT);
    uint32_t lc = call(t, "GetLevelCount", 0);
    check(lc == 4u, "levelcount", "GetLevelCount %u", lc);
    memset(g_mem + DESC, 0xAB, 64);
    uint32_t hd = call(t, "GetLevelDesc", 2, 2u, DESC);
    uint32_t f = MR32(DESC), ty = MR32(DESC + 4), us = MR32(DESC + 8), w = MR32(DESC + 24), h = MR32(DESC + 28);
    if (MW) check(hd == 0 && f == 21u && ty == 1u && us == 1u && MR32(DESC + 12) == 0u && w == 64u && h == 16u
                  && MR32(DESC + 32) == 0xABABABABu, "leveldesc", "hr 0x%08x fmt %u type %u usage %u pool %u %ux%u past 0x%08x",
                  hd, f, ty, us, MR32(DESC + 12), w, h, MR32(DESC + 32));
    else    check(hd == 0 && f == 21u && w == 256u && h == 64u, "leveldesc-ug2", "hr 0x%08x fmt %u %ux%u (UG2's base size)", hd, f, w, h);
    if (MW){
        uint32_t hb = call(t, "GetLevelDesc", 2, 4u, DESC);
        check(hb == D3DERR_INVALIDCALL, "leveldesc-past", "level 4 of 4: hr 0x%08x", hb);
        call(t, "GetSurfaceLevel", 2, 3u, OUT + 8u);
        uint32_t sf = MR32(OUT + 8u);
        call(sf, "GetDesc", 1, DESC);
        check(MR32(DESC + 24) == 32u && MR32(DESC + 28) == 8u && MR32(DESC + 8) == 1u, "surfacedesc-level",
              "level 3's surface %ux%u usage %u", MR32(DESC + 24), MR32(DESC + 28), MR32(DESC + 8));
        release(sf);
        /* buffers: D3DVERTEXBUFFER_DESC is 24 bytes, D3DINDEXBUFFER_DESC 20, nothing written past them */
        call(g_dev, "CreateVertexBuffer", 6, 4096u, 8u, 0x44u, 0u, OUT + 12u, 0u);
        call(g_dev, "CreateIndexBuffer", 6, 600u, 8u, 102u, 1u, OUT + 16u, 0u);
        memset(g_mem + DESC, 0xAB, 64);
        call(MR32(OUT + 12u), "GetDesc", 1, DESC);
        int vok = MR32(DESC) == 100u && MR32(DESC + 4) == 6u && MR32(DESC + 8) == 8u && MR32(DESC + 16) == 4096u
               && MR32(DESC + 20) == 0x44u && MR32(DESC + 24) == 0xABABABABu;
        memset(g_mem + DESC, 0xAB, 64);
        call(MR32(OUT + 16u), "GetDesc", 1, DESC);
        int iok = MR32(DESC) == 102u && MR32(DESC + 4) == 7u && MR32(DESC + 12) == 1u && MR32(DESC + 16) == 600u
               && MR32(DESC + 20) == 0xABABABABu;
        check(vok && iok, "bufferdesc", "vertex buffer %d, index buffer %d", vok, iok);
        uint32_t gt = call(MR32(OUT + 12u), "GetType", 0), gi = call(MR32(OUT + 16u), "GetType", 0);
        check(gt == 6u && gi == 7u, "gettype", "vertex buffer %u, index buffer %u", gt, gi);
        release(MR32(OUT + 12u)); release(MR32(OUT + 16u));
        /* a volume texture: its own type and D3DVOLUME_DESC (28 bytes, nothing past it) */
        MR32(OUT + 20u) = 0;
        call(g_dev, "CreateVolumeTexture", 9, 16u, 8u, 4u, 2u, 0u, 21u, 1u, OUT + 20u, 0u);
        uint32_t vt = MR32(OUT + 20u);
        uint32_t vty = call(vt, "GetType", 0), vlc = call(vt, "GetLevelCount", 0);
        memset(g_mem + DESC, 0xAB, 64);
        uint32_t vh = call(vt, "GetLevelDesc", 2, 1u, DESC);
        int volok = vh == 0u && MR32(DESC) == 21u && MR32(DESC + 4) == 2u && MR32(DESC + 8) == 0u && MR32(DESC + 12) == 1u
               && MR32(DESC + 16) == 8u && MR32(DESC + 20) == 4u && MR32(DESC + 24) == 2u && MR32(DESC + 28) == 0xABABABABu;
        uint32_t vpast = call(vt, "GetLevelDesc", 2, 2u, DESC);
        uint32_t tty = call(t, "GetType", 0);
        check(vty == 4u && vlc == 2u && volok && vpast == D3DERR_INVALIDCALL && tty == 3u, "volumedesc",
              "type %u (a texture's %u), %u levels, level 1: hr 0x%08x fmt %u type %u %ux%ux%u past 0x%08x, level 2 0x%08x",
              vty, tty, vlc, vh, MR32(DESC), MR32(DESC + 4), MR32(DESC + 16), MR32(DESC + 20), MR32(DESC + 24),
              MR32(DESC + 28), vpast);
        release(vt);
    }
    release(t);

    /* the mode list: MW's classic 4:3 modes first, UG2's own six rows */
    uint32_t n = call(g_d3d, "GetAdapterModeCount", 2, 0u, 22u);
    uint32_t got[16][2] = {{0}}, bad = 0;
    for (uint32_t i = 0; i < n && i < 16; i++){
        if (call(g_d3d, "EnumAdapterModes", 4, 0u, 22u, i, DESC)) bad++;
        got[i][0] = MR32(DESC); got[i][1] = MR32(DESC + 4);
    }
    uint32_t past = call(g_d3d, "EnumAdapterModes", 4, 0u, 22u, n, DESC);
    static const uint32_t classic[6][2] = {{640,480},{800,600},{1024,768},{1280,960},{1280,1024},{1600,1200}};
    int first = n >= 6u; for (int i = 0; i < 6 && first; i++) first = got[i][0] == classic[i][0] && got[i][1] == classic[i][1];
    if (MW){
        /* game_mw.c's own rows, in order: the six classic 4:3 modes, then wide ones that match none of them */
        uint32_t rows = 0, same = 1, wide = 1;
        while (g_game.d3d_modes && rows < 16u && g_game.d3d_modes[rows][0]) rows++;
        for (uint32_t i = 0; i < rows && i < n; i++) same &= got[i][0] == g_game.d3d_modes[i][0] && got[i][1] == g_game.d3d_modes[i][1];
        for (uint32_t i = 6; i < n && i < 16; i++) for (int k = 0; k < 6; k++)
            if (got[i][0] == classic[k][0] && got[i][1] == classic[k][1]) wide = 0;
        check(rows > 6u && n == rows && same && !bad && first && wide && past == D3DERR_INVALIDCALL, "modes",
              "%u modes of the table's %u, as the table %d, %u failed, classic first %d, the rest wide %d, past the end 0x%08x",
              n, rows, same, bad, first, wide, past);
    }
    else    check(n == 6u && !bad && past == D3DERR_INVALIDCALL, "modes-ug2", "%u modes, %u failed, past the end 0x%08x", n, bad, past);

    /* multisampling: MW is told what the renderer draws (none), UG2 keeps D3D_OK */
    MR32(OUT) = 0xCDCDCDCDu;
    uint32_t ms = call(g_d3d, "CheckDeviceMultiSampleType", 6, 0u, 1u, 21u, 0u, 1u, OUT);
    uint32_t q = MR32(OUT);
    MR32(OUT) = 0xCDCDCDCDu;
    uint32_t m0 = call(g_d3d, "CheckDeviceMultiSampleType", 6, 0u, 1u, 21u, 0u, 0u, OUT);
    if (MW) check(ms == D3DERR_NOTAVAILABLE && q == 0u && m0 == 0u && MR32(OUT) == 1u, "multisample",
                  "NONMASKABLE 0x%08x quality %u, NONE 0x%08x quality %u", ms, q, m0, MR32(OUT));
    else    check(ms == 0u && q == 0xCDCDCDCDu, "multisample-ug2", "0x%08x quality 0x%08x (UG2's answer)", ms, q);

    /* CheckDeviceFormat: MW is told what the creators make, a texture format CreateTexture makes and the depth
     * textures of 8 stages (D16, D24X8, D24S8; its shadow-map probes: D24S8 at 0x6C1725 made, 'DF16' at 0x6C175D,
     * DF24 and INTZ not, docs/web/gpu-abi.md 11.4); UG2 keeps D3D_OK */
    static const uint32_t fm[] = { 21u, 22u, 23u, 25u, 28u, 41u, 75u, 113u, 0x31545844u, 0x35545844u, 0x36314644u, 0x5A544E49u };
    unsigned agree = 0;
    for (unsigned k = 0; k < NELM(fm); k++) for (uint32_t u = 0; u < 2u; u++){        /* plain, render target */
        uint32_t ck = call(g_d3d, "CheckDeviceFormat", 6, 0u, 1u, 22u, u, 3u, fm[k]);
        MR32(OUT) = 0;
        uint32_t cr = call(g_dev, "CreateTexture", 8, 16u, 16u, 1u, u, fm[k], u ? 0u : 1u, OUT, 0u);
        if (!cr) release(MR32(OUT));
        if ((ck == 0u) == (cr == 0u)) agree++;
    }
    uint32_t d24 = call(g_d3d, "CheckDeviceFormat", 6, 0u, 1u, 22u, 2u, 3u, 75u);
    uint32_t df16 = call(g_d3d, "CheckDeviceFormat", 6, 0u, 1u, 22u, 2u, 3u, 0x36314644u);
    /* the depth textures the renderer makes and those it does not, each against CreateTexture */
    static const uint32_t dfm[] = { 75u, 77u, 80u, 0x36314644u, 0x34324644u, 0x5A544E49u, 113u, 21u };
    unsigned dagree = 0, dmade = 0;
    for (unsigned k = 0; k < NELM(dfm); k++){
        uint32_t ck = call(g_d3d, "CheckDeviceFormat", 6, 0u, 1u, 22u, 2u, 3u, dfm[k]);
        MR32(OUT) = 0;
        uint32_t cr = call(g_dev, "CreateTexture", 8, 64u, 64u, 1u, 2u, dfm[k], 0u, OUT, 0u);
        if (!cr){ dmade++; release(MR32(OUT)); }
        if ((ck == 0u) == (cr == 0u)) dagree++;
    }
    if (MW) check(dagree == NELM(dfm) && dmade == 3u, "depthtex", "%u of %u depth-texture answers agree with CreateTexture, "
                  "%u made (D16, D24X8, D24S8)", dagree, (unsigned)NELM(dfm), dmade);
    /* the caps' shader versions (offsets 196 and 204): MW 3.0 with PS20Caps.NumTemps 32 (GameOps d3d_sm3) and
     * StretchRectFilterCaps (0xF4) min and mag point/linear, UG2 2.0 and none */
    MR32(OUT) = 0; mem_commit(OUT, 0x130);
    uint32_t gc = call(g_d3d, "GetDeviceCaps", 3, 0u, 1u, OUT);
    uint32_t vsv = MR32(OUT + 196u), psv = MR32(OUT + 204u), ntemps = MR32(OUT + 0x110u), srf = MR32(OUT + 0xF4u);
    if (MW) check(gc == 0u && vsv == 0xFFFE0300u && psv == 0xFFFF0300u && ntemps == 32u && srf == 0x03000300u, "caps-sm3",
                  "GetDeviceCaps 0x%08x: vs 0x%08x ps 0x%08x, PS20Caps.NumTemps %u, StretchRectFilterCaps 0x%08x",
                  gc, vsv, psv, ntemps, srf);
    else    check(gc == 0u && vsv == 0xFFFE0200u && psv == 0xFFFF0200u && ntemps == 0u && srf == 0u, "caps-sm2-ug2",
                  "GetDeviceCaps 0x%08x: vs 0x%08x ps 0x%08x, PS20Caps.NumTemps %u, StretchRectFilterCaps 0x%08x "
                  "(UG2's 2.0)", gc, vsv, psv, ntemps, srf);
    uint32_t dss = call(g_d3d, "CheckDeviceFormat", 6, 0u, 1u, 22u, 2u, 1u, 75u);
    uint32_t dsc = call(g_d3d, "CheckDeviceFormat", 6, 0u, 1u, 22u, 2u, 1u, 21u);
    uint32_t rts = call(g_d3d, "CheckDeviceFormat", 6, 0u, 1u, 22u, 1u, 1u, 23u);
    uint32_t dm = call(g_d3d, "CheckDepthStencilMatch", 5, 0u, 1u, 22u, 22u, 75u);
    uint32_t dmc = call(g_d3d, "CheckDepthStencilMatch", 5, 0u, 1u, 22u, 22u, 21u);
    if (MW) check(agree == 2u * NELM(fm) && d24 == 0u && df16 == D3DERR_NOTAVAILABLE && dss == 0u
                  && dsc == D3DERR_NOTAVAILABLE && rts == D3DERR_NOTAVAILABLE && dm == 0u && dmc == D3DERR_NOTAVAILABLE,
                  "checkformat", "%u of %u agree with CreateTexture; depth textures D24S8 0x%08x DF16 0x%08x; depth surface "
                  "D24S8 0x%08x A8R8G8B8 0x%08x; R5G6B5 target 0x%08x; depth match 0x%08x, with a colour format 0x%08x",
                  agree, 2u * NELM(fm), d24, df16, dss, dsc, rts, dm, dmc);
    else    check(d24 == 0u && df16 == 0u && dss == 0u && dsc == 0u && dmc == 0u, "checkformat-ug2",
                  "D24S8 0x%08x DF16 0x%08x, surfaces 0x%08x 0x%08x, match 0x%08x (UG2's D3D_OK)", d24, df16, dss, dsc, dmc);
}

/* ---- M3-04: every primitive type against D3D9's rules (CPU reference) ----- */
/* vertex k: a FLOAT3 position, a D3DCOLOR with k's low byte in R and B and its high byte in G, padding to the stride */
static void make_vertices(uint32_t base, uint32_t stride, uint32_t n){
    for (uint32_t k = 0; k < n; k++){
        uint32_t o = base + k * stride;
        memset(g_mem + o, 0, stride);
        MRF32(o) = -0.9f + 0.007f * (float)k; MRF32(o + 4) = 0.6f * sinf(0.7f * (float)k) + 0.05f * (float)(k & 1);
        MRF32(o + 8) = 0.5f; MR32(o + 12) = 0xFF000000u | ((k & 0xFFu) << 16) | (((k >> 8) & 0xFFu) << 8) | (k & 0xFFu);
    }
}
static int vid(const TVert *v){ return (int)lroundf(v->r * 255.f) + 256 * (int)lroundf(v->g * 255.f); }
/* the reference: the vertex sequence D3D9 draws for nprim primitives of type pt (vertex indices) */
static unsigned reference(uint32_t pt, uint32_t nprim, int *out){
    unsigned n = 0;
    for (uint32_t i = 0; i < nprim; i++) switch (pt){
    case 1: out[n++] = (int)i; break;
    case 2: out[n++] = (int)(2*i); out[n++] = (int)(2*i + 1); break;
    case 3: out[n++] = (int)i; out[n++] = (int)(i + 1); break;
    case 4: out[n++] = (int)(3*i); out[n++] = (int)(3*i + 1); out[n++] = (int)(3*i + 2); break;
    case 5: if (i & 1){ out[n++] = (int)(i + 1); out[n++] = (int)i; } else { out[n++] = (int)i; out[n++] = (int)(i + 1); }
            out[n++] = (int)(i + 2); break;
    default: out[n++] = 0; out[n++] = (int)(i + 1); out[n++] = (int)(i + 2); break;
    }
    return n;
}
/* what the renderer was handed, back as the vertex sequence of its primitives:
 * lists as they are; a line (two triangles across it) as its two ends; a point
 * (two triangles around it) as its vertex; a strip or fan as D3D9 would order it */
static unsigned recorded(int *out, uint32_t *ptype){
    unsigned n = 0;
    *ptype = g_rec_prim;
    if (g_rec_prim == 5 || g_rec_prim == 6){
        int seq[4096]; unsigned m = 0;
        for (unsigned i = 0; i < g_recn && m < 4096; i++) seq[m++] = vid(&g_rec[i]);
        for (unsigned t = 0; t + 2 < m; t++){
            if (g_rec_prim == 6){ out[n++] = seq[0]; out[n++] = seq[t + 1]; out[n++] = seq[t + 2]; }
            else if (t & 1){ out[n++] = seq[t + 1]; out[n++] = seq[t]; out[n++] = seq[t + 2]; }
            else { out[n++] = seq[t]; out[n++] = seq[t + 1]; out[n++] = seq[t + 2]; }
        }
        return n;
    }
    for (unsigned i = 0; i < g_recn; i++) out[n++] = vid(&g_rec[i]);
    return n;
}
/* lines and points come as quads: 6 vertices each, the ends a b a a' b b (draw_lines), 6 of one (draw_points) */
static unsigned unquad(const int *in, unsigned n, uint32_t pt, int *out){
    unsigned m = 0;
    for (unsigned i = 0; i + 5 < n + 0u; i += 6){
        if (pt == 1){ out[m++] = in[i]; }
        else { out[m++] = in[i]; out[m++] = in[i + 1]; }
    }
    return m;
}
static int same_tris(const int *a, const int *b, unsigned n, int rot){
    for (unsigned t = 0; t + 2 < n + 1u && t < n; t += 3){
        int ok = a[t] == b[t] && a[t+1] == b[t+1] && a[t+2] == b[t+2];
        if (!ok && rot) ok = (a[t] == b[t+1] && a[t+1] == b[t+2] && a[t+2] == b[t])
                          || (a[t] == b[t+2] && a[t+1] == b[t] && a[t+2] == b[t+1]);
        if (!ok) return 0;
    }
    return 1;
}
static const char *PTN[7] = {"?", "points", "lines", "linestrip", "triangles", "tristrip", "fan"};
/* api: 0 DrawPrimitiveUP, 1 DrawPrimitive, 2 DrawIndexedPrimitive, 3 DrawIndexedPrimitiveUP */
static void t_prim(int api, uint32_t pt, uint32_t nprim, uint32_t stride){
    static const char *AN[4] = {"up", "vb", "indexed", "indexed-up"};
    uint32_t nv = pt == 1 ? nprim : pt == 2 ? 2*nprim : pt == 3 ? nprim + 1 : pt == 4 ? 3*nprim : nprim + 2;
    /* indexed: index k names vertex perm(k) (reversed), so the check sees the indices were followed */
    uint32_t vb = 0, ib = 0;
    make_vertices(VTX, stride, nv);
    g_recn = 0; g_rec_calls = 0; g_rec_over = 0;
    if (api == 0) call(g_dev, "DrawPrimitiveUP", 4, pt, nprim, VTX, stride);
    else if (api == 1){
        call(g_dev, "CreateVertexBuffer", 6, nv * stride + 3u * stride, 0u, 0u, 0u, OUT, 0u);
        vb = MR32(OUT);
        call(vb, "Lock", 4, 0u, 0u, OUT + 4u, 0u);
        uint32_t p = MR32(OUT + 4u);
        make_vertices(p, stride, 3);                           /* StartVertex 3: three before the run */
        memcpy(g_mem + p + 3u * stride, g_mem + VTX, nv * stride);
        call(vb, "Unlock", 0);
        call(g_dev, "SetStreamSource", 4, 0u, vb, 0u, stride);
        call(g_dev, "DrawPrimitive", 3, pt, 3u, nprim);
    } else {
        for (uint32_t k = 0; k < nv; k++) MR16(IDX + 2u*k) = (uint16_t)(nv - 1u - k);
        if (api == 3) call(g_dev, "DrawIndexedPrimitiveUP", 8, pt, 0u, nv, nprim, IDX, 101u, VTX, stride);
        else {
            call(g_dev, "CreateVertexBuffer", 6, nv * stride, 0u, 0u, 0u, OUT, 0u);
            vb = MR32(OUT);
            call(vb, "Lock", 4, 0u, 0u, OUT + 4u, 0u);
            memcpy(g_mem + MR32(OUT + 4u), g_mem + VTX, nv * stride);
            call(vb, "Unlock", 0);
            call(g_dev, "CreateIndexBuffer", 6, 2u * nv + 8u, 0u, 101u, 0u, OUT + 8u, 0u);
            ib = MR32(OUT + 8u);
            call(ib, "Lock", 4, 0u, 0u, OUT + 12u, 0u);
            uint32_t ip = MR32(OUT + 12u);
            MR16(ip) = 0xFFFFu; MR16(ip + 2) = 0xFFFFu;        /* StartIndex 2 */
            memcpy(g_mem + ip + 4u, g_mem + IDX, 2u * nv);
            call(ib, "Unlock", 0);
            call(g_dev, "SetStreamSource", 4, 0u, vb, 0u, stride);
            call(g_dev, "SetIndices", 1, ib);
            call(g_dev, "DrawIndexedPrimitive", 6, pt, 0u, 0u, nv, 2u, nprim);
        }
    }
    static int want[40000], got[40000], raw[40000];
    unsigned nw = reference(pt, nprim, want);
    if (api >= 2) for (unsigned i = 0; i < nw; i++) want[i] = (int)(nv - 1u) - want[i];
    uint32_t rp = 0;
    unsigned nr = recorded(raw, &rp), ng;
    if (pt <= 3) ng = unquad(raw, nr, pt, got); else { memcpy(got, raw, nr * sizeof *got); ng = nr; }
    int ok = ng == nw;
    /* triangles: D3D9's order, each from its first vertex (flat shading's); only UG2's indexed strips and fans, the
     * old path, may give the same winding from another corner */
    if (ok && pt >= 4) ok = same_tris(got, want, nw, api == 2 && !MW);
    else if (ok) for (unsigned i = 0; i < nw && ok; i++) ok = got[i] == want[i];
    char name[64]; snprintf(name, sizeof name, "prim-%s-%s-%u", AN[api], PTN[pt], stride);
    check(ok && !g_rec_over, name, "%u primitives: %u vertices recorded (type %u, %u calls), %u expected; first got %d %d %d want %d %d %d",
          nprim, ng, rp, g_rec_calls, nw, ng > 0 ? got[0] : -1, ng > 1 ? got[1] : -1, ng > 2 ? got[2] : -1,
          want[0], nw > 1 ? want[1] : -1, nw > 2 ? want[2] : -1);
    if (vb){ call(g_dev, "SetStreamSource", 4, 0u, 0u, 0u, 0u); release(vb); }
    if (ib){ call(g_dev, "SetIndices", 1, 0u); release(ib); }
}

/* ---- what the game sees through Lock: D3D9's MANAGED pool, whose copy stays where Lock showed it -------------
 * Most Wanted keeps a static buffer's or texture level's copy outside the guest (game.h d3d_host_backing) and
 * shows it in a window of the buffer heap between Lock and Unlock; UG2 keeps one block for the resource's life.
 * Both must read back what was written through any Lock. */
#define LK_RO   0x10u                              /* D3DLOCK_READONLY */
#define LK_NOOV 0x1000u                            /* D3DLOCK_NOOVERWRITE */
#define LK_DISC 0x2000u                            /* D3DLOCK_DISCARD */
int com_gbuf_census(char *, size_t);
static unsigned gk_count(const char *kind){        /* live buffer-heap blocks of one kind ([mem] census) */
    char b[1024], k[16]; com_gbuf_census(b, sizeof b); snprintf(k, sizeof k, " %s=", kind);
    const char *s = strstr(b, k); float mb; unsigned n = 0;
    if (s) sscanf(s + strlen(k), "%fMB/%u", &mb, &n);
    return n;
}
static uint32_t vb_make(uint32_t len){ MR32(OUT) = 0; call(g_dev, "CreateVertexBuffer", 6, len, 0u, 0u, 1u, OUT, 0u); return MR32(OUT); }
static uint32_t vb_lock(uint32_t vb, uint32_t off, uint32_t sz, uint32_t fl){
    MR32(OUT + 4u) = 0; call(vb, "Lock", 4, off, sz, OUT + 4u, fl); return MR32(OUT + 4u); }
static void vb_unlock(uint32_t vb){ call(vb, "Unlock", 0); }
static uint32_t tex_make(uint32_t w, uint32_t h){ MR32(OUT) = 0; call(g_dev, "CreateTexture", 8, w, h, 1u, 0u, 21u, 1u, OUT, 0u); return MR32(OUT); }
static uint32_t tex_lock(uint32_t t, uint32_t fl){ MR32(OUT + 20u) = 0; call(t, "LockRect", 4, 0u, OUT + 16u, 0u, fl); return MR32(OUT + 20u); }
static void tex_unlock(uint32_t t){ call(t, "UnlockRect", 1, 0u); }
static void pat(uint32_t p, uint32_t n, uint8_t seed){ for (uint32_t i = 0; i < n; i++) MR8(p + i) = (uint8_t)(seed + i * 7u); }
static int is_pat(uint32_t p, uint32_t n, uint8_t seed){
    if (!p) return 0;
    for (uint32_t i = 0; i < n; i++) if (MR8(p + i) != (uint8_t)(seed + i * 7u)) return 0;
    return 1; }
#define PAT_AT(seed, off) ((uint8_t)((seed) + (off) * 7u))   /* the pattern's seed for the bytes from off */
static int is_byte(uint32_t p, uint32_t n, uint8_t v){
    if (!p) return 0;
    for (uint32_t i = 0; i < n; i++) if (MR8(p + i) != v) return 0;
    return 1; }
static void present(void){ call(g_dev, "Present", 4, 0u, 0u, 0u, 0u); }
static void t_hostback(void){
    const uint32_t N = 1024;
    unsigned vb0 = gk_count("vb"), tx0 = gk_count("tex");
    uint32_t vb = vb_make(N), p, q, t;
    p = vb_lock(vb, 0, 0, 0); pat(p, N, 1); vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO); int ok = is_pat(q, N, 1); vb_unlock(vb);
    check(p && ok, "hb-roundtrip", "Lock 0x%08x, a whole Lock's writes read back %d", p, ok);
    /* a Lock of part: its window holds the buffer's bytes there; the rest stays */
    p = vb_lock(vb, 100, 50, 0); int pre = is_pat(p, 50, PAT_AT(1, 100)); if (p) memset(g_mem + p, 0xAA, 50); vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO);
    check(pre && is_byte(q + 100, 50, 0xAA) && is_pat(q, 100, 1) && is_pat(q + 150, N - 150, PAT_AT(1, 150)), "hb-partial",
          "Lock(100,50): the old bytes there %d, written %d, before kept %d, after kept %d", pre, is_byte(q + 100, 50, 0xAA),
          is_pat(q, 100, 1), is_pat(q + 150, N - 150, PAT_AT(1, 150)));
    vb_unlock(vb);
    p = vb_lock(vb, 200, 20, LK_NOOV); pre = is_pat(p, 20, PAT_AT(1, 200)); if (p) memset(g_mem + p, 0xBB, 20); vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO);
    check(pre && is_byte(q + 200, 20, 0xBB) && is_pat(q + 220, N - 220, PAT_AT(1, 220)), "hb-nooverwrite",
          "NOOVERWRITE(200,20): the old bytes there %d, written %d, after kept %d", pre, is_byte(q + 200, 20, 0xBB),
          is_pat(q + 220, N - 220, PAT_AT(1, 220)));
    vb_unlock(vb); release(vb);

    /* written outside the locked range, as D3D9's managed copy (one mapping of the whole buffer) and UG2's block
     * take it: kept, and another buffer's window, locked meanwhile, keeps its own bytes */
    vb = vb_make(N); p = vb_lock(vb, 0, 0, 0); pat(p, N, 2); vb_unlock(vb);
    uint32_t nb = vb_make(N); p = vb_lock(nb, 0, 0, 0); pat(p, N, 3); vb_unlock(nb);
    p = vb_lock(vb, 100, 50, 0);
    uint32_t nq = vb_lock(nb, 0, 0, 0);
    if (p){ memset(g_mem + p + 50, 0xE1, 300); memset(g_mem + p - 100, 0xE2, 100); }   /* [150,450) and [0,100) */
    int nb_ok = is_pat(nq, N, 3); vb_unlock(nb); vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO);
    int past = is_byte(q + 150, 300, 0xE1), before = is_byte(q, 100, 0xE2), rest = is_pat(q + 450, N - 450, PAT_AT(2, 450));
    vb_unlock(vb);
    nq = vb_lock(nb, 0, 0, LK_RO); nb_ok = nb_ok && is_pat(nq, N, 3); vb_unlock(nb);
    check(past && before && rest && nb_ok, "hb-past-range", "Lock(100,50), then [150,450) and [0,100) written: kept %d %d, the rest %d; "
          "the other buffer's bytes %d", past, before, rest, nb_ok);
    release(nb); release(vb);

    /* nested Locks (D3D9: one lock count; every pointer stays valid until the last Unlock) */
    vb = vb_make(N); p = vb_lock(vb, 0, 0, 0); pat(p, N, 3); vb_unlock(vb);
    uint32_t p1 = vb_lock(vb, 0, 0, 0), p2 = vb_lock(vb, 0, 0, 0);
    if (p2) MR8(p2 + 10) = 0x11;
    vb_unlock(vb);
    if (p1) MR8(p1 + 20) = 0x22;                        /* between the two Unlocks */
    vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO);
    check(p1 && p1 == p2 && MR8(q + 10) == 0x11 && MR8(q + 20) == 0x22 && is_pat(q + 21, N - 21, PAT_AT(3, 21)), "hb-nested",
          "same pointer %d; written before the 1st Unlock %d, between the Unlocks %d", p1 == p2, MR8(q + 10) == 0x11, MR8(q + 20) == 0x22);
    vb_unlock(vb);
    p1 = vb_lock(vb, 0, 64, 0); p2 = vb_lock(vb, 128, 64, 0);
    if (p1) memset(g_mem + p1, 0x33, 64);
    if (p2) memset(g_mem + p2, 0x44, 64);
    vb_unlock(vb); vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO);
    check(is_byte(q, 64, 0x33) && is_byte(q + 128, 64, 0x44) && is_pat(q + 64, 64, PAT_AT(3, 64)), "hb-nested-disjoint",
          "Lock(0,64) + Lock(128,64), both written after both Locks: [0,64) %d, [128,192) %d, [64,128) kept %d",
          is_byte(q, 64, 0x33), is_byte(q + 128, 64, 0x44), is_pat(q + 64, 64, PAT_AT(3, 64)));
    vb_unlock(vb);
    p1 = vb_lock(vb, 0, 0, LK_RO); p2 = vb_lock(vb, 0, 0, 0);
    if (p2) MR8(p2 + 30) = 0x55;
    vb_unlock(vb); vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO);
    check(MR8(q + 30) == 0x55, "hb-nested-readonly", "a writing Lock inside a READONLY one: the write %s", MR8(q + 30) == 0x55 ? "kept" : "lost");
    vb_unlock(vb); release(vb);

    /* one texture level locked twice before its UnlockRects: one window, uploaded at each UnlockRect, the
     * writes between them kept */
    t = tex_make(64, 64); p = tex_lock(t, 0); pat(p, 64*64*4, 10); tex_unlock(t);
    unsigned u0 = g_uploads;
    p1 = tex_lock(t, 0); p2 = tex_lock(t, 0);
    tex_unlock(t); unsigned u1 = g_uploads;
    if (p1) MR8(p1 + 4) = 0x5E;
    tex_unlock(t); unsigned u2 = g_uploads;
    q = tex_lock(t, LK_RO);
    check(p1 && p1 == p2 && u1 - u0 == 1u && u2 - u1 == 1u && MR8(q + 4) == 0x5E && is_pat(q + 5, 64*64*4 - 5, PAT_AT(10, 5)), "hb-tex-nested",
          "same pointer %d; uploads at the 1st UnlockRect %u, at the 2nd %u; the write between them %s", p1 == p2, u1 - u0, u2 - u1,
          MR8(q + 4) == 0x5E ? "kept" : "lost");
    tex_unlock(t); release(t);

    /* DISCARD on a static buffer or texture (valid only on DYNAMIC ones): the bytes not written stay */
    vb = vb_make(N); p = vb_lock(vb, 0, 0, 0); pat(p, N, 4); vb_unlock(vb);
    p = vb_lock(vb, 300, 16, LK_DISC); if (p) memset(g_mem + p, 0xCC, 8); vb_unlock(vb);
    q = vb_lock(vb, 0, 0, LK_RO); int dv = is_byte(q + 300, 8, 0xCC) && is_pat(q + 308, N - 308, PAT_AT(4, 308)) && is_pat(q, 300, 4); vb_unlock(vb);
    release(vb);
    t = tex_make(64, 64); p = tex_lock(t, 0); pat(p, 64*64*4, 6); tex_unlock(t);
    p = tex_lock(t, LK_DISC); if (p) memset(g_mem + p, 0xCC, 8); tex_unlock(t);
    q = tex_lock(t, LK_RO); int dt = is_byte(q, 8, 0xCC) && is_pat(q + 8, 64*64*4 - 8, PAT_AT(6, 8)); tex_unlock(t);
    release(t);
    check(dv && dt, "hb-discard-static", "8 of a DISCARD Lock's bytes written, the rest kept: buffer %d, texture %d", dv, dt);

    /* a pointer kept after Unlock (D3D9 MANAGED: the copy stays there): it reads its resource's data, another
     * resource's window is not put there, and a write through it is its own resource's */
    uint32_t a = vb_make(N), b = vb_make(N);
    uint32_t pa = vb_lock(a, 0, 0, 0); pat(pa, N, 5); vb_unlock(a);
    int reads = is_pat(pa, N, 5);
    uint32_t pb = vb_lock(b, 0, 0, 0); pat(pb, N, 9);
    if (pa) MR8(pa + 40) = 0x99;                        /* the game writes through A's old pointer */
    vb_unlock(b);
    q = vb_lock(b, 0, 0, LK_RO); int b_ok = is_pat(q, N, 9); vb_unlock(b);
    q = vb_lock(a, 0, 0, LK_RO); int in_a = MR8(q + 40) == 0x99 && is_pat(q, 40, 5); vb_unlock(a);
    release(a); release(b);
    uint32_t ta = tex_make(32, 32), tb = tex_make(32, 32);
    pa = tex_lock(ta, 0); pat(pa, 32*32*4, 7); tex_unlock(ta);
    int treads = is_pat(pa, 32*32*4, 7);
    pb = tex_lock(tb, 0); pat(pb, 32*32*4, 8);
    if (pa) MR8(pa + 40) = 0x99;
    tex_unlock(tb);
    q = tex_lock(tb, LK_RO); int tb_ok = is_pat(q, 32*32*4, 8); tex_unlock(tb);
    release(ta); release(tb);
    check(reads && pb != pa && b_ok && in_a && treads && tb_ok, "hb-stale-pointer",
          "buffers: A's old pointer reads A %d, B's window elsewhere %d, B kept %d, the write in A %d; textures: reads %d, B kept %d",
          reads, pb != pa, b_ok, in_a, treads, tb_ok);
    if (MW){                                            /* the parked windows are bounded: 1,024 of them, 8 MB */
        enum { NS = 3000, NL = 200 }; static uint32_t v[NS]; unsigned vb1 = gk_count("vb"), most = 0, most2 = 0;
        for (int i = 0; i < NS; i++){ v[i] = vb_make(1024); p = vb_lock(v[i], 0, 0, 0); pat(p, 1024, (uint8_t)i); vb_unlock(v[i]); }
        most = gk_count("vb") - vb1;
        for (int i = 0; i < NS; i++) release(v[i]);
        for (int i = 0; i < NL; i++){ v[i] = vb_make(65536); p = vb_lock(v[i], 0, 0, 0); pat(p, 65536, (uint8_t)i); vb_unlock(v[i]); }
        most2 = gk_count("vb") - vb1;
        unsigned wrong = 0;
        for (int i = 0; i < NL; i++){ q = vb_lock(v[i], 0, 0, LK_RO); if (!is_pat(q, 65536, (uint8_t)i)) wrong++; vb_unlock(v[i]); }
        for (int i = 0; i < NL; i++) release(v[i]);
        check(most <= 1024u && most2 <= 128u && !wrong && gk_count("vb") == vb1, "hb-park-bounds",
              "parked: %u of %u 1 KB windows, %u of %u 64 KB ones; %u read back wrong; %u blocks left", most, NS, most2, NL, wrong,
              gk_count("vb") - vb1);
    }

    /* locked every frame: the data of the last Lock, and a Lock of part points into the same window */
    vb = vb_make(N);
    for (int i = 0; i < 8; i++){ p = vb_lock(vb, 0, 0, 0); pat(p, N, (uint8_t)(20 + i)); vb_unlock(vb); present(); }
    q = vb_lock(vb, 0, 0, LK_RO); ok = is_pat(q, N, 27); vb_unlock(vb);
    p = vb_lock(vb, 64, 32, 0); vb_unlock(vb);
    check(ok && p == q + 64u, "hb-per-frame", "eight per-frame Locks: the last kept %d; Lock(64,32) at the window + 64 %d", ok, p == q + 64u);
    release(vb);

    /* a texture level: a round trip, and the level kept over a Reset */
    t = tex_make(64, 64);
    p = tex_lock(t, 0); pat(p, 64*64*4, 11); tex_unlock(t);
    q = tex_lock(t, LK_RO); ok = is_pat(q, 64*64*4, 11); tex_unlock(t);
    check(p && ok, "hb-tex-roundtrip", "LockRect 0x%08x, read back %d", p, ok);
    vb = vb_make(N); p = vb_lock(vb, 0, 0, 0); pat(p, N, 13); vb_unlock(vb);
    present_params(1280, 720, 1); call(g_dev, "Reset", 1, PP);
    q = vb_lock(vb, 0, 0, LK_RO); ok = is_pat(q, N, 13); vb_unlock(vb);
    uint32_t qt = tex_lock(t, LK_RO); int okt = is_pat(qt, 64*64*4, 11); tex_unlock(t);
    check(ok && okt, "hb-reset", "after a Reset: the buffer %d, the texture %d", ok, okt);
    release(vb); release(t);

    /* 2,000 resources made, written, read and released: nothing left in the buffer heap */
    for (int i = 0; i < 2000; i++){
        vb = vb_make(4096); p = vb_lock(vb, 0, 0, 0); pat(p, 4096, (uint8_t)i); vb_unlock(vb);
        if (i & 1){ vb_lock(vb, 0, 0, LK_RO); vb_unlock(vb); }
        release(vb);
        t = tex_make(64, 64); p = tex_lock(t, 0); pat(p, 64*64*4, (uint8_t)i); tex_unlock(t); release(t);
    }
    check(gk_count("vb") == vb0 && gk_count("tex") == tx0, "hb-no-leak", "vertex buffer blocks %u (%u before), texture blocks %u (%u before)",
          gk_count("vb"), vb0, gk_count("tex"), tx0);

    /* 20,000 live textures, a pseudo-random half released, the rest read back */
    enum { NT = 20000 }; static uint32_t tex[NT]; uint32_t bad = 0, rnd = 12345u;
    for (int i = 0; i < NT; i++){ tex[i] = tex_make(16, 16); p = tex_lock(tex[i], 0); pat(p, 16*16*4, (uint8_t)(i * 13)); tex_unlock(tex[i]); }
    for (int i = 0; i < NT; i++){ rnd = rnd * 1103515245u + 12345u; if ((rnd >> 16) & 1){ release(tex[i]); tex[i] = 0; } }
    for (int i = 0; i < NT; i++) if (tex[i]){ q = tex_lock(tex[i], LK_RO); if (!is_pat(q, 16*16*4, (uint8_t)(i * 13))) bad++; tex_unlock(tex[i]); }
    for (int i = 0; i < NT; i++) if (tex[i]) release(tex[i]);
    check(!bad && gk_count("tex") == tx0, "hb-many", "%u textures read back wrong; texture blocks %u after (%u before)", bad, gk_count("tex"), tx0);
}

int main(void){
    for (uint32_t pg = 0; pg < (0x03000000u >> 14); pg++) g_cbits[pg >> 3] |= (uint8_t)(1u << (pg & 7));
    g_cpu.esp = STACK;
    com_init();
    g_d3d = com_direct3dcreate9();
    present_params(1280, 720, 1);
    uint32_t r = call(g_d3d, "CreateDevice", 6, 0u, 1u, 0u, 0x40u, PP, OUT);
    g_dev = MR32(OUT);
    check(r == 0 && alive(g_dev), "device", "CreateDevice 0x%08x device 0x%08x", r, g_dev);
    call(g_dev, "SetFVF", 1, 0x42u);
    t_lifetimes();
    t_state();
    static const uint32_t strides[3] = {48, 24, 32};
    for (int s = 0; s < 3; s++){
        t_prim(0, 6, 2, strides[s]);                           /* MW's front end: DrawPrimitiveUP fans */
        t_prim(0, 6, 40, strides[s]);
    }
    for (uint32_t pt = 1; pt <= 6; pt++) for (int api = 0; api < 4; api++){
        t_prim(api, pt, 7, 32);
        t_prim(api, pt, 1, 24);
    }
    if (MW){
        t_prim(1, 4, 1500, 32);                                /* past one chunk of the index scratch */
        t_prim(2, 2, 2500, 48);
        t_prim(0, 4, 1400, 32);                                /* 4,200 vertices: DrawPrimitiveUP in chunks */
    }
    t_hostback();
    check(!g_stack_bad, "stdcall", "%d calls left the stack wrong", g_stack_bad);
    printf("d3d9_unit (%s): %d passed, %d failed\n", MW ? "mw" : "ug2", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
