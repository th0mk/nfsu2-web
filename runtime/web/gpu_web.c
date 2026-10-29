/* wasm: the renderer's game side.  Implements the metal_* API that com.c
 * records through (on the Mac: runtime/metal.m), as strong symbols over the
 * weak stand-ins in null_platform.c.
 *
 * The game thread cannot touch WebGPU (its objects belong to the page), so
 * everything goes into shared memory and the page's encoder
 * (web/src/gpu) turns it into WebGPU work:
 *   - frame records: GwDraw per draw/clear/StretchRect plus the TVert, raw
 *     vertex, VS constant and PS constant arenas, double-buffered as on the
 *     Mac (one buffer being written, one handed off);
 *   - an ordered resource queue: texture create/upload/free, the persistent
 *     vertex/index mirror pool, shader text.  Frames carry the queue position
 *     at handoff, so a frame never sees uploads recorded after it (the Mac's
 *     replaceRegion was immediate);
 *   - the present handshake (GwCtl.state, Atomics notify) and the display
 *     clock the page fills from requestAnimationFrame (GwVsync), which feeds
 *     framerate.c's vsync lock and NFSU2_FPS=auto.
 * Contract: gpu_proto.h / web/src/gpu/proto.ts.
 *
 * NFSU2_RENDERER=webgpu (set by a page that attaches a renderer) turns the
 * recording on.  Without it this file behaves like the null renderer: ids
 * and sizes for com.c, every draw dropped. */
#include "cpu.h"
#include "gfx.h"
#include "host_abi.h"
#include "gpu_proto.h"
#include "wgsl.h"
#include "texrep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>

#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

void guest_wait_begin(void);     /* win32_ext.c: release the guest lock around a host wait */
void guest_wait_end(void);
int  hfr_mailbox(void);          /* framerate.c */
void hfr_note_vsync(double ts, double dur);

