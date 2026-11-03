/* gpu_native.c -- the Switch's "page" for the renderer: drains the record buffers runtime/web/gpu_web.c writes (the
 * contract of runtime/web/gpu_proto.h, the one web/src/gpu/index.ts drains in the browser) and, for now, draws a
 * SLIDESHOW: one frame every SHOT_MS is rasterized on the CPU (the 2D UI: screen-space triangles with a texture, the
 * vertex colour and the game's blending) and handed to nx_main.c to show.  Frames in between are taken and dropped
 * (their uploads still applied), so the game is held up only by the frame being drawn.
 *
 * Not drawn yet: draws with a vertex shader (the 3D scene), a pixel shader, a render target, blits, and the second
 * to fourth texture stages.  The GPU renderer (GLES) replaces this file's rasterizer; the queue and texture handling
 * stay.  Compiled like the runtime (no libnx headers): nx_main.c reads the picture through gpu_native_shot(). */
#include "cpu.h"
#include "gfx.h"
#include "web/gpu_proto.h"
#include "gpu_gl.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *gw_native_ctl(void);
void *gw_native_ring(void);
int   gw_native_on(void);
int   gw_native_buf(int b, void **draws, void **tv, void **raw, void **k, void **psc);
void *nfsu2_host_mailbox(int kind, uint32_t *size);

