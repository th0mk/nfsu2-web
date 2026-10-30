/* The renderer contract between the C recorder (runtime/web/gpu_web.c, on the
 * game thread) and the WebGPU encoder (web/src/gpu, on the page).  Everything
 * here lives in the wasm memory; the page reads it zero-copy.  The page-side
 * mirror is web/src/gpu/proto.ts (same offsets; web/tools/check-gpu-proto.mjs
 * compares the two, and the page refuses a block whose layout word differs).
 *
 * Only fixed-width fields: wasm32 and the TypeScript views must agree byte for
 * byte, and `long` is 32 bits on wasm32.
 *
 *   GwCtl     one per process, registered as HOST_KIND_GPU
 *   GwFrame   per record buffer (2): what the frame's records are and where
 *   GwDraw    one per draw / clear / StretchRect, in submission order
 *   GwCmd     resource queue record (texture create/upload/free, mirror pool,
 *             shaders), ordered against frames by GwFrame.rq_end
 *   GwVsync   the page's display clock, registered as HOST_KIND_VSYNC
 */
#ifndef NFSU2_GPU_PROTO_H
#define NFSU2_GPU_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define GW_MAGIC   0x55504757u          /* "WGPU" */
#define GW_VERSION 6u                   /* 2: the headlights block in the K arena (GwHL); 3: K blocks 16-byte aligned, bound in windows;
                                           4: GwDraw.clip; 5: GW_BLIT's source rectangle, a texture read back (GwFrame.rb_*);
                                           6: SM3 and depth textures (gpu-abi.md section 11): GwCtl.stages, GwDraw.tex4-7 and
                                           smphi, GW_Z_TEX, GW_PS_DEPTH_SHIFT, GW_TF_DEPTH, the PS kinds word (GW_BK_*) */
#define GW_NBUF    2                    /* record buffers: one being written, one handed off */

/* GwDraw.kind */
enum { GW_DRAW = 0, GW_CLEAR = 1, GW_BLIT = 2 };

/* One record.  Fields as in metal.m's MDraw, minus the HD-only ones (c0, gh). */
typedef struct {
    uint32_t first;       /* TVert path: first vertex in the TVert arena; GW_BLIT: source rectangle left | top << 16 */
    uint32_t count;       /* vertices (TVert/raw) or indices (indexed); a multiple of 3;
                           * GW_BLIT: source rectangle right | bottom << 16 (pixels), 0 = the whole source */
    uint32_t tex;         /* stage-0 texture id (0 = white) */
    uint32_t target;      /* render target: texid | (cube face + 1) << 16; 0 = back buffer */
    uint32_t kind;        /* GW_DRAW, GW_CLEAR, GW_BLIT */
    uint32_t blend;       /* bit0 enable, 1-5 SRCBLEND, 6-10 DESTBLEND, 11-13 BLENDOP, 14-17 write mask */
    float    aref;        /* alpha test reference (0 = no test) */
    uint32_t depth;       /* bit0 Z test, bit1 Z write, bits 2-4 D3DCMPFUNC, bits 8+ Z surface id: 0 none, 1 the
                           * implicit one, 2-63 created surfaces, GW_Z_TEX + texid a depth texture (GW_TF_DEPTH) */
    uint32_t tex1, tex2, tex3;
    int32_t  ps;          /* pixel shader module id: bits 0-7 the game's shader (0 = fixed function), +256 = stage-0
                           * cube variant, bits GW_PS_DEPTH_SHIFT+ the stages that sample a depth texture (one bit each) */
    uint32_t psci;        /* index of the PS constant block in the frame's pool */
    int32_t  vsid;        /* GPU vertex shader id (0 = TVert path) */
    uint32_t rawoff;      /* raw arena byte offset (raw draw) or pool offset of vertex 0 (indexed) */
    uint32_t koff;        /* VS constant block offset in the K arena (16-byte aligned) */
    float    proj[4];     /* per stage: TEXCOORDINDEX + 16 * projection mode */
    uint32_t smp[4];      /* packed sampler keys (com.c smp_push) */
    uint32_t blit_src;    /* GW_BLIT: source texture id (0 = back buffer) */
    uint32_t bcol;        /* D3DRS_BLENDFACTOR (ARGB) */
    uint32_t cull;        /* D3DCULL: 1 none, 2 CW, 3 CCW */
    float    fogv[4];     /* fog colour rgb + enable */
    uint32_t clr_argb;    /* GW_CLEAR: colour */
    uint32_t clr_flags;   /* GW_CLEAR: D3DCLEAR_* (1 target, 2 Z, 4 stencil) */
    float    clr_z;       /* GW_CLEAR: Z value */
    uint32_t ksz;         /* VS constant block size in bytes (16 + 16 * nk) */
    uint32_t ix_isz;      /* indexed draw: index size 2 or 4; 0 = not indexed */
    uint32_t ix_ib;       /* indexed draw: pool offset of the first index */
    int32_t  ix_base;     /* indexed draw: BaseVertexIndex */
    uint32_t clip;        /* the 2D UI on a screen narrower than 4:3: rows y0 | y1 << 16 of the back buffer
                           * (its 4:3 band), the draw kept between them; 0 = the whole target (com.c) */
    uint32_t tex4, tex5, tex6, tex7;  /* stage 4-7 texture ids (0 = white); read only with GwCtl.stages 8 */
    uint32_t smphi[4];    /* packed sampler keys of stages 4-7 (as smp); read only with GwCtl.stages 8 */
} GwDraw;