/* ---- shared-memory waits (wasm atomics on the shared linear memory) ---- */
static inline int32_t ald(int32_t *p){ return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void ast(int32_t *p, int32_t v){ __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline void wake(int32_t *p){ __builtin_wasm_memory_atomic_notify((int *)p, 0xFFFFFFFFu); }
/* wait while *p == v, at most ms milliseconds; 0 woken, 1 not equal, 2 timed out */
static inline int wait32(int32_t *p, int32_t v, double ms){
    return __builtin_wasm_memory_atomic_wait32((int *)p, v, ms < 0 ? -1 : (int64_t)(ms * 1e6));
}
static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

/* ---- state ------------------------------------------------------------- */
static GwCtl   *g_ctl;
static GwVsync *g_vs;
static int      g_on;            /* recording for a page renderer */
static int      g_inited;
static int      g_detached;      /* the page never attached: dropped back to null behaviour */

#define PSC_N  32
#define MAXV   (256u * 1024u)               /* TVerts per buffer (CPU vertex path) */
#define MAXD   (16u * 1024u)                /* records per buffer */
#define MAXRV  (16u * 1024u * 1024u)        /* raw vertex arena (bytes) */
#define MAXKB  ( 4u * 1024u * 1024u)        /* VS constant arena (bytes) */
#define RQ_CAP (64u * 1024u * 1024u)        /* resource queue ring (bytes, power of two) */
#define KZERO  256u                         /* K arena bytes [0, 256): an all-zero block for TVert draws (base 0) */
#define KFIRST 2048u                        /* [GW_K_HL, + GW_HL_BYTES): the frame's headlights; [GW_K_MARKS, + GW_MARKS_BYTES):
                                               quick chat's places; [GW_K_TONE, + GW_TONE_BYTES): Exposure and
                                               White Level; the draws' blocks from here */
_Static_assert(GW_K_HL >= KZERO && GW_K_HL + GW_HL_BYTES <= KFIRST && GW_K_MARKS + GW_MARKS_BYTES <= KFIRST
               && GW_K_TONE + GW_TONE_BYTES <= KFIRST, "K arena");

typedef struct {
    GwDraw  *d;  unsigned dn;
    TVert   *v;  unsigned vn;
    uint8_t *rv; unsigned rvn;
    uint8_t *kb; unsigned kn;
    float  (*psc)[PSC_N][4]; unsigned pscn, psc_last, psc_lastver;
    unsigned last_koff, last_ksz; int last_vs;
} Buf;
static Buf g_b[GW_NBUF];
static int g_cur = 0;

static unsigned g_cur_target = 0, g_cur_blend = 0, g_cur_bcol = 0xFFFFFFFFu, g_cur_cull = 3u;
static unsigned g_cur_depth = 1u << 8, g_cur_dmode = 0, g_cur_zid = 1;
static unsigned g_cur_tex1, g_cur_tex2, g_cur_tex3;
static int      g_cur_ps;
static float    g_cur_psc[PSC_N][4];
static unsigned g_psc_ver = 1;
static float    g_cur_proj[4];
static unsigned g_cur_smp[4];
static float    g_cur_aref;
static float    g_cur_fog[4];
static unsigned g_clear_argb = 0;            /* last colour Clear set (frame header) */
static unsigned g_frame_clear = 0;           /* colour in effect when the current frame began */

static uint32_t g_gframe = 0;                /* guest frames handed off */
static long     g_presents = 0, g_tris = 0, g_dropped = 0, g_replaced = 0, g_tex_created = 0, g_tex_uploaded = 0;
static unsigned long g_tex_released = 0;
static unsigned g_ov_rv = 0, g_ov_kb = 0, g_ov_d = 0, g_ov_v = 0;
static double   g_wait_present_ms = 0, g_wait_rq_ms = 0;
static unsigned long long g_rq_bytes = 0;

static unsigned g_bb_w = 0, g_bb_h = 0;      /* device size (metal_resize) */

static long     g_dump_req = -1;
static char     g_dump_label[64];

/* ---- init / attach ----------------------------------------------------- */
static void shader_put(int op, int id, const char *src);
static void *zalloc(size_t n){ void *p = aligned_alloc(64, (n + 63) & ~(size_t)63); if (p) memset(p, 0, n); return p; }

static void gw_init(void){
    if (g_inited) return;
    g_inited = 1;
    g_ctl = zalloc(sizeof *g_ctl);
    g_vs  = zalloc(sizeof *g_vs);
    if (!g_ctl || !g_vs){ fprintf(stderr, "[gpu] out of memory for the control block\n"); return; }
    g_ctl->magic = GW_MAGIC; g_ctl->version = GW_VERSION; g_ctl->layout = GW_LAYOUT; g_ctl->size = sizeof *g_ctl;
    const char *r = getenv("NFSU2_RENDERER");
    int want = r && !strcmp(r, "webgpu");
    if (want){
        int ok = 1;
        for (int b = 0; b < GW_NBUF; b++){
            Buf *B = &g_b[b];
            B->d = zalloc(sizeof(GwDraw) * MAXD); B->v = zalloc(sizeof(TVert) * MAXV);
            B->rv = zalloc(MAXRV); B->kb = zalloc(MAXKB + 4096); B->psc = zalloc(sizeof *B->psc * MAXD);
            if (!B->d || !B->v || !B->rv || !B->kb || !B->psc) ok = 0;
        }
        for (int b = 0; b < GW_NBUF; b++){
            g_b[b].kn = KFIRST;
            if (g_b[b].kb) ((GwMarks *)(g_b[b].kb + GW_K_MARKS))->magic = GW_MARKS_MAGIC;
        }
        uint8_t *rq = zalloc(RQ_CAP);
        if (!rq) ok = 0;
        if (!ok){ fprintf(stderr, "[gpu] out of memory for the record buffers: no rendering\n"); want = 0; }
        else { g_ctl->rq_ptr = (uint32_t)(uintptr_t)rq; g_ctl->rq_cap = RQ_CAP; }
    }
    g_on = want;
    /* The renderer runs the game's vertex shaders on the GPU (com.c reads
     * GPUVS at its first draw, after this); the CPU interpreter is 3-4x slower
     * in wasm.  An explicit GPUVS=0 still wins. */
    if (g_on) setenv("GPUVS", "1", 0);
    if (g_on){
        /* the translator's built-in modules (docs/web/gpu-abi.md section 1): vs_0
         * for TVert draws, ps_0 / ps_256 for draws without a pixel shader */
        static char t[16384];
        if (wgsl_tvert_module(t, sizeof t) > 0) shader_put(GW_SHADER_VS, 0, t);
        if (wgsl_ff_module(0, t, sizeof t, 0u) > 0) shader_put(GW_SHADER_PS, 0, t);
        if (wgsl_ff_module(256, t, sizeof t, 1u) > 0) shader_put(GW_SHADER_PS, 256, t);
    }
    nfsu2_register(HOST_KIND_GPU, g_ctl, sizeof *g_ctl);
    nfsu2_register(HOST_KIND_VSYNC, g_vs, sizeof *g_vs);
    fprintf(stderr, "[gpu] %s (control block 0x%x, layout %08x)\n",
            g_on ? "WebGPU renderer: recording for the page" : "no renderer (NFSU2_RENDERER unset): draws dropped",
            (unsigned)(uintptr_t)g_ctl, GW_LAYOUT);
}

/* The page sets attached once its renderer drains the block.  Commands queue
 * up before that; the first present (or a full queue) waits for it. */
static int gw_attached(void){
    if (!g_on) return 0;
    int32_t a = ald(&g_ctl->attached);
    if (a == GW_ATTACHED) return 1;
    double t0 = now_ms();
    if (!a){
        double lim = ENVC("NFSU2_RENDERER_WAIT_MS") ? atof(ENVC("NFSU2_RENDERER_WAIT_MS")) : 30000.0;
        fprintf(stderr, "[gpu] waiting for the page's renderer to attach\n");
        guest_wait_begin();
        while (!(a = ald(&g_ctl->attached)) && now_ms() - t0 < lim) wait32(&g_ctl->attached, 0, 50);
        guest_wait_end();
        /* gave up waiting: the block says so (0 -> GW_DETACHED), so a renderer
         * that comes later is refused and the page reports it, instead of
         * attaching to a game that no longer records (a black screen).  The
         * page attaches with a CAS from 0 too: exactly one of the two wins. */
        if (!a){
            int32_t zero = 0;
            if (__atomic_compare_exchange_n(&g_ctl->attached, &zero, GW_DETACHED, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                wake(&g_ctl->attached);
            else a = zero;                   /* a renderer attached (or let go) just now */
        }
        if (a == GW_ATTACHED){ fprintf(stderr, "[gpu] renderer attached after %.0f ms\n", now_ms() - t0); return 1; }
    }
    /* never attached, or the page's renderer let go (detach): no frame will be
     * taken again, so stop recording now instead of stalling the game */
    if (a == GW_DETACHED) fprintf(stderr, "[gpu] the page's renderer detached: dropping draws from now on\n");
    else fprintf(stderr, "[gpu] no renderer attached after %.0f ms: dropping draws from now on\n", now_ms() - t0);
    g_on = 0; g_detached = 1;
    return 0;
}

/* ---- resource queue ---------------------------------------------------- */
static uint32_t g_rq_bell_at = 0;
static void ring_bell(void){
    __atomic_fetch_add(&g_ctl->bell, 1, __ATOMIC_SEQ_CST);
    wake(&g_ctl->bell);
    g_rq_bell_at = (uint32_t)ald(&g_ctl->rq_head);
}
/* A record with payload bytes after the 32-byte header, or NULL (renderer off
 * or the record cannot fit). */
static GwCmd *rq_begin(uint32_t op, uint32_t payload){
    if (!g_on) return NULL;
    uint32_t cap = g_ctl->rq_cap, len = (GW_CMD_HDR + payload + 15u) & ~15u;
    if (len > cap / 2){
        fprintf(stderr, "[gpu] resource record of %u bytes does not fit the queue: dropped (op %u)\n", len, op);
        return NULL;
    }
    uint8_t *ring = (uint8_t *)(uintptr_t)g_ctl->rq_ptr;
    for (int waited = 0;;){
        uint32_t head = (uint32_t)g_ctl->rq_head, tail = (uint32_t)ald(&g_ctl->rq_tail);
        uint32_t off = head & (cap - 1u), pad = off + len > cap ? cap - off : 0u;
        if (head - tail + pad + len <= cap){
            if (pad){
                GwCmd *p = (GwCmd *)(ring + off); p->op = GW_PAD; p->len = pad;
                head += pad; ast(&g_ctl->rq_head, (int32_t)head);
            }
            GwCmd *c = (GwCmd *)(ring + (head & (cap - 1u)));
            memset(c, 0, GW_CMD_HDR); c->op = op; c->len = len;
            return c;
        }
        /* full: the page drains it (it may be waiting for this doorbell) */
        if (!gw_attached()) return NULL;
        ring_bell();
        double t0 = now_ms();
        guest_wait_begin();
        wait32(&g_ctl->rq_tail, (int32_t)tail, 100);
        guest_wait_end();
        g_wait_rq_ms += now_ms() - t0;
        if (++waited == 1) fprintf(stderr, "[gpu] resource queue full (%u MB): waiting for the page\n", cap >> 20);
    }
}
static void rq_commit(GwCmd *c){
    uint32_t head = (uint32_t)g_ctl->rq_head + c->len;
    g_rq_bytes += c->len;
    ast(&g_ctl->rq_head, (int32_t)head);
    if (head - g_rq_bell_at > g_ctl->rq_cap / 8u) ring_bell();
}

/* ---- textures -----------------------------------------------------------
 * Ids, sizes and formats live here (com.c reads them back synchronously);
 * the GPU textures live on the page.  Freed ids are reused three guest
 * frames later, as on the Mac: frames recorded before the Release may still
 * name the id.  pack: a texture pack's texture stands in for the game's
 * (metal_replace_texture), whose own uploads are dropped from then on. */
#define MAXTEX 16384
static struct { uint16_t w, h; uint8_t levels, cube, rt, live, pack; uint32_t fmt, lvlmask; } g_tex[MAXTEX];
static unsigned g_texn = 1;
static unsigned g_tfree[MAXTEX], g_ntfree = 0;
#define TFQ 16384
static struct { unsigned id; uint32_t at; } g_tfq[TFQ];
static unsigned g_tfq_h = 0, g_tfq_n = 0;

static unsigned fmt_blk(unsigned f, unsigned *bpb){
    switch (f){
    case 21: case 22: *bpb = 4; return 1;                 /* A8R8G8B8, X8R8G8B8 */
    case 28: case 41: *bpb = 1; return 1;                 /* A8, "L8" (P8, as on the Mac) */
    case 0x31545844: *bpb = 8; return 4;                  /* DXT1 */
    case 0x33545844: case 0x35545844: *bpb = 16; return 4;/* DXT3, DXT5 */
    default: *bpb = 0; return 0;
    }
}
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned f){
    unsigned bpb, blk = fmt_blk(f, &bpb); if (!blk) return 0;
    unsigned lw = w >> level; if (!lw) lw = 1;
    return blk == 1 ? lw * bpb : ((lw + blk - 1) / blk) * bpb;
}
unsigned metal_level_rows(unsigned h, unsigned level, unsigned f){
    unsigned bpb, blk = fmt_blk(f, &bpb); if (!blk) return 0;
    unsigned lh = h >> level; if (!lh) lh = 1;
    return blk == 1 ? lh : (lh + blk - 1) / blk;
}
static unsigned tex_new(unsigned w, unsigned h, unsigned levels, unsigned f, unsigned rt, unsigned cube){
    gw_init();
    unsigned bpb;
    if (!fmt_blk(f, &bpb) || !w || !h || w > 16384 || h > 16384) return 0;
    unsigned id = 0;
    if (g_ntfree) id = g_tfree[--g_ntfree];
    else if (g_texn < MAXTEX) id = g_texn++;
    if (!id) return 0;
    if (!levels) levels = 1;
    g_tex[id].w = (uint16_t)w; g_tex[id].h = (uint16_t)h; g_tex[id].levels = (uint8_t)(levels > 255 ? 255 : levels);
    g_tex[id].cube = cube ? 1 : 0; g_tex[id].rt = rt ? 1 : 0; g_tex[id].live = 1; g_tex[id].pack = 0;
    g_tex[id].fmt = f; g_tex[id].lvlmask = 0;
    g_tex_created++;
    GwCmd *c = rq_begin(GW_TEX_CREATE, 0);
    if (c){ c->a[0] = id; c->a[1] = w; c->a[2] = h; c->a[3] = levels; c->a[4] = f;
            c->a[5] = (rt ? GW_TF_RT : 0) | (cube ? GW_TF_CUBE : 0); rq_commit(c); }
    return id;
}
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels, unsigned f, unsigned is_rt){
    return tex_new(w, h, levels, f, is_rt, 0);
}
unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned f){ return tex_new(w, h, levels, f, 0, 0); }
/* Cube maps are always render targets (the game draws the car's environment
 * map face by face) and can also be written with LockRect. */
unsigned metal_create_texture_cube(unsigned edge, unsigned levels, unsigned f, unsigned is_rt){
    return tex_new(edge, edge, levels, f, is_rt, 1);
}
int  metal_tex_is_cube(unsigned id){ return id && id < MAXTEX && g_tex[id].live ? g_tex[id].cube : 0; }
void metal_tex_size(unsigned id, unsigned *w, unsigned *h){
    int ok = id && id < MAXTEX && g_tex[id].live;
    if (w) *w = ok ? g_tex[id].w : 0;
    if (h) *h = ok ? g_tex[id].h : 0;
}
void metal_free_texture(unsigned id){
    if (!id || id >= g_texn || id >= MAXTEX || !g_tex[id].live) return;
    if (g_tfq_n >= TFQ) return;                        /* keep it rather than risk a live id */
    g_tfq[(g_tfq_h + g_tfq_n) % TFQ].id = id;
    g_tfq[(g_tfq_h + g_tfq_n) % TFQ].at = g_gframe;
    g_tfq_n++;
}
static void tex_free_due(int now){
    while (g_tfq_n){
        unsigned i = g_tfq_h;
        if (!now && g_gframe - g_tfq[i].at < 3u) break;
        unsigned id = g_tfq[i].id;
        g_tfq_h = (g_tfq_h + 1) % TFQ; g_tfq_n--;
        if (!g_tex[id].live) continue;
        g_tex[id].live = 0; g_tex_released++;
        texrep_released(id);
        GwCmd *c = rq_begin(GW_TEX_FREE, 0);
        if (c){ c->a[0] = id; rq_commit(c); }
        g_tfree[g_ntfree++] = id;
    }
}
unsigned long metal_tex_released(void){ return g_tex_released; }
long metal_tex_created(void){ return g_tex_created; }
long metal_tex_uploaded(void){ return g_tex_uploaded; }

/* UnlockRect: the whole level, copied now (the lock buffer is rewritten at
 * the next Lock and freed at Release). */
static void tex_upload(unsigned id, unsigned level, unsigned face, const void *data,
                       unsigned pitch, unsigned w, unsigned h, unsigned f){
    unsigned bpb, blk = fmt_blk(f, &bpb);
    if (!blk || level >= g_tex[id].levels) return;
    unsigned lw = w >> level, lh = h >> level; if (!lw) lw = 1; if (!lh) lh = 1;
    unsigned rows = blk == 1 ? lh : (lh + blk - 1) / blk;
    unsigned need = (blk == 1 ? lw * bpb : ((lw + blk - 1) / blk) * bpb);
    if (pitch < need) return;
    if (g_tex[id].cube && face > 5u) face = 0;
    if (level < 32u) g_tex[id].lvlmask |= 1u << level;
    g_tex_uploaded++;
    uint32_t bytes = pitch * rows;
    GwCmd *c = rq_begin(GW_TEX_UPLOAD, bytes);
    if (!c) return;
    c->a[0] = id; c->a[1] = level | (face << 16); c->a[2] = lw; c->a[3] = lh; c->a[4] = pitch; c->a[5] = rows;
    memcpy((uint8_t *)c + GW_CMD_HDR, data, bytes);
    rq_commit(c);
}
void metal_upload_texture_face(unsigned id, unsigned level, unsigned face, const void *data,
                               unsigned pitch, unsigned w, unsigned h, unsigned f){
    if (!id || id >= MAXTEX || !g_tex[id].live || !data || g_tex[id].pack) return;
    tex_upload(id, level, face, data, pitch, w, h, f);
}
/* texrep_web.c: the texture becomes a pack's, w x h in format f with the
 * levels in data (level 0 first, each metal_level_pitch x metal_level_rows);
 * the game's own uploads to it are dropped from then on.  data NULL makes it
 * the game's again, empty, at the size given (the game's), for its uploads. */
void metal_replace_texture(unsigned id, unsigned w, unsigned h, unsigned levels, unsigned f, const uint8_t *data){
    unsigned bpb;
    if (!id || id >= MAXTEX || !g_tex[id].live || g_tex[id].rt || g_tex[id].cube || !fmt_blk(f, &bpb)) return;
    if (!levels) levels = 1;
    g_tex[id].w = (uint16_t)w; g_tex[id].h = (uint16_t)h; g_tex[id].levels = (uint8_t)(levels > 255 ? 255 : levels);
    g_tex[id].fmt = f; g_tex[id].lvlmask = 0; g_tex[id].pack = data != NULL;
    GwCmd *c = rq_begin(GW_TEX_CREATE, 0);        /* the page frees the old one first */
    if (c){ c->a[0] = id; c->a[1] = w; c->a[2] = h; c->a[3] = levels; c->a[4] = f; c->a[5] = 0; rq_commit(c); }
    for (unsigned l = 0; data && l < levels; l++){
        unsigned pitch = metal_level_pitch(w, l, f);
        tex_upload(id, l, 0u, data, pitch, w, h, f);
        data += (size_t)pitch * metal_level_rows(h, l, f);
    }
}
void metal_upload_texture(unsigned id, unsigned level, const void *data, unsigned pitch, unsigned w, unsigned h, unsigned f){
    metal_upload_texture_face(id, level, 0u, data, pitch, w, h, f);
}
void metal_miplevel_report(void){
    unsigned bad = 0, tot = 0;
    for (unsigned i = 1; i < g_texn && i < MAXTEX; i++){
        if (!g_tex[i].live || g_tex[i].levels <= 1 || g_tex[i].rt) continue;
        tot++;
        unsigned lv = g_tex[i].levels, want = lv >= 32 ? 0xFFFFFFFFu : (1u << lv) - 1u;
        if ((g_tex[i].lvlmask & want) != want) bad++;
    }
    fprintf(stderr, "  mip levels        : %u multi-level textures, %u with levels never uploaded\n", tot, bad);
}
void metal_mem_stats(uint64_t *a, unsigned *live, unsigned *slots, uint64_t *bytes){
    unsigned n = 0; uint64_t b = 0;
    for (unsigned i = 1; i < g_texn && i < MAXTEX; i++) if (g_tex[i].live){
        n++;
        unsigned bpb, blk = fmt_blk(g_tex[i].fmt, &bpb);
        for (unsigned l = 0; l < g_tex[i].levels && blk; l++)
            b += (uint64_t)metal_level_pitch(g_tex[i].w, l, g_tex[i].fmt) * metal_level_rows(g_tex[i].h, l, g_tex[i].fmt)
                 * (g_tex[i].cube ? 6u : 1u);
    }
    if (a) *a = b; if (live) *live = n; if (slots) *slots = g_texn; if (bytes) *bytes = b;
}
/* Release frees render targets (NFSU2_COMFREE default); nothing to do at Reset. */
void metal_reset_rts(void){ }
void metal_z_forget(unsigned zid){
    gw_init();
    GwCmd *c = rq_begin(GW_Z_FORGET, 0);
    if (c){ c->a[0] = zid; rq_commit(c); }
}

/* ---- the persistent vertex/index mirror pool ----------------------------
 * As metal.m (PERSISTENT VERTEX / INDEX POOL): static VBs/IBs are copied once
 * into one pool that the page keeps as a GPU buffer; indexed draws read
 * from it.  The game side keeps a CPU shadow (com.c scans index ranges in
 * it); every new mirror is also queued as a POOL_WRITE with its bytes, so
 * the shadow can move when it grows. */
#define MIR_BITS  12u
#define MIR_N     (1u << MIR_BITS)
#define MIR_TOMB  1u
#define PF_N      4096
#define PF_DELAY  8u
#define MIR_IDLE  900u
#define POOL_MAX  (768u << 20)
static uint8_t *g_pool;
static uint32_t g_pool_cap = 0, g_pool_top = 0;
static struct { uint32_t key, gen, off, size, imax, used; } g_mir[MIR_N];
/* The keys are guest object addresses, 16-byte aligned: the top bits of the
 * product (the low ones depend only on the key's low bits, and used to put
 * every key in 256 of the 4,096 slots, 65 ns a lookup). */
static inline unsigned mir_hash(uint32_t key){ return (key * 2654435761u) >> (32u - MIR_BITS); }
static struct { uint32_t off, size, at; } g_pf[PF_N];
static int g_pfn = 0;
static unsigned long g_mir_build = 0, g_mir_drop = 0, g_mir_live = 0, g_mir_full = 0, g_pf_lost = 0;
static unsigned long long g_mir_bytes = 0;
static int pool_reserve(uint32_t top){
    if (top <= g_pool_cap) return 1;
    uint32_t nc = g_pool_cap ? g_pool_cap : (4u << 20);
    while (nc < top) nc += nc / 2;
    if (nc > POOL_MAX) return 0;
    uint8_t *np = realloc(g_pool, nc);
    if (!np) return 0;
    g_pool = np; g_pool_cap = nc;
    GwCmd *c = rq_begin(GW_POOL_GROW, 0);
    if (c){ c->a[0] = nc; rq_commit(c); }
    return 1;
}
static int pool_alloc(uint32_t want, uint32_t *off, uint32_t *size){
    want = (want + 255u) & ~255u;
    for (int i = 0; i < g_pfn; i++){
        if (g_pf[i].size < want || g_gframe - g_pf[i].at < PF_DELAY) continue;
        *off = g_pf[i].off;
        if (g_pf[i].size - want >= 4096u){ *size = want; g_pf[i].off += want; g_pf[i].size -= want; }
        else { *size = g_pf[i].size; g_pf[i] = g_pf[--g_pfn]; }
        return 1;
    }
    if (!pool_reserve(g_pool_top + want)) return 0;
    *off = g_pool_top; *size = want; g_pool_top += want;
    return 1;
}
static void pool_free(uint32_t off, uint32_t size){
    if (!size) return;
    for (int i = 0; i < g_pfn; ){
        if (g_pf[i].off + g_pf[i].size == off || off + size == g_pf[i].off){
            if (g_pf[i].off < off) off = g_pf[i].off;
            size += g_pf[i].size;
            g_pf[i] = g_pf[--g_pfn];
            i = 0; continue;
        }
        i++;
    }
    if (g_pfn < PF_N){ g_pf[g_pfn].off = off; g_pf[g_pfn].size = size; g_pf[g_pfn].at = g_gframe; g_pfn++; }
    else g_pf_lost++;
}
static void mir_sweep(void){
    unsigned tomb = 0;
    for (unsigned i = 0; i < MIR_N; i++){
        if (g_mir[i].key == MIR_TOMB) tomb++;
        if (g_mir[i].key <= MIR_TOMB || g_gframe - g_mir[i].used < MIR_IDLE) continue;
        pool_free(g_mir[i].off, g_mir[i].size);
        g_mir[i].key = MIR_TOMB; g_mir_drop++; g_mir_live--; tomb++;
    }
    /* A lookup probes past tombstones until it finds the key or an empty
     * slot, so dropped mirrors made every DrawIndexedPrimitive's two lookups
     * long walks (a race: ~750 live entries and ~2,000 tombstones of 4,096).
     * Rebuilt without them once they are an eighth of the table. */
    if (tomb > MIR_N / 8u){
        static __typeof__(g_mir[0]) live[MIR_N];
        unsigned n = 0;
        for (unsigned i = 0; i < MIR_N; i++) if (g_mir[i].key > MIR_TOMB) live[n++] = g_mir[i];
        memset(g_mir, 0, sizeof g_mir);
        for (unsigned k = 0; k < n; k++){
            unsigned h = mir_hash(live[k].key);
            while (g_mir[h].key) h = (h + 1u) & (MIR_N - 1u);
            g_mir[h] = live[k];
        }
    }
}
int metal_mirror(uint32_t key, uint32_t gen, const void *src, uint32_t size, unsigned isz, uint32_t *off, uint32_t *imax){
    gw_init();
    if (!g_on || key <= MIR_TOMB || !size || size > (64u << 20)) return 0;
    { static uint32_t last = 0; if (g_gframe - last >= 256u){ last = g_gframe; mir_sweep(); } }
    unsigned h = mir_hash(key), slot = MIR_N, q;
    for (q = 0; q < 64u; q++){
        unsigned i = (h + q) & (MIR_N - 1u);
        if (g_mir[i].key == key){ slot = i; break; }
        if (g_mir[i].key <= MIR_TOMB && slot == MIR_N) slot = i;
        if (!g_mir[i].key) break;
    }
    if (slot == MIR_N){ g_mir_full++; return 0; }
    if (g_mir[slot].key == key && g_mir[slot].gen == gen && g_mir[slot].size >= size){
        g_mir[slot].used = g_gframe;
        *off = g_mir[slot].off; if (imax) *imax = g_mir[slot].imax;
        return 1;
    }
    if (g_mir[slot].key == key){ pool_free(g_mir[slot].off, g_mir[slot].size); g_mir[slot].key = MIR_TOMB; g_mir_live--; }
    uint32_t o, sz;
    if (!pool_alloc(size, &o, &sz)) return 0;
    uint32_t wlen = (size + 3u) & ~3u;                 /* writeBuffer moves whole words */
    GwCmd *c = rq_begin(GW_POOL_WRITE, wlen);
    if (!c) return 0;
    uint8_t *dst = g_pool + o;
    memcpy(dst, src, size);
    c->a[0] = o; c->a[1] = wlen;
    memcpy((uint8_t *)c + GW_CMD_HDR, src, size);
    if (wlen > size) memset((uint8_t *)c + GW_CMD_HDR + size, 0, wlen - size);
    rq_commit(c);
    uint32_t mx = 0;
    if (isz == 2u){ const uint16_t *ix = (const uint16_t *)dst; for (uint32_t k = 0; k < size / 2u; k++) if (ix[k] > mx) mx = ix[k]; }
    else if (isz == 4u){ const uint32_t *ix = (const uint32_t *)dst; for (uint32_t k = 0; k < size / 4u; k++) if (ix[k] > mx) mx = ix[k]; }
    g_mir[slot].key = key; g_mir[slot].gen = gen; g_mir[slot].off = o; g_mir[slot].size = sz;
    g_mir[slot].imax = mx; g_mir[slot].used = g_gframe;
    g_mir_build++; g_mir_live++; g_mir_bytes += size;
    *off = o; if (imax) *imax = mx;
    return 1;
}
void metal_mirror_forget(uint32_t key){
    if (key <= MIR_TOMB) return;
    unsigned h = mir_hash(key);
    for (unsigned q = 0; q < 64u; q++){
        unsigned i = (h + q) & (MIR_N - 1u);
        if (g_mir[i].key == key){ pool_free(g_mir[i].off, g_mir[i].size); g_mir[i].key = MIR_TOMB; g_mir_live--; g_mir_drop++; return; }
        if (!g_mir[i].key) return;
    }
}
const uint8_t *metal_pool_cpu(void){ return g_pool; }

/* ---- shaders --------------------------------------------------------------
 * The translator (vsemu.c, track gpu-shaders) hands over WGSL.  Until then
 * com.c passes MSL, which the page cannot use: such a vertex shader reports
 * "not ready" (com.c then runs it on the CPU and records TVerts) and such a
 * pixel shader is ignored (the page draws those draws with its built-in
 * fixed-function fragment stage). */
#define MAXVSF 512
#define MAXPS  512
static uint8_t g_vs_ok[MAXVSF], g_ps_ok[MAXPS];
static unsigned g_vs_n = 0, g_ps_n = 0, g_msl_vs = 0, g_msl_ps = 0;
static int is_wgsl(const char *s){ return s && (strstr(s, "@vertex") || strstr(s, "@fragment")); }
static void shader_put(int op, int id, const char *src){
    uint32_t n = (uint32_t)strlen(src) + 1u;
    GwCmd *c = rq_begin((uint32_t)op, n);
    if (!c) return;
    c->a[0] = (uint32_t)id; c->a[1] = n;
    memcpy((uint8_t *)c + GW_CMD_HDR, src, n);
    rq_commit(c);
}
void metal_register_vs(int id, const char *src){
    gw_init();
    if (id <= 0 || id >= MAXVSF || !src || g_vs_ok[id]) return;
    if (!g_on) return;
    if (!is_wgsl(src)){ g_msl_vs++; return; }
    shader_put(GW_SHADER_VS, id, src);
    g_vs_ok[id] = 1; g_vs_n++;
}
/* Without a renderer every shader "compiles" (draws are dropped anyway and the
 * CPU vertex path would only cost time). */
int metal_vs_ready(int id){ return !g_on ? 1 : (id > 0 && id < MAXVSF && g_vs_ok[id]); }
void metal_register_ps(int id, const char *src){
    gw_init();
    if (id <= 0 || id >= MAXPS || !src || g_ps_ok[id] || !g_on) return;
    if (!is_wgsl(src)){ g_msl_ps++; return; }
    shader_put(GW_SHADER_PS, id, src);
    g_ps_ok[id] = 1; g_ps_n++;
}

/* ---- state setters (com.c rs_apply / draw_up) ---------------------------- */
void metal_set_blend(unsigned key, float aref){ g_cur_blend = key; g_cur_aref = aref; }
void metal_set_blendcolor(unsigned argb){ g_cur_bcol = argb; }
void metal_set_clear_color(unsigned argb){ g_clear_argb = argb; }
void metal_set_cull(unsigned m){ g_cur_cull = (m >= 1u && m <= 3u) ? m : 3u; }
void metal_set_depth(unsigned mode){ g_cur_dmode = mode & 63u; g_cur_depth = g_cur_dmode | (g_cur_zid << 8); }
void metal_set_zsurf(unsigned zid){ if (zid >= 64u) zid = 0; g_cur_zid = zid; g_cur_depth = g_cur_dmode | (g_cur_zid << 8); }
void metal_set_fog(unsigned enable, unsigned argb){
    static int off = -1;
    if (off < 0){ const char *e = ENVC("NOFOG"); off = (e && *e && *e != '0'); }
    if (off) enable = 0;
    g_cur_fog[0] = (float)((argb >> 16) & 0xFFu) / 255.0f;
    g_cur_fog[1] = (float)((argb >>  8) & 0xFFu) / 255.0f;
    g_cur_fog[2] = (float)( argb        & 0xFFu) / 255.0f;
    g_cur_fog[3] = enable ? 1.0f : 0.0f;
}
void metal_set_render_target(unsigned t){ g_cur_target = t; }
void metal_set_samplers(const unsigned k[4]){ for (int i = 0; i < 4; i++) g_cur_smp[i] = k[i]; }
void metal_set_stage_tex(unsigned a, unsigned b, unsigned c3){ g_cur_tex1 = a; g_cur_tex2 = b; g_cur_tex3 = c3; }
void metal_set_ps(int id, const float *c32){
    g_cur_ps = id;
    if (c32 && memcmp(g_cur_psc, c32, sizeof g_cur_psc)){ memcpy(g_cur_psc, c32, sizeof g_cur_psc); g_psc_ver++; }
}
void metal_set_texproj(const float p[4]){ memcpy(g_cur_proj, p, sizeof g_cur_proj); }
/* com.c, every draw: the 2D UI on a screen narrower than 4:3 is shown in a 4:3
 * band across the middle (ws_ui_y = sy < 1, the band's share of the height);
 * the draw is kept inside it, so what the game parks outside its 640x480 HUD
 * space stays hidden as on a 4:3 screen.  sy = 1: the whole target. */
static uint32_t g_cur_clip;
void metal_set_ui_clip(float sy){
    uint32_t h = g_bb_h;
    if (!(sy > 0.f && sy < 1.f) || !h){ g_cur_clip = 0; return; }
    uint32_t y0 = (uint32_t)lroundf((float)h * (1.f - sy) * 0.5f), y1 = (uint32_t)lroundf((float)h * (1.f + sy) * 0.5f);
    g_cur_clip = y0 | (y1 > h ? h : y1) << 16;
}
void metal_begin_frame(void){ }

/* ---- records ------------------------------------------------------------- */
static unsigned psc_put(Buf *B){
    if (B->pscn && B->psc_lastver == g_psc_ver) return B->psc_last;
    if (B->pscn >= MAXD) return B->psc_last;
    memcpy(B->psc[B->pscn], g_cur_psc, sizeof g_cur_psc);
    B->psc_last = B->pscn++; B->psc_lastver = g_psc_ver;
    return B->psc_last;
}
#define PSCUBE 256
static int ps_of(unsigned tex){
    return (g_cur_ps > 0 && g_cur_ps < PSCUBE && metal_tex_is_cube(tex)) ? g_cur_ps + PSCUBE : g_cur_ps;
}
/* the record every draw shares: state at the time of the call */
static GwDraw *rec_draw(Buf *B, unsigned tex){
    GwDraw *D = &B->d[B->dn];
    memset(D, 0, sizeof *D);        /* slots are reused across frames (see metal.m's is_blit story) */
    D->kind = GW_DRAW; D->tex = tex; D->target = g_cur_target;
    D->blend = g_cur_blend; D->aref = g_cur_aref; D->depth = g_cur_depth;
    D->tex1 = g_cur_tex1; D->tex2 = g_cur_tex2; D->tex3 = g_cur_tex3;
    D->ps = ps_of(tex); D->psci = psc_put(B);
    memcpy(D->proj, g_cur_proj, sizeof D->proj);
    memcpy(D->smp, g_cur_smp, sizeof D->smp);
    D->bcol = g_cur_bcol; D->cull = g_cur_cull;
    memcpy(D->fogv, g_cur_fog, sizeof D->fogv);
    D->clip = g_cur_clip;
    return D;
}
void metal_draw_tv(const TVert *src, unsigned nverts, unsigned prim, unsigned tex){
    if (nverts < 3) return;
    unsigned tris = (prim == 4) ? nverts / 3 : nverts - 2;
    if (!tris) return;
    g_tris += tris;
    if (!g_on) return;
    Buf *B = &g_b[g_cur];
    if (B->vn + tris * 3 > MAXV){ g_ov_v++; return; }
    if (B->dn >= MAXD){ g_ov_d++; return; }
    TVert *dst = B->v + B->vn;
    for (unsigned t = 0; t < tris; t++){
        unsigned idx[3];
        if (prim == 6){ idx[0] = 0; idx[1] = t + 1; idx[2] = t + 2; }                 /* fan */
        else if (prim == 5){                                                            /* strip */
            if (t & 1){ idx[0] = t + 1; idx[1] = t; idx[2] = t + 2; }
            else      { idx[0] = t;     idx[1] = t + 1; idx[2] = t + 2; }
        }
        else { idx[0] = t * 3; idx[1] = t * 3 + 1; idx[2] = t * 3 + 2; }               /* list */
        for (int k = 0; k < 3; k++) *dst++ = src[idx[k]];
    }
    GwDraw *D = rec_draw(B, tex);
    D->first = B->vn; D->count = tris * 3;
    B->dn++; B->vn += tris * 3;
}
/* the VS constant block {u32 stride, tsel, base, 0; vec4 c[nk]}, deduplicated
 * against the previous one.  base: byte offset of vertex 0 in the buffer the
 * shader pulls from (raw arena or pool).  Blocks are 16-byte aligned: the page
 * binds the arena in 64 KB windows and finds a block by its row there
 * (GW_KWIN_ROWS), so a block needs no dynamic offset of its own. */
static GwDraw *raw_rec(Buf *B, unsigned stride, unsigned base, unsigned tex, int vsid, const float *k, unsigned nk){
    unsigned ksz = 16u + nk * 16u;
    uint32_t hdr[4] = { stride, 0, base, 0 };
    for (int q = 0; q < 4; q++) hdr[1] |= ((uint32_t)((int)g_cur_proj[q]) & 3u) << (8 * q);
    unsigned koff;
    if (B->last_ksz == ksz && B->last_vs == vsid && B->kn &&
        !memcmp(B->kb + B->last_koff, hdr, 16) && !memcmp(B->kb + B->last_koff + 16, k, nk * 16u)){
        koff = B->last_koff;
    } else {
        koff = (B->kn + 15u) & ~15u;
        if (koff + ksz > MAXKB){ g_ov_kb++; return NULL; }
        memcpy(B->kb + koff, hdr, 16);
        memcpy(B->kb + koff + 16, k, nk * 16u);
        B->kn = koff + ksz;
        B->last_koff = koff; B->last_ksz = ksz; B->last_vs = vsid;
    }
    GwDraw *D = rec_draw(B, tex);
    D->vsid = vsid; D->koff = koff; D->ksz = ksz;
    return D;
}
int metal_draw_raw(const void *src, unsigned nverts, unsigned stride, unsigned prim, unsigned tex, int vsid,
                   const float *k, unsigned nk){
    if (nverts < 3 || !stride || stride > 256u) return 0;
    unsigned tris = (prim == 4) ? nverts / 3 : nverts - 2;
    if (!tris) return 0;
    g_tris += tris;
    if (!g_on) return 1;
    Buf *B = &g_b[g_cur];
    const uint8_t *S = (const uint8_t *)src;
    unsigned need = tris * 3u * stride, roff = (B->rvn + 255u) & ~255u;
    if (roff + need > MAXRV){ g_ov_rv++; return 0; }
    if (B->dn >= MAXD){ g_ov_d++; return 0; }
    GwDraw *D = raw_rec(B, stride, roff, tex, vsid, k, nk);
    if (!D) return 0;
    uint8_t *dst = B->rv + roff;
    if (prim == 4) memcpy(dst, S, need);
    else for (unsigned t = 0; t < tris; t++){
        unsigned idx[3];
        if (prim == 6){ idx[0] = 0; idx[1] = t + 1; idx[2] = t + 2; }
        else if (t & 1){ idx[0] = t + 1; idx[1] = t; idx[2] = t + 2; }
        else { idx[0] = t; idx[1] = t + 1; idx[2] = t + 2; }
        for (int q = 0; q < 3; q++){ memcpy(dst, S + (size_t)idx[q] * stride, stride); dst += stride; }
    }
    B->rvn = roff + need;
    D->count = tris * 3; D->rawoff = roff;
    B->dn++;
    return 1;
}
int metal_draw_ix(unsigned vbo, unsigned stride, unsigned ibo, unsigned isz, unsigned nidx, int base, unsigned tex,
                  int vsid, const float *k, unsigned nk, unsigned long long gh){
    (void)gh;
    if (!g_pool || nidx < 3 || !stride || stride > 256u || (isz != 2u && isz != 4u)) return 0;
    g_tris += nidx / 3;
    if (!g_on) return 1;
    Buf *B = &g_b[g_cur];
    if (B->dn >= MAXD){ g_ov_d++; return 0; }
    GwDraw *D = raw_rec(B, stride, vbo, tex, vsid, k, nk);
    if (!D) return 0;
    D->count = nidx - nidx % 3u; D->rawoff = vbo;
    D->ix_isz = isz; D->ix_ib = ibo; D->ix_base = base;
    B->dn++;
    return 1;
}
void metal_raw_overflow(unsigned *rv, unsigned *kb){ if (rv) *rv = g_ov_rv; if (kb) *kb = g_ov_kb; }

/* StretchRect: an ordered record, the whole source (or r: left, top, right,
 * bottom in source pixels) to the whole destination. */
void metal_blit_rect(unsigned src, unsigned dst, const int *r){
    if (!g_on) return;
    Buf *B = &g_b[g_cur];
    if (B->dn >= MAXD){ g_ov_d++; return; }
    GwDraw *D = &B->d[B->dn++];
    memset(D, 0, sizeof *D);
    D->kind = GW_BLIT; D->target = dst; D->blit_src = src;
    if (r){
        uint32_t q[4];
        for (int i = 0; i < 4; i++) q[i] = r[i] < 0 ? 0u : r[i] > 0xFFFF ? 0xFFFFu : (uint32_t)r[i];
        if (q[2] > q[0] && q[3] > q[1]){ D->first = q[0] | q[1] << 16; D->count = q[2] | q[3] << 16; }
    }
}
void metal_blit(unsigned src, unsigned dst){ metal_blit_rect(src, dst, NULL); }
void metal_clear_target_ex(unsigned argb, unsigned flags, float z){
    if (!g_on) return;
    Buf *B = &g_b[g_cur];
    if (B->dn >= MAXD){ g_ov_d++; return; }
    { static unsigned ov = 0xFFFFFFFFu;          /* CLEARCOL=0xAARRGGBB: diagnostics */
      if (ov == 0xFFFFFFFFu){ const char *e = ENVC("CLEARCOL"); ov = e ? (unsigned)strtoul(e, 0, 0) : 0u; }
      if (ov) argb = ov; }
    GwDraw *D = &B->d[B->dn++];
    memset(D, 0, sizeof *D);
    D->kind = GW_CLEAR; D->target = g_cur_target; D->clr_argb = argb; D->clr_flags = flags; D->clr_z = z;
    D->depth = g_cur_depth;
}
void metal_clear_target(unsigned argb){ metal_clear_target_ex(argb, 1u, 1.f); }

/* ---- sizes ----------------------------------------------------------------
 * The view is the page's canvas in device pixels (GwCtl.view_w/h once the
 * page attached; before that NFSU2_WIDTH/HEIGHT, which the page sets from
 * the same canvas).  com.c res_pick turns it into the device size; the page
 * shows the back buffer aspect-fit in the canvas. */
static void view_wh(unsigned *w, unsigned *h){
    gw_init();
    unsigned vw = g_ctl ? g_ctl->view_w : 0, vh = g_ctl ? g_ctl->view_h : 0;
    if (!vw || !vh){
        const char *ew = getenv("NFSU2_WIDTH"), *eh = getenv("NFSU2_HEIGHT");
        vw = ew && *ew ? (unsigned)atoi(ew) : 1280u; vh = eh && *eh ? (unsigned)atoi(eh) : 720u;
    }
    if (vw < 320u || vh < 240u || vw > 8192u || vh > 8192u){ vw = 1280u; vh = 720u; }
    *w = vw & ~1u; *h = vh & ~1u;
}
/* the view size the game last chose its device size from (CreateDevice and
 * Reset read it through res_pick) */
static unsigned g_view_used_w, g_view_used_h;
void metal_view_size(unsigned *ow, unsigned *oh){
    unsigned w, h; view_wh(&w, &h);
    g_view_used_w = w; g_view_used_h = h;
    if (ow) *ow = w; if (oh) *oh = h;
}
/* The canvas changed size (window resize, fullscreen, another screen): once
 * the new size has held for 250 ms, ask the game for a device reset, as
 * metal.m does at the end of a live resize.  The game's Reset picks the new
 * device size from metal_view_size. */
void com_request_reset(void);
static void view_follow(void){
    static unsigned seen_w, seen_h; static double since = 0, asked = 0;
    if (!g_ctl || ald(&g_ctl->attached) != GW_ATTACHED || !g_view_used_w) return;
    unsigned w = g_ctl->view_w & ~1u, h = g_ctl->view_h & ~1u;
    if (w < 320u || h < 240u || (w == g_view_used_w && h == g_view_used_h)) { seen_w = w; seen_h = h; return; }
    double t = now_ms();
    if (w != seen_w || h != seen_h){ seen_w = w; seen_h = h; since = t; return; }
    if (t - since < 250.0 || (asked && t - asked < 2000.0)) return;
    asked = t;
    fprintf(stderr, "[gpu] canvas %ux%u (device chosen for %ux%u): requesting a device reset\n", w, h, g_view_used_w, g_view_used_h);
    com_request_reset();
}
/* a size with the game's own aspect that fills the view (pillarboxed), as
 * metal.m does for the screen */
void metal_native_size(unsigned gw, unsigned gh, unsigned *ow, unsigned *oh){
    if (!ow || !oh) return;
    *ow = gw; *oh = gh;
    if (!gw || !gh) return;
    unsigned vw, vh; view_wh(&vw, &vh);
    unsigned a = gw, b = gh; while (b){ unsigned t = a % b; a = b; b = t; }
    unsigned ax = gw / a, ay = gh / a;
    double u = fmin((double)vw / ax, (double)vh / ay);
    unsigned units = (unsigned)u, w = ax * units, h = ay * units;
    if (!units || w <= gw || h <= gh || w > 8192u || h > 8192u) return;
    *ow = w; *oh = h;
}
void metal_want_size(unsigned w, unsigned h){ if (w && h && w <= 8192u && h <= 8192u && !g_bb_w){ g_bb_w = w; g_bb_h = h; } }
void metal_resize(unsigned w, unsigned h){ if (w && h && w <= 8192u && h <= 8192u){ g_bb_w = w; g_bb_h = h; } }
void metal_screen_metrics(double *backing, double *panel){
    gw_init();
    double d = g_ctl && g_ctl->dpr1000 ? g_ctl->dpr1000 / 1000.0 : 1.0;
    if (backing) *backing = d;
    if (panel) *panel = 0.0;          /* no panel density on the web */
}

/* ---- display clock --------------------------------------------------------- */
int metal_display_hz(void){
    gw_init();
    int hz = g_vs ? __atomic_load_n(&g_vs->hz, __ATOMIC_RELAXED) : 0;
    if (hz > 0) return hz;
    const char *e = getenv("NFSU2_DISPLAY_HZ");
    return e && *e ? atoi(e) : 60;
}
static int32_t g_vs_seen = 0;
static void vsync_feed(void){
    if (!g_vs) return;
    int32_t t = ald(&g_vs->tick);
    if (t == g_vs_seen) return;
    double ts = 0, per = 0;
    for (int tries = 0; tries < 4; tries++){
        int32_t s0 = ald(&g_vs->seq);
        if (s0 & 1) continue;
        ts = g_vs->ts_ms; per = g_vs->period_ms;
        if (ald(&g_vs->seq) == s0) break;
        ts = 0;
    }
    if (ts > 0 && per > 1.0){ g_vs_seen = t; hfr_note_vsync(ts / 1000.0, per / 1000.0); }
}

/* ---- present --------------------------------------------------------------
 * handoff(): the buffer just recorded goes to the page, and recording moves
 * to the other one.  As on the Mac: wait (guest lock released) while the
 * previous frame has not been taken; a frame the page has not started to
 * encode is replaced in mailbox mode, or after 60 ms when the page is not
 * encoding at all (a hidden tab).  A frame being encoded is always waited
 * for: its buffer is the one the game would write next. */
/* runtime/headlights.c: the other cars' headlights for the frame being recorded */
void gw_headlights(const GwHL *blk){
    if (!g_on || !blk) return;
    memcpy(g_b[g_cur].kb + GW_K_HL, blk, sizeof *blk);
}
/* roam_game.c (through roam_web.c): quick chat's places over the cars for the frame being recorded */
void gw_marks(const GwMark *m, int n){
    if (!g_on) return;
    GwMarks *k = (GwMarks *)(g_b[g_cur].kb + GW_K_MARKS);
    if (n > GW_MARKS_MAX) n = GW_MARKS_MAX;
    if (n > 0) memcpy(k->m, m, sizeof *m * (size_t)n);
    k->n = n > 0 ? (uint32_t)n : 0;
}
/* tonemenu.c: Exposure (as a multiplier) and White Level for every frame from the next handed over */
static GwTone g_tone = { GW_TONE_MAGIC, 1.f, 1.f, 1.f };
void gw_tone(float mult, float white, float knee){ g_tone.mult = mult; g_tone.white = white; g_tone.knee = knee; }
void metal_request_dump(long guest_frame, const char *label){
    snprintf(g_dump_label, sizeof g_dump_label, "%s", label ? label : "dump");
    for (char *q = g_dump_label; *q; q++) if (*q == '/' || *q == ' ' || *q == '\\') *q = '_';
    g_dump_req = guest_frame;
}
static uint32_t g_rb_n = 0;       /* textures read back (metal_read_texture) */
static int      g_handed = 0;     /* the frame being recorded went to the page early, with a readback */
static void handoff(uint32_t rb_id, uint32_t rb_tex, uint32_t rb_dst, uint32_t rb_pitch, uint32_t rb_rows){
    Buf *B = &g_b[g_cur];
    GwFrame *F = &g_ctl->frame[g_cur];
    F->seq = g_gframe; F->dn = B->dn; F->vn = B->vn; F->rvn = B->rvn; F->kn = B->kn; F->pscn = B->pscn;
    F->draws = (uint32_t)(uintptr_t)B->d; F->tv = (uint32_t)(uintptr_t)B->v; F->raw = (uint32_t)(uintptr_t)B->rv;
    F->k = (uint32_t)(uintptr_t)B->kb; F->psc = (uint32_t)(uintptr_t)B->psc;
    memcpy(B->kb + GW_K_TONE, &g_tone, sizeof g_tone);
    F->rq_end = (uint32_t)g_ctl->rq_head;
    unsigned vw, vh; view_wh(&vw, &vh);
    F->bb_w = g_bb_w ? g_bb_w : vw; F->bb_h = g_bb_h ? g_bb_h : vh;
    F->clear_argb = g_frame_clear;
    F->pool_size = g_pool_cap;
    F->dump = 0;
    if (g_dump_req >= 0){
        F->dump = 1; F->dump_frame = (int32_t)g_dump_req;
        memcpy(F->dump_label, g_dump_label, sizeof F->dump_label);
        g_dump_req = -1;
    }
    F->rb_id = rb_id; F->rb_tex = rb_tex; F->rb_dst = rb_dst; F->rb_pitch = rb_pitch; F->rb_rows = rb_rows;
    int mailbox = hfr_mailbox();
    int32_t mine = g_cur + 1;
    double t0 = now_ms();
    int unlocked = 0, late = 0;
    for (;;){
        int32_t s = ald(&g_ctl->state);
        int ready = GW_ST_READY(s), enc = GW_ST_ENC(s);
        if (!ready){
            if (__atomic_compare_exchange_n(&g_ctl->state, &s, s | mine, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
            continue;
        }
        /* a frame is pending: replace it (mailbox, or the page is not encoding for 60 ms) or wait */
        /* (the page clears its encode claim before it can detach: both run on its main thread) */
        int replace = !enc && (mailbox || ald(&g_ctl->attached) != GW_ATTACHED || (late && now_ms() - t0 >= 60.0));
        if (replace){
            GwFrame *old = &g_ctl->frame[ready - 1];
            if (__atomic_compare_exchange_n(&g_ctl->state, &s, (s & ~3) | mine, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)){
                if (old->dump && !F->dump){   /* a requested dump moves to the frame that replaces it */
                    F->dump = 1; F->dump_frame = old->dump_frame; memcpy(F->dump_label, old->dump_label, sizeof F->dump_label); }
                if (mailbox) g_replaced++; else g_dropped++;
                break;
            }
            continue;
        }
        if (!unlocked){ guest_wait_begin(); unlocked = 1; }
        ring_bell();
        double left = 60.0 - (now_ms() - t0);
        if (wait32(&g_ctl->state, s, left > 1.0 ? left : 1.0) == 2 || now_ms() - t0 >= 60.0) late = 1;
    }
    if (unlocked) guest_wait_end();
    g_wait_present_ms += now_ms() - t0;
    ring_bell();
    /* the other buffer is free now (the page released it before taking ours, or it was replaced) */
    g_cur ^= 1;
    Buf *N = &g_b[g_cur];
    N->dn = N->vn = N->rvn = N->pscn = 0; N->kn = KFIRST; N->last_ksz = 0;
    ((GwHL *)(N->kb + GW_K_HL))->n = 0;           /* no other cars' headlights until headlights.c sets them */
    ((GwMarks *)(N->kb + GW_K_MARKS))->n = 0;     /* nor quick chat's places until roam_game.c does */
    g_frame_clear = g_clear_argb;
}
void metal_present(void){
    gw_init();
    g_presents++;
    if (g_on) gw_attached();          /* the first present waits for the page; a detach ends recording */
    if (!g_on){ g_gframe++; tex_free_due(0); g_handed = 0; return; }
    /* a readback handed the frame over already: only what was drawn since */
    if (!g_handed || g_b[g_cur].dn) handoff(0, 0, 0, 0, 0);
    g_handed = 0;
    g_gframe++;
    tex_free_due(0);
    vsync_feed();
    view_follow();
}

/* LockRect on a render target the game made lockable (com.c, CreateRenderTarget):
 * the pixels the GPU drew there, level 0, rows x pitch bytes at dst.  The game
 * takes the picture of a DVD or magazine cover this way (0x5C3120: the middle
 * of the screen StretchRect'ed into a 512x512 target, locked, copied into the
 * cover's save and its texture), after EndScene and just before Present.
 * Records the page has not seen yet drew it, so the frame recorded so far goes
 * to the page now with the request: the page shows it as any frame (it is the
 * whole frame), copies the texture out and writes dst, and the game waits for
 * that with the guest lock released, at most 3 s (a hidden tab may not draw).
 * Present then hands off only what was drawn in between.  1: dst holds the
 * pixels; 0: no renderer, or the page did not answer (dst as it was). */
int metal_read_texture(unsigned id, void *dst, unsigned pitch, unsigned rows){
    gw_init();
    if (!g_on || !dst || !id || id >= MAXTEX || !g_tex[id].live || !g_tex[id].rt || g_tex[id].cube) return 0;
    if (!gw_attached()) return 0;
    uint32_t rid = ++g_rb_n & ~GW_RB_FLAG;
    if (!rid) rid = g_rb_n = 1;
    ast(&g_ctl->rb_want, (int32_t)rid);
    double t0 = now_ms();
    handoff(rid, id, (uint32_t)(uintptr_t)dst, pitch, rows);
    g_handed = 1;
    double lim = 3000.0;
    int got = -1;
    guest_wait_begin();
    for (;;){
        int32_t d = ald(&g_ctl->rb_done);
        if (((uint32_t)d & ~GW_RB_FLAG) == rid){ got = !((uint32_t)d & GW_RB_FLAG); break; }
        double left = lim - (now_ms() - t0);
        if (left <= 0 || ald(&g_ctl->attached) != GW_ATTACHED){
            /* no answer: cancel, unless the page is writing dst right now (it
             * claims and writes in one go: a moment) */
            int32_t w = (int32_t)rid;
            if (__atomic_compare_exchange_n(&g_ctl->rb_want, &w, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
            if (lim >= 5000.0) break;
            lim = 5000.0; continue;
        }
        wait32(&g_ctl->rb_done, d, left < 50.0 ? (left > 1.0 ? left : 1.0) : 50.0);
    }
    guest_wait_end();
    fprintf(stderr, "[gpu] texture %u (%ux%u) read back for the game: %s after %.0f ms\n", id, g_tex[id].w, g_tex[id].h,
            got > 0 ? "done" : got == 0 ? "the page could not read it" : "no answer", now_ms() - t0);
    return got > 0;
}

long metal_frames(void){ return g_on && g_ctl ? (long)g_ctl->frames_done : g_presents; }
long metal_frames_dropped(void){ return g_dropped; }
long metal_tris(void){ return g_tris; }
unsigned long long metal_wait_present_ns(void){ return (unsigned long long)(g_wait_present_ms * 1e6); }
int  metal_available(void){ gw_init(); return 1; }
int  metal_window_ready(void){ return g_on; }

void metal_budget_report(void){
    gw_init();
    if (!g_on && !g_detached){ fprintf(stderr, "  [gpu] no renderer: %ld presents, %ld textures\n", g_presents, g_tex_created); return; }
    GwCtl *c = g_ctl;
    double fr = c->frames_done ? (double)c->frames_done : 1.0;
    fprintf(stderr, "  [gpu] WebGPU: %ld presents, %u frames encoded, %ld dropped, %ld replaced (mailbox)\n",
            g_presents, c->frames_done, g_dropped, g_replaced);
    fprintf(stderr, "  [gpu] encode %.3f ms/frame, %.0f draws/frame, %.1f passes/frame, %u pipelines, %u WebGPU errors, %u dumps\n",
            c->st_enc_us / 1000.0 / fr, c->st_draws / fr, c->st_passes / fr, c->st_pipelines, c->st_errors, c->dumps_done);
    fprintf(stderr, "  [gpu] textures %ld created, %ld uploads, %lu released; resource queue %.1f MB, waited %.0f ms; present waits %.3f ms/frame\n",
            g_tex_created, g_tex_uploaded, g_tex_released, g_rq_bytes / 1048576.0, g_wait_rq_ms,
            g_presents ? g_wait_present_ms / (double)g_presents : 0.0);
    fprintf(stderr, "  [gpu] mirrors: %lu built (%.1f MB), %lu live, %lu dropped, %lu table-full, pool %.1f MB\n",
            g_mir_build, g_mir_bytes / 1048576.0, g_mir_live, g_mir_drop, g_mir_full, g_pool_cap / 1048576.0);
    fprintf(stderr, "  [gpu] shaders: %u VS + %u PS WGSL, %u VS + %u PS without WGSL (CPU vertex path / built-in fragment); "
                    "overflows draw %u tvert %u raw %u const %u\n",
            g_vs_n, g_ps_n, g_msl_vs, g_msl_ps, g_ov_d, g_ov_v, g_ov_rv, g_ov_kb);
}