#define SHOT_MS 50.0                   /* the least between two pictures handed to the rasterizer thread */

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static inline int32_t ald(int32_t *p){ return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static inline void ast(int32_t *p, int32_t v){ __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static inline void wake(int32_t *p){ __builtin_wasm_memory_atomic_notify((int *)p, 0xFFFFFFFFu); }
static inline void waitv(int32_t *p, int32_t v, double ms){ __builtin_wasm_memory_atomic_wait32((int *)p, v, (int64_t)(ms * 1e6)); }

/* ---- textures: level 0 as ARGB (A << 24 | R << 16 | G << 8 | B, a D3DFMT_A8R8G8B8 pixel read as a u32) ---- */
#define MAXTEX 16384
typedef struct { uint32_t w, h, fmt; uint8_t rt, cube, a8; uint32_t *px; } Tex;
static Tex g_tx[MAXTEX];
static pthread_mutex_t g_tx_lock = PTHREAD_MUTEX_INITIALIZER;   /* the queue (consumer thread) and the rasterizer thread share the textures */

static uint32_t rgb565(uint16_t c){
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
/* one 4x4 DXT block (kind 1, 3 or 5) into 16 pixels */
static void dxt_block(const uint8_t *b, int kind, uint32_t out[16]){
    const uint8_t *c = kind == 1 ? b : b + 8;
    uint16_t c0 = (uint16_t)(c[0] | c[1] << 8), c1 = (uint16_t)(c[2] | c[3] << 8);
    uint32_t col[4], p0 = rgb565(c0), p1 = rgb565(c1);
    col[0] = p0; col[1] = p1;
    #define CH(p, s) (((p) >> (s)) & 255u)
    #define MIX(a, b, wa, wb, d) ((((wa) * CH(a, 16) + (wb) * CH(b, 16)) / (d)) << 16 | (((wa) * CH(a, 8) + (wb) * CH(b, 8)) / (d)) << 8 | (((wa) * CH(a, 0) + (wb) * CH(b, 0)) / (d)))
    if (kind != 1 || c0 > c1){ col[2] = 0xFF000000u | MIX(p0, p1, 2u, 1u, 3u); col[3] = 0xFF000000u | MIX(p0, p1, 1u, 2u, 3u); }
    else { col[2] = 0xFF000000u | MIX(p0, p1, 1u, 1u, 2u); col[3] = 0; }          /* DXT1 with 1-bit alpha: index 3 clear */
    uint32_t idx = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
    for (int i = 0; i < 16; i++){
        uint32_t px = col[(idx >> (2 * i)) & 3];
        if (kind == 3){ uint32_t a = (b[i >> 1] >> ((i & 1) * 4)) & 15u; px = (px & 0x00FFFFFFu) | ((a * 17u) << 24); }
        else if (kind == 5){
            uint32_t a0 = b[0], a1 = b[1], al[8], bits = 0;
            al[0] = a0; al[1] = a1;
            if (a0 > a1) for (uint32_t k = 1; k < 7; k++) al[1 + k] = ((7 - k) * a0 + k * a1) / 7;
            else { for (uint32_t k = 1; k < 5; k++) al[1 + k] = ((5 - k) * a0 + k * a1) / 5; al[6] = 0; al[7] = 255; }
            uint64_t v = 0; for (int k = 0; k < 6; k++) v |= (uint64_t)b[2 + k] << (8 * k);
            bits = (uint32_t)((v >> (3 * i)) & 7u);
            px = (px & 0x00FFFFFFu) | (al[bits] << 24);
        }
        out[i] = px;
    }
}
static void tex_upload0(Tex *t, const uint8_t *src, uint32_t pitch, uint32_t rows){
    uint32_t w = t->w, h = t->h, f = t->fmt;
    if (!t->px) t->px = malloc((size_t)w * h * 4);
    if (!t->px) return;
    uint32_t *d = t->px;
    if (f == 21 || f == 22){
        for (uint32_t y = 0; y < h && y < rows; y++){
            const uint32_t *s = (const uint32_t *)(src + (size_t)y * pitch);
            for (uint32_t x = 0; x < w; x++) d[y * w + x] = f == 22 ? s[x] | 0xFF000000u : s[x];
        }
    } else if (f == 28){                 /* A8: white with the alpha, so the diffuse colour tints it */
        for (uint32_t y = 0; y < h && y < rows; y++) for (uint32_t x = 0; x < w; x++){ uint32_t a = src[(size_t)y * pitch + x]; d[y * w + x] = a << 24 | 0x00FFFFFFu; }
    } else if (f == 41){                 /* "L8" (P8 on the Mac): grey */
        for (uint32_t y = 0; y < h && y < rows; y++) for (uint32_t x = 0; x < w; x++){ uint32_t v = src[(size_t)y * pitch + x]; d[y * w + x] = 0xFF000000u | v << 16 | v << 8 | v; }
    } else if (f == 0x31545844u || f == 0x33545844u || f == 0x35545844u){
        int kind = f == 0x31545844u ? 1 : f == 0x33545844u ? 3 : 5; uint32_t bs = kind == 1 ? 8 : 16, px[16];
        for (uint32_t by = 0; by < (h + 3) / 4 && by < rows; by++) for (uint32_t bx = 0; bx < (w + 3) / 4; bx++){
            dxt_block(src + (size_t)by * pitch + bx * bs, kind, px);
            for (uint32_t i = 0; i < 16; i++){ uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2); if (x < w && y < h) d[y * w + x] = px[i]; }
        }
    }
}

/* ---- resource queue ---- */
static uint8_t *g_ring; static uint32_t g_cap;
static unsigned long g_ops[10], g_uploaded;
static int g_gl;                       /* NFSU2_NX_GL=1: the OpenGL renderer (gpu_gl.c) instead of the software one */
void *nfsu2_native_window(void);
static void drain(GwCtl *C, uint32_t limit){
    uint32_t tail = (uint32_t)ald(&C->rq_tail);
    while ((int32_t)(limit - tail) > 0){
        GwCmd *c = (GwCmd *)(g_ring + (tail & (g_cap - 1u)));
        if (c->len < GW_CMD_HDR) break;
        if (c->op < 10) g_ops[c->op]++;
        if ((c->op == GW_SHADER_VS || c->op == GW_SHADER_PS) && getenv("NFSU2_NX_SHADER_DUMP")){
            char path[600]; snprintf(path, sizeof path, "%s/%s_%u.wgsl", getenv("NFSU2_NX_SHADER_DUMP"), c->op == GW_SHADER_VS ? "vs" : "ps", c->a[0]);
            FILE *sf = fopen(path, "wb"); if (sf){ fwrite((const char *)c + GW_CMD_HDR, 1, c->a[1], sf); fclose(sf); }
        }
        if (g_gl){
            const uint8_t *pl = (const uint8_t *)c + GW_CMD_HDR;
            switch (c->op){
            case GW_TEX_CREATE: glr_tex_create(c->a[0], c->a[1], c->a[2], c->a[3], c->a[4], c->a[5]); break;
            case GW_TEX_UPLOAD: glr_tex_upload(c->a[0], c->a[1] & 0xFFFFu, c->a[1] >> 16, c->a[2], c->a[3], c->a[4], c->a[5], pl); break;
            case GW_TEX_FREE: glr_tex_free(c->a[0]); break;
            case GW_POOL_GROW: glr_pool_grow(c->a[0]); break;
            case GW_POOL_WRITE: glr_pool_write(c->a[0], c->a[1], pl); break;
            case GW_SHADER_VS: case GW_SHADER_PS: glr_shader(c->op == GW_SHADER_PS, c->a[0], (const char *)pl, c->a[1] ? c->a[1] - 1 : 0); break;
            case GW_Z_FORGET: glr_z_forget(c->a[0]); break;
            default: break;
            }
            tail += c->len; ast(&C->rq_tail, (int32_t)tail);
            continue;
        }
        pthread_mutex_lock(&g_tx_lock);
        if (c->op == GW_TEX_CREATE){
            uint32_t id = c->a[0];
            if (id && id < MAXTEX){
                Tex *t = &g_tx[id]; free(t->px); memset(t, 0, sizeof *t);
                t->w = c->a[1]; t->h = c->a[2]; t->fmt = c->a[4]; t->rt = !!(c->a[5] & GW_TF_RT); t->cube = !!(c->a[5] & GW_TF_CUBE);
                if (t->w > 4096 || t->h > 4096 || t->cube) t->w = t->h = 0;      /* not drawn by this renderer */
            }
        } else if (c->op == GW_TEX_UPLOAD){
            uint32_t id = c->a[0];
            if (id && id < MAXTEX && g_tx[id].w && !g_tx[id].rt && c->a[1] == 0 && c->a[2] == g_tx[id].w){
                tex_upload0(&g_tx[id], (const uint8_t *)c + GW_CMD_HDR, c->a[4], c->a[5]); g_uploaded++; }
        } else if (c->op == GW_TEX_FREE){
            uint32_t id = c->a[0];
            if (id && id < MAXTEX){ free(g_tx[id].px); memset(&g_tx[id], 0, sizeof g_tx[id]); }
        }
        pthread_mutex_unlock(&g_tx_lock);
        tail += c->len;
        ast(&C->rq_tail, (int32_t)tail);
    }
    wake(&C->rq_tail);
}

/* ---- the rasterizer ---- */
static uint32_t *g_fb[2]; static float *g_zb; static uint32_t g_fbw, g_fbh; static unsigned g_scale = 2;   /* the picture is drawn at 1/g_scale of the game's size (NFSU2_NX_SCALE) */ static _Atomic unsigned g_seq;
static int g_draw3d; static unsigned long g_skip_3d, g_ps_ff, g_drawn, g_skip_vs, g_skip_rt, g_skip_other, g_tris_in;

static void blend_factor(unsigned code, const float s[4], const float d[4], float o[4]){
    float f[4] = { 1, 1, 1, 1 };
    switch (code){
    case 1: f[0] = f[1] = f[2] = f[3] = 0; break;
    case 3: memcpy(f, s, sizeof f); break;
    case 4: for (int i = 0; i < 4; i++) f[i] = 1 - s[i]; break;
    case 5: f[0] = f[1] = f[2] = f[3] = s[3]; break;
    case 6: f[0] = f[1] = f[2] = f[3] = 1 - s[3]; break;
    case 7: f[0] = f[1] = f[2] = f[3] = d[3]; break;
    case 8: f[0] = f[1] = f[2] = f[3] = 1 - d[3]; break;
    case 9: memcpy(f, d, sizeof f); break;
    case 10: for (int i = 0; i < 4; i++) f[i] = 1 - d[i]; break;
    default: break;                         /* 2 ONE and the rest */
    }
    memcpy(o, f, sizeof f);
}
static inline float clamp01(float v){ return v < 0 ? 0 : v > 1 ? 1 : v; }

static void raster_tri(uint32_t *fb, const TVert *a, const TVert *b, const TVert *c, const GwDraw *D, const Tex *tx){
    const TVert *v[3] = { a, b, c };
    float sx[3], sy[3], sz[3];
    for (int i = 0; i < 3; i++){
        float w = v[i]->w; if (!(w > 1e-6f)) return;
        float iw = 1.0f / w;
        sx[i] = (v[i]->x * iw * 0.5f + 0.5f) * (float)g_fbw;
        sy[i] = (0.5f - v[i]->y * iw * 0.5f) * (float)g_fbh;
        sz[i] = v[i]->z * iw;
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-6f) return;
    if (D->cull == 3u && area < 0.0f) return;           /* D3DCULL_CCW culls counter-clockwise (y down: area < 0) */
    if (D->cull == 2u && area > 0.0f) return;           /* D3DCULL_CW */
    unsigned ztest = D->depth & 1u, zwrite = (D->depth >> 1) & 1u, zfunc = (D->depth >> 2) & 7u; if (!zfunc) zfunc = 8;
    float minx = fminf(sx[0], fminf(sx[1], sx[2])), maxx = fmaxf(sx[0], fmaxf(sx[1], sx[2]));
    float miny = fminf(sy[0], fminf(sy[1], sy[2])), maxy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    int cy0 = 0, cy1 = (int)g_fbh;
    if (D->clip){ cy0 = (int)(D->clip & 0xFFFFu) / (int)g_scale; cy1 = (int)(D->clip >> 16) / (int)g_scale; }
    if (x0 < 0) x0 = 0; if (y0 < cy0) y0 = cy0; if (x1 > (int)g_fbw) x1 = (int)g_fbw; if (y1 > cy1) y1 = cy1;
    float inv = 1.0f / area;
    unsigned blend_on = D->blend & 1u, sf = (D->blend >> 1) & 31u, df = (D->blend >> 6) & 31u, wmask = (D->blend >> 14) & 15u;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++){
        float px = (float)x + 0.5f, py = (float)y + 0.5f;
        float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * inv;
        float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * inv;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
        if (z < 0.0f || z > 1.0f) continue;
        float *zp = &g_zb[(size_t)y * g_fbw + (size_t)x];
        if (ztest){
            int ok;
            switch (zfunc){ case 1: ok = 0; break; case 2: ok = z < *zp; break; case 3: ok = z == *zp; break; case 4: ok = z <= *zp; break;
                            case 5: ok = z > *zp; break; case 6: ok = z != *zp; break; case 7: ok = z >= *zp; break; default: ok = 1; }
            if (!ok) continue;
        }
        float col[4] = { w0 * a->r + w1 * b->r + w2 * c->r, w0 * a->g + w1 * b->g + w2 * c->g,
                         w0 * a->b + w1 * b->b + w2 * c->b, w0 * a->a + w1 * b->a + w2 * c->a };
        if (tx && tx->px){
            float u = w0 * a->t[0][0] + w1 * b->t[0][0] + w2 * c->t[0][0], t = w0 * a->t[0][1] + w1 * b->t[0][1] + w2 * c->t[0][1];
            int tu = (int)(u * (float)tx->w), tv = (int)(t * (float)tx->h);
            if (tu < 0) tu = 0; if (tu >= (int)tx->w) tu = (int)tx->w - 1; if (tv < 0) tv = 0; if (tv >= (int)tx->h) tv = (int)tx->h - 1;
            uint32_t p = tx->px[(size_t)tv * tx->w + (size_t)tu];
            col[0] *= (float)((p >> 16) & 255) / 255.0f; col[1] *= (float)((p >> 8) & 255) / 255.0f;
            col[2] *= (float)(p & 255) / 255.0f;         col[3] *= (float)(p >> 24) / 255.0f;
        }
        for (int i = 0; i < 4; i++) col[i] = clamp01(col[i]);
        if (D->aref > 0.0f && col[3] < D->aref) continue;
        uint32_t *dst = &fb[(size_t)y * g_fbw + (size_t)x];
        float out[4] = { col[0], col[1], col[2], col[3] };
        if (blend_on){
            uint32_t q = *dst;
            float d[4] = { (float)((q >> 16) & 255) / 255.0f, (float)((q >> 8) & 255) / 255.0f, (float)(q & 255) / 255.0f, (float)(q >> 24) / 255.0f };
            float fs[4], fd[4];
            blend_factor(sf, col, d, fs); blend_factor(df, col, d, fd);
            for (int i = 0; i < 3; i++) out[i] = clamp01(col[i] * fs[i] + d[i] * fd[i]);
            out[3] = clamp01(col[3] * fs[3] + d[3] * fd[3]);
        }
        uint32_t o = *dst;
        if (wmask & 1u) o = (o & 0xFF00FFFFu) | ((uint32_t)(out[0] * 255.0f + 0.5f) << 16);
        if (wmask & 2u) o = (o & 0xFFFF00FFu) | ((uint32_t)(out[1] * 255.0f + 0.5f) << 8);
        if (wmask & 4u) o = (o & 0xFFFFFF00u) | (uint32_t)(out[2] * 255.0f + 0.5f);
        if (wmask & 8u) o = (o & 0x00FFFFFFu) | ((uint32_t)(out[3] * 255.0f + 0.5f) << 24);
        *dst = o | 0xFF000000u;           /* the screen has no alpha */
        if (zwrite && ztest) *zp = z;
    }
}

/* A frame's records and vertices are copied (a few hundred draws, some thousand vertices) and drawn by a thread of
 * its own, so the game goes on at its own speed while a picture is drawn: the first version drew in the consumer and held
 * the game up for the 0.4 s a 1280x720 picture took on this CPU.  One job at a time; a frame that comes while one is
 * being drawn is not drawn at all (the slideshow). */
typedef struct { GwDraw *d; TVert *v; unsigned dn, vn, dcap, vcap, bbw, bbh, clear; } Job;
static Job g_job; static int g_job_ready, g_job_busy;
static pthread_mutex_t g_jm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_jc = PTHREAD_COND_INITIALIZER;

static void raster_job(const Job *j){
    unsigned w = j->bbw / g_scale, h = j->bbh / g_scale;
    if (!w || !h) return;
    if (w != g_fbw || h != g_fbh){
        free(g_fb[0]); free(g_fb[1]); free(g_zb); g_fbw = w; g_fbh = h;
        g_zb = malloc((size_t)w * h * sizeof(float));
        g_fb[0] = calloc((size_t)w * h, 4); g_fb[1] = calloc((size_t)w * h, 4);
        if (!g_fb[0] || !g_fb[1] || !g_zb){ g_fbw = g_fbh = 0; return; }
    }
    uint32_t *fb = g_fb[(atomic_load(&g_seq)) & 1u];
    size_t npx = (size_t)g_fbw * g_fbh;
    for (size_t i = 0; i < npx; i++){ fb[i] = 0xFF000000u | (j->clear & 0x00FFFFFFu); g_zb[i] = 1.0f; }
    for (uint32_t i = 0; i < j->dn; i++){
        const GwDraw *D = &j->d[i];
        if (D->kind == GW_CLEAR){
            if (!D->target && (D->clr_flags & 1u)) for (size_t k = 0; k < npx; k++) fb[k] = 0xFF000000u | (D->clr_argb & 0x00FFFFFFu);
            if (!D->target && (D->clr_flags & 2u)) for (size_t k = 0; k < npx; k++) g_zb[k] = D->clr_z;
            continue;
        }
        if (D->kind != GW_DRAW){ g_skip_other++; continue; }
        if (D->vsid) { g_skip_vs++; continue; }
        if (D->ps) g_ps_ff++;                 /* a pixel shader: drawn as texture * vertex colour, the UI's case */
        if (D->target) { g_skip_rt++; continue; }
        if ((D->depth & 1u) && !g_draw3d){ g_skip_3d++; continue; }   /* the 3D scene (Z tested): the GPU renderer's job */
        pthread_mutex_lock(&g_tx_lock);
        const Tex *tx = D->tex && D->tex < MAXTEX && g_tx[D->tex].px ? &g_tx[D->tex] : NULL;
        for (uint32_t t = 0; t + 2 < D->count && D->first + t + 2 < j->vn; t += 3){
            raster_tri(fb, &j->v[D->first + t], &j->v[D->first + t + 1], &j->v[D->first + t + 2], D, tx);
            g_tris_in++;
        }
        pthread_mutex_unlock(&g_tx_lock);
        g_drawn++;
    }
    atomic_fetch_add(&g_seq, 1);
}
static void *raster_main(void *unused){
    (void)unused;
    for (;;){
        pthread_mutex_lock(&g_jm);
        while (!g_job_ready) pthread_cond_wait(&g_jc, &g_jm);
        g_job_ready = 0; g_job_busy = 1;
        pthread_mutex_unlock(&g_jm);
        raster_job(&g_job);
        pthread_mutex_lock(&g_jm); g_job_busy = 0; pthread_mutex_unlock(&g_jm);
    }
    return NULL;
}
/* the consumer: copy the frame's records for the rasterizer thread, unless it is still drawing the last one */
static int submit_job(const GwFrame *F, int buf){
    pthread_mutex_lock(&g_jm); int busy = g_job_busy || g_job_ready; pthread_mutex_unlock(&g_jm);
    if (busy) return 0;
    void *dp, *tvp, *rawp, *kp, *pscp;
    if (!gw_native_buf(buf, &dp, &tvp, &rawp, &kp, &pscp)) return 0;
    Job *j = &g_job;
    if (F->dn > j->dcap){ free(j->d); j->d = malloc(sizeof(GwDraw) * (j->dcap = F->dn + 256)); }
    if (F->vn > j->vcap){ free(j->v); j->v = malloc(sizeof(TVert) * (j->vcap = F->vn + 1024)); }
    if (!j->d || !j->v){ j->dcap = j->vcap = 0; return 0; }
    memcpy(j->d, dp, sizeof(GwDraw) * F->dn); memcpy(j->v, tvp, sizeof(TVert) * F->vn);
    j->dn = F->dn; j->vn = F->vn; j->bbw = F->bb_w; j->bbh = F->bb_h; j->clear = F->clear_argb;
    pthread_mutex_lock(&g_jm); g_job_ready = 1; pthread_cond_signal(&g_jc); pthread_mutex_unlock(&g_jm);
    return 1;
}

/* what nx_main.c shows: the last finished picture (ARGB), 0 while there is none */
const uint32_t *gpu_native_shot(unsigned *w, unsigned *h, unsigned *seq){
    if (g_gl) return glr_shot(w, h, seq);
    unsigned s = atomic_load(&g_seq);
    *w = g_fbw; *h = g_fbh; *seq = s;
    return s ? g_fb[(s - 1) & 1u] : NULL;
}

/* ---- the consumer ---- */
static pthread_t g_th; static int g_started;
/* the GL renderer's monitor: if no frame has started for 2.5 s, say what it is doing (the console froze once with no report) */
static volatile int g_stop, g_stopped;      /* gpu_native_stop(): the consumer leaves its loop and the GL context */
static void *monitor(void *unused){
    (void)unused;
    unsigned long last = 0; double since = now_ms();
    for (;;){
        struct timespec ts = { 0, 500000000 }; nanosleep(&ts, NULL);
        if (g_stop) return NULL;
        unsigned long fcount = glr_frame_count();
        if (fcount != last){ last = fcount; since = now_ms(); }
        else if (now_ms() - since > 2500.0){
            char w[200]; glr_where(w, sizeof w);
            fprintf(stderr, "[gpu-nx] no frame for %.1f s: the renderer is %s\n", (now_ms() - since) / 1000.0, w);
            fflush(stderr);
            since += 2000.0;
        }
    }
    return NULL;
}
void nfsu2_pin(unsigned mask);
static void *consumer(void *unused){
    (void)unused;
    nfsu2_pin(0x2);                      /* the renderer: core 1 */
    GwCtl *C = NULL; uint32_t sz;
    while (!(C = nfsu2_host_mailbox(1 /* HOST_KIND_GPU */, &sz))) { struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
    GwVsync *V = nfsu2_host_mailbox(6 /* HOST_KIND_VSYNC */, &sz);
    g_ring = gw_native_ring(); g_cap = C->rq_cap;
    if (!gw_native_on() || !g_ring){ fprintf(stderr, "[gpu-nx] the recorder is off (NFSU2_RENDERER)\n"); return NULL; }
    { const char *e = getenv("NFSU2_NX_GL");
      if (e && *e == '1'){
          if (glr_init(nfsu2_native_window(), 1280, 720) == 0){ g_gl = 1; fprintf(stderr, "[gpu-nx] OpenGL renderer\n"); pthread_t mt; pthread_create(&mt, NULL, monitor, NULL); }
          else fprintf(stderr, "[gpu-nx] OpenGL renderer failed to start: software renderer\n");
      }
      const char *sh = getenv("NFSU2_NX_SHOTS"); if (g_gl && sh && atoi(sh) > 0) glr_want_shots((unsigned)atoi(sh)); }
    int32_t zero = 0;
    if (__atomic_compare_exchange_n(&C->attached, &zero, GW_ATTACHED, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)){
        C->view_w = 1280; C->view_h = 720; C->dpr1000 = 1000; C->features = 0;
        wake(&C->attached);
        fprintf(stderr, "[gpu-nx] attached to the recorder (ring %u MB): slideshow, a picture every %.2f s or so\n", g_cap >> 20, SHOT_MS / 1000.0);
    } else { fprintf(stderr, "[gpu-nx] could not attach (state %d)\n", ald(&C->attached)); return NULL; }
    { const char *e = getenv("NFSU2_NX_3D"); g_draw3d = e && *e == '1'; }
    double last_shot = -1e9, last_log = now_ms(), next_tick = now_ms(), gap = SHOT_MS;
    for (;;){
        if (g_stop) break;
        double now = now_ms();
        if (V && now >= next_tick){             /* the display clock: 60 Hz */
            V->seq++; V->ts_ms = now; V->period_ms = 1000.0 / 60.0; V->hz = 60; V->visible = 1; V->seq++;
            __atomic_fetch_add(&V->tick, 1, __ATOMIC_SEQ_CST); wake(&V->tick);
            next_tick += 1000.0 / 60.0; if (next_tick < now - 100.0) next_tick = now;
        }
        if (C->rb_want && C->rb_want != C->rb_done){      /* a texture read back: not available, say so */
            int32_t want = C->rb_want; ast(&C->rb_done, (int32_t)((uint32_t)want | GW_RB_FLAG)); wake(&C->rb_done); }
        uint32_t head = (uint32_t)ald(&C->rq_head);
        int32_t s = ald(&C->state); int r = s & 3, e = (s >> 2) & 3;
        if (r && !e){
            if (!__atomic_compare_exchange_n(&C->state, &s, s | (r << 2), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
            wake(&C->state);
            GwFrame *F = &C->frame[r - 1];
            drain(C, F->rq_end);
            if (g_gl){
                void *dp, *tvp, *rawp, *kp, *pscp;
                if (gw_native_buf(r - 1, &dp, &tvp, &rawp, &kp, &pscp)) glr_frame(F, dp, tvp, rawp, kp, pscp);
            } else if (now - last_shot >= SHOT_MS && F->dn && submit_job(F, r - 1)) last_shot = now;
            C->frames_done++;
            ast(&C->state, 0); wake(&C->state);
        } else {
            drain(C, head);
            waitv(&C->bell, ald(&C->bell), 4.0);
        }
        if (g_gl && now - last_log >= 5000.0){ char sb[640]; glr_stats(sb, sizeof sb); last_log = now; fprintf(stderr, "[gpu-nx] %s\n", sb); }
        else if (now - last_log >= 5000.0){
            last_log = now;
            fprintf(stderr, "[gpu-nx] frames %u, pictures %u, draws drawn %lu (tris %lu), ps-as-ff %lu, skipped: 3d %lu vs %lu rt %lu other %lu; uploads %lu\n",
                    C->frames_done, atomic_load(&g_seq), g_drawn, g_tris_in, g_ps_ff, g_skip_3d, g_skip_vs, g_skip_rt, g_skip_other, g_uploaded);
        }
    }
    if (g_gl) glr_shutdown();
    g_stopped = 1;
    return NULL;
}
void gpu_native_stop(void){
    g_stop = 1;
    for (int i = 0; i < 150 && !g_stopped; i++){ struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
}
void gpu_native_start(void){
    if (g_started) return;
    g_started = 1;
    { const char *e = getenv("NFSU2_NX_SCALE"); if (e && atoi(e) >= 1 && atoi(e) <= 16) g_scale = (unsigned)atoi(e); }
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1u << 20);
    pthread_create(&g_th, &at, consumer, NULL);
    pthread_t rt; pthread_create(&rt, &at, raster_main, NULL);
}