/* GwDraw.depth bits 8+ from this value on: a depth texture's id + GW_Z_TEX,
 * that texture as the draw's Z (created surfaces stay below it, com.c zid_take) */
#define GW_Z_TEX 64u
/* GwDraw.depth bits from this one up (8 stages only; 0 in UG2's records): the draw's depth bias, bits 24-27 the
 * constant part in units of 2^-24 (D3DRS_DEPTHBIAS x 2^24, 0..15), bits 28-31 the slope scale
 * (D3DRS_SLOPESCALEDEPTHBIAS rounded, 0..15); the Z surface id stays in bits 8-23 (gpu-abi.md section 11.4).
 * The page's depth32float Z applies a unit of 2^(e-23), e the exponent of the primitive's largest z: 2^-24 for
 * z in [0.5, 1), smaller nearer the camera (gpu-abi.md says what else is approximate) */
#define GW_ZBIAS_SHIFT 24u
/* GwDraw.ps bits from this one up: stage k's texture is a depth texture
 * (GW_TF_DEPTH) and module ps & 0x1FF samples it with a depth compare; the
 * game registers that variant under the whole id (gpu-abi.md section 11) */
#define GW_PS_DEPTH_SHIFT 9u
/* texture stages a game's modules and draws use (GwCtl.stages) */
#define GW_STAGES_MAX 8u

typedef struct {
    uint32_t seq;         /* guest frame number at handoff */
    uint32_t dn;          /* records */
    uint32_t vn;          /* TVerts (112 bytes each, gfx.h) */
    uint32_t rvn;         /* raw vertex arena bytes */
    uint32_t kn;          /* VS constant arena bytes */
    uint32_t pscn;        /* PS constant blocks (32 x vec4 = 512 bytes each) */
    uint32_t draws, tv, raw, k, psc;   /* wasm addresses of this buffer's arenas */
    uint32_t rq_end;      /* resource queue position at handoff: apply the queue up to here first */
    uint32_t bb_w, bb_h;  /* back buffer (device) size */
    uint32_t clear_argb;  /* clear colour in effect when the frame began */
    uint32_t pool_size;   /* mirror pool size at handoff */
    uint32_t dump;        /* nonzero: read this frame back (autotest "dump") */
    int32_t  dump_frame;  /* guest frame number for the file name */
    char     dump_label[64];
    /* rb_id nonzero: after this frame, texture rb_tex's level 0 goes to the
     * game, rb_rows rows of rb_pitch bytes at wasm address rb_dst (BGRA, as
     * D3DFMT_A8R8G8B8 is in memory).  The game waits in LockRect for it
     * (GwCtl.rb_want / rb_done); gpu_web.c metal_read_texture. */
    uint32_t rb_id, rb_tex, rb_dst, rb_pitch, rb_rows;
} GwFrame;

/* GwCtl.state: the handoff word.  bits 0-1 ready buffer + 1 (0 = none),
 * bits 2-3 buffer the page is encoding + 1 (0 = none).  The game publishes
 * ready (or, in mailbox mode, replaces a ready frame the page has not taken);
 * the page takes it by CAS {r,0} -> {r,r}, encodes, then stores 0. */
/* GwCtl.attached: 0 until a renderer attaches (the first present waits for
 * it), GW_ATTACHED while one drains the block, GW_DETACHED once it detached
 * or once the game stopped waiting for one (NFSU2_RENDERER_WAIT_MS, 30 s; the
 * game's CAS 0 -> GW_DETACHED).  A detached block never takes a renderer again
 * (a new one would lack every texture and shader queued before it): the game
 * drops back to the null renderer at once, and the page attaches only with a
 * CAS from 0, so a renderer that comes too late is refused, not shown black. */
