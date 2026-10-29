/* COM bridge: offers the guest COM objects with native implementations.
 * Guest object: [obj+0]=vtable.  vtable entry: 0xE0000000|(iface<<16)|method
 * All methods are stdcall, first argument 'this'.                       */
#include <time.h>
#include <stdatomic.h>
#include "cpu.h"
#include "game.h"
#include "d3d9_tables.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* getenv() SCANS the environment array and takes a lock on every call.  On
 * per-draw paths (for example the "PDLIST" check in
 * SetVertexShaderConstantF was the FIRST term of an && chain) that has a
 * measurable cost: in the profile getenv+__findenv_locked ate 3% of the
 * guest thread.  Environment variables do not change during a run ->
 * resolve once, keep. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })


void mem_commit(uint32_t,uint32_t);
void guest_backtrace(CPU*,int);
void dispatch_indirect(CPU*,uint32_t);
static int g_verbose = -1;
static int VB(void){ if(g_verbose<0) g_verbose = ENVC("VERBOSE")?1:0; return g_verbose; }
/* Metal bridge (metal.m) - inert when absent */
int  metal_available(void);
void metal_set_clear_color(unsigned argb);
void metal_present(void);
long metal_frames(void);
long metal_tris(void);
long metal_frames_dropped(void);
long metal_tex_created(void);
long metal_tex_uploaded(void);
void metal_begin_frame(void);
int  metal_tex_is_depth(unsigned id);       /* a depth texture (8 stages, gpu-abi.md 11.4) */
#include "gfx.h"
#include "vsemu.h"
#ifdef __wasm__
/* The browser renderer takes WGSL instead of MSL (docs/web/gpu-abi.md). */
#include "web/wgsl.h"
#define SHADER_TEXT_CAP (256*1024)
#else
#define SHADER_TEXT_CAP (16384)
#endif
#include "widescreen.h"
#include "enhance.h"

/* COM dispatch compares the method NAME: a run has 144 MILLION com_calls
 * and each one did dozens of strcmps in an if-else chain (in the profile
 * strcmp+_platform_strcmp was 3.6% of the guest thread, com_call itself
 * 9.3%).  The name is already fixed by the (interface, slot) pair; but
 * instead of rewriting the whole chain, make the comparison CHEAP: almost
 * all mismatches are rejected on the first character, so skip the call. */
#define NMEQ(nmv_, lit_) ((nmv_)[0] == (lit_)[0] && !strcmp((nmv_), (lit_)))
unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt);
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt);
void     metal_set_render_target(unsigned texid);
void     metal_clear_target(unsigned argb);
void     metal_set_blend(unsigned key, float alpha_ref);
void     metal_upload_texture(unsigned id_, unsigned level, const void *data,
                              unsigned pitch, unsigned w, unsigned h, unsigned d3dfmt);
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned d3dfmt);
unsigned metal_level_rows(unsigned h, unsigned level, unsigned d3dfmt);
#include "texrep.h"     /* optional texture packs; inert unless configured */
void     metal_fill_mouse(unsigned char *dst, unsigned n);
void     metal_get_cursor(int *x, int *y);
#define NEL(a) ((unsigned)(sizeof(a)/sizeof(*(a))))

#define COM_MAGIC 0xE0000000u
/* v12na: COM objects and Lock buffers come from two internal guest heaps
 * (heap.c) and are freed on the last Release.  NFSU2_HEAP=bump keeps the old
 * bump arenas below (nothing is ever reused there). */
uint32_t gheap_alloc_c(CPU *c, int heap, uint32_t n, int zero);
void     gheap_free_c(CPU *c, int heap, uint32_t p);
int      gheap_bump(void);
int      gheap_check_block(CPU *c, uint32_t p, const char *what);
extern int g_gh_com, g_gh_gbuf;
static CPU *g_com_cpu;                   /* com_call's CPU: call sites for heap stats/checks */
static uint64_t g_com_bytes = 0, g_gbuf_bytes = 0;   /* live, heap mode */
#define COM_BASE  0x30000000u
#define COM_MAX   0x38000000u
static uint32_t com_ptr = COM_BASE;
static uint32_t com_alloc(uint32_t n){
    n=(n+15)&~15u;
    if (!gheap_bump()){ uint32_t p = gheap_alloc_c(g_com_cpu, g_gh_com, n, 1); if (p) g_com_bytes += n; return p; }
    if (com_ptr+n > COM_MAX) return 0;
    uint32_t p=com_ptr; com_ptr+=n; mem_commit(p,n); memset(g_mem+p,0,n); return p;
}
static void com_free(uint32_t p, uint32_t n){
    if (!p || gheap_bump()) return;
    gheap_free_c(g_com_cpu, g_gh_com, p); g_com_bytes -= (n + 15u) & ~15u;
}
/* guest-side general allocator (for Lock etc.) */
#define GBUF_BASE 0x38000000u
#define GBUF_MAX  0x60000000u
static uint32_t gbuf_ptr = GBUF_BASE;
/* Lock buffers get a little slack past the computed size: the bump arena
 * used to commit 1 MB extra after each buffer because some paths wrote a
 * bit beyond it.  Under NFSU2_HEAPCHECK there is no slack, so the tail canary
 * reports such writes (checked at Unlock). */
static uint32_t gbuf_slack(void){
    static int s = -1;
    if (s < 0){ const char *e = getenv("NFSU2_HEAPCHECK"); s = (e && *e && *e != '0') ? 0 : 256; }
    return (uint32_t)s;
}
/* The census of the buffer heap by kind ([mem] lines, heap.c heap_census_line):
 * live bytes and blocks per kind, the kind of each live block in a small
 * host-side table (open addressing, deletion by backward shift). */
enum { GK_OTHER, GK_TEX, GK_TEXDYN, GK_SURF, GK_VB, GK_VBDYN, GK_IB, GK_IBDYN, GK_SND, GK_N };
static const char *const g_gk_name[GK_N] = { "other", "tex", "texdyn", "surf", "vb", "vbdyn", "ib", "ibdyn", "snd" };
static uint64_t g_gk_bytes[GK_N];
static uint32_t g_gk_n[GK_N];
#define GKH_N (1u << 17)
static uint32_t g_gkh_key[GKH_N];
static uint8_t  g_gkh_kind[GKH_N];
static uint32_t g_gkh_size[GKH_N];
static inline uint32_t gkh_slot(uint32_t p){ return (p >> 4) * 2654435761u >> 15; }
static void gk_note(uint32_t p, uint32_t n, int kind){
    if (!p) return;
    uint32_t i = gkh_slot(p) & (GKH_N - 1u);
    for (uint32_t probe = 0; probe < GKH_N; probe++, i = (i + 1u) & (GKH_N - 1u))
        if (!g_gkh_key[i]){ g_gkh_key[i] = p; g_gkh_kind[i] = (uint8_t)kind; g_gkh_size[i] = n;
                            g_gk_bytes[kind] += n; g_gk_n[kind]++; return; }
}
static void gk_forget(uint32_t p){
    if (!p) return;
    uint32_t i = gkh_slot(p) & (GKH_N - 1u);
    for (uint32_t probe = 0; probe < GKH_N && g_gkh_key[i]; probe++, i = (i + 1u) & (GKH_N - 1u)){
        if (g_gkh_key[i] != p) continue;
        g_gk_bytes[g_gkh_kind[i]] -= g_gkh_size[i]; g_gk_n[g_gkh_kind[i]]--;
        g_gkh_key[i] = 0;
        /* backward shift: pull later entries of the run into the hole */
        for (uint32_t j = (i + 1u) & (GKH_N - 1u); g_gkh_key[j]; j = (j + 1u) & (GKH_N - 1u)){
            uint32_t h = gkh_slot(g_gkh_key[j]) & (GKH_N - 1u);
            if (((j - h) & (GKH_N - 1u)) >= ((j - i) & (GKH_N - 1u))){
                g_gkh_key[i] = g_gkh_key[j]; g_gkh_kind[i] = g_gkh_kind[j]; g_gkh_size[i] = g_gkh_size[j];
                g_gkh_key[j] = 0; i = j; }
        }
        return;
    }
}
int com_gbuf_census(char *buf, size_t n){
    int k = snprintf(buf, n, " gbuf:");
    for (int i = 0; i < GK_N && k > 0 && k < (int)n - 32; i++)
        if (g_gk_n[i]) k += snprintf(buf + k, n - (size_t)k, " %s=%.1fMB/%u", g_gk_name[i], g_gk_bytes[i] / 1048576.0, g_gk_n[i]);
    int com_hostback_census(char *, size_t);
    if (k > 0 && k < (int)n - 1) k += com_hostback_census(buf + k, n - (size_t)k);
    return k;
}
static uint32_t gbuf_alloc_kz(uint32_t n, int kind, int zero);
static uint32_t gbuf_alloc_k(uint32_t n, int kind){ return gbuf_alloc_kz(n, kind, 1); }
static uint32_t gbuf_alloc(uint32_t n){ return gbuf_alloc_k(n, GK_OTHER); }
int heap_census_line(char *buf, size_t n);
static uint32_t gbuf_alloc_kz(uint32_t n, int kind, int zero){
    if (!gheap_bump()){
        uint32_t p = gheap_alloc_c(g_com_cpu, g_gh_gbuf, n + gbuf_slack(), zero);
        if (p){ g_gbuf_bytes += n + gbuf_slack(); gk_note(p, n + gbuf_slack(), kind); }
        else { static int sh = 0;                     /* the guest's window is full: say what holds it */
            if (sh++ < 4){ char ml[768]; heap_census_line(ml, sizeof ml);
                fprintf(stderr, "[mem] a %s buffer of %u bytes FAILED (caller 0x%08x):%s\n", g_gk_name[kind], n,
                        g_com_cpu ? g_com_cpu->eip : 0u, ml); } }
        return p; }
    n=(n+63)&~63u;
    if (gbuf_ptr+n > GBUF_MAX) return 0;
    uint32_t p=gbuf_ptr; gbuf_ptr+=n;
    /* Each buffer commits its FULL size + a safety margin.  Some game
     * paths (high texture/LOD settings) write a little more than we
     * compute; what followed was not committed, so it crashed
     * (measured: 0x38098000).  The margin removes this class of crash;
     * mmap is lazy, so no physical memory is spent. */
    mem_commit(p, n + (1u<<20));
    return p;
}
/* guest scratch memory for runtime/dinput_pad.c (callback structs) */
uint32_t com_scratch(uint32_t n){ return gbuf_alloc(n); }
/* runtime/dinput_pad.c: the DirectInput gamepad (device kind 3) */
int      dipad_guid(uint32_t guid);
void     dipad_created(uint32_t dev, uint32_t guid);
int      dipad_enum(CPU *c, uint32_t devtype, uint32_t cb, uint32_t ref, uint32_t flags);
uint32_t dipad_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *args);
/* ... and its force-feedback effects (IDirectInputEffect) */
uint32_t dipad_effect_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *args);
void     dipad_effect_free(uint32_t obj);
/* runtime/dinput_kbd.c: the DirectInput keyboard (device kind 1) */
void     dikbd_created(uint32_t dev);
uint32_t dikbd_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *args);
void     dikbd_report(void);
uint32_t gheap_usize(uint32_t p);
static void gbuf_free(uint32_t p){
    if (!p || gheap_bump()) return;
    uint32_t n = gheap_usize(p);
    gk_forget(p);
    gheap_free_c(g_com_cpu, g_gh_gbuf, p); g_gbuf_bytes -= n;
}

/* ---- host backing (game.h d3d_host_backing) --------------------------------
 * A texture level's or a vertex or index buffer's copy lives in the runtime's
 * memory, outside the guest's window; the guest sees it only between Lock and
 * Unlock, in a window of the buffer heap: D3D9 keeps a managed resource's copy
 * where the game sees it only through Lock.  Dynamic resources (D3DUSAGE_DYNAMIC,
 * locked every frame) stay in the guest's memory as before.
 *
 * An entry is keyed by the guest address of the field that holds the window
 * (a texture's level slot, a buffer's +8), which is 0 while nothing is locked.
 * Lock: a window of the whole buffer or level, as D3D9's managed copy is one
 * mapping (a game may touch bytes outside the range it locked), filled from the
 * copy (zeros if it was never written; also for D3DLOCK_DISCARD, which is valid
 * on dynamic resources only), the pointer at the locked offset; Unlock: the window back into the copy (unless the lock was
 * read-only) and freed.  Locks nest as D3D9 counts them: a Lock before the
 * Unlock points into the same window, which stays until the last Unlock and is
 * written back unless every Lock of the nest was read-only.  Draws read a
 * buffer's copy (buf_src).
 * A resource that is locked again within HB_PIN_FRAMES presents of its last
 * Unlock, HB_PIN_TIMES times in a row, keeps its window from then on (a
 * per-frame lock copies no more than before); its copy goes. */
#define HB_N          (1u << 17)
#define HB_PIN_FRAMES 4u
#define HB_PIN_TIMES  3u
#define BUF_HB        56u                 /* VB / IB: the entry's index, 0 = in the guest's memory */
static uint64_t g_present;                /* Presents so far (below) */
typedef struct {
    uint32_t key, size;                   /* the window's field; the copy's size */
    uint8_t *host;                        /* the copy (NULL: never written, reads as zeros) */
    uint32_t wlen, wflags;                /* the window's length (the whole buffer or level) and the lock's flags */
    uint32_t locks;                       /* Locks not matched by an Unlock yet */
    uint32_t last_unlock, near;           /* the Present count at the last Unlock; Locks soon after one, in a row */
    uint8_t kind, pinned;                 /* its census kind (GK_*); 1: the window stays, no copy */
} HBack;
static HBack   *g_hb;                     /* entries; index 0 unused */
static uint32_t g_hb_free_head, g_hb_top = 1;
static uint32_t *g_hb_map;                /* key -> index, open addressing */
static uint64_t g_hb_bytes, g_hb_locks, g_hb_relocks, g_hb_copied, g_hb_copyin, g_hb_pins, g_hb_full, g_hb_nested;
static int hb_on(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("NFSU2_HOSTBACK");   /* NFSU2_HOSTBACK=0: the A/B escape hatch */
                v = g_game.d3d_host_backing && !gheap_bump() && !(e && *e == '0'); }
    return v;
}
static inline uint32_t hb_slot(uint32_t key){ return (key >> 2) * 2654435761u >> 15; }
static uint32_t hb_find(uint32_t key){
    if (!g_hb_map || !key) return 0;
    for (uint32_t i = hb_slot(key) & (HB_N - 1u), q = 0; q < HB_N && g_hb_map[i]; q++, i = (i + 1u) & (HB_N - 1u))
        if (g_hb[g_hb_map[i]].key == key) return g_hb_map[i];
    return 0;
}
static uint32_t hb_make(uint32_t key, uint32_t size, int kind){
    if (!g_hb){ g_hb = calloc(HB_N, sizeof *g_hb); g_hb_map = calloc(HB_N, sizeof *g_hb_map);
                if (!g_hb || !g_hb_map){ free(g_hb); free(g_hb_map); g_hb = 0; g_hb_map = 0; } }
    if (!g_hb) return 0;
    uint32_t x = hb_find(key);
    if (x) return x;
    if (g_hb_free_head){ x = g_hb_free_head; g_hb_free_head = g_hb[x].key; }
    else if (g_hb_top < HB_N * 3u / 4u) x = g_hb_top++;     /* the map stays at most 3/4 full */
    else { g_hb_full++; return 0; }
    g_hb[x] = (HBack){ .key = key, .size = size, .kind = (uint8_t)kind };
    uint32_t i = hb_slot(key) & (HB_N - 1u);
    while (g_hb_map[i]) i = (i + 1u) & (HB_N - 1u);
    g_hb_map[i] = x;
    return x;
}
/* the entry's copy goes (its window, if any, is the caller's: the field still names it) */
static void hb_drop(uint32_t key){
    if (!g_hb_map || !key) return;
    uint32_t i = hb_slot(key) & (HB_N - 1u);
    for (uint32_t q = 0; q < HB_N && g_hb_map[i]; q++, i = (i + 1u) & (HB_N - 1u)){
        uint32_t x = g_hb_map[i];
        if (g_hb[x].key != key) continue;
        if (g_hb[x].host){ free(g_hb[x].host); g_hb_bytes -= g_hb[x].size; }
        g_hb[x] = (HBack){0}; g_hb[x].key = g_hb_free_head; g_hb_free_head = x;
        g_hb_map[i] = 0;
        for (uint32_t j = (i + 1u) & (HB_N - 1u); g_hb_map[j]; j = (j + 1u) & (HB_N - 1u)){
            uint32_t h = hb_slot(g_hb[g_hb_map[j]].key) & (HB_N - 1u);
            if (((j - h) & (HB_N - 1u)) >= ((j - i) & (HB_N - 1u))){ g_hb_map[i] = g_hb_map[j]; g_hb_map[j] = 0; i = j; }
        }
        return;
    }
}
/* the copy at least size bytes (zeros past what it held) */
static uint8_t *hb_host(HBack *e, uint32_t size){
    if (size < e->size) size = e->size;
    if (e->host && size == e->size) return e->host;
    uint8_t *p = realloc(e->host, size);
    if (!p) return 0;
    if (!e->host) memset(p, 0, size);
    else if (size > e->size) memset(p + e->size, 0, size - e->size);
    g_hb_bytes += size - (e->host ? e->size : 0u);
    e->host = p; e->size = size;
    return p;
}
/* Lock: the window of entry x, the whole copy (at least len bytes; + slack) in the buffer heap.
 * Pinned: the window stays from then on and the copy goes. */
static uint32_t hb_lock(uint32_t x, uint32_t len, uint32_t flags){
    HBack *e = &g_hb[x];
    /* locked again a few presents after its Unlock (not within one frame: a load fills a buffer in parts) */
    uint32_t d = (uint32_t)g_present - e->last_unlock;
    if (!e->last_unlock || d >= HB_PIN_FRAMES) e->near = 0; else if (d) e->near++;
    int pin = e->near >= HB_PIN_TIMES;
    if (len < e->size) len = e->size;
    uint32_t w = gbuf_alloc_kz(len, e->kind, !e->host);
    if (!w) return 0;
    /* D3DLOCK_DISCARD and NOOVERWRITE are valid on dynamic resources only, and these are static: a
     * DISCARD Lock still shows the old bytes, as the managed copy does, so bytes it leaves unwritten stay */
    if (e->host){
        uint32_t n = e->size < len ? e->size : len;
        memcpy(g_mem + w, e->host, n);
        if (n < len) memset(g_mem + w + n, 0, len - n);
        g_hb_copied += n; g_hb_copyin += n;
    }
    g_hb_locks++; if (e->host) g_hb_relocks++;
    e->wlen = len; e->wflags = flags; e->locks = 1;
    if (pin){ e->pinned = 1; g_hb_pins++;
        if (e->host){ free(e->host); g_hb_bytes -= e->size; e->host = 0; } }
    return w;
}
/* A Lock before the Unlock: the same window, one more Unlock to come; a writing Lock makes the nest write back */
static void hb_nest(uint32_t x, uint32_t flags){
    HBack *e = &g_hb[x];
    e->locks++; g_hb_nested++;
    if (!(flags & 0x10u)) e->wflags &= ~0x10u;
}
/* Unlock: at the last of a nest, the window at w back into the copy and freed (1: the field is cleared;
 * not a pinned entry's) */
static int hb_unlock(uint32_t x, uint32_t w){
    HBack *e = &g_hb[x];
    if (e->pinned || !w) return 0;
    if (e->locks > 1){ e->locks--; return 0; }
    e->locks = 0;
    if (!(e->wflags & 0x10u)){                   /* D3DLOCK_READONLY: nothing written */
        uint8_t *h = hb_host(e, e->wlen);
        if (h){ memcpy(h, g_mem + w, e->wlen); g_hb_copied += e->wlen; }
    }
    e->last_unlock = (uint32_t)g_present; if (!e->last_unlock) e->last_unlock = 1;
    gbuf_free(w);
    return 1;
}
/* A draw's vertex or index data: a buffer's host copy (p, len bytes from p) or
 * guest memory at ga (0: none), whose pages are checked as it is read. */
typedef struct { const uint8_t *p; uint32_t ga, len; } BSrc;
static BSrc buf_src(uint32_t o, uint32_t off){
    uint32_t x = MR32(o + BUF_HB);
    if (x && x < HB_N && g_hb && !g_hb[x].pinned){
        HBack *e = &g_hb[x];
        uint32_t n = MR32(o + 4u);                        /* the buffer's length */
        if (!e->host && !hb_host(e, n)) return (BSrc){ 0, 0u, 0u };
        if (off >= n || off >= e->size) return (BSrc){ 0, 0u, 0u };
        return (BSrc){ e->host + off, 0u, n - off };
    }
    uint32_t a = MR32(o + 8u);
    return (BSrc){ 0, a ? a + off : 0u, 0u };
}
static inline BSrc guest_src(uint32_t a){ return (BSrc){ 0, a, 0u }; }
/* n bytes at off, or NULL (past the copy, or guest pages not committed) */
static inline const uint8_t *bsrc_at(BSrc s, uint32_t off, uint32_t n){
    if (s.p) return ((uint64_t)off + n <= s.len) ? s.p + off : NULL;
    if (!s.ga) return NULL;
    uint32_t a = s.ga + off;
    return (mem_is_committed(a) && mem_is_committed(a + n - 1u)) ? g_mem + a : NULL;
}
static inline uint32_t bsrc_index(const uint8_t *p, uint32_t isz){
    if (isz == 2u){ uint16_t v; memcpy(&v, p, 2); return v; }
    uint32_t v; memcpy(&v, p, 4); return v;
}
/* [mem] lines: the copies outside the window, and the Lock traffic since the last line */
int com_hostback_census(char *buf, size_t n){
    static uint64_t locks0, relocks0, copied0, copyin0;
    uint32_t live = g_hb_top - 1u;
    for (uint32_t x = g_hb_free_head; x && live; x = g_hb[x].key) live--;
    int k = snprintf(buf, n, " host: %.1fMB in %u entries, %llu locks (%llu again) %.1fMB copied (%.1fMB in), %llu pinned,"
                             " %llu nested%s",
                     g_hb_bytes / 1048576.0, live, (unsigned long long)(g_hb_locks - locks0),
                     (unsigned long long)(g_hb_relocks - relocks0), (g_hb_copied - copied0) / 1048576.0,
                     (g_hb_copyin - copyin0) / 1048576.0, (unsigned long long)g_hb_pins,
                     (unsigned long long)g_hb_nested, g_hb_full ? " FULL" : "");
    locks0 = g_hb_locks; relocks0 = g_hb_relocks; copied0 = g_hb_copied; copyin0 = g_hb_copyin;
    return k;
}

typedef struct { const char *name; const Method *m; unsigned n; } Iface;
static const Iface g_if[] = {
    {"?",              0,         0},
    {"IDirect3D9",       M_D3D9,    NEL(M_D3D9)},
    {"IDirect3DDevice9", M_DEVICE,  NEL(M_DEVICE)},
    {"IDirect3DTexture9",M_TEXTURE, NEL(M_TEXTURE)},
    {"IDirect3DVertexBuffer9", M_VBUF, NEL(M_VBUF)},
    {"IDirect3DIndexBuffer9",  M_IBUF, NEL(M_IBUF)},
    {"IDirect3DSurface9",      M_SURFACE, NEL(M_SURFACE)},
    {"IDirect3DSimple",        M_SIMPLE,  NEL(M_SIMPLE)},
    {"IDirect3DVertexShader9",   M_VSHADER,    NEL(M_VSHADER)},
    {"IDirect3DVertexDeclaration9", M_VDECL,   NEL(M_VDECL)},
    {"IDirect3DStateBlock9",     M_STATEBLOCK, NEL(M_STATEBLOCK)},
    {"IDirect3DSwapChain9",      M_SWAPCHAIN,  NEL(M_SWAPCHAIN)},
    {"IDirect3DQuery9",          M_QUERY,      NEL(M_QUERY)},
    {"IDirectInput8",            M_DI8,        NEL(M_DI8)},
    {"IDirectInputDevice8",      M_DIDEV,      NEL(M_DIDEV)},
    {"IDirectSound",             M_DS,         NEL(M_DS)},
    {"IDirectSoundBuffer",       M_DSBUF,      NEL(M_DSBUF)},
    {"IDirectInputEffect",       M_DIEFF,      NEL(M_DIEFF)},
};
enum { IF_D3D9=1, IF_DEV, IF_TEX, IF_VB, IF_IB, IF_SURF, IF_SIMPLE,
       IF_VSHADER, IF_VDECL, IF_STATEBLOCK, IF_SWAPCHAIN, IF_QUERY,
       IF_DI8, IF_DIDEV, IF_DS, IF_DSBUF, IF_DIEFF };

static uint64_t g_com_new_n[NEL(g_if)];   /* per interface: objects created */
static uint64_t g_com_del_n[NEL(g_if)];   /* ... and destroyed (last Release) */
/* --- COM object lifetime (v12na) --------------------------------------------
 * Every object carries a 16-byte header in front of it, in the same block:
 * reference count, flags, parent (surfaces of a texture), interface.  The
 * vtable is shared per interface, so "obj+0 == that vtable" identifies one
 * of our objects.  Semantics follow D3D9: Create* and Get* hand out a
 * reference, the device holds one on every bound resource (SetTexture,
 * SetStreamSource, SetIndices, SetRenderTarget, SetDepthStencilSurface) and
 * state blocks on the resources they record; a texture's surfaces share the
 * texture's count; device-owned objects (back buffer, implicit depth
 * surface, device, swap chain) are never destroyed.  On the last Release
 * the object, its Lock buffers and its Metal texture are freed (metal.m
 * delays the Metal side until no recorded frame can use it).
 * NFSU2_COMFREE=0 (escape hatch): counts are kept but nothing is destroyed. */
#define CH_REF(o)    MR32((o) - 16u)
#define CH_FLAGS(o)  MR32((o) - 12u)
#define CH_PARENT(o) MR32((o) - 8u)
#define CH_IFACE(o)  MR32((o) - 4u)
#define CF_CUBE   1u      /* cube texture */
#define CF_DEVICE 2u      /* owned by the device: Release never destroys it */
#define CF_CHILD  4u      /* surface of a texture: AddRef/Release go to the parent */
#define CF_DEAD   8u
#define CF_VOLUME 16u     /* volume texture (game.h d3d9_strict only: GetType, GetLevelDesc) */
static uint32_t g_vtab[NEL(g_if)];
static uint64_t g_com_live = 0, g_com_overrel = 0, g_com_bad = 0;
int com_frees(void){
    static int v = -1;
    if (v < 0){ const char *e = getenv("NFSU2_COMFREE"); v = !(e && *e == '0'); }
    return v;
}
static uint32_t com_new(uint32_t iface, uint32_t extra){
    unsigned n = g_if[iface].n;
    g_com_new_n[iface]++;
    uint32_t vt = g_vtab[iface];
    if (!vt){
        vt = com_alloc(n*4u);
        for (unsigned i=0;i<n;i++) MR32(vt+4u*i) = COM_MAGIC | (iface<<16) | i;
        g_vtab[iface] = vt;
    }
    uint32_t blk = com_alloc(16u+4u+extra);
    if (!blk) return 0;
    uint32_t obj = blk + 16u;
    CH_REF(obj) = 1; CH_FLAGS(obj) = 0; CH_PARENT(obj) = 0; CH_IFACE(obj) = iface | (extra << 16);
    g_com_live++;
    if (ENVC("NFSU2_COMLOG"))                         /* test-only: lifetime trace */
        fprintf(stderr, "[com] create %s 0x%08x (caller 0x%08x)\n", g_if[iface].name, obj, g_com_cpu ? g_com_cpu->eip : 0u);
    /* ZERO the extra area.  Object fields (size, format, texture id,
     * cube face index...) are always read assuming "0 = not set";
     * if com_alloc does not return fresh memory, garbage is read. */
    for (uint32_t q = 0; q < 4u + extra; q += 4u) MR32(obj + q) = 0u;
    MR32(obj) = vt;
    return obj;
}

/* dinput_pad.c CreateEffect: a new IDirectInputEffect (16 bytes of its own) */
uint32_t com_new_dieffect(void){ return com_new(IF_DIEFF, 16u); }

#define PSCUBE 256   /* must MATCH metal.m: cube ps variant */
uint64_t g_com_calls=0, g_com_unimpl=0;
/* per-method call counter: the work list for the render layer */
#define MSTAT_IF 20
#define MSTAT_M  128
static uint64_t g_mstat[MSTAT_IF][MSTAT_M];
static uint64_t g_present=0, g_draw=0;
static uint32_t g_d3d9=0, g_dev=0, g_cur_decl=0, g_cur_fvf=0, g_cur_tex=0;

/* ---------------- vertex shader (interpreted on the CPU) ----------------
 * NFSU2 does all its drawing with DrawPrimitiveUP and the positions are RAW
 * (POSITION, not POSITIONT): placing them correctly on screen requires
 * running the vertex shader.  See runtime/vsemu.c                     */
#define MAXVS 1024
static VSProg  *g_vsprog[MAXVS];
static unsigned g_vsprogn = 1;          /* 0 = "no shader" */
void mprot_arm(uint32_t);
/* CoreAudio output (runtime/audio.m) */
int      audio_register(uint32_t data, uint32_t bytes, uint32_t rate,
                        uint32_t chans, uint32_t bits);
void     audio_play(int id, int loop);
void     audio_stop(int id);
uint32_t audio_cursor(int id);
void     audio_set_volume(int id, long millibel);
void     audio_init(void);
void     audio_report(void);
void cov_reset(void);
static VSConst  g_vsk;                  /* c#/i#/b# constant file */
static unsigned g_cur_vs = 0;
static uint64_t g_vs_draws = 0, g_ff_draws = 0;
static uint64_t g_v_in = 0, g_v_out = 0, g_v_badw = 0;
static uint64_t g_tex_fail = 0;
static uint64_t g_dikey_calls=0, g_dikey_hits=0;
static uint64_t g_mat_ok=0, g_mat_bad=0;
static uint64_t g_di_enum=0, g_di_objs=0;
/* --- vertex/index buffer draw path (3D models) ------------------
 * The game draws 3D geometry with DrawIndexedPrimitive; the UI with
 * DrawPrimitiveUP.  While this path was not implemented ALL 3D model
 * geometry (including the car in the menu) was silently dropped.   */
static uint32_t g_ss_vb = 0, g_ss_off = 0, g_ss_stride = 0;
static uint32_t g_ib_obj = 0;
static uint32_t g_idx_scratch = 0;
static uint64_t g_dip_draws = 0, g_dip_skip = 0, g_dip_tris = 0;
static uint32_t *g_lk_tid=0; static unsigned *g_lk_cnt=0; static uint64_t *g_lk_byt=0;
static uint32_t *g_lock_obj=0; static unsigned *g_lock_cnt=0; static int g_lock_n=0;
static uint64_t g_snd_bytes=0, g_snd_nonzero=0, g_snd_zero=0, g_snd_play=0;
static uint32_t g_cur_rt = 0;
/* D3DRENDERSTATETYPE values; the defaults are D3D9's initial state */
/* Resolutions offered in the game's settings menu (desktop last). */
/* The game's OWN internal resolution table.  Measured experimentally:
 * g_RacingResolution in the registry is an index into this table; 6 and
 * above OVERRUN the game's own array (CreateDevice received garbage like
 * 8126268x268571776).  Earlier 12 modes were advertised here, so the
 * settings menu showed more than the 6 rows the game supports and picking
 * an extra row caused an array overrun -> crash.  Vanilla NFSU2 supports
 * only 4:3/5:4; there is no widescreen. */
/* Why we do not extend the table: ADVERTISING a 7th mode overruns the
 * game's own array and produces a garbage size (8126268x268571776 every
 * time); the game keeps it and creates its render targets at that size.
 * Patching the table in guest memory was tried too; it could not be found
 * as a contiguous {w,h} array.  So the row count stays at 6; only the
 * LABELS and the real device size are ours. */
/* --- Resolution rows in the settings menu ---------------------------
 * The game's internal table (g_RacingResolution 0..5, selected row
 * [0x870d1c]) is fixed and all 4:3.  The menu text comes from READY-MADE
 * strings in .rdata (sub_0049b690: row -> "640x480".."1600x1200"; display
 * only, the size is not read from this text).  The EnumAdapterModes list
 * only decides which rows CAN BE SELECTED: the game looks up the table
 * value in the list.  Measured (2026-09-27, the user's display): with the
 * list replaced by the labels the arrows could not reach any row; before
 * that, because the desktop mode was reported as 1920x1080, the 1600x1200
 * row (the so-called "1080p") could never be selected.
 *
 * Layout: list = the game's table (all 6 rows selectable), desktop
 * mode >= 1600x1200, the menu strings are rewritten in memory and the
 * device is set up at the row's REAL resolution (aspect difference Hor+,
 * res_pick).  Text slots are 8/8/12/12/12/12 bytes: short labels for the
 * first two rows.
 * NOWIDE=1: the game's original table and strings. */
static const uint32_t g_tbl[6][2] = {
    {640,480},{800,600},{1024,768},{1280,960},{1280,1024},{1600,1200},
};
static uint32_t g_modes[6][2] = {
    {1280,720},{1920,1080},{2560,1440},{0,0},{3840,2160},{1600,1200},
};
#define ROW_NATIVE 3
/* the label slots are the game's (game.h res_labels; UG2's are 8/8/12/12/12/12 bytes) */
static void label_put(int row, const char *txt){
    if (!g_game.res_labels) return;
    uint32_t a = g_game.res_labels[row].va, cap = g_game.res_labels[row].cap, n = (uint32_t)strlen(txt);
    if (n + 1 > cap || !mem_is_committed(a) || !mem_is_committed(a + cap - 1)) return;
    /* is the expected original text in place (do not write into another EXE version) */
    uint8_t c0 = MR8(a);
    if (!(c0 >= '0' && c0 <= '9')) return;
    memset(g_mem + a, 0, cap); memcpy(g_mem + a, txt, n);
}
static int g_cur_row = -1;             /* the game's active row (-1 = unknown) */
static int g_row4_on = 1;              /* is the 4K row selectable (see modes_refresh) */
static int modemap_on(void){ return !ENVC("NOWIDE"); }
/* The rows are derived from the VIEW's (window / full screen) aspect:
 * "1080p" = 1080 lines at the view's aspect.  So the drawable fills the
 * window without bars, and the resolution only changes the quality.  The
 * "Screen" row is the view's own pixel count (1:1, 9 MP upper limit);
 * 1600x1200 stays 4:3 on purpose (the original look, bars at the sides). */
static void modes_refresh(void){
    unsigned vw = 0, vh = 0;
    void metal_view_size(unsigned*, unsigned*); metal_view_size(&vw, &vh);
    if (!vw || !vh){ vw = 2560; vh = 1600; }
    static const unsigned hts[6] = {720, 1080, 1440, 0, 2160, 0};
    for (int i = 0; i < 6; i++){
        if (!hts[i]) continue;
        unsigned hh = hts[i], ww = (unsigned)((double)hh * vw / vh + 0.5) & ~1u;
        g_modes[i][0] = ww; g_modes[i][1] = hh;
    }
    /* NFSU2_RENDER (widescreen.c): backing pixels, panel pixels or a scale.
     * NFSMAXPIX bounds the size at a scale of 1 and below; a scale above 1 is
     * chosen to go beyond it and multiplies the bounded size (bounded after
     * the scale, 150% and 200% both came out at ~104% on a 4K screen), up to
     * 8192 a side: WebGPU's default texture limit (gpu/index.ts asks for no
     * more). */
    double rf = ws_render_factor(), up = rf > 1.0 ? rf : 1.0;
    double dw = vw * rf / up, dh = vh * rf / up;
    double maxpix = ws_max_pixels();
    if (maxpix > 1.0e5 && dw * dh > maxpix){ double k = sqrt(maxpix / (dw * dh)); dw *= k; dh *= k; }
    dw *= up; dh *= up;
    if (dw > 8192.0 || dh > 8192.0){ double k = 8192.0 / (dw > dh ? dw : dh); dw *= k; dh *= k; }
    unsigned w = (unsigned)dw & ~1u, h = (unsigned)dh & ~1u;
    g_modes[ROW_NATIVE][0] = w; g_modes[ROW_NATIVE][1] = h;
    /* The 4K row only if the display can show it (screen row >= 2160).
     * On a smaller display (MacBook ~3K) 4K was shown downscaled: ~1.6x
     * the pixels, no gain on screen, the laptop heats up (user request
     * 2026-10-14).  The row cannot be removed (the table has 6 rows); it is
     * reported to the game as NOT SELECTABLE: EnumAdapterModes does not
     * return 1280x1024 in its 5th entry.  Read from the game code:
     * sub_005be5a0 marks availability ([0x86e880+4*row]) by looking up the
     * table value in the list, sub_0049b600 skips an unavailable row with
     * the arrows, sub_005be690 drops to the next available row below when
     * the saved row is unavailable (4K -> screen).  If this row is reached
     * anyway, the screen resolution and its label are used. */
    g_row4_on = 2160u <= h;
    if (!g_row4_on){ g_modes[4][0] = w; g_modes[4][1] = h; }
    if (!modemap_on()) return;
    static const char *fixed[6] = {"720p","1080p","1440p",0,"4K","1600x1200"};
    char nb[16];
    for (int i = 0; i < 6; i++){
        if (fixed[i] && (i != 4 || g_row4_on)) label_put(i, fixed[i]);
        else { snprintf(nb, sizeof nb, "%ux%u", w, h); label_put(i, nb); }
    }
}
static const uint32_t *mode_label(unsigned i){
    if (!modemap_on()) return g_tbl[i];
    if (i == ROW_NATIVE && !g_modes[i][0]) modes_refresh();
    return g_modes[i];
}
static int tbl_row(uint32_t w, uint32_t h){
    for (int i = 0; i < 6; i++) if (g_tbl[i][0] == w && g_tbl[i][1] == h) return i;
    return -1;
}
/* the rows of a game's own mode list (game.h d3d_modes), up to its {0,0} row */
static uint32_t game_mode_count(void){
    uint32_t n = 0;
    while (g_game.d3d_modes && n < 64u && g_game.d3d_modes[n][0]) n++;
    return n;
}
/* the formats the renderer makes textures and targets in: its own format table, which
 * metal_level_pitch reads (0 for a format it has none for), as metal_create_texture_ex does */
static int fmt_makes(uint32_t f){ return metal_level_pitch(1u, 0u, f) != 0u; }
/* D3D9's depth-stencil formats (D16_LOCKABLE, D32, D15S1, D24S8, D24X8, D24X4S4, D16, D32F_LOCKABLE, D24FS8) */
static int fmt_is_depth(uint32_t f){
    return f == 70u || f == 71u || f == 73u || f == 75u || f == 77u || f == 79u || f == 80u || f == 82u || f == 83u;
}

uint32_t g_rs[256] = {0};        /* also read by d3dx_hle.c's in-line no-op check */
/* RSLOG=1: measure WHICH render states the game sets.  Instead of applying
 * a state we do not handle on a blind guess, first confirm it is used. */
static uint32_t g_rs_hits[256] = {0};
static uint32_t g_rs_vals[256][4];        /* first 4 distinct values */
static uint8_t  g_rs_nval[256] = {0};

/* ---- D3D9 state blocks ------------------------------------------------
 * The game records state with BeginStateBlock/EndStateBlock, then saves
 * and restores it around effect passes with Capture/Apply (in the trace
 * 367 recordings, 418 Captures, 326 Applies).  These used to come back as
 * EMPTY objects, so Apply restored nothing: blend mode, alpha test and
 * texture stage operations leaked over from the previous pass -> overly
 * bright light streaks and wrongly dark surfaces. */
#define MAXSB     512
#define MAXSBREC  128
typedef struct { uint32_t magic; uint32_t a[8]; int nargs; } SBRec;
typedef struct { SBRec r[MAXSBREC]; int n; int used; } SBlock;
static SBlock  g_sb[MAXSB];
static int     g_sbn = 1;          /* 0 = invalid */
int     g_sb_rec = 0;       /* block being recorded, 0 = not recording (read by d3dx_hle.c) */
int     g_sb_replay = 0;
static uint64_t g_sb_applied = 0, g_sb_captured = 0, g_sb_dropped = 0, g_sb_full = 0;

/* Raw state shadows: Capture reads from these (the values the game SET). */
static uint32_t g_raw_vs = 0, g_raw_ps = 0, g_raw_decl = 0, g_raw_fvf = 0;
static uint32_t g_raw_tex[8] = {0};
/* The D3D DEFAULT of D3DTSS_TEXCOORDINDEX is the stage number, NOT 0.
 * With the table starting at zero, stages 1 and 2 -- the headlight texture
 * and the falloff ramp -- were sampled with the road's own (repeating)
 * diffuse UV; the headlight cone was copied all along the road and the
 * whole surface glowed in blotches.  The game never writes this state
 * because the default should already be right. */
uint32_t g_tss_raw[8][40] = {
    [0] = { [11] = 0u }, [1] = { [11] = 1u }, [2] = { [11] = 2u },
    [3] = { [11] = 3u }, [4] = { [11] = 4u }, [5] = { [11] = 5u },
    [6] = { [11] = 6u }, [7] = { [11] = 7u },
};
uint32_t g_smp_raw[16][20];

/* Is this method recorded into a state block? */
/* --- method IDENTITY ----------------------------------------------------
 * Dispatch used to compare the method NAME.  NMEQ's first-letter filter
 * did not help: almost all D3D9 device methods start with
 * "Set"/"Get"/"Draw", so every step of the chain still ran a FULL
 * strcmp (measured: 11% of the guest thread was strcmp, 2335 samples of
 * it from com_call).  The identity is now resolved ONCE per method
 * ((iface,slot) -> id cache), and the chain is all integer
 * comparisons.  A run has 377 million com_calls. */
enum {
    MID__none = 0,
    MID_SetRenderState,
    MID_SetTextureStageState,
    MID_SetSamplerState,
    MID_SetTexture,
    MID_SetVertexShader,
    MID_SetPixelShader,
    MID_SetVertexDeclaration,
    MID_SetFVF,
    MID_SetVertexShaderConstantF,
    MID_SetPixelShaderConstantF,
    MID_SetVertexShaderConstantI,
    MID_SetVertexShaderConstantB,
    MID_SetTransform,
    MID_SetMaterial,
    MID_SetLight,
    MID_LightEnable,
    MID_SetStreamSource,
    MID_SetIndices,
    MID_SetViewport,
    MID_SetScissorRect,
    MID_SetNPatchMode,
    MID_SetClipPlane,
    MID_GetSurfaceLevel,
    MID_GetCubeMapSurface,
    MID_LockRect,
    MID_UnlockRect,
    MID_AddDirtyRect,
    MID_QueryInterface,
    MID_AddRef,
    MID_Release,
    MID_GetAdapterCount,
    MID_GetAdapterModeCount,
    MID_EnumAdapterModes,
    MID_GetAdapterDisplayMode,
    MID_GetAdapterIdentifier,
    MID_GetDeviceCaps,
    MID_CreateDevice,
    MID_TestCooperativeLevel,
    MID_GetAvailableTextureMem,
    MID_GetDirect3D,
    MID_GetDisplayMode,
    MID_Clear,
    MID_BeginScene,
    MID_EndScene,
    MID_Present,
    MID_StretchRect,
    MID_SetRenderTarget,
    MID_Reset,
    MID_DrawIndexedPrimitive,
    MID_DrawPrimitiveUP,
    MID_CreateTexture,
    MID_CreateCubeTexture,
    MID_CreateVolumeTexture,
    MID_CreateVertexBuffer,
    MID_CreateIndexBuffer,
    MID_CreateRenderTarget,
    MID_CreateDepthStencilSurface,
    MID_CreateOffscreenPlainSurface,
    MID_GetBackBuffer,
    MID_GetRenderTarget,
    MID_GetDepthStencilSurface,
    MID_SetDepthStencilSurface,
    MID_CreateVertexDeclaration,
    MID_CreateVertexShader,
    MID_CreatePixelShader,
    MID_BeginStateBlock,
    MID_CreateStateBlock,
    MID_CreateQuery,
    MID_EndStateBlock,
    MID_CreateAdditionalSwapChain,
    MID_GetSwapChain,
    MID_GetNumberOfSwapChains,
    MID_GetTransform,
    MID_GetMaterial,
    MID_GetLight,
    MID_GetViewport,
    MID_GetScissorRect,
    MID_GetClipStatus,
    MID_GetCreationParameters,
    MID_GetRasterStatus,
    MID_GetGammaRamp,
    MID_GetClipPlane,
    MID_GetDevice,
    MID_Lock,
    MID_GetLevelCount,
    MID_GetType,
    MID_GetDesc,
    MID_GetLevelDesc,
    MID_CreateSoundBuffer,
    MID_GetCaps,
    MID_SetCooperativeLevel,
    MID_GetSpeakerConfig,
    MID_DuplicateSoundBuffer,
    MID_Unlock,
    MID_Play,
    MID_Stop,
    MID_SetVolume,
    MID_SetFrequency,
    MID_GetStatus,
    MID_GetCurrentPosition,
    MID_GetFormat,
    MID_EnumDevices,
    MID_EnumDevicesBySemantics,
    MID_GetDeviceStatus,
    MID_Initialize,
    MID_FindDevice,
    MID_EnumObjects,
    MID_GetObjectInfo,
    MID_GetDeviceInfo,
    MID_SetDataFormat,
    MID_Acquire,
    MID_Unacquire,
    MID_Poll,
    MID_GetDeviceState,
    MID_BuildActionMap,
    MID_SetActionMap,
    MID_GetDeviceData,
    MID_GetCapabilities,
    MID_GetProperty,
    MID_SetProperty,
    MID_SetEventNotification,
    MID_GetDataSize,
    MID_GetData,
    MID_Apply,
    MID_Capture,
    MID_GetFunction,
    MID_GetDeclaration,
    MID_CheckDeviceMultiSampleType,
    MID_DrawPrimitive,
    MID_DrawIndexedPrimitiveUP,
    MID_CheckDeviceFormat,
    MID_CheckDepthStencilMatch,
    MID__count
};
static const char *const g_midname[MID__count] = {
    "",
    "SetRenderState",
    "SetTextureStageState",
    "SetSamplerState",
    "SetTexture",
    "SetVertexShader",
    "SetPixelShader",
    "SetVertexDeclaration",
    "SetFVF",
    "SetVertexShaderConstantF",
    "SetPixelShaderConstantF",
    "SetVertexShaderConstantI",
    "SetVertexShaderConstantB",
    "SetTransform",
    "SetMaterial",
    "SetLight",
    "LightEnable",
    "SetStreamSource",
    "SetIndices",
    "SetViewport",
    "SetScissorRect",
    "SetNPatchMode",
    "SetClipPlane",
    "GetSurfaceLevel",
    "GetCubeMapSurface",
    "LockRect",
    "UnlockRect",
    "AddDirtyRect",
    "QueryInterface",
    "AddRef",
    "Release",
    "GetAdapterCount",
    "GetAdapterModeCount",
    "EnumAdapterModes",
    "GetAdapterDisplayMode",
    "GetAdapterIdentifier",
    "GetDeviceCaps",
    "CreateDevice",
    "TestCooperativeLevel",
    "GetAvailableTextureMem",
    "GetDirect3D",
    "GetDisplayMode",
    "Clear",
    "BeginScene",
    "EndScene",
    "Present",
    "StretchRect",
    "SetRenderTarget",
    "Reset",
    "DrawIndexedPrimitive",
    "DrawPrimitiveUP",
    "CreateTexture",
    "CreateCubeTexture",
    "CreateVolumeTexture",
    "CreateVertexBuffer",
    "CreateIndexBuffer",
    "CreateRenderTarget",
    "CreateDepthStencilSurface",
    "CreateOffscreenPlainSurface",
    "GetBackBuffer",
    "GetRenderTarget",
    "GetDepthStencilSurface",
    "SetDepthStencilSurface",
    "CreateVertexDeclaration",
    "CreateVertexShader",
    "CreatePixelShader",
    "BeginStateBlock",
    "CreateStateBlock",
    "CreateQuery",
    "EndStateBlock",
    "CreateAdditionalSwapChain",
    "GetSwapChain",
    "GetNumberOfSwapChains",
    "GetTransform",
    "GetMaterial",
    "GetLight",
    "GetViewport",
    "GetScissorRect",
    "GetClipStatus",
    "GetCreationParameters",
    "GetRasterStatus",
    "GetGammaRamp",
    "GetClipPlane",
    "GetDevice",
    "Lock",
    "GetLevelCount",
    "GetType",
    "GetDesc",
    "GetLevelDesc",
    "CreateSoundBuffer",
    "GetCaps",
    "SetCooperativeLevel",
    "GetSpeakerConfig",
    "DuplicateSoundBuffer",
    "Unlock",
    "Play",
    "Stop",
    "SetVolume",
    "SetFrequency",
    "GetStatus",
    "GetCurrentPosition",
    "GetFormat",
    "EnumDevices",
    "EnumDevicesBySemantics",
    "GetDeviceStatus",
    "Initialize",
    "FindDevice",
    "EnumObjects",
    "GetObjectInfo",
    "GetDeviceInfo",
    "SetDataFormat",
    "Acquire",
    "Unacquire",
    "Poll",
    "GetDeviceState",
    "BuildActionMap",
    "SetActionMap",
    "GetDeviceData",
    "GetCapabilities",
    "GetProperty",
    "SetProperty",
    "SetEventNotification",
    "GetDataSize",
    "GetData",
    "Apply",
    "Capture",
    "GetFunction",
    "GetDeclaration",
    "CheckDeviceMultiSampleType",
    "DrawPrimitive",
    "DrawIndexedPrimitiveUP",
    "CheckDeviceFormat",
    "CheckDepthStencilMatch",
};
static int mid_resolve(const char *nm){
    for (int i = 1; i < MID__count; i++)
        if (nm[0] == g_midname[i][0] && !strcmp(nm, g_midname[i])) return i;
    return -1;                       /* unknown: do not search again */
}
#define MIDIF 32
#define MIDM 160
static int16_t g_midc[MIDIF][MIDM];  /* 0 = not resolved yet */
#define MID(x_) (mid == MID_##x_)

static int sb_recordable(int mid){
    return MID(SetRenderState)       || MID(SetTextureStageState)
        || MID(SetSamplerState)      || MID(SetTexture)
        || MID(SetVertexShader)      || MID(SetPixelShader)
        || MID(SetVertexDeclaration) || MID(SetFVF)
        || MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)
        || MID(SetVertexShaderConstantI) || MID(SetVertexShaderConstantB)
        || MID(SetTransform)         || MID(SetMaterial)
        || MID(SetLight)             || MID(LightEnable)
        || MID(SetStreamSource)      || MID(SetIndices)
        || MID(SetViewport)          || MID(SetScissorRect)
        || MID(SetNPatchMode)        || MID(SetClipPlane);
}
static int g_rs_init = 0;
/* 19=SRCBLEND 20=DESTBLEND 22=CULLMODE 27=ALPHABLENDENABLE
 * 15=ALPHATESTENABLE 24=ALPHAREF 25=ALPHAFUNC 168=COLORWRITEENABLE 171=BLENDOP */
/* If g_rs[] has not changed, rs_apply() is pure waste: in a run the game
 * calls SetRenderState 91.5 MILLION times and most of those rewrite the
 * same value.  rs_apply derives entirely from g_rs, so skipping it gives
 * exactly the same result.  The flag is STICKY: changes made while metal
 * is not ready yet are still applied once the device arrives. */
int g_rs_dirty = 1;
static uint64_t g_rs_skip = 0, g_rs_run = 0;
static void rs_apply(void){
    g_rs_run++;
    if (!g_rs_init){ g_rs_init=1;
        g_rs[19]=5; g_rs[20]=6; g_rs[27]=0; g_rs[168]=0xF; g_rs[171]=1;
        g_rs[22]=3;                            /* CULLMODE default: CCW */
        g_rs[15]=0; g_rs[24]=0; g_rs[25]=8;
        g_rs[7]=1; g_rs[14]=1; g_rs[23]=4;     /* Z defaults: on */
        g_rs[193]=0xFFFFFFFFu; }               /* BLENDFACTOR default: white */
    unsigned key = (g_rs[27]?1u:0u)
                 | ((g_rs[19] & 31u) << 1)
                 | ((g_rs[20] & 31u) << 6)
                 | ((g_rs[171] & 7u) << 11)
                 | ((g_rs[168] & 15u) << 14);
    /* alpha test: we only support the "greater/greater-equal" form */
    float aref = 0.f;
    if (g_rs[15] && (g_rs[25]==5 || g_rs[25]==6 || g_rs[25]==7))
        aref = (float)(g_rs[24] & 0xFFu) / 255.0f;
    metal_set_blend(key, aref);
    /* D3DRS_BLENDFACTOR (193): when SRCBLEND/DESTBLEND = BLENDFACTOR /
     * INVBLENDFACTOR the blend cross-fades with this CONSTANT colour.
     * In Metal this is encoder state (setBlendColor), not pipeline state. */
    { void metal_set_blendcolor(uint32_t argb);
      metal_set_blendcolor(g_rs[193]); }
    /* D3DRS_CULLMODE (22): 1=NONE 2=CW 3=CCW.  It was never applied; since
     * Metal's default is CullModeNone, the BACK FACES of everything were
     * drawn too.  The far side of a closed body then races the near side in
     * depth -> constantly flickering patches on the car's doors/bumpers, on
     * building edges and along the sky line. */
    { void metal_set_cull(unsigned);
      metal_set_cull(g_rs[22] ? g_rs[22] : 3u); }
    /* D3DRS_FOGENABLE (28) + D3DRS_FOGCOLOR (34).  With a vertex shader
     * the fog factor comes from oFog; the fixed-function stage blends the
     * pixel towards FOGCOLOR.  Measured: FOGENABLE is set 5.3M times, 6 of
     * the 18 vertex shaders write oFog. */
    { void metal_set_fog(unsigned, unsigned);
      metal_set_fog(g_rs[28], g_rs[34]); }
    /* Depth: 7=ZENABLE 14=ZWRITEENABLE 23=ZFUNC (default LESSEQUAL=4).
     * The UI draws with Z off, so this must be carried per draw. */
    { void metal_set_depth(unsigned);
      unsigned zf = g_rs[23] ? (g_rs[23] & 7u) : 4u;
      unsigned dm = (g_rs[7] ? 1u : 0u) | (g_rs[14] ? 2u : 0u) | (zf << 2);
      metal_set_depth(dm); }
    /* 8 stages (Most Wanted): 195=DEPTHBIAS and 175=SLOPESCALEDEPTHBIAS, floats; its shadow-map pass sets
     * 2^-22 (4 units of a 24-bit Z) and 4.0.  Packed as gpu_proto.h GW_ZBIAS_SHIFT says: 4 bits each, so a
     * negative or larger constant and a fractional or larger slope scale change; each such pair is logged
     * (the first 8), to show whether the game asks for one */
    if (g_game.gpu_stages == 8){
        void metal_set_depth_bias(unsigned);
        float cb, sb; memcpy(&cb, &g_rs[195], 4); memcpy(&sb, &g_rs[175], 4);
        float u = cb * 16777216.f;
        unsigned cu = (u > 0.f) ? (u >= 15.f ? 15u : (unsigned)lroundf(u)) : 0u;
        unsigned su = (sb > 0.f) ? (sb >= 15.f ? 15u : (unsigned)lroundf(sb)) : 0u;
        if (!(fabsf(u - (float)cu) < 1e-3f && fabsf(sb - (float)su) < 1e-3f)){
            static uint32_t seen[8][2]; static int nseen;
            int k = 0;
            while (k < nseen && (seen[k][0] != g_rs[195] || seen[k][1] != g_rs[175])) k++;
            if (k == nseen && nseen < 8){
                seen[nseen][0] = g_rs[195]; seen[nseen][1] = g_rs[175]; nseen++;
                fprintf(stderr, "[d3d] depth bias %g, slope scale %g: drawn with %u x 2^-24 and %u (GW_ZBIAS_SHIFT)\n",
                        (double)cb, (double)sb, cu, su);
            }
        }
        metal_set_depth_bias(cu | su << 4);
    }
}
static uint64_t g_rt_set=0, g_rt_screen=0;
static uint64_t g_rt_tex=0;
static uint32_t g_bb_w = 1280, g_bb_h = 720;   /* back buffer size */
/* Cube textures share the IDirect3DTexture9 vtable.  v12na: the cube
 * flag lives in the object header (CF_CUBE).  The old address bitmap was
 * only valid while COM memory was never reused, and that table had to be
 * unbounded because the game recreates its two cube maps on every Reset. */
static unsigned g_cube_n = 0;
static void cube_mark(uint32_t obj){
    if (obj && mem_is_committed(obj - 12u)){ CH_FLAGS(obj) |= CF_CUBE; g_cube_n++; }
}
static int is_cube_tex(uint32_t obj){
    return obj && !(obj & 15u) && mem_is_committed(obj - 12u) && (CH_FLAGS(obj) & CF_CUBE);
}
static int is_volume_tex(uint32_t obj){
    return obj && !(obj & 15u) && mem_is_committed(obj - 12u) && (CH_FLAGS(obj) & CF_VOLUME);
}

/* ---- reference counting (see com_new) ------------------------------------ */
void metal_free_texture(unsigned);
void metal_mirror_forget(uint32_t);
static uint32_t g_bind_tex[8], g_bind_vb[4], g_bind_ib, g_bind_rt[4], g_bind_ds;
static uint64_t g_child_uncached = 0;
static int com_valid(uint32_t o){
    if (!o || (o & 15u) || !mem_is_committed(o - 16u) || !mem_is_committed(o + 3u)) return 0;
    uint32_t i = CH_IFACE(o) & 0xFFFFu;
    return i > 0 && i < NEL(g_if) && g_vtab[i] && MR32(o) == g_vtab[i];
}
static uint32_t com_addref(uint32_t o){
    if (!com_valid(o)) return 1;
    if (CH_FLAGS(o) & CF_CHILD){ uint32_t p = CH_PARENT(o); return com_valid(p) ? com_addref(p) : 1u; }
    return ++CH_REF(o);
}
static void com_destroy(uint32_t o);
static void sb_destroy(uint32_t o);
static uint32_t com_release(uint32_t o){
    if (!com_valid(o)){ g_com_bad++; return 0; }
    if (CH_FLAGS(o) & CF_CHILD){ uint32_t p = CH_PARENT(o); return com_valid(p) ? com_release(p) : 0u; }
    uint32_t r = CH_REF(o);
    if (!r){                                      /* more Releases than references: keep it */
        if (g_com_overrel++ < 8)
            fprintf(stderr, "[com] Release on %s 0x%08x with no reference left (caller 0x%08x)%s\n",
                    g_if[CH_IFACE(o) & 0xFFFFu].name, o, g_com_cpu ? g_com_cpu->eip : 0u,
                    (CH_FLAGS(o) & CF_DEVICE) ? ", device-owned" : "");
        return 0; }
    CH_REF(o) = --r;
    if (!r && !(CH_FLAGS(o) & CF_DEVICE)){
        if (com_frees()) com_destroy(o);
        else if ((CH_IFACE(o) & 0xFFFFu) == IF_STATEBLOCK) sb_destroy(o);   /* the slot table is finite */
    }
    return r;
}
/* d3dx_hle.c: the native effect SetTexture path counts its references.  It
 * runs outside com_call, so it names its CPU: a last Release frees through
 * gheap_free_c(g_com_cpu), and the CPU of the last COM call may belong to a
 * thread that has ended since (its CPU lived on that thread's stack). */
uint32_t com_obj_addref(CPU *c, uint32_t o){ g_com_cpu = c; return com_addref(o); }
uint32_t com_obj_release(CPU *c, uint32_t o){ g_com_cpu = c; return com_release(o); }
/* n AddRefs (n > 0) or -n Releases of o in a row (d3dx_hle.c n8_ring) */
void com_obj_refn(CPU *c, uint32_t o, int32_t n){
    g_com_cpu = c;
    for (; n > 0; n--) com_addref(o);
    for (; n < 0; n++) com_release(o);
}
/* win32_ext.c: a guest thread ends; its CPU must not be used again. */
void com_forget_cpu(CPU *c){ if (g_com_cpu == c) g_com_cpu = 0; }
/* The device holds a reference on whatever is bound. */
static void bind_set(uint32_t *slot, uint32_t o){
    uint32_t old = *slot;
    if (old == o) return;
    if (o) com_addref(o);
    *slot = o;
    if (old) com_release(old);
}
/* Surface (face, level) of texture t.  One cached child per slot at
 * +64..+103 (2D: level; cube: face + 6*level); it shares the texture's
 * count.  GetSurfaceLevel used to create a new object on every call. */
static uint32_t tex_child(uint32_t t, uint32_t face, uint32_t level, int cube){
    uint32_t k = cube ? face + 6u * level : level;
    uint32_t slot = (k < 10u) ? t + 64u + 4u * k : 0u;
    uint32_t sf = slot ? MR32(slot) : 0u;
    if (sf && com_valid(sf) && CH_PARENT(sf) == t) return sf;
    sf = com_new(IF_SURF, 64);
    if (!sf) return 0;
    CH_FLAGS(sf) |= CF_CHILD; CH_PARENT(sf) = t; CH_REF(sf) = 0;
    MR32(sf + 48u) = level;                       /* its mip level (res_desc) */
    if (slot) MR32(slot) = sf; else g_child_uncached++;
    return sf;
}
/* Fields the descriptor methods read, beside the sizes and formats:
 *   texture  +140 Usage, +144 Pool (as created), +148 a volume texture's depth
 *   surface  +12 Usage, +16 Pool, +48 mip level of a texture's surface
 *   VB / IB  +32 Usage, +36 Pool, +40 FVF (VB); +48 and +52 are the mirror's (Lock) */
#define TEX_USAGE 140u
#define TEX_POOL  144u
#define TEX_DEPTH 148u
#define SURF_USAGE 12u
#define SURF_POOL  16u
#define BUF_USAGE 32u
#define BUF_POOL  36u
#define BUF_FVF   40u
/* GetDesc / GetLevelDesc as D3D9 answers them (game.h d3d9_strict): the
 * struct the interface's method fills, nothing past it.  A texture's level
 * has its own size (at least 1); a texture's surface is its level of the
 * texture, with the texture's usage and pool; the back buffer is a render
 * target and the implicit depth surface a depth-stencil, both D3DPOOL_DEFAULT;
 * a volume texture's level is a D3DVOLUME_DESC. */
static uint32_t res_desc(uint32_t iface, uint32_t self, uint32_t level, uint32_t p){
    if (!p || !com_valid(self)) return 0x8876086Cu;                     /* D3DERR_INVALIDCALL */
    if (iface == IF_VB || iface == IF_IB){
        /* D3DVERTEXBUFFER_DESC {Format, Type, Usage, Pool, Size, FVF}; D3DINDEXBUFFER_DESC without FVF */
        mem_commit(p, 24);
        MR32(p)      = iface == IF_VB ? 100u : (MR32(self + 12u) == 102u ? 102u : 101u);   /* VERTEXDATA, INDEX16/32 */
        MR32(p + 4)  = iface == IF_VB ? 6u : 7u;
        MR32(p + 8)  = MR32(self + BUF_USAGE);
        MR32(p + 12) = MR32(self + BUF_POOL);
        MR32(p + 16) = MR32(self + 4u);
        if (iface == IF_VB) MR32(p + 20) = MR32(self + BUF_FVF);
        return 0;
    }
    if (iface == IF_TEX && is_volume_tex(self)){
        /* a volume texture's level: D3DVOLUME_DESC {Format, Type, Usage, Pool, Width, Height, Depth}, 28 bytes */
        uint32_t nlev = MR32(self + 24u) ? MR32(self + 24u) : 1u;
        if (level >= nlev) return 0x8876086Cu;
        uint32_t vw = MR32(self + 4u) >> level, vh = MR32(self + 8u) >> level, vd = MR32(self + TEX_DEPTH) >> level;
        mem_commit(p, 28);
        MR32(p)      = MR32(self + 20u) ? MR32(self + 20u) : 22u;
        MR32(p + 4)  = 2u;                          /* D3DRTYPE_VOLUME */
        MR32(p + 8)  = MR32(self + TEX_USAGE);
        MR32(p + 12) = MR32(self + TEX_POOL);
        MR32(p + 16) = vw ? vw : 1u;
        MR32(p + 20) = vh ? vh : 1u;
        MR32(p + 24) = vd ? vd : 1u;
        return 0;
    }
    uint32_t tex = 0, w, h, fmt, usage, pool;
    if (iface == IF_TEX){ tex = self; }
    else if (iface == IF_SURF && (CH_FLAGS(self) & CF_CHILD) && com_valid(CH_PARENT(self))){
        tex = CH_PARENT(self); level = MR32(self + 48u); }
    else if (iface != IF_SURF) return 0x8876086Cu;
    if (tex){
        uint32_t nlev = MR32(tex + 24u) ? MR32(tex + 24u) : 1u;
        if (level >= nlev) return 0x8876086Cu;
        w = MR32(tex + 4u) >> level; h = MR32(tex + 8u) >> level;
        fmt = MR32(tex + 20u); usage = MR32(tex + TEX_USAGE); pool = MR32(tex + TEX_POOL);
    } else {
        w = MR32(self + 4u); h = MR32(self + 8u); fmt = MR32(self + 20u);
        uint32_t kind = MR32(self + 40u);
        usage = kind == 1u ? 1u : kind == 4u ? 2u : MR32(self + SURF_USAGE);   /* back buffer, implicit Z */
        pool = (kind == 1u || kind == 4u) ? 0u : MR32(self + SURF_POOL);
    }
    mem_commit(p, 32);
    MR32(p)      = fmt ? fmt : 22u;                 /* D3DSURFACE_DESC: Format */
    MR32(p + 4)  = 1u;                              /* Type: D3DRTYPE_SURFACE */
    MR32(p + 8)  = usage;
    MR32(p + 12) = pool;
    MR32(p + 16) = 0;                               /* MultiSampleType: NONE */
    MR32(p + 20) = 0;                               /* MultiSampleQuality */
    MR32(p + 24) = w ? w : 1u;
    MR32(p + 28) = h ? h : 1u;
    return 0;
}
static uint32_t g_present_no;
static void com_drop(uint32_t o){
    uint32_t iv = CH_IFACE(o), iface = iv & 0xFFFFu, extra = iv >> 16;
    if (ENVC("NFSU2_COMLOG"))                         /* test-only: lifetime trace */
        fprintf(stderr, "[com] destroy %s 0x%08x (frame %u, caller 0x%08x)\n",
                iface < NEL(g_if) ? g_if[iface].name : "?", o, g_present_no, g_com_cpu ? g_com_cpu->eip : 0u);
    CH_FLAGS(o) |= CF_DEAD; MR32(o) = 0;              /* a stale call now faults loudly */
    if (iface < NEL(g_if)) g_com_del_n[iface]++;
    g_com_live--;
    com_free(o - 16u, 16u + 4u + extra);
}
static void sb_destroy(uint32_t o);
/* Created depth-surface ids (metal.m g_ztex; 1 = the implicit surface).
 * A released id is not handed out again for three presents: frames recorded
 * before the release may still be encoded on the main thread, and the first
 * pass that names a re-created id drops its Metal depth texture
 * (metal_z_forget) and sizes the new one by its own target.  At a Reset the
 * game releases its 8 depth surfaces and creates 8 new ones at once; with
 * the ids reused at once (last in, first out) a pre-Reset pass on the old
 * 128x128 target could take the forget meant for the new 1024x1024 mirror
 * depth, and the mirror, road and car reflections drew without a Z test
 * until the next Reset.  As with textures (metal.m
 * g_tfq): metal_present waits for the main thread's previous frame, so three
 * presents after the release no recorded frame names the old surface. */
#define ZID_QUARANTINE 3u
static uint8_t  g_zid_state[64];                  /* 0 free, 1 live, 2 released */
static uint32_t g_zid_freed_at[64];               /* g_present_no at the release */
static uint32_t g_zid_next = 2;                    /* where the next search starts */
static uint32_t g_present_no;
uint32_t com_present_no(void){ return g_present_no; }   /* Presents so far (debug_ug2.c) */
static void zid_release(uint32_t zid){
    if (zid < 2u || zid >= 64u) return;
    g_zid_state[zid] = 2; g_zid_freed_at[zid] = g_present_no;
}
static uint32_t zid_take(void){
    uint32_t pick = 0, oldest = 0;
    for (uint32_t k = 0; k < 62u; k++){
        uint32_t z = 2u + (g_zid_next - 2u + k) % 62u;
        if (g_zid_state[z] == 1) continue;
        uint32_t age = g_present_no - g_zid_freed_at[z];
        if (g_zid_state[z] == 0 || age >= ZID_QUARANTINE){ pick = z; break; }
        if (!pick || age > oldest){ pick = z; oldest = age; }   /* none free yet: the longest released */
    }
    if (!pick) pick = g_zid_next;                  /* 62 live surfaces: share an id (as before) */
    g_zid_next = pick + 1u >= 64u ? 2u : pick + 1u;
    g_zid_state[pick] = 1;
    return pick;
}
/* DirectSound buffers: a duplicate shares the original's PCM block, which
 * is freed when the last buffer using it goes. */
static struct { uint32_t data; int n; } g_dsshare[64];
static void ds_share(uint32_t data, int d){
    if (!data) return;
    for (int i = 0; i < 64; i++) if (g_dsshare[i].data == data){
        g_dsshare[i].n += d;
        if (g_dsshare[i].n <= 0){ g_dsshare[i].data = 0; gbuf_free(data); }
        return; }
    if (d > 0) for (int i = 0; i < 64; i++) if (!g_dsshare[i].data){ g_dsshare[i].data = data; g_dsshare[i].n = d; return; }
    /* table full / unknown block: keep it (a leak, never a use after free) */
}
static void com_destroy(uint32_t o){
    uint32_t iface = CH_IFACE(o) & 0xFFFFu;
    switch (iface){
    case IF_TEX: {
        uint32_t tid = MR32(o + 28u);
        if (tid && metal_available()) metal_free_texture(tid);
        for (uint32_t k = 0; k < 8u; k++){ gbuf_free(MR32(o + 32u + 4u*k)); MR32(o + 32u + 4u*k) = 0; }
        if (CH_FLAGS(o) & CF_CUBE)
            for (uint32_t k = 0; k < 8u; k++){ gbuf_free(MR32(o + 104u + 4u*k)); MR32(o + 104u + 4u*k) = 0; }
        if (g_hb_map)                                 /* the levels' copies (host backing) */
            for (uint32_t k = 0; k < 8u; k++){ hb_drop(o + 32u + 4u*k); hb_drop(o + 104u + 4u*k); }
        for (uint32_t k = 0; k < 10u; k++){
            uint32_t sf = MR32(o + 64u + 4u*k);
            if (sf && com_valid(sf) && CH_PARENT(sf) == o){ gbuf_free(MR32(sf + 32u)); com_drop(sf); }
        }
        break; }
    case IF_VB: case IF_IB:
        gbuf_free(MR32(o + 8u)); MR32(o + 8u) = 0;
        if (MR32(o + BUF_HB)){ hb_drop(o + 8u); MR32(o + BUF_HB) = 0; }   /* its copy (host backing) */
        if (metal_available()){ metal_mirror_forget(o); if (iface == IF_IB) metal_mirror_forget(o + 2u); }
        break;
    case IF_SURF:
        if (MR32(o + 40u) == 3u && MR32(o + 28u) && metal_available()) metal_free_texture(MR32(o + 28u));
        if (MR32(o + 56u) == 0x5A535246u) zid_release(MR32(o + 52u));   /* CreateDepthStencilSurface id (ZSURF_MAGIC) */
        gbuf_free(MR32(o + 32u));
        break;
    case IF_STATEBLOCK: sb_destroy(o); break;
    case IF_QUERY: case IF_SWAPCHAIN: break;
    case IF_DSBUF: {
        /* the game silences buffers by releasing them (see Release); the
         * mixer forgets the slot first, then the PCM block can go */
        void audio_unregister(int);
        if (MR32(o + 28u)) audio_unregister((int)MR32(o + 28u));
        MR32(o + 28u) = 0;
        ds_share(MR32(o + 8u), -1);
        break; }
    case IF_DIEFF: dipad_effect_free(o); break;
    default: return;                    /* devices, input, sound: never freed */
    }
    com_drop(o);
}
/* State blocks hold a reference on the textures and buffers they record. */
static int rec_mid(uint32_t magic){
    uint32_t mm = magic & 0xFFFFu, ifc = (magic >> 16) & 0x1FFFu;
    if (ifc >= NEL(g_if) || mm >= g_if[ifc].n) return 0;
    if (ifc < MIDIF && mm < MIDM){
        int md = g_midc[ifc][mm];
        if (!md) md = g_midc[ifc][mm] = (int16_t)mid_resolve(g_if[ifc].m[mm].name);
        return md;
    }
    return mid_resolve(g_if[ifc].m[mm].name);
}
static int sb_objarg(int mid){
    return MID(SetTexture) || MID(SetStreamSource) ? 2 : MID(SetIndices) ? 1 : 0;
}
static void sb_destroy(uint32_t o){
    int bi = mem_is_committed(o + 11u) ? (int)MR32(o + 8u) : 0;
    if (bi <= 0 || bi >= MAXSB) return;
    SBlock *B = &g_sb[bi];
    for (int k = 0; k < B->n; k++){
        int oa = sb_objarg(rec_mid(B->r[k].magic));
        if (oa && B->r[k].a[oa]){ uint32_t x = B->r[k].a[oa]; B->r[k].a[oa] = 0; com_release(x); }
    }
    B->used = 0; B->n = 0;
    if (g_sb_rec == bi) g_sb_rec = 0;
    MR32(o + 8u) = 0;
}

/* Device-owned singletons: back buffer, implicit depth surface, "no depth". */
static uint32_t g_bb_surf = 0, g_zimp_surf = 0, g_znull_surf = 0;
static uint32_t g_swap0 = 0;
static uint32_t dev_surface_kind(uint32_t *slot, uint32_t iface){
    if (!*slot){ *slot = com_new(iface, 64); if (*slot){ CH_FLAGS(*slot) |= CF_DEVICE; CH_REF(*slot) = 0; } }
    return *slot;
}
/* Lock-generation seed per VB/IB (+48): metal_mirror keys on (address,
 * generation), and addresses are reused now. */
static uint32_t buf_serial(void){ static uint32_t n = 0; return (++n) << 12; }

static uint64_t g_st_ok=0, g_st_null=0, g_st_zero=0, g_st_other=0;
static uint32_t g_tex_stage[4] = {0,0,0,0};
static float    g_psc[32][4];                    /* ps constants c0..c31 */
static uint32_t g_psc_max = 0;                   /* highest index seen + 1 */
static uint32_t g_psc_drop = 0;                  /* writes above 32 (dropped) */
static int      g_cur_ps_id = 0;                 /* active ps id */
static int      g_psn = 0;                       /* number of registered ps */
static uint64_t g_stage_hist[4] = {0,0,0,0};
static float g_rmin[3] = { 1e30f, 1e30f, 1e30f };
static float g_rmax[3] = { -1e30f, -1e30f, -1e30f };
/* last 8 SetVertexShaderConstantF calls (for diagnostics) */
static struct { uint32_t sr, pd, n; } g_sc[8];
static unsigned g_scn = 0;

/* D3DDECLTYPE -> read 4 floats into a v# register */
static void decl_fetch(uint32_t addr, unsigned type, float out[4]){
    out[0]=out[1]=out[2]=0.f; out[3]=1.f;
    switch (type){
    case 0: out[0]=MRF32(addr); break;                                   /* FLOAT1 */
    case 1: out[0]=MRF32(addr); out[1]=MRF32(addr+4); break;             /* FLOAT2 */
    case 2: out[0]=MRF32(addr); out[1]=MRF32(addr+4);
            out[2]=MRF32(addr+8); break;                                 /* FLOAT3 */
    case 3: out[0]=MRF32(addr); out[1]=MRF32(addr+4);
            out[2]=MRF32(addr+8); out[3]=MRF32(addr+12); break;          /* FLOAT4 */
    case 4: /* D3DCOLOR: B,G,R,A in memory -> (R,G,B,A) */
            out[0]=MR8(addr+2)/255.f; out[1]=MR8(addr+1)/255.f;
            out[2]=MR8(addr)/255.f;   out[3]=MR8(addr+3)/255.f; break;
    case 5: out[0]=MR8(addr); out[1]=MR8(addr+1);
            out[2]=MR8(addr+2); out[3]=MR8(addr+3); break;               /* UBYTE4 */
    case 8: out[0]=MR8(addr)/255.f; out[1]=MR8(addr+1)/255.f;
            out[2]=MR8(addr+2)/255.f; out[3]=MR8(addr+3)/255.f; break;   /* UBYTE4N */
    case 6: out[0]=(int16_t)MR16(addr); out[1]=(int16_t)MR16(addr+2); break;
    case 7: out[0]=(int16_t)MR16(addr);   out[1]=(int16_t)MR16(addr+2);
            out[2]=(int16_t)MR16(addr+4); out[3]=(int16_t)MR16(addr+6); break;
    case 9: out[0]=(int16_t)MR16(addr)/32767.f;
            out[1]=(int16_t)MR16(addr+2)/32767.f; break;
    case 10:out[0]=(int16_t)MR16(addr)/32767.f;   out[1]=(int16_t)MR16(addr+2)/32767.f;
            out[2]=(int16_t)MR16(addr+4)/32767.f; out[3]=(int16_t)MR16(addr+6)/32767.f; break;
    case 11:out[0]=MR16(addr)/65535.f; out[1]=MR16(addr+2)/65535.f; break;
    case 12:out[0]=MR16(addr)/65535.f;   out[1]=MR16(addr+2)/65535.f;
            out[2]=MR16(addr+4)/65535.f; out[3]=MR16(addr+6)/65535.f; break;
    default: break;
    }
}

/* --- DRAWDUMP=<frame#>: dump every draw of that frame in detail --- */
static uint32_t g_present_no;
#define ZSURF_MAGIC 0x5A535246u   /* 'ZSRF': Z surface object, +52 = id */
static uint32_t g_cur_zid = 1;           /* bound Z surface (0 = none) */
/* Back buffer surface (+40 = 1); its size follows Reset. */
static uint32_t bb_surface(void){
    uint32_t sf = dev_surface_kind(&g_bb_surf, IF_SURF);
    if (sf){ MR32(sf+4)=g_bb_w; MR32(sf+8)=g_bb_h; MR32(sf+20)=22u; MR32(sf+40)=1; }
    return sf;
}
/* Implicit depth surface (zid 1) and the "no depth" surface (zid 0). */
static uint32_t z_surface(uint32_t zid){
    uint32_t sf = dev_surface_kind(zid ? &g_zimp_surf : &g_znull_surf, IF_SURF);
    if (sf){ MR32(sf+4)=g_bb_w; MR32(sf+8)=g_bb_h; MR32(sf+20)=80u; MR32(sf+40)=4;
             MR32(sf+52)=zid; MR32(sf+56)=ZSURF_MAGIC; }
    return sf;
}
/* --- device loss / re-creation -----------------------------------------
 * When the window mode changes (full screen <-> window) the drawing
 * resolution must change too; the game's render targets depend on the
 * device size, so ONLY the game itself can do that, with Reset.  As on
 * Windows at alt-tab we report the device as "lost": Present returns
 * D3DERR_DEVICELOST, TestCooperativeLevel D3DERR_DEVICENOTRESET, the game
 * releases its resources and calls Reset, res_pick picks a size for the
 * new mode. */
static _Atomic int g_lost = 0;
static unsigned long g_tcl_n = 0, g_tcl_lost_n = 0, g_lost_presents = 0, g_resets = 0, g_reset_fails = 0;
void com_request_reset(void){
    if (!g_dev) return;          /* no device yet: CreateDevice will see the new mode anyway */
    atomic_store(&g_lost, 1); }
static int g_wc_armed = 0, g_wc_after = 0;
static int      g_dd_want    = -1;
static int dd_on(void){
    static int init=0;
    if (!init){ init=1; const char *e=ENVC("DRAWDUMP"); g_dd_want = e&&*e ? atoi(e) : -1; }
    return g_dd_want >= 0 && (uint32_t)g_dd_want == g_present_no;
}
static void dd_decl(uint32_t d){
    uint32_t el = (d && mem_is_committed(d+4)) ? MR32(d+4) : 0;
    static const char *TY[]={"FLOAT1","FLOAT2","FLOAT3","FLOAT4","D3DCOLOR","UBYTE4",
        "SHORT2","SHORT4","UBYTE4N","SHORT2N","SHORT4N","USHORT2N","USHORT4N",
        "UDEC3","DEC3N","FLOAT16_2","FLOAT16_4","UNUSED"};
    static const char *US[]={"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL","PSIZE",
        "TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT","COLOR","FOG",
        "DEPTH","SAMPLE"};
    if (!el){ fprintf(stderr,"      decl=0x%08x (no declaration)\n", d); return; }
    fprintf(stderr,"      decl=0x%08x:", d);
    for (uint32_t k=0;k<16;k++){
        uint32_t e=el+8u*k;
        if (!mem_is_committed(e+7)) break;
        uint32_t of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6), ui=MR8(e+7);
        if (MR16(e)==0xFFu && ty==17u) break;
        fprintf(stderr," %u:%s/%s%u", of, ty<18?TY[ty]:"?", us<14?US[us]:"?", ui);
    }
    fprintf(stderr,"\n");
}
int g_com_trace = 0;
void com_set_trace(int v){ g_com_trace=v; }
void draw_stats_save(uint64_t*,uint64_t*);
/* During replay (state block Apply) the arguments come from the record,
 * not from the guest stack. */
static const uint32_t *g_arg_ovr = 0;
static uint32_t A(CPU*c,int i){
    if (g_arg_ovr) return g_arg_ovr[i & 7];
    return MR32(c->esp + 4u*(uint32_t)i);
}

/* --- run a guest callback as stdcall --------------------
 * DirectInput's EnumDevices family works with callbacks.  If the callback
 * is never made the game finds NO input device at all: the mouse cursor
 * is read but menu actions are bound to nothing.
 * The CPU state is preserved; with stdcall the callee pops the arguments. */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args){
    if (!fn) return 0;
    CPU save = *c;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(0xDEADBEEFu);                    /* fake return address */
    dispatch_indirect(c, fn);
    uint32_t ret = c->eax;
    *c = save;                              /* back to the pre-call state */
    return ret;
}

/* GUID_SysKeyboard / GUID_SysMouse (DirectInput system devices) */
static void put_guid(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3,
                     const uint8_t d4[8]){
    MR32(p)=d1; MR16(p+4)=d2; MR16(p+6)=d3;
    for (int i=0;i<8;i++) MR8(p+8+i)=d4[i];
}

/* Fill a DIDEVICEINSTANCEA (580 bytes) and return its address. */
static uint32_t make_devinst(int keyboard);
uint32_t com_devinst(int keyboard){ return make_devinst(keyboard); }   /* dinput_kbd.c */
static uint32_t make_devinst(int keyboard){
    static uint32_t kb = 0, ms = 0;
    uint32_t *slot = keyboard ? &kb : &ms;
    if (*slot) return *slot;
    uint32_t p = gbuf_alloc(600);
    memset(g_mem+p, 0, 600);
    MR32(p) = 580u;                                   /* dwSize */
    static const uint8_t tail[8] = {0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00};
    put_guid(p+4,  keyboard?0x6F1D2B61u:0x6F1D2B60u, 0xD5A0, 0x11CF, tail);
    put_guid(p+20, keyboard?0x6F1D2B61u:0x6F1D2B60u, 0xD5A0, 0x11CF, tail);
    /* dwDevType: subtype << 8 | type.  0x13=KEYBOARD (PCENH), 0x12=MOUSE */
    MR32(p+36) = keyboard ? 0x0413u : 0x0212u;
    snprintf((char*)(g_mem+p+40),  260, "%s", keyboard ? "Keyboard" : "Mouse");
    snprintf((char*)(g_mem+p+300), 260, "%s", keyboard ? "Keyboard" : "Mouse");
    *slot = p;
    return p;
}

static void fill_caps(uint32_t p);


/* --- vertex processing: gather inputs per the declaration, run the shader --- */
typedef struct { uint16_t off; uint8_t type; int8_t reg; } DElem;

static int g_fixmat = -1;
static uint64_t g_t_draw_ns = 0, g_v_xform = 0;
static uint64_t g_t_vs_ns = 0;
static uint64_t g_v_per_vs[64] = {0};
/* Draw path timing statistics (exit report) only with DRAWTIME=1: two clock
 * reads per draw were ~0.8% of the guest thread in the real game. */
static inline uint64_t now_ns(void){
    static int on = -1;
    if (on < 0) on = getenv("DRAWTIME") != NULL;
    if (!on) return 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
/* GPU shader id for a (vertex shader, vertex declaration) pair.  The
 * same bytecode can be used with different declarations, so the key is
 * the two together. */
#define MAXGVS 256
static struct { uint32_t vs, decl; int gid, state; } g_gvs[MAXGVS];
static int g_gvsn = 0, g_gvs_next = 1;
static int g_gvs_nk[MAXGVS];
static int g_gpuvs = -1;
static uint64_t g_gpuvs_draws = 0, g_gpuvs_fail = 0, g_gpuvs_drop = 0;

static int gpuvs_id(const VSProg *prog, uint32_t vs, uint32_t decl,
                    const DElem *el, int nel){
    for (int i = 0; i < g_gvsn; i++)
        if (g_gvs[i].vs == vs && g_gvs[i].decl == decl)
            return g_gvs[i].state > 0 ? g_gvs[i].gid : -1;
    if (g_gvsn >= MAXGVS || g_gvs_next >= MAXGVS) return -1;
    int slot = g_gvsn++;
    g_gvs[slot].vs = vs; g_gvs[slot].decl = decl;
    g_gvs[slot].state = -1; g_gvs[slot].gid = 0;

    int dcl[16][2];
    for (int i = 0; i < 16; i++){ dcl[i][0] = -1; dcl[i][1] = 0; }
    for (int k = 0; k < nel; k++){
        int rg = el[k].reg;
        if (rg >= 0 && rg < 16){ dcl[rg][0] = (int)el[k].off; dcl[rg][1] = (int)el[k].type; }
    }
    int gid = g_gvs_next;
    /* GPUVSMASK: which translated shaders are allowed (bit0=vs_1).
     * For bisecting an image bug shader by shader. */
    { static long long mask = -1;        /* 64 bits on wasm32 too (ids go past 32) */
      if (mask < 0){ const char *em = ENVC("GPUVSMASK");
                     mask = (em && *em) ? strtoll(em, 0, 0) : -1LL; }
      if (mask >= 0 && !((mask >> (gid-1)) & 1LL)){
          if (ENVC("GPUVSLOG"))
              fprintf(stderr, "  [gpuvs] shader=%u decl=%08x -> outside the mask, on the CPU\n", vs, decl);
          g_gvs_next++;         /* consume the id anyway so the mask bits do not shift */
          return -1;
      } }
    static char msl[160*1024];          /* HD: + capture kernel */
#ifdef __wasm__
    int len = vs_to_wgsl(prog, gid, (const int (*)[2])dcl, msl, sizeof msl);
    if (wgsl_dump_on()) wgsl_dump_vs(gid, (int)vs, decl, (const int (*)[2])dcl, msl, len);
#else
    int len = vs_to_msl(prog, gid, (const int (*)[2])dcl, msl, sizeof msl);
#endif
    if (len <= 0){ g_gpuvs_fail++; return -1; }
    void metal_register_vs(int, const char *);
    int  metal_vs_ready(int);
    metal_register_vs(gid, msl);
    if (!metal_vs_ready(gid)){ g_gpuvs_fail++; return -1; }
    g_gvs_next++;
    g_gvs[slot].gid = gid; g_gvs[slot].state = 1;
    g_gvs_nk[gid] = vs_kmax(prog);
#ifdef __wasm__
    /* the falloff of a lit shader (oT2.x = s * w + o) for the other cars' headlights */
    { float rp[2]; void headlights_vs_ramp(float, float);
      if (vs_light_creg(prog, rp) >= 0) headlights_vs_ramp(rp[0], rp[1]); }
#endif
#if NFS_ENHANCED
    /* HD ray tracing experiment: offset/type of the position element, the
     * constant register of the position matrix and whether it is skinned
     * (BLENDINDICES = usage 2). */
    { int pr = vs_input_reg(prog, 0, 0), po = -1, pty = -1, cw = -1;
      for (int k = 0; k < nel; k++) if (el[k].reg == pr){ po = (int)el[k].off; pty = (int)el[k].type; }
      int cx = vs_pos_creg(prog, &cw);
      float ramp[2] = { 0.005f, 0.5f };
      int lr = vs_light_creg(prog, ramp);
      void metal_hd_vs_info(int, int, int, int, int, int, int, float, float);
      metal_hd_vs_info(gid, po, pty, (cw == cx + 3) ? cx : -1,
                       vs_input_reg(prog, 2, 0) >= 0 || vs_input_reg(prog, 1, 0) >= 0,
                       vs_fog_creg(prog), lr, ramp[0], ramp[1]); }
#endif
    if (ENVC("GPUVSLOG"))
        fprintf(stderr, "  [gpuvs] shader=%u decl=%08x -> vs_%d (%u instructions, %d constants)%s\n",
                vs, decl, gid, vs_ninstr(prog), g_gvs_nk[gid],
                vs_writes_fog(prog) ? "  [WRITES oFog]" : "");
    /* GPUVSDUMP=<gid>: print the generated MSL and the source bytecode. */
    { const char *dv = ENVC("GPUVSDUMP");
      if (dv && *dv && atoi(dv) == gid){
          fprintf(stderr, "--- vs_%d source disassembly ---\n", gid);
          vs_disasm(prog, stderr);
          fprintf(stderr, "--- vs_%d generated MSL ---\n%s\n", gid, msl);
      } }
    return gid;
}

/* The game's front-end WorldViewProj matrix arrives broken (only rows 2
 * and 3 are written).  FIXMAT: if it is broken, put in a sensible matrix.
 * The decision does not change during a draw, so it is made once OUTSIDE
 * the vertex loop; the CPU and GPU paths use the same helper so the two
 * do not diverge. */
/* --- Widescreen (Hor+) -------------------------------------------------
 * NFSU2 is designed for 4:3: ALL 6 rows of the internal resolution table
 * are 4:3 and index 6+ overruns the array, so 1080p cannot be picked in
 * the menu.  With the device at 1920x1080 the game renders the same 4:3
 * image and STRETCHES it horizontally (measured: the 1080p frame is a
 * pure stretch of the 4:3 frame -- difference 2.03, vertical-crop
 * hypothesis 34.2).
 * Fix: in perspective draws scale clip-space x by game_aspect/device_aspect.
 * The proportions are corrected and MORE of the scene shows horizontally
 * (Hor+).  Telling them apart: oPos.w = dot(v, c3); c3.w=0 in perspective,
 * c3.w=1 in the orthographic (UI) matrix.  The UI is left alone. */
static float g_aspfix = 1.f;
/* Hor+ factor (for clip-space x); the HD composite pass uses it to bring
 * the camera into the same space as the geometry drawn on screen. */
float com_aspfix(void){ return ws_persp_x(); }
#if NFS_ENHANCED
static VSProg *g_psprog[256];
/* Reflection-separation variant: base ps (cube=1 for the cube variant),
 * reflection stage k, Metal id vid.  1 on success. */
int com_ps_refl_variant(int base, int cube, int k, int vid){
    if (base <= 0 || base >= 256 || !g_psprog[base]) return 0;
    static char msl[32768];
    if (ps_to_msl_ex2(g_psprog[base], vid, msl, sizeof msl, cube ? 1u : 0u, k) <= 0) return 0;
    void metal_register_ps(int, const char *);
    metal_register_ps(vid, msl);
    fprintf(stderr, "[HD] reflection variant: ps %d%s stage %d -> %d\n",
            base, cube ? " (cube)" : "", k, vid);
    return 1;
}
#endif
/* With 8 stages (GameOps gpu_stages): each pixel shader's program, kept for
 * the depth variants a draw may need (gpu_web.c rec_draw, gpu-abi.md 11.4) */
static VSProg *g_ps8[PSCUBE];
/* register module `full`: pixel shader id (full & 0xFF), its stage-0 cube
 * twin (bit 8) and the stages that sample a depth texture (bits 9+); 1 when
 * it is translated */
int com_ps_depth_variant(unsigned full){
#ifdef __wasm__
    unsigned id = full & 0xFFu, cube = (full >> 8) & 1u, dm = full >> 9;
    if (!id || !dm || !g_ps8[id]) return 0;
    static char msl[SHADER_TEXT_CAP];
    wgsl_set_depth_stages(dm);
    int n = ps_to_wgsl_ex(g_ps8[id], (int)full, msl, sizeof msl, cube);
    wgsl_set_depth_stages(0);
    if (wgsl_dump_on()) wgsl_dump_ps((int)full, (int)id, msl, n);
    if (n <= 0) return 0;
    void metal_register_ps(int, const char *);
    metal_register_ps((int)full, msl);
    return 1;
#else
    (void)full; return 0;
#endif
}
static uint32_t g_req_w = 0, g_req_h = 0;   /* the resolution the game believes it has */
unsigned long g_asp_persp = 0, g_asp_ortho = 0;
/* Perspective draws the renderer rescaled into an off-screen target
 * (NFSU2_FOV=renderer): cube map faces and other render targets. */
static unsigned long g_asp_cube = 0, g_asp_rt = 0;
/* Resolution the game asks for -> resolution the device REALLY draws at.
 * Three rules are gathered in one place: the NFSRES pin, the widescreen
 * choice from the menu and the Retina pixel density.  CreateDevice and
 * Reset MUST use the same rule: Reset used to pass the raw value through,
 * so changing the resolution in game silently undid the correction. */
static void res_pick(uint32_t gw, uint32_t gh, uint32_t *dw, uint32_t *dh){
    static int rdone = 0, pinned = 0;
    static uint32_t rw = 0, rh = 0;
    *dw = gw; *dh = gh;
    /* The cursor must be in the space the game BELIEVES in, not on the drawable. */
    { void metal_set_game_size(unsigned,unsigned); metal_set_game_size(gw, gh); }
    if (!rdone){ rdone = 1; const char *e = ENVC("NFSRES");
        if (e && *e){ unsigned a = 0, b = 0;
            if (sscanf(e, "%ux%u", &a, &b) == 2 && a >= 640 && b >= 480
                && a <= 8192 && b <= 8192){ rw = a; rh = b; pinned = 1; } } }
    uint32_t aw = rw, ah = rh;
    int row = tbl_row(gw, gh);
    g_cur_row = row;
    if (modemap_on()) modes_refresh();                 /* screen row + menu strings */
    if (!pinned && row >= 0 && modemap_on()){
        aw = g_modes[row][0]; ah = g_modes[row][1];
        fprintf(stderr,"com: menu row %d -> %ux%u\n", row + 1, aw, ah);
    }
    else if (!pinned){
        uint32_t bw = aw ? aw : gw, bh = ah ? ah : gh;
        const char *nv = ENVC("NFSNATIVE");
        if (bw && bh && !(nv && nv[0] == '0')){
            void metal_native_size(unsigned,unsigned,unsigned*,unsigned*);
            unsigned nw = bw, nh = bh;
            metal_native_size(bw, bh, &nw, &nh);
            if (nw > bw || nh > bh){ aw = nw; ah = nh; } }
    }
    if (!aw || !ah){
        /* No override: device = what the game asked for.  Values left over
         * from an earlier CreateDevice must not go stale after a Reset
         * (RT resizing and GetDisplayMode look at them). */
        g_req_w = g_req_h = 0; g_aspfix = 1.f;
        ws_set_mapping(1.f);
        { void metal_set_ui_aspect(float, float); metal_set_ui_aspect(1.f, 1.f); }
        return;
    }
    g_req_w = gw; g_req_h = gh;
    /* The game keeps drawing for its own aspect; we compensate for the
     * difference in clip space.  If the aspect is kept exactly, this
     * factor stays 1.0. */
    if (gw && gh){
        float ga = (float)gw / (float)gh, da = (float)aw / (float)ah;
        g_aspfix = ga / da;
        const char *ov = ENVC("ASPFIX");
        if (ov && *ov) g_aspfix = (float)atof(ov);
    }
    *dw = aw; *dh = ah;
    /* widescreen.c decides what the renderer and the game hooks do with
     * the ratio; the mouse maps onto the 4:3 HUD the same way. */
    ws_set_mapping(g_aspfix);
    { void metal_set_ui_aspect(float, float); metal_set_ui_aspect(ws_ui_x(), ws_ui_y()); }
    fprintf(stderr,"com: device resolution %ux%u -> %ux%u (aspfix=%.4f)\n",
            gw, gh, aw, ah, (double)g_aspfix);
}
/* Copy ONLY the part of the constant file this shader reads.
 * Previously the whole file (4416 bytes) was copied per draw just to
 * scale one row (16 bytes): 1316 copies per frame in a race, 5.7 MB,
 * 0.14 ms (measured).  Downstream never reads outside [0, vs_kmax): the
 * GPU path hands metal_draw_raw exactly vs_kmax rows, and the CPU path
 * vs_run stays within that bound too (with relative addressing vs_kmax
 * is 256 anyway). */
static int16_t g_vs_kmax[MAXVS];          /* 0 = not looked up yet */
static void vsk_copy_used(VSConst *dst, const VSConst *src, const VSProg *prog){
    unsigned vs = g_cur_vs;
    int n = (vs < MAXVS) ? g_vs_kmax[vs] : 0;
    if (n <= 0){ n = vs_kmax(prog); if (n < 1) n = 1; if (n > 256) n = 256;
                 if (vs < MAXVS) g_vs_kmax[vs] = (int16_t)n; }
    memcpy(dst->f, src->f, (size_t)n * sizeof dst->f[0]);
    memcpy(dst->i, src->i, sizeof dst->i);
    memcpy(dst->b, src->b, sizeof dst->b);
}
/* The constant register of the position matrix differs per shader; resolve
 * it once and keep it (-2 = not looked up yet). */
static int8_t g_pos_cx[MAXVS], g_pos_cy[MAXVS], g_pos_cw[MAXVS]; static uint8_t g_pos_seen[MAXVS];
static unsigned long g_asp_np[MAXVS], g_asp_no[MAXVS]; static float g_asp_cw3[MAXVS];
/* WSLOG=1 (diagnostic): first draw of every (target, shader, kind) with the
 * position rows, so it is visible which passes the aspect fix touches. */
static void ws_log_draw(const char *kind, const VSConst *vsk, int cx, int cw){
    static int on = -1, n = 0;
    static struct { unsigned rt, vs; char k; } seen[512];
    if (on < 0) on = ENVC("WSLOG") != NULL;
    if (!on || n >= 512) return;
    for (int i = 0; i < n; i++)
        if (seen[i].rt == g_cur_rt && seen[i].vs == g_cur_vs && seen[i].k == kind[0]) return;
    seen[n].rt = g_cur_rt; seen[n].vs = g_cur_vs; seen[n].k = kind[0]; n++;
    unsigned w = 0, h = 0; void metal_tex_size(unsigned, unsigned*, unsigned*);
    metal_tex_size(g_cur_rt, &w, &h);
    fprintf(stderr, "  [ws] f=%u rt=%u (%ux%u) vs=%u ps=%d %s", g_present_no, g_cur_rt, w, h,
            g_cur_vs, g_cur_ps_id, kind);
    if (cx >= 0) fprintf(stderr, " cx=c%d(%.5f %.5f %.5f %.5f)", cx,
            vsk->f[cx][0], vsk->f[cx][1], vsk->f[cx][2], vsk->f[cx][3]);
    if (cw >= 0) fprintf(stderr, " cw=c%d(%.5f %.5f %.5f %.5f)", cw,
            vsk->f[cw][0], vsk->f[cw][1], vsk->f[cw][2], vsk->f[cw][3]);
    fprintf(stderr, "\n");
}
/* this draw's 2D UI band (aspect_apply), 1 = none */
static float g_ui_clip = 1.f;
static const VSConst *aspect_apply(const VSConst *vsk, const VSProg *prog){
    if (g_aspfix == 1.f || !prog) return vsk;
    unsigned vs = g_cur_vs;
    if (vs >= MAXVS) return vsk;
    if (!g_pos_seen[vs]){
        int w = -1; int x = vs_pos_creg(prog, &w), y = vs_pos_creg_y(prog);
        g_pos_cx[vs] = (int8_t)((x >= 0 && x <= 120) ? x : -1);
        g_pos_cy[vs] = (int8_t)((y >= 0 && y <= 121) ? y : -1);
        g_pos_cw[vs] = (int8_t)((w >= 0 && w <= 123) ? w : -1);
        g_pos_seen[vs] = 1;
    }
    int cx = g_pos_cx[vs], cy = g_pos_cy[vs], cw = g_pos_cw[vs];
    if (cx < 0 || cw < 0){ g_asp_ortho++; ws_log_draw("none", vsk, cx, cw); return vsk; }
    /* Perspective?  oPos.w = dot(v, c[cw]).  In an orthographic/screen matrix
     * this row is (0,0,0,1): w is constant 1.  In a perspective one the xyz
     * part is the view axis and is NOT ZERO (the w component is the camera
     * distance -- measured: 5.79 / 222.0 / -86.5, so a "w==0" test was wrong). */
    g_asp_cw3[vs] = vsk->f[cw][3];
    float sx, sy;
    float ax = fabsf(vsk->f[cw][0]) + fabsf(vsk->f[cw][1]) + fabsf(vsk->f[cw][2]);
    int ortho = ax < 1e-6f;
    if (ortho){
        g_asp_ortho++; g_asp_no[vs]++; ws_log_draw("ortho", vsk, cx, cw);
        /* The 4:3 HUD is shown in the middle of the screen, so circles stay
         * circles (HUDFIX=0: stretched).  Full-screen post-processing quads
         * have no position matrix and never get here.  Only for the screen
         * (and screen-sized targets): the rear-view mirror's rounded mask is
         * an identity-matrix quad drawn into its own 256x256 target, and
         * narrowing it left the target's corners unmasked (stale content
         * beside the mirror). */
        sx = ws_ui_x(); sy = ws_ui_y();
        if (g_cur_rt){
            static unsigned rt_seen = 0; static int rt_screen = 0;
            if (g_cur_rt != rt_seen){
                unsigned w = 0, h = 0; void metal_tex_size(unsigned, unsigned *, unsigned *);
                metal_tex_size(g_cur_rt, &w, &h);
                rt_seen = g_cur_rt; rt_screen = (w == g_bb_w && h == g_bb_h);
            }
            if (!rt_screen) sx = sy = 1.f;
        }
        /* A quad the game draws over the whole target (the rain tint) is in
         * HUD units too, but must cover the whole screen (widescreen.c). */
        if (ws_fill_target()) sx = sy = 1.f;
        /* Narrower than 4:3 the HUD space is a band across the middle, and
         * what the game parks outside 640x480 would show above and below it:
         * the draw is kept to the band (gpu_web.c metal_set_ui_clip), but for
         * the cutscene bars, which belong on the picture's edges. */
        if (sy < 1.f && !ws_take_border_quad()) g_ui_clip = sy;
        if (ENVC("ASPLOG")){ static int no = 0;
            if (no < 6){ no++;
                fprintf(stderr,"  [asp-ortho] vs=%u cx=%d cw=%d  c%d=(%.4f %.4f %.4f %.4f)"
                        "  c%d=(%.4f %.4f %.4f %.4f)\n", vs, cx, cw,
                        cx, vsk->f[cx][0],vsk->f[cx][1],vsk->f[cx][2],vsk->f[cx][3],
                        cw, vsk->f[cw][0],vsk->f[cw][1],vsk->f[cw][2],vsk->f[cw][3]); } }
    } else {
        g_asp_persp++; g_asp_np[vs]++; ws_log_draw("persp", vsk, cx, cw);
        /* Hor+ is applied in the game's projection setup (widescreen.c);
         * NFSU2_FOV=renderer scales every perspective draw here instead. */
        sx = ws_persp_x(); sy = ws_persp_y();
    }
    if (sx == 1.f && (sy == 1.f || cy < 0)) return vsk;
    if (!ortho && g_cur_rt){ int metal_tex_is_cube(unsigned);
        if (metal_tex_is_cube(g_cur_rt)) g_asp_cube++; else g_asp_rt++; }
    static VSConst tmp;
    vsk_copy_used(&tmp, vsk, prog);
    for (int k = 0; k < 4; k++){
        tmp.f[cx][k] *= sx;
        if (cy >= 0) tmp.f[cy][k] *= sy;
    }
    if (!ortho && ENVC("ASPLOG")){ static int n = 0;
        if (n < 6){ n++;
            fprintf(stderr,"  [asp] vs=%u c%d=(%.4f %.4f %.4f %.4f) -> (%.4f %.4f %.4f %.4f)"
                    "  c%d=(%.3f %.3f %.3f %.3f)\n", g_cur_vs, cx,
                    vsk->f[cx][0],vsk->f[cx][1],vsk->f[cx][2],vsk->f[cx][3],
                    tmp.f[cx][0],tmp.f[cx][1],tmp.f[cx][2],tmp.f[cx][3],
                    cw, vsk->f[cw][0],vsk->f[cw][1],vsk->f[cw][2],vsk->f[cw][3]); } }
    return &tmp;
}

uint64_t g_fixmat_hits = 0;      /* v12na: how often the "broken WVP" pattern was seen */
static const VSConst *fixmat_apply(const VSConst *vsk){
    if (!(vsk->f[3][3] == 0.f && vsk->f[3][0] == 0.f &&
          vsk->f[0][0] == 0.f && vsk->f[1][1] == 0.f)) return vsk;
    if (g_fixmat_hits++ == 0) fprintf(stderr, "com: FIXMAT pattern seen for the first time (FIXMAT=%s)\n",
                                      ENVC("FIXMAT") ? ENVC("FIXMAT") : "-");
    if (g_fixmat < 0){ const char *e6 = ENVC("FIXMAT");
                       g_fixmat = (e6 && *e6 && *e6 != '0') ? 1 : 0; }
    if (!g_fixmat) return vsk;
    static VSConst tmp;
    static int mode = -1;
    if (mode < 0){ const char *e4 = ENVC("FIXMAT_MODE"); mode = e4 ? atoi(e4) : 0; }
    memcpy(&tmp, vsk, sizeof tmp);
    if (mode == 4){
        static float t4 = -1e30f;
        if (t4 == -1e30f){ const char *e9 = ENVC("FIXMAT_YOFF");
                           t4 = e9 ? (float)atof(e9) : 0.f; }
        memset(tmp.f, 0, sizeof tmp.f);
        tmp.f[0][0]=1.f; tmp.f[1][1]=1.f; tmp.f[1][3]=t4;
        tmp.f[2][2]=1.f; tmp.f[3][3]=1.f;
        return &tmp;
    }
    if (mode == 3){
        /* The raw positions are already in clip space; the matrix the
         * game meant to write is the identity. */
        memset(tmp.f, 0, sizeof tmp.f);
        tmp.f[0][0]=1.f; tmp.f[1][1]=1.f; tmp.f[2][2]=1.f; tmp.f[3][3]=1.f;
        return &tmp;
    }
    if (mode == 1 || mode == 2){
        float ys = (mode == 1) ? -1.f : 1.f;
        float yb = (mode == 1) ?  1.f : -1.f;
        tmp.f[0][0]=0.75f; tmp.f[0][1]=0; tmp.f[0][2]=0; tmp.f[0][3]=0;
        tmp.f[1][0]=0; tmp.f[1][1]=ys; tmp.f[1][2]=0; tmp.f[1][3]=yb;
        tmp.f[2][0]=0; tmp.f[2][1]=0; tmp.f[2][2]=1.f; tmp.f[2][3]=0;
        tmp.f[3][0]=0; tmp.f[3][1]=0; tmp.f[3][2]=0; tmp.f[3][3]=1.f;
        return &tmp;
    }
    tmp.f[0][0]=0.75f; tmp.f[0][1]=0; tmp.f[0][2]=0; tmp.f[0][3]=0;
    { static float yoff = -1e30f, ysc = 1.f;
      if (yoff == -1e30f){
          const char *e2 = ENVC("FIXMAT_YOFF");
          const char *e3 = ENVC("FIXMAT_YSCALE");
          yoff = e2 ? (float)atof(e2) : 0.f;
          ysc  = e3 ? (float)atof(e3) : 1.f; }
      tmp.f[1][0]=0; tmp.f[1][1]=ysc; tmp.f[1][2]=0; tmp.f[1][3]=-yoff; }
    tmp.f[2][0]=0; tmp.f[2][1]=0; tmp.f[2][2]=1.f; tmp.f[2][3]=0;
    tmp.f[3][0]=0; tmp.f[3][1]=0; tmp.f[3][2]=0; tmp.f[3][3]=1.f;
    return &tmp;
}

/* Per stage: "which coordinate set + projective divide".
 * D3DTSS_TEXCOORDINDEX = 11, D3DTSS_TEXTURETRANSFORMFLAGS = 24.
 * Encoding: index + 16*mode  (mode 0 = no divide, 1 = /z, 2 = /w).
 * In the trace, across 422 thousand calls, stages 1 and 2 use COORDINATE 0;
 * we were giving stage N coordinate N. */
/* Carry the game's D3DSAMP_* state over to the Metal sampler.
 * D3DSAMP: 1=ADDRESSU 2=ADDRESSV 5=MAGFILTER 6=MINFILTER 7=MIPFILTER
 *          9=MAXMIPLEVEL 10=MAXANISOTROPY
 * Previously all of it was ignored and everything was drawn linear+mip-linear. */
static void smp_push(void){
    void metal_set_samplers(const unsigned k[4]);
    unsigned k[8];
    const int n = g_game.gpu_stages == 8 ? 8 : 4;
    for (int st = 0; st < n; st++){
        unsigned mn = g_smp_raw[st][6], mg = g_smp_raw[st][5], mp = g_smp_raw[st][7];
        unsigned au = g_smp_raw[st][1], av = g_smp_raw[st][2], an = g_smp_raw[st][10];
        if (mn < 1u || mn > 3u) mn = 2u;          /* invalid/unset -> LINEAR */
        if (mg < 1u || mg > 3u) mg = 2u;
        if (mp > 2u) mp = 0u;
        if (au < 1u || au > 5u) au = 1u;          /* -> WRAP */
        if (av < 1u || av > 5u) av = 1u;
        if (an < 1u) an = 1u; if (an > 16u) an = 16u;
        unsigned al = 0; while ((1u << al) < an) al++;
        k[st] = (mn & 3u) | ((mg & 3u) << 2) | ((mp & 3u) << 4)
              | ((au & 7u) << 6) | ((av & 7u) << 9) | ((al & 7u) << 12);
    }
    metal_set_samplers(k);
    if (n == 8){
        /* stages 4-7 (docs/web/gpu-abi.md 11.2): their textures and sampler keys */
        void metal_set_stage_hi(const unsigned t[4], const unsigned k[4]);
        unsigned t[4];
        for (int st = 4; st < 8; st++){
            uint32_t to = g_raw_tex[st];
            t[st - 4] = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
        }
        metal_set_stage_hi(t, k + 4);
    }
}

static void texproj_push(void){
    void metal_set_texproj(const float[4]);
    float sel[4];
    for (int st = 0; st < 4; st++){
        unsigned ci  = g_tss_raw[st][11] & 0xFFFFu;
        unsigned ttf = g_tss_raw[st][24];
        if (ci > 3u) ci = (unsigned)st;
        int pm = 0;
        if (ttf & 0x100u){                 /* D3DTTFF_PROJECTED */
            /* NFSU2 writes this with a COUNT FIELD of 0 (measured: all 28
             * calls in the trace are exactly "D3DTTFF_PROJECTED").  The vertex
             * shader produces a 4-component coordinate (oT1.xyz = dot(v0,c4..c6),
             * oT1.w = r0.x), so count 0 -> divide by W.
             * Without the divide the headlight texture spreads over the surface
             * instead of forming a cone: that is the "headlight streaks" the
             * user saw and the streaky reflections on the ground (stage 3 = reflected_sampler). */
            unsigned cnt = ttf & 7u;
            pm = (cnt == 3u) ? 1 : 2;      /* 3 -> xy/z, others -> xy/w */
        }
        sel[st] = (float)(ci + 16u*(unsigned)pm);
    }
    metal_set_texproj(sel);
}

/* vertices of a DrawPrimitiveUP call with n primitives: D3DPT_POINTLIST 1,
 * LINELIST 2, LINESTRIP 3, TRIANGLELIST 4, TRIANGLESTRIP 5, TRIANGLEFAN 6 */
static uint32_t prim_verts(uint32_t pt, uint32_t n){
    switch (pt){
    case 1: return n;
    case 2: return n * 2u;
    case 3: return n + 1u;
    case 4: return n * 3u;
    default: return n + 2u;
    }
}

/* The renderers draw triangles only: each line segment becomes a quad across
 * its minor axis (the diamond rule covers one pixel per column of an x-major
 * line), from the transformed vertices.  D3D9 lines are one pixel wide, a
 * hairline at today's sizes: these are one HUD pixel (of 640x480) wide, in
 * whole pixels.  The dyno chart draws its curves and rpm marker as
 * one-segment line strips (0x50dcd0): taken for a triangle strip, each got
 * a third vertex from the game's stack, near the middle of the screen. */
static void draw_lines(const TVert *tv, uint32_t nv, uint32_t pt){
    void metal_tex_size(unsigned, unsigned *, unsigned *);
    void metal_set_cull(unsigned);
    unsigned w = g_bb_w, h = g_bb_h;
    if (g_cur_rt) metal_tex_size(g_cur_rt, &w, &h);
    if (!w || !h) return;
    float px = fminf((float)w / 640.f, (float)h / 480.f);
    px = px < 1.f ? 1.f : roundf(px);
    static TVert q[64 * 6];
    unsigned nq = 0;
    uint32_t nseg = (pt == 2u) ? nv / 2u : nv - 1u;
    metal_set_cull(1u);                      /* lines are never culled */
    for (uint32_t s = 0; s < nseg; s++){
        const TVert *a = &tv[(pt == 2u) ? 2u * s : s], *b = a + 1;
        if (a->w <= 0.f || b->w <= 0.f) continue;
        float dx = (b->x / b->w - a->x / a->w) * (float)w;
        float dy = (b->y / b->w - a->y / a->w) * (float)h;
        if (dx == 0.f && dy == 0.f) continue;
        /* half the width either side: NDC spans 2 / size per pixel */
        float ox = 0.f, oy = 0.f;
        if (fabsf(dx) >= fabsf(dy)) oy = px / (float)h; else ox = px / (float)w;
        TVert *o = &q[nq];
        o[0] = *a; o[0].x += ox * a->w; o[0].y += oy * a->w;
        o[1] = *b; o[1].x += ox * b->w; o[1].y += oy * b->w;
        o[2] = *a; o[2].x -= ox * a->w; o[2].y -= oy * a->w;
        o[3] = o[2]; o[4] = o[1];
        o[5] = *b; o[5].x -= ox * b->w; o[5].y -= oy * b->w;
        nq += 6;
        if (nq == sizeof q / sizeof q[0]){ metal_draw_tv(q, nq, 4u, g_cur_tex); nq = 0; }
    }
    if (nq) metal_draw_tv(q, nq, 4u, g_cur_tex);
    metal_set_cull(g_rs[22] ? g_rs[22] : 3u);
}

/* Point lists: each point a square of D3DRS_POINTSIZE pixels (1.0 when unset)
 * around the transformed vertex, in HUD pixels as the lines are, never culled.
 * With D3DRS_POINTSPRITEENABLE the corners get texture coordinates (0,0) top
 * left to (1,1) bottom right in every stage, as D3D9's point sprites do;
 * otherwise each corner keeps the vertex's own. */
static void draw_points(const TVert *tv, uint32_t nv){
    void metal_tex_size(unsigned, unsigned *, unsigned *);
    void metal_set_cull(unsigned);
    unsigned w = g_bb_w, h = g_bb_h;
    if (g_cur_rt) metal_tex_size(g_cur_rt, &w, &h);
    if (!w || !h) return;
    float px = fminf((float)w / 640.f, (float)h / 480.f);
    px = px < 1.f ? 1.f : roundf(px);
    float ps; memcpy(&ps, &g_rs[154], 4);                         /* D3DRS_POINTSIZE */
    if (!(ps > 0.f && ps < 8192.f)) ps = 1.f;
    float hx = ps * px / (float)w, hy = ps * px / (float)h;       /* half the side, in NDC */
    int sprite = g_rs[156] != 0u;                                 /* D3DRS_POINTSPRITEENABLE */
    static TVert q[64 * 6];
    static const float cu[4] = {0.f, 1.f, 0.f, 1.f}, cv[4] = {0.f, 0.f, 1.f, 1.f};
    static const float sx[4] = {-1.f, 1.f, -1.f, 1.f}, sy[4] = {1.f, 1.f, -1.f, -1.f};
    static const int tri[6] = {0, 1, 2, 2, 1, 3};
    unsigned nq = 0;
    metal_set_cull(1u);
    for (uint32_t i = 0; i < nv; i++){
        const TVert *a = &tv[i];
        if (a->w <= 0.f) continue;
        for (int k = 0; k < 6; k++){
            int cn = tri[k];
            TVert *o = &q[nq + (unsigned)k];
            *o = *a;
            o->x += sx[cn] * hx * a->w; o->y += sy[cn] * hy * a->w;
            if (sprite) for (int s = 0; s < 4; s++){ o->t[s][0] = cu[cn]; o->t[s][1] = cv[cn]; }
        }
        nq += 6;
        if (nq == sizeof q / sizeof q[0]){ metal_draw_tv(q, nq, 4u, g_cur_tex); nq = 0; }
    }
    if (nq) metal_draw_tv(q, nq, 4u, g_cur_tex);
    metal_set_cull(g_rs[22] ? g_rs[22] : 3u);
}

/* dip_indexed -> draw_up: this draw's vertices are to be read by index from
 * the copies in the pool (while n > 0).  If draw_up cannot take the GPU path
 * it returns 0 and the caller falls back to the old (index expansion) path. */
static struct { uint32_t n, vbo, ibo, isz; int32_t base; uint64_t gh; } g_ix;
static uint64_t g_dip_ix = 0, g_dip_ixfb[6];
static int draw_up(uint32_t pt, uint32_t nv, uint32_t vp, uint32_t stride){
    uint64_t _t0 = now_ns();
    static TVert tv[4096];
    const VSConst *vsk = &g_vsk;
    if (g_fixmat < 0){ const char *e6 = ENVC("FIXMAT");
        g_fixmat = (e6 && *e6 && *e6 != '0') ? 1 : 0; }
    if (g_gpuvs < 0){ const char *e8 = ENVC("GPUVS");
        g_gpuvs = (e8 && *e8 && *e8 != '0') ? 1 : 0; }
    const VSProg *prog = (g_cur_vs && g_cur_vs < MAXVS) ? g_vsprog[g_cur_vs] : 0;
    vsk = fixmat_apply(vsk);
    g_ui_clip = 1.f;
    vsk = aspect_apply(vsk, prog);
#ifdef __wasm__
    { void metal_set_ui_clip(float); metal_set_ui_clip(g_ui_clip); }
#endif

    /* Declaration parsing ONCE per (declaration, shader).
     * Previously every draw rebuilt it from scratch, reading up to 32
     * elements from guest memory and testing each with mem_is_committed
     * (~1300 times per frame).  Safe: declaration objects are copied when
     * created and never change, and com_alloc does not reuse addresses. */
    static struct { uint32_t decl; const VSProg *prog; int nel, p_off, p_type, c_off, c_type,
                    t_off, t_type, ok; DElem el[16]; } dcc[64];
    uint32_t d = g_cur_decl;
    unsigned dci = (unsigned)(((uintptr_t)prog >> 4) ^ (d >> 3) ^ (d >> 11)) & 63u;
    int dlog = ENVC("DECLLOG") != NULL;
    if (dlog || !dcc[dci].ok || dcc[dci].decl != d || dcc[dci].prog != prog){
    DElem el[16]; int nel = 0;
    int p_off = -1, p_type = 2, c_off = -1, c_type = 4, t_off = -1, t_type = 1;
    uint32_t ep = (d && mem_is_committed(d+8)) ? MR32(d+4) : 0;
    if (ep){
        for (int k = 0; k < 32 && nel < 16; k++){
            uint32_t e = ep + 8u*k;
            if (!mem_is_committed(e+7)) break;
            unsigned st=MR16(e), of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6), ui=MR8(e+7);
            if (st == 0xFFu && ty == 17u) break;
            if (st != 0) continue;                       /* stream 0 only */
            /* DECLLOG=<ps>: print only the declarations drawn with that
             * pixel shader, once, with ALL their elements. */
            { const char *dl = ENVC("DECLLOG");
              if (dl && atoi(dl) == g_cur_ps_id){
                static uint32_t seen[32]; static int sn = 0; int nw = 1;
                for (int q=0;q<sn;q++) if (seen[q]==d) nw = 0;
                if (nw && k == 0 && sn < 32){ seen[sn++] = d;
                    fprintf(stderr,"  [decl] ps=%d decl=0x%08x stride=%u vs=%u\n",
                            g_cur_ps_id, d, stride, g_cur_vs);
                    for (int q2 = 0; q2 < 24; q2++){
                        uint32_t e2 = ep + 8u*q2;
                        if (!mem_is_committed(e2+7)) break;
                        unsigned s2=MR16(e2), o2=MR16(e2+2), t2=MR8(e2+4),
                                 u2=MR8(e2+6), i2=MR8(e2+7);
                        if (s2 == 0xFFu && t2 == 17u) break;
                        int rg2 = prog ? vs_input_reg(prog,(int)u2,(int)i2) : -2;
                        fprintf(stderr,"       stream=%u offset=%2u type=%u usage=%u/%u -> v%d\n",
                                s2,o2,t2,u2,i2,rg2);
                    }
                }
              } }
            if (us == 0 && ui == 0){ p_off = (int)of; p_type = (int)ty; }
            else if (us == 10 && ui == 0){ c_off = (int)of; c_type = (int)ty; }
            else if (us == 5 && ui == 0){ t_off = (int)of; t_type = (int)ty; }
            if (prog){
                int rg = vs_input_reg(prog, (int)us, (int)ui);
                if (rg >= 0){ el[nel].off=(uint16_t)of; el[nel].type=(uint8_t)ty;
                              el[nel].reg=(int8_t)rg; nel++; }
            }
        }
    }
    /* without a declaration, NFSU2's standard layout: pos@0 color@12 uv@16 */
    if (p_off < 0){ p_off = 0;  p_type = 2; }
    if (c_off < 0){ c_off = 12; c_type = 4; }
    if (t_off < 0){ t_off = 16; t_type = 1; }
    dcc[dci].decl = d; dcc[dci].prog = prog; dcc[dci].nel = nel;
    dcc[dci].p_off = p_off; dcc[dci].p_type = p_type; dcc[dci].c_off = c_off;
    dcc[dci].c_type = c_type; dcc[dci].t_off = t_off; dcc[dci].t_type = t_type;
    memcpy(dcc[dci].el, el, sizeof el); dcc[dci].ok = 1;
    }
    const DElem *el = dcc[dci].el; int nel = dcc[dci].nel;
    int p_off = dcc[dci].p_off, p_type = dcc[dci].p_type, c_off = dcc[dci].c_off,
        c_type = dcc[dci].c_type, t_off = dcc[dci].t_off, t_type = dcc[dci].t_type;

    /* ---------------- GPU vertex shader (GPUVS) ----------------
     * Measured: ~45% of the run time was in the CPU vs_run interpreter,
     * ceiling ~3.5 M vertices/s.  The game's own bytecode is translated to
     * MSL and run on the GPU; vertices are sent RAW, and the fetch layout is
     * taken from the D3D declaration and baked into the shader.
     * A shader that cannot be translated (flow control etc.) does not run
     * silently wrong: vs_to_msl returns 0 and we fall back to the proven CPU path.
     * Lines take the CPU path: draw_lines widens them from the transformed vertices. */
    int lines = (pt == 2u || pt == 3u);
    if (prog && nel && stride && stride <= 256u && g_gpuvs && !lines && pt != 1u){   /* points too: draw_points */
        int gid = gpuvs_id(prog, g_cur_vs, g_cur_decl, el, nel);
        if (gid > 0){
            int  metal_draw_raw(const void*, unsigned, unsigned, unsigned,
                                unsigned, int, const float*, unsigned);
            void metal_set_stage_tex(unsigned,unsigned,unsigned);
            void metal_set_ps(int, const float *);
            metal_set_stage_tex(g_tex_stage[1], g_tex_stage[2], g_tex_stage[3]);
            metal_set_ps(g_cur_ps_id, &g_psc[0][0]);
            texproj_push(); smp_push();
            if (g_ix.n){
                int metal_draw_ix(unsigned, unsigned, unsigned, unsigned, unsigned, int,
                                  unsigned, int, const float *, unsigned, unsigned long long);
                if (metal_draw_ix(g_ix.vbo, stride, g_ix.ibo, g_ix.isz, g_ix.n, g_ix.base,
                                  g_cur_tex, gid, &vsk->f[0][0], (unsigned)g_gvs_nk[gid], g_ix.gh)){
                    g_vs_draws++; g_gpuvs_draws++; g_v_xform += nv;
                    g_t_draw_ns += now_ns() - _t0;
                    return 1;
                }
                return 0;
            }
            if (metal_draw_raw(g_mem + vp, nv, stride, pt, g_cur_tex, gid,
                               &vsk->f[0][0], (unsigned)g_gvs_nk[gid])){
                g_vs_draws++; g_gpuvs_draws++; g_v_xform += nv;
                g_t_draw_ns += now_ns() - _t0;
                return 1;
            }
            /* Could not be queued (arena overflow): let the CPU path carry on --
             * silently dropping it made geometry disappear. */
            g_gpuvs_drop++;
        }
    }
    if (g_ix.n) return 0;           /* an indexed source cannot go to the CPU path */

    for (uint32_t i = 0; i < nv; i++){
        uint32_t o = vp + i*stride;
        TVert *out = &tv[i];
        if (prog && nel){
            /* DIAGNOSTIC (FIXMAT=1): the game's WVP matrix arrives as diag(0,0,1,1) --
             * X/Y scale zero.  While the root cause is hunted, put in a sensible
             * orthographic matrix so the rest of the pipeline can be seen. */
            /* Is the matrix sane per draw?  If some draws arrive with a correct
             * matrix, the front end is using two different spaces. */
            { int degenerate = (vsk->f[3][3] == 0.f && vsk->f[3][0] == 0.f &&
                                vsk->f[0][0] == 0.f && vsk->f[1][1] == 0.f);
              if (degenerate){ g_mat_bad++;
                  static uint32_t seen[8]; static int ns=0;
                  int f4=0; for(int q=0;q<ns;q++) if(seen[q]==g_cur_vs) f4=1;
                  if(!f4 && ns<8 && ENVC("MATLOG")){ seen[ns++]=g_cur_vs;
                      fprintf(stderr,"  [matrix-BAD] shader=%u tex=%u last upload sr=%u n=%u pd=%08x\n",
                              g_cur_vs, g_cur_tex,
                              g_scn?g_sc[(g_scn-1)&7].sr:0,
                              g_scn?g_sc[(g_scn-1)&7].n:0,
                              g_scn?g_sc[(g_scn-1)&7].pd:0); } }
              else {
                  g_mat_ok++;
                  static uint32_t seenok[8]; static int nsok=0;
                  int f5=0; for(int q=0;q<nsok;q++) if(seenok[q]==g_cur_vs) f5=1;
                  if(!f5 && nsok<8 && ENVC("MATLOG")){ seenok[nsok++]=g_cur_vs;
                      fprintf(stderr,"  [matrix-VALID] shader=%u tex=%u pd=%08x\n",
                              g_cur_vs, g_cur_tex, g_scn?g_sc[(g_scn-1)&7].pd:0); }
                  static int shown = 0;
                  if (0){ shown++;
                      fprintf(stderr,"  [matrix-REAL] draw %llu:\n",
                              (unsigned long long)g_vs_draws);
                      for (int rw=0; rw<4; rw++)
                          fprintf(stderr,"      %9.4f %9.4f %9.4f %9.4f\n",
                                  vsk->f[rw][0], vsk->f[rw][1],
                                  vsk->f[rw][2], vsk->f[rw][3]);
                  }
              } }
            float v[16][4];
            for (int r2 = 0; r2 < 16; r2++){ v[r2][0]=v[r2][1]=v[r2][2]=0.f; v[r2][3]=1.f; }
            for (int k = 0; k < nel; k++)
                decl_fetch(o + el[k].off, el[k].type, v[el[k].reg]);
            VSOut so;
            /* NOVS=1: MEASUREMENT only.  Skips the vertex shader interpreter
             * to see whether this really is the bottleneck. */
            static int novs = -1;
            if (novs < 0){ const char *e7 = ENVC("NOVS"); novs = (e7 && *e7 && *e7!='0'); }
            if (novs){ memset(&so, 0, sizeof so);
                       so.pos[0]=v[0][0]; so.pos[1]=v[0][1];
                       so.pos[2]=v[0][2]; so.pos[3]=1.f;
                       so.col[0][0]=so.col[0][1]=so.col[0][2]=so.col[0][3]=1.f; }
            else { uint64_t _v0 = now_ns();
                   vs_run(prog, vsk, (const float (*)[4])v, &so);
                   g_t_vs_ns += now_ns() - _v0; }
            out->x=so.pos[0]; out->y=so.pos[1]; out->z=so.pos[2]; out->w=so.pos[3];
            out->r=so.col[0][0]; out->g=so.col[0][1];
            out->b=so.col[0][2]; out->a=so.col[0][3];
            out->r1=so.col[1][0]; out->g1=so.col[1][1];
            out->b1=so.col[1][2]; out->a1=so.col[1][3];
            /* On the GPU path the vertex shader makes this selection; the CPU
             * path must apply the SAME permutation or the two paths diverge. */
            for (int ts = 0; ts < 4; ts++){
                unsigned ci = g_tss_raw[ts][11] & 0xFFFFu;
                if (ci > 3u) ci = (unsigned)ts;
                for (int tc = 0; tc < 4; tc++) out->t[ts][tc] = so.tex[ci][tc];
            }
        } else {
            float pos[4], col[4], uv[4];
            decl_fetch(o + (unsigned)p_off, (unsigned)p_type, pos);
            decl_fetch(o + (unsigned)c_off, (unsigned)c_type, col);
            decl_fetch(o + (unsigned)t_off, (unsigned)t_type, uv);
            out->x=pos[0]; out->y=pos[1]; out->z=pos[2];
            out->w=(p_type==3)?pos[3]:1.f;
            out->r=col[0]; out->g=col[1]; out->b=col[2]; out->a=col[3];
            out->r1=0.f; out->g1=0.f; out->b1=0.f; out->a1=0.f;
            for (int ts = 0; ts < 4; ts++){
                out->t[ts][0]=uv[0]; out->t[ts][1]=uv[1];
                out->t[ts][2]=0.f;   out->t[ts][3]=1.f;
            }
        }
    }
    if (prog && nel){
        g_vs_draws++;
        /* RAW position range of shader draws: shows which space the UI is
         * in and what the matrix has to do. */
        for (uint32_t i3 = 0; i3 < nv; i3++){
            uint32_t o3 = vp + i3*stride;
            if (!mem_is_committed(o3 + (unsigned)p_off + 11)) break;
            float rx = MRF32(o3 + (unsigned)p_off);
            float ry = MRF32(o3 + (unsigned)p_off + 4);
            float rz = MRF32(o3 + (unsigned)p_off + 8);
            if (rx < g_rmin[0]) g_rmin[0] = rx;  if (rx > g_rmax[0]) g_rmax[0] = rx;
            if (ry < g_rmin[1]) g_rmin[1] = ry;  if (ry > g_rmax[1]) g_rmax[1] = ry;
            if (rz < g_rmin[2]) g_rmin[2] = rz;  if (rz > g_rmax[2]) g_rmax[2] = rz;
        }
    } else g_ff_draws++;
    /* Are the transformed vertices on screen?  Go from clip space to NDC and count. */
    { for (uint32_t i2 = 0; i2 < nv; i2++){
        float w2 = tv[i2].w;
        if (w2 <= 0.f){ g_v_badw++; continue; }
        float nx = tv[i2].x / w2, ny = tv[i2].y / w2;
        if (nx >= -1.f && nx <= 1.f && ny >= -1.f && ny <= 1.f) g_v_in++;
        else g_v_out++;
      } }
    { void metal_set_stage_tex(unsigned,unsigned,unsigned);
      void metal_set_ps(int, const float *);
      metal_set_stage_tex(g_tex_stage[1], g_tex_stage[2], g_tex_stage[3]);
      metal_set_ps(g_cur_ps_id, &g_psc[0][0]);
      texproj_push(); smp_push();
      if (ENVC("PSC0")){ static int n5=0;
          if (n5<6 && g_tex_stage[1]){ n5++;
              fprintf(stderr,"  [ps] ps=%d c0=(%.3f %.3f %.3f %.3f) tex=%u/%u/%u/%u\n",
                      g_cur_ps_id,
                      (double)g_psc[0][0],(double)g_psc[0][1],
                      (double)g_psc[0][2],(double)g_psc[0][3],
                      g_tex_stage[0], g_tex_stage[1],
                      g_tex_stage[2], g_tex_stage[3]); } } }
    { int nstage=0; for(int q=0;q<4;q++) if (g_tex_stage[q]) nstage++;
      if (nstage>0 && nstage<=4) g_stage_hist[nstage-1]++;
      static int shown=0;
      if (ENVC("STAGEDUMP") && nstage>=2 && shown<8){ shown++;
          fprintf(stderr,"  [stage] draw: tex0=%u tex1=%u tex2=%u tex3=%u  verts=%u\n",
                  g_tex_stage[0],g_tex_stage[1],g_tex_stage[2],g_tex_stage[3], nv); } }
    g_t_draw_ns += now_ns() - _t0; g_v_xform += nv;
    if (prog && g_cur_vs < 64u) g_v_per_vs[g_cur_vs] += nv;
    if (dd_on()){
        static int nd = 0;
        if (nd++ < 6){
            fprintf(stderr,"   >>> draw type=%u verts=%u stride=%u tex=%u shader=%u elements=%d\n",
                    pt, nv, stride, g_cur_tex, g_cur_vs, nel);
            if (prog) vs_disasm(prog, stderr);
            for (int k=0;k<nel;k++)
                fprintf(stderr,"      element: offset=%u type=%u -> v%d\n",
                        el[k].off, el[k].type, el[k].reg);
            for (uint32_t i=0;i<nv && i<3;i++){
                uint32_t o = vp + i*stride;
                if (prog && nel){
                    float v[16][4];
                    for (int r2=0;r2<16;r2++){ v[r2][0]=v[r2][1]=v[r2][2]=0.f; v[r2][3]=1.f; }
                    for (int k=0;k<nel;k++) decl_fetch(o+el[k].off, el[k].type, v[el[k].reg]);
                    for (int k=0;k<nel;k++)
                        fprintf(stderr,"        v%d = %g %g %g %g\n", el[k].reg,
                                v[el[k].reg][0],v[el[k].reg][1],v[el[k].reg][2],v[el[k].reg][3]);
                }
                fprintf(stderr,"        -> pos=(%g %g %g %g) colour=(%.3g %.3g %.3g %.3g) uv=(%g %g)\n",
                        tv[i].x,tv[i].y,tv[i].z,tv[i].w,
                        tv[i].r,tv[i].g,tv[i].b,tv[i].a, tv[i].t[0][0],tv[i].t[0][1]);
            }
            fprintf(stderr,"      last constant uploads (newest first):\n");
            for (int q=1; q<=4 && q<=(int)g_scn; q++){
                unsigned ix = (g_scn - q) & 7;
                fprintf(stderr,"        sr=%-3u n=%-3u pd=%08x", g_sc[ix].sr, g_sc[ix].n, g_sc[ix].pd);
                if (g_sc[ix].pd && mem_is_committed(g_sc[ix].pd+63))
                    fprintf(stderr,"  first4=[%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g]",
                        MRF32(g_sc[ix].pd), MRF32(g_sc[ix].pd+4),
                        MRF32(g_sc[ix].pd+8), MRF32(g_sc[ix].pd+12),
                        MRF32(g_sc[ix].pd+16), MRF32(g_sc[ix].pd+20),
                        MRF32(g_sc[ix].pd+24), MRF32(g_sc[ix].pd+28));
                fprintf(stderr,"\n");
            }
            /* c0..c7 in use */
            for (int q=0;q<8;q++)
                fprintf(stderr,"        c%d = %g %g %g %g\n", q,
                        g_vsk.f[q][0],g_vsk.f[q][1],g_vsk.f[q][2],g_vsk.f[q][3]);
        }
    }
    if (lines) draw_lines(tv, nv, pt);
    else if (pt == 1u) draw_points(tv, nv);
    else metal_draw_tv(tv, nv, pt, g_cur_tex);
    return 1;
}

/* DrawIndexedPrimitive fast path: a static VB/IB is mirrored into the pool and
 * drawn indexed (see metal.m PERSISTENT VERTEX / INDEX POOL).  0 = not applicable,
 * the caller falls back to the index expansion path. */
static int dip_indexed(uint32_t pt, uint32_t basev, uint32_t starti, uint32_t ntri, uint32_t isz){
    int metal_mirror(uint32_t, uint32_t, const void *, uint32_t, unsigned, uint32_t *, uint32_t *);
    const uint8_t *metal_pool_cpu(void);
    static int off = -1;             /* NOIX=1: off (A/B measurement, safety) */
    if (off < 0){ const char *e = ENVC("NOIX"); off = (e && *e && *e != '0'); }
    if (off) return 0;
    if (pt != 4u || !g_gpuvs){ g_dip_ixfb[0]++; return 0; }         /* strip/fan: old path */
    uint32_t vb = g_ss_vb, ib = g_ib_obj, stride = g_ss_stride, so = g_ss_off;
    if (!mem_is_committed(vb+63) || !mem_is_committed(ib+63)){ g_dip_ixfb[1]++; return 0; }
    uint32_t vsz = MR32(vb+4), isb = MR32(ib+4);
    /* locked in the last 4 frames, so its contents may be changing: rather than
     * re-mirror every frame, take the old path (dynamic buffers always go there) */
    if ((uint32_t)g_present - MR32(vb+52) < 4u || (uint32_t)g_present - MR32(ib+52) < 4u){
        g_dip_ixfb[2]++; return 0; }
    uint32_t ni = ntri * 3u;
    const uint8_t *vbp = vsz ? bsrc_at(buf_src(vb, 0), 0, vsz) : NULL;
    const uint8_t *ibp = isb ? bsrc_at(buf_src(ib, 0), 0, isb) : NULL;
    if ((stride & 3u) || (so & 3u) || (int32_t)basev < 0 || !vbp || !ibp ||
        (uint64_t)(starti + ni) * isz > isb){
        g_dip_ixfb[3]++; return 0; }
    /* A Metal index offset must be a multiple of 4; for an odd start with
     * 16-bit indices a second mirror of the IB, shifted by 2 bytes, is used
     * (key ib+2: com_alloc aligns to 16, so it collides with no other object).
     * Measured: without this 19% of DIP draws fell back to the old path. */
    uint32_t sh = ((starti * isz) & 3u) ? 2u : 0u;
    if (sh && isz != 2u){ g_dip_ixfb[3]++; return 0; }
    uint32_t voff, ioff, imax;
    if (!metal_mirror(vb, MR32(vb+48), vbp, vsz, 0, &voff, NULL) ||
        !metal_mirror(ib + sh, MR32(ib+48), ibp + sh, isb - sh, isz, &ioff, &imax)){
        g_dip_ixfb[4]++; return 0; }
    /* Bound: if the largest index of the whole IB fits this VB, no scan;
     * if not (one IB serves several VBs), scan the draw's range. */
    if ((uint64_t)so + ((uint64_t)basev + imax + 1u) * stride > vsz){
        const uint8_t *P = metal_pool_cpu() + ioff + (size_t)starti * isz - sh;
        uint32_t mx = 0;
        if (isz == 2u){ const uint16_t *x = (const uint16_t *)P; for (uint32_t k = 0; k < ni; k++) if (x[k] > mx) mx = x[k]; }
        else          { const uint32_t *x = (const uint32_t *)P; for (uint32_t k = 0; k < ni; k++) if (x[k] > mx) mx = x[k]; }
        if ((uint64_t)so + ((uint64_t)basev + mx + 1u) * stride > vsz){ g_dip_ixfb[5]++; return 0; }
    }
    /* geometry identity (HD ray tracing cache): same mirror + same range =
     * same geometry; when the generation changes, so does the identity */
    uint64_t gh = 1469598103934665603ull;
    uint32_t kv[9] = { vb, MR32(vb+48), ib, MR32(ib+48), starti, ni, basev, stride, so };
    for (int q = 0; q < 9; q++){ gh = (gh ^ kv[q]) * 1099511628211ull; gh ^= gh >> 29; }
    g_ix.n = ni; g_ix.vbo = voff + so; g_ix.ibo = ioff + starti * isz - sh; g_ix.isz = isz;
    g_ix.base = (int32_t)basev; g_ix.gh = gh | 1ull;
    int ok = draw_up(4u, ni, 0u, stride);
    g_ix.n = 0;
    if (ok) g_dip_ix++;
    return ok;
}

/* DrawPrimitive, DrawIndexedPrimitiveUP and the indexed point and line lists:
 * nprim primitives of type pt, vertex k of the run at vb + idx(k) * stride
 * (idx(k) = k without indices; ip/isz: the index data, base added to each).
 * The run goes out as lists (D3D9's order: a strip's odd triangle is
 * (k+1, k, k+2), a fan's (0, k+1, k+2), a line strip's segment (k, k+1)),
 * copied through the index scratch in chunks draw_up takes.  0 = a vertex or
 * index outside committed memory or past a host copy (nothing more is drawn).
 * The vertices and indices are guest memory or a buffer's host copy (buf_src). */
static uint64_t g_prim_runs[8], g_prim_bad = 0;
static int draw_prims(uint32_t pt, uint32_t nprim, BSrc vb, uint32_t stride,
                      BSrc ip, uint32_t isz, int32_t base){
    if (pt < 1u || pt > 6u || !nprim || !stride || stride > 256u) return 1;
    int indexed = ip.p || ip.ga;
    g_prim_runs[pt]++;
    if (!g_idx_scratch) g_idx_scratch = gbuf_alloc(4096u*64u);
    if (!g_idx_scratch) return 0;
    uint32_t lt = pt == 1u ? 1u : pt <= 3u ? 2u : 4u;            /* the list type it goes out as */
    uint32_t per = lt == 1u ? 1u : lt == 2u ? 2u : 3u;           /* vertices per primitive */
    uint32_t cap = (4096u * 64u) / stride; if (cap > 4095u) cap = 4095u;
    uint32_t chunk = cap / per;                                   /* primitives per draw_up */
    for (uint32_t done = 0; done < nprim; ){
        uint32_t n = nprim - done < chunk ? nprim - done : chunk;
        for (uint32_t p = 0; p < n; p++){
            uint32_t g = done + p, k[3];
            switch (pt){
            case 1: k[0] = g; break;
            case 2: k[0] = 2u*g; k[1] = 2u*g + 1u; break;
            case 3: k[0] = g; k[1] = g + 1u; break;
            case 4: k[0] = 3u*g; k[1] = 3u*g + 1u; k[2] = 3u*g + 2u; break;
            case 5: if (g & 1u){ k[0] = g + 1u; k[1] = g; } else { k[0] = g; k[1] = g + 1u; } k[2] = g + 2u; break;
            default: k[0] = 0u; k[1] = g + 1u; k[2] = g + 2u; break;
            }
            for (uint32_t q = 0; q < per; q++){
                uint32_t vi = k[q];
                if (indexed){
                    const uint8_t *ia = bsrc_at(ip, vi * isz, isz);
                    if (!ia){ g_prim_bad++; return 0; }
                    vi = bsrc_index(ia, isz) + (uint32_t)base;
                }
                uint32_t dst = g_idx_scratch + (p * per + q) * stride;
                const uint8_t *src = bsrc_at(vb, vi * stride, stride);
                if (!src){ g_prim_bad++; return 0; }
                memcpy(g_mem + dst, src, stride);
            }
        }
        draw_up(lt, n * per, g_idx_scratch, stride);
        done += n;
    }
    return 1;
}

/* ---- D3DX HLE stage 2: call recording and no-op check (dxrec.h) --------
 * D3DX's state assignment (sub_00669286) issues ~36 thousand device calls
 * per frame, and 96% of them rewrite the value the device already has.
 * Here we check whether the values a handler would write if the call were
 * made NOW equal the current state, by repeating the handler's own logic.
 * Only the 8 methods below are supported; any other device call
 * invalidates the record (nothing is skipped).                           */
#include "dxrec.h"
#define DXREC_DEPTH 8
static DxRec *g_dxrec[DXREC_DEPTH];
int    g_dxrec_depth = 0;
const char *dxrec_mid_name(int mid){ return (mid > 0 && mid < MID__count) ? g_midname[mid] : "?"; }
int dxrec_sb_recording(void){ return g_sb_rec != 0 || g_sb_replay != 0; }
void dxrec_begin(DxRec *r, int kind){
    int in_state = 0, same = 0;
    for (int i = 0; i < g_dxrec_depth && i < DXREC_DEPTH; i++){
        if (g_dxrec[i]->kind == DXREC_STATE) in_state = 1;
        if (g_dxrec[i]->kind == kind) same = 1;
    }
    if (in_state || (same && kind == DXREC_PASS))
        for (int i = 0; i < g_dxrec_depth && i < DXREC_DEPTH; i++) g_dxrec[i]->ok = 0;
    r->kind = (uint8_t)kind; r->ok = 1; r->ncall = 0; r->ndata = 0; r->allred = 1;
    if (g_dxrec_depth < DXREC_DEPTH) g_dxrec[g_dxrec_depth] = r; else r->ok = 0;
    g_dxrec_depth++;
}
void dxrec_append(const DxRec *src){
    int lim = g_dxrec_depth < DXREC_DEPTH ? g_dxrec_depth : DXREC_DEPTH;
    for (int i = 0; i < lim; i++){
        DxRec *r = g_dxrec[i];
        if (!r->ok) continue;
        if (!src->ok || r->ncall + src->ncall > r->maxcall
            || r->ndata + src->ndata > r->maxdata){ r->ok = 0; continue; }
        for (int k = 0; k < src->ncall; k++){
            DxCall *d = &r->call[r->ncall++];
            *d = src->call[k];
            d->doff = (uint16_t)(src->call[k].doff + r->ndata);
        }
        memcpy(r->data + r->ndata, src->data, src->ndata);
        r->ndata = (uint16_t)(r->ndata + src->ndata);
    }
}
void dxrec_end(void){ if (g_dxrec_depth > 0) g_dxrec_depth--; }

/* Number of bytes to copy/compare for the constant methods that take a data
 * pointer (same bounds as the handler's). */
static uint32_t dxrec_datalen(int mid, const uint32_t *a){
    if (MID(SetVertexShaderConstantF)){ uint32_t n = a[2]; if (a[0] >= 256u) return 0;
        if (a[0] + n > 256u) n = 256u - a[0]; return n*16u; }
    if (MID(SetPixelShaderConstantF)){ uint32_t n = a[2]; if (a[0] >= 32u) return 0;
        if (a[0] + n > 32u) n = 32u - a[0]; return n*16u; }
    return 0;
}
/* a[]: the arguments after this; d: constant data (dlen bytes). */
static int dxrec_call_redundant(int mid, const uint32_t *a, const uint8_t *d, uint32_t dlen){
    if (MID(SetRenderState))
        return a[0] >= 256u || (g_rs[a[0]] == a[1] && !g_rs_dirty);
    if (MID(SetSamplerState))
        return !(a[0] < 16u && a[1] < 20u) || g_smp_raw[a[0]][a[1]] == a[2];
    if (MID(SetTextureStageState))
        return !(a[0] < 8u && a[1] < 40u) || g_tss_raw[a[0]][a[1]] == a[2];
    if (MID(SetTexture)){
        uint32_t stg = a[0], to = a[1];
        if (stg >= 8u) return 1;
        uint32_t der = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
        if (g_raw_tex[stg] != to) return 0;
        if (stg < 4u && g_tex_stage[stg] != der) return 0;
        if (stg == 0 && g_cur_tex != der) return 0;
        return 1;
    }
    if (MID(SetVertexShader)){
        uint32_t h = a[0];
        unsigned der = (h && mem_is_committed(h+15)) ? MR32(h+12) : 0;
        if (der >= MAXVS) der = 0;
        return g_raw_vs == h && g_cur_vs == der;
    }
    if (MID(SetPixelShader)){
        uint32_t h = a[0];
        int der = (h && mem_is_committed(h+15)) ? (int)MR32(h+12) : 0;
        return g_raw_ps == h && g_cur_ps_id == der;
    }
    if (MID(SetVertexShaderConstantF))
        return dlen == 0 || !memcmp(g_vsk.f[a[0]], d, dlen);
    if (MID(SetPixelShaderConstantF))
        return dlen == 0 || !memcmp(g_psc[a[0]], d, dlen);
    return 0;
}
/* D3DX native path (d3dx_hle.c, stage 4): would this device call be a no-op
 * if made NOW?  If so the caller skips it entirely (saves the guest dispatch
 * + com_call).  During state block recording/replay, an open dxrec record or
 * an FXTRACE frame the answer is NO: the call must really be made so it
 * lands in the record/trace.
 * kind: 1 SetRenderState(a0, a1), 2 SetTextureStageState(a0, a1, a2),
 *       3 SetSamplerState(a0, a1, a2), 4 SetTexture(a0, a1),
 *       5/6 Set{Vertex,Pixel}ShaderConstantF(a0, a1=data, a2), 7/8 Set{Vertex,Pixel}Shader(a0). */
int fxtr_now(void); extern int g_fxtr;
int dxn_skip(int kind, uint32_t a0, uint32_t a1, uint32_t a2){
    if (g_sb_rec || g_sb_replay || g_dxrec_depth) return 0;
    if (g_fxtr && fxtr_now()) return 0;
    uint32_t a[3] = { a0, a1, a2 };
    int mid = kind == 1 ? MID_SetRenderState : kind == 2 ? MID_SetTextureStageState
            : kind == 3 ? MID_SetSamplerState : kind == 4 ? MID_SetTexture
            : kind == 5 ? MID_SetVertexShaderConstantF : kind == 6 ? MID_SetPixelShaderConstantF
            : kind == 7 ? MID_SetVertexShader : kind == 8 ? MID_SetPixelShader : 0;
    if (!mid) return 0;
    if (kind == 5 || kind == 6){                 /* a1: guest data pointer */
        uint32_t dlen = dxrec_datalen(mid, a);
        if (!dlen) return 0;
        if (!a1 || !mem_is_committed(a1) || !mem_is_committed(a1 + dlen - 1u)) return 0;
        return dxrec_call_redundant(mid, a, g_mem + a1, dlen);
    }
    return dxrec_call_redundant(mid, a, NULL, 0);
}
static int dxrec_supported(int mid){
    return MID(SetRenderState) || MID(SetSamplerState) || MID(SetTextureStageState)
        || MID(SetTexture) || MID(SetVertexShader) || MID(SetPixelShader)
        || MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF);
}
/* From com_call, BEFORE the handler: append to every open record. */
static void dxrec_note(CPU *c, int iface, int mid, int nargs){
    /* Reference count calls that are no-ops for us (texture AddRef/Release) do not spoil the record. */
    if ((MID(AddRef) || MID(Release)) && iface != IF_STATEBLOCK
        && iface != IF_DS && iface != IF_DSBUF) return;
    int okc = iface == IF_DEV && dxrec_supported(mid) && !dxrec_sb_recording()
              && nargs >= 2;
    uint32_t a[3] = { 0, 0, 0 };
    for (int i = 0; i < 3 && i + 1 < nargs; i++) a[i] = A(c, i+1);
    uint32_t dlen = okc ? dxrec_datalen(mid, a) : 0;
    const uint8_t *dp = NULL;
    if (dlen){
        uint32_t pd = a[1];
        if (!pd || !mem_is_committed(pd) || !mem_is_committed(pd+dlen-1u)) okc = 0;
        else dp = g_mem + pd;
    }
    int red = okc && dxrec_call_redundant(mid, a, dp, dlen);
    int lim = g_dxrec_depth < DXREC_DEPTH ? g_dxrec_depth : DXREC_DEPTH;
    for (int i = 0; i < lim; i++){
        DxRec *r = g_dxrec[i];
        if (!r->ok) continue;
        if (!okc || r->ncall >= r->maxcall || r->ndata + dlen > r->maxdata){ r->ok = 0; continue; }
        DxCall *k = &r->call[r->ncall++];
        k->mid = (int16_t)mid; k->a[0] = a[0]; k->a[1] = a[1]; k->a[2] = a[2];
        k->doff = r->ndata; k->dlen = (uint16_t)dlen; k->red = (uint8_t)red;
        if (dlen){ memcpy(r->data + r->ndata, dp, dlen); r->ndata += dlen; }
        if (!red) r->allred = 0;
    }
}
int dxrec_redundant(const DxRec *r){
    if (!r->ok || dxrec_sb_recording()) return 0;
    for (int i = 0; i < r->ncall; i++){
        const DxCall *k = &r->call[i];
        if (!dxrec_call_redundant(k->mid, k->a, r->data + k->doff, k->dlen)) return 0;
    }
    return 1;
}

/* COMSTAT=<frame>: IDirect3DDevice9 method counts, whether they came from
 * INSIDE or OUTSIDE D3DX BeginPass / CommitChanges, and the REDUNDANT share
 * (same as the device's last value).  For D3DX HLE stage 2; use together
 * with D3DXSTAT (that wrapper keeps the BeginPass depth). */
extern int g_dx_in_pass;
static int g_cs_on = -1; static long g_cs_from = 0, g_cs_f0 = -1;
static uint64_t cs_n[MID__count][2], cs_red[MID__count][2];
typedef struct { uint32_t k0, k1; uint64_t v; } CsSlot;
#define CS_CAP (1u<<15)
static CsSlot *cs_tab;
static void comstat(CPU *c, int mid, int nargs){
    long metal_frames(void);
    long f = metal_frames();
    if (f < g_cs_from || mid <= 0) return;
    if (g_cs_f0 < 0) g_cs_f0 = f;
    if (!cs_tab) cs_tab = calloc(CS_CAP, sizeof *cs_tab);
    int in = g_dx_in_pass > 0;
    cs_n[mid][in]++;
    const char *nm = g_midname[mid];
    if (nm[0] != 'S' || nm[1] != 'e' || nm[2] != 't') return;
    int nkey = (MID(SetSamplerState) || MID(SetTextureStageState)) ? 2 : (nargs >= 3 ? 1 : 0);
    uint32_t k1 = 0; for (int i = 0; i < nkey; i++) k1 = k1*0x10001u + A(c, 1+i);
    uint64_t v = 0xcbf29ce484222325ull;
    for (int i = 1 + nkey; i < nargs; i++){ v ^= A(c,i); v *= 0x100000001b3ull; }
    uint32_t dp = 0, db = 0;
    if (MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)
        || MID(SetVertexShaderConstantI)){ dp = A(c,2); db = A(c,3)*16u; }
    else if (MID(SetTransform) || MID(SetMaterial)){ dp = A(c, nargs-1); db = 64u; }
    if (dp && db <= 4096u && mem_is_committed(dp) && mem_is_committed(dp+db-1u))
        for (uint32_t i = 0; i < db; i++){ v ^= MR8(dp+i); v *= 0x100000001b3ull; }
    uint32_t h = ((uint32_t)mid * 0x9E3779B1u) ^ (k1 * 0x85EBCA77u);
    for (uint32_t j = 0; j < CS_CAP; j++){
        CsSlot *t = &cs_tab[(h + j) & (CS_CAP-1)];
        if (t->k0 == 0){ t->k0 = (uint32_t)mid; t->k1 = k1; t->v = v; return; }
        if (t->k0 == (uint32_t)mid && t->k1 == k1){
            if (t->v == v) cs_red[mid][in]++;
            t->v = v; return; }
    }
}
void comstat_report(void){
    if (g_cs_f0 < 0) return;
    long metal_frames(void);
    double fr = (double)(metal_frames() - g_cs_f0); if (fr < 1) fr = 1;
    fprintf(stderr, "\n=== COMSTAT (%.0f frames): IDirect3DDevice9 calls/frame ===\n", fr);
    fprintf(stderr, "  %-28s %10s %10s %9s %10s %9s\n", "method", "BeginPass", "redundant",
            "", "outside", "redundant");
    for (int pass = 0; pass < 40; pass++){
        int best = -1; uint64_t bv = 0;
        for (int i = 1; i < MID__count; i++){
            uint64_t t = cs_n[i][0] + cs_n[i][1];
            if (t > bv){ bv = t; best = i; } }
        if (best < 0 || bv < (uint64_t)fr) break;
        int i = best;
        fprintf(stderr, "  %-28s %10.1f %9.1f%% %9s %10.1f %8.1f%%\n", g_midname[i],
            cs_n[i][1]/fr, cs_n[i][1] ? 100.0*cs_red[i][1]/cs_n[i][1] : 0.0, "",
            cs_n[i][0]/fr, cs_n[i][0] ? 100.0*cs_red[i][0]/cs_n[i][0] : 0.0);
        cs_n[i][0] = cs_n[i][1] = 0;
    }
}

/* FXTRACE=<frame>[,<frame>...]: in those frames print every device call
 * (arguments, constant/matrix data) and the effect method markers from
 * d3dx_hle.c, in order.  Behaviour reference so the native ID3DXEffect
 * produces the same device state as D3DX (D3DX HLE stage 4). */
int g_fxtr = 0;
static uint32_t g_fxtr_fr[8]; static int g_fxtr_nf;
__attribute__((constructor)) static void fxtr_init(void){
    const char *e = getenv("FXTRACE");
    for (; e && *e && g_fxtr_nf < 8; ){
        g_fxtr_fr[g_fxtr_nf++] = (uint32_t)strtoul(e, (char**)&e, 10);
        if (*e == ',') e++; else break;
    }
    g_fxtr = g_fxtr_nf > 0;
}
int fxtr_now(void){
    for (int i = 0; i < g_fxtr_nf; i++) if (g_fxtr_fr[i] == g_present_no) return 1;
    return 0;
}
static void fxtr_floats(uint32_t p, uint32_t n){
    for (uint32_t i = 0; i < n; i++){
        float f; uint32_t u = (p && mem_is_committed(p+4u*i)) ? MR32(p+4u*i) : 0u;
        memcpy(&f, &u, 4); fprintf(stderr, "%s%g", (i&3) ? "," : (i ? " | " : " "), f);
    }
}
static void fxtr_dev(CPU *c, int iface, int mid, int nargs, const char *nm){
    if (iface != IF_DEV){
        if (iface == IF_STATEBLOCK || ((MID(AddRef) || MID(Release)) && iface == IF_TEX))
            fprintf(stderr, "D  [%d]%s %08x\n", iface, nm, A(c,0));
        return;
    }
    fprintf(stderr, "D  %s", nm);
    for (int i = 1; i < nargs && i < 7; i++) fprintf(stderr, " %x", A(c,i));
    if (MID(SetVertexShaderConstantF) || MID(SetPixelShaderConstantF)) fxtr_floats(A(c,2), A(c,3)*4u);
    else if (MID(SetTransform)) fxtr_floats(A(c,2), 16u);
    fprintf(stderr, "\n");
}

int com_call(CPU *c, uint32_t magic){
    uint32_t iface=(magic>>16)&0x1FFFu, m=magic&0xFFFFu;
    g_com_calls++;
    g_com_cpu = c;
    if (!g_sb_replay) POP32(c->eip);
    if (iface==0 || iface>=NEL(g_if) || m>=g_if[iface].n){
        fprintf(stderr,"  [COM] invalid interface/method %u:%u\n", iface, m);
        c->eax=0x80004001u; return 1;
    }
    if (iface < MSTAT_IF && m < MSTAT_M) g_mstat[iface][m]++;
    const Method *M=&g_if[iface].m[m];
    int nargs=M->nargs; uint32_t r=0; int impl=1;
    const char *nm=M->name;
    int mid;
    if (iface < MIDIF && m < MIDM){
        mid = g_midc[iface][m];
        if (!mid) mid = g_midc[iface][m] = (int16_t)mid_resolve(nm);
    } else mid = mid_resolve(nm);

    if (g_dxrec_depth) dxrec_note(c, (int)iface, mid, nargs);
    if (g_fxtr && fxtr_now()) fxtr_dev(c, (int)iface, mid, nargs, nm);
    /* ZSEQ=<frame>: the order of target / Z surface / Clear / Z state in that frame (diagnostic) */
    if (iface==IF_DEV && ENVC("ZSEQ") && g_present_no == (uint32_t)atoi(ENVC("ZSEQ"))){
        static unsigned nd = 0;
        if (nm[0]=='D' && nm[1]=='r'){ nd++; }
        else if (MID(SetRenderTarget) || MID(SetDepthStencilSurface) || MID(Clear)
                 || (MID(SetRenderState) && (A(c,1)==7||A(c,1)==14||A(c,1)==23))
                 || MID(BeginScene) || MID(EndScene) || MID(StretchRect)){
            if (nd){ fprintf(stderr, "  [zseq]   .. %u draws\n", nd); nd = 0; }
            uint32_t s2 = A(c,2);
            fprintf(stderr, "  [zseq] %s %x %x %x %x  (rt=%u", nm, A(c,1), A(c,2), A(c,3), A(c,4), g_cur_rt);
            if ((MID(SetRenderTarget)||MID(SetDepthStencilSurface)) && s2 && mem_is_committed(s2+47))
                fprintf(stderr, " surface tid=%u %ux%u type=%u", MR32(s2+28), MR32(s2+4), MR32(s2+8), MR32(s2+40));
            if (MID(SetDepthStencilSurface) && A(c,1) && mem_is_committed(A(c,1)+47))
                fprintf(stderr, " zsurface %ux%u type=%u", MR32(A(c,1)+4), MR32(A(c,1)+8), MR32(A(c,1)+40));
            fprintf(stderr, ")\n");
        }
    }
    if (g_cs_on && iface==IF_DEV){
        if (g_cs_on < 0){ const char *e = getenv("COMSTAT"); g_cs_on = e != NULL; g_cs_from = e ? atol(e) : 0; }
        if (g_cs_on) comstat(c, mid, nargs);
    }

    /* --- CUBE TEXTURE: same vtable slot, ONE MORE argument ---------------
     * IDirect3DCubeTexture9 methods sit in the same slots as their 2D
     * counterparts but take a leading FaceType parameter.  The function
     * bodies already told them apart; but the STACK CLEANUP (nargs) had not
     * been fixed, so every call leaked 4 bytes.  With reflections on, the
     * game calls GetCubeMapSurface for the 6 cube faces and the stack slips
     * by exactly 24 bytes: the caller's epilogue pops from the wrong place,
     * esi and the return address are corrupted and the game crashed on the
     * first frame (reproduced exactly with g_CarEnvironmentMapEnable=1). */
    /* Check the CHEAP method id first: is_cube_tex() reads guest MEMORY
     * and used to run on EVERY texture call -- in a run,
     * IDirect3DTexture9 AddRef+Release alone are 96.8 million calls. */
    if (iface==IF_TEX &&
        (MID(GetSurfaceLevel) || MID(GetCubeMapSurface) ||
         MID(LockRect) || MID(UnlockRect) ||
         MID(AddDirtyRect)) && is_cube_tex(A(c,0)))
        nargs++;

    /* --- state block RECORDING -----------------------------------------
     * In D3D9 the device state does NOT change while recording; the call
     * is only written into the block. */
    if (g_sb_rec && iface==IF_DEV && !g_sb_replay && sb_recordable(mid)){
        SBlock *B = &g_sb[g_sb_rec];
        if (B->n < MAXSBREC){
            SBRec *R = &B->r[B->n++];
            R->magic = magic; R->nargs = nargs;
            for (int i2 = 0; i2 < 8; i2++) R->a[i2] = (i2 < nargs) ? A(c,i2) : 0u;
            { int oa = sb_objarg(mid); if (oa && R->a[oa]) com_addref(R->a[oa]); }
        } else g_sb_dropped++;
        c->eax = 0;
        c->esp += 4u*(uint32_t)nargs;
        return 1;
    }

    /* --- common: IUnknown --- */
    if      (MID(QueryInterface)){
        /* In DirectSound8, CreateSoundBuffer returns an IDirectSoundBuffer;
         * the game calls QueryInterface to get the IDirectSoundBuffer8.
         * Returning E_NOINTERFACE may leave those sounds never used at all
         * (measured: 4 buffers are created but none is written to).
         * We return the same object - the vtable slots are shared anyway. */
        uint32_t ppv = (nargs >= 3) ? A(c,2) : 0u;
        if ((iface==IF_DS || iface==IF_DSBUF) && ppv){
            mem_commit(ppv,4); MR32(ppv) = A(c,0); r = 0;
            com_addref(A(c,0));                 /* v12na: a new interface is a reference */
        } else r=0x80004002u;
    }
    else if (MID(AddRef))  r = com_addref(A(c,0));
    else if (MID(Release)){
        /* The game silences a sound buffer with Release, NOT with Stop
         * (measured: in a 3-minute session Stop=0, Release=7).  Without stopping
         * it, the menu music looped stale contents for the whole race and
         * clashed with the race audio. */
        /* State blocks give their slot back when destroyed (sb_destroy):
         * the game creates and drops blocks all session long. */
        if (iface==IF_DSBUF){
            uint32_t self0=A(c,0);
            if (self0 && mem_is_committed(self0+31) && MR32(self0+28)){
                void audio_stop(int);
                audio_stop((int)MR32(self0+28));
                MR32(self0+12)=0;
            }
        }
        r = com_release(A(c,0)); }
    /* --- IDirect3D9 --- */
    else if (iface==IF_D3D9){
        if      (MID(GetAdapterCount))     r=1;
        /* The resolution list used to be ONE row (1280x720), so the
         * resolution could not be changed in the game's settings menu.  We
         * give a real list; the chosen mode reaches Metal via CreateDevice/Reset. */
        else if (MID(GetAdapterModeCount)) r = g_game.d3d_modes ? game_mode_count() : (uint32_t)NEL(g_modes);
        else if (MID(EnumAdapterModes) && g_game.d3d_modes){
            /* (this, Adapter, Format, Mode, pMode): the game's own list (game.h d3d_modes) */
            uint32_t mode=A(c,3), pm=A(c,4), fmt=A(c,2);
            if (mode >= game_mode_count() || !pm) r=0x8876086Cu;          /* D3DERR_INVALIDCALL */
            else { mem_commit(pm,16);
                   MR32(pm) = g_game.d3d_modes[mode][0]; MR32(pm+4) = g_game.d3d_modes[mode][1];
                   MR32(pm+8) = 60; MR32(pm+12) = fmt ? fmt : 22u;
                   r=0; } }
        else if (MID(EnumAdapterModes)){
            uint32_t mode=A(c,3), pm=A(c,4);
            if (mode >= NEL(g_modes)) { r=0x8876086Cu; }
            else { uint32_t fmt=A(c,2); if(!fmt) fmt=22u;   /* echo the requested format */
                   if (mode == 0 && modemap_on()) modes_refresh();   /* menu labels */
                   const uint32_t *ml = g_tbl[mode];   /* selectability: the game's table */
                   if (mode == 4 && modemap_on() && !g_row4_on)
                       ml = g_tbl[ROW_NATIVE];         /* 4K not selectable: see modes_refresh */
                   if(pm){ mem_commit(pm,16);
                           MR32(pm)   = ml[0];
                           MR32(pm+4) = ml[1];
                           MR32(pm+8) = 60;
                           MR32(pm+12)= fmt; }
                   r=0; } }
        else if (MID(GetAdapterDisplayMode)){
            uint32_t pm=A(c,2);
            if(pm){ mem_commit(pm,16);
                    /* Desktop mode: the game drops rows larger than this
                     * (which is why 1600x1200 could never be selected). */
                    const uint32_t *nl = mode_label(ROW_NATIVE);
                    MR32(pm)   = nl[0] > 1600u ? nl[0] : 1600u;
                    MR32(pm+4) = nl[1] > 1200u ? nl[1] : 1200u;
                    MR32(pm+8)=60; MR32(pm+12)=22; }
            r=0; }
        else if (MID(GetAdapterIdentifier)){
            uint32_t p=A(c,3);
            /* D3DADAPTER_IDENTIFIER9 (1100 bytes): Driver[512] Description[512]
             * DeviceName[32] DriverVersion@1056 VendorId@1064 DeviceId@1068
             * SubSysId@1072 Revision@1076 GUID@1080 WHQLLevel@1096.
             * The game's first-run "system check" looks at VendorId/DeviceId
             * ([0x86C548]/[0x86C54C]); reporting 0 made it fall into the
             * unknown-vendor case, turn off ALL effects and pick 640x480.
             * NVIDIA GeForce 6800 (0x10DE/0x0040) is the cleanest path.
             * Can be changed with
             * NFSU2_GPU_VENDOR / NFSU2_GPU_DEVICE. */
            if(p){ mem_commit(p,1100); memset(g_mem+p,0,1100);
                   strcpy((char*)(g_mem+p),"nv4_disp.dll");
                   strcpy((char*)(g_mem+p+512),"NFSU2 Native Renderer (Metal, reports GeForce 6800)");
                   strcpy((char*)(g_mem+p+1024),"\\\\.\\DISPLAY1");
                   MR32(p+1056) = 0x00000A55u; MR32(p+1060) = 0x00060000u;   /* 6.x.0.2645 */
                   const char *gv = ENVC("NFSU2_GPU_VENDOR"), *gd = ENVC("NFSU2_GPU_DEVICE");
                   MR32(p+1064) = gv && *gv ? (uint32_t)strtoul(gv,0,0) : 0x10DEu;
                   MR32(p+1068) = gd && *gd ? (uint32_t)strtoul(gd,0,0) : 0x0040u;
                   MR32(p+1076) = 0xA1u; MR32(p+1096) = 1u; }
            r=0; }
        else if (MID(GetDeviceCaps)){ uint32_t pc=A(c,3); if(pc) fill_caps(pc); r=0; }
        else if (MID(CreateDevice)){
            if(!g_dev){ g_dev=com_new(IF_DEV,256); if (g_dev) CH_FLAGS(g_dev) |= CF_DEVICE; }
            /* (this, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
             *  pPresentationParameters, ppReturnedDeviceInterface)
             * pPresentationParameters was NEVER READ: whatever resolution
             * the game asked for, we stayed at 1280x720.
             * That is why changing the resolution in the settings menu had
             * no effect -- the game recreates the device with the new size,
             * and we handed back the same device and ignored the size. */
            uint32_t ppp=A(c,5);
            /* x87 state, as Direct3D 9 leaves it: without
             * D3DCREATE_FPU_PRESERVE (0x2) the calling thread's control
             * word becomes single precision, round to nearest, all
             * exceptions masked (PC=00, RC=00: cw & ~0xF3F | 0x3F).  The
             * game passes 0x50/0x20 (0x5B9B88/0x5B9BB5), so its main
             * thread runs at PC=24.  Only binaries lifted with X87PC=1 round
             * to it (tools/lift.py); NFSU2_FPU_PRESERVE=1 keeps the old
             * word for A/B runs. */
            if (!(A(c,4) & 0x2u) && !ENVC("NFSU2_FPU_PRESERVE")){
                uint16_t ocw = c->fcw;
                c->fcw = (uint16_t)((c->fcw & ~0x0F3Fu) | 0x003Fu);
                fprintf(stderr, "com: CreateDevice flags 0x%x: x87 control word %04x -> %04x\n",
                        A(c,4), ocw, c->fcw);
            }
            if (ppp && mem_is_committed(ppp+7)){
                uint32_t w4=MR32(ppp), h4=MR32(ppp+4);
                uint32_t win4 = mem_is_committed(ppp+35) ? MR32(ppp+32) : 1u;
                fprintf(stderr,"com: CreateDevice request %ux%u (%s)\n",
                        w4, h4, win4 ? "windowed" : "fullscreen");
                if (w4 > 8192u || h4 > 8192u){      /* the game's array overrun */
                    fprintf(stderr,"com: invalid size ignored, keeping %ux%u\n",
                            g_bb_w, g_bb_h);
                    w4 = h4 = 0;
                }
                /* NFSRES=<w>x<h>: override the resolution the game asks for.
                 * The game's own internal table has 6 rows, all 4:3; 1080p
                 * cannot be picked from the menu (index 6+ overruns the array). */
                { uint32_t dw = w4, dh = h4;
                  res_pick(w4, h4, &dw, &dh);
                  w4 = dw; h4 = dh; }
                if (w4 && h4){
                    g_bb_w = w4; g_bb_h = h4;
                    void metal_want_size(unsigned,unsigned);
                    metal_want_size(w4,h4);              /* if the window is not open yet */
                    if (metal_available()){ void metal_resize(unsigned,unsigned);
                                            metal_resize(w4,h4); }
                }
            }
            /* RESSCAN=1: find the game's OWN resolution table in guest memory.
             * The table is not stored plainly in the EXE but built at run
             * time; patching it is the only clean way to carry 1080p into
             * every part of the game (aspect ratio, render targets, viewport). */
            if (ENVC("RESSCAN")){
                static const uint32_t want[12] = {640,480,800,600,1024,768,
                                                  1280,960,1280,1024,1600,1200};
                extern uint32_t g_image_base, g_image_size;      /* mem_load_pe */
                struct { uint32_t lo, hi; } rng[] = {
                    {g_image_base, (g_image_base + g_image_size + 0xFFFFu) & ~0xFFFFu},   /* image (to 64 KB) */
                    {0x10000000u,0x18000000u},          /* heap  */
                };
                int found = 0;
                for (unsigned q = 0; q < sizeof rng/sizeof *rng && found < 8; q++)
                    for (uint32_t a = rng[q].lo; a + 48u < rng[q].hi && found < 8; a += 4u){
                        if (!mem_is_committed(a) || !mem_is_committed(a+47u)) continue;
                        int ok = 1;
                        for (int k = 0; k < 12; k++)
                            if (MR32(a + 4u*(uint32_t)k) != want[k]){ ok = 0; break; }
                        if (ok){ fprintf(stderr,"com: [resscan] table 0x%08x\n", a); found++; }
                    }
                /* If the full sequence is not found, report just the 1600x1200 pairs */
                if (!found){
                    int n2 = 0;
                    for (unsigned q = 0; q < sizeof rng/sizeof *rng && n2 < 12; q++)
                        for (uint32_t a = rng[q].lo; a + 8u < rng[q].hi && n2 < 12; a += 4u){
                            if (!mem_is_committed(a) || !mem_is_committed(a+7u)) continue;
                            if (MR32(a)==1600u && MR32(a+4u)==1200u){
                                fprintf(stderr,"com: [resscan] 1600x1200 @ 0x%08x  "
                                        "before: %u %u %u %u\n", a,
                                        mem_is_committed(a-16u)?MR32(a-16u):0,
                                        mem_is_committed(a-12u)?MR32(a-12u):0,
                                        mem_is_committed(a-8u)?MR32(a-8u):0,
                                        mem_is_committed(a-4u)?MR32(a-4u):0);
                                n2++; }
                        }
                    if (!n2) fprintf(stderr,"com: [resscan] table not found\n");
                }
            }
            uint32_t pp=A(c,6); if(pp) MR32(pp)=g_dev;
            fprintf(stderr,"com: IDirect3DDevice9 created 0x%08x (%ux%u)\n",
                    g_dev, g_bb_w, g_bb_h);
            r = g_dev ? 0u : 0x8007000Eu; }
        else if (MID(CheckDeviceMultiSampleType) && g_game.d3d9_strict){
            /* (this, Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels).  The
             * renderer draws without multisampling: D3DMULTISAMPLE_NONE with its one quality level, every
             * other type D3DERR_NOTAVAILABLE with none.  Most Wanted asks for NONMASKABLE (0x6C16CD) and
             * offers quality levels - 1 as its FSAA maximum [0x901890]. */
            uint32_t q = A(c,6), none = A(c,5) == 0u;
            if (q){ mem_commit(q,4); MR32(q) = none ? 1u : 0u; }
            r = none ? 0u : 0x8876086Au; }
        else if (MID(CheckDeviceFormat) && g_game.d3d9_strict){
            /* (this, Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat): what the creators
             * make, so a game that asks first never gets a creator's failure.  A texture, cube or volume
             * texture and a render-target surface: a format the renderer makes (CreateTexture,
             * CreateCubeTexture and CreateRenderTarget fail for any other).  A depth-stencil surface: a
             * D3D9 depth format.  A depth-stencil texture: none, the renderer has no depth textures (Most
             * Wanted asks for D24S8 at 0x6C1725 and 'DF16' at 0x6C175D and creates its shadow map in the
             * one it is given, 0x6C87A2).  A plain surface and a buffer: any. */
            uint32_t usage = A(c,4), rtype = A(c,5), f = A(c,6), ok;
            /* With 8 stages (GameOps gpu_stages) the renderer has depth textures: D16, D24X8 and D24S8 as a
             * 2D texture with D3DUSAGE_DEPTHSTENCIL (docs/web/gpu-abi.md 11.4), sampled with a hardware compare
             * as NVIDIA's; 'DF16', 'DF24', 'INTZ' and the rest D3DERR_NOTAVAILABLE, so Most Wanted takes the
             * D24S8 shadow map (0x6C1725) and not the raw-depth one ('DF16', 0x6C175D) */
            if (rtype == 3u && (usage & 2u) && g_game.gpu_stages == 8) ok = f == 75u || f == 77u || f == 80u;
            else if (rtype >= 3u && rtype <= 5u) ok = !(usage & 2u) && fmt_makes(f);  /* D3DUSAGE_DEPTHSTENCIL 2 */
            else if (rtype == 1u) ok = (usage & 2u) ? fmt_is_depth(f) : (usage & 1u) ? fmt_makes(f) : 1;
            else ok = 1;
            r = ok ? 0u : 0x8876086Au; }                                       /* D3DERR_NOTAVAILABLE */
        else if (MID(CheckDepthStencilMatch) && g_game.d3d9_strict){
            /* (this, Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat): every
             * depth surface goes with every target the renderer makes */
            r = (fmt_is_depth(A(c,5)) && fmt_makes(A(c,4))) ? 0u : 0x8876086Au; }
        else r=0;                       /* Check* -> D3D_OK */
    }
    /* --- IDirect3DDevice9 --- */
    else if (iface==IF_DEV){if (MID(SetRenderState)){
            uint32_t st=A(c,1), val=A(c,2);
            if (st < 256u){
                if (g_rs[st] != val){ g_rs[st] = val; g_rs_dirty = 1;
                    int seen=0; for(int k=0;k<g_rs_nval[st];k++) if(g_rs_vals[st][k]==val) seen=1;
                    if(!seen && g_rs_nval[st]<4) g_rs_vals[st][g_rs_nval[st]++]=val;
                }
                g_rs_hits[st]++;
            }
            if (g_rs_dirty){ if (metal_available()){ rs_apply(); g_rs_dirty = 0; } }
            else g_rs_skip++;
            r=0; }
        else if (MID(SetSamplerState)){
            uint32_t sm=A(c,1), ty=A(c,2), val=A(c,3);
            if (sm < 16u && ty < 20u) g_smp_raw[sm][ty] = val;
            if (ENVC("SMPLOG") && sm < 4u && ty < 20u){
                static uint32_t seen[4][20];
                if (seen[sm][ty] != val + 1u){ seen[sm][ty] = val + 1u;
                    const char *tn = ty==1?"ADDRESSU":ty==2?"ADDRESSV":ty==3?"ADDRESSW":
                                     ty==5?"MAGFILTER":ty==6?"MINFILTER":ty==7?"MIPFILTER":
                                     ty==8?"MIPMAPLODBIAS":ty==9?"MAXMIPLEVEL":
                                     ty==10?"MAXANISOTROPY":ty==11?"SRGB":"?";
                    fprintf(stderr,"  [smp] stage%u %s(%u) = %u\n", sm, tn, ty, val); } }
            r=0; }
        else if (MID(SetTextureStageState)){
            uint32_t stg=A(c,1), ty=A(c,2), val=A(c,3);
            if (stg < 8u && ty < 40u) g_tss_raw[stg][ty] = val;
            r=0; }
        else if (MID(SetTexture)){
            /* CUBELOG=1: measure whether the cube texture really gets bound.
             * The Metal side has NO cube support (it is created as plain 2D);
             * first confirm this path is used. */
            if (ENVC("CUBELOG") && A(c,2) && is_cube_tex(A(c,2))){
                static unsigned n2 = 0;
                if (n2 < 8){ n2++;
                    fprintf(stderr,"  [cube] SetTexture stage=%u tex=0x%08x vs=%u\n",
                            A(c,1), A(c,2), g_cur_vs); }
            }
            uint32_t stg=A(c,1), to=A(c,2);
            if (stg < 8u){ g_raw_tex[stg] = to; bind_set(&g_bind_tex[stg], to); }
            /* Track all stages: car paint/reflection may be in the upper
             * stages; for now for measurement only. */
            if (stg < 4u)
                g_tex_stage[stg] = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
            if (stg==0){
                g_cur_tex = (to && mem_is_committed(to+28)) ? MR32(to+28) : 0;
                if (!to) g_st_null++;
                else if (!g_cur_tex){ g_st_zero++;
                    static int sd=0;
                    if (sd<4 && mem_is_committed(to+31)){ sd++;
                        uint32_t vt = MR32(to);
                        uint32_t mg = (vt && mem_is_committed(vt)) ? MR32(vt) : 0;
                        fprintf(stderr,"  [SetTexture] object=%08x vtable=%08x first-entry=%08x"
                                " -> interface=%u  fields: +4=%u +8=%u +14=%u(fmt) +18=%u +1c=%u\n",
                                to, vt, mg, (mg>>16)&0x1FFFu,
                                MR32(to+4), MR32(to+8), MR32(to+0x14),
                                MR32(to+0x18), MR32(to+0x1c));
                    }
                } else g_st_ok++;
            } else g_st_other++;
            r=0; }
        else if (!strncmp(nm,"Draw",4)){
            g_draw++;
            { static const char *seen[8]; static int ns=0;
              int f=0; for(int q=0;q<ns;q++) if(seen[q]==nm) f=1;   /* names come from the g_if table: the pointer suffices */
              if(!f && ns<8){ seen[ns++]=nm;
                  fprintf(stderr,"  [draw-name] \"%s\"\n", nm); } }
            if (MID(DrawIndexedPrimitive)){
                /* (this, Type, BaseVertexIndex, MinIndex, NumVertices,
                 *  StartIndex, PrimitiveCount) */
                uint32_t pt=A(c,1), basev=A(c,2), starti=A(c,5), nprim=A(c,6);
                r=0;
                if (metal_available() && g_ss_vb && g_ib_obj && g_ss_stride &&
                    nprim && nprim < 200000u){
                    /* the stream's vertices and the indices: guest memory or a buffer's host copy */
                    BSrc vsrc = buf_src(g_ss_vb, g_ss_off), isrc = buf_src(g_ib_obj, 0u);
                    int vbase = vsrc.p || vsrc.ga, ibase = isrc.p || isrc.ga;
                    uint32_t ifmt  = MR32(g_ib_obj+12);
                    uint32_t isz   = (ifmt==102u) ? 4u : 2u;   /* INDEX32 : INDEX16 */
                    uint32_t ntri  = (pt==4u||pt==5u||pt==6u) ? nprim : 0u;
                    /* point and line lists and strips; with game.h d3d9_strict triangle strips and fans too,
                     * in D3D9's order (the old path below turns a strip's odd triangle to start at another
                     * corner, which flat shading sees; UG2 draws only lists, its 3 sites push type 4) */
                    int listed = (pt >= 1u && pt <= 3u) || (g_game.d3d9_strict && (pt == 5u || pt == 6u));
                    if (listed && vbase && ibase){
                        if (draw_prims(pt, nprim, vsrc, g_ss_stride, buf_src(g_ib_obj, starti*isz), isz, (int32_t)basev))
                            g_dip_draws++;
                        else g_dip_skip++; }
                    else if (!ntri || !vbase || !ibase){ g_dip_skip++; }
                    else if (dip_indexed(pt, basev, starti, ntri, isz)){
                        g_dip_draws++; g_dip_tris += ntri; }
                    else {
                        if (!g_idx_scratch) g_idx_scratch = gbuf_alloc(4096u*64u);
                        uint32_t maxv = 4095u / 3u * 3u;       /* draw_up limit */
                        uint32_t done = 0;
                        while (done < ntri && g_ss_stride <= 64u){
                            uint32_t chunk = ntri - done;
                            if (chunk*3u > maxv) chunk = maxv/3u;
                            uint32_t nv = chunk*3u, ok = 1;
                            for (uint32_t t=0; t<chunk && ok; t++){
                                uint32_t g = done + t, i0,i1,i2;
                                if (pt==4u){ i0=g*3u; i1=g*3u+1u; i2=g*3u+2u; }
                                else if (pt==5u){ i0=g; i1=g+1u; i2=g+2u;
                                                  if (g&1u){ uint32_t tmp=i1; i1=i2; i2=tmp; } }
                                else { i0=0u; i1=g+1u; i2=g+2u; }
                                uint32_t idx[3] = { i0, i1, i2 };
                                for (int k=0;k<3;k++){
                                    const uint8_t *ia = bsrc_at(isrc, (starti + idx[k])*isz, isz);
                                    if (!ia){ ok=0; break; }
                                    uint32_t vi = bsrc_index(ia, isz);
                                    uint32_t dst = g_idx_scratch + (t*3u+(uint32_t)k)*g_ss_stride;
                                    const uint8_t *src = bsrc_at(vsrc, (basev + vi)*g_ss_stride, g_ss_stride);
                                    if (!src){ ok=0; break; }
                                    memcpy(g_mem+dst, src, g_ss_stride);
                                }
                            }
                            if (!ok){ g_dip_skip++; break; }
                            draw_up(4u, nv, g_idx_scratch, g_ss_stride);
                            g_dip_draws++; g_dip_tris += chunk;
                            done += chunk;
                        }
                    }
                }
            }
            else if (MID(DrawPrimitiveUP)){
                /* (this, PrimitiveType, PrimitiveCount, pVertexData, VertexStride) */
                static uint64_t ptype[8], stride_hist[64];
                static int sh = 0;
                uint32_t pt=A(c,1), stride=A(c,4);
                if (pt<8) ptype[pt]++;
                if (stride < 64*4 && (stride&3)==0) stride_hist[stride/4]++;
                draw_stats_save(ptype, stride_hist);
                if (metal_available()){
                    uint32_t nprim=A(c,2), vp=A(c,3);
                    uint32_t nv = prim_verts(pt, nprim);
                    /* points: draw_points.  UG2 draws none: its 11 DrawPrimitiveUP sites in SPEED2.EXE
                     * (calls through the device's +0x14C) push the types 6, 5 and 3 only, so its draws
                     * here are as before (the 3 of a line strip and the 5/6 of a quad have nv >= 2) */
                    if (vp && stride && pt >= 1u && pt <= 6u && nv >= 1u && nv < 4096u &&
                        mem_is_committed(vp) && mem_is_committed(vp + nv*stride - 1))
                        draw_up(pt, nv, vp, stride);
                    /* a longer run in chunks (game.h d3d9_strict; UG2 leaves it out) */
                    else if (g_game.d3d9_strict && vp && nv >= 4096u) draw_prims(pt, nprim, guest_src(vp), stride, guest_src(0u), 0u, 0);
                }
                if (dd_on()){
                    static uint32_t last_decl = 0xFFFFFFFFu; static int nd = 0;
                    uint32_t nprim=A(c,2), vp=A(c,3);
                    uint32_t nv = prim_verts(pt, nprim);
                    if (g_cur_decl != last_decl){ last_decl = g_cur_decl; dd_decl(g_cur_decl); }
                    if (nd++ < 40){
                        fprintf(stderr,"   #%-3d type=%u verts=%-4u stride=%u tex=%u\n",
                                nd, pt, nv, stride, g_cur_tex);
                        for (uint32_t v=0; v<nv && v<4; v++){
                            uint32_t o = vp + v*stride;
                            if (!mem_is_committed(o+31)) break;
                            fprintf(stderr,"        [%u] ", v);
                            for (int w2=0; w2<8; w2++)
                                fprintf(stderr,"%s%.4g", w2?" ":"", (double)MRF32(o+4*w2));
                            fprintf(stderr,"  | raw:");
                            for (int w2=0; w2<8; w2++) fprintf(stderr," %08x", MR32(o+4*w2));
                            fprintf(stderr,"\n");
                        }
                    }
                }
                if (VB() && sh < 6){
                    sh++;
                    uint32_t d  = g_cur_decl;
                    uint32_t el = (d && mem_is_committed(d+4)) ? MR32(d+4) : 0;
                    fprintf(stderr,"  [draw] type=%u count=%u stride=%u decl=0x%08x fvf=0x%08x elements=0x%08x\n",
                            pt, A(c,2), stride, d, g_cur_fvf, el);
                    /* dump the first vertices' data */
                    { uint32_t vp = A(c,3), nprim = A(c,2);
                      uint32_t nv = prim_verts(pt, nprim);
                      if (nv > 6) nv = 6;
                      for (uint32_t v=0; v<nv; v++){
                        uint32_t o = vp + v*stride;
                        if (!mem_is_committed(o+24)) break;
                        fprintf(stderr,"        v%u: pos=(%.2f, %.2f, %.2f) colour=0x%08x uv=(%.3f, %.3f)\n",
                                v, MRF32(o), MRF32(o+4), MRF32(o+8), MR32(o+12),
                                MRF32(o+16), MRF32(o+20));
                      } }
                    for (uint32_t k = 0; el && k < 16; k++){
                        uint32_t e = el + 8u*k;
                        if (!mem_is_committed(e+7)) break;
                        uint32_t st=MR16(e), of=MR16(e+2), ty=MR8(e+4), us=MR8(e+6);
                        if (st == 0xFFu && ty == 17u) break;
                        fprintf(stderr,"        offset=%-3u type=%-2u usage=%u\n", of, ty, us);
                    }
                }
            }
            else if (MID(DrawPrimitive)){
                /* (this, PrimitiveType, StartVertex, PrimitiveCount): stream 0's vertices.  UG2 draws none:
                 * SPEED2.EXE has no call through the device's +0x144, nor DrawIndexedPrimitiveUP's +0x150,
                 * and its logs name only DrawIndexedPrimitive and DrawPrimitiveUP ([draw-name]) */
                uint32_t pt=A(c,1), startv=A(c,2), nprim=A(c,3);
                if (metal_available() && g_ss_vb && g_ss_stride && nprim && nprim < 1000000u){
                    BSrc vsrc = buf_src(g_ss_vb, g_ss_off + startv*g_ss_stride);
                    if (vsrc.p || vsrc.ga) draw_prims(pt, nprim, vsrc, g_ss_stride, guest_src(0u), 0u, 0);
                }
            }
            else if (MID(DrawIndexedPrimitiveUP)){
                /* (this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
                 *  IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride) (UG2 draws none, above) */
                uint32_t pt=A(c,1), nprim=A(c,4), ip=A(c,5), ifmt=A(c,6), vp=A(c,7), stride=A(c,8);
                if (metal_available() && ip && vp && nprim && nprim < 1000000u)
                    draw_prims(pt, nprim, guest_src(vp), stride, guest_src(ip), ifmt == 102u ? 4u : 2u, 0);
            }
            r=0; }
        else if (MID(SetVertexShader)){
            uint32_t h=A(c,1); g_raw_vs = h;
            g_cur_vs = (h && mem_is_committed(h+15)) ? MR32(h+12) : 0;
            if (g_cur_vs >= MAXVS) g_cur_vs = 0;
            r=0; }
        else if (MID(SetPixelShader)){
            uint32_t sh=A(c,1); g_raw_ps = sh;
            g_cur_ps_id = (sh && mem_is_committed(sh+15)) ? (int)MR32(sh+12) : 0;
            r=0; }
        else if (MID(SetVertexShaderConstantF)){
            /* (this, StartRegister, pConstantData, Vector4fCount) */
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            static int sh=0;
            g_sc[g_scn & 7].sr = sr; g_sc[g_scn & 7].pd = pd;
            g_sc[g_scn & 7].n = n; g_scn++;
            for (uint32_t k=0; k<n && sr+k<256; k++){
                uint32_t o=pd+16u*k;
                if (!mem_is_committed(o+15)) break;
                memcpy(g_vsk.f[sr+k], g_mem+o, 16);
            }
            /* VSKDUMP: dump the AREA AROUND pd.  If the matrix is shifted by
             * 2 vectors, the right rows should sit at pd-32. */
            /* WATCHVSK: who writes this buffer?  Make the page read-only right
             * after the upload; the next frame's writer gets caught. */
            /* PDLIST: list the distinct pd values and whether the matrix is
             * broken.  For comparing good/broken blocks. */
            if (ENVC("PDLIST") && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                static uint32_t seen[32]; static int ns=0;
                int f=0; for (int q=0;q<ns;q++) if (seen[q]==pd) f=1;
                if (!f && ns<32){ seen[ns++]=pd;
                    float m00=MRF32(pd), m11=MRF32(pd+20), m33=MRF32(pd+60);
                    int bad = (m00==0.f && m11==0.f && m33==0.f);
                    fprintf(stderr,"  [pd] %08x n=%u %s  m00=%g m11=%g m33=%g\n",
                            pd, n, bad?"BAD ":"good", m00, m11, m33);
                }
            }
            /* OWNERSCAN: find the structure on the heap that points at pd, look for a name in it. */
            if (ENVC("OWNERSCAN") && sr==0 && n==5 && pd){
                static int os_=0, oseen=0;
                oseen++;
                if (oseen > 400 && !os_){ os_=1;
                    fprintf(stderr,"  [owner] structures pointing at pd=%08x:\n", pd);
                    int found = 0;
                    for (uint64_t A2 = 0x10000000ull; A2 < 0x30000000ull && found < 12; A2 += 4){
                        uint32_t a3 = (uint32_t)A2;
                        if ((a3 & 0x3FFFu) == 0 && !mem_is_committed(a3)) { A2 += 0x3FFCull; continue; }
                        if (!mem_is_committed(a3+3)) continue;
                        if (MR32(a3) != pd) continue;
                        found++;
                        fprintf(stderr,"    structure field %08x:\n", a3);
                        uint32_t base = (a3 > 0x80u) ? a3 - 0x80u : 0x10000u;
                        for (uint32_t q = base; q <= a3 + 0x80u; q += 4u){
                            if (!mem_is_committed(q+3)) continue;
                            uint32_t P = MR32(q);
                            if (P < 0x10000u || !mem_is_committed(P+48u)) continue;
                            const char *t = (const char*)(g_mem+P);
                            int ok = 1, L = 0;
                            while (L < 44 && t[L]){
                                unsigned char ch = (unsigned char)t[L];
                                if (ch < 0x20u || ch > 0x7Eu){ ok = 0; break; }
                                L++; }
                            if (ok && L >= 3 && L < 44 && t[L] == 0)
                                fprintf(stderr,"        %08x (pd%+d) -> \"%s\"\n",
                                        q, (int)q - (int)a3, t);
                        }
                    }
                    if (!found) fprintf(stderr,"    (no pointer to it found)\n");
                }
            }
            /* PARAMNAME: scan the pointers on the stack to find the NAME of
             * the D3DX effect parameter.  To learn which parameter is never
             * set. */
            if (ENVC("PARAMNAME") && sr==0 && n==5 && pd){
                static int pn=0, pseen=0;
                pseen++;
                if (pseen > 400 && pn < 1){ pn=1;
                    fprintf(stderr,"  [param] stack scan for pd=%08x:\n", pd);
                    for (uint32_t off=0; off<0x120u; off+=4u){
                        uint32_t sp2 = c->esp + off;
                        if (!mem_is_committed(sp2+3)) continue;
                        uint32_t V = MR32(sp2);
                        if (V < 0x10000u || !mem_is_committed(V+0x67u)) continue;
                        for (uint32_t q=0; q<0x64u; q+=4u){
                            uint32_t P = MR32(V+q);
                            if (P < 0x10000u || !mem_is_committed(P+40u)) continue;
                            const char *t = (const char*)(g_mem+P);
                            int ok=1, L=0;
                            while (L<40 && t[L]){
                                unsigned char ch=(unsigned char)t[L];
                                if (ch<0x20u || ch>0x7Eu){ ok=0; break; }
                                L++; }
                            if (ok && L>=4 && L<40 && t[L]==0)
                                fprintf(stderr,"      stack+%03x -> structure %08x +%02x -> \"%s\"\n",
                                        off, V, q, t);
                        }
                    }
                }
            }
            /* VSKBT: the guest call chain that uploads the matrix */
            if (ENVC("VSKBT") && sr==0 && n==5 && pd){
                static int bt=0, bseen=0;
                bseen++;
                if (bseen > atoi(ENVC("VSKBT")) && bt<2){ bt++;
                    fprintf(stderr,"  [vsk-caller] upload #%d pd=%08x\n", bseen, pd);
                    guest_backtrace(c, 14); }
            }
            if (ENVC("WATCHVSK") && sr==0 && n==5 && pd){
                static int wv=0, wseen=0;
                wseen++;
                if (wseen > atoi(ENVC("WATCHVSK")) && !wv){ wv=1; mprot_arm(pd); }
            }
            if (ENVC("VSKDUMP") && sr==0 && n==5 && pd){
                static int vd=0, vseen=0;
                vseen++;
                if (vseen>500 && vd<3){ vd++;
                    fprintf(stderr,"  [vsk] around pd=%08x n=%u:\n", pd, n);
                    for (int q=-4; q<8; q++){
                        uint32_t o2 = pd + (uint32_t)(16*q);
                        if (!mem_is_committed(o2+15)) { fprintf(stderr,"      %+3d: (none)\n", q); continue; }
                        fprintf(stderr,"      %+3d (%08x): %14.7g %14.7g %14.7g %14.7g\n", q, o2,
                                MRF32(o2), MRF32(o2+4), MRF32(o2+8), MRF32(o2+12));
                    }
                }
            }
            if (ENVC("CONSTDUMP") && sr==0 && n==5 && pd && mem_is_committed(pd+79)){
                static int cd=0;
                if (cd<2){ cd++;
                    for (uint32_t bp2=(pd & ~15u); bp2>=0x10000000u; bp2-=16u){
                        if (!mem_is_committed(bp2+4)) break;
                        if (MR32(bp2+4)==0xA110C8EDu){
                            fprintf(stderr,"  [block] pd=%08x  block start=%08x size=%u"
                                    "  -> pd offset=%d\n",
                                    pd, bp2+16u, MR32(bp2), (int)(pd-(bp2+16u)));
                            break; } }
                    fprintf(stderr,"  [consts] pd=%08x n=%u  (including the 2 vec4 before the block)\n", pd, n);
                    for (int rw=-2; rw<(int)n && rw<11; rw++){
                        uint32_t a2 = pd + (uint32_t)(rw*16);
                        if (!mem_is_committed(a2+15)) continue;
                        fprintf(stderr,"      c%-3d %11.5g %11.5g %11.5g %11.5g\n", rw,
                                MRF32(a2), MRF32(a2+4), MRF32(a2+8), MRF32(a2+12));
                    }
                }
            }
            /* MATCHECK: is the matrix sane? for c0..c3: last row like (0,0,0,1)
             * or at least one element of reasonable magnitude (a count every
             * 20,000 uploads, ~5 lines a second in a race: players' logs were
             * mostly these) */
            if (ENVC("MATCHECK") && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                static uint64_t good=0, bad=0, zero=0; static int shown=0;
                int nz=0, big=0;
                for (int q=0;q<16;q++){
                    float fv = MRF32(pd+4u*q);
                    if (fv != 0.f) nz++;
                    if (fabsf(fv) > 1e6f) big++;
                }
                if (nz==0) zero++;
                else if (big){ bad++;
                    if (ENVC("WATCHALLOC") && !g_wc_armed){
                        g_wc_armed = 1;
                        void watch_dump(const char*, int);
                        void watch_arm(uint32_t);
                        watch_dump("constant block writer", 30);
                        watch_arm(0);
                    }
                    if (0){
                        if (++g_wc_after == 40){
                            void watch_dump(const char*, int);
                            void watch_arm(uint32_t);
                            watch_dump("constant block writer", 24);
                            watch_arm(0);              /* stop watching */
                            g_wc_armed = 0;
                        }
                    }
                    /* c->eip = just past the guest instruction that called
                     * SetVertexShaderConstantF (POP32'd on entry to com_call) */
                    static uint32_t caller[8]; static int ncall=0;
                    int seen=0;
                    for (int q=0;q<ncall;q++) if (caller[q]==c->eip) seen=1;
                    if (!seen && ncall<8){ caller[ncall++]=c->eip;
                        fprintf(stderr,"  [matrix-BAD] pd=%08x  caller return=0x%08x  n=%u\n",
                                pd, c->eip, n);
                        /* The caller uses an ebp-based frame and ebp is live:
                         * [ebp-8] = parameter descriptor (see 0x00669d77). */
                        /* which heap block is pd in? (header: +0 size, +4 0xA110C8ED) */
                        for (uint32_t bp = (pd & ~15u); bp >= 0x10000000u; bp -= 16u){
                            if (!mem_is_committed(bp+4)) break;
                            if (MR32(bp+4) == 0xA110C8EDu){
                                uint32_t bsz = MR32(bp);
                                fprintf(stderr,"      pd block: start=%08x size=%u  pd offset=%d\n",
                                        bp+16u, bsz, (int)(pd - (bp+16u)));
                                fprintf(stderr,"      first 64 bytes of the block:");
                                for (int q=0;q<64;q++){ if(q%32==0) fprintf(stderr,"\n        ");
                                    fprintf(stderr,"%02x ", MR8(bp+16u+q)); }
                                fprintf(stderr,"\n        ascii: ");
                                for (int q=0;q<64;q++){ uint8_t ch=MR8(bp+16u+q);
                                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                fprintf(stderr,"\n");
                                break;
                            }
                        }
                        /* Set a watchpoint to catch the next write:
                         * the block is refilled every frame, so setting
                         * it now is enough. */
                        if (ENVC("WATCHCONST")){
                            void watch_arm(uint32_t);
                            watch_arm(pd + 16u);
                            g_wc_armed = 1;
                        }
                        /* Search memory for the semantic hash (h=h*33+c): do the game
                         * side and the effect side hold the same value? */
                        /* Find the "WorldViewProj" string and the structures pointing at it:
                         * to see the parameter descriptor's type field directly. */
                        if (ENVC("FINDPARAM")){
                            static const char *needle = "WorldViewProj";
                            size_t nl = strlen(needle);
                            uint32_t found[8]; int nf = 0;
                            for (uint32_t a = 0x10000000u; a < 0x10400000u && nf < 8; a += 4){
                                if (!mem_is_committed(a + 32)) { a += 0x3FFCu; continue; }
                                if (!memcmp(g_mem + a, needle, nl) && MR8(a+nl) == 0)
                                    found[nf++] = a;
                            }
                            fprintf(stderr,"      'WorldViewProj' found: in %d places\n", nf);
                            for (int k = 0; k < nf; k++){
                                fprintf(stderr,"        @%08x\n", found[k]);
                                /* look for dwords pointing at this address */
                                int np = 0;
                                for (uint32_t a = 0x10000000u; a < 0x10400000u && np < 6; a += 4){
                                    if (!mem_is_committed(a + 4)) { a += 0x3FFCu; continue; }
                                    if (MR32(a) != found[k]) continue;
                                    np++;
                                    /* which field of a structure is the pointer in? try assuming +8 */
                                    for (int off = 0; off <= 0x10; off += 4){
                                        uint32_t base = a - (uint32_t)off;
                                        if (!mem_is_committed(base + 0x5F)) continue;
                                        fprintf(stderr,"          pointer@%08x (structure start %08x, field +%02x)"
                                                "  +54=%u +56=%u +04=%u +00=%u\n",
                                                a, base, off, MR16(base+0x54), MR16(base+0x56),
                                                MR32(base+4), MR32(base));
                                    }
                                }
                            }
                        }
                        uint32_t desc = mem_is_committed(c->ebp) ? MR32(c->ebp - 8u) : 0;
                        fprintf(stderr,"      ebp=%08x descriptor=%08x\n", c->ebp, desc);
                        if (desc && mem_is_committed(desc+0x5F)){
                            for (int rw2=0; rw2<0x60; rw2+=16){
                                fprintf(stderr,"        +%02x:", rw2);
                                for (int q=0;q<16;q++) fprintf(stderr," %02x", MR8(desc+rw2+q));
                                fprintf(stderr,"  ");
                                for (int q=0;q<16;q++){ uint8_t ch=MR8(desc+rw2+q);
                                    fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                fprintf(stderr,"\n");
                            }
                            fprintf(stderr,"        reg=%u count=%u flag30=%02x\n",
                                    MR16(desc+0x54), MR16(desc+0x56), MR8(desc+0x30));
                            for (int f=0; f<0x60; f+=4){
                                uint32_t v = MR32(desc+f);
                                if (v > 0x10000000u && v < 0x30000000u && mem_is_committed(v+15)){
                                    fprintf(stderr,"        [+%02x]->%08x (pd delta %+d):", f, v, (int)(v - pd));
                                    for (int q=0;q<16;q++) fprintf(stderr," %02x", MR8(v+q));
                                    fprintf(stderr,"  ");
                                    for (int q=0;q<16;q++){ uint8_t ch=MR8(v+q);
                                        fputc((ch>=32&&ch<127)?ch:'.', stderr); }
                                    fprintf(stderr,"\n");
                                }
                            }
                        }
                        for (uint32_t rw=0; rw<n && rw<13; rw++){
                            if (!mem_is_committed(pd+rw*16+15)) break;
                            fprintf(stderr,"      c%-2u %12.4g %12.4g %12.4g %12.4g\n", rw,
                                MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                                MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
                        }
                        /* BEFORE the buffer: the game sometimes passes a pointer
                         * shifted backwards; the real data may be there */
                        if (pd >= 64 && mem_is_committed(pd-64)){
                            fprintf(stderr,"      --- 4 vec4 before pd ---\n");
                            for (int rw=-4; rw<0; rw++)
                                fprintf(stderr,"      c%-2d %12.4g %12.4g %12.4g %12.4g\n", rw,
                                    MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                                    MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
                        }
                    }
                    static int bt=0;
                    if (ENVC("MATBT") && bt<2){ bt++;
                        fprintf(stderr,"  [matrix-BAD] pd=%08x, call stack:\n", pd);
                        guest_backtrace(c, 14); } }
                else { good++;
                       if (shown<3){ shown++;
                         fprintf(stderr,"  [matrix-GOOD] pd=%08x\n", pd);
                         for (int rw=0;rw<4;rw++)
                           fprintf(stderr,"      %9.4f %9.4f %9.4f %9.4f\n",
                             MRF32(pd+rw*16),MRF32(pd+rw*16+4),
                             MRF32(pd+rw*16+8),MRF32(pd+rw*16+12)); } }
                if (((good+bad+zero) % 20000)==0)
                    fprintf(stderr,"  [matrix] good=%llu bad=%llu zero=%llu\n",
                            (unsigned long long)good,(unsigned long long)bad,
                            (unsigned long long)zero);
            }
            if (ENVC("VSCDUMP") && sh<6 && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                sh++;
                fprintf(stderr,"  [vsconst] esp=%08x pd=%08x sr=%u n=%u  stack:",
                        c->esp, pd, sr, n);
                for (int q=0;q<6;q++) fprintf(stderr," %08x", MR32(c->esp+4u*q));
                fprintf(stderr,"\n            raw@pd:");
                for (int q=0;q<16;q++){ if(q%8==0) fprintf(stderr,"\n              ");
                    fprintf(stderr," %08x", MR32(pd+4u*q)); }
                fprintf(stderr,"\n");
            }
            if (VB() && sh<8 && sr==0 && n>=4 && pd && mem_is_committed(pd+63)){
                sh++;
                fprintf(stderr,"  [vsconst] c0..c3 (StartRegister=%u, %u vectors):\n", sr, n);
                for (uint32_t rw=0; rw<4; rw++)
                    fprintf(stderr,"     [%8.3f %8.3f %8.3f %8.3f]\n",
                            MRF32(pd+rw*16), MRF32(pd+rw*16+4),
                            MRF32(pd+rw*16+8), MRF32(pd+rw*16+12));
            }
            r=0; }
        else if (MID(SetStreamSource)){
            /* (this, StreamNumber, pStreamData, OffsetInBytes, Stride) */
            if (A(c,1)==0u){ g_ss_vb=A(c,2); g_ss_off=A(c,3); g_ss_stride=A(c,4); }
            if (A(c,1) < 4u) bind_set(&g_bind_vb[A(c,1)], A(c,2));
            r=0; }
        else if (MID(SetIndices)){ g_ib_obj=A(c,1); bind_set(&g_bind_ib, A(c,1)); r=0; }
        else if (MID(SetPixelShaderConstantF)){
            /* (this, StartRegister, pConstantData, Vector4fCount) */
            uint32_t sr2=A(c,1), pd2=A(c,2), n2=A(c,3);
            if (sr2+n2 > g_psc_max) g_psc_max = sr2+n2;
            if (sr2+n2 > 32u) g_psc_drop++;
            for (uint32_t q4=0; q4<n2 && sr2+q4<32u; q4++){
                uint32_t off = pd2 + 16u*q4;
                if (!pd2 || !mem_is_committed(off+15)) break;
                for (int c4=0;c4<4;c4++) g_psc[sr2+q4][c4] = MRF32(off+4u*(uint32_t)c4);
            }
            r=0; }
        
        else if      (MID(TestCooperativeLevel)){
            g_tcl_n++;
            /* Device loss (fullscreen switch): as on D3D9 the game releases
             * its D3DPOOL_DEFAULT resources and calls Reset. */
            r = atomic_load(&g_lost) ? 0x88760869u /* D3DERR_DEVICENOTRESET */ : 0u;
            if (r && g_tcl_lost_n++ < 4) fprintf(stderr,"com: TestCooperativeLevel -> DEVICENOTRESET\n"); }
        else if (MID(GetAvailableTextureMem)) r=256u*1024u*1024u;
        else if (MID(GetDeviceCaps)){ uint32_t pc=A(c,1); if(pc) fill_caps(pc); r=0; }
        else if (MID(GetDirect3D)){ uint32_t p=A(c,1); if(p) MR32(p)=g_d3d9; com_addref(g_d3d9); r=0; }
        else if (MID(GetDisplayMode)){
            /* Must be the CURRENT size; with a fixed 1280x720 the game's
             * settings screen could not find the selected row in the list.
             * For the same reason, when the device is larger than the game
             * thinks (Retina scale, NFSRES) we report the size the game ITSELF
             * picked, not the real one (e.g. 2448x1836): that is not in its table. */
            uint32_t pm=A(c,2);
            uint32_t dmw = g_req_w ? g_req_w : g_bb_w;
            uint32_t dmh = g_req_h ? g_req_h : g_bb_h;

            if(pm){mem_commit(pm,16);MR32(pm)=dmw;MR32(pm+4)=dmh;
                   MR32(pm+8)=60;MR32(pm+12)=22;} r=0; }
        else if (MID(Clear)){
            /* Clear(this,Count,pRects,Flags,Color,Z,Stencil) -> Color = arg4 */
            /* Flags: 1=TARGET 2=ZBUFFER 4=STENCIL; Z = arg5 (float) */
            if (metal_available()){
                void metal_clear_target_ex(unsigned, unsigned, float);
                uint32_t zb = A(c,5); float zf; memcpy(&zf, &zb, 4);
                metal_set_clear_color(A(c,4)); metal_clear_target_ex(A(c,4), A(c,3), zf); }
            r=0; }
        else if (MID(BeginScene)){ if (metal_available()) metal_begin_frame(); r=0; }
        else if (MID(EndScene)) r=0;
        else if (MID(Present)){ g_present_no++;
            { extern void autotest_on_present(CPU*); autotest_on_present(c); }
            { void watchdog_on_present(void); watchdog_on_present(); }
            { extern void discord_on_present(void); discord_on_present(); }   /* Discord presence: samples, never blocks */
            texrep_frame();
            { extern void guest_cpu_mark(void); guest_cpu_mark(); }
            /* the game's own work at every Present (UG2: its debugging switches, debug_ug2.c) */
            if (g_game.on_present) g_game.on_present(c);
            /* a controller connected since the game's enumeration found none (game.h pad_late_enum) */
            { void dipad_present(CPU *c); dipad_present(c); }
            /* PRESENTBT: work out the structure of the main loop. */
            { const char *pb = ENVC("PRESENTBT");
              static int nb = 0;
              if (pb && nb < 2 && g_present_no >= (uint32_t)atoi(pb)){ nb++;
                  fprintf(stderr,"  [main-loop] Present #%u:\n", g_present_no);
                  guest_backtrace(c, 26); } }
            /* To measure the steady state: at the given frame, reset
             * coverage and start the sampler.  That shows "what is running
             * now" rather than "everything since startup". */
            { static int done = 0; const char *ra2 = ENVC("COVRESET_AT");
              uint32_t at = ra2 ? (uint32_t)atoi(ra2) : 0;
              if (!done && at && g_present_no >= at){ done = 1;
                  cov_reset();
                  fprintf(stderr,"  [measure] frame %u: coverage reset, sampler started\n",
                          g_present_no); }
              }
            g_present++;
            { void vclock_tick(void); vclock_tick(); }
            /* Game speed = real fps / 60, so throttle the frame to 60 Hz
             * (DEFAULT: NO throttle; PACE60=1 locks it to 60 Hz). */
            { void frame_pace(void); frame_pace(); }
            /* COVRESET=<Present#>: reset coverage from that frame on so the
             * report shows ONLY the code that runs in the steady state. */
            { const char *cr = ENVC("COVRESET");
              if (cr && g_present_no == (uint32_t)atoi(cr)){
                  void cov_reset(void); cov_reset();
                  fprintf(stderr,"  coverage reset (Present %u)\n", g_present_no);
              } }
            if (metal_available()) metal_present();
            { extern int g_perflog; void perflog_guest_frame(void);
              if (g_perflog) perflog_guest_frame(); }
            { void mprot_rearm(void); mprot_rearm(); }
            if (g_present_no == 100){
                if (ENVC("HASHSCAN")){
                    static const uint32_t want[] = {0x3EE7363Fu, 0x04191C37u};
                    static const char *nmz[] = {"WORLDVIEWPROJECTION","TEXTUREOFFSET"};
                    for (int hz=0; hz<2; hz++){
                        int cnt=0; uint32_t first[6]; int nf2=0;
                        for (uint32_t a=0x10000000u; a<0x10800000u; a+=4){
                            if (!mem_is_committed(a+4)){ a += 0x3FFCu; continue; }
                            if (MR32(a)==want[hz]){ cnt++; if(nf2<6) first[nf2++]=a; }
                        }
                        fprintf(stderr,"  [hash] %-20s 0x%08X -> in %d places:",
                                nmz[hz], want[hz], cnt);
                        for (int q=0;q<nf2;q++) fprintf(stderr," %08x", first[q]);
                        fprintf(stderr,"\n");
                    }
                }
            }
            /* RESETAT=<frame>: cause a device loss at that frame (test) */
            { const char *ra = ENVC("RESETAT");   /* several frames, comma-separated */
              int hitr = 0;
              for (const char *q = ra; q && *q; ){ if ((uint32_t)atoi(q) == g_present_no) hitr = 1;
                  while (*q && *q != ',') q++; if (*q) q++; }
              if (hitr){
                  fprintf(stderr,"com: RESETAT: device lost at frame %u\n", g_present_no);
                  com_request_reset(); } }
            r=0;
            if (atomic_load(&g_lost)){ r = 0x88760868u;   /* D3DERR_DEVICELOST */
                if (g_lost_presents++ < 4) fprintf(stderr,"com: Present -> DEVICELOST\n"); }
            }
        else if (MID(StretchRect)){
            /* (this, pSrcSurface, pSrcRect, pDstSurface, pDstRect, Filter)
             * had NEVER been implemented (measured: 2261 calls in a session).
             * The game uses it to copy the scene from the back buffer into a
             * texture; post-processing (bleach bypass / motion blur / DOF)
             * reads from that texture.  Without the copy the source stayed
             * empty and the composite came out BLACK -- that was why the main
             * menu went completely dark at high detail.
             * Surface layout: +28 = Metal texture id, +40 = origin
             * (1 = back buffer, 2 = texture surface, 4 = active target). */
            uint32_t ss=A(c,1), ds=A(c,3);
            uint32_t stex = (ss && mem_is_committed(ss+31)) ? MR32(ss+28) : 0u;
            uint32_t dtex = (ds && mem_is_committed(ds+31)) ? MR32(ds+28) : 0u;
            if (ENVC("BLITLOG")){
                static int nb2=0;
                if (nb2 < 8){ nb2++;
                    uint32_t so=(ss&&mem_is_committed(ss+43))?MR32(ss+40):0;
                    uint32_t doo=(ds&&mem_is_committed(ds+43))?MR32(ds+40):0;
                    fprintf(stderr,"  [blit] source tex=%u origin=%u (%ux%u) -> target tex=%u origin=%u (%ux%u)\n",
                            stex, so, ss?MR32(ss+4):0, ss?MR32(ss+8):0,
                            dtex, doo, ds?MR32(ds+4):0, ds?MR32(ds+8):0); }
            }
            /* A source rectangle (the cover shoot's picture, 0x5C3120: the
             * middle of the screen) is in the screen the game believes it has
             * (g_req_w x g_req_h), whose 640x480 menus res_pick shows in the
             * middle of the back buffer, ws_ui_x/y of its size: one from the
             * back buffer is mapped the same way.  The destination is whole. */
            uint32_t srp = A(c,2);
            int sr[4], *srq = NULL;
            if (srp && mem_is_committed(srp+15)){
                for (int k = 0; k < 4; k++) sr[k] = (int)MR32(srp + 4u*(uint32_t)k);
                uint32_t so = (ss && mem_is_committed(ss+43)) ? MR32(ss+40) : 0;
                if (so == 1u && g_req_w && g_req_h){
                    float ux = ws_ui_x(), uy = ws_ui_y();
                    for (int k = 0; k < 4; k += 2){
                        sr[k]   = (int)lroundf((float)g_bb_w * (0.5f + ((float)sr[k]   / (float)g_req_w - 0.5f) * ux));
                        sr[k+1] = (int)lroundf((float)g_bb_h * (0.5f + ((float)sr[k+1] / (float)g_req_h - 0.5f) * uy));
                    }
                }
                srq = sr;
            }
            if (metal_available() && (stex || dtex) && stex != dtex){
                void metal_blit_rect(unsigned,unsigned,const int*);
                metal_blit_rect(stex, dtex, srq);
            }
            r=0; }
        else if (MID(SetRenderTarget)){
            if (ENVC("CUBELOG")){
                static unsigned n3 = 0;
                uint32_t sf2 = A(c,2);
                if (n3 < 8 && sf2 && mem_is_committed(sf2+43) && MR32(sf2+40)==2u){
                    n3++;
                    fprintf(stderr,"  [cube] SetRenderTarget CUBE FACE 0x%08x\n", sf2);
                }
            }
            /* (this, RenderTargetIndex, pRenderTarget) */
            uint32_t idx=A(c,1), surf=A(c,2);
            if (idx < 4u) bind_set(&g_bind_rt[idx], surf);
            if (idx==0 && metal_available()){
                uint32_t tid = (surf && mem_is_committed(surf+35)) ? MR32(surf+28) : 0;
                { static uint64_t org[8]; static int shown=0;
                  uint32_t o5 = (surf && mem_is_committed(surf+43)) ? MR32(surf+40) : 7;
                  if (o5 < 8) org[o5]++;
                  if (ENVC("RTLOG") && !shown && (org[0]+org[1]+org[2]+org[3]+org[4]+org[7]) % 400 == 0){
                    fprintf(stderr,"  [RT] origin counts: none=%llu backbuf=%llu surfaceLvl=%llu"
                            " created=%llu getRT=%llu unknown=%llu\n",
                            (unsigned long long)org[0],(unsigned long long)org[1],
                            (unsigned long long)org[2],(unsigned long long)org[3],
                            (unsigned long long)org[4],(unsigned long long)org[7]); } }
                uint32_t face = (surf && mem_is_committed(surf+47)) ? MR32(surf+44) : 0u;
                g_cur_rt = tid;
                metal_set_render_target(tid | ((face & 7u) << 16));
                if (tid) g_rt_set++; else g_rt_screen++;
            }
            r=0; }
        else if (MID(SetTransform)){
            static int nt=0;
            uint32_t ty=A(c,1), mp=A(c,2);
            /* XFORMAT=<frame>: ALL SetTransform calls of that frame only. */
            { static long xa = -2; if (xa == -2){ const char *e = ENVC("XFORMAT"); xa = e ? atol(e) : -1; }
              if (xa >= 0 && (long)g_present_no == xa && mp && mem_is_committed(mp+63)){
                const char *tn = ty==2?"VIEW":(ty==3?"PROJECTION":(ty==256?"WORLD":"?"));
                fprintf(stderr,"  [xf@%ld] type=%u(%s) caller=0x%08x\n", xa, ty, tn, c->eip);
                for (int rw=0; rw<4; rw++)
                    fprintf(stderr,"      %10.4f %10.4f %10.4f %10.4f\n",
                            MRF32(mp+rw*16), MRF32(mp+rw*16+4),
                            MRF32(mp+rw*16+8), MRF32(mp+rw*16+12)); } }
            if (ENVC("XFORM") && nt<9 && mp && mem_is_committed(mp+63)){ nt++;
                const char *tn = ty==2?"VIEW":(ty==3?"PROJECTION":(ty==256?"WORLD":"?"));
                fprintf(stderr,"  [SetTransform] type=%u(%s)\n", ty, tn);
                for (int rw=0; rw<4; rw++)
                    fprintf(stderr,"      %10.4f %10.4f %10.4f %10.4f\n",
                            MRF32(mp+rw*16), MRF32(mp+rw*16+4),
                            MRF32(mp+rw*16+8), MRF32(mp+rw*16+12));
            }
            r=0; }
        else if (MID(SetViewport)){
            static int nv2=0;
            uint32_t vp2=A(c,1);
            if (ENVC("XFORM") && nv2<6 && vp2 && mem_is_committed(vp2+23)){ nv2++;
                fprintf(stderr,"  [SetViewport] x=%u y=%u g=%u y=%u minZ=%.3f maxZ=%.3f\n",
                        MR32(vp2), MR32(vp2+4), MR32(vp2+8), MR32(vp2+12),
                        MRF32(vp2+16), MRF32(vp2+20));
            }
            r=0; }
        else if (MID(SetVertexShaderConstantI)){
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            for (uint32_t k=0; k<n && sr+k<16; k++){
                uint32_t o=pd+16u*k;
                if (!mem_is_committed(o+15)) break;
                for (int j2=0;j2<4;j2++) g_vsk.i[sr+k][j2]=(int32_t)MR32(o+4u*j2);
            }
            r=0; }
        else if (MID(SetVertexShaderConstantB)){
            uint32_t sr=A(c,1), pd=A(c,2), n=A(c,3);
            for (uint32_t k=0; k<n && sr+k<16; k++){
                uint32_t o=pd+4u*k;
                if (!mem_is_committed(o+3)) break;
                g_vsk.b[sr+k]=(int32_t)MR32(o);
            }
            r=0; }
        else if (MID(SetVertexDeclaration)){ g_cur_decl=A(c,1); g_raw_decl=g_cur_decl; r=0; }
        else if (MID(Reset)){
            /* (this, D3DPRESENT_PARAMETERS*)
             * The game resets the device when resolution/graphics detail changes.
             * This method had NEVER been implemented; changing a setting crashed. */
            uint32_t pp3=A(c,1);
            /* D3D9: a Reset that fails leaves the device lost (Present D3DERR_DEVICELOST,
             * TestCooperativeLevel D3DERR_DEVICENOTRESET) until a Reset succeeds; a game
             * that went on drawing after a failed Reset crashed (GAME.md 5.7).  It fails on
             * no parameters, more than 3 back buffers (D3DPRESENT_BACK_BUFFERS_MAX) and,
             * with game.h d3d9_strict, a size past 8192 a side: the renderer's limit, not
             * D3D9's (metal_resize ignores a larger size; UG2 keeps its old size there). */
            int rbad = !pp3 || !mem_is_committed(pp3) || !mem_is_committed(pp3+55u);
            if (!rbad && MR32(pp3+12) > 3u) rbad = 1;
            if (!rbad && g_game.d3d9_strict && (MR32(pp3) > 8192u || MR32(pp3+4) > 8192u)) rbad = 1;
            if (rbad){
                atomic_store(&g_lost, 1);
                if (g_reset_fails++ < 8) fprintf(stderr, "com: Reset failed (D3DERR_INVALIDCALL): the device stays lost\n");
                r = 0x8876086Cu;
                goto reset_done;
            }
            if (pp3 && mem_is_committed(pp3+7)){
                uint32_t w3=MR32(pp3), h3=MR32(pp3+4);
                fprintf(stderr,"  [D3D] Reset: %ux%u\n", w3, h3);
                if (w3 > 8192u || h3 > 8192u) w3 = h3 = 0;   /* garbage size */
                /* The SAME rule as CreateDevice must apply: Reset used to
                 * pass the raw value through, so changing the resolution
                 * in game made the picture blurry again. */
                if (w3 && h3){ uint32_t d3w = w3, d3h = h3;
                    res_pick(w3, h3, &d3w, &d3h); w3 = d3w; h3 = d3h; }
                if (w3 && h3){ g_bb_w = w3; g_bb_h = h3; }
                if (w3 && h3){
                    void metal_want_size(unsigned,unsigned); metal_want_size(w3,h3);
                    if (metal_available()){ void metal_resize(unsigned,unsigned);
                                            metal_resize(w3,h3); } }
            }
            g_resets++;
            /* The game must have released every render target before Reset
             * (D3D9 rule); release them on the Metal side too, otherwise every
             * resolution change leaked all of them. */
            if (metal_available()){ void metal_reset_rts(void); metal_reset_rts(); }
            if (atomic_exchange(&g_lost, 0))
                fprintf(stderr,"com: device loss closed by Reset (TCL %lu calls)\n", g_tcl_n);
            /* After Reset the state returns to defaults.  D3D9: target 0 goes
             * back to the back buffer, the Z surface to the IMPLICIT Z
             * (EnableAutoDepthStencil), and the game relies on that and never
             * sets either again.  On the Metal side the last target/Z used to
             * stay as it was.  The fullscreen switch hits Reset MID-FRAME via
             * the device-loss path: at that moment a draw into texture 7 with
             * no Z (NULL) was in progress (measured).  Afterwards the main
             * scene was drawn without a Z test -> car inside out, ground
             * reflection black.  Old behaviour: NORTRESET=1 (3/3 broken). */
            if (g_cur_zid != 1u)
                fprintf(stderr, "com: Reset: bound Z surface %u -> implicit Z\n", g_cur_zid);
            /* D3D9 Reset returns ALL device state (render/sampler/stage
             * states) to defaults and the game relies on it: it resets its
             * own state shadow too and never writes again the states it
             * thinks are "already set".  We kept the old values (a Reset
             * landing mid-frame leaked the current pass's state).  No
             * visible difference in the measured runs; done per the D3D9 rule. */
            if (!ENVC("NORSRESET")){
                memset(g_rs, 0, sizeof g_rs); g_rs_init = 0; g_rs_dirty = 1;
                memset(g_smp_raw, 0, sizeof g_smp_raw);
                memset(g_tss_raw, 0, sizeof g_tss_raw);
                for (int q7 = 0; q7 < 8; q7++) g_tss_raw[q7][11] = (uint32_t)q7;
                if (metal_available()){ rs_apply(); g_rs_dirty = 0; }
            }
            if (g_cur_rt)
                fprintf(stderr, "com: Reset: bound render target tex %u -> back buffer\n", g_cur_rt);
            if (!ENVC("NORTRESET")){
            g_cur_zid = 1;
            if (metal_available()){ void metal_set_zsurf(unsigned); metal_set_zsurf(1);
                                    metal_set_render_target(0); } }
            g_cur_tex = 0; g_cur_rt = 0; g_cur_ps_id = 0;
            for (int q6=0;q6<4;q6++) g_tex_stage[q6]=0;
            /* v12na: Reset also unbinds every texture, stream, index buffer,
             * render target and Z surface (D3D9), so the device's references
             * go and the game's releases can free them. */
            if (com_frees()){
                for (int q8 = 0; q8 < 8; q8++){ g_raw_tex[q8] = 0; bind_set(&g_bind_tex[q8], 0); }
                for (int q8 = 0; q8 < 4; q8++){ bind_set(&g_bind_vb[q8], 0); bind_set(&g_bind_rt[q8], 0); }
                bind_set(&g_bind_ib, 0); bind_set(&g_bind_ds, 0);
                g_ss_vb = 0; g_ib_obj = 0;
            }
            r=0;
          reset_done:; }
        else if (MID(SetFVF)){ g_cur_fvf=A(c,1); g_raw_fvf=g_cur_fvf; r=0; }
        else if (MID(CreateTexture)){
            /* (this,W,H,Levels,Usage,Format,Pool,ppTex,pShared) */
            uint32_t w=A(c,1), h=A(c,2), lev=A(c,3), usage=A(c,4), fmt=A(c,5), pool=A(c,6);
            /* With NFSRES active the game creates its post-processing
             * targets at the resolution IT believes in (1280x1024).  The
             * scene is drawn at 1920x1080, then copied into that smaller
             * target and scaled back up: both a resolution loss and a scaled
             * blit.  Pulling full-screen targets to the device size makes
             * the chain 1:1 from start to end. */
            if ((usage & 1u) && g_req_w && w == g_req_w && h == g_req_h){
                static int sr4 = 0;
                if (sr4 < 3){ sr4++;
                    fprintf(stderr,"  [RT-tex] %ux%u -> %ux%u (NFSRES)\n", w,h,g_bb_w,g_bb_h); }
                w = g_bb_w; h = g_bb_h;
            }
            uint32_t pp=A(c,7); uint32_t t=com_new(IF_TEX,160);   /* +64..+103: child surfaces */
            if (!t){ if (pp) MR32(pp)=0; r=0x8007000Eu; goto create_done; }   /* E_OUTOFMEMORY: nothing made */
            MR32(t+4)=w; MR32(t+8)=h; MR32(t+20)=fmt; MR32(t+24)=lev?lev:1u;
            MR32(t+TEX_USAGE)=usage; MR32(t+TEX_POOL)=pool;
            /* HD build: reflection targets grow in Metal while the game keeps
             * seeing the old size (not compiled in the original build). */
            uint32_t mw = w, mh = h, mlev = lev ? lev : 1u;
            int ekind = (usage & 1u) ? enh_rt_size(c->eip, g_bb_w, g_bb_h, &mw, &mh) : ENH_NONE;
            /* Road reflection: full mip chain for the blur pass (see enhance.c). */
            if (ekind == ENH_ROAD){ mlev = 1u; for (uint32_t q = mw > mh ? mw : mh; q > 1u; q >>= 1) mlev++; }
            if (metal_available()){
                /* D3DUSAGE_RENDERTARGET = 1 */
                /* D3DUSAGE_DEPTHSTENCIL = 2: a depth texture (D16, D24X8, D24S8) with 8 stages */
                unsigned metal_create_texture_depth(unsigned, unsigned, unsigned);
                uint32_t tid = (g_game.gpu_stages == 8 && (usage & 2u))
                    ? metal_create_texture_depth(mw,mh,fmt)
                    : metal_create_texture_ex(mw,mh,mlev,fmt,(usage & 1u)?1u:0u);
                MR32(t+28)=tid;
                if (ekind == ENH_ROAD && tid){ void metal_hd_mark_road(unsigned,unsigned,unsigned);
                                               metal_hd_mark_road(tid, w, h); }
                if ((usage & 1u) && ENVC("RTIDLOG"))
                    fprintf(stderr,"  [RT-id] %ux%u (metal %ux%u) id=%u caller=0x%08x\n",
                            w, h, mw, mh, tid, c->eip);
                if (usage & 1u){ g_rt_tex++;
                    static int sr3=0;
                    if (sr3<6){ sr3++;
                        fprintf(stderr,"  [RT-tex] %ux%u fmt=%u levels=%u -> id=%u\n",
                                w,h,fmt,lev?lev:1u,tid); } }
                if (tid && !(usage & 1u) && !metal_tex_is_depth(tid)) texrep_created(tid, w, h, mlev, fmt, usage, pool);
                if (!tid){ g_tex_fail++;
                    static uint32_t bad[16]; static int nb=0; int seen=0;
                    for (int q=0;q<nb;q++) if (bad[q]==fmt) seen=1;
                    if (!seen && nb<16){ bad[nb++]=fmt;
                        fprintf(stderr,"  [tex] UNSUPPORTED format %u (0x%x) %ux%u usage=0x%x\n",
                                fmt, fmt, w, h, usage); }
                    /* D3D9: a texture the device cannot make fails its creation, so the game
                     * takes its fallback (game.h d3d9_strict; UG2 keeps the empty texture) */
                    if (g_game.d3d9_strict){ com_release(t); if (pp) MR32(pp)=0; r=0x8876086Cu; goto create_done; }
                }
            }
            { static uint64_t fmt_hist[64]; static int sh=0;
              /* D3DFMT: 21=A8R8G8B8 22=X8R8G8B8 23=R5G6B5 25=A1R5G5B5 26=A4R4G4B4
                 827611204='DXT1' 861165636='DXT3' 894720068='DXT5' */
              const char *fn2 = "?";
              switch(fmt){ case 21: fn2="A8R8G8B8"; break; case 22: fn2="X8R8G8B8"; break;
                           case 23: fn2="R5G6B5"; break; case 25: fn2="A1R5G5B5"; break;
                           case 26: fn2="A4R4G4B4"; break; case 28: fn2="A8"; break;
                           case 0x31545844: fn2="DXT1"; break; case 0x33545844: fn2="DXT3"; break;
                           case 0x35545844: fn2="DXT5"; break; case 41: fn2="L8"; break; }
              if (fmt<64) fmt_hist[fmt]++;
              if (VB() && sh<12){ sh++;
                fprintf(stderr,"  [tex] %ux%u lev=%u usage=0x%x fmt=%u(%s) pool=%u\n",
                        w,h,lev,usage,fmt,fn2,pool); } }
            if(pp) MR32(pp)=t; r=0;
          create_done:; }
        else if (MID(CreateCubeTexture)){
            /* (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pShared)
             * Used to return an empty COM object: no size/format, no Metal
             * texture -> the reflection path read garbage values. */
            uint32_t edge=A(c,1), lev=A(c,2), usage=A(c,3), fmt=A(c,4), pp=A(c,6);
            uint32_t t2=com_new(IF_TEX,160);        /* wide enough for the face buffers */
            /* nothing made: say so (a cube reported made but NULL left the game no fallback, GAME.md 5.7) */
            if (!t2){ if (pp) MR32(pp)=0; r=0x8007000Eu; goto cube_create_done; }
            if (!edge || edge > 4096u) edge = 256u;
            MR32(t2+4)=edge; MR32(t2+8)=edge; MR32(t2+20)=fmt?fmt:22u;
            MR32(t2+24)=lev?lev:1u;
            MR32(t2+TEX_USAGE)=usage; MR32(t2+TEX_POOL)=A(c,5);
            uint32_t medge = edge, medge2 = edge;
            if (usage & 1u) enh_rt_size(c->eip, g_bb_w, g_bb_h, &medge, &medge2);
            if (metal_available()){
                unsigned metal_create_texture_cube(unsigned,unsigned,unsigned,unsigned);
                /* A cube texture is ALWAYS a render target: the game draws the
                 * environment map into the six faces and then samples it. */
                MR32(t2+28)= getenv("NOCUBE")
                    ? metal_create_texture_ex(medge,medge,lev?lev:1u,fmt?fmt:22u,1u)
                    : metal_create_texture_cube(medge,lev?lev:1u,fmt?fmt:22u,1u);
                if (!MR32(t2+28) && g_game.d3d9_strict){ com_release(t2); if (pp) MR32(pp)=0; r=0x8876086Cu; goto cube_create_done; }
            }
            cube_mark(t2);                          /* CUBE TEXTURE record */
            if(pp) MR32(pp)=t2; r=0;
          cube_create_done:; }
        else if (MID(GetCubeMapSurface)){
            /* (this, FaceType, Level, ppCubeMapSurface) */
            uint32_t self2=A(c,0), pp=A(c,3); uint32_t sf=tex_child(self2, A(c,1) < 6u ? A(c,1) : 0u, A(c,2), 1);
            if (!sf){ if(pp) MR32(pp)=0; r=0x8007000Eu; goto cube_done; }
            com_addref(self2);
            MR32(sf+40)=2;
            /* +44: FACE index + 1 (0 = not a cube surface).  SetRenderTarget
             * packs this into the target field; otherwise all six faces are
             * drawn on top of each other into the same texture. */
            { uint32_t face = A(c,1); MR32(sf+44) = (face < 6u) ? face + 1u : 1u; }
            if (self2 && mem_is_committed(self2+31)){
                MR32(sf+4)=MR32(self2+4); MR32(sf+8)=MR32(self2+8);
                MR32(sf+20)=MR32(self2+20); MR32(sf+28)=MR32(self2+28);
            }
            if(pp) MR32(pp)=sf; r=0;
          cube_done:; }
        else if (MID(CreateVolumeTexture)){ uint32_t pp=A(c,8), v=com_new(IF_TEX,160);   /* same layout as 2D (child slots) */
            /* (this, Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pShared).  The
             * renderer has no volume textures, so nothing is made there; with game.h d3d9_strict the object
             * keeps what it was made as, for GetType, GetLevelCount and GetLevelDesc.  Its levels
             * (GetVolumeLevel, LockBox) are not modelled: Most Wanted makes none (its device calls through
             * [0x982BDC] create textures and cubes only, and it imports no D3DX volume function). */
            if (v && g_game.d3d9_strict){
                CH_FLAGS(v) |= CF_VOLUME;
                MR32(v+4)=A(c,1); MR32(v+8)=A(c,2); MR32(v+TEX_DEPTH)=A(c,3); MR32(v+24)=A(c,4)?A(c,4):1u;
                MR32(v+TEX_USAGE)=A(c,5); MR32(v+20)=A(c,6); MR32(v+TEX_POOL)=A(c,7);
            }
            if(pp) MR32(pp)=v;
            r = v ? 0u : 0x8007000Eu; }
        /* Every creator says when it made nothing (E_OUTOFMEMORY, the pointer NULL): an
         * object that reads as made but is missing leaves the game no fallback (GAME.md 5.7) */
        else if (MID(CreateVertexBuffer)||MID(CreateIndexBuffer)){
            /* (this, Length, Usage, FVF | Format, Pool, ppBuffer, pShared) */
            int ib = MID(CreateIndexBuffer);
            uint32_t pp=A(c,5); uint32_t b=com_new(ib ? IF_IB : IF_VB,64);
            int dyn = (A(c,2) & 0x200u) != 0;            /* D3DUSAGE_DYNAMIC */
            int kind = ib ? (dyn ? GK_IBDYN : GK_IB) : (dyn ? GK_VBDYN : GK_VB);
            /* host backing: the contents outside the guest's window, seen there between Lock and Unlock */
            uint32_t x = 0;
            if (b && !dyn && A(c,1) && hb_on()){ hb_drop(b + 8u); x = hb_make(b + 8u, A(c,1), kind); }
            uint32_t mem = (b && !x) ? gbuf_alloc_k(A(c,1)?A(c,1):64, kind) : 0u;
            int made = b != 0;
            if (b && !mem && !x){ com_release(b); b = 0; }
            if (!b)                         /* the game seldom checks: say so, for the fault that follows */
                fprintf(stderr, "[d3d] %s(%u bytes, usage 0x%x, pool %u) failed: no %s (ret 0x%08x)
",
                        ib ? "CreateIndexBuffer" : "CreateVertexBuffer", A(c,1), A(c,2), A(c,4),
                        made ? "memory" : "object", c->eip);
            if (b){
                MR32(b+4)=A(c,1); MR32(b+8)=mem; MR32(b+BUF_HB)=x;
                MR32(b+48)=buf_serial();    /* mirror generation: unique even if the address is reused */
                if (ib) MR32(b+12)=A(c,3);  /* Format: 101=16bit, 102=32bit */
                else MR32(b+BUF_FVF)=A(c,3);
                MR32(b+BUF_USAGE)=A(c,2); MR32(b+BUF_POOL)=A(c,4);
            }
            if(pp) MR32(pp)=b;
            r = b ? 0u : 0x8007000Eu; }
        else if (MID(CreateRenderTarget)){
            /* (this,W,H,Format,MultiSample,MSQuality,Lockable,ppSurface,pShared) */
            uint32_t w2=A(c,1), h2=A(c,2), f2=A(c,3), pp=A(c,7);
            uint32_t sf=com_new(IF_SURF,64);
            r = sf ? 0u : 0x8007000Eu;
            if (sf){
                MR32(sf+4)=w2; MR32(sf+8)=h2; MR32(sf+20)=f2; MR32(sf+40)=3;
                MR32(sf+SURF_USAGE)=1u;                 /* D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT */
                if (metal_available()) MR32(sf+28)=metal_create_texture_ex(w2,h2,1,f2,1);
                /* the renderer could not make it (game.h d3d9_strict) */
                if (metal_available() && !MR32(sf+28) && g_game.d3d9_strict){ com_release(sf); sf = 0; r = 0x8876086Cu; }
            }
            if(pp) MR32(pp)=sf; }
        else if (MID(CreateDepthStencilSurface)){
            uint32_t pp=A(c,7); uint32_t sf=com_new(IF_SURF,64);
            if (!sf){ if(pp) MR32(pp)=0; r=0x8007000Eu; goto ds_done; }
            MR32(sf+4)=A(c,1); MR32(sf+8)=A(c,2); MR32(sf+20)=A(c,3); MR32(sf+40)=5;
            MR32(sf+SURF_USAGE)=2u;                     /* D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT */
            /* Z surface id (metal.m g_ztex): 1 implicit, >=2 created.
             * Never a live surface's id, and a released one only three
             * presents later (zid_take); the texture is reset below. */
            uint32_t zid = zid_take();
            MR32(sf+52) = zid; MR32(sf+56) = ZSURF_MAGIC;
            if (metal_available()){ void metal_z_forget(unsigned); metal_z_forget(zid); }
            if(pp) MR32(pp)=sf; r=0;
          ds_done:; }
        else if (MID(CreateOffscreenPlainSurface)){
            /* (this, Width, Height, Format, Pool, ppSurface, pShared) */
            uint32_t pp=A(c,5), sf=com_new(IF_SURF,64);
            if (sf && g_game.d3d9_strict){ MR32(sf+4)=A(c,1); MR32(sf+8)=A(c,2); MR32(sf+20)=A(c,3); MR32(sf+SURF_POOL)=A(c,4); }
            if(pp) MR32(pp)=sf;
            r = sf ? 0u : 0x8007000Eu; }
        else if (MID(GetBackBuffer)){ uint32_t pp=A(c,4); uint32_t sf=bb_surface();
            if (sf) com_addref(sf);
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(GetRenderTarget)){
            /* v12na: the surface that is bound (D3D9), with a reference; the
             * back buffer for target 0 when nothing else is.  Each call used
             * to create a new surface object. */
            uint32_t idx=A(c,1), pp=A(c,2), sf = idx < 4u ? g_bind_rt[idx] : 0u;
            if (!sf && idx == 0u) sf = bb_surface();
            if (sf){ com_addref(sf); r=0; } else r=0x88760866u;   /* D3DERR_NOTFOUND */
            if(pp) MR32(pp)=sf; }
        else if (MID(GetDepthStencilSurface)){
            /* The bound Z surface; before any SetDepthStencilSurface the
             * implicit one, after SetDepthStencilSurface(NULL) a "no depth"
             * surface the game can bind back (it does). */
            uint32_t pp=A(c,1), sf=g_bind_ds;
            if (!sf) sf = z_surface(g_cur_zid ? 1u : 0u);
            if (sf) com_addref(sf);
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(SetDepthStencilSurface)){
            /* (this, pNewZStencil).  NULL -> Z test/write disabled. */
            uint32_t sf=A(c,1), zid=0;
            bind_set(&g_bind_ds, sf);
            if (sf){
                zid = 1;   /* unrecognised surface: implicit */
                if (mem_is_committed(sf+59) && MR32(sf+56)==ZSURF_MAGIC) zid = MR32(sf+52);
                /* a depth texture's level (8 stages): that texture as the Z (gpu-abi.md 11.4) */
                else if (g_game.gpu_stages == 8 && mem_is_committed(sf+43) && MR32(sf+40) == 2u){
                    int metal_tex_is_depth(unsigned);
                    uint32_t tid = MR32(sf+28);
                    if (metal_tex_is_depth(tid)) zid = 64u + tid;             /* GW_Z_TEX */
                }
            }
            g_cur_zid = zid;
            if (metal_available()){ void metal_set_zsurf(unsigned); metal_set_zsurf(zid); }
            r=0; }
        else if (MID(CreateVertexDeclaration)){
            uint32_t el=A(c,1), pp=A(c,2);
            { static int sh=0;
              if (VB() && sh<16 && el){ sh++;
                static const char *TY[]={"FLOAT1","FLOAT2","FLOAT3","FLOAT4","D3DCOLOR",
                    "UBYTE4","SHORT2","SHORT4","UBYTE4N","SHORT2N","SHORT4N","USHORT2N",
                    "USHORT4N","UDEC3","DEC3N","FLOAT16_2","FLOAT16_4","UNUSED"};
                static const char *US[]={"POSITION","BLENDWEIGHT","BLENDINDICES","NORMAL",
                    "PSIZE","TEXCOORD","TANGENT","BINORMAL","TESSFACTOR","POSITIONT",
                    "COLOR","FOG","DEPTH","SAMPLE"};
                fprintf(stderr,"  [vdecl] declaration:\n");
                for (uint32_t k=0;k<32;k++){
                    uint32_t e=el+8u*k;
                    if (!mem_is_committed(e+7)) break;
                    uint32_t st=MR16(e), of=MR16(e+2);
                    uint32_t ty=MR8(e+4), me=MR8(e+5), us=MR8(e+6), ui=MR8(e+7);
                    if (st==0xFFu && ty==17u){ fprintf(stderr,"     [%u] END\n",k); break; }
                    fprintf(stderr,"     [%u] stream=%u offset=%-3u type=%-8s usage=%-10s idx=%u (method=%u)\n",
                            k, st, of, ty<18?TY[ty]:"?", us<14?US[us]:"?", ui, me);
                }
              } }
            uint32_t h=com_new(IF_VDECL,32);
            if (!h){ if(pp) MR32(pp)=0; r=0x8007000Eu; goto decl_done; }   /* nothing made: say so */
            /* Declarations and shaders are kept for the session: the GPU
             * vertex-shader cache (gpuvs_id) and the compiled programs are
             * keyed by them, and a reused address would hit a stale entry.
             * The game creates them once (79 + 68 objects). */
            CH_FLAGS(h) |= CF_DEVICE;
            uint32_t n=0;
            if (el){
                for(; n<64; n++){
                    uint32_t e=el+8u*n;
                    if (!mem_is_committed(e+7)) break;
                    if (MR16(e)==0xFFu && MR8(e+4)==17u){ n++; break; }
                }
            }
            uint32_t bytes = 8u*n;
            uint32_t copy = bytes ? gbuf_alloc(bytes) : 0;
            if (copy && el) memcpy(g_mem+copy, g_mem+el, bytes);
            MR32(h+4)=copy; MR32(h+8)=bytes;
#ifdef __wasm__
            if (wgsl_dump_on() && copy) wgsl_dump_decl(h, g_mem+copy, bytes);
#endif
            if(pp) MR32(pp)=h; r=0;
          decl_done:; }
        else if (MID(CreateVertexShader)||MID(CreatePixelShader)){
            uint32_t fn=A(c,1), pp=A(c,2);
            uint32_t h=com_new(IF_VSHADER,32);
            if (!h){ if(pp) MR32(pp)=0; r=0x8007000Eu; goto shader_done; }   /* nothing made: say so */
            CH_FLAGS(h) |= CF_DEVICE;                   /* kept: see CreateVertexDeclaration */
            /* scan the shader token stream: up to the 0x0000FFFF END token */
            uint32_t sz=0;
            if (fn){ uint32_t k=0;
                for (; k<16384; k++){
                    if (!mem_is_committed(fn+4u*k)) break;
                    uint32_t t=MR32(fn+4u*k); if(t==0x0000FFFFu){ k++; break; }
                }
                sz = 4u*k; }
            uint32_t copy = sz ? gbuf_alloc(sz) : 0;
            if (copy && fn) memcpy(g_mem+copy, g_mem+fn, sz);
            MR32(h+4)=copy; MR32(h+8)=sz; MR32(h+12)=0;
            /* PSDUMP: pixel shaders use the same token format; compile and
             * disassemble them to see WHAT they do. */
            if (MID(CreatePixelShader) && copy && sz >= 8 && ENVC("PSDUMP")){
                static int pn = 0;
                if (pn < atoi(ENVC("PSDUMP"))){
                    VSProg *pp2 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                    if (pp2 && vs_ninstr(pp2) < 5u){ pp2 = 0; }   /* skip simple UI */
                    if (pp2){ pn++;
                        fprintf(stderr,"  --- PIXEL SHADER #%d (%u instructions) ---\n",
                                pn, vs_ninstr(pp2));
                        vs_disasm(pp2, stderr);
                        { static char msl[16384];
                          int len = ps_to_msl(pp2, pn, msl, sizeof msl);
                          if (len > 0) fprintf(stderr, "--- GENERATED MSL ---\n%s\n", msl);
                          else fprintf(stderr, "--- MSL not generated ---\n"); }
                    }
                }
            }
            /* Translate the pixel shader to MSL and register it with Metal:
             * the game's own shader runs on the GPU UNCHANGED. */
            if (MID(CreatePixelShader) && copy && sz >= 8){
                VSProg *pp3 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                if (pp3){
                    static char msl[SHADER_TEXT_CAP];
                    int id = ++g_psn;
                    void metal_register_ps(int, const char *);
#ifdef __wasm__
                    if (wgsl_dump_on()) wgsl_dump_obj("ps", id, g_mem+copy, sz, pp3);
                    int pl = ps_to_wgsl(pp3, id, msl, sizeof msl);
                    if (wgsl_dump_on()) wgsl_dump_ps(id, id, msl, pl);
#else
                    int pl = ps_to_msl(pp3, id, msl, sizeof msl);
#endif
                    if (pl > 0){
                        metal_register_ps(id, msl);
                        MR32(h+12) = (uint32_t)id;
                    }
                    /* CUBE VARIANT (id + PSCUBE): in D3D9, ps_1_x does not
                     * declare the texture TYPE; the type comes from the
                     * binding.  The car environment map binds a cube texture
                     * to stage 0; if a shader expecting texture2d is bound
                     * to that slot, Metal rejects the draw
                     * (validateCommonDrawErrors).  The variant is chosen at
                     * draw time from the bound texture. */
#ifdef __wasm__
                    if (id < PSCUBE){
                        int cl = ps_to_wgsl_ex(pp3, id + PSCUBE, msl, sizeof msl, 1u);
                        if (wgsl_dump_on()) wgsl_dump_ps(id + PSCUBE, id, msl, cl);
                        if (cl > 0) metal_register_ps(id + PSCUBE, msl);
                    }
#else
                    if (id < PSCUBE &&
                        ps_to_msl_ex(pp3, id + PSCUBE, msl, sizeof msl, 1u) > 0)
                        metal_register_ps(id + PSCUBE, msl);
#endif
#if NFS_ENHANCED
                    /* HD: the reflection-split variant is generated at draw
                     * time when needed (com_ps_refl_variant); the program is kept. */
                    if (id < PSCUBE && !g_psprog[id]) g_psprog[id] = pp3;
                    else
#endif
                    if (g_game.gpu_stages == 8 && pl > 0 && id < PSCUBE && !g_ps8[id]) g_ps8[id] = pp3;
                    else
                    vs_free(pp3);
                }
            }
            if (MID(CreateVertexShader) && copy && sz >= 8 && g_vsprogn < MAXVS){
                VSProg *vp2 = vs_compile((const uint32_t *)(g_mem+copy), sz/4u);
                if (vp2){ g_vsprog[g_vsprogn] = vp2; MR32(h+12) = g_vsprogn++;
#ifdef __wasm__
                    if (wgsl_dump_on()) wgsl_dump_obj("vs", (int)g_vsprogn - 1, g_mem+copy, sz, vp2);
#endif
                    if (ENVC("VSDUMP")){
                        static int vd2 = 0;
                        if (vd2 < atoi(ENVC("VSDUMP")) && vs_ninstr(vp2) >= 10u){ vd2++;
                            static int dcl[16][2];
                            for (int q7=0;q7<16;q7++){ dcl[q7][0]=-1; dcl[q7][1]=0; }
                            dcl[0][0]=0;  dcl[0][1]=2;    /* pos  FLOAT3 @0  */
                            dcl[1][0]=12; dcl[1][1]=4;    /* col  D3DCOLOR   */
                            dcl[2][0]=16; dcl[2][1]=1;    /* uv   FLOAT2     */
                            static char msl2[24576];
                            int L2 = vs_to_msl(vp2, (int)g_vsprogn, dcl, msl2, sizeof msl2);
                            fprintf(stderr,"--- VERTEX SHADER #%u (%u instructions) MSL ---\n%s\n",
                                    g_vsprogn, vs_ninstr(vp2), L2>0?msl2:"(not generated)");
                        }
                    }
                }
            }
            if(pp) MR32(pp)=h; r=0;
          shader_done:; }
        else if (MID(BeginStateBlock)){
            /* D3D9: a nested Begin is INVALID (the second returns
             * D3DERR_INVALIDCALL).  The game does it (845 and 846 back to
             * back in the trace); counting the second as a new block lost
             * the outer one. */
            if (g_sb_rec) r=0x8876086Cu;          /* D3DERR_INVALIDCALL */
            else {
                int slot = 0;
                for (int q = 1; q < g_sbn; q++) if (!g_sb[q].used){ slot = q; break; }
                if (!slot && g_sbn < MAXSB) slot = g_sbn++;
                if (slot){ g_sb_rec = slot; g_sb[slot].n = 0; g_sb[slot].used = 1; }
                else { g_sb_rec = 0; g_sb_full++; }
                r=0;
            } }
        else if (MID(CreateStateBlock)){
            /* Captures a predefined set (D3DSBT_ALL etc.).  Never used in
             * the trace; we leave an empty block. */
            uint32_t pp=A(c,2);
            r=0;
            if(pp){ uint32_t o=com_new(IF_STATEBLOCK,32); if (o) MR32(o+8)=0; else r=0x8007000Eu; MR32(pp)=o; } }
        else if (MID(CreateQuery)){ uint32_t pp=A(c,2), o=pp ? com_new(IF_QUERY,32) : 0u;
            if(pp) MR32(pp)=o;
            r = (pp && !o) ? 0x8007000Eu : 0u; }
        else if (MID(EndStateBlock)){
            uint32_t pp=A(c,1);
            uint32_t o=com_new(IF_STATEBLOCK,32);
            if (o) MR32(o+8) = (uint32_t)g_sb_rec;  /* block index */
            else if (g_sb_rec){ g_sb[g_sb_rec].used = 0; g_sb[g_sb_rec].n = 0; }   /* nothing made: the slot goes back */
            g_sb_rec = 0;
            if(pp) MR32(pp)=o;
            r = o ? 0u : 0x8007000Eu; }
        else if (MID(CreateAdditionalSwapChain)){ uint32_t pp=A(c,2), o=pp ? com_new(IF_SWAPCHAIN,32) : 0u;
            if(pp) MR32(pp)=o;
            r = (pp && !o) ? 0x8007000Eu : 0u; }
        else if (MID(GetSwapChain)){ uint32_t pp=A(c,2), sc = dev_surface_kind(&g_swap0, IF_SWAPCHAIN);
            if (sc) com_addref(sc);
            if(pp) MR32(pp)=sc; r=0; }
        else if (MID(GetNumberOfSwapChains)) r=1;
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            /* fill the output parameter with a safe default */
            uint32_t p=A(c,nargs-1); uint32_t sz=4;
            if      (MID(GetTransform))           sz=64;
            else if (MID(GetMaterial))            sz=68;
            else if (MID(GetLight))               sz=104;
            else if (MID(GetViewport))            sz=24;
            else if (MID(GetScissorRect))         sz=16;
            else if (MID(GetClipStatus))          sz=8;
            else if (MID(GetCreationParameters))  sz=16;
            else if (MID(GetRasterStatus))        sz=8;
            else if (MID(GetGammaRamp))           sz=1536;
            else if (MID(GetClipPlane))           sz=16;
            else if (!strncmp(nm,"GetVertexShaderConstant",23)
                  || !strncmp(nm,"GetPixelShaderConstant",22)) sz=16*4;
            if (p && p < 0xE0000000u){ mem_commit(p,sz); memset(g_mem+p,0,sz); }
            if (MID(GetViewport) && p){ MR32(p+8)=1280; MR32(p+12)=720; MRF32(p+20)=1.0f; }
            r=0; }
        else r=0;                        /* remaining Set* -> D3D_OK */
    }
    /* --- resources --- */
    else if (iface==IF_TEX || iface==IF_VB || iface==IF_IB || iface==IF_SURF){
        uint32_t self=A(c,0);
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; com_addref(g_dev); r=0; }   /* D3D9: a reference */
        else if (MID(Lock)){
            uint32_t off=A(c,1), sz=A(c,2), pp=A(c,3);
            /* PERSISTENT POOL: the contents may change -> advance the mirror
             * generation and treat the buffer as "volatile" for a few frames
             * (see dip_indexed). */
            if (self && (iface==IF_VB || iface==IF_IB)){
                MR32(self+48) = MR32(self+48) + 1u; MR32(self+52) = (uint32_t)g_present; }
            /* host backing: a window of the whole buffer, filled from the copy, the pointer at the offset */
            uint32_t x = (self && (iface==IF_VB || iface==IF_IB) && hb_on()) ? MR32(self+BUF_HB) : 0u;
            if (x && x < HB_N && !g_hb[x].pinned){
                uint32_t size = MR32(self+4), w = MR32(self+8);
                if (off >= size) off = 0;
                if (w) hb_nest(x, A(c,4));              /* locked again before its Unlock: the same window */
                else { w = hb_lock(x, size, A(c,4)); MR32(self+8) = w; }
                if (pp) MR32(pp) = w ? w + off : 0u;
                r = w ? 0u : 0x8876086Cu;                /* no window: D3DERR_INVALIDCALL */
                goto lock_done;
            }
            uint32_t base = self?MR32(self+8):0;
            if(!base){ base=gbuf_alloc(sz?sz:65536); if(self) MR32(self+8)=base; }
            if(pp) MR32(pp)=base+off; r=0;
          lock_done:; }
        else if (MID(LockRect)){
            /* 2D texture  : (Level, pLockedRect, pRect, Flags)
             * CUBE texture: (FaceType, Level, pLockedRect, pRect, Flags)
             * Surface     : (pLockedRect, pRect, Flags)
             * If the cube signature is treated like 2D, Level is read as
             * pLockedRect; the game's real struct is NOT filled and it
             * crashes on a garbage pointer (measured: guest address 0x400). */
            int is_cube = (iface==IF_TEX && is_cube_tex(self));
            uint32_t face  = is_cube ? A(c,1) : 0u;
            uint32_t level = is_cube ? A(c,2) : ((iface==IF_TEX) ? A(c,1) : 0u);
            uint32_t plr   = is_cube ? A(c,3) : ((iface==IF_TEX) ? A(c,2) : A(c,1));
            uint32_t w = self?MR32(self+4):256, h = self?MR32(self+8):256;
            uint32_t fmt = self?MR32(self+20):21u;
            if(!w) w=256; if(!h) h=256;
            if(w>8192) w=8192; if(h>8192) h=8192;
            if(level>7) level=7;
            uint32_t pitch = metal_available() ? metal_level_pitch(w,level,fmt) : 0;
            uint32_t rows  = metal_available() ? metal_level_rows(h,level,fmt)  : 0;
            if(!pitch){ uint32_t lw=w>>level; if(!lw)lw=1; pitch=lw*4u; }
            if(!rows){  uint32_t lh=h>>level; if(!lh)lh=1; rows=lh; }
            uint32_t slot = self ? (is_cube ? (self + 104u + 4u*(face & 7u))
                                            : (self + 32u + 4u*level)) : 0;
            uint32_t buf = slot ? MR32(slot) : 0;
            if(!buf){
                int kind = iface != IF_TEX ? GK_SURF : (MR32(self+TEX_USAGE) & 0x200u) ? GK_TEXDYN : GK_TEX;
                /* host backing: a texture's level (not a dynamic one's, nor a render target's) */
                uint32_t x = (kind == GK_TEX && slot && hb_on() && !(MR32(self+TEX_USAGE) & 1u))
                           ? hb_make(slot, pitch*rows + 64u, kind) : 0u;
                if (x && !g_hb[x].pinned)
                    buf = hb_lock(x, pitch*rows + 64u, is_cube ? A(c,5) : A(c,4));
                else buf = gbuf_alloc_k(pitch*rows + 64u, kind);
                if(slot) MR32(slot)=buf; }
            else if (iface == IF_TEX && hb_on()){      /* locked again before its UnlockRect: the same window */
                uint32_t x = hb_find(slot);
                if (x && !g_hb[x].pinned && g_hb[x].locks) hb_nest(x, is_cube ? A(c,5) : A(c,4));
            }
            /* a render target the game made lockable (CreateRenderTarget):
             * what the GPU drew there, the cover shoot's picture (gpu_web.c) */
            if (iface==IF_SURF && self && MR32(self+40)==3u && MR32(self+28) && metal_available()){
                int metal_read_texture(unsigned, void *, unsigned, unsigned);
                metal_read_texture(MR32(self+28), g_mem+buf, pitch, rows);
            }
            if(plr){ mem_commit(plr,8); MR32(plr)=pitch; MR32(plr+4)=buf; }
            r=0; }
        else if (MID(UnlockRect)){
            /* 2D: (Level)  |  CUBE: (FaceType, Level)  |  Surface: () */
            int is_cube2 = (iface==IF_TEX && is_cube_tex(self));
            uint32_t face2 = is_cube2 ? A(c,1) : 0u;
            uint32_t level = is_cube2 ? A(c,2) : ((iface==IF_TEX) ? A(c,1) : 0u);
            if(level>7) level=7;
            uint32_t w = self?MR32(self+4):0, h = self?MR32(self+8):0;
            uint32_t fmt = self?MR32(self+20):21u, mid = self?MR32(self+28):0;
            uint32_t buf = self ? (is_cube2 ? MR32(self + 104u + 4u*(face2 & 7u))
                                            : MR32(self + 32u + 4u*level)) : 0;
            gheap_check_block(c, buf, "LOCK BUFFER OVERRUN (UnlockRect)");   /* NFSU2_HEAPCHECK only */
            if (metal_available() && mid && buf && w && h)
                metal_upload_texture(mid, level, g_mem+buf,
                                     metal_level_pitch(w,level,fmt), w, h, fmt);
            if (metal_available() && mid && buf && w && h && iface==IF_TEX && !is_cube2)
                texrep_uploaded(mid, level, g_mem+buf, metal_level_pitch(w,level,fmt), metal_level_rows(h,level,fmt));
            else if (iface==IF_SURF && mid) texrep_surface_written(mid);   /* the car skin composite */
            if (ENVC("LOCKLOG") && w==640 && h==480){
                static int nl=0;
                if (nl<3){ nl++;
                    fprintf(stderr,"  [lock] %ux%u fmt=%u lev=%u mid=%u pitch=%u  first 32 bytes:\n    ",
                            w,h,fmt,level,mid, metal_level_pitch(w,level,fmt));
                    for (int q=0;q<32;q++) fprintf(stderr,"%02x ", MR8(buf+q));
                    fprintf(stderr,"\n    second row: ");
                    uint32_t pt = metal_level_pitch(w,level,fmt);
                    for (int q=0;q<16;q++) fprintf(stderr,"%02x ", MR8(buf+pt+q));
                    fprintf(stderr,"\n");
                }
            }
            /* host backing: the window back into the level's copy, and freed, at the last UnlockRect of a
             * nest (each UnlockRect uploads the level, as UG2's path does) */
            if (iface==IF_TEX && self && buf && hb_on()){
                uint32_t sl = is_cube2 ? self + 104u + 4u*(face2 & 7u) : self + 32u + 4u*level;
                uint32_t x = hb_find(sl);
                if (x && hb_unlock(x, buf)) MR32(sl) = 0;
            }
            r=0; }
        else if (MID(Unlock) && (iface==IF_VB || iface==IF_IB)){
            gheap_check_block(c, self ? MR32(self+8) : 0u, "LOCK BUFFER OVERRUN (Unlock)");
            uint32_t x = (self && hb_on()) ? MR32(self+BUF_HB) : 0u;
            if (x && x < HB_N && hb_unlock(x, MR32(self+8))) MR32(self+8) = 0;
            r=0; }
        else if (MID(GetLevelCount)) r=(self&&MR32(self+24))?MR32(self+24):1u;
        else if (MID(GetType) && g_game.d3d9_strict)       /* D3DRESOURCETYPE */
            r = iface==IF_TEX ? (is_cube_tex(self) ? 5u : is_volume_tex(self) ? 4u : 3u)
              : iface==IF_SURF ? 1u : iface==IF_VB ? 6u : 7u;
        else if (MID(GetType)) r=(iface==IF_TEX)?3u:(iface==IF_SURF?1u:(iface==IF_VB?4u:5u));
        else if ((MID(GetDesc)||MID(GetLevelDesc)) && g_game.d3d9_strict) r = res_desc(iface, self, A(c,1), A(c,nargs-1));
        else if (MID(GetSurfaceLevel)||MID(GetCubeMapSurface)){
            /* The surface must carry the Metal id of the texture it belongs
             * to; otherwise SetRenderTarget cannot know which texture to draw to.
             * A CUBE texture uses the same vtable slot but a different signature:
             *   2D   : GetSurfaceLevel(Level, ppSurface)
             *   CUBE : GetCubeMapSurface(FaceType, Level, ppSurface)
             * Treated like 2D, the pointer is written to the WRONG argument. */
            uint32_t self0=A(c,0);
            int cube3 = is_cube_tex(self0);
            uint32_t pp = cube3 ? A(c,3) : A(c,2);
            /* v12na: the texture's own (cached) surface; it shares the texture's
             * reference count, as in D3D9 */
            uint32_t face3 = cube3 ? (A(c,1) < 6u ? A(c,1) : 0u) : 0u, lvl3 = cube3 ? A(c,2) : A(c,1);
            uint32_t sf = tex_child(self0, face3, lvl3, cube3);
            if (!sf){ if(pp) MR32(pp)=0; r=0x8007000Eu; goto surf_done; }
            com_addref(self0);
            MR32(sf+40)=2;
            uint32_t self=A(c,0);
            if (self && mem_is_committed(self+31)){
                MR32(sf+4)=MR32(self+4); MR32(sf+8)=MR32(self+8);
                MR32(sf+20)=MR32(self+20); MR32(sf+28)=MR32(self+28);
            }
            if(pp) MR32(pp)=sf; r=0;
          surf_done:; }
        else if (MID(GetDesc)||MID(GetLevelDesc)){
            /* D3DSURFACE_DESC layout:
             *   0 Format, 4 Type, 8 Usage, 12 Pool,
             *  16 MultiSampleType, 20 MultiSampleQuality,
             *  24 Width, 28 Height
             * Width/height USED TO be written to 16/20 (i.e. the
             * MultiSample fields) -> the game read a size of 0, computed a
             * garbage pointer at high detail and crashed. */
            uint32_t p=A(c,nargs-1);
            if(p){ mem_commit(p,32); memset(g_mem+p,0,32);
                uint32_t fw = self?MR32(self+4):256, fh = self?MR32(self+8):256;
                uint32_t ff = self?MR32(self+20):0;
                if (!fw) fw = 640; if (!fh) fh = 480;
                if (!ff) ff = 22;                    /* D3DFMT_X8R8G8B8 */
                MR32(p)    = ff;                     /* Format          */
                MR32(p+4)  = (iface==IF_TEX) ? 3u : 1u;  /* Type: TEXTURE/SURFACE */
                MR32(p+8)  = 0;                      /* Usage           */
                MR32(p+12) = 0;                      /* Pool: DEFAULT   */
                MR32(p+16) = 0;                      /* MultiSampleType */
                MR32(p+20) = 0;                      /* MS Quality      */
                MR32(p+24) = fw;                     /* Width           */
                MR32(p+28) = fh;                     /* Height          */
            }
            r=0; }
        else r=0;
    }
    /* --- DirectSound --- */
    else if (iface==IF_DS){
        if      (MID(CreateSoundBuffer)){
            uint32_t desc=A(c,1), pp=A(c,2);
            uint32_t b=com_new(IF_DSBUF,64);
            /* DSBUFFERDESC: dwSize@0 dwFlags@4 dwBufferBytes@8 dwReserved@12
             *               lpwfxFormat@16 guid3DAlgorithm@20
             * NOTE: the size used to be read from +12 (dwReserved) -> always
             * 0, falling back to 65536.  The game feeds its stream buffer by
             * the size IT asked for; with our cursor wrapping much faster the
             * game streamed the song too fast and wrote silence where it
             * could not keep up -- "music plays fast and cuts out". */
            uint32_t bytes = (desc && mem_is_committed(desc+11)) ? MR32(desc+8) : 0;
            if (ENVC("SNDBUF"))
                fprintf(stderr, "  [sound] CreateSoundBuffer requested=%u bytes "
                        "(flags=%08x, old-wrong-field=%u)\n",
                        bytes, desc?MR32(desc+4):0, desc?MR32(desc+12):0);
            /* DSLOG=1: which guest subsystem created this buffer?  The SFX
             * buffers (#1-#4) never play, so finding their owner is essential. */
            if (ENVC("DSLOG")){
                fprintf(stderr, "  [sound] --- CreateSoundBuffer #%d call chain ---\n",
                        (int)g_snd_play);
                guest_backtrace(c, 10);
            }
            if(!bytes || bytes>0x400000u) bytes=65536u;
            MR32(b+4)=bytes; MR32(b+8)=gbuf_alloc_k(bytes, GK_SND); MR32(b+12)=0;
            ds_share(MR32(b+8), +1);
            /* Bytes/second from WAVEFORMATEX: the play cursor must advance
             * REALISTICALLY.  The video player drives frames by the audio
             * clock; with a fake cursor playback never ends. */
            { uint32_t bps = 176400u;                /* default 44.1k 16-bit stereo */
              uint32_t pfmt = (desc && mem_is_committed(desc+19)) ? MR32(desc+16) : 0;
              if (pfmt && mem_is_committed(pfmt+15)){
                  uint32_t v = MR32(pfmt+8);         /* nAvgBytesPerSec */
                  if (v >= 8000u && v <= 1000000u) bps = v;
              }
              MR32(b+20)=bps; MR32(b+24)=0;
              /* Register with the CoreAudio mixer.  The primary buffer
               * (DSBCAPS_PRIMARYBUFFER = 0x1) is not played. */
              uint32_t flags = (desc && mem_is_committed(desc+7)) ? MR32(desc+4) : 0;
              uint32_t rate = 44100u, chans = 2u, bits = 16u;
              if (pfmt && mem_is_committed(pfmt+15)){
                  uint32_t ch = MR16(pfmt+2), sr = MR32(pfmt+4), bt = MR16(pfmt+14);
                  /* WAVEFORMATEX(TENSIBLE): wFormatTag@0 nChannels@2 nSamplesPerSec@4
                   * ... wBitsPerSample@14 cbSize@16, then (tag 0xFFFE) Samples@18
                   * dwChannelMask@20.  The EA sound libraries of both games try
                   * 1, 2, 4 and 6 channels at start-up (Most Wanted's 4 and 6
                   * extensible, masks 0x33 and 0x3F), then make their output
                   * with the channel count their settings ask for: 2 by
                   * default (Most Wanted: 0x81ED00; no direct caller of its
                   * setter), so stereo.  Asked for 6, Most Wanted's (0x82007E)
                   * would make a 6-channel output buffer.  Up to 8 channels
                   * are kept as they are and mixed down to stereo
                   * (audio_web.c); 1 and 2 as ever. */
                  if (ENVC("SNDBUF"))
                      fprintf(stderr, "  [sound] CreateSoundBuffer format: tag %04x, %u channels, %u Hz, %u bit, mask %08x\n",
                              MR16(pfmt), ch, sr, bt,
                              (MR16(pfmt) == 0xFFFEu && mem_is_committed(pfmt+23)) ? MR32(pfmt+20) : 0u);
                  if (ch>=1u && ch<=8u) chans = ch;
                  if (sr>=4000u && sr<=192000u) rate = sr;
                  if (bt==8u || bt==16u) bits = bt;
              }
              MR32(b+28)=0;
              if (!(flags & 0x1u)){
                  audio_init();
                  MR32(b+28) = (uint32_t)audio_register(MR32(b+8), bytes, rate, chans, bits);
              } }
            if(pp) MR32(pp)=b; r=0; }
        else if (MID(GetCaps)){
            /* DSCAPS: dwSize@0 dwFlags@4 dwMinSecondarySampleRate@8
             *         dwMaxSecondarySampleRate@12 dwPrimaryBuffers@16
             *         dwMaxHwMixingAllBuffers@20 ...
             * The flags used to be only 0x200 (SECONDARYSTEREO) and
             * dwPrimaryBuffers got 100000.  The game learns the sound
             * capabilities from here. */
            uint32_t p=A(c,1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>256u) sz=0x64u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz;
                   MR32(p+4)=0x00000F1Fu;   /* primary mono/stereo/8/16 +
                                             * secondary mono/stereo/8/16 +
                                             * CONTINUOUSRATE */
                   MR32(p+8)=100u; MR32(p+12)=100000u;
                   MR32(p+16)=1u;           /* dwPrimaryBuffers */
                   MR32(p+20)=64; MR32(p+24)=64;
                   MR32(p+28)=64; MR32(p+32)=64;
                   MR32(p+36)=0x7FFFFFFFu;  /* dwTotalHwMemBytes  */
                   MR32(p+40)=0x7FFFFFFFu;  /* dwFreeHwMemBytes   */
                   MR32(p+44)=0x7FFFFFFFu;
                   MR32(p+48)=64; MR32(p+52)=64; MR32(p+56)=64; }
            r=0; }
        else if (MID(SetCooperativeLevel)) r=0;
        else if (MID(GetSpeakerConfig)){ uint32_t p=A(c,1); if(p){mem_commit(p,4);MR32(p)=0x00000006u;} r=0; }
        else if (MID(DuplicateSoundBuffer)){
            /* (this, pOriginal, ppDuplicate) - the copy shares the same PCM
             * data.  It used to return an EMPTY COM object: no size, no
             * data, no voice in the mixer.  The game plays its concurrent
             * effects (engine, menu clicks) this way, so ALL of them were silent. */
            uint32_t src=A(c,1), pp=A(c,2);
            uint32_t b=com_new(IF_DSBUF,64);
            if (src && mem_is_committed(src+31)){
                MR32(b+4)=MR32(src+4);          /* size    */
                MR32(b+8)=MR32(src+8);          /* data    */
                ds_share(MR32(b+8), +1);
                MR32(b+20)=MR32(src+20);        /* bytes/s */
                MR32(b+12)=0; MR32(b+16)=0; MR32(b+24)=0;
                int audio_duplicate(int);
                MR32(b+28)=(uint32_t)audio_duplicate((int)MR32(src+28));
            }
            if(pp) MR32(pp)=b; r=0; }
        else r=0;
    }
    else if (iface==IF_DSBUF){
        uint32_t self=A(c,0);
        uint32_t sz = self?MR32(self+4):65536u, buf = self?MR32(self+8):0;
        if      (MID(Lock)){
            /* Which buffer object is being locked?  SFX events lock a
             * buffer but are not heard; they may be writing to an
             * unregistered buffer (the primary, or one not in the report). */
            /* Which THREAD is locking?  The stream (music) is on a separate
             * thread; SFX events come from the MAIN thread.  Telling the two
             * apart is the most direct way to show whether SFX data is
             * really being written. */
            { uint32_t tid = (c->fs_base && mem_is_committed(c->fs_base+0x24u))
                             ? MR32(c->fs_base+0x24u) : 0u;
              static uint32_t tl[8]; static unsigned tc[8]; static uint64_t tb[8];
              int ti=-1; for(int q=0;q<8;q++) if(tl[q]==tid+1u) ti=q;
              if(ti<0) for(int q=0;q<8;q++) if(!tl[q]){ tl[q]=tid+1u; ti=q; break; }
              if(ti>=0){ tc[ti]++; tb[ti]+=A(c,2); }
              g_lk_tid=tl; g_lk_cnt=tc; g_lk_byt=tb; }
            { static uint32_t obj[16]; static unsigned cnt[16]; static int nobj=0;
              static int reg_atexit=0;
              int fi=-1; for(int q=0;q<nobj;q++) if(obj[q]==self) fi=q;
              if(fi<0 && nobj<16){ fi=nobj++; obj[fi]=self; cnt[fi]=0; }
              if(fi>=0) cnt[fi]++;
              (void)reg_atexit;
              g_lock_obj = obj; g_lock_cnt = cnt; g_lock_n = nobj; }
            if (ENVC("DSLOG")){ static int nl3=0;
                if (nl3 < 2){ nl3++;
                    fprintf(stderr,"  [sound] --- Lock call chain (buffer %u) ---\n",
                            self?MR32(self+28):0);
                    guest_backtrace(c, 12); } }
            /* (this, dwOffset, dwBytes, ppv1, pcb1, ppv2, pcb2, dwFlags)
             *
             * A DirectSound buffer is CIRCULAR: if the requested range runs
             * past the end, TWO regions are returned (up to the end + the rest
             * from the start).  It used to hand out one flat region; the part
             * that belonged at the start of the buffer was never written, and
             * every wrap left a silent gap. */
            uint32_t off=A(c,1), want=A(c,2), pp1=A(c,3), pn1=A(c,4),
                     pp2=A(c,5), pn2=A(c,6), fl=A(c,7);
            if(!buf){ buf=gbuf_alloc(sz?sz:65536u); if(self) MR32(self+8)=buf; }
            if(!sz) sz = 65536u;
            if (fl & 0x2u){ off = 0; want = sz; }        /* ENTIREBUFFER */
            off %= sz;
            if (!want || want > sz) want = sz;
            uint32_t avail = sz - off;
            uint32_t b1 = (want <= avail) ? want : avail;
            uint32_t b2 = want - b1;
            if (ENVC("SNDLOG")){
                static int nl=0;
                uint32_t sid2 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                if (nl<40 && sid2){ nl++;
                    uint32_t rp=0, rw=0;
                    void audio_cursors(int, uint32_t*, uint32_t*);
                    audio_cursors((int)sid2, &rp, &rw);
                    fprintf(stderr,"  [snd] lock ofs=%6u size=%6u  |  play=%6u write=%6u"
                            "  buffer=%u  ahead=%d\n", off, want, rp, rw, sz,
                            (int)((off + sz - rp) % sz));
                }
            }
            if(pp1){ mem_commit(pp1,4); MR32(pp1)=buf+off; }
            if(pn1){ mem_commit(pn1,4); MR32(pn1)=b1; }
            if(pp2){ mem_commit(pp2,4); MR32(pp2)= b2 ? buf : 0u; }
            if(pn2){ mem_commit(pn2,4); MR32(pn2)= b2; }
            r=0; }
        else if (MID(Unlock)){
            /* the game's own work at an Unlock (UG2: the VOICELOG switch, debug_ug2.c) */
            if (g_game.on_sound_unlock) g_game.on_sound_unlock();
            /* Is the game writing real PCM into this buffer?  If so, the sound
             * engine (and so the front-end logic) IS running. */
            uint32_t p1=A(c,1), n1=A(c,2);
            if (p1 && n1 && n1 < 1u<<22 && mem_is_committed(p1+ (n1>16?15:0))){
                uint32_t nz=0, lim = n1 < 4096u ? n1 : 4096u;
                for (uint32_t q=0;q<lim;q++) if (MR8(p1+q)) nz++;
                if (nz){ g_snd_bytes += n1; g_snd_nonzero++; }
                else      g_snd_zero++;
                { uint32_t sid4 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                  if (sid4){ void audio_note_write_stat(int, uint32_t, int);
                             audio_note_write_stat((int)sid4, n1, nz ? 1 : 0); } }
                /* SNDZERO: report regions written as ENTIRELY ZERO, with the
                 * calling guest address.  Measured: every 30 s exactly the game
                 * WRITES ~50-95 ms of silence (not a starved buffer: empty data). */
                if (ENVC("SNDZERO")){
                    double audio_time(void);
                    double _t = audio_time();
                    /* The buffer clears at startup are not of interest; the
                     * real event happens AFTER the first 20 s, while the 30 s
                     * stream block is refreshed.  Look at the whole region (not
                     * the first 4 KB): the second half of the write may be empty. */
                    if (_t > 20.0 && n1 >= 1024u){
                        uint32_t nz2 = 0, lim2 = n1 < 65536u ? n1 : 65536u;
                        for (uint32_t q = 0; q < lim2; q++) if (MR8(p1+q)) nz2++;
                        if (nz2 * 4u < lim2){          /* more than 75% zero */
                            static int nzl = 0;
                            if (nzl < 120){ nzl++;
                                fprintf(stderr, "  [zero-write] t=%.3f s  ofs=%u size=%u "
                                        "full=%.1f%%  caller=guest:%08x\n",
                                        _t, p1 - (self?MR32(self+8):0), n1,
                                        100.0*nz2/lim2, c->eip);
                            }
                        }
                    }
                }
                /* Tell the mixer the write position (for the servo).
                 * NOTE: the lock may have WRAPPED -- the second region (pv2,cb2)
                 * continues from the START of the buffer.  Looking only at the
                 * first region made a wrap look like the write cursor was at
                 * the end of the buffer and fed the servo wrong values. */
                { uint32_t sid3 = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
                  uint32_t base3 = self ? MR32(self+8) : 0;
                  uint32_t n2u   = A(c,4);
                  if (sid3 && base3 && p1 >= base3){
                      void audio_note_write(int, uint32_t);
                      if (n2u && n2u < 1u<<22)
                          audio_note_write((int)sid3, n2u);          /* continues from the start */
                      else
                          audio_note_write((int)sid3, (p1 - base3) + n1);
                  } }
            }
            r=0; }
        else if (MID(Play)){
            if(self){ MR32(self+12)=1; uint64_t host_us(void);
                      MR32(self+24)=(uint32_t)(host_us()/1000ull);
                      /* (this, Reserved, Priority, Flags); DSBPLAY_LOOPING=1 */
                      if (MR32(self+28)) audio_play((int)MR32(self+28), (A(c,3) & 1u) ? 1 : 0); }
            g_snd_play++;
            if (ENVC("DSLOG")){
                fprintf(stderr, "  [sound] --- Play call chain (buffer %u) ---\n",
                        self?MR32(self+28):0);
                guest_backtrace(c, 10);
            }
            r=0; }
        else if (MID(Stop)){
            if(self){ MR32(self+12)=0; MR32(self+24)=0;
                      if (MR32(self+28)) audio_stop((int)MR32(self+28)); }
            r=0; }
        else if (MID(SetVolume)){
            if(self && MR32(self+28)) audio_set_volume((int)MR32(self+28), (long)(int32_t)A(c,1));
            r=0; }
        else if (MID(SetFrequency)){
            if(self && MR32(self+28)){ void audio_set_freq(int, uint32_t);
                                       audio_set_freq((int)MR32(self+28), A(c,1)); }
            r=0; }
        else if (MID(GetStatus)){
            uint32_t p=A(c,1); if(p){mem_commit(p,4); MR32(p)= (self&&MR32(self+12))?1u:0u;} r=0; }
        else if (MID(GetCurrentPosition)){
            int break_gcp = 0;
            if (g_game.on_sound_position) g_game.on_sound_position();
            /* The play cursor advances by ELAPSED TIME (not a fake fixed
             * step): the video player drives its frames by this clock. */
            uint32_t p1=A(c,1), p2=A(c,2);
            uint64_t host_us(void);
            uint32_t bps = (self && mem_is_committed(self+27)) ? MR32(self+20) : 176400u;
            if (!bps) bps = 176400u;
            uint32_t playing = self ? MR32(self+12) : 0;
            uint32_t t0 = self ? MR32(self+24) : 0;
            uint64_t now = host_us();
            if (playing && !t0){ t0 = (uint32_t)(now/1000ull); if(self) MR32(self+24)=t0; }
            uint32_t pos = 0;
            /* If registered with the mixer, use the REAL play cursor: the
             * game's streaming logic picks where to write from it; if an
             * estimated clock drifts from real playback, the sound breaks up. */
            { uint32_t sid = (self && mem_is_committed(self+31)) ? MR32(self+28) : 0;
              if (sid && playing){
                  uint32_t rp=0, rw=0;
                  void audio_cursors(int, uint32_t*, uint32_t*);
                  audio_cursors((int)sid, &rp, &rw);
                  if (p1){ mem_commit(p1,4); MR32(p1)=rp; }
                  if (p2){ mem_commit(p2,4); MR32(p2)=rw; }
                  if (self) MR32(self+16)=rp;
                  r=0; break_gcp = 1;
              } }
            if (!break_gcp && playing && t0){
                uint64_t ms = (now/1000ull) - (uint64_t)t0;
                uint64_t byt = ms * (uint64_t)bps / 1000ull;
                pos = sz ? (uint32_t)(byt % (uint64_t)sz) : 0;
            } else if (self) pos = MR32(self+16);
            if (!break_gcp){
                if(self) MR32(self+16)=pos;
                if(p1){mem_commit(p1,4); MR32(p1)=pos;}
                if(p2){mem_commit(p2,4); MR32(p2)=sz?((pos+2048u)%sz):0;}
            }
            r=0; }
        else if (MID(GetFormat)){
            uint32_t p=A(c,1), avail=A(c,2), pw=A(c,3);
            if(p && avail>=18u){ mem_commit(p,18); memset(g_mem+p,0,18);
                MR16(p)=1; MR16(p+2)=2; MR32(p+4)=44100; MR32(p+8)=44100*4;
                MR16(p+12)=4; MR16(p+14)=16; }
            if(pw){mem_commit(pw,4); MR32(pw)=18;}
            r=0; }
        else if (MID(GetCaps)){
            uint32_t p=A(c,1);
            if(p){ uint32_t s2=MR32(p); if(!s2||s2>256u) s2=16u; mem_commit(p,s2);
                   memset(g_mem+p,0,s2); MR32(p)=s2; MR32(p+8)=sz; }
            r=0; }
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,8); memset(g_mem+p,0,8); }
            r=0; }
        else r=0;
    }
    /* --- DirectInput8 --- */
    else if (iface==IF_DI8){
        if      (MID(CreateDevice)){
            /* (rguid, lplpDirectInputDevice, pUnkOuter) */
            uint32_t g=A(c,1), pp=A(c,2); uint32_t d=com_new(IF_DIDEV,128);
            uint32_t kind = 0;
            if (g && mem_is_committed(g+15)){
                uint32_t d1 = MR32(g);
                if (d1 == 0x6F1D2B61u) kind = 1;        /* GUID_SysKeyboard */
                else if (d1 == 0x6F1D2B60u) kind = 2;   /* GUID_SysMouse    */
                else if (dipad_guid(g)) kind = 3;       /* the game pad     */
                fprintf(stderr,"  [DI] CreateDevice GUID %08x-%04x-%04x -> %s\n",
                        d1, MR16(g+4), MR16(g+6),
                        kind==1?"KEYBOARD":(kind==2?"MOUSE":(kind==3?"GAMEPAD":"unknown")));
            }
            MR32(d+8) = kind;
            if (kind == 1) dikbd_created(d);
            if (kind == 3) dipad_created(d, g);
            if(pp) MR32(pp)=d; r=0; }
        else if (MID(EnumDevices)){
            /* (dwDevType, lpCallback, pvRef, dwFlags), as DirectInput 8 does it (and Wine's dinput, in its
             * order): the system mouse, then the system keyboard, each for its class or its type filter
             * (DI8DEVCLASS_ALL 0, DI8DEVCLASS_POINTER 2 or DI8DEVTYPE_MOUSE 0x12, DI8DEVCLASS_KEYBOARD 3 or
             * DI8DEVTYPE_KEYBOARD 0x13); neither has force feedback, so DIEDFL_FORCEFEEDBACK (0x100) leaves
             * both out.  Then the game controllers: the virtual pad (runtime/dinput_pad.c).  The callback's
             * DIENUM_STOP (0) ends the enumeration.  Underground 2 asks for DI8DEVCLASS_GAMECTRL only (0x5cfa4a,
             * 0x5d00b5); Most Wanted asks for DI8DEVCLASS_ALL (0x7f1b39) and makes its keyboard and mouse
             * from what it is offered: offering neither left it without a keyboard. */
            uint32_t devtype=A(c,1), cb=A(c,2), ref=A(c,3), flags=A(c,4), t = devtype & 0xFFu;
            int offered = 0;
            for (int is_kb = 0; is_kb < 2 && cb && !(flags & 0x100u); is_kb++){
                if (t != 0u && t != (is_kb ? 3u : 2u) && t != (is_kb ? 0x13u : 0x12u)) continue;
                uint32_t args[2] = { make_devinst(is_kb), ref };
                offered = 1;
                if (call_guest(c, cb, 2, args) == 0) cb = 0;       /* DIENUM_STOP */
            }
            g_di_enum += offered;
            if (cb) dipad_enum(c, devtype, cb, ref, flags);
            r=0; }
        else if (MID(EnumDevicesBySemantics)){
            /* (ptszUserName, lpdiActionFormat, lpCallback, pvRef, dwFlags)
             * callback: (lpddi, lpdid, dwFlags, dwRemaining, pvRef) */
            uint32_t cb=A(c,3), ref=A(c,4);
            if (cb && !ENVC("DI_NO_SEM")){
                for (int k=0;k<2;k++){
                    int is_kb = (k==0);
                    uint32_t dev = com_new(IF_DIDEV,128);
                    MR32(dev+8) = is_kb ? 1u : 2u;     /* device kind marker */
                    if (is_kb) dikbd_created(dev);
                    uint32_t args[5] = { make_devinst(is_kb), dev,
                                         0x00000001u,          /* DIEDBS_MAPPEDPRI1 */
                                         (uint32_t)(1-k), ref };
                    if (call_guest(c, cb, 5, args) == 0) break;
                }
                g_di_enum++;
            }
            r=0; }
        else if (MID(GetDeviceStatus)) r=0;
        else if (MID(Initialize))      r=0;
        else if (MID(FindDevice))      r=0x80070002u;
        else r=0;
    }
    else if (iface==IF_DIEFF){
        uint32_t av[8] = {0};
        for (int i = 0; i < nargs && i < 8; i++) av[i] = A(c,i);
        r = dipad_effect_call(c, A(c,0), nm, nargs, av);
    }
    else if (iface==IF_DIDEV){
        uint32_t self=A(c,0);
        uint32_t dkind = (self && mem_is_committed(self+11)) ? MR32(self+8) : 0;
        /* DITRACE=1: per-device call chain.  The game sets up the keyboard
         * device but never reads it; see where the chain breaks off. */
        int di_tr = 0;
        { static int n2 = 0;
          if (ENVC("DITRACE") && n2 < 140){ n2++; di_tr = 1;
              fprintf(stderr,"  [DI%u/%08x] %-24s", dkind, self, nm); } }
        if ((dkind == 1u || dkind == 3u) && !MID(AddRef) && !MID(Release) && !MID(QueryInterface)){
            uint32_t av[8] = {0};
            for (int i = 0; i < nargs && i < 8; i++) av[i] = A(c,i);
            r = dkind == 1u ? dikbd_call(c, self, nm, nargs, av)      /* keyboard */
                            : dipad_call(c, self, nm, nargs, av);     /* game pad */
            if (dkind == 1u && MID(EnumObjects)) g_di_objs++;
        }
        /* the mouse (kind 2) and anything else */
        else if (MID(EnumObjects)) r=0;
        else if (MID(GetDeviceInfo)){
            uint32_t p=A(c,1);
            if (p && mem_is_committed(p+3)){
                uint32_t sz=MR32(p); if(!sz||sz>700u) sz=580u;
                uint32_t src = make_devinst(dkind != 2u);
                mem_commit(p, sz);
                memcpy(g_mem+p, g_mem+src, sz < 580u ? sz : 580u);
                MR32(p)=sz;
            }
            r=0; }
        else
        if      (MID(SetDataFormat)){ if(self) MR32(self+4)=A(c,1); r=0; }
        else if (MID(SetCooperativeLevel)) r=0;
        else if (MID(Acquire)||MID(Unacquire)||MID(Poll)) r=0;
        else if (MID(GetDeviceState)){
            uint32_t cb=A(c,1), p=A(c,2);
            { static uint32_t seen_cb[8]; static int ns=0;
              int f2=0; for (int q=0;q<ns;q++) if (seen_cb[q]==cb) f2=1;
              if (!f2 && ns<8){ seen_cb[ns++]=cb;
                fprintf(stderr,"  [input] GetDeviceState size=%u (dataFormat=0x%08x device=%u)\n",
                        cb, self?MR32(self+4):0,
                        (self && mem_is_committed(self+11)) ? MR32(self+8) : 0); } }
            if(p && cb && cb<4096u){
                mem_commit(p,cb); memset(g_mem+p,0,cb);
                if (cb == 20u && metal_available()){        /* DIMOUSESTATE2 */
                    metal_fill_mouse(g_mem+p, cb);
                    g_dikey_calls++;
                    for (uint32_t q=12;q<20;q++)
                        if (MR8(p+q)){ g_dikey_hits++; break; }
                }
                else if (cb == 16u && metal_available()){   /* DIMOUSESTATE */
                    unsigned char tmpm[20];
                    metal_fill_mouse(tmpm, 20);
                    memcpy(g_mem+p, tmpm, 16);
                    g_dikey_calls++;
                }
            }
            r=0; }
        else if (MID(GetDeviceData)){                     /* the game polls the mouse state */
            uint32_t pcnt=A(c,3);
            if (pcnt && mem_is_committed(pcnt+3)) MR32(pcnt)=0;
            r=0; }
        else if (MID(GetCapabilities)){
            uint32_t p=A(c,1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>256u) sz=44u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz;
                   MR32(p+4)=0x00000001u;   /* DIDC_ATTACHED */
                   MR32(p+8)=0x00000212u;   /* DI8DEVTYPE_MOUSE */
                   MR32(p+12)=3; MR32(p+16)=8; }   /* dwAxes, dwButtons */
            r=0; }
        else if (MID(GetProperty)){
            uint32_t p=A(c,2);
            if(p){ uint32_t sz=MR32(p); if(sz && sz<=256u){ mem_commit(p,sz);
                   memset(g_mem+p+16,0,sz-16>0?sz-16:0); } }
            r=0; }
        else if (MID(SetProperty)) r=0;
        else if (MID(GetObjectInfo)){
            uint32_t p=A(c,nargs-1);
            if(p){ uint32_t sz=MR32(p); if(!sz||sz>512u) sz=32u; mem_commit(p,sz);
                   memset(g_mem+p,0,sz); MR32(p)=sz; }
            r=0; }
        else if (MID(SetEventNotification)) r=0;
        else if (!strncmp(nm,"Get",3) && nargs>=2){
            uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,32); memset(g_mem+p,0,32); }
            r=0; }
        else r=0;
        if (di_tr) fprintf(stderr, " -> 0x%08x\n", r);
    }
    else if (iface==IF_QUERY){
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; com_addref(g_dev); r=0; }   /* D3D9: a reference */
        else if (MID(GetDataSize)) r=4;
        else if (MID(GetData)){ uint32_t p=A(c,1); if(p){mem_commit(p,4);MR32(p)=0;} r=0; }
        else r=0;
    }
    else if (iface==IF_SWAPCHAIN){
        if      (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; com_addref(g_dev); r=0; }   /* D3D9: a reference */
        else if (MID(GetBackBuffer)){ uint32_t pp=A(c,3), sf=bb_surface();      /* swap chain */
            if (sf) com_addref(sf);
            if(pp) MR32(pp)=sf; r=0; }
        else if (MID(Present)){ g_present++; r=0; }
        else if (!strncmp(nm,"Get",3) && nargs>=2){ uint32_t p=A(c,nargs-1);
            if(p && p<0xE0000000u){ mem_commit(p,64); memset(g_mem+p,0,64);} r=0; }
        else r=0;
    }
    else if (iface==IF_VSHADER || iface==IF_VDECL || iface==IF_STATEBLOCK || iface==IF_SIMPLE){
        if (iface==IF_STATEBLOCK && MID(Apply)){
            uint32_t self5=A(c,0);
            int bi = (self5 && mem_is_committed(self5+11)) ? (int)MR32(self5+8) : 0;
            if (bi > 0 && bi < MAXSB && g_sb[bi].used){
                SBlock *B = &g_sb[bi];
                const uint32_t *save_ovr = g_arg_ovr;
                int save_rep = g_sb_replay, save_rec = g_sb_rec;
                g_sb_replay = 1; g_sb_rec = 0;
                for (int k = 0; k < B->n; k++){
                    g_arg_ovr = B->r[k].a;
                    com_call(c, B->r[k].magic);
                }
                g_arg_ovr = save_ovr; g_sb_replay = save_rep; g_sb_rec = save_rec;
                g_sb_applied++;
            }
            r=0; }
        else if (iface==IF_STATEBLOCK && MID(Capture)){
            /* Rewrite every state in the block with its CURRENT value.  This
             * is how the game builds its save/restore idiom: record once,
             * Capture before changing, then Apply. */
            uint32_t self6=A(c,0);
            int bi = (self6 && mem_is_committed(self6+11)) ? (int)MR32(self6+8) : 0;
            if (bi > 0 && bi < MAXSB && g_sb[bi].used){
                SBlock *B = &g_sb[bi];
                for (int k = 0; k < B->n; k++){
                    SBRec *R = &B->r[k];
                    uint32_t mm = R->magic & 0xFFFFu;
                    uint32_t ifc = (R->magic>>16) & 0x1FFFu;
                    if (ifc >= NEL(g_if) || mm >= g_if[ifc].n) continue;
                    /* a cached numeric id instead of the method name: D3DX Begin
                     * captures 5 blocks per effect, the name compare was a hot spot */
                    int md;
                    if (ifc < MIDIF && mm < MIDM){
                        md = g_midc[ifc][mm];
                        if (!md) md = g_midc[ifc][mm] = (int16_t)mid_resolve(g_if[ifc].m[mm].name);
                    } else md = mid_resolve(g_if[ifc].m[mm].name);
                    if      (md == MID_SetRenderState){
                        if (R->a[1] < 256u) R->a[2] = g_rs[R->a[1]]; }
                    else if (md == MID_SetTextureStageState){
                        if (R->a[1] < 8u && R->a[2] < 40u) R->a[3] = g_tss_raw[R->a[1]][R->a[2]]; }
                    else if (md == MID_SetSamplerState){
                        if (R->a[1] < 16u && R->a[2] < 20u) R->a[3] = g_smp_raw[R->a[1]][R->a[2]]; }
                    else if (md == MID_SetTexture){
                        if (R->a[1] < 8u && R->a[2] != g_raw_tex[R->a[1]]){
                            uint32_t old = R->a[2];
                            R->a[2] = g_raw_tex[R->a[1]];
                            if (R->a[2]) com_addref(R->a[2]);
                            if (old) com_release(old); } }
                    else if (md == MID_SetVertexShader)      R->a[1] = g_raw_vs;
                    else if (md == MID_SetPixelShader)       R->a[1] = g_raw_ps;
                    else if (md == MID_SetVertexDeclaration) R->a[1] = g_raw_decl;
                    else if (md == MID_SetFVF)               R->a[1] = g_cur_fvf;
                }
                g_sb_captured++;
            }
            r=0; }
        else if (MID(GetDevice)){ uint32_t p=A(c,1); if(p) MR32(p)=g_dev; com_addref(g_dev); r=0; }   /* D3D9: a reference */
        else if (MID(GetFunction)||MID(GetDeclaration)){
            uint32_t self2=A(c,0), pdata=A(c,1), psize=A(c,2);
            uint32_t src = self2?MR32(self2+4):0, sz = self2?MR32(self2+8):0;
            if (pdata && src && sz){ mem_commit(pdata,sz); memcpy(g_mem+pdata, g_mem+src, sz); }
            if (psize){ mem_commit(psize,4); MR32(psize)=sz; }
            r=0; }
        else r=0;
    }
    else r=0;

    if (g_com_trace && !MID(Present) && strncmp(nm,"Draw",4)
        && !MID(SetRenderState) && !MID(SetTextureStageState))
        fprintf(stderr,"  [COM] %s::%-28s (%d arg) -> 0x%08x%s\n",
                g_if[iface].name, nm, nargs, r, impl?"":" [MISSING]");
    if(!impl) g_com_unimpl++;
    c->eax=r;
    if (!g_sb_replay) c->esp += 4u*(uint32_t)nargs;
    return 1;
}

static uint32_t g_ds = 0;
/* DirectSoundCreate(lpGuid, ppDS, pUnkOuter) */
uint32_t com_directsoundcreate(uint32_t ppDS){
    if (!g_ds){ g_ds = com_new(IF_DS, 64); CH_FLAGS(g_ds) |= CF_DEVICE;
        fprintf(stderr,"com: IDirectSound object 0x%08x\n", g_ds); }
    else com_addref(g_ds);                  /* every create hands out a reference */
    if (ppDS) MR32(ppDS) = g_ds;
    return 0;
}

static uint32_t g_di8 = 0;
/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) */
uint32_t com_directinput8create(uint32_t ppvOut){
    if (!g_di8){ g_di8 = com_new(IF_DI8, 64); CH_FLAGS(g_di8) |= CF_DEVICE;
        fprintf(stderr,"com: IDirectInput8 object 0x%08x\n", g_di8); }
    else com_addref(g_di8);
    if (ppvOut) MR32(ppvOut) = g_di8;
    return 0;                       /* DI_OK */
}

uint32_t com_direct3dcreate9(void){
    if(!g_d3d9){ g_d3d9=com_new(IF_D3D9,64); CH_FLAGS(g_d3d9) |= CF_DEVICE;
        fprintf(stderr,"com: IDirect3D9 object 0x%08x\n", g_d3d9); }
    else com_addref(g_d3d9);
    return g_d3d9;
}
/* NFSU2_COMLIVE=1 (test-only): live objects per interface on every [status]. */
int com_live_line(char *buf, size_t n){
    static const char *const ab[] = { "?", "d3d", "dev", "tex", "vb", "ib", "surf", "simple", "vs", "decl",
                                      "sb", "swap", "query", "di", "didev", "ds", "dsbuf" };
    int k = 0;
    for (unsigned i = 1; i < NEL(g_if) && k < (int)n - 24; i++){
        uint64_t live = g_com_new_n[i] - g_com_del_n[i];
        if (live) k += snprintf(buf + k, n - (size_t)k, " %s=%llu", i < NEL(ab) ? ab[i] : "?", (unsigned long long)live);
    }
    return k;
}
/* [status] com=/gbuf=: COM object and Lock-buffer arenas (heap.c). */
void com_mem_stats(uint64_t *com_bytes, uint64_t *buf_bytes, uint64_t *objs){
    if (gheap_bump()){ *com_bytes = com_ptr - COM_BASE; *buf_bytes = gbuf_ptr - GBUF_BASE; }
    else { *com_bytes = g_com_bytes; *buf_bytes = g_gbuf_bytes; }
    *objs = g_com_live;
}
void com_init(void){
#ifndef __wasm__
    mem_commit(COM_BASE,0x10000);
#endif
   
    { extern char **environ; (void)environ; }
}
static uint64_t *g_ptype = 0, *g_stride = 0;
void draw_stats_save(uint64_t *a, uint64_t *b){ g_ptype=a; g_stride=b; }

void com_report(void){
    fprintf(stderr,"\n=== COM / D3D9 LAYER ===\n");
    ws_report();
#ifdef __wasm__
    { void headlights_report(void); headlights_report(); }
#endif
    if (g_aspfix != 1.f)
        { fprintf(stderr,"  widescreen       : aspfix %.4f, %lu perspective / %lu orthographic draws\n",
                  g_aspfix, g_asp_persp, g_asp_ortho);
          fprintf(stderr,"  renderer Hor+ into render targets: %lu cube face draws, %lu other\n",
                  g_asp_cube, g_asp_rt);
          for (unsigned v2 = 0; v2 < MAXVS; v2++)
              if (g_asp_np[v2] || g_asp_no[v2])
                  fprintf(stderr,"     vs=%-3u cx=%-3d cw=%-3d  persp %8lu / ortho %8lu  c[cw].w=%.3f\n",
                          v2, g_pos_cx[v2], g_pos_cw[v2], g_asp_np[v2], g_asp_no[v2], g_asp_cw3[v2]); }
    if (ENVC("RSLOG")){
        /* the states rs_apply() actually uses */
        static const unsigned handled[] = {7,14,15,19,20,22,23,24,25,27,168,171,193};
        fprintf(stderr,"  --- SetRenderState use (UNHANDLED ones starred) ---\n");
        for (unsigned i=0;i<256;i++){
            if (!g_rs_hits[i]) continue;
            int h=0; for (unsigned k=0;k<sizeof handled/sizeof*handled;k++) if(handled[k]==i) h=1;
            fprintf(stderr,"   %c st=%-3u %8u times  values:", h?' ':'*', i, g_rs_hits[i]);
            for (int k=0;k<g_rs_nval[i];k++){
                float f; memcpy(&f,&g_rs_vals[i][k],4);
                fprintf(stderr," 0x%x(%g)", g_rs_vals[i][k], f);
            }
            fprintf(stderr,"%s\n", g_rs_nval[i]>=4?" ...":"");
        }
    }
    fprintf(stderr,"  SetRenderState    : %llu applied, %llu SKIPPED (%.1f%% were redundant)\n",
            (unsigned long long)g_rs_run, (unsigned long long)g_rs_skip,
            100.0*(double)g_rs_skip/(double)((g_rs_skip+g_rs_run) ? (g_rs_skip+g_rs_run) : 1));
    fprintf(stderr,"  COM calls in total : %llu\n",(unsigned long long)g_com_calls);
    fprintf(stderr,"  Present (frames)   : %llu\n",(unsigned long long)g_present);
    if (metal_available())
        fprintf(stderr,"  Metal presents     : %ld frames, %ld triangles\n"
                       "  Metal textures     : %ld created, %ld uploads\n",
                metal_frames(), metal_tris(),
                metal_tex_created(), metal_tex_uploaded());
    fprintf(stderr,"  Draw* calls        : %llu\n",(unsigned long long)g_draw);
    fprintf(stderr,"  ps constant file   : highest used c%u, %u writes at 32 and above\n",
            g_psc_max ? g_psc_max-1u : 0u, g_psc_drop);
    fprintf(stderr,"  COM objects by interface (created / destroyed / AddRef calls / Release calls):%s\n",
            com_frees() ? "" : "  [NFSU2_COMFREE=0: never destroyed]");
    for (unsigned i = 1; i < NEL(g_if) && i < MSTAT_IF; i++)
        if (g_com_new_n[i] || g_mstat[i][1] || g_mstat[i][2])
            fprintf(stderr,"    %-28s %9llu / %9llu / %10llu / %10llu\n", g_if[i].name, (unsigned long long)g_com_new_n[i],
                    (unsigned long long)g_com_del_n[i], (unsigned long long)g_mstat[i][1], (unsigned long long)g_mstat[i][2]);
    { unsigned long metal_tex_released(void);
      fprintf(stderr,"  COM lifetime      : %llu live objects, %llu over-released (kept), %llu calls on non-objects,"
                     " %llu uncached child surfaces; Metal textures released %lu\n",
              (unsigned long long)g_com_live, (unsigned long long)g_com_overrel, (unsigned long long)g_com_bad,
              (unsigned long long)g_child_uncached, metal_available() ? metal_tex_released() : 0ul); }
    { uint64_t cb, bb, ob; com_mem_stats(&cb, &bb, &ob);
      fprintf(stderr,"  guest COM heap     : %llu KB\n", (unsigned long long)(cb / 1024u));
      fprintf(stderr,"  guest buffer heap  : %llu KB\n", (unsigned long long)(bb / 1024u)); }
    if (g_ptype){
        static const char *PT[] = {"?","POINTLIST","LINELIST","LINESTRIP",
                                   "TRIANGLELIST","TRIANGLESTRIP","TRIANGLEFAN","?"};
        fprintf(stderr,"  --- DrawPrimitiveUP: primitive types ---\n");
        for (int i=0;i<8;i++) if (g_ptype[i])
            fprintf(stderr,"    %-16s %llu\n", PT[i], (unsigned long long)g_ptype[i]);
        fprintf(stderr,"  --- vertex strides ---\n");
        for (int i=0;i<64;i++) if (g_stride[i])
            fprintf(stderr,"    %3d bytes %llu\n", i*4, (unsigned long long)g_stride[i]);
    }
    fprintf(stderr,"  --- DirectInput / DirectSound calls ---\n");
    { int any=0;
      for (unsigned i=1;i<NEL(g_if) && i<MSTAT_IF;i++){
        if (i!=IF_DI8 && i!=IF_DIDEV && i!=IF_DIEFF && i!=IF_DS && i!=IF_DSBUF && i!=IF_QUERY) continue;
        for (unsigned j=0;j<g_if[i].n && j<MSTAT_M;j++)
            if (g_mstat[i][j]){ any=1;
                fprintf(stderr,"    %-26s %-26s %llu\n",
                        g_if[i].name, g_if[i].m[j].name,
                        (unsigned long long)g_mstat[i][j]); }
      }
      if(!any) fprintf(stderr,"    (no calls)\n");
    }
    fprintf(stderr,"  vertex shaders    : %llu draws with a shader, %llu without, %u compiled\n",
            (unsigned long long)g_vs_draws, (unsigned long long)g_ff_draws, g_vsprogn-1);
    fprintf(stderr,"  state blocks      : %d slots, %llu Apply, %llu Capture, "
            "%llu records overflowed, %llu found no slot\n",
            g_sbn-1, (unsigned long long)g_sb_applied,
            (unsigned long long)g_sb_captured, (unsigned long long)g_sb_dropped,
            (unsigned long long)g_sb_full);
    fprintf(stderr,"  GPU vertex shaders: %llu draws on the GPU, %llu translations rejected,"
            " %d/%d (shader,decl) pairs registered\n",
            (unsigned long long)g_gpuvs_draws, (unsigned long long)g_gpuvs_fail,
            g_gvs_next-1, g_gvsn);
    fprintf(stderr,"  indexed draws      : %llu from the pool, old path: strip/fan %llu, object %llu, new lock %llu, "
            "align/bounds %llu, mirror %llu, index out of range %llu\n",
            (unsigned long long)g_dip_ix, (unsigned long long)g_dip_ixfb[0], (unsigned long long)g_dip_ixfb[1],
            (unsigned long long)g_dip_ixfb[2], (unsigned long long)g_dip_ixfb[3], (unsigned long long)g_dip_ixfb[4],
            (unsigned long long)g_dip_ixfb[5]);
    { unsigned ovrv=0, ovkb=0; void metal_raw_overflow(unsigned*,unsigned*);
      metal_raw_overflow(&ovrv,&ovkb);
      if (ovrv || ovkb || g_gpuvs_drop)
          fprintf(stderr,"  GPU vertex arena overflow : %u verts, %u constants, %llu draws went back to the CPU\n",
                  ovrv, ovkb, (unsigned long long)g_gpuvs_drop); }
    if (metal_available())
        fprintf(stderr,"  dropped frames    : %ld\n", metal_frames_dropped());
    { extern uint64_t g_icall_hits;
      if (ENVC("ICALL"))
          fprintf(stderr,"  ICALL hits        : %llu\n", (unsigned long long)g_icall_hits); }
    { const char *cp = ENVC("COVREPORT");
      if (cp){ void cov_report(const char*); cov_report(cp); } }
    { extern uint64_t g_timer_ticks;
      fprintf(stderr,"  timer ticks       : %llu\n", (unsigned long long)g_timer_ticks); }
    fprintf(stderr,"  draw matrices     : %llu valid, %llu bad\n",
            (unsigned long long)g_mat_ok,(unsigned long long)g_mat_bad);
    fprintf(stderr,"  FIXMAT pattern    : %llu times (bad front-end WVP matrix)\n", (unsigned long long)g_fixmat_hits);
    dikbd_report();
    fprintf(stderr,"  DI device enum    : %llu enumerations, %llu object enumerations\n",
            (unsigned long long)g_di_enum, (unsigned long long)g_di_objs);
    for (int q2 = 0; g_lk_tid && q2 < 8; q2++){
        if (!g_lk_tid[q2]) continue;
        fprintf(stderr,"  [lock] tid=%u  locks=%u  total=%.1f KB\n",
                g_lk_tid[q2]-1u, g_lk_cnt[q2], g_lk_byt[q2]/1024.0);
    }
    for (int q2 = 0; q2 < g_lock_n; q2++){
        uint32_t o2 = g_lock_obj[q2];
        fprintf(stderr,"  [lock] object 0x%08x  sound-id=%u  size=%u  locks=%u\n",
                o2, (o2 && mem_is_committed(o2+31)) ? MR32(o2+28) : 0u,
                (o2 && mem_is_committed(o2+7)) ? MR32(o2+4) : 0u, g_lock_cnt[q2]);
    }
    fprintf(stderr,"  sound buffers     : %llu non-empty writes, %llu empty writes, %.1f KB, %llu Play\n",
            (unsigned long long)g_snd_nonzero,(unsigned long long)g_snd_zero,
            g_snd_bytes/1024.0,(unsigned long long)g_snd_play);
    audio_report();
    fprintf(stderr,"  keyboard reads    : %llu calls, %llu with a key down\n",
            (unsigned long long)g_dikey_calls,(unsigned long long)g_dikey_hits);
    fprintf(stderr,"  RT textures created: %llu\n",(unsigned long long)g_rt_tex);
    { void metal_miplevel_report(void); if (metal_available()) metal_miplevel_report(); }
    texrep_report();
    fprintf(stderr,"  render target     : to a texture %llu, to the screen %llu\n",
            (unsigned long long)g_rt_set,(unsigned long long)g_rt_screen);
    fprintf(stderr,"  SetTexture        : valid %llu, NULL %llu, id=0 %llu, stage!=0 %llu\n",
            (unsigned long long)g_st_ok,(unsigned long long)g_st_null,
            (unsigned long long)g_st_zero,(unsigned long long)g_st_other);
    fprintf(stderr,"  texture creation  : %llu failed\n", (unsigned long long)g_tex_fail);
    fprintf(stderr,"  of which vs_run   : %.2f s\n", g_t_vs_ns/1e9);
    fprintf(stderr,"  --- VERTICES PER SHADER ---\n");
    for (int q=0;q<8;q++){
        uint64_t best=0; int bi=-1;
        for (int w=0;w<64;w++) if (g_v_per_vs[w]>best){ best=g_v_per_vs[w]; bi=w; }
        if (bi<0) break;
        fprintf(stderr,"     vs%-3d %12llu verts (%.1f%%)  %u instructions\n", bi,
                (unsigned long long)best,
                g_v_xform ? 100.0*(double)best/(double)g_v_xform : 0.0,
                (bi>0 && (unsigned)bi<g_vsprogn && g_vsprog[bi]) ? vs_ninstr(g_vsprog[bi]) : 0u);
        g_v_per_vs[bi]=0;
    }
    fprintf(stderr,"  draw path time    : %.2f s (with DRAWTIME=1), %llu verts transformed (%.1f M verts/s)\n",
            g_t_draw_ns/1e9, (unsigned long long)g_v_xform,
            g_t_draw_ns ? (double)g_v_xform/(g_t_draw_ns/1000.0) : 0.0);
    fprintf(stderr,"  bound tex stages  : 1: %llu, 2: %llu, 3: %llu, 4: %llu draws\n",
            (unsigned long long)g_stage_hist[0],(unsigned long long)g_stage_hist[1],
            (unsigned long long)g_stage_hist[2],(unsigned long long)g_stage_hist[3]);
    fprintf(stderr,"  indexed draws    : %llu draws, %llu triangles, %llu skipped\n",
            (unsigned long long)g_dip_draws,(unsigned long long)g_dip_tris,
            (unsigned long long)g_dip_skip);
    { uint64_t np = 0; for (int q = 1; q < 7; q++) np += g_prim_runs[q];
      if (np || g_prim_bad)
          fprintf(stderr,"  listed runs      : points %llu, lines %llu, line strips %llu, triangles %llu, strips %llu, fans %llu"
                  " (DrawPrimitive, DrawIndexedPrimitiveUP, indexed points and lines); %llu stopped outside memory\n",
                  (unsigned long long)g_prim_runs[1], (unsigned long long)g_prim_runs[2], (unsigned long long)g_prim_runs[3],
                  (unsigned long long)g_prim_runs[4], (unsigned long long)g_prim_runs[5], (unsigned long long)g_prim_runs[6],
                  (unsigned long long)g_prim_bad); }
    if (g_reset_fails) fprintf(stderr,"  failed Resets    : %lu (the device stayed lost until one succeeded)\n", g_reset_fails);
    fprintf(stderr,"  raw position range: x[%.4g..%.4g] y[%.4g..%.4g] z[%.4g..%.4g]\n",
            g_rmin[0],g_rmax[0],g_rmin[1],g_rmax[1],g_rmin[2],g_rmax[2]);
    fprintf(stderr,"  vertex positions  : on screen %llu, off %llu, w<=0 %llu\n",
            (unsigned long long)g_v_in, (unsigned long long)g_v_out,
            (unsigned long long)g_v_badw);
    for (unsigned q=1; q<g_vsprogn && q<6; q++)
        fprintf(stderr,"     vs%u: version %u.%u, %u instructions, %u unknown\n", q,
                vs_version(g_vsprog[q])>>8, vs_version(g_vsprog[q])&0xFF,
                vs_ninstr(g_vsprog[q]), vs_unknown(g_vsprog[q]));
    fprintf(stderr,"  --- MOST CALLED D3D9/COM METHODS ---\n");
    for (int k=0;k<120;k++){
        uint64_t best=0; int bi=0,bm=0;
        for (unsigned i=1;i<NEL(g_if) && i<MSTAT_IF;i++)
            for (unsigned j=0;j<g_if[i].n && j<MSTAT_M;j++)
                if (g_mstat[i][j]>best){ best=g_mstat[i][j]; bi=(int)i; bm=(int)j; }
        if (!best) break;
        fprintf(stderr,"    %-26s %-28s %llu\n",
                g_if[bi].name, g_if[bi].m[bm].name, (unsigned long long)best);
        g_mstat[bi][bm]=0;
    }
}

static void fill_caps(uint32_t p){
    mem_commit(p,0x150); memset(g_mem+p,0,0x150);
    MR32(p+0x00)=1; MR32(p+0x08)=0x00020000; MR32(p+0x0C)=0x00000002;
    MR32(p+0x10)=0x00000020; MR32(p+0x14)=0x80000001; MR32(p+0x18)=1;
    MR32(p+0x1C)=0x001BBEF0; MR32(p+0x20)=0x0000033F; MR32(p+0x24)=0x0F730191;
    MR32(p+0x28)=0x000000FF; MR32(p+0x2C)=0x00003FFF; MR32(p+0x30)=0x00003FFF;
    MR32(p+0x34)=0x000000FF; MR32(p+0x38)=0x0008FFFF; MR32(p+0x3C)=0x0021EDA5;
    MR32(p+0x40)=0x07030700; MR32(p+0x44)=0x07030700; MR32(p+0x48)=0x07030700;
    MR32(p+0x4C)=0x0000003F; MR32(p+0x50)=0x0000003F; MR32(p+0x54)=0x0000001F;
    MR32(p+0x58)=4096; MR32(p+0x5C)=4096; MR32(p+0x60)=256; MR32(p+0x64)=8192;
    MR32(p+0x68)=4096; MR32(p+0x6C)=16;
    MRF32(p+0x70)=1.0e10f; MRF32(p+0x74)=-32768.0f; MRF32(p+0x78)=-32768.0f;
    MRF32(p+0x7C)=32768.0f; MRF32(p+0x80)=32768.0f; MRF32(p+0x84)=0.0f;
    MR32(p+0x88)=0x000001FF; MR32(p+0x8C)=0x00100008; MR32(p+0x90)=0x03FFFFFF;
    MR32(p+0x94)=8; MR32(p+0x98)=8; MR32(p+0x9C)=0x0000013B;
    MR32(p+0xA0)=8; MR32(p+0xA4)=6; MR32(p+0xA8)=4; MR32(p+0xAC)=8;
    MRF32(p+0xB0)=256.0f; MR32(p+0xB4)=1048575; MR32(p+0xB8)=1048575;
    MR32(p+0xBC)=16; MR32(p+0xC0)=508;
    { /* Shader versions: 2.0 (UG2), 3.0 for a game with GameOps d3d_sm3 (the
       * translator's vs_3_0, ps_2_x and ps_3_0, docs/web/psref.md); D3D_SM in
       * the environment overrides either (0 fixed function, 11, 14, 20, 30) */
      const char *sm = ENVC("D3D_SM");
      uint32_t vs=0xFFFE0200u, ps=0xFFFF0200u;
      if (g_game.d3d_sm3){ vs=0xFFFE0300u; ps=0xFFFF0300u; }
      if (sm){ int v=atoi(sm);
        if (v==0){ vs=0; ps=0; }
        else if (v==11){ vs=0xFFFE0101u; ps=0xFFFF0101u; }
        else if (v==14){ vs=0xFFFE0101u; ps=0xFFFF0104u; }
        else if (v==20){ vs=0xFFFE0200u; ps=0xFFFF0200u; }
        else if (v==30){ vs=0xFFFE0300u; ps=0xFFFF0300u; }
      }
      MR32(p+0xC4)=vs; MR32(p+0xC8)=256; MR32(p+0xCC)=ps;
      if (g_game.d3d_sm3 && ps >= 0xFFFF0300u){
          /* what goes with 3.0 (D3DCAPS9 after DevCaps2): one adapter in its
           * group, one render target (every MW pixel shader writes oC0 only),
           * StretchRect's point and linear filters (D3DPTFILTERCAPS_MINFPOINT,
           * MINFLINEAR, MAGFPOINT, MAGFLINEAR); VS20Caps and PS20Caps at
           * their 3.0 values (predication, 24 dynamic and 4 static flow
           * control levels, 32 temporaries: ten of MW's ps_2_x use r16..r30,
           * GAME.md 5.3; the arbitrary-swizzle, gradient, predication and
           * no-limit bits, 512 slots), no vertex texture filters (the
           * translator has no vertex texture fetch), 65535 instructions
           * executed, 512 slots of 3.0 code */
          MR32(p+0xE8)=1; MR32(p+0xF0)=1; MR32(p+0xF4)=0x03000300u;
          MR32(p+0xF8)=1; MR32(p+0xFC)=24; MR32(p+0x100)=32; MR32(p+0x104)=4;
          MR32(p+0x108)=0x1F; MR32(p+0x10C)=24; MR32(p+0x110)=32; MR32(p+0x114)=4; MR32(p+0x118)=512;
          MR32(p+0x120)=65535; MR32(p+0x124)=65535; MR32(p+0x128)=512; MR32(p+0x12C)=512;
      } }
    MRF32(p+0xD0)=8.0f; MR32(p+0xD4)=0x00000051;
}