#define GW_ATTACHED 1
#define GW_DETACHED 2
#define GW_ST_READY(s) ((s) & 3)
#define GW_ST_ENC(s)   (((s) >> 2) & 3)

typedef struct {
    uint32_t magic, version, layout, size;
    int32_t  state;       /* handoff word (above) */
    int32_t  bell;        /* doorbell: bumped and notified on every publish and queue flush */
    int32_t  attached;    /* page: GW_ATTACHED while a renderer drains this block, GW_DETACHED after it let go */
    int32_t  rq_head;     /* resource queue: bytes written (game), wraps mod 2^32 */
    int32_t  rq_tail;     /* resource queue: bytes consumed (page); notified */
    uint32_t rq_ptr;      /* ring address */
    uint32_t rq_cap;      /* ring size, a power of two */
    uint32_t frames_done; /* page: frames encoded and submitted */
    uint32_t view_w, view_h;   /* page: canvas size in device pixels */
    uint32_t dpr1000;     /* page: devicePixelRatio x 1000 */
    uint32_t features;    /* page: GW_FEAT_* */
    uint32_t st_draws;    /* page: draws encoded (total) */
    uint32_t st_passes;   /* page: render passes (total) */
    uint32_t st_enc_us;   /* page: encode time (total, microseconds) */
    uint32_t st_errors;   /* page: WebGPU validation errors seen */
    uint32_t dumps_done;  /* page: frame dumps written */
    uint32_t st_pipelines;/* page: pipelines created */
    /* a texture read back (GwFrame.rb_*): rb_want is the request the game
     * waits for; the page claims it with a CAS to rb_id | GW_RB_FLAG before it
     * writes rb_dst (the game cancels with a CAS to 0 when it stops waiting:
     * the lock buffer is not the page's to write then), then stores rb_done =
     * rb_id once the bytes are there, rb_id | GW_RB_FLAG when it could not
     * read the texture, and notifies */
    int32_t  rb_want, rb_done;
    /* game, set before it registers the block and never changed: the texture
     * stages of every draw and module (tex0-7, smp0-7, VOut uv0-uv7), 4 or
     * GW_STAGES_MAX; 0 = 4 (gpu-abi.md section 11) */
    uint32_t stages;
    GwFrame  frame[GW_NBUF];
} GwCtl;

#define GW_RB_FLAG 0x80000000u

#define GW_FEAT_BC       1u   /* texture-compression-bc */
#define GW_FEAT_SWIZZLE  2u   /* texture-component-swizzle */

/* The page's display clock, filled from requestAnimationFrame. */
typedef struct {
    int32_t  tick;        /* bumped (and notified) at every rAF */
    int32_t  seq;         /* seqlock: odd while the page writes ts/period */
    double   ts_ms;       /* the latest rAF time, ms on the shared clock (performance.timeOrigin + rAF timestamp) */
    double   period_ms;   /* median rAF interval */
    int32_t  hz;          /* refresh rate from the median interval (0 = not measured yet) */
    int32_t  visible;     /* document visible */
} GwVsync;

/* Resource queue records: a 32-byte header, then the payload.  Records are
 * 16-byte aligned and never wrap (a GW_PAD record fills the end of the ring). */
enum {
    GW_PAD         = 0,
    GW_TEX_CREATE  = 1,   /* a: id, w, h, levels, d3dfmt, flags (GW_TF_*) */
    GW_TEX_UPLOAD  = 2,   /* a: id, level | face << 16, level w, level h, pitch, rows; payload pitch*rows */
    GW_TEX_FREE    = 3,   /* a: id */
    GW_POOL_GROW   = 4,   /* a: new size */
    GW_POOL_WRITE  = 5,   /* a: offset, bytes; payload */
    GW_SHADER_VS   = 6,   /* a: id, text bytes; payload WGSL (NUL-terminated) */
    GW_SHADER_PS   = 7,   /* a: id, text bytes, binding kinds (GW_BK_*); payload WGSL (NUL-terminated) */
    GW_Z_FORGET    = 8,   /* a: zid */
};
#define GW_TF_RT    1u
#define GW_TF_CUBE  2u
#define GW_TF_DEPTH 4u    /* a depth texture (D3DUSAGE_DEPTHSTENCIL: D3DFMT_D16, D24X8 or D24S8): a draw's Z through
                           * GW_Z_TEX, sampled with a depth compare; no uploads, no read back */
/* GW_SHADER_PS a[2]: how the module declares the texture of each stage k,
 * 2 bits at 2k; 0 everywhere = every stage 2D (a +256 module's stage 0 is a
 * cube either way) */
#define GW_BK_2D    0u    /* texture_2d<f32> + sampler */
#define GW_BK_CUBE  1u    /* texture_cube<f32> + sampler */
#define GW_BK_DEPTH 2u    /* texture_depth_2d + sampler_comparison (LESSEQUAL) */
#define GW_BK(kinds, k) (((kinds) >> (2u * (k))) & 3u)
typedef struct { uint32_t op, len; uint32_t a[6]; } GwCmd;
#define GW_CMD_HDR 32u

/* The other cars' headlights (runtime/headlights.c): every frame's VS
 * constant arena holds this block at GW_K_HL, bound as group 0 binding 3 (H)
 * for the lit pixel shaders, which add car k's light at q * t (q: their
 * coordinate in the player's light, uv1; t: columns) with the falloff
 * coordinate s * q'.w + o (gpu-abi.md section 2).  n = 0: none this frame. */
#define GW_HL_MAX   8
typedef struct { float t[16]; float col[4]; } GwHLight;
typedef struct { uint32_t n; float s, o, pad; GwHLight l[GW_HL_MAX]; } GwHL;
#define GW_K_HL     256u
#define GW_HL_BYTES 656u

/* Quick chat's places over the cars (roam_game.c say_marks): every frame's VS
 * constant arena holds this block at GW_K_MARKS, which the page reads with
 * the frame it encodes, so a line said moves with its car in that very
 * picture.  magic: GW_MARKS_MAGIC (where a module without the block has the
 * draws' constants); n marks: the player's id, where the name over their car
 * is (x, y: parts of the back buffer from its top left corner, 0 .. 1) and
 * its scale (the game's, 0.5 .. 1 with the distance).  n = 0: none this
 * frame.  The page binds none of it. */
#define GW_MARKS_MAX   32
#define GW_MARKS_MAGIC 0x4B52414Du        /* "MARK" */
typedef struct { uint32_t id; float x, y, s; } GwMark;
typedef struct { uint32_t magic, n, pad[2]; GwMark m[GW_MARKS_MAX]; } GwMarks;
#define GW_K_MARKS     1024u
#define GW_MARKS_BYTES 528u

/* Exposure and White Level (tonemenu.c): every frame's VS constant arena
 * holds this block at GW_K_TONE, which the page reads with the frame it
 * encodes.  Its last pass takes each colour channel c (0 .. 1, as drawn) to
 * y = min(1, c * mult), and a y above knee * white bends down so that 1
 * lands on white (web/src/gpu/wgsl.ts TONE).  mult 1 and white 1: the
 * picture as drawn, without that pass.  magic: GW_TONE_MAGIC (where a module
 * without the block has zeros).  The page binds none of it. */
#define GW_TONE_MAGIC  0x454E4F54u        /* "TONE" */
typedef struct { uint32_t magic; float mult, white, knee; } GwTone;
#define GW_K_TONE      1552u
#define GW_TONE_BYTES  16u

/* Input age (framerate.c): every frame's VS constant arena holds this block
 * at GW_K_LAT: when the game read the input this frame simulates with, and
 * when it handed the frame off, in ms of the shared clock
 * (performance.timeOrigin + performance.now()).  The page logs how old that
 * input is when the frame is submitted, done on the GPU and next shown.
 * magic: GW_LAT_MAGIC (where a module without the block has zeros).  The page
 * binds none of it. */
#define GW_LAT_MAGIC   0x4E54414Cu        /* "LATN" */
typedef struct { uint32_t magic, pad; double input_ms, handoff_ms; } GwLat;
#define GW_K_LAT       1568u
#define GW_LAT_BYTES   24u

/* The page binds the VS constant arena (K) in windows of GW_KWIN_ROWS rows of
 * 16 bytes: 64 KB, the default maxUniformBufferBindingSize.  A draw's block
 * (16-byte header + c[0..255]) starts at row firstInstance of its window, so
 * consecutive draws share one bind group (gpu-abi.md section 3). */
#define GW_KWIN_ROWS 4096u

/* Layout word: changes whenever a struct above changes shape. */
#define GW_LAYOUT ((uint32_t)(sizeof(GwDraw) | sizeof(GwFrame) << 8 | (sizeof(GwCtl) & 0xFFF) << 16) ^ (GW_VERSION << 28))

_Static_assert(sizeof(GwDraw) == 188, "GwDraw");
_Static_assert(offsetof(GwDraw, kind) == 16 && offsetof(GwDraw, aref) == 24 && offsetof(GwDraw, depth) == 28, "GwDraw a");
_Static_assert(offsetof(GwDraw, ps) == 44 && offsetof(GwDraw, psci) == 48 && offsetof(GwDraw, vsid) == 52, "GwDraw b");
_Static_assert(offsetof(GwDraw, rawoff) == 56 && offsetof(GwDraw, koff) == 60 && offsetof(GwDraw, proj) == 64, "GwDraw c");
_Static_assert(offsetof(GwDraw, smp) == 80 && offsetof(GwDraw, blit_src) == 96 && offsetof(GwDraw, bcol) == 100, "GwDraw d");
_Static_assert(offsetof(GwDraw, cull) == 104 && offsetof(GwDraw, fogv) == 108 && offsetof(GwDraw, clr_argb) == 124, "GwDraw e");
_Static_assert(offsetof(GwDraw, clr_flags) == 128 && offsetof(GwDraw, clr_z) == 132 && offsetof(GwDraw, ksz) == 136, "GwDraw f");
_Static_assert(offsetof(GwDraw, ix_isz) == 140 && offsetof(GwDraw, ix_ib) == 144 && offsetof(GwDraw, ix_base) == 148, "GwDraw g");
_Static_assert(offsetof(GwDraw, clip) == 152 && offsetof(GwDraw, tex4) == 156 && offsetof(GwDraw, tex7) == 168, "GwDraw h");
_Static_assert(offsetof(GwDraw, smphi) == 172, "GwDraw i");
_Static_assert(sizeof(GwFrame) == 156 && offsetof(GwFrame, rq_end) == 44 && offsetof(GwFrame, dump_label) == 72, "GwFrame");
_Static_assert(offsetof(GwFrame, rb_id) == 136 && offsetof(GwFrame, rb_rows) == 152, "GwFrame rb");
_Static_assert(offsetof(GwCtl, state) == 16 && offsetof(GwCtl, bell) == 20 && offsetof(GwCtl, attached) == 24, "GwCtl a");
_Static_assert(offsetof(GwCtl, rq_head) == 28 && offsetof(GwCtl, rq_tail) == 32 && offsetof(GwCtl, rq_ptr) == 36, "GwCtl b");
_Static_assert(offsetof(GwCtl, frames_done) == 44 && offsetof(GwCtl, view_w) == 48 && offsetof(GwCtl, features) == 60, "GwCtl c");
_Static_assert(offsetof(GwCtl, st_draws) == 64 && offsetof(GwCtl, st_pipelines) == 84 && offsetof(GwCtl, frame) == 100, "GwCtl d");
_Static_assert(offsetof(GwCtl, rb_want) == 88 && offsetof(GwCtl, rb_done) == 92 && offsetof(GwCtl, stages) == 96, "GwCtl e");
_Static_assert(sizeof(GwCtl) == 100 + 2 * 156, "GwCtl");
_Static_assert(sizeof(GwVsync) == 32 && offsetof(GwVsync, ts_ms) == 8 && offsetof(GwVsync, hz) == 24, "GwVsync");
_Static_assert(sizeof(GwCmd) == GW_CMD_HDR, "GwCmd");
_Static_assert(sizeof(GwHLight) == 80 && sizeof(GwHL) == GW_HL_BYTES && offsetof(GwHL, l) == 16, "GwHL");
_Static_assert(sizeof(GwMark) == 16 && sizeof(GwMarks) == GW_MARKS_BYTES && offsetof(GwMarks, m) == 16, "GwMarks");
_Static_assert(GW_K_MARKS >= GW_K_HL + GW_HL_BYTES, "GwMarks after GwHL");
_Static_assert(sizeof(GwTone) == GW_TONE_BYTES && GW_K_TONE >= GW_K_MARKS + GW_MARKS_BYTES, "GwTone after GwMarks");
_Static_assert(sizeof(GwDraw) < 256, "GW_LAYOUT keeps sizeof(GwDraw) in 8 bits");
_Static_assert(GW_PS_DEPTH_SHIFT + GW_STAGES_MAX <= 31, "GwDraw.ps depth bits");
_Static_assert(sizeof(GwLat) == GW_LAT_BYTES && offsetof(GwLat, input_ms) == 8 && GW_K_LAT % 8 == 0
               && GW_K_LAT >= GW_K_TONE + GW_TONE_BYTES, "GwLat after GwTone");
#endif
