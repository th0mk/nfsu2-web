/* D3D9 -> Metal bridge (stage 1: window + Clear/Present)
 *
 * On macOS the window must live on the MAIN thread.  The guest already runs
 * on a separate thread, so that fits: the main thread runs the NSApplication
 * loop, the guest thread queues draw commands.                           */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

/* getenv() SCANS the environment array and takes a lock on every call.  On
 * per-draw paths (e.g. the "PDLIST" check in SetVertexShaderConstantF was
 * the FIRST term of the && chain) that has a measurable cost: in the
 * profile getenv+__findenv_locked ate 3% of the guest thread.
 * Environment variables do not change during a run -> resolve once, keep. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

#import <Metal/Metal.h>
#include "enhance.h"
#include "perflog.h"
#include "fpscounter.h"
#include "texrep.h"
#include <stdatomic.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

static NSWindow        *g_win;
NSWindow *metal_nswindow(void){ return g_win; }          /* keyboard_mac.m */
#import <ImageIO/ImageIO.h>
#import <CoreServices/CoreServices.h>

static CAMetalLayer    *g_layer;
static long             g_skip_empty;   /* frame with no content: do not present */
static id<MTLTexture>   g_capture;   /* readable copy for frame capture */

/* --- frame dump ----------------------------------------------------------
 * DUMPFRAME / NFSU2_DUMP_FRAMES = comma list of items, host (presented)
 * frame numbers:  "100,300"  "200-400" (every frame)  "200-400/50"
 * (every 50th)  "%120" (every 120th frame, whole run).  Unlimited items.
 * DUMPDIR / NFSU2_DUMP_DIR = output dir (default /tmp).
 * NFSU2_DUMP_SCALE = 0.1..1 downscale factor (smaller PNGs).
 * The input script's "dump" command calls metal_request_dump(): the next
 * presented frame is written as nfsu2_g<guestframe>_<label>.png.        */
typedef struct { long lo, hi, step; } DumpSpec;
static DumpSpec g_dumps[256];
static int   g_dump_n;
static char  g_dump_dir[512];
static double g_dump_scale = 1.0;
static _Atomic long g_dump_req_frame = -1;     /* guest frame of pending request */
static char  g_dump_req_label[64];
static long  g_dump_cur_guest = -1;            /* set while writing a requested dump */

static void dump_init(void){
    const char *d = ENVC("NFSU2_DUMP_DIR"); if (!d || !*d) d = ENVC("DUMPDIR");
    snprintf(g_dump_dir, sizeof g_dump_dir, "%s", d && *d ? d : "/tmp");
    const char *sc = ENVC("NFSU2_DUMP_SCALE");
    if (sc && *sc){ g_dump_scale = atof(sc); if (g_dump_scale <= 0.05 || g_dump_scale > 1.0) g_dump_scale = 1.0; }
    const char *e = ENVC("NFSU2_DUMP_FRAMES"); if (!e || !*e) e = ENVC("DUMPFRAME");
    if (!e || !*e) return;
    for (const char *p = e; *p && g_dump_n < 256; ){
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        DumpSpec ds = {0, 0, 1};
        if (*p == '%'){ ds.lo = 1; ds.hi = 0x7fffffffL; ds.step = atol(p + 1); if (ds.step < 1) ds.step = 1; }
        else {
            char *q; ds.lo = strtol(p, &q, 10); ds.hi = ds.lo;
            if (*q == '-'){ ds.hi = strtol(q + 1, &q, 10); }
            if (*q == '/'){ ds.step = strtol(q + 1, &q, 10); if (ds.step < 1) ds.step = 1; }
        }
        g_dumps[g_dump_n++] = ds;
        while (*p && *p != ',') p++;
    }
}
static int dump_wanted(long f){
    for (int i = 0; i < g_dump_n; i++){
        DumpSpec *d = &g_dumps[i];
        if (f >= d->lo && f <= d->hi && ((f - d->lo) % d->step) == 0) return 1;
    }
    return 0;
}
/* guest thread (autotest.c): dump the next presented frame */
void metal_request_dump(long guest_frame, const char *label){
    snprintf(g_dump_req_label, sizeof g_dump_req_label, "%s", label ? label : "dump");
    for (char *q = g_dump_req_label; *q; q++) if (*q == '/' || *q == ' ') *q = '_';
    atomic_store(&g_dump_req_frame, guest_frame);
}
static void dump_texture(id<MTLTexture> t, long frame){
    NSUInteger w = t.width, h = t.height, stride = w * 4;
    void *buf = malloc(stride * h);
    if (!buf) return;
    [t getBytes:buf bytesPerRow:stride fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* Metal BGRA8Unorm -> CG: 32-bit little endian + premultiplied first */
    CGContextRef cg = CGBitmapContextCreate(buf, w, h, 8, stride, cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    if (cg){
        CGImageRef img = CGBitmapContextCreateImage(cg);
        if (g_dump_scale < 1.0){
            size_t w2 = (size_t)(w * g_dump_scale), h2 = (size_t)(h * g_dump_scale);
            CGContextRef c2 = CGBitmapContextCreate(NULL, w2, h2, 8, 0, cs,
                kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
            if (c2){
                CGContextSetInterpolationQuality(c2, kCGInterpolationHigh);
                CGContextDrawImage(c2, CGRectMake(0, 0, w2, h2), img);
                CGImageRef i2 = CGBitmapContextCreateImage(c2);
                if (i2){ CGImageRelease(img); img = i2; }
                CGContextRelease(c2);
            }
        }
        char path[640];
        if (g_dump_cur_guest >= 0)
            snprintf(path, sizeof path, "%s/nfsu2_g%06ld_%s.png", g_dump_dir, g_dump_cur_guest, g_dump_req_label);
        else
            snprintf(path, sizeof path, "%s/nfsu2_frame_%04ld.png", g_dump_dir, frame);
        CFStringRef sp = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, sp, kCFURLPOSIXPathStyle, false);
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (dst){ CGImageDestinationAddImage(dst, img, NULL);
                  CGImageDestinationFinalize(dst); CFRelease(dst); }
        CFRelease(url); CFRelease(sp); CGImageRelease(img); CGContextRelease(cg);
        fprintf(stderr, "metal: frame %ld written -> %s\n", frame, path);
    }
    CGColorSpaceRelease(cs); free(buf);
}
static id<MTLDevice>    g_dev;
static id<MTLCommandQueue> g_queue;

/* Frame state written by the guest thread, read by the main thread */
static _Atomic float    g_clear_r = 0.f, g_clear_g = 0.f, g_clear_b = 0.f;
static _Atomic int      g_frame_pending = 0;
/* Who waits for whom?  Frame time = guest CPU + the main thread's pickup
 * delay + vsync.  Without separating the three we cannot know which one to
 * optimize. */
static _Atomic unsigned long long g_wait_present_ns = 0;  /* guest: for the previous frame to be taken */
static _Atomic unsigned long long g_wait_drawable_ns = 0; /* main: nextDrawable (vsync) */
static _Atomic unsigned long long g_wait_frame_ns = 0;    /* main: for a frame to be ready */

static _Atomic long     g_frames_drawn  = 0;
static _Atomic long     g_tris_drawn    = 0;
static _Atomic long     g_tex_created   = 0, g_tex_uploaded = 0;
static _Atomic int      g_should_quit   = 0;
/* autotest.c: NFSU2_QUIT_FRAMES / script "quit" -> main loop exits cleanly */
void metal_request_quit(void){ atomic_store(&g_should_quit, 1); }

/* --- mouse state: the front end is navigated with the mouse (DIMOUSESTATE2)
 * DirectInput wants relative motion; we also keep the absolute position for
 * GetCursorPos. */
static _Atomic int g_mouse_x = 640, g_mouse_y = 360;
/* --- presentation geometry ---------------------------------------------
 * The drawable is in PIXELS, the window content in POINTS; on Retina the
 * two differ by a factor of 2.  Treating them as equal caused two separate
 * bugs: (1) the 1280x960 surface was scaled up to a 2560x1836-pixel area
 * and every pixel resampled, so the image went blurry, (2) mouse points
 * were taken for pixels, so the cursor drifted. */
static _Atomic int g_draw_w = 1280, g_draw_h = 720;   /* drawable - pixels */
static _Atomic int g_cont_w = 1280, g_cont_h = 720;   /* content  - points */
/* The resolution the game THINKS it has.  At Retina scale smaller than the
 * drawable (the game thinks 1280x960 while the surface is 2448x1836); the
 * cursor must be in this space. */
static _Atomic int g_game_w = 1280, g_game_h = 720;
/* --- full screen --------------------------------------------------------
 * In full screen the drawable goes up to the SCREEN's pixel size (and
 * aspect); the game's 4:3 request is corrected Hor+ in clip space
 * (res_pick/aspfix).  During the transition the size can only change with
 * the game's Reset, so com_request_reset() produces a device loss.
 * g_fs_pw/ph: full-screen content area (pixels), measured from AppKit on
 * entry. */
static _Atomic int g_fs = 0, g_fs_pw = 0, g_fs_ph = 0;
/* The (orthographic) UI is narrowed around the centre by aspfix via HUDFIX;
 * the mouse must map to that area too.  x10000 integer.  The HUD is narrowed on a
 * wider screen (x) and lowered on a narrower one (y), see widescreen.c. */
static _Atomic int g_ui_asp = 10000, g_ui_asp_y = 10000;
void metal_set_ui_aspect(float ax, float ay){
    if (!(ax > 0.05f && ax <= 1.0f)) ax = 1.0f;
    if (!(ay > 0.05f && ay <= 1.0f)) ay = 1.0f;
    atomic_store(&g_ui_asp, (int)lroundf(ax * 10000.f));
    atomic_store(&g_ui_asp_y, (int)lroundf(ay * 10000.f));
}
void com_request_reset(void);
static _Atomic int g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_dz = 0;
static _Atomic unsigned g_mouse_btn = 0;

void metal_mouse_add(int dx, int dy){                 /* synthetic motion (test) */
    atomic_fetch_add(&g_mouse_dx, dx); atomic_fetch_add(&g_mouse_dy, dy); }
void metal_fill_mouse(unsigned char *dst, unsigned n){
    if (n < 20) return;
    int dx = atomic_exchange(&g_mouse_dx, 0);
    int dy = atomic_exchange(&g_mouse_dy, 0);
    int dz = atomic_exchange(&g_mouse_dz, 0);
    unsigned b = atomic_load(&g_mouse_btn);
    memcpy(dst + 0, &dx, 4);
    memcpy(dst + 4, &dy, 4);
    memcpy(dst + 8, &dz, 4);
    for (int i = 0; i < 8; i++) dst[12 + i] = (b >> i) & 1u ? 0x80 : 0x00;
}
void metal_get_cursor(int *x, int *y){
    /* MOUSEAT=<x>,<y>[@<frame>,<x>,<y>]: pin the cursor in game space (test) */
    { const char *e = ENVC("MOUSEAT");
      if (e && *e){ int ax = 0, ay = 0, f2 = 0, bx = 0, by = 0;
          int n2 = sscanf(e, "%d,%d@%d,%d,%d", &ax, &ay, &f2, &bx, &by);
          if (n2 == 5 && atomic_load(&g_frames_drawn) >= f2){ ax = bx; ay = by; }
          if (x) *x = ax; if (y) *y = ay; return; } }
    if (x) *x = atomic_load(&g_mouse_x);
    if (y) *y = atomic_load(&g_mouse_y);
}
/* The window size the game thinks it has (GetClientRect/GetWindowRect).  The
 * game scales GetCursorPos into its own UI space by this rectangle; while a
 * fixed 1280x720 was returned, at 1280x960 the cursor drifted down by a
 * factor of 1.33 vertically (measured: MOUSEAT). */
void metal_game_size(unsigned *w, unsigned *h){
    if (w) *w = (unsigned)atomic_load(&g_game_w);
    if (h) *h = (unsigned)atomic_load(&g_game_h);
}
void metal_set_game_size(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    atomic_store(&g_game_w, (int)w); atomic_store(&g_game_h, (int)h);
}
/* The area the image covers in the window (points).  It is fitted to the
 * content KEEPING THE ASPECT RATIO (kCAGravityResizeAspect), so in full
 * screen black bars appear at the edges.  Returns 0 if the geometry is not
 * known yet. */
static int shown_rect(double *ox, double *oy, double *vw, double *vh){
    double dw = atomic_load(&g_draw_w), dh = atomic_load(&g_draw_h);
    double cw = atomic_load(&g_cont_w), ch = atomic_load(&g_cont_h);
    if (dw < 1.0 || dh < 1.0 || cw < 1.0 || ch < 1.0) return 0;
    double k = fmin(cw / dw, ch / dh);           /* points / drawable pixel */
    *vw = dw * k; *vh = dh * k;
    *ox = (cw - *vw) * 0.5; *oy = (ch - *vh) * 0.5;
    return 1;
}
/* Window point -> the GAME's resolution space (not the drawable: at Retina
 * scale the game thinks 1280x960 while the surface is 2448x1836).  Accounts
 * for the bars; Y is flipped: AppKit is bottom-left, the game top-left. */
/* autotest.c "click <x> <y>" (handled in the window loop next to MOUSESIM) */
static _Atomic int g_click_req = 0; static double g_click_x, g_click_y;
void metal_request_click(double x, double y){ g_click_x = x; g_click_y = y; atomic_store(&g_click_req, 1); }
static void mouse_from_point(double px, double py, int *gx, int *gy){
    double ox, oy, vw, vh;
    double lw = atomic_load(&g_game_w), lh = atomic_load(&g_game_h);
    double ch = atomic_load(&g_cont_h);
    if (!shown_rect(&ox, &oy, &vw, &vh) || lw < 1.0 || lh < 1.0){
        *gx = (int)px; *gy = (int)py; return; }
    { double a = atomic_load(&g_ui_asp) / 10000.0;   /* UI centred, narrowed */
      ox += vw * (1.0 - a) * 0.5; vw *= a;
      double b = atomic_load(&g_ui_asp_y) / 10000.0;
      oy += vh * (1.0 - b) * 0.5; vh *= b; }
    double x = (px - ox) / vw * lw;
    double y = (ch - py - oy) / vh * lh;
    if (x < 0.0) x = 0.0; else if (x > lw - 1.0) x = lw - 1.0;
    if (y < 0.0) y = 0.0; else if (y > lh - 1.0) y = lh - 1.0;
    *gx = (int)x; *gy = (int)y;
}
/* DirectInput relative motion.  A window POINT used to equal a game pixel,
 * so the point delta went through as is.  The window is now sized
 * independently of the game's resolution, so we scale to keep the same
 * feel: moving from one edge of the image to the other still produces a
 * game width of motion.  (Unscaled, with NFSNATIVE=0 the window was half
 * size, so the cursor moved at half speed.) */
static void mouse_delta_scale(double *sx, double *sy){
    double ox, oy, vw, vh;
    double lw = atomic_load(&g_game_w), lh = atomic_load(&g_game_h);
    *sx = *sy = 1.0;
    if (!shown_rect(&ox, &oy, &vw, &vh) || lw < 1.0 || lh < 1.0) return;
    vw *= atomic_load(&g_ui_asp) / 10000.0;
    vh *= atomic_load(&g_ui_asp_y) / 10000.0;
    *sx = lw / vw; *sy = lh / vh;
}

/* --- keyboard: runtime/keyboard_mac.m (NSEvent -> Win32 messages and
 * DirectInput state), runtime/keyboard.c (shared key state, host messages) */
#include "kbd.h"
int  kbdmac_handle(NSEvent *ev);
void kbdmac_tick(void);
void kbdmac_release_all(const char *why);

/* Windows mouse messages for the game's window procedure (it only uses
 * them for SetCapture/ReleaseCapture; menus read the DirectInput mouse and
 * GetCursorPos).  Coordinates are the game's client area. */
static uint32_t mouse_mk(NSEventModifierFlags fl){
    unsigned b = atomic_load(&g_mouse_btn);
    return ((b & 1u) ? 0x01u : 0u) | ((b & 2u) ? 0x02u : 0u) | ((b & 4u) ? 0x10u : 0u)
         | ((b & 8u) ? 0x20u : 0u) | ((b & 16u) ? 0x40u : 0u)
         | ((fl & NSEventModifierFlagShift) ? 0x04u : 0u) | ((fl & NSEventModifierFlagControl) ? 0x08u : 0u);
}
static uint32_t mouse_lp(void){
    return ((uint32_t)(atomic_load(&g_mouse_y) & 0xFFFF) << 16) | (uint32_t)(atomic_load(&g_mouse_x) & 0xFFFF);
}
static void mouse_button(int b, int down, NSEventModifierFlags fl){
    unsigned bit = 1u << b, was = atomic_load(&g_mouse_btn) & bit;
    if (down) atomic_fetch_or(&g_mouse_btn, bit); else atomic_fetch_and(&g_mouse_btn, ~bit);
    if (!!was == !!down) return;
    static const uint32_t dn[3] = {0x0201u, 0x0204u, 0x0207u};     /* WM_L/R/MBUTTONDOWN */
    if (b < 3) kbd_post(down ? dn[b] : dn[b] + 1u, mouse_mk(fl), mouse_lp());
}
static void mouse_release_all(void){
    for (int b = 0; b < 8; b++) mouse_button(b, 0, 0);
    atomic_store(&g_mouse_btn, 0u);
}
/* Textures are created BEFORE the window opens.  So the "available"
 * criterion is the device, not the layer; otherwise early textures were
 * silently lost. */
int  metal_available(void){ return g_dev != nil; }
int  metal_window_ready(void){ return g_layer != nil; }

long metal_frames(void){ return atomic_load(&g_frames_drawn); }
long metal_tris(void){ return atomic_load(&g_tris_drawn); }
long metal_tex_created(void){ return atomic_load(&g_tex_created); }
long metal_tex_uploaded(void){ return atomic_load(&g_tex_uploaded); }
int  metal_quit_requested(void){ return atomic_load(&g_should_quit); }

/* ---------------- draw command queue ----------------
 * The guest thread accumulates draws here; the main thread encodes them at Present. */
#include "gfx.h"
typedef TVert MVert;   /* clip space + two colours + 4x4 texture coordinates */
/* target: 0 = screen (drawable), otherwise the offscreen target g_tex[target].
 * An entry with is_clear=1 is not a draw but a command to clear the target. */
/* blend: packed blend state (see metal_blend_key)
 *   bit0      : alpha blending on
 *   bit1-5    : source factor (D3DBLEND)
 *   bit6-10   : destination factor
 *   bit11-13  : blend operation (D3DBLENDOP)
 *   bit14-17  : colour write mask                                        */
/* vsid > 0 : the vertex shader runs on the GPU.  Then the vertices are not
 * in g_vbuf but RAW in g_rvbuf (rawoff), and the constants are in g_kbuf
 * (koff); the draw is issued with vertexStart:0 from the buffer offset. */
/* ps_2_0 can use 32 float constants.  The file was cut off at 8 and, worse,
 * vsemu.c FOLDED higher indices with 'idx & 7': a shader reading constant
 * c8 saw c0.  Post-processing (bloom threshold, blur tap weights) uses
 * exactly that range. */
#define PSC_N 32
typedef struct { unsigned first, count, tex, target, clear_argb, is_clear;
                 unsigned blend; float alpha_ref; unsigned depth;
                 unsigned tex1, tex2, tex3; int ps;
                 /* ps constants are in the frame's pool (g_pscb[b][psci]); c0 also
                  * here (the HD ray-tracing material reads only that) */
                 unsigned psci; float c0[4];
                 int vsid; unsigned rawoff, koff; float proj[4];
                 unsigned smp[4];
                 unsigned is_blit, blit_src; unsigned bcol; unsigned cull;
                 float fogv[4];
                 unsigned clr_flags; float clr_z; unsigned ksz;
                 /* indexed draw (see PERSISTENT VERTEX / INDEX POOL): if ix_isz is 2/4,
                  * rawoff is the offset of vertex 0 in the pool, ix_ib the offset of
                  * the first index; count is the index count.  gh: geometry id
                  * (0 = hash of the contents). */
                 unsigned ix_isz, ix_ib; int ix_base; unsigned long long gh; } MDraw;

#define MAXV  (256*1024)
#define MAXD  (16*1024)
#define MAXRV (16u*1024u*1024u)      /* raw vertex arena (bytes) */
#define MAXKB ( 4u*1024u*1024u)      /* vertex constant arena (bytes) */
static MVert  *g_vbuf[2];
static MDraw  *g_dbuf[2];
static unsigned g_vn[2], g_dn[2];
static uint8_t *g_rvbuf[2];  static unsigned g_rvn[2];
static uint8_t *g_kbuf[2];   static unsigned g_kn[2];
static id<MTLBuffer> g_mtlrvb, g_mtlkb;
static unsigned g_ov_rv = 0, g_ov_kb = 0;   /* arena overflow counter */
/* --- frame budget measurement: how busy is the GPU really? -----------
 * Until now only the CPU side was measured; "what are we spending the
 * hardware on" cannot be answered without knowing the GPU time. */
static _Atomic unsigned long long g_gpu_ns = 0, g_gpu_n = 0, g_gpu_max_ns = 0;
static _Atomic unsigned long long g_enc_ns = 0;         /* encoding (CPU) */
/* GPUWIN=a,b: the same measurements for frames [a,b) only (so menus do not
 * dilute the average; to measure per-feature GPU cost in a race window) */
static _Atomic unsigned long long g_gpuw_ns = 0, g_gpuw_n = 0, g_encw_ns = 0, g_encw_n = 0;
static long g_gpuw_a = -1, g_gpuw_b = -1;
static int gpuw_in(long f){
    if (g_gpuw_a == -1){ const char *e = ENVC("GPUWIN"); long a = 0, b = 0;
        if (e && sscanf(e, "%ld,%ld", &a, &b) == 2 && b > a){ g_gpuw_b = b; g_gpuw_a = a; } else g_gpuw_a = -2; }
    return g_gpuw_a >= 0 && f >= g_gpuw_a && f < g_gpuw_b;
}
/* ---- STEP-BY-STEP MEASUREMENT (HD_RT_AB=1) -----------------------------
 * Splits the GPUWIN window into 100-frame slices; in each slice part of the
 * ray-tracing pipeline is switched off and the GPU time per slice is
 * reported.  The weather (rain) is random: instead of comparing separate
 * runs this gives the cost of each step in the SAME run, on the same scene.
 * The first 30 frames of a slice are dropped (frames of the previous mode
 * still in the queue). */
#define AB_N 7
static const char *g_ab_name[AB_N] = {
    "0 full (ray tracing + composite)", "1 tracing, no composite", "2 plain tracing (no shading), no composite",
    "3 capture + structure build", "4 capture only", "5 no ray tracing, SSR composite", "6 no ray tracing, no composite" };
static int g_ab_on = -1, g_ab_mode = -1;      /* g_ab_mode: this frame's mode (-1 = normal) */
static _Atomic unsigned long long g_ab_ns[AB_N], g_ab_cnt[AB_N];
static int ab_frame(long f, int *count_it){
    if (g_ab_on < 0){ const char *e = ENVC("HD_RT_AB"); g_ab_on = (e && *e == '1') ? 1 : 0; }
    *count_it = 0;
    if (!g_ab_on || !gpuw_in(f)) return -1;
    long r = f - g_gpuw_a;
    *count_it = (r % 100) >= 30;
    return (int)((r / 100) % AB_N);
}
/* ---- PASS PROFILE --------------------------------------------------------
 * PASSPROF=N: on every Nth frame in the GPUWIN window, measure the GPU time
 * of every pass (render / compute / blit / acceleration structure) with the
 * Metal counter sampler.  Answers "where does the GPU time go" DIRECTLY,
 * not from the difference of toggling a feature.  Render passes report the
 * vertex and fragment stages separately (Apple GPUs are tile based: first
 * all vertex work, then fragment work).
 * Timestamps are in GPU clock; converted to ns with a two-point
 * sampleTimestamps. */
#include <mach/mach_time.h>
#define PP_MAXP 160
#define PP_MAXC 64
static id<MTLCounterSet> g_pp_set;
static int g_pp_every = -1;
static int g_pp_mode = 3;    /* PASSPROF_MODE: 1 = render passes only, 2 = compute/blit/accel only, 3 = all */
static pthread_mutex_t g_pp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[48]; double v, f; unsigned long long n; } g_pp_cat[PP_MAXC];
static int g_pp_ncat; static unsigned long long g_pp_frames;
static double g_pp_cb_s;                      /* command buffer GPU time of the same frames (s) */
static MTLTimestamp g_pp_c0, g_pp_g0;
static id<MTLCounterSampleBuffer> g_pp_sb;    /* this frame (encoding thread only) */
static int g_pp_np; static uint8_t g_pp_pc[PP_MAXP], g_pp_pk[PP_MAXP];
static int pp_cat(const char *name){
    for (int i = 0; i < g_pp_ncat; i++) if (!strcmp(g_pp_cat[i].name, name)) return i;
    if (g_pp_ncat >= PP_MAXC) return PP_MAXC - 1;
    snprintf(g_pp_cat[g_pp_ncat].name, sizeof g_pp_cat[0].name, "%s", name);
    return g_pp_ncat++;
}
static void pp_begin(long fno){
    g_pp_sb = nil; g_pp_np = 0;
    if (g_pp_every < 0){
        const char *e = ENVC("PASSPROF"); g_pp_every = e ? atoi(e) : 0;
        { const char *m = ENVC("PASSPROF_MODE"); if (m && atoi(m) > 0) g_pp_mode = atoi(m); }
        if (g_pp_every > 0){
            if (![g_dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) g_pp_every = 0;
            for (id<MTLCounterSet> cs in g_dev.counterSets)
                if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) g_pp_set = cs;
            if (!g_pp_set) g_pp_every = 0;
            if (!g_pp_every) fprintf(stderr, "[PASSPROF] this GPU has no counter sampling\n");
            else [g_dev sampleTimestamps:&g_pp_c0 gpuTimestamp:&g_pp_g0];
        }
    }
    if (g_pp_every <= 0 || !gpuw_in(fno) || fno % g_pp_every) return;
    MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
    sd.counterSet = g_pp_set; sd.storageMode = MTLStorageModeShared; sd.sampleCount = PP_MAXP * 4;
    NSError *er = nil;
    g_pp_sb = [g_dev newCounterSampleBufferWithDescriptor:sd error:&er];
}
static int pp_slot(const char *name, int kind){
    if (!g_pp_sb || g_pp_np >= PP_MAXP) return -1;
    if (!(g_pp_mode & (kind ? 2 : 1))) return -1;
    pthread_mutex_lock(&g_pp_lock); int c = pp_cat(name); pthread_mutex_unlock(&g_pp_lock);
    g_pp_pc[g_pp_np] = (uint8_t)c; g_pp_pk[g_pp_np] = (uint8_t)kind;
    return g_pp_np++;
}
static void pp_rp(MTLRenderPassDescriptor *rp, const char *name){
    int k = pp_slot(name, 0); if (k < 0) return;
    MTLRenderPassSampleBufferAttachmentDescriptor *a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = g_pp_sb;
    a.startOfVertexSampleIndex = (NSUInteger)k * 4;     a.endOfVertexSampleIndex = (NSUInteger)k * 4 + 1;
    a.startOfFragmentSampleIndex = (NSUInteger)k * 4 + 2; a.endOfFragmentSampleIndex = (NSUInteger)k * 4 + 3;
}
static id<MTLBlitCommandEncoder> pp_blit(id<MTLCommandBuffer> cb, const char *name){
    int k = pp_slot(name, 1); if (k < 0) return [cb blitCommandEncoder];
    MTLBlitPassDescriptor *d = [MTLBlitPassDescriptor blitPassDescriptor];
    d.sampleBufferAttachments[0].sampleBuffer = g_pp_sb;
    d.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)k * 4;
    d.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)k * 4 + 1;
    return [cb blitCommandEncoderWithDescriptor:d];
}
static id<MTLComputeCommandEncoder> pp_compute(id<MTLCommandBuffer> cb, const char *name){
    int k = pp_slot(name, 1); if (k < 0) return [cb computeCommandEncoder];
    MTLComputePassDescriptor *d = [MTLComputePassDescriptor computePassDescriptor];
    d.sampleBufferAttachments[0].sampleBuffer = g_pp_sb;
    d.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)k * 4;
    d.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)k * 4 + 1;
    return [cb computeCommandEncoderWithDescriptor:d];
}
static id<MTLAccelerationStructureCommandEncoder> pp_accel(id<MTLCommandBuffer> cb, const char *name){
    if (@available(macOS 13.0, *)){
        int k = pp_slot(name, 1);
        if (k >= 0){
            MTLAccelerationStructurePassDescriptor *d = [MTLAccelerationStructurePassDescriptor accelerationStructurePassDescriptor];
            d.sampleBufferAttachments[0].sampleBuffer = g_pp_sb;
            d.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)k * 4;
            d.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)k * 4 + 1;
            return [cb accelerationStructureCommandEncoderWithDescriptor:d];
        }
    }
    return [cb accelerationStructureCommandEncoder];
}
static void pp_end(id<MTLCommandBuffer> cb){
    if (!g_pp_sb || !g_pp_np) { g_pp_sb = nil; return; }
    id<MTLCounterSampleBuffer> sb = g_pp_sb; int np = g_pp_np;
    { static int once = 0;            /* pass ORDER of the first profiled frame */
      if (!once){ once = 1; fprintf(stderr, "  --- pass order (first profiled frame) ---\n");
          for (int k = 0; k < np; k++) fprintf(stderr, "  [order %2d] %s\n", k, g_pp_cat[g_pp_pc[k]].name); } }
    uint8_t *pc = malloc((size_t)np * 2); memcpy(pc, g_pp_pc, (size_t)np); memcpy(pc + np, g_pp_pk, (size_t)np);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b2){
        NSData *dt = [sb resolveCounterRange:NSMakeRange(0, (NSUInteger)np * 4)];
        if (dt && dt.length >= (NSUInteger)np * 4 * sizeof(MTLCounterResultTimestamp)){
            const MTLCounterResultTimestamp *t = (const MTLCounterResultTimestamp *)dt.bytes;
            pthread_mutex_lock(&g_pp_lock);
            for (int k = 0; k < np; k++){
                uint64_t a = t[k*4].timestamp, b = t[k*4+1].timestamp, c = t[k*4+2].timestamp, d2 = t[k*4+3].timestamp;
                int ci = pc[k];
                if (a != MTLCounterErrorValue && b != MTLCounterErrorValue && b >= a) g_pp_cat[ci].v += (double)(b - a);
                if (!pc[np + k] && c != MTLCounterErrorValue && d2 != MTLCounterErrorValue && d2 >= c) g_pp_cat[ci].f += (double)(d2 - c);
                g_pp_cat[ci].n++;
            }
            g_pp_frames++; g_pp_cb_s += b2.GPUEndTime - b2.GPUStartTime;
            pthread_mutex_unlock(&g_pp_lock);
        }
        free(pc);
    }];
    g_pp_sb = nil;
}
static void pp_report(void){
    if (g_pp_every <= 0 || !g_pp_frames) return;
    MTLTimestamp c1, g1; [g_dev sampleTimestamps:&c1 gpuTimestamp:&g1];
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double sc = (g1 > g_pp_g0) ? ((double)(c1 - g_pp_c0) * tb.numer / tb.denom) / (double)(g1 - g_pp_g0) : 1.0;   /* ns / GPU tick */
    double tot = 0; int ord[PP_MAXC];
    for (int i = 0; i < g_pp_ncat; i++){ ord[i] = i; tot += g_pp_cat[i].v + g_pp_cat[i].f; }
    /* On Apple GPUs the counter samples come directly in ns (sampleTimestamps in mach ticks):
     * if the pass total exceeds the command buffer time, the scale is 1. */
    if (tot * sc > g_pp_cb_s * 1e9 * 1.5) sc = 1.0;
    for (int i = 0; i < g_pp_ncat; i++) for (int j = i + 1; j < g_pp_ncat; j++)
        if (g_pp_cat[ord[j]].v + g_pp_cat[ord[j]].f > g_pp_cat[ord[i]].v + g_pp_cat[ord[i]].f){ int t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
    double fr = (double)g_pp_frames;
    fprintf(stderr, "  --- pass profile: %llu frames, passes in total %.3f ms/frame, command buffer %.3f ms/frame (scale %.4f ns/tick) ---\n",
            g_pp_frames, tot * sc / fr / 1e6, g_pp_cb_s / fr * 1e3, sc);
    fprintf(stderr, "  %-44s %8s %8s %8s %7s\n", "pass", "ms/frame", "vertex", "fragment", "n/frame");
    for (int q = 0; q < g_pp_ncat; q++){ int i = ord[q];
        fprintf(stderr, "  %-44s %8.3f %8.3f %8.3f %7.2f\n", g_pp_cat[i].name,
                (g_pp_cat[i].v + g_pp_cat[i].f) * sc / fr / 1e6, g_pp_cat[i].v * sc / fr / 1e6,
                g_pp_cat[i].f * sc / fr / 1e6, (double)g_pp_cat[i].n / fr); }
}
static _Atomic unsigned long long g_pass_n = 0, g_draw_n = 0;
/* Render target = texture id | ((face+1) << 16).  With a cube texture the
 * game draws to the SIX FACES of the same texture separately; without the
 * face all six went on top of each other to the same surface. */
#define TGT_ID(t)   ((t) & 0xFFFFu)
#define TGT_FACE(t) (((t) >> 16) & 7u)          /* 0 = no face */
static unsigned g_cur_target = 0;          /* the guest's active render target */
static unsigned g_cur_blend  = 0;
static unsigned g_cur_bcol   = 0xFFFFFFFFu;   /* D3DRS_BLENDFACTOR (ARGB) */
static unsigned g_cur_cull   = 3u;            /* D3DRS_CULLMODE: 1=NONE 2=CW 3=CCW */
static unsigned g_cur_depth  = 1u << 8; /* bit0=test, bit1=write, bit2-4=func,
                                          * bit8+ = bound Z surface (DEPTH_ZID) */
static unsigned g_cur_dmode  = 0;
static unsigned g_cur_zid    = 1;       /* 1 = the device's implicit Z surface */
static unsigned g_cur_tex1 = 0, g_cur_tex2 = 0, g_cur_tex3 = 0;
static int      g_cur_ps = 0;              /* active pixel shader */
static float    g_cur_psc[PSC_N][4];       /* ps constants c0..c(PSC_N-1) */
/* Per-frame ps constant pool.  Every draw record used to carry all 32
 * constants (512 bytes): a copy + clear, plus another copy and compare when
 * encoding, ~2 KB per draw.  Measured: 92% of draws use the SAME constants
 * as the previous one (the game writes up to c3).  Now constants are added
 * to the pool only when they change, and the record carries the index. */
static float    (*g_pscb[2])[PSC_N][4];
static unsigned g_pscn[2], g_psc_last[2], g_psc_lastver[2];
static unsigned g_psc_ver = 1;              /* bumped whenever g_cur_psc changes */
static unsigned psc_put(int b){              /* under g_qlock */
    if (g_pscn[b] && g_psc_lastver[b] == g_psc_ver) return g_psc_last[b];
    if (g_pscn[b] >= MAXD) return g_psc_last[b];
    memcpy(g_pscb[b][g_pscn[b]], g_cur_psc, sizeof g_cur_psc);
    g_psc_last[b] = g_pscn[b]++; g_psc_lastver[b] = g_psc_ver;
    return g_psc_last[b];
}
/* Per-stage projection mode: 0 = none, 3 = xy/z, 4 = xy/w.
 * D3DTSS_TEXTURETRANSFORMFLAGS | D3DTTFF_PROJECTED. */
static float    g_cur_proj[4];
static unsigned g_cur_smp[4] = {0,0,0,0};
/* The game's D3DSAMP_* state.  There used to be ONE global sampler
 * (linear/linear/mip-linear, no anisotropy).  The game actually asks for
 * MINFILTER=ANISOTROPIC + MAXANISOTROPY=16; with isotropic trilinear the
 * derivative estimate jumps at triangle EDGES, a mip one level too coarse
 * is picked and a 1-pixel line appears along the edges + diagonal of every
 * quad (most visible on lettering).                                     */
void metal_set_samplers(const unsigned k[4]){
    for (int i = 0; i < 4; i++) g_cur_smp[i] = k[i];
}
#if NFS_ENHANCED
#define MAXPS 2560          /* + reflection variants: ps + PSREFL*(1+stage) */
#else
#define MAXPS 512
#endif
#define PSCUBE 256          /* cube variant: psid + PSCUBE */
#define PSREFL 512          /* (HD) reflection separation variant */
static id<MTLFunction> g_psfn[MAXPS];      /* ps compiled from the translation */
/* v12na: a version of the same ps WITHOUT the alpha test (discard).  On
 * Apple TBDR GPUs a shader containing discard turns off hidden surface
 * removal (HSR); draws with the alpha test off (alpha_ref <= 0) use this
 * version.  NOHSRFIX=1 turns it off (A/B). */
static id<MTLFunction> g_psfn_nd[MAXPS];
static int hsrfix_on(void){ static int on = -1; if (on < 0){ const char *e = ENVC("NOHSRFIX"); on = !(e && *e && *e != '0'); } return on; }
/* HSRAB=1: toggle the discard-free version in 120-frame slices within
 * GPUWIN; GPU time is summed per mode in the same run and the same scene
 * (the first 30 frames of a slice are dropped).  Report:
 * metal_budget_report. */
static int g_hsr_ab = -1, g_hsr_cur = 1;
static _Atomic unsigned long long g_hsr_ns[2], g_hsr_cnt[2];
static int hsr_draw_on(void){
    if (g_hsr_ab < 0){ const char *e = ENVC("HSRAB"); g_hsr_ab = (e && *e == '1'); }
    return g_hsr_ab ? g_hsr_cur : hsrfix_on();
}
static id<MTLTexture>        g_depth_tex;  /* scratch used when there is NO Z surface */
/* --- Z surfaces -------------------------------------------------------
 * There used to be a single Z texture, re-created every time the target's
 * size changed.  In one frame the game goes back and forth between 320x240
 * (light/glow), 6 x 128x128 (reflection cube), the screen and 640x480
 * (blur) targets; on every return to the screen Z was loaded from a new
 * (undefined) texture -> city lights and some cars showed through walls.
 * Now it is as in D3D: every Z surface has its own texture,
 * SetDepthStencilSurface decides which one is bound and Clear(ZBUFFER) when
 * it is cleared; the contents are kept across frames. */
#define MAXZ 64
#define DEPTH_ZID(d) ((d) >> 8)
static id<MTLTexture> g_ztex[MAXZ];
static unsigned char  g_zpend[MAXZ];        /* clear on the next pass */
static float          g_zpend_v[MAXZ];
static _Atomic unsigned long long g_z_mismatch = 0, g_z_null = 0;
static _Atomic unsigned char g_zforget[MAXZ];  /* guest re-created it: drop the texture */
/* Released at a device Reset: D3D9 has the game release every depth surface
 * it created before Reset, and the game creates new ones (new ids) after it.
 * A dead id's texture used to stay until com.c's id counter wrapped round to
 * it (62 ids, ~9 Resets); with the larger mirror target that was tens of MB
 * per Reset.  metal_reset_rts marks ids 2.. dead; a create clears its id
 * again; rt_free_pending drops what is still dead 3 frames after the Reset. */
static _Atomic unsigned char g_zdead[MAXZ];
static _Atomic long g_zsweep_at = 0;
static unsigned long g_z_swept = 0;
void metal_z_forget(unsigned zid){
    if (zid < MAXZ){ atomic_store(&g_zdead[zid], 0); atomic_store(&g_zforget[zid], 1); } }
static id<MTLDepthStencilState> g_dss[64];
static float    g_cur_aref   = 0.f;
/* D3DRS_FOGENABLE(28) + D3DRS_FOGCOLOR(34).  Fog was NEVER applied: the
 * vertex shader's oFog output went to a "dummy" in MSL.  6 of the game's 18
 * vertex shaders write oFog and FOGENABLE is set 5.3M times -> distant
 * geometry never faded. (r,g,b,on) */
static float    g_cur_fog[4] = {0.f,0.f,0.f,0.f};
static int      g_cur = 0;                 /* buffer the guest writes */
static int      g_ready = -1;              /* buffer the main thread will draw */
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qcond = PTHREAD_COND_INITIALIZER;
/* g_qcond: main thread -> guest ("got your frame").
 * g_rcond: guest -> main thread ("frame ready").  The main thread used to
 * POLL with a 4 ms nanosleep; the frame time histogram was quantized in
 * exact 5 ms steps (15.5/20.5/25.5/30.5 ms, EMPTY in between).  After the
 * guest finished a frame it waited 0-5 ms for nothing until the main
 * thread picked it up.  Now the wakeup is immediate. */
static pthread_cond_t  g_rcond = PTHREAD_COND_INITIALIZER;
static _Atomic long    g_frames_dropped = 0;
static _Atomic long    g_frames_replaced = 0;   /* mailbox (framerate.c hfr_mailbox) */
/* The buffer the main thread is ENCODING right now (-1 = none).  Even if
 * metal_present times out it CANNOT swap to this buffer: the main thread
 * walks g_dbuf[rb] without the lock; if the guest wrote the new frame's
 * draws into the same buffer the frame would be cut short or new draws
 * would be drawn with old vertex data. */
static int g_enc_rb = -1;
static _Atomic unsigned long g_cb_err = 0;         /* frame that ended with a GPU error */
static _Atomic unsigned long g_enc_late = 0;       /* encoding still running when 60 ms ran out */
static long g_enc_late_f[8];                       /* frame numbers of the first few */

static id<MTLRenderPipelineState> g_pso;
static id<MTLLibrary>             g_lib;
static MTLVertexDescriptor       *g_vdesc;
static id<MTLBuffer>              g_mtlvb;
/* --- per-frame buffer ring ---------------------------------------------
 * g_mtlvb / g_mtlrvb / g_mtlkb used to be SINGLE: every frame's vertex and
 * constant data was copied into the same shared buffer.  [cb commit] does
 * NOT WAIT for the GPU; if the next frame's memcpy comes before the GPU has
 * run the previous frame's vertex stage, that frame's draws (first/rawoff/
 * koff laid out for the OLD frame) read the NEW frame's bytes.  Screen-space
 * work drawn at the end of the frame (post-processing + HUD) is the first
 * victim: huge triangles covering the screen with the font atlas on them;
 * the world intact (user photos, 2026-10-13).  Measured: in a race, in 6472
 * of 9000 frames the buffer was overwritten before the previous frame had
 * finished on the GPU; in 1046 the layout had shifted too.
 * Now every frame takes a set the GPU has FINISHED with; the completion
 * handler gives it back.  Correctness depends on the semaphore, not on the
 * NUMBER of sets: if all are in flight fb_acquire waits for the GPU.  Three
 * sets rest on measurement: 8000 race frames, two runs, 0 waits (2+ frames
 * in flight at once: 6/9000 in a visible window, 2292/7600 in a transparent
 * test window -- the third set covers that).  The one exception is the
 * machine asleep (lid closed, DarkWake): the GPU stops, so 3138 waits were
 * seen in 7600 frames; the timing of those runs is invalid. */
#define NFB 3
static id<MTLBuffer>         g_fb_vb[NFB], g_fb_rvb[NFB], g_fb_kb[NFB];
static _Atomic int           g_fb_busy[NFB];
static dispatch_semaphore_t  g_fb_sem;
static _Atomic unsigned long g_fb_overlap = 0, g_fb_wait = 0;
static _Atomic unsigned long long g_fb_wait_ns = 0;
static int fb_acquire(void){
    if (dispatch_semaphore_wait(g_fb_sem, DISPATCH_TIME_NOW) != 0){
        struct timespec w0, w1; clock_gettime(CLOCK_MONOTONIC, &w0);
        atomic_fetch_add(&g_fb_wait, 1ul);          /* all on the GPU: wait until one finishes */
        dispatch_semaphore_wait(g_fb_sem, DISPATCH_TIME_FOREVER);
        clock_gettime(CLOCK_MONOTONIC, &w1);
        atomic_fetch_add(&g_fb_wait_ns, (unsigned long long)(w1.tv_sec - w0.tv_sec) * 1000000000ull
                         + (unsigned long long)w1.tv_nsec - (unsigned long long)w0.tv_nsec);
    }
    int got = 0, any = 0;
    for (int i = 0; i < NFB; i++){
        if (!got){ int z = 0;
            if (atomic_compare_exchange_strong(&g_fb_busy[i], &z, 1)){ got = i + 1; continue; } }
        if (atomic_load(&g_fb_busy[i])) any = 1;
    }
    if (any) atomic_fetch_add(&g_fb_overlap, 1ul);  /* this frame used to overwrite the buffer */
    return got - 1;                                 /* the semaphore guarantees at least one free set */
}
static void fb_release(int i){
    atomic_store(&g_fb_busy[i], 0);
    dispatch_semaphore_signal(g_fb_sem);
}
/* Set buffers are NOT a fixed MAXV/MAXRV/MAXKB size; they grow with what
 * the frame needs.  Once a shared buffer has been used on the GPU ALL of it
 * counts toward the footprint (measured: +85 MB with only 64 KB of the
 * 3 x 28 MB buffers filled); three fixed-size sets held 147 MB, while the
 * CPU vertex path carries ~400 vertices (45 KB) per frame in a race.  The
 * set comes FREE from fb_acquire, so the old buffer is in no command
 * buffer; replacing it is safe. */
#define FB_STEP (256u * 1024u)
static _Atomic unsigned long g_fb_grow = 0;
static id<MTLBuffer> fb_fit(id<MTLBuffer> b, NSUInteger need, NSUInteger maxlen){
    if (b && b.length >= need) return b;
    NSUInteger cap = need + need / 4;               /* headroom: do not re-create every frame */
    cap = (cap + FB_STEP - 1) / FB_STEP * FB_STEP;
    if (cap < FB_STEP) cap = FB_STEP;
    if (cap > maxlen) cap = maxlen;
    id<MTLBuffer> nb = [g_dev newBufferWithLength:cap options:MTLResourceStorageModeShared];
    if (!nb) return b;                              /* fb_put writes what fits in the old one */
    if (b) atomic_fetch_add(&g_fb_grow, 1ul);
    return nb;
}
static void fb_put(id<MTLBuffer> b, const void *src, NSUInteger n){
    if (b && n) memcpy([b contents], src, n < b.length ? n : b.length);
}
/* --- PERSISTENT VERTEX / INDEX POOL ---------------------------------------
 * The game's static (never locked) vertex and index buffers are mirrored on
 * the GPU ONCE; indexed draws are drawn from there with
 * drawIndexedPrimitives.  The indices used to be expanded on the guest side
 * every frame (an intermediate array), copied to the arena and then to the
 * Metal buffer: 3 x 2.7 MB/frame, ~1 ms CPU; and since the vertices were
 * multiplied x2.1 the GPU vertex stage (~19% of the pass time at 4K HD) ran
 * twice as much as well.  Measured (2026-10-14): 99.6% of the source VBs
 * have not been locked for 120+ frames.
 * The pool is ONE buffer, so the HD capture kernel can read all records
 * with a single binding.  Mirrors are only built/dropped on the guest
 * thread; the main thread binds the pool object as it was when the frame
 * was handed over (growing copies the old one into the new one, offsets do
 * not change).  Freed blocks are reused PF_DELAY frames later so frames in
 * flight can finish. */
#define MIR_N     4096u             /* mirror table (open addressing, 2^n) */
#define MIR_TOMB  1u                /* deleted slot (a guest address is never 1) */
#define PF_N      4096
#define PF_DELAY  8u                /* frames */
#define MIR_IDLE  900u              /* a mirror not drawn for this many frames is dropped */
#define POOL_MAX  (768u << 20)
static id<MTLBuffer> g_pool;        /* current pool (changed under g_qlock) */
static id<MTLBuffer> g_pool_fr[2];  /* the pool as it was when the frame was handed over */
static uint32_t g_pool_top = 0;
static struct { uint32_t key, gen, off, size, imax, used; } g_mir[MIR_N];
static struct { uint32_t off, size, at; } g_pf[PF_N];
static int g_pfn = 0;
static uint32_t g_gframe = 0;       /* guest frame counter (metal_present) */
unsigned metal_guest_frames(void){ return g_gframe; }   /* frame-rate counter (a stats read) */
static unsigned long g_mir_build = 0, g_mir_drop = 0, g_mir_live = 0, g_mir_full = 0;
static unsigned long long g_mir_bytes = 0;
static int pool_reserve(uint32_t top){
    NSUInteger cap = g_pool ? g_pool.length : 0;
    if (top <= cap) return 1;
    NSUInteger nc = cap ? cap : (4u << 20);
    while (nc < top) nc += nc / 2;
    if (nc > POOL_MAX) return 0;
    id<MTLBuffer> nb = [g_dev newBufferWithLength:nc options:MTLResourceStorageModeShared];
    if (!nb) return 0;
    if (g_pool && g_pool_top) memcpy([nb contents], [g_pool contents], g_pool_top);
    pthread_mutex_lock(&g_qlock); g_pool = nb; pthread_mutex_unlock(&g_qlock);
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
/* v12na: freed blocks are merged with free neighbours.  Without merging,
 * every race load left the free list a little more fragmented (other tracks,
 * other buffer sizes), new mirrors went to the top and the pool kept
 * growing toward POOL_MAX.  The merged block may be reused only when its
 * newest part is PF_DELAY frames old. */
static unsigned long g_pf_lost = 0;
static void pool_free(uint32_t off, uint32_t size){
    if (!size) return;
    uint32_t at = g_gframe;
    for (int i = 0; i < g_pfn; ){
        if (g_pf[i].off + g_pf[i].size == off || off + size == g_pf[i].off){
            if (g_pf[i].off < off) off = g_pf[i].off;
            size += g_pf[i].size;
            g_pf[i] = g_pf[--g_pfn];                /* re-scan: the merged block may touch another */
            i = 0; continue;
        }
        i++;
    }
    if (g_pfn < PF_N){ g_pf[g_pfn].off = off; g_pf[g_pfn].size = size; g_pf[g_pfn].at = at; g_pfn++; }
    else g_pf_lost++;
}
static void mir_sweep(void){                 /* drop mirrors not drawn for a long time */
    for (unsigned i = 0; i < MIR_N; i++){
        if (g_mir[i].key <= MIR_TOMB || g_gframe - g_mir[i].used < MIR_IDLE) continue;
        pool_free(g_mir[i].off, g_mir[i].size);
        g_mir[i].key = MIR_TOMB; g_mir_drop++; g_mir_live--;
    }
}
/* Guest: the pool offset of this generation (gen) of the guest object (key);
 * built now if missing or stale.  If isz > 0 it is an index buffer and *imax
 * becomes the largest index in the whole buffer.  0 = could not build (the
 * caller falls back to the old path). */
int metal_mirror(uint32_t key, uint32_t gen, const void *src, uint32_t size, unsigned isz,
                 uint32_t *off, uint32_t *imax){
    if (!g_dev || key <= MIR_TOMB || !size || size > (64u << 20)) return 0;
    { static uint32_t last = 0; if (g_gframe - last >= 256u){ last = g_gframe; mir_sweep(); } }
    unsigned h = (key * 2654435761u) & (MIR_N - 1u), slot = MIR_N, q;
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
    uint8_t *dst = (uint8_t *)[g_pool contents] + o;
    memcpy(dst, src, size);
    uint32_t mx = 0;
    if (isz == 2u){ const uint16_t *ix = (const uint16_t *)dst; for (uint32_t k = 0; k < size / 2u; k++) if (ix[k] > mx) mx = ix[k]; }
    else if (isz == 4u){ const uint32_t *ix = (const uint32_t *)dst; for (uint32_t k = 0; k < size / 4u; k++) if (ix[k] > mx) mx = ix[k]; }
    g_mir[slot].key = key; g_mir[slot].gen = gen; g_mir[slot].off = o; g_mir[slot].size = sz;
    g_mir[slot].imax = mx; g_mir[slot].used = g_gframe;
    g_mir_build++; g_mir_live++; g_mir_bytes += size;
    *off = o; if (imax) *imax = mx;
    return 1;
}
/* Guest (com.c): the guest buffer is gone; drop its mirror now (its address
 * may soon belong to a new buffer). */
void metal_mirror_forget(uint32_t key){
    if (key <= MIR_TOMB) return;
    unsigned h = (key * 2654435761u) & (MIR_N - 1u);
    for (unsigned q = 0; q < 64u; q++){
        unsigned i = (h + q) & (MIR_N - 1u);
        if (g_mir[i].key == key){ pool_free(g_mir[i].off, g_mir[i].size); g_mir[i].key = MIR_TOMB; g_mir_live--; g_mir_drop++; return; }
        if (!g_mir[i].key) return;
    }
}
/* Guest: CPU view of the mirrors in the pool (for scanning index ranges). */
const uint8_t *metal_pool_cpu(void){ return g_pool ? (const uint8_t *)[g_pool contents] : NULL; }
/* Main thread: raw bytes of the draw's j-th vertex (HD ray-tracing CPU reads). */
static const uint8_t *dr_vtx(int rb, const MDraw *dr, unsigned j, unsigned stride){
    if (!dr->ix_isz) return g_rvbuf[rb] + dr->rawoff + (size_t)j * stride;
    const uint8_t *P = (const uint8_t *)[g_pool_fr[rb] contents];
    uint32_t ix = dr->ix_isz == 4u ? ((const uint32_t *)(P + dr->ix_ib))[j]
                                   : ((const uint16_t *)(P + dr->ix_ib))[j];
    return P + dr->rawoff + (size_t)((int64_t)ix + dr->ix_base) * stride;
}
static id<MTLBuffer>              g_blitvb;   /* full-screen quad for StretchRect */
static id<MTLSamplerState>        g_samp;

/* --- texture table --- */
#define MAXTEX 16384   /* long session + many Resets: every Reset re-creates the RTs */
static uint8_t g_tex_cube[MAXTEX];          /* 1 = MTLTextureTypeCube */
static unsigned g_tex_lvlmask[MAXTEX];
static id<MTLTexture> g_tex[MAXTEX];
static unsigned       g_texn = 1;            /* 0 = "no texture" (white) */
/* --- releasing render targets at Reset -----------------------------------
 * In D3D9 the game must release ALL D3DPOOL_DEFAULT resources (render
 * targets included) before Reset, otherwise Reset fails.  We never released
 * the Metal texture on Release: every resolution change leaked ~6 render
 * targets (>100 MB per change at 4K).  At Reset the render targets created
 * up to that point are considered dead and are released on the main thread
 * once the frame(s) on screen are done. */
static uint8_t g_tex_isrt[MAXTEX];
static _Atomic unsigned g_rtfree_upto = 0;    /* RTs with an id BELOW this are dead */
static unsigned         g_rtfree_done = 1;    /* released up to here */
static _Atomic long     g_rtfree_at = 0;      /* release after this frame */
static unsigned long    g_rt_freed = 0;
static uint8_t          g_tex_freed[MAXTEX];      /* diagnostic: released */
static unsigned short   g_freed_w[MAXTEX], g_freed_h[MAXTEX];
static unsigned long    g_freed_use_smp = 0, g_freed_use_tgt = 0;
static void freed_use(unsigned id_, int as_tgt){
    if (id_ >= MAXTEX || !g_tex_freed[id_]) return;
    if (as_tgt) g_freed_use_tgt++; else g_freed_use_smp++;
    static int shown = 0;
    if (shown < 12){ shown++;
        fprintf(stderr, "metal: WARNING released RT used as a %s: id=%u %ux%u%s\n",
                as_tgt ? "TARGET" : "texture", id_, g_freed_w[id_], g_freed_h[id_],
                g_tex_cube[id_] ? " (cube)" : ""); }
}
static id<MTLTexture> g_white;

/* --- texture lifetime (v12na, long sessions) ------------------------------
 * Textures used to live for the whole session: ids were never reused and
 * only render targets were dropped at Reset (heuristically), so every race
 * load added its ~1350 textures and after a dozen loads the 16384-entry table
 * would have been full.  com.c now frees a texture when its last reference
 * goes (metal_free_texture).  Frames already recorded may still name the id:
 * metal_present waits until the main thread has finished the previous frame,
 * so two presents after the release every frame that could use the id has
 * been encoded (command buffers retain what they encode).  The Metal object
 * is dropped three presents later and only then is the id reused. */
#define TFQ 16384
static struct { unsigned id; uint32_t at; } g_tfq[TFQ];
static unsigned g_tfq_h = 0, g_tfq_n = 0;
static unsigned g_tfree[MAXTEX], g_ntfree = 0;
static unsigned long g_tex_released = 0, g_tfq_full = 0;
static void hd_tex_forget(unsigned id_);
/* Guest thread only (texture creation, metal_present). */
static unsigned tex_new_id(void){
    unsigned id_ = 0;
    if (g_ntfree) id_ = g_tfree[--g_ntfree];
    else if (g_texn < MAXTEX) id_ = g_texn++;
    if (!id_) return 0;
    g_tex_cube[id_] = 0; g_tex_lvlmask[id_] = 0; g_tex_isrt[id_] = 0; g_tex_freed[id_] = 0;
    hd_tex_forget(id_);
    return id_;
}
void metal_free_texture(unsigned id_){
    if (!id_ || id_ >= g_texn || id_ >= MAXTEX) return;
    if (g_tfq_n >= TFQ){ g_tfq_full++; return; }          /* keep it rather than risk a live id */
    /* a texture pack's replacement for this id goes with it (texrep.m, guest
     * thread like this call): the last id sharing a replacement frees its
     * memory; the encoder stops binding it from the next frame on */
    texrep_released(id_);
    g_tfq[(g_tfq_h + g_tfq_n) % TFQ].id = id_;
    g_tfq[(g_tfq_h + g_tfq_n) % TFQ].at = g_gframe;
    g_tfq_n++;
}
static void tex_free_due(int now){
    while (g_tfq_n){
        unsigned i = g_tfq_h;
        if (!now && g_gframe - g_tfq[i].at < 3u) break;
        unsigned id_ = g_tfq[i].id;
        g_tfq_h = (g_tfq_h + 1) % TFQ; g_tfq_n--;
        if (g_tex[id_]){ g_freed_w[id_] = (unsigned short)g_tex[id_].width; g_freed_h[id_] = (unsigned short)g_tex[id_].height; }
        g_tex[id_] = nil; g_tex_isrt[id_] = 0; g_tex_freed[id_] = 1; g_tex_released++;   /* freed_use() reports late users */
        g_tfree[g_ntfree++] = id_;
    }
}
unsigned long metal_tex_released(void){ return g_tex_released; }

/* D3DFMT -> MTLPixelFormat; returns the block size and bytes per block */
static MTLPixelFormat d3dfmt_to_mtl(unsigned f, unsigned *blk, unsigned *bpb){
    switch (f){
    case 21: case 22: *blk=1; *bpb=4; return MTLPixelFormatBGRA8Unorm;   /* A8R8G8B8 */
    case 28: *blk=1; *bpb=1; return MTLPixelFormatA8Unorm;               /* A8 */
    case 41: *blk=1; *bpb=1; return MTLPixelFormatR8Unorm;               /* L8 */
    case 0x31545844: *blk=4; *bpb=8;  return MTLPixelFormatBC1_RGBA;     /* DXT1 */
    case 0x33545844: *blk=4; *bpb=16; return MTLPixelFormatBC2_RGBA;     /* DXT3 */
    case 0x35545844: *blk=4; *bpb=16; return MTLPixelFormatBC3_RGBA;     /* DXT5 */
    default: *blk=0; *bpb=0; return MTLPixelFormatInvalid;
    }
}

unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt);
/* New texture contents are UNDEFINED: ALL mip levels and (for a cube) ALL
 * faces are cleared to transparent black.  Only level 0 of a 2D texture
 * used to be cleared, a cube never.  In the main menu the car environment
 * map (256/128 cube RT) is NEVER drawn but the body samples it (ps 265,
 * stage 0): once Reset releases the targets Metal can hand that memory to
 * new textures, and the cube carried the old texture's garbage (confirmed
 * with deliberate garbage: noise on the body). */
static void tex_zero_all(id<MTLTexture> t, unsigned w, unsigned h, unsigned levels,
                         unsigned faces, unsigned blk, unsigned bpb){
    if (ENVC("NOTEXZERO")) { if (faces == 1) levels = 1; else return; }  /* diagnostic: old behaviour */
    for (unsigned lv = 0; lv < levels; lv++){
        unsigned lw = w >> lv, lh = h >> lv;
        if (!lw) lw = 1; if (!lh) lh = 1;
        size_t row, rows;
        if (!blk || blk == 1){ row = (size_t)lw * (bpb ? bpb : 4); rows = lh; }
        else { row = (size_t)((lw + blk - 1) / blk) * bpb; rows = (lh + blk - 1) / blk; }
        void *zero = calloc(1, row * rows);
        if (!zero) return;
        for (unsigned f = 0; f < faces; f++)
            [t replaceRegion:MTLRegionMake2D(0,0,lw,lh) mipmapLevel:lv slice:f
                   withBytes:zero bytesPerRow:row bytesPerImage:0];
        free(zero);
    }
}
unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned d3dfmt){
    return metal_create_texture_ex(w, h, levels, d3dfmt, 0);
}
unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels,
                                 unsigned d3dfmt, unsigned is_rt){
    if (!g_dev || (g_texn >= MAXTEX && !g_ntfree)) return 0;
    unsigned blk, bpb;
    MTLPixelFormat pf = d3dfmt_to_mtl(d3dfmt, &blk, &bpb);
    if (pf == MTLPixelFormatInvalid) return 0;
    if (!levels) levels = 1;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf
                              width:w height:h mipmapped:(levels > 1)];
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead | (is_rt ? MTLTextureUsageRenderTarget : 0);
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    if (!t) return 0;
    /* New texture contents are undefined; clear to transparent black so
     * textures never uploaded do not show as garbage (e.g. render targets). */
    tex_zero_all(t, w, h, levels, 1, blk, bpb);
    unsigned id_ = tex_new_id();
    if (!id_) return 0;
    g_tex[id_] = t;
    g_tex_isrt[id_] = is_rt ? 1 : 0;
    atomic_fetch_add(&g_tex_created, 1); if (g_perflog) perflog_event(PL_TEX, 0, 0);
    return id_;
}
/* [status] mtl=/tex=: Metal heap usage and live texture count (heap.c). */
void metal_mem_stats(uint64_t *alloc_bytes, unsigned *live_tex, unsigned *tex_slots, uint64_t *tex_bytes){
    *alloc_bytes = g_dev ? (uint64_t)g_dev.currentAllocatedSize : 0;
    unsigned n = 0, top = g_texn < MAXTEX ? g_texn : MAXTEX;
    uint64_t b = 0;
    /* sizes only while texture lifetime is ours (guest thread); with
     * NFSU2_COMFREE=0 the main thread may drop render targets meanwhile */
    int com_frees(void); int sz = com_frees();
    for (unsigned i = 1; i < top; i++){
        if (g_tex[i] == nil) continue;
        n++;
        if (sz){ id<MTLTexture> t = g_tex[i]; b += t.allocatedSize; } }
    *live_tex = n; *tex_slots = top; *tex_bytes = b;
}
/* Guest (Reset): the render targets created so far are dead. */
void metal_reset_rts(void){
    if (ENVC("NORTFREE")) return;        /* diagnostic: turn releasing off */
    /* the created depth surfaces (ids 2..; 1 is the implicit one): D3D9 has
     * the game release every one before Reset, and com.c only recycles their
     * ids (a Release does not drop the Metal depth texture), so their
     * textures go here in both lifetime modes; a create after the Reset
     * clears its id again (metal_z_forget) */
    for (unsigned zid = 2; zid < MAXZ; zid++) atomic_store(&g_zdead[zid], 1);
    atomic_store(&g_zsweep_at, atomic_load(&g_frames_drawn) + 3);
    { int com_frees(void); if (com_frees()) return; }   /* v12na: Release frees them (metal_free_texture) */
    atomic_store(&g_rtfree_upto, g_texn);
    atomic_store(&g_rtfree_at, atomic_load(&g_frames_drawn) + 3);
}
static void z_sweep_dead(void);
/* Main thread, end of frame: release the dead RTs once their time has come. */
static void rt_free_pending(void){
    z_sweep_dead();
    unsigned upto = atomic_load(&g_rtfree_upto);
    if (upto <= g_rtfree_done || atomic_load(&g_frames_drawn) < atomic_load(&g_rtfree_at)) return;
    for (unsigned i = g_rtfree_done; i < upto && i < MAXTEX; i++)
        if (g_tex_isrt[i] && g_tex[i]){
            g_freed_w[i] = (unsigned short)g_tex[i].width; g_freed_h[i] = (unsigned short)g_tex[i].height;
            g_tex[i] = nil; g_tex_isrt[i] = 0; g_tex_freed[i] = 1; g_rt_freed++; }
    g_rtfree_done = upto;
}

/* ===== Optional graphics, every build (enhance.c)
 * MSAA on the screen passes (NFSU2_MSAA / settings.ini msaa) and the larger
 * mirror (NFSU2_MIRROR, size only: enhance.c).  This code came from the HD
 * build; everything is off by default, and off it adds
 * no pass, no attachment and no pipeline: the frame is the original's.  The
 * larger road reflection with its mip-based blur, SSR, ray tracing and the
 * larger cube map stay in the unshipped research build only (NFS_ENHANCED,
 * ./link.sh hd): measured, they are no improvement on the original look. */
#if NFS_ENHANCED
/* HD: road reflection targets (see enhance.c "ROAD REFLECTION BLUR").
 * They are created with a mip chain; before a blur draw samples one of these
 * textures, its mips are generated if it was dirtied since the last draw. */
static uint8_t  g_hd_road[MAXTEX], g_hd_dirty[MAXTEX];
static uint16_t g_hd_ow[MAXTEX],   g_hd_oh[MAXTEX];
static _Atomic unsigned long long g_hd_mipgen, g_hd_blurdraw;
void metal_hd_mark_road(unsigned id_, unsigned ow, unsigned oh){
    if (!id_ || id_ >= MAXTEX) return;
    g_hd_ow[id_] = (uint16_t)ow; g_hd_oh[id_] = (uint16_t)oh;
    g_hd_dirty[id_] = 1;
    g_hd_road[id_] = 1;
}
static int hd_is_road(unsigned t){ return t && t < MAXTEX && g_hd_road[t] && g_tex[t]; }
#else
/* com.c calls it for an enlarged road target, which only the research build makes */
void metal_hd_mark_road(unsigned id_, unsigned ow, unsigned oh){ (void)id_; (void)ow; (void)oh; }
#endif

/* HD: MSAA for the screen (HD_MSAA=4 default, 0/1 off).
 * Only the SCREEN passes are multisampled: the game draws its scene and UI
 * straight into the back buffer.  Render targets (reflection, post-process,
 * cube) stay single-sampled.  Every screen pass ends with
 * StoreAndMultisampleResolve, so the drawable that StretchRect reads from
 * the back buffer always holds the resolved image.  For Z, every surface
 * keeps a multisampled TWIN (contents are not shared; see the g_hd_zshare
 * counter). */
static int g_hd_msn = -1;
static id<MTLTexture> g_hd_mscol, g_hd_msdepth_scratch;
static id<MTLTexture> g_hd_msz[64];
/* --- MEMORYLESS MSAA ATTACHMENTS -------------------------------------------
 * Multisampled color / G-buffer / depth used to be Private always: at 4K
 * ~930 MB (color 133 + G 664 + Z 133).  Yet every measured frame has ONE
 * multisampled screen pass, and the color/G samples are never written to
 * memory, only resolved to single-sample (hd_ms_end).  On an Apple GPU such
 * an attachment can be MTLStorageModeMemoryless: it lives in tile memory and
 * no memory is allocated.  Condition: the attachment is NOT LOADED
 * (Clear/DontCare) and NOT STORED.  When a pass opens, hd_ms_memless_ok
 * looks ahead and decides; if the condition will not hold (split pass, copy
 * to screen) the pass opens with the Private twins -- which are now created
 * only when needed.  If the prediction failed, that is counted at close and
 * memoryless mode is switched off for good.  HD_MSMEMLESS=0 turns it off. */
static id<MTLTexture> g_hd_mscol_ml, g_hd_msz_ml;
static int g_hd_ml_off = 0;                    /* prediction failed: off for good */
static _Atomic unsigned long long g_hd_ml_pass, g_hd_ml_miss;
/* Whether MS depth is stored is decided at close (hd_ms_end).  If the last
 * multisampled pass of a Z did not store it, its samples are lost: the next
 * pass that wants to load without clearing opens with a clear and is counted. */
static uint8_t g_hd_msz_seen[64], g_hd_msz_kept[64];
static _Atomic unsigned long long g_hd_mszst, g_hd_mszskip, g_hd_mszlost;
/* open multisampled screen pass (main thread only): dz = MS depth bound and
 * its store action chosen at close; zclr = opened with a depth clear;
 * res = resolve to the single-sample twin bound */
static struct { int on, ml, dz, zclr, res; unsigned zid; } g_hd_msp;
/* pipeline that draws the screen copy into the multisampled texture, per
 * log2(samples): the descriptor is kept at startup, the state is built when
 * the count changes (MSAA BY RESOLUTION) */
static MTLRenderPipelineDescriptor *g_pso_ms_desc;
static id<MTLRenderPipelineState>   g_pso_msn[4];
static int hd_msaa_l2(void);
static id<MTLRenderPipelineState> hd_pso_ms(void){
    int l = hd_msaa_l2();
    if (!l || !g_pso_ms_desc) return nil;
    if (!g_pso_msn[l]){ NSError *e = nil;
        g_pso_ms_desc.rasterSampleCount = (NSUInteger)1u << l;
        g_pso_msn[l] = [g_dev newRenderPipelineStateWithDescriptor:g_pso_ms_desc error:&e]; }
    return g_pso_msn[l];
}
static uint8_t g_hd_zms_used[64];             /* used on screen this frame */
static uint8_t g_hd_zpend_ms[64];             /* the twin's pending Clear(Z) */
static float   g_hd_zpend_msv[64];
static _Atomic unsigned long long g_hd_mspass, g_hd_msblit, g_hd_zshare, g_hd_zrev, g_hd_zres;
static uint8_t g_hd_zsdirty[64];              /* Z written to the single-sample twin off screen */
/* main thread: drop the textures of depth surfaces released at a Reset (see g_zdead) */
static void z_sweep_dead(void){
    long at = atomic_load(&g_zsweep_at);
    if (!at || atomic_load(&g_frames_drawn) < at) return;
    atomic_store(&g_zsweep_at, 0);
    for (unsigned zid = 2; zid < MAXZ; zid++){
        if (!atomic_exchange(&g_zdead[zid], 0)) continue;
        if (g_ztex[zid]) g_z_swept++;
        g_ztex[zid] = nil;
        if (zid < 64u){ g_hd_msz[zid] = nil; g_hd_msz_seen[zid] = 0; g_hd_msz_kept[zid] = 0; }
    }
}
static int hd_msaa_n(void){
    if (g_hd_msn < 0){
        int n = enh_msaa_samples(NULL);       /* NFSU2_MSAA (HD build: HD_MSAA) */
        if (n != 2 && n != 4 && n != 8) n = 1;
        /* 8x is not supported on every Apple GPU (M4 Pro: 1, 2, 4): 4x, not off */
        while (n > 1 && g_dev && ![g_dev supportsTextureSampleCount:(NSUInteger)n]) n >>= 1;
        g_hd_msn = n;
        if (n > 1 || NFS_ENHANCED)
            fprintf(stderr, "[HD] MSAA: %s\n", n > 1 ? (n == 8 ? "8x" : n == 4 ? "4x" : "2x") : "OFF");
    }
    return g_hd_msn;
}
int metal_msaa_now(void){ return g_hd_msn > 1 ? g_hd_msn : 1; }   /* frame-rate counter */
/* --- MSAA BY RESOLUTION ----------------------------------------------------
 * 2x at 1440 rows and above, 4x below (user decision 2026-10-14: at 1440p and
 * at the Mac's own display resolution).  At this pixel density the edge gain
 * of 4x is small; its cost is the tile memory and resolve of the multisampled
 * scene pass (at 4K this pass is ~44% of GPU time; how much of it comes from
 * the sample count was not measured).  The screen size is checked every frame;
 * when the count changes, the MS textures (sample count check) and the
 * pipelines (log2(samples) in the key) renew themselves.  If HD_MSAA is given
 * the count is fixed; also with HD_MSLAZY=0 (its single pipelines are built
 * at startup). */
static int hd_mslazy(void);
static int hd_msaa_l2(void){ int n = hd_msaa_n(); return n >= 8 ? 3 : n >= 4 ? 2 : n >= 2 ? 1 : 0; }
static void hd_msaa_frame(NSUInteger w, NSUInteger h){
    static int aut = -1;
    if (aut < 0) (void)enh_msaa_samples(&aut);    /* msaa = auto */
    if (!aut || hd_msaa_n() <= 1 || !hd_mslazy()) return;
    int want = (h >= 1400u) ? 2 : 4;
    if (want == g_hd_msn || ![g_dev supportsTextureSampleCount:(NSUInteger)want]) return;
    g_hd_msn = want;
    fprintf(stderr, "[HD] MSAA: %dx (drawable %lux%lu; 2x at 1400 rows and above)\n",
            want, (unsigned long)w, (unsigned long)h);
}
static id<MTLTexture> hd_ms_tex(id<MTLTexture> old, MTLPixelFormat pf,
                                NSUInteger w, NSUInteger h){
    if (old && old.width == w && old.height == h && old.sampleCount == (NSUInteger)hd_msaa_n()) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.textureType = MTLTextureType2DMultisample;
    td.sampleCount = (NSUInteger)hd_msaa_n();
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:td];
}
static id<MTLTexture> hd_ms_tex_ml(id<MTLTexture> old, MTLPixelFormat pf,
                                   NSUInteger w, NSUInteger h){   /* see MEMORYLESS MSAA ATTACHMENTS */
    if (old && old.width == w && old.height == h && old.sampleCount == (NSUInteger)hd_msaa_n()) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.textureType = MTLTextureType2DMultisample;
    td.sampleCount = (NSUInteger)hd_msaa_n();
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeMemoryless;
    return [g_dev newTextureWithDescriptor:td];
}
static int hd_ml_hw(void){                     /* memoryless texture support + HD_MSMEMLESS */
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_MSMEMLESS");
        v = !(e && *e == '0') && g_dev && [g_dev supportsFamily:MTLGPUFamilyApple1]; }
    return v;
}

#if NFS_ENHANCED
/* ===== Research build only (./link.sh hd): SSR, ray-traced reflections ===== */
/* HD: the main view's camera, per FRAME (guest writes, main thread reads).
 * 48 floats: view (16), projection (16), product (16); D3D row-vector
 * layout (v' = v * M).  Source: overrides.c sub_005c7ec0. */
static float g_hd_cam[2][48];
static float g_hd_cam_k[2];                 /* Hor+ clip x factor */
static float g_hd_cube[2][9];               /* world directions of the cube's +x,+y,+z axes */
static int   g_hd_cam_ok[2];
void metal_hd_set_camera(const float *m48, float k, const float *cube9){
    pthread_mutex_lock(&g_qlock);
    memcpy(g_hd_cam[g_cur], m48, sizeof g_hd_cam[0]);
    memcpy(g_hd_cube[g_cur], cube9, sizeof g_hd_cube[0]);
    g_hd_cam_k[g_cur] = k;
    g_hd_cam_ok[g_cur] = 1;
    pthread_mutex_unlock(&g_qlock);
}

/* ===== HD: SSR (screen-space reflection) ==================================
 * Reflective surfaces (road: ps 5/6, which sample the road reflection
 * texture) write their own reflection weight B and the original reflection
 * R0 into the G-buffer during the main scene pass (vsemu.c ps_to_msl_ex2).
 * As soon as the scene ends (the first StretchRect from the screen) the
 * composite pass marches a ray through the depth buffer and, on a hit,
 * writes color + B*(R_new - R0).  Without a hit R0 stays (the game's planar
 * reflection: the right fallback for off-screen content).
 * Later R_new will come from ray tracing; G-buffer and composite stay the same.
 * Env: HD_SSR=0 off, HD_SSR_STEPS=48, HD_SSR_MAXD=120 (meters). */
static int g_hd_ssr = -1;
#define HD_NG 3   /* G-buffer: three targets, see G-BUFFER PACKING */
/* --- G-BUFFER PACKING ------------------------------------------------------
 * Old layout (HD_GPACK=0): B+class RGBA16F, R0 RGBA8, direction RGBA16F = 20 bytes.
 * Packed (default) 12 bytes, still three targets, all resolvable under MSAA:
 *   G0 RGBA8Unorm   R0.rgb + class/4.  Because the class stays in 8 bits, edge
 *                   averages are filtered out as before (GB_ROAD/GB_CAR).
 *   G1 RGB10A2Unorm B/4: range 0..4, step 0.004.  On the road B = texture
 *                   alpha x vertex alpha <= 1; car paint's was not
 *                   measured.  Above 4 it is clamped: the reflection weakens, not breaks.
 *   G2 RGB10A2Unorm unit direction, code = 512 + 511*d (512 = exact zero).  Linear
 *                   encoding preserves the average in the resolve: at an edge
 *                   a direction averaged with zero gets shorter but points the
 *                   same way (octahedral encoding would break that).
 * Measured (2026-10-14, GPU test without the game: the same random samples
 * written to both layouts at 2x and 4x, resolved by the hardware, read back
 * with these macros): class decisions identical; the B*(R_new - R0)
 * difference against the old layout at most 1 LSB (median 0.3); direction
 * difference on a uniform pixel at most 0.105 degrees.
 * Readers bind at the same indices (G0 2, G1 3, G2 4); decoding is in the
 * GB_* macros (hd_gb_src), the writer is vsemu.c ps_to_msl_ex2.  Gain:
 * multisampled tile memory for color + G per sample 24 -> 16 bytes; the
 * per-frame resolve writes, composite reads and single-sample G textures
 * are 40% smaller (119 -> 71 MB at 3024x1964). */
static int hd_gpack(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_GPACK"); v = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[HD] G-buffer: %s\n", v ? "packed (12 bytes/sample)" : "old (20 bytes/sample)"); }
    return v;
}
static MTLPixelFormat hd_gfmt(int gi){
    static const MTLPixelFormat old[HD_NG] = { MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm,
                                               MTLPixelFormatRGBA16Float };
    static const MTLPixelFormat pk[HD_NG]  = { MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGB10A2Unorm,
                                               MTLPixelFormatRGB10A2Unorm };
    return hd_gpack() ? pk[gi] : old[gi];
}
/* prepended to every shader that reads the G-buffer */
static NSString *kGBDec = @
"#if GB_PACK\n"
"#define GB_CLS(g0)    ((g0).a * 4.0)\n"
"#define GB_R0(g0, g1) ((g0).rgb)\n"
"#define GB_B(g0, g1)  ((g1).rgb * 4.0)\n"
"#define GB_DIR(g2)    (((g2).rgb * 1023.0 - 512.0) * (1.0 / 511.0))\n"
/* the rounding of the 8-bit class (<= 0.008) must not let edge averages in */
"#define GB_ROAD(c)    ((c) > 0.51 && (c) < 1.49)\n"
"#define GB_CAR(c)     ((c) > 1.76 && (c) < 2.24)\n"
"#else\n"
"#define GB_CLS(g0)    ((g0).a)\n"
"#define GB_R0(g0, g1) ((g1).rgb)\n"
"#define GB_B(g0, g1)  ((g0).rgb)\n"
"#define GB_DIR(g2)    ((g2).xyz)\n"
"#define GB_ROAD(c)    ((c) > 0.5 && (c) < 1.5)\n"
"#define GB_CAR(c)     ((c) > 1.75 && (c) < 2.25)\n"
"#endif\n";
static NSString *hd_gb_src(NSString *src){
    return [[(hd_gpack() ? @"#define GB_PACK 1\n" : @"#define GB_PACK 0\n")
             stringByAppendingString:kGBDec] stringByAppendingString:src];
}
static id<MTLTexture> g_hd_g[HD_NG], g_hd_gms[HD_NG], g_hd_gms_ml[HD_NG], g_hd_scenecopy;
static id<MTLRenderPipelineState> g_hd_ssr_pso, g_hd_ssr_pso_ms;
static uint8_t g_hd_vtried[MAXPS];
static _Atomic unsigned long long g_hd_ssr_n, g_hd_refl_draws;
static int hd_ssr_on(void){
    if (g_hd_ssr < 0){ const char *e = ENVC("HD_SSR");
        g_hd_ssr = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[HD] SSR: %s\n", g_hd_ssr ? "ON" : "OFF"); }
    return g_hd_ssr;
}
/* At draw time (guest thread): for a ps that draws to the screen and
 * samples the road reflection texture, switch to the separating variant. */
int metal_tex_is_cube(unsigned id_);
static int hd_refl_ps(int ps, unsigned target, const unsigned tx[4]){
    if (ps <= 0 || ps >= PSREFL || target || !hd_ssr_on()) return ps;
    int k = -1;
    for (int q = 0; q < 4; q++) if (hd_is_road(tx[q])){ k = q; break; }
    /* car paint: cube map in stage 0 (the ps is already the cube variant) */
    { static int car = -1;
      if (car < 0){ const char *e = ENVC("HD_SSR_CAR"); car = (e && *e == '0') ? 0 : 1; }
      if (k < 0 && car && ps >= PSCUBE && metal_tex_is_cube(tx[0])) k = 0; }
    if (k < 0) return ps;
    int vid = ps + PSREFL * (1 + k);
    if (vid >= MAXPS) return ps;
    if (!g_psfn[vid] && !g_hd_vtried[vid]){
        g_hd_vtried[vid] = 1;
        int com_ps_refl_variant(int, int, int, int);
        com_ps_refl_variant(ps % PSCUBE, ps >= PSCUBE, k, vid);
    }
    return g_psfn[vid] ? vid : ps;
}
/* 4x4 inverse, row-major.  0 on failure. */
static int hd_inv4(const float *m, float *o){
    float inv[16], det;
    inv[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
    return 1;
}
static id<MTLTexture> hd_tex(id<MTLTexture> old, MTLPixelFormat pf, NSUInteger w, NSUInteger h,
                            NSUInteger samples){
    if (old && old.width == w && old.height == h && old.sampleCount == samples) return old;
    MTLTextureDescriptor *td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    if (samples > 1){ td.textureType = MTLTextureType2DMultisample; td.sampleCount = samples;
                      td.usage = MTLTextureUsageRenderTarget; }
    else td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:td];
}
static NSString *kSSRSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct FSO { float4 pos [[position]]; };\n"
"vertex FSO hd_fs_vert(uint vid [[vertex_id]]) {\n"
"  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
"  FSO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
"struct SSRP { float4x4 vp; float4x4 ivp; float4 cam; float4 prm; float4 sz; float4 cam2;\n"
"              float4 cb0; float4 cb1; float4 cb2; };\n"
/* prm = (A, Bc, strength, max distance), sz = (W, H, step, thickness) */
"static float lin_z(float d, constant SSRP &S) { return S.prm.y / (d - S.prm.x); }\n"
"fragment float4 hd_ssr_frag(FSO in [[stage_in]],\n"
"    texture2d<float> scene [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g2 [[texture(3)]],\n"
"    texture2d<float> g3 [[texture(4)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  uint2 px = uint2(in.pos.xy);\n"
"  float4 c = scene.read(px);\n"
"  float4 gb = g1.read(px);\n"
"  int dbg = int(S.cam.w) - 1;                             /* 0 normal, 1 hit, 2 direction */\n"
"  float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);                                /* filter out edge averages */\n"
"  if (!road && !car) return dbg > 0 ? float4(c.rgb * 0.25, c.a) : c;\n"
"  float d = dep.read(px);\n"
"  if (d >= 1.0) return c;\n"
"  float2 uv = (float2(px) + 0.5) / S.sz.xy;\n"
"  float4 wp = S.ivp * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
"  float3 P = wp.xyz / wp.w;\n"
"  float3 V = normalize(P - S.cam.xyz);\n"
"  float3 Rd; float maxd = S.prm.w; float lod = 0.0; float dt = 0.25;\n"
"  if (road) Rd = reflect(V, float3(0.0, 0.0, 1.0));\n"
"  else {\n"
"    float3 gd = GB_DIR(g3.read(px));\n"
"    if (dot(gd, gd) < 1e-6) return c;\n"
"    Rd = normalize(gd.x * S.cb0.xyz + gd.y * S.cb1.xyz + gd.z * S.cb2.xyz);\n"
"    if (dbg == 2) return float4(Rd * 0.5 + 0.5, 1.0);\n"
/* N = normalize(R - V): step slightly off the surface, do not hit itself */
"    float3 N = normalize(Rd - V);\n"
"    if (dbg == 3) {\n"                 /* does N agree with the geometric normal from depth? */
"      float2 u1 = (float2(px) + float2(1.5, 0.5)) / S.sz.xy, u2 = (float2(px) + float2(0.5, 1.5)) / S.sz.xy;\n"
"      float4 w1 = S.ivp * float4(u1.x * 2.0 - 1.0, 1.0 - u1.y * 2.0, dep.read(px + uint2(1, 0)), 1.0);\n"
"      float4 w2 = S.ivp * float4(u2.x * 2.0 - 1.0, 1.0 - u2.y * 2.0, dep.read(px + uint2(0, 1)), 1.0);\n"
"      float3 Ng = normalize(cross(w1.xyz / w1.w - P, w2.xyz / w2.w - P));\n"
"      if (dot(Ng, V) > 0.0) Ng = -Ng;\n"
"      float a = dot(N, Ng);\n"
"      return float4(a < 0.3 ? 1.0 : 0.0, a > 0.7 ? 1.0 : 0.0, (a >= 0.3 && a <= 0.7) ? 1.0 : 0.0, 1.0);\n"
"    }\n"
"    if (dot(N, V) > -0.02) return dbg > 0 ? float4(0.0, 0.0, 0.3, 1.0) : c;\n"
"    P += N * 0.04;\n"
"    maxd = min(maxd, 60.0); lod = S.cam2.x; dt = 0.08;\n"
"  }\n"
"  if (road && Rd.z <= 0.0) return c;\n"
"  constexpr sampler ls(filter::linear, mip_filter::linear, address::clamp_to_edge);\n"
"  int steps = int(S.sz.z);\n"
"  float t = 0.0, tprev = 0.0;\n"
"  float2 huv = float2(-1.0); float conf = 0.0;\n"
"  for (int i = 0; i < steps; i++) {\n"
"    tprev = t; t += dt; dt *= 1.12;\n"
"    if (t > maxd) break;\n"
"    float4 q = S.vp * float4(P + Rd * t, 1.0);\n"
"    if (q.w <= 0.6) break;\n"
"    float2 suv = float2(q.x / q.w * 0.5 + 0.5, 0.5 - q.y / q.w * 0.5);\n"
"    if (any(suv < 0.0) || any(suv > 1.0)) break;\n"
"    float zs = lin_z(dep.read(uint2(suv * S.sz.xy)), S);\n"
"    float diff = q.w - zs;\n"
"    if (diff > 0.02) {\n"
"      if (diff < max(S.sz.w, dt * 1.5)) {\n"
"        float a = tprev, b = t;\n"
"        for (int j = 0; j < 6; j++) {\n"
"          float m = 0.5 * (a + b);\n"
"          float4 qm = S.vp * float4(P + Rd * m, 1.0);\n"
"          float2 mu = float2(qm.x / qm.w * 0.5 + 0.5, 0.5 - qm.y / qm.w * 0.5);\n"
"          float zm = lin_z(dep.read(uint2(clamp(mu, 0.0, 0.9999) * S.sz.xy)), S);\n"
"          if (qm.w - zm > 0.0) b = m; else a = m;\n"
"          suv = mu; }\n"
"        float2 e = min(suv, 1.0 - suv);\n"
"        conf = smoothstep(0.0, 0.08, min(e.x, e.y)) * (1.0 - smoothstep(0.6, 1.0, t / maxd));\n"
"        huv = suv;\n"
"      }\n"
"      break;\n"
"    }\n"
"  }\n"
"  if (dbg == 1) return float4(road ? 0.3 + 0.7 * saturate(length(GB_B(gb, g2.read(px)))) : 0.0, conf,\n"
"                              conf > 0.0 ? (road ? 0.0 : 1.0) : 0.6, 1.0);\n"
"  if (conf <= 0.0) return c;\n"
"  float4 gv = g2.read(px);\n"
"  float3 R0 = GB_R0(gb, gv);\n"
"  float3 Rn = mix(R0, scene.sample(ls, huv, level(lod)).rgb, conf * S.prm.z);\n"
"  return float4(c.rgb + GB_B(gb, gv) * (Rn - R0), c.a);\n"
"}\n";
/* ===== HD: RAY TRACING TIMING PROBE (HD_RTPROBE=1, on the DUMPFRAME frame) =====
 * Question: on this machine, HOW LONG does it take to put the scene into a
 * ray tracing structure every frame and cast reflection rays?  Runs once,
 * prints a report, writes two PNGs.
 *   1) Capture (CPU): the main scene's draws into world space.  The vertex
 *      shader is NOT RUN: since oPos = p * WVP,
 *      world = p * (WVP * (VP')^-1); WVP is the draw's c[cx..cx+3] constants.
 *   2) Build (GPU): the whole scene as one primitive acceleration structure;
 *      default build, fast build and refit timed separately.
 *   3) Trace (GPU): primary rays (all pixels; a verification image) and
 *      reflection rays (only the pixels that reflect in the G-buffer).      */
#define HD_MAXVSI 512
static struct { int16_t po, pty, cx, fogr, litr; uint8_t skin, ok; float ramp[2]; } g_hd_vsi[HD_MAXVSI];
void metal_hd_vs_info(int gid, int po, int pty, int cx, int skinned, int fogr, int litr, float ramp0, float ramp1){
    if (gid <= 0 || gid >= HD_MAXVSI) return;
    g_hd_vsi[gid].po = (int16_t)po; g_hd_vsi[gid].pty = (int16_t)pty;
    g_hd_vsi[gid].cx = (int16_t)cx; g_hd_vsi[gid].skin = skinned ? 1 : 0;
    g_hd_vsi[gid].fogr = (int16_t)fogr;
    g_hd_vsi[gid].litr = (int16_t)litr; g_hd_vsi[gid].ramp[0] = ramp0; g_hd_vsi[gid].ramp[1] = ramp1;
    g_hd_vsi[gid].ok = 1;
}
static NSString *kRTSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct RTP { float4x4 ivp; float4 cam; float4 cb0; float4 cb1; float4 cb2; float4 sz; };\n"
/* sz = (W, H, mode, -) ; mode 0 = primary, 1 = reflection */
"kernel void hd_rt_probe(uint2 tid [[thread_position_in_grid]],\n"
"    texture2d<float, access::write> outp [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g3 [[texture(3)]],\n"
"    primitive_acceleration_structure acc [[buffer(0)]], constant RTP &S [[buffer(1)]]) {\n"
"  if (tid.x >= uint(S.sz.x) || tid.y >= uint(S.sz.y)) return;\n"
"  float2 uv = (float2(tid) + 0.5) / S.sz.xy;\n"
"  float2 nd = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
"  ray r; r.min_distance = 0.02; r.max_distance = 3000.0;\n"
"  float tag = 0.0;\n"
"  if (S.sz.z < 0.5) {\n"
"    float4 w = S.ivp * float4(nd, 0.5, 1.0);\n"
"    r.origin = S.cam.xyz; r.direction = normalize(w.xyz / w.w - S.cam.xyz);\n"
"  } else {\n"
"    float4 gb = g1.read(tid); float cls = GB_CLS(gb);\n"
"    bool road = GB_ROAD(cls), car = GB_CAR(cls);\n"
"    float d = dep.read(tid);\n"
"    if ((!road && !car) || d >= 1.0) { outp.write(float4(0.0), tid); return; }\n"
"    float4 w = S.ivp * float4(nd, d, 1.0);\n"
"    float3 P = w.xyz / w.w; float3 V = normalize(P - S.cam.xyz); float3 N = float3(0.0, 0.0, 1.0);\n"
"    float3 Rd;\n"
"    if (road) Rd = reflect(V, N);\n"
"    else { float3 gd = GB_DIR(g3.read(tid));\n"
"           Rd = normalize(gd.x * S.cb0.xyz + gd.y * S.cb1.xyz + gd.z * S.cb2.xyz); N = normalize(Rd - V); }\n"
"    r.origin = P + N * 0.05; r.direction = Rd; tag = road ? 0.5 : 1.0;\n"
"  }\n"
"  intersector<triangle_data> it;\n"
"  it.assume_geometry_type(geometry_type::triangle);\n"
"  it.force_opacity(forced_opacity::opaque);\n"
"  intersection_result<triangle_data> h = it.intersect(r, acc);\n"
"  if (h.type == intersection_type::none) { outp.write(float4(0.0, 0.0, tag > 0.0 ? 0.25 : 0.0, tag > 0.0 ? 1.0 : 0.5), tid); return; }\n"
/* light shading from distance + triangle id: makes the geometry readable */
"  float sh = 1.0 / (1.0 + 0.02 * h.distance);\n"
"  float hsh = float((h.primitive_id * 2654435761u) >> 24) / 255.0;\n"
"  outp.write(float4(sh * (0.6 + 0.4 * hsh), sh * (0.7 + 0.3 * h.triangle_barycentric_coord.x), sh, 1.0), tid);\n"
"}\n";
static double hd_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
                            return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }
static void hd_rt_probe(int rb, id<MTLTexture> ref, long frame){
    static int done = 0; if (done) return; done = 1;
    fprintf(stderr, "\n[RT] ===== ray tracing timing (frame %ld) =====\n", frame);
    if (!g_dev.supportsRaytracing){ fprintf(stderr, "[RT] the device does not support ray tracing\n"); return; }
    if (!g_hd_cam_ok[rb]){ fprintf(stderr, "[RT] no camera\n"); return; }
    float vp[16], ivp[16], iv[16];
    memcpy(vp, &g_hd_cam[rb][32], sizeof vp);
    for (int r4 = 0; r4 < 4; r4++) vp[r4*4] *= g_hd_cam_k[rb];
    if (!hd_inv4(vp, ivp) || !hd_inv4(&g_hd_cam[rb][0], iv)) return;

    /* --- 1) capture --- */
    double t0 = hd_now();
    size_t cap = 1u << 20, nv = 0;
    float *V = (float *)malloc(cap * 12);
    unsigned nd = 0, sk_vs = 0, sk_skin = 0, sk_ortho = 0, sk_nz = 0;
    for (unsigned i = 0; i < g_dn[rb]; i++){
        MDraw *dr = &g_dbuf[rb][i];
        if (dr->is_blit && !dr->blit_src) break;            /* end of scene */
        if (dr->target || dr->is_clear || dr->is_blit || dr->count < 3) continue;
        if (dr->vsid <= 0 || dr->vsid >= HD_MAXVSI || !g_hd_vsi[dr->vsid].ok ||
            g_hd_vsi[dr->vsid].po < 0 || g_hd_vsi[dr->vsid].cx < 0 ||
            (g_hd_vsi[dr->vsid].pty != 2 && g_hd_vsi[dr->vsid].pty != 3)){ sk_vs++;
            { static uint8_t seen[HD_MAXVSI];
              if (dr->vsid > 0 && dr->vsid < HD_MAXVSI && !seen[dr->vsid]){ seen[dr->vsid] = 1;
                  fprintf(stderr, "[RT]   skipped vs_%d: info=%d position offset=%d type=%d matrix register=%d (ps %d, %u verts)\n",
                          dr->vsid, g_hd_vsi[dr->vsid].ok, g_hd_vsi[dr->vsid].po, g_hd_vsi[dr->vsid].pty,
                          g_hd_vsi[dr->vsid].cx, dr->ps, dr->count); } }
            continue; }
        if (g_hd_vsi[dr->vsid].skin){ sk_skin++; continue; }
        if (!(dr->depth & 2u)){ sk_nz++; continue; }        /* no Z write: lights, particles */
        const uint8_t *kb = g_kbuf[rb] + dr->koff;
        unsigned stride; memcpy(&stride, kb, 4);
        const float *k = (const float *)(kb + 16);
        int cx = g_hd_vsi[dr->vsid].cx;
        if ((unsigned)(cx + 4) * 16u > dr->ksz - 16u || !stride){ sk_vs++; continue; }
        const float *c3 = k + (cx + 3) * 4;
        if (c3[0] == 0.f && c3[1] == 0.f && c3[2] == 0.f){ sk_ortho++; continue; }
        /* M[r][c] = c[cx+c][r];  W = M * ivp  (double precision) */
        double W[16];
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++){
            double a = 0; for (int q = 0; q < 4; q++) a += (double)k[(cx + q) * 4 + r] * (double)ivp[q * 4 + c];
            W[r * 4 + c] = a; }
        if (nv + dr->count > cap){ while (nv + dr->count > cap) cap *= 2; V = (float *)realloc(V, cap * 12); }
        unsigned po = (unsigned)g_hd_vsi[dr->vsid].po;
        for (unsigned j = 0; j < dr->count; j++){
            float p[3]; memcpy(p, dr_vtx(rb, dr, j, stride) + po, 12);
            double x = p[0]*W[0] + p[1]*W[4] + p[2]*W[8]  + W[12];
            double y = p[0]*W[1] + p[1]*W[5] + p[2]*W[9]  + W[13];
            double z = p[0]*W[2] + p[1]*W[6] + p[2]*W[10] + W[14];
            double w = p[0]*W[3] + p[1]*W[7] + p[2]*W[11] + W[15];
            float *o = V + (nv + j) * 3;
            o[0] = (float)(x / w); o[1] = (float)(y / w); o[2] = (float)(z / w);
        }
        nv += dr->count - dr->count % 3; nd++;
    }
    double t_cap = hd_now() - t0;
    size_t ntri = nv / 3;
    fprintf(stderr, "[RT] capture: %u draws, %zu triangles, CPU %.2f ms  (skipped: vs %u, skinned %u, orthographic %u, no Z write %u)\n",
            nd, ntri, t_cap, sk_vs, sk_skin, sk_ortho, sk_nz);
    if (ntri < 16){ free(V); return; }
    { float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
      for (size_t q = 0; q < nv; q++) for (int a = 0; a < 3; a++){ float v = V[q*3+a];
          if (v < mn[a]) mn[a] = v; if (v > mx[a]) mx[a] = v; }
      fprintf(stderr, "[RT] world box: x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]  camera (%.1f %.1f %.1f)\n",
              mn[0],mx[0],mn[1],mx[1],mn[2],mx[2], iv[12],iv[13],iv[14]); }

    @autoreleasepool {
    id<MTLBuffer> vb = [g_dev newBufferWithBytes:V length:nv * 12 options:MTLResourceStorageModeShared];
    free(V);
    /* --- 2) build --- */
    id<MTLAccelerationStructure> acc = nil;
    static const char *un[3] = { "default build", "fast build (PreferFastBuild)", "refit" };
    for (int u = 0; u < 3; u++){
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = vb; gd.vertexStride = 12; gd.triangleCount = ntri; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd];
        ad.usage = (u == 1) ? MTLAccelerationStructureUsagePreferFastBuild
                 : (u == 2) ? MTLAccelerationStructureUsageRefit : MTLAccelerationStructureUsageNone;
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:ad];
        id<MTLAccelerationStructure> a2 = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        NSUInteger ss = sz.buildScratchBufferSize > sz.refitScratchBufferSize ? sz.buildScratchBufferSize : sz.refitScratchBufferSize;
        id<MTLBuffer> scr = [g_dev newBufferWithLength:ss ? ss : 16 options:MTLResourceStorageModePrivate];
        double best = 1e30, sum = 0; int N = 6;
        for (int it = 0; it < N; it++){
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> ae = [cb accelerationStructureCommandEncoder];
            if (u == 2 && it > 0)
                [ae refitAccelerationStructure:a2 descriptor:ad destination:a2 scratchBuffer:scr scratchBufferOffset:0];
            else
                [ae buildAccelerationStructure:a2 descriptor:ad scratchBuffer:scr scratchBufferOffset:0];
            [ae endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
            if (u == 2 && it == 0) continue;               /* first round is the build */
            if (it == 0) continue;                         /* warm-up */
            if (ms < best) best = ms; sum += ms;
        }
        fprintf(stderr, "[RT] %-34s: best %.2f ms, avg %.2f ms  (structure %.1f MB)\n", un[u], best, sum / (N - 1),
                sz.accelerationStructureSize / 1048576.0);
        if (u == 0) acc = a2;
    }
    /* --- 3) trace --- */
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:hd_gb_src(kRTSrc) options:nil error:&e];
    if (!lib){ fprintf(stderr, "[RT] kernel did not compile: %s\n", [[e localizedDescription] UTF8String]); return; }
    id<MTLComputePipelineState> cps = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"hd_rt_probe"] error:&e];
    if (!cps){ fprintf(stderr, "[RT] compute pipeline could not be built: %s\n", [[e localizedDescription] UTF8String]); return; }
    NSUInteger Wd = ref.width, Hd = ref.height;
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                     width:Wd height:Hd mipmapped:NO];
    od.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModeShared;
    id<MTLTexture> outp = [g_dev newTextureWithDescriptor:od];
    id<MTLTexture> zt = g_ztex[1];
    int have_g = g_hd_g[0] && g_hd_g[2] && zt && zt.width == Wd && zt.height == Hd && g_hd_g[0].width == Wd;
    struct { float ivp[16], cam[4], cb[3][4], sz[4]; } P;
    memcpy(P.ivp, ivp, sizeof ivp);
    P.cam[0] = iv[12]; P.cam[1] = iv[13]; P.cam[2] = iv[14]; P.cam[3] = 1.f;
    for (int a3 = 0; a3 < 3; a3++){ memcpy(P.cb[a3], &g_hd_cube[rb][a3*3], 12); P.cb[a3][3] = 0.f; }
    P.sz[0] = (float)Wd; P.sz[1] = (float)Hd; P.sz[3] = 0.f;
    static const char *mn2[2] = { "primary ray (all pixels)", "reflection ray (road + car)" };
    for (int mode = 0; mode < 2; mode++){
        if (mode == 1 && !have_g){ fprintf(stderr, "[RT] reflection mode skipped: no G-buffer (HD_SSR must be on)\n"); break; }
        P.sz[2] = (float)mode;
        double best = 1e30, sum = 0; int N = 8;
        for (int it = 0; it < N; it++){
            id<MTLCommandBuffer> cb = [g_queue commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:cps];
            [ce setTexture:outp atIndex:0];
            [ce setTexture:(have_g ? zt : nil) atIndex:1];
            [ce setTexture:(have_g ? g_hd_g[0] : nil) atIndex:2];
            [ce setTexture:(have_g ? g_hd_g[2] : nil) atIndex:3];
            [ce setAccelerationStructure:acc atBufferIndex:0];
            [ce setBytes:&P length:sizeof P atIndex:1];
            NSUInteger tw = cps.threadExecutionWidth, th = cps.maxTotalThreadsPerThreadgroup / tw;
            [ce dispatchThreadgroups:MTLSizeMake((Wd + tw - 1) / tw, (Hd + th - 1) / th, 1)
               threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
            [ce endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            double ms = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
            if (it == 0) continue;
            if (ms < best) best = ms; sum += ms;
        }
        /* ray count: the pixels with alpha > 0.75 traced a ray */
        size_t rays = 0, hits = 0;
        { uint8_t *px = (uint8_t *)malloc(Wd * Hd * 4);
          [outp getBytes:px bytesPerRow:Wd * 4 fromRegion:MTLRegionMake2D(0, 0, Wd, Hd) mipmapLevel:0];
          for (size_t q = 0; q < Wd * Hd; q++){
              if (mode == 0){ rays++; if (px[q*4+3] > 200) hits++; }
              else if (px[q*4+3] > 200){ rays++; if (px[q*4+2] > 80 || px[q*4+1] > 8) hits++; } }
          free(px); }
        fprintf(stderr, "[RT] %-34s: best %.2f ms, avg %.2f ms  (%zu rays, %.0f%% hits, %.0f M rays/s)\n",
                mn2[mode], best, sum / (N - 1), rays, rays ? 100.0 * hits / rays : 0.0,
                best > 0 ? rays / best / 1000.0 : 0.0);
        dump_texture(outp, 99001 + mode);
    }
    fprintf(stderr, "[RT] ===== end =====\n\n");
    }
}

/* ===== HD: RAY-TRACED REFLECTION (HD_RT=0 turns it off -> falls back to SSR) =====
 * Every frame, as soon as the main scene ends (in place of the SSR composite):
 *   1) CAPTURE (GPU): for every draw of the scene the game's OWN vertex
 *      shader runs once more as a compute kernel (vsemu.c vsk_N) and writes
 *      world position + color + texture coordinates + fog to a buffer.  No
 *      draw is skipped: road, skinned models, all of them.
 *   2) ACCELERATION STRUCTURE: rebuilt every frame from all triangles
 *      (measured: 1.7 ms for ~19k triangles).
 *   3) REFLECTION: a ray for road and car pixels; on a hit the math of the
 *      game's pixel shader (ps 2/10: 2*texture*color; ps 4/6: with the
 *      headlight term; car: body texture), the alpha test and fog are applied.
 *      Without a hit the game's own reflection (R0) stays.
 * Material textures are sampled through a bindless table (argument buffer). */
#define RT_MAXV   (600u * 1024u)     /* per-frame capture: vertices */
#define RT_SMAXV  (480u * 1024u)     /* static store: vertices */
#define RT_SMAXE  8192u              /* static materials */
#define RT_MAXREC 8192u
#define RT_MAXTEX 512u
typedef struct { uint32_t rawoff, koff, first, mat, ib; int32_t base; uint32_t isz, pad; } HDRecC;
typedef struct { uint32_t tex0, tex1, tex2, flags; float c0[4]; float fogc[4]; float prm[4]; float lw[16]; } HDMatC;
static int g_rt = -1;
static id<MTLComputePipelineState> g_hd_vsk[HD_MAXVSI];
static id<MTLBuffer> g_rt_pos, g_rt_attr, g_rt_scratch, g_rt_item[3], g_rt_rec[3], g_rt_mat[3], g_rt_arg[3];
/* --- INSTANCE STRUCTURE: IN-PLACE UPDATE (refit) --------------------------
 * The structure of the moving geometry used to be rebuilt from scratch every
 * frame.  If the same draws as in the previous frame arrive in the same
 * order with the same triangle counts, the triangle layout is the same; only
 * the positions change.  Then the tree is not rebuilt, its boxes are fitted
 * to the new positions (refit, a small fraction of a build).  Since motion
 * loosens the tree over time, it is still rebuilt from scratch every
 * RT_REFIT_MAX frames.  HD_RT_REFIT=0 is the old behavior. */
#define RT_REFIT_MAX 20u
static uint64_t g_rt_dsig;                      /* signature of the last built/refitted layout */
static uint32_t g_rt_dsince = 0xFFFFFFFFu;      /* frames since the last full build */
static _Atomic unsigned long long g_rt_nbuild, g_rt_nrefit;
static int hd_rt_refit_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_RT_REFIT"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static MTLAccelerationStructureUsage hd_rt_dyn_usage(void){
    return MTLAccelerationStructureUsagePreferFastBuild | (hd_rt_refit_on() ? MTLAccelerationStructureUsageRefit : 0);
}
static id<MTLBuffer> g_rt_sposb[2], g_rt_sattrb[2], g_rt_sscratch, g_rt_tscratch, g_rt_inst;
static unsigned g_rt_sbuf;           /* active static buffer pair (switches on compaction) */
#define g_rt_spos  g_rt_sposb[g_rt_sbuf]
#define g_rt_sattr g_rt_sattrb[g_rt_sbuf]
static id<MTLAccelerationStructure> g_rt_as, g_rt_sas, g_rt_tlas;
static int g_rt_cache = 1, g_rt_only_static;
static id<MTLRenderPipelineState> g_rt_pso, g_rt_pso_ms;
static id<MTLComputePipelineState> g_rt_kern;
static id<MTLTexture> g_rt_out;
static id<MTLArgumentEncoder> g_rt_argenc;
static unsigned g_rt_ring;
static id<MTLTexture> g_rt_texs[RT_MAXTEX];
static uint32_t g_rt_tstamp[MAXTEX]; static uint16_t g_rt_tslot[MAXTEX]; static uint32_t g_rt_stamp;
static _Atomic unsigned long long g_rt_frames, g_rt_tris, g_rt_draws, g_rt_skipk, g_rt_over;
static int hd_rt_on(void){
    if (g_rt < 0){ const char *e = ENVC("HD_RT");
        g_rt = (e && *e == '0') ? 0 : 1;
        if (g_rt && g_dev && !g_dev.supportsRaytracing) g_rt = 0;
        if (@available(macOS 13.0, *)) {} else g_rt = 0;      /* acceleration structure in a render pass */
        fprintf(stderr, "[HD] ray-traced reflections: %s\n", g_rt ? "ON" : "OFF"); }
    return g_rt;
}
static id<MTLComputePipelineState> g_rt_fixk;   /* vertex check (HD_RT_FIX=0 turns it off) */
static id<MTLBuffer> g_rt_fixcnt;               /* [0] NaN/infinite triangles, [1] far-off triangles (totals) */
static NSString *kRTFSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"using namespace raytracing;\n"
"struct FSO { float4 pos [[position]]; };\n"
"vertex FSO hd_fs_vert(uint vid [[vertex_id]]) {\n"
"  float2 p = float2(float((vid << 1) & 2), float(vid & 2));\n"
"  FSO o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
"struct SSRP { float4x4 vp; float4x4 ivp; float4 cam; float4 prm; float4 sz; float4 cam2;\n"
"              float4 cb0; float4 cb1; float4 cb2; float4 fogp; };\n"
"struct HDMat { uint tex0; uint tex1; uint tex2; uint flags; float4 c0; float4 fogc; float4 prm;\n"
"               float4 l0; float4 l1; float4 l2; float4 l3; };\n"
"struct HDAttr { float4 col0; float4 uv1; float4 uv2; float2 uv0; float fog; float pad; };\n"
"struct HDTex { array<texture2d<float>, 512> t; };\n"
"static float2 hd_proj(float4 q, uint mode) {\n"
"  float d = mode == 2u ? q.w : (mode == 1u ? q.z : 1.0);\n"
"  return q.xy / (abs(d) < 1e-6 ? 1e-6 : d); }\n"
/* The rays are cast in a COMPUTE kernel.  The same code in a fragment shader
 * measured GPU ~14 ms/frame (the game became GPU-bound, ~74 fps); ~2 ms as a kernel. */
"kernel void hd_rt_kern(uint2 tid0 [[thread_position_in_grid]],\n"
"    texture2d<float, access::write> outp [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]], texture2d<float> g3 [[texture(4)]],\n"
"    constant SSRP &S [[buffer(0)]], instance_acceleration_structure acc [[buffer(1)]],\n"
"    device const HDMat *MAT [[buffer(3)]],\n"
"    device const HDAttr *ATT [[buffer(4)]], constant HDTex &TX [[buffer(6)]],\n"
"    device const HDAttr *SATT [[buffer(7)]]) {\n"
/* S.cb2.w = 0 full; 1/2 = that frame's half of the checkerboard (see HALF RAYS) */
"  uint par = uint(S.cb2.w + 0.5);\n"
"  uint2 tid = par == 0u ? tid0 : uint2(tid0.x * 2u + ((tid0.y + par) & 1u), tid0.y);\n"
/* S.cb0.w = scale (1 or 2): output (W/scale, H/scale); each output pixel stands
 * for pixel (tid * scale) of the full-resolution G-buffer. */
"  uint sc = uint(max(S.cb0.w, 1.0));\n"
"  uint2 px = tid * sc;\n"
"  if (px.x >= uint(S.sz.x) || px.y >= uint(S.sz.y)) return;\n"
"  float4 gb = g1.read(px); float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);\n"
"  float d = dep.read(px);\n"
"  if ((!road && !car) || d >= 1.0) { outp.write(float4(0.0), tid); return; }\n"
"  float2 uv = (float2(px) + 0.5) / S.sz.xy;\n"
"  float4 wp = S.ivp * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);\n"
"  float3 P = wp.xyz / wp.w;\n"
"  float3 V = normalize(P - S.cam.xyz);\n"
"  float3 Rd, N = float3(0.0, 0.0, 1.0);\n"
"  if (road) { Rd = reflect(V, N); if (Rd.z <= 0.0) { outp.write(float4(0.0), tid); return; } }\n"
"  else {\n"
"    float3 gd = GB_DIR(g3.read(px));\n"
"    if (dot(gd, gd) < 1e-6) { outp.write(float4(0.0), tid); return; }\n"
"    Rd = normalize(gd.x * S.cb0.xyz + gd.y * S.cb1.xyz + gd.z * S.cb2.xyz);\n"
"    N = normalize(Rd - V);\n"
"    if (dot(N, V) > -0.02) { outp.write(float4(0.0), tid); return; }\n"
"  }\n"
"  constexpr sampler rs(filter::linear, mip_filter::linear, address::repeat);\n"
"  constexpr sampler cs(filter::linear, address::clamp_to_edge);\n"
"  float maxd = S.cam2.y;\n"
"  float3 org = P + N * 0.05;\n"
"  float tot = 0.0, conf = 0.0; float3 rc = float3(0.0);\n"
"  intersector<triangle_data, instancing> isec;\n"
"  isec.assume_geometry_type(geometry_type::triangle);\n"
"  isec.force_opacity(forced_opacity::opaque);\n"
"  for (int k = 0; k < 5; k++) {\n"
"    ray r; r.origin = org; r.direction = Rd; r.min_distance = 0.01; r.max_distance = maxd - tot;\n"
"    intersection_result<triangle_data, instancing> h = isec.intersect(r, acc);\n"
"    if (h.type == intersection_type::none) break;\n"
"    uint g0 = h.primitive_id * 3u;\n"
/* instance 0 = per-frame capture (mask 1), instance 1 = static store (mask 2; S.cb1.w: static only) */
"    bool st = h.instance_id == 1u || S.cb1.w > 0.5;\n"
"    tot += h.distance;\n"
"    if (S.cam2.w > 1.5) { rc = float3(1.0 / (1.0 + 0.02 * tot)); conf = 1.0; break; }\n"
"    float2 b = h.triangle_barycentric_coord; float w0 = 1.0 - b.x - b.y;\n"
"    HDAttr a0, a1, a2;\n"
"    if (st) { a0 = SATT[g0]; a1 = SATT[g0 + 1u]; a2 = SATT[g0 + 2u]; }\n"
"    else    { a0 = ATT[g0];  a1 = ATT[g0 + 1u];  a2 = ATT[g0 + 2u]; }\n"
"    HDMat m = MAT[uint(a0.pad + 0.5)];\n"
"    float4 col = a0.col0 * w0 + a1.col0 * b.x + a2.col0 * b.y;\n"
"    float4 uv1 = a0.uv1 * w0 + a1.uv1 * b.x + a2.uv1 * b.y;\n"
"    float2 uv0 = a0.uv0 * w0 + a1.uv0 * b.x + a2.uv0 * b.y;\n"
"    float fog = a0.fog * w0 + a1.fog * b.x + a2.fog * b.y;\n"
/* static store: the stored fog is relative to the camera at promotion time;
 * recompute it with the game's formula (vs_4: fog = clamp((end - depth) * scale, min, 1)) */
"    if ((m.flags & 0x10000u) != 0u) {\n"
"      float3 hp = org + Rd * h.distance;\n"
"      float zv = (S.vp * float4(hp, 1.0)).w;\n"
"      fog = S.fogp.w > 0.5 ? clamp((S.fogp.x - zv) * S.fogp.y, S.fogp.z, 1.0) : 1.0;\n"
"    }\n"
"    bool mcar = (m.flags & 2u) != 0u;\n"
"    float lod = clamp(log2(1.0 + tot * S.cam2.z), 0.0, 5.0);\n"
"    float4 tc = S.cam2.w > 0.5 ? float4(1.0) : TX.t[mcar ? m.tex1 : m.tex0].sample(rs, mcar ? uv1.xy : uv0, level(lod));\n"
/* alpha test (trees, fences): the ray continues through the surface */
"    if ((m.flags & 1u) != 0u && tc.a * col.a < m.prm.x) { org += Rd * (h.distance + 0.02); continue; }\n"
"    float3 lit = col.rgb;\n"
/* The static copy is stored as the UNLIT BASE.  If the piece is drawn with
 * headlight light this frame (0x20000), the headlight term is built at the hit
 * point with the draw's CURRENT light matrix (world -> light, m.l0..l3):
 * oT1 = p.L, oT2.x = scale*q.w + offset. */
"    if ((m.flags & 0x20000u) != 0u && S.cam2.w < 0.5) {\n"
"      float3 hp2 = org + Rd * h.distance;\n"
"      float4 q1 = hp2.x * m.l0 + hp2.y * m.l1 + hp2.z * m.l2 + m.l3;\n"
"      float4 q2 = float4(q1.w * m.prm.y + m.prm.z, 0.0, 1.0, 1.0);\n"
"      float3 l1 = TX.t[m.tex1].sample(cs, hd_proj(q1, (m.flags >> 8) & 3u), level(0.0)).rgb;\n"
"      float3 l2 = TX.t[m.tex2].sample(cs, hd_proj(q2, (m.flags >> 10) & 3u), level(0.0)).rgb;\n"
"      lit = l1 * l2 * m.c0.rgb + col.rgb;\n"
"    }\n"
"    if ((m.flags & 4u) != 0u && S.cam2.w < 0.5) {\n"   /* ps 4/6: headlight texture x falloff ramp */
"      float4 uv2 = a0.uv2 * w0 + a1.uv2 * b.x + a2.uv2 * b.y;\n"
"      float3 l1 = TX.t[m.tex1].sample(cs, hd_proj(uv1, (m.flags >> 8) & 3u), level(0.0)).rgb;\n"
"      float3 l2 = TX.t[m.tex2].sample(cs, hd_proj(uv2, (m.flags >> 10) & 3u), level(0.0)).rgb;\n"
"      lit = l1 * l2 * m.c0.rgb + col.rgb;\n"
"    }\n"
"    float3 rgb = saturate(2.0 * tc.rgb * lit);\n"
"    rgb = mix(m.fogc.rgb, rgb, mix(1.0, saturate(fog), m.fogc.a));\n"
"    rc = rgb; conf = 1.0 - smoothstep(0.85, 1.0, tot / maxd);\n"
"    break;\n"
"  }\n"
"  outp.write(float4(rc, conf), tid);\n"
"}\n"
/* VERTEX CHECK: if a captured triangle has a NaN / infinite / absurdly far
 * vertex, make it degenerate.  ONE triangle with a NaN poisons the boxes of
 * the acceleration structure from the root: rays can cull no node, tracing
 * gets ~15 times slower (measured: at 3024x1964 a plain trace takes 58 ms
 * instead of 4 ms, in ~8% of runs; which draw produces it depends on
 * randomly chosen content). */
"struct HDFix { uint ntri; float lim; uint first; uint slot; };\n"
"kernel void hd_rt_fix(uint tid [[thread_position_in_grid]],\n"
"    device packed_float3 *P [[buffer(0)]], device atomic_uint *CNT [[buffer(1)]],\n"
"    constant HDFix &F [[buffer(2)]]) {\n"
"  if (tid >= F.ntri) return;\n"
"  uint v = F.first + tid * 3u;\n"
/* F.lim < 0: DIAGNOSTIC (HD_RT_POISON): instead of checking, move one vertex of the first triangle to -F.lim */
"  if (F.lim < 0.0) { if (tid == 0u) P[v] = float3(-F.lim, P[v].y, P[v].z); return; }\n"
"  float3 a = P[v], b = P[v + 1u], c = P[v + 2u];\n"
"  float3 m = max(max(abs(a), abs(b)), abs(c));\n"
"  bool nan = any(isnan(a)) || any(isnan(b)) || any(isnan(c)) || any(isinf(a)) || any(isinf(b)) || any(isinf(c));\n"
"  bool far = !nan && max(m.x, max(m.y, m.z)) > F.lim;\n"
"  if (nan || far) {\n"
"    P[v] = float3(0.0); P[v + 1u] = float3(0.0); P[v + 2u] = float3(0.0);\n"
"    atomic_fetch_add_explicit(&CNT[nan ? 0 : 1], 1u, memory_order_relaxed);\n"
"    atomic_fetch_add_explicit(&CNT[2u + F.slot * 2u + (nan ? 0u : 1u)], 1u, memory_order_relaxed);\n"
"  }\n"
"}\n"
/* composite: color + B * (R_new - R0) */
"static float4 rtc(uint2 px, float4 c, depth2d<float> dep, texture2d<float> g1,\n"
"                  texture2d<float> g2, texture2d<float> rt, constant SSRP &S) {\n"
"  float4 gb = g1.read(px);\n"
"  int dbg = int(S.cam.w) - 1;\n"
"  float cls = GB_CLS(gb);\n"
"  bool road = GB_ROAD(cls);\n"
"  bool car  = GB_CAR(cls);\n"
"  if (!road && !car) return dbg > 0 ? float4(c.rgb * 0.25, c.a) : c;\n"
"  float4 r;\n"
"  uint sc = uint(max(S.cb0.w, 1.0));\n"
"  if (sc <= 1u) r = rt.read(px);\n"
"  else {\n"                 /* depth- and class-aware bilinear upscale */
"    float2 f = float2(px) / float(sc); uint2 b0 = uint2(floor(f)); float2 fr = f - floor(f);\n"
"    uint2 lim = uint2(rt.get_width() - 1, rt.get_height() - 1), fl = uint2(S.sz.xy) - 1u;\n"
"    float zc = S.prm.y / (dep.read(px) - S.prm.x);\n"
"    float4 acc = float4(0.0); float ws = 0.0;\n"
"    for (uint j = 0u; j < 4u; j++) {\n"
"      uint2 o = uint2(j & 1u, j >> 1u); uint2 q = min(b0 + o, lim); uint2 qf = min(q * sc, fl);\n"
"      float zq = S.prm.y / (dep.read(qf) - S.prm.x);\n"
"      float same = abs(GB_CLS(g1.read(qf)) - cls) < 0.3 ? 1.0 : 0.0;\n"
"      float w = (o.x == 1u ? fr.x : 1.0 - fr.x) * (o.y == 1u ? fr.y : 1.0 - fr.y);\n"
"      w = (w + 0.02) * same / (1.0 + 60.0 * abs(zq - zc) / max(zc, 0.5));\n"
"      acc += rt.read(q) * w; ws += w; }\n"
"    r = ws > 0.0 ? acc / ws : float4(0.0);\n"
"  }\n"
"  if (dbg == 4) return float4(r.a > 0.0 ? r.rgb : float3(0.0, 0.0, 0.35), 1.0);\n"
"  if (dbg == 1) return float4(road ? 0.5 : 0.0, r.a, r.a > 0.0 ? (road ? 0.0 : 1.0) : 0.6, 1.0);\n"
"  if (r.a <= 0.0) return c;\n"
"  float4 gv = g2.read(px);\n"
"  float3 R0 = GB_R0(gb, gv);\n"
"  float3 Rn = mix(R0, r.rgb, r.a * S.prm.z);\n"
"  return float4(c.rgb + GB_B(gb, gv) * (Rn - R0), c.a);\n"
"}\n"
"fragment float4 hd_rtc_frag(FSO in [[stage_in]],\n"
"    texture2d<float> scene [[texture(0)]], depth2d<float> dep [[texture(1)]],\n"
"    texture2d<float> g1 [[texture(2)]],\n"
"    texture2d<float> g2 [[texture(3)]], texture2d<float> rt [[texture(5)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  uint2 px = uint2(in.pos.xy);\n"
"  return rtc(px, scene.read(px), dep, g1, g2, rt, S);\n"
"}\n"
/* copy-free: the pixel's own color from tile memory (programmable blending) */
"fragment float4 hd_rtc_frag_fb(FSO in [[stage_in]], float4 c [[color(0)]],\n"
"    depth2d<float> dep [[texture(1)]], texture2d<float> g1 [[texture(2)]],\n"
"    texture2d<float> g2 [[texture(3)]], texture2d<float> rt [[texture(5)]],\n"
"    constant SSRP &S [[buffer(0)]]) {\n"
"  return rtc(uint2(in.pos.xy), c, dep, g1, g2, rt, S);\n"
"}\n";
/* --- COPY-FREE COMPOSITE ----------------------------------------------------
 * The ray tracing composite reads only each pixel's OWN color.  It used to
 * copy the whole screen first (0.34 ms GPU at 3024x1964, ~46 MB of traffic),
 * then write the whole screen again.  On Apple GPUs the pass can load the
 * screen and read the pixel's color from tile memory: no copy needed.  SSR
 * (samples other pixels) and the single MSAA mode stay on the old path.
 * HD_RT_FB=0 is the old path. */
static id<MTLRenderPipelineState> g_rt_pso_fb;
static int hd_rt_fb_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("HD_RT_FB"); v = !(e && *e == '0') && [g_dev supportsFamily:MTLGPUFamilyApple1]; }
    return v;
}
static void hd_rt_init(void){
    if (!hd_rt_on() || g_rt_pso) return;
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:hd_gb_src(kRTFSrc) options:nil error:&e];
    if (!lib){ fprintf(stderr, "[HD] RT shader did not compile: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    id<MTLFunction> ff = [lib newFunctionWithName:@"hd_rtc_frag"];
    id<MTLFunction> kf = [lib newFunctionWithName:@"hd_rt_kern"];
    g_rt_kern = [g_dev newComputePipelineStateWithFunction:kf error:&e];
    { id<MTLFunction> xf = [lib newFunctionWithName:@"hd_rt_fix"];
      if (xf) g_rt_fixk = [g_dev newComputePipelineStateWithFunction:xf error:&e];
      g_rt_fixcnt = [g_dev newBufferWithLength:(2 + 2 * HD_MAXVSI) * 4 options:MTLResourceStorageModeShared];
      memset([g_rt_fixcnt contents], 0, (2 + 2 * HD_MAXVSI) * 4);
      { const char *ev = ENVC("HD_RT_FIX"); if (ev && *ev == '0') g_rt_fixk = nil; } }
    if (!g_rt_kern){ fprintf(stderr, "[HD] RT kernel could not be built: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"hd_fs_vert"];
    pd.fragmentFunction = ff;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_rt_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (hd_rt_fb_on()){ MTLRenderPipelineDescriptor *pf = [pd copy];
        pf.fragmentFunction = [lib newFunctionWithName:@"hd_rtc_frag_fb"];
        NSError *ef = nil; g_rt_pso_fb = [g_dev newRenderPipelineStateWithDescriptor:pf error:&ef];
        if (!g_rt_pso_fb) fprintf(stderr, "[HD] copy-free composite could not be built: %s\n", [[ef localizedDescription] UTF8String]); }
    if (hd_msaa_n() > 1){ pd.rasterSampleCount = (NSUInteger)hd_msaa_n();
                          g_rt_pso_ms = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e]; }
    if (!g_rt_pso){ fprintf(stderr, "[HD] RT pipeline could not be built: %s\n", [[e localizedDescription] UTF8String]); g_rt = 0; return; }
    g_rt_argenc = [kf newArgumentEncoderWithBufferIndex:6];
    g_rt_pos   = [g_dev newBufferWithLength:RT_MAXV * 12 options:MTLResourceStorageModePrivate];
    g_rt_attr  = [g_dev newBufferWithLength:RT_MAXV * 64 options:MTLResourceStorageModePrivate];
    for (int i = 0; i < 2; i++){
        g_rt_sposb[i]  = [g_dev newBufferWithLength:RT_SMAXV * 12 options:MTLResourceStorageModePrivate];
        g_rt_sattrb[i] = [g_dev newBufferWithLength:RT_SMAXV * 64 options:MTLResourceStorageModePrivate];
    }
    for (int i = 0; i < 3; i++){
        g_rt_item[i] = [g_dev newBufferWithLength:RT_MAXV * 4 options:MTLResourceStorageModeShared];
        g_rt_rec[i]  = [g_dev newBufferWithLength:RT_MAXREC * sizeof(HDRecC) options:MTLResourceStorageModeShared];
        g_rt_mat[i]  = [g_dev newBufferWithLength:(RT_SMAXE + RT_MAXREC) * sizeof(HDMatC) options:MTLResourceStorageModeShared];
        g_rt_arg[i]  = [g_dev newBufferWithLength:g_rt_argenc.encodedLength options:MTLResourceStorageModeShared];
    }
    /* the sub-structures and scratch buffers are allocated once, at the largest size */
    for (int k = 0; k < 2; k++){
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = k ? g_rt_spos : g_rt_pos; gd.vertexStride = 12;
        gd.triangleCount = (k ? RT_SMAXV : RT_MAXV) / 3; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd];
        ad.usage = k ? MTLAccelerationStructureUsageNone : hd_rt_dyn_usage();
        MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:ad];
        id<MTLAccelerationStructure> a = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
        NSUInteger scs = sz.buildScratchBufferSize > sz.refitScratchBufferSize ? sz.buildScratchBufferSize : sz.refitScratchBufferSize;
        id<MTLBuffer> sc = [g_dev newBufferWithLength:scs options:MTLResourceStorageModePrivate];
        if (k){ g_rt_sas = a; g_rt_sscratch = sc; } else { g_rt_as = a; g_rt_scratch = sc; }
    }
    g_rt_inst = [g_dev newBufferWithLength:2 * sizeof(MTLAccelerationStructureInstanceDescriptor)
                                   options:MTLResourceStorageModeShared];
    { MTLAccelerationStructureInstanceDescriptor *id2 = (MTLAccelerationStructureInstanceDescriptor *)[g_rt_inst contents];
      memset(id2, 0, 2 * sizeof *id2);
      for (int k = 0; k < 2; k++){
          id2[k].transformationMatrix.columns[0] = MTLPackedFloat3Make(1, 0, 0);
          id2[k].transformationMatrix.columns[1] = MTLPackedFloat3Make(0, 1, 0);
          id2[k].transformationMatrix.columns[2] = MTLPackedFloat3Make(0, 0, 1);
          id2[k].transformationMatrix.columns[3] = MTLPackedFloat3Make(0, 0, 0);
          id2[k].options = MTLAccelerationStructureInstanceOptionOpaque;
          id2[k].mask = 0xFF; id2[k].accelerationStructureIndex = (uint32_t)k; } }
    { MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
      td.instancedAccelerationStructures = @[g_rt_as, g_rt_sas];
      td.instanceCount = 2; td.instanceDescriptorBuffer = g_rt_inst;
      MTLAccelerationStructureSizes sz = [g_dev accelerationStructureSizesWithDescriptor:td];
      g_rt_tlas = [g_dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
      g_rt_tscratch = [g_dev newBufferWithLength:sz.buildScratchBufferSize options:MTLResourceStorageModePrivate]; }
    { const char *e2 = ENVC("HD_RT_CACHE"); g_rt_cache = (e2 && *e2 == '0') ? 0 : 1; }
    fprintf(stderr, "[HD] RT ready: dynamic at most %u triangles, static store at most %u triangles, cache %s\n",
            RT_MAXV / 3, RT_SMAXV / 3, g_rt_cache ? "ON" : "OFF");
}
static unsigned hd_rt_slot(unsigned tex, unsigned *nt){
    if (!tex || tex >= MAXTEX || !g_tex[tex] || g_tex_cube[tex] ||
        g_tex[tex].textureType != MTLTextureType2D) return 0;
    if (g_rt_tstamp[tex] == g_rt_stamp) return g_rt_tslot[tex];
    if (*nt >= RT_MAXTEX) return 0;
    g_rt_tstamp[tex] = g_rt_stamp; g_rt_tslot[tex] = (uint16_t)*nt;
    g_rt_texs[*nt] = g_tex[tex];
    return (*nt)++;
}
/* ---- STATIC SCENE CACHE -----------------------------------------------
 * The game does not draw what the camera does not see; the scene behind the
 * car would be missing from the reflection.  Also, capturing the whole scene
 * and building the structure every frame pushed the GPU past the frame
 * budget (measured: 15.6 ms/frame).
 * Draw = (hash of the raw vertex data, world matrix W).  If the same draw is
 * seen at the same place for RT_PROMOTE frames it counts as STATIC: its
 * captured vertices are copied to the static store on the GPU (no read-back
 * to the CPU) and the static sub-structure is rebuilt at most once every
 * RT_SREBUILD frames.  Static entries leave the per-frame capture; the
 * per-frame sub-structure keeps the cars + what just came into view.
 * Eviction: when it is inside the view yet not drawn (moved/removed), or
 * when it is very far.  Cars (cube-mapped ps) and skinned meshes never enter. */
#define RT_PROMOTE  12u
#define RT_SREBUILD 30u
#define RT_TABN     16384u
typedef struct { uint64_t rk, gh; float W[12]; float c[3], r; uint32_t first, n, seen, born, matidx;
                 uint16_t sight; uint8_t state; uint8_t pad; uint8_t vol; } RTEnt;
/* state: 0 empty, 1 candidate, 2 static (waiting for the structure), 3 static, 9 deleted.
 * pad: copied to the store.  gh: geometry-only hash (table key); rk: gh + texture + VS.
 * vol: volatility (below). */
static RTEnt g_rt_tab[RT_TABN];
static HDMatC g_rt_smatc[RT_SMAXE]; static uint32_t g_rt_smtex[RT_SMAXE][3];
static uint32_t g_rt_scur, g_rt_sdead, g_rt_smn, g_rt_sbuilt, g_rt_frame_no, g_rt_last_sbuild, g_rt_tomb;
static int g_rt_sdirty;
static _Atomic unsigned long long g_rt_promo, g_rt_evict, g_rt_reset, g_rt_sbuilds, g_rt_dynv, g_rt_statv;
static int g_rt_need_clear = 1;          /* a new buffer's contents are undefined */
static uint32_t g_rt_mfree[RT_SMAXE], g_rt_mfree_n;
static uint32_t g_rt_mpend[RT_SMAXE], g_rt_mpend_n;   /* slots of evicted entries: free once the structure is rebuilt */
static unsigned g_rt_ev_far, g_rt_ev_frus, g_rt_ev_cand;
static _Atomic unsigned long long g_rt_evsib;
static _Atomic unsigned long long g_rt_compact, g_rt_rehash;
#define RT_HIDX(gh) ((unsigned)((gh) * 0x9E3779B97F4A7C15ull >> 50) & (RT_TABN - 1))
/* Deleted entries (9) lengthen the probe chains; re-place the live ones in the table. */
static void rt_rehash(void){
    static RTEnt tmp[RT_TABN];
    unsigned n = 0;
    for (unsigned q = 0; q < RT_TABN; q++) if (g_rt_tab[q].state && g_rt_tab[q].state != 9) tmp[n++] = g_rt_tab[q];
    memset(g_rt_tab, 0, sizeof g_rt_tab); g_rt_tomb = 0;
    for (unsigned q = 0; q < n; q++){
        unsigned i = RT_HIDX(tmp[q].gh);
        while (g_rt_tab[i].state) i = (i + 1) & (RT_TABN - 1);
        g_rt_tab[i] = tmp[q];
    }
    atomic_fetch_add(&g_rt_rehash, 1ull);
}
/* Evict an entry.  If it is static, its range in the store is zeroed
 * (degenerate triangles) and its material slot is freed once the structure
 * is rebuilt.  If the zeroing list is full, the eviction is postponed
 * (returns 0). */
static uint32_t g_rt_zr[1024][2]; static unsigned g_rt_nzr;
static int rt_evict(RTEnt *e){
    if (e->state == 2 || e->state == 3){
        if (e->pad){
            if (g_rt_nzr >= 1024) return 0;
            g_rt_zr[g_rt_nzr][0] = e->first; g_rt_zr[g_rt_nzr][1] = e->n; g_rt_nzr++;
        }
        g_rt_sdead += e->n; g_rt_sdirty = 1;
        if (g_rt_mpend_n < RT_SMAXE) g_rt_mpend[g_rt_mpend_n++] = e->matidx;
        atomic_fetch_add(&g_rt_evict, 1ull);
    }
    e->state = 9; g_rt_tomb++;
    return 1;
}
/* COMPACTION: once dead ranges pass 40% of the store, the live entries are
 * copied back to back into the other buffer pair (on the GPU).  Resetting
 * the whole store also erased the accumulated geometry behind the camera.
 * The command buffers of earlier frames keep reading the old pair
 * (ping-pong). */
static void rt_compact(id<MTLCommandBuffer> cb){
    unsigned src = g_rt_sbuf, dst = 1u - g_rt_sbuf;
    uint32_t cur = 0;
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    for (unsigned q = 0; q < RT_TABN; q++){
        RTEnt *e = &g_rt_tab[q];
        if (e->state != 2 && e->state != 3) continue;
        if (!e->pad){ e->state = 1; g_rt_mfree[g_rt_mfree_n++] = e->matidx; continue; }   /* not copied yet */
        [be copyFromBuffer:g_rt_sposb[src] sourceOffset:(NSUInteger)e->first * 12 toBuffer:g_rt_sposb[dst]
         destinationOffset:(NSUInteger)cur * 12 size:(NSUInteger)e->n * 12];
        [be copyFromBuffer:g_rt_sattrb[src] sourceOffset:(NSUInteger)e->first * 64 toBuffer:g_rt_sattrb[dst]
         destinationOffset:(NSUInteger)cur * 64 size:(NSUInteger)e->n * 64];
        e->first = cur; cur += e->n; e->state = 2;              /* in the per-frame capture until the structure is rebuilt */
    }
    /* the rest is left over from an earlier compaction: promotions whose copy is late must not show garbage triangles */
    if (cur < RT_SMAXV)
        [be fillBuffer:g_rt_sposb[dst] range:NSMakeRange((NSUInteger)cur * 12, (NSUInteger)(RT_SMAXV - cur) * 12) value:0];
    [be endEncoding];
    g_rt_sbuf = dst; g_rt_scur = cur; g_rt_sdead = 0; g_rt_sbuilt = 0; g_rt_sdirty = 1;
    atomic_fetch_add(&g_rt_compact, 1ull);
}
static void rt_cache_reset(void){
    g_rt_need_clear = 1;                 /* make the old geometry degenerate */
    g_rt_mfree_n = g_rt_mpend_n = 0;
    memset(g_rt_tab, 0, sizeof g_rt_tab);
    g_rt_scur = g_rt_sdead = g_rt_smn = g_rt_sbuilt = g_rt_tomb = 0; g_rt_sdirty = 0;
    atomic_fetch_add(&g_rt_reset, 1ull);
}
static uint32_t g_rt_slit[RT_SMAXE];      /* static material: drawn with headlight light this frame */
static inline void rt_mat_of(const MDraw *dr, HDMatC *m){
    int base = dr->ps % PSREFL, cube = base >= PSCUBE, b = base % PSCUBE;
    memset(m, 0, sizeof *m);
    m->tex0 = dr->tex; m->tex1 = dr->tex1; m->tex2 = dr->tex2;        /* texture IDS */
    m->flags = (dr->alpha_ref > 0.f ? 1u : 0u) | (cube ? 2u : 0u) | ((b == 4 || b == 6) ? 4u : 0u)
             | ((((unsigned)(int)dr->proj[1] >> 4) & 3u) << 8) | ((((unsigned)(int)dr->proj[2] >> 4) & 3u) << 10);
    memcpy(m->c0, dr->c0, 16); memcpy(m->fogc, dr->fogv, 16);
    m->prm[0] = dr->alpha_ref;
}
/* Refresh a static entry's material from the current draw (so fog color etc.
 * stay current).  The static copy is stored as the UNLIT BASE: the headlights
 * (ps 4/6) travel with the player's car; frozen per vertex, the pool of light
 * would stay on the road after the car has gone (measured: the start line
 * glows in the reflection after the opponents have left).  The game draws
 * nearly all of the near scene with the lit VS (255 draws at race start);
 * recapturing all of them every frame would defeat the cache (GPU 7.1 ->
 * 10.9 ms).  Instead the draw's light matrix (VS constants c[K..K+3],
 * object -> light) is converted to world space and written into the
 * material; the kernel builds the headlight term at the hit point.
 * If the piece was not drawn lit that frame (or not drawn at all), there is
 * no term. */
static void rt_smat_update(const RTEnt *e, const MDraw *dr, const float *k, uint32_t frame){
    HDMatC m; rt_mat_of(dr, &m);
    int lit = (m.flags & 4u) != 0u, K = g_hd_vsi[dr->vsid].litr;
    m.flags = (m.flags & ~4u) | 0x10000u;                       /* 0x10000: fog is recomputed */
    if (lit && K >= 0 && (unsigned)(K + 4) * 16u <= dr->ksz - 16u){
        /* world = object*A + t  ->  object = (world - t)*A^-1;  q = object*LW,  LW[r][c] = c[K+c][r] */
        const float *W = e->W;
        double a00 = W[0], a01 = W[1], a02 = W[2], a10 = W[3], a11 = W[4], a12 = W[5], a20 = W[6], a21 = W[7], a22 = W[8];
        double det = a00*(a11*a22 - a12*a21) - a01*(a10*a22 - a12*a20) + a02*(a10*a21 - a11*a20);
        if (fabs(det) > 1e-12){
            double id = 1.0 / det, ai[3][3] = {
                { (a11*a22 - a12*a21)*id, (a02*a21 - a01*a22)*id, (a01*a12 - a02*a11)*id },
                { (a12*a20 - a10*a22)*id, (a00*a22 - a02*a20)*id, (a02*a10 - a00*a12)*id },
                { (a10*a21 - a11*a20)*id, (a01*a20 - a00*a21)*id, (a00*a11 - a01*a10)*id } };
            for (int c = 0; c < 4; c++){
                double l[3];
                for (int j = 0; j < 3; j++){
                    l[j] = 0; for (int r = 0; r < 3; r++) l[j] += ai[j][r] * (double)k[(K + c) * 4 + r];
                    m.lw[j * 4 + c] = (float)l[j]; }
                m.lw[12 + c] = (float)((double)k[(K + c) * 4 + 3] - (double)W[9]*l[0] - (double)W[10]*l[1] - (double)W[11]*l[2]);
            }
            m.prm[1] = g_hd_vsi[dr->vsid].ramp[0]; m.prm[2] = g_hd_vsi[dr->vsid].ramp[1];
            g_rt_slit[e->matidx] = frame;
        } else m.flags &= ~0xF00u;
    } else m.flags &= ~0xF00u;
    g_rt_smatc[e->matidx] = m;
}
static uint64_t rt_rawhash(const uint8_t *p, unsigned bytes, unsigned n, unsigned stride){
    uint64_t h = 1469598103934665603ull ^ ((uint64_t)n << 32) ^ stride;
    unsigned seg = bytes < 288u ? bytes : 96u;
    unsigned off[3] = { 0, bytes < 288u ? 0 : (bytes / 2u) & ~3u, bytes < 288u ? 0 : bytes - 96u };
    for (int s3 = 0; s3 < (bytes < 288u ? 1 : 3); s3++){
        const uint8_t *q = p + off[s3];
        unsigned k = 0;
        for (; k + 8 <= seg; k += 8){ uint64_t v; memcpy(&v, q + k, 8); h = (h ^ v) * 1099511628211ull; h ^= h >> 29; }
        for (; k < seg; k++){ h = (h ^ q[k]) * 1099511628211ull; }
    }
    return h ? h : 1;
}
static int rt_same_t(const float *a, const float *b){
    return fabsf(a[9] - b[9]) + fabsf(a[10] - b[10]) + fabsf(a[11] - b[11]) <= 0.03f;
}
static int rt_same_r(const float *a, const float *b){
    float dr = 0; for (int q = 0; q < 9; q++) dr += fabsf(a[q] - b[q]);
    return dr <= 0.02f;
}
/* The same geometry (gh) drawn at the same PLACE but with another
 * texture/VS or another ORIENTATION is a SIBLING:
 *   - a road piece with headlight light <-> without, animated textures (rk differs);
 *   - camera-facing tree/light billboards, rotating objects (orientation differs).
 * If both stay in the store they overlap: a ray hits one at random, and
 * alpha-tested stacks make the ray be cast again and again (measured:
 * 12 copies of one tree; GPU 8 -> 22 ms).  Rule:
 *   - an unseen (3+ frames) STATIC sibling is evicted;
 *   - an unseen CANDIDATE sibling is reused in place (so an object that
 *     turns every frame does not fill the table with new entries);
 *   - every change raises the VOLATILITY (vol), promotion threshold
 *     RT_PROMOTE << vol: something that keeps changing stays in the
 *     per-frame capture and does not churn the store.
 * Siblings drawn together in the same frame (mirrored twin pieces) live. */
static RTEnt *rt_find(uint64_t rk, uint64_t gh, const float *W, uint32_t frame){
    unsigned i = RT_HIDX(gh), tomb = RT_TABN, vol = 0;
    RTEnt *found = 0, *empty = 0, *reuse = 0;
    for (unsigned probe = 0; probe < 512; probe++, i = (i + 1) & (RT_TABN - 1)){
        RTEnt *e = &g_rt_tab[i];
        if (e->state == 0){ empty = e; break; }
        if (e->state == 9){ if (tomb == RT_TABN) tomb = i; continue; }
        if (e->gh != gh || !rt_same_t(e->W, W)) continue;
        if (e->rk == rk && rt_same_r(e->W, W)){ found = e; continue; }
        if (e->vol > vol) vol = e->vol;
        if (e->state == 1){ if (frame - e->seen >= 1u && !reuse) reuse = e; continue; }
        if (frame - e->seen >= 3u){
            unsigned v = e->vol + 1u; if (v > 6u) v = 6u;
            if (rt_evict(e)){ if (v > vol) vol = v; atomic_fetch_add(&g_rt_evsib, 1ull); }
        }
    }
    if (!found){
        if (reuse){ found = reuse; if (vol < 6u) vol++; }
        else if (tomb != RT_TABN){ found = &g_rt_tab[tomb]; g_rt_tomb--; }
        else if (empty) found = empty;
        else return 0;
        memset(found, 0, sizeof *found);
        found->rk = rk; found->gh = gh; memcpy(found->W, W, sizeof found->W);
        found->state = 1; found->born = frame; found->seen = frame - 1u;
    }
    if (vol > found->vol) found->vol = (uint8_t)vol;
    return found;
}
/* Capture the scene's draws [0, upto), build the sub-structures and the top
 * structure.  Returns: total triangles (0 = no ray tracing this frame).
 * fogp: the game's fog parameters for static hits (end, scale, lower bound,
 * present). */
static unsigned hd_rt_frame(id<MTLCommandBuffer> cb, int rb, unsigned upto, const float *iv,
                            const float *vp, const float *ivp, float ipx, float ipy,
                            unsigned *ntex, float *fogp){
    if (!g_rt_pso) return 0;
    unsigned ring = (g_rt_ring = (g_rt_ring + 1) % 3);
    uint32_t frame = ++g_rt_frame_no;
    static unsigned cnt[HD_MAXVSI], off[HD_MAXVSI], fill[HD_MAXVSI];
    static uint8_t  act[MAXD];            /* 0 skip, 1 capture this frame */
    static RTEnt   *ent[MAXD];
    memset(cnt, 0, sizeof cnt);
    unsigned nrec = 0, total = 0, npromo = 0;
    fogp[0] = fogp[1] = fogp[2] = fogp[3] = 0.f;
    g_rt_nzr = 0;
    if (g_rt_cache){
        /* HD_RT_STRESS=N: compaction + rehash every N frames (tests the rare paths) */
        static int stress = -1; if (stress < 0){ const char *ev = ENVC("HD_RT_STRESS"); stress = ev ? atoi(ev) : 0; }
        if (stress > 0 && frame % (uint32_t)stress == 0u){ rt_rehash(); if (g_rt_scur) rt_compact(cb); }
        if (g_rt_tomb > RT_TABN / 4u) rt_rehash();
        if ((g_rt_scur > 30000u && g_rt_sdead * 5u > g_rt_scur * 2u) ||
            (g_rt_scur > RT_SMAXV - 30000u && g_rt_sdead > 0u)) rt_compact(cb);
        if (g_rt_scur > RT_SMAXV - 30000u && g_rt_sdead == 0u && frame % 600u == 0u) rt_cache_reset();  /* full and nothing dead: the way to drop the oldest */
    }
    #define RT_OK(dr) (!(dr)->target && !(dr)->is_clear && !(dr)->is_blit && (dr)->count >= 3 && \
        (dr)->vsid > 0 && (dr)->vsid < HD_MAXVSI && ((dr)->depth & 2u))
    /* --- pass 1: classify --- */
    for (unsigned i = 0; i < upto && i < MAXD; i++){
        MDraw *dr = &g_dbuf[rb][i];
        act[i] = 0; ent[i] = 0;
        if (!RT_OK(dr)) continue;
        if (!g_hd_vsk[dr->vsid]){ atomic_fetch_add(&g_rt_skipk, 1ull); continue; }
        const uint8_t *kb = g_kbuf[rb] + dr->koff;
        const float *k = (const float *)(kb + 16);
        unsigned stride; memcpy(&stride, kb, 4);
        int cx = g_hd_vsi[dr->vsid].ok ? g_hd_vsi[dr->vsid].cx : -1;
        int have_m = cx >= 0 && (unsigned)(cx + 4) * 16u <= dr->ksz - 16u;
        if (have_m){
            const float *c3 = k + (cx + 3) * 4;
            if (c3[0] == 0.f && c3[1] == 0.f && c3[2] == 0.f) continue;       /* orthographic (screen) */
        }
        { int fr = g_hd_vsi[dr->vsid].fogr;                                   /* the game's fog constants */
          if (fr >= 0 && fogp[3] == 0.f && (unsigned)(fr + 1) * 16u <= dr->ksz - 16u){
              fogp[0] = k[fr*4]; fogp[1] = k[fr*4+1]; fogp[2] = k[fr*4+2]; fogp[3] = 1.f; } }
        unsigned n = dr->count - dr->count % 3u;
        int base = dr->ps % PSREFL, cube = base >= PSCUBE;
        RTEnt *e = 0;
        if (g_rt_cache && have_m && !cube && !g_hd_vsi[dr->vsid].skin && stride && n <= 60000u){
            /* W = M * P'^-1 * V^-1,  M[r][c] = c[cx+c][r].  The SAME path as the
             * capture kernel: view = (x/P00', y/P11, w), homogeneous column EXACTLY (0,0,0,1).
             * It used to be M * (VP')^-1: the homogeneous component came out of the
             * (z - Q*w)/(-Q*near) cancellation with ~1e-4 error; multiplied by the
             * camera position (hundreds of units), W's translation wobbled 0.03-0.1
             * units with the camera.  Result: the same object counted as a separate
             * entry at every camera angle and piled up in the store (measured: 878
             * of 1771 entries were copies; alpha-tested stacks of copies made the
             * ray be cast 5 times again and took the GPU from 8 ms to 22 ms). */
            float W[12];
            for (int r = 0; r < 4; r++){
                double vx = (double)k[cx * 4 + r] * (double)ipx, vy = (double)k[(cx + 1) * 4 + r] * (double)ipy,
                       vz = (double)k[(cx + 3) * 4 + r];
                for (int c = 0; c < 3; c++)
                    W[r * 3 + c] = (float)(vx * iv[c] + vy * iv[4 + c] + vz * iv[8 + c] + (r == 3 ? (double)iv[12 + c] : 0.0));
            }
            /* for an indexed draw the identity (VB/IB + generation + range) was computed on the guest side */
            uint64_t gh = dr->gh ? dr->gh : rt_rawhash(g_rvbuf[rb] + dr->rawoff, n * stride, n, stride);
            /* rk has NO VS id: the draws of the same piece with and without headlight
             * light (different VS, same positions) are one entry */
            uint64_t rk = (gh ^ ((uint64_t)dr->tex << 12)) * 0xD6E8FEB86659FD93ull;
            e = rt_find(rk ? rk : 1, gh, W, frame);
            if (e){
                /* promotion needs CONSECUTIVE sightings: a rotating object comes back to the
                 * same angle periodically, and a total count made it "static" at every angle */
                if (e->seen != frame){
                    if (e->state == 1 && frame - e->seen > 2u) e->sight = 0;
                    e->seen = frame; if (e->sight < 60000) e->sight++; }
                if (e->state == 3 && e->n == n){
                    rt_smat_update(e, dr, k, frame);
                    ent[i] = e; continue;                                      /* in the static structure: no capture */
                }
                if (e->state == 1 && e->sight >= (RT_PROMOTE << e->vol) && npromo < 400u &&
                    g_rt_scur + n <= RT_SMAXV - 30000u && (g_rt_mfree_n || g_rt_smn < RT_SMAXE - 64u)){
                    npromo++;
                    /* PROMOTION: bounding sphere from the raw positions */
                    unsigned po = (unsigned)(g_hd_vsi[dr->vsid].po < 0 ? 0 : g_hd_vsi[dr->vsid].po);
                    float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
                    for (unsigned j = 0; j < n; j++){ float pv[3]; memcpy(pv, dr_vtx(rb, dr, j, stride) + po, 12);
                        for (int a3 = 0; a3 < 3; a3++){ if (pv[a3] < mn[a3]) mn[a3] = pv[a3]; if (pv[a3] > mx[a3]) mx[a3] = pv[a3]; } }
                    float lc[3] = { 0.5f*(mn[0]+mx[0]), 0.5f*(mn[1]+mx[1]), 0.5f*(mn[2]+mx[2]) };
                    float hx = 0.5f*(mx[0]-mn[0]), hy = 0.5f*(mx[1]-mn[1]), hz = 0.5f*(mx[2]-mn[2]);
                    for (int c = 0; c < 3; c++) e->c[c] = lc[0]*W[c] + lc[1]*W[3+c] + lc[2]*W[6+c] + W[9+c];
                    float sc2 = 0; for (int q = 0; q < 9; q++) sc2 += W[q]*W[q];
                    e->r = sqrtf(hx*hx + hy*hy + hz*hz) * sqrtf(sc2 / 3.f) + 0.1f;
                    float ddx = e->c[0] - iv[12], ddy = e->c[1] - iv[13], ddz = e->c[2] - iv[14];
                    if (sqrtf(ddx*ddx + ddy*ddy + ddz*ddz) > 800.f + e->r){
                        /* out of ray range: do not store it (it would be evicted as "far" the moment it is unseen) */
                        e->sight = 0;
                    } else {
                    e->state = 2; e->first = g_rt_scur; e->n = n; e->pad = 0;
                    e->matidx = g_rt_mfree_n ? g_rt_mfree[--g_rt_mfree_n] : g_rt_smn++;
                    g_rt_scur += n; g_rt_sdirty = 1;
                    atomic_fetch_add(&g_rt_promo, 1ull);
                    }
                }
            }
        }
        if (total + n > RT_MAXV || nrec >= RT_MAXREC){ atomic_fetch_add(&g_rt_over, 1ull); continue; }
        act[i] = 1; ent[i] = e;
        cnt[dr->vsid] += n; total += n; nrec++;
    }
    { unsigned a = 0; for (int g = 0; g < HD_MAXVSI; g++){ off[g] = a; a += cnt[g]; } }
    memcpy(fill, off, sizeof fill);
    uint32_t *item = (uint32_t *)[g_rt_item[ring] contents];
    HDRecC *rec = (HDRecC *)[g_rt_rec[ring] contents];
    HDMatC *mat = (HDMatC *)[g_rt_mat[ring] contents];
    unsigned nt = 1, ri = 0;
    g_rt_stamp++; g_rt_texs[0] = g_white;
    /* GPU copies of the promoted entries: (first per-frame vertex, first static vertex, count) */
    static uint32_t cp[512][3]; unsigned ncp = 0;
    uint64_t dsig = 1469598103934665603ull;         /* signature of the per-frame triangle layout (see refit) */
    /* --- pass 2: records + materials --- */
    for (unsigned i = 0; i < upto && i < MAXD; i++){
        if (!act[i]) continue;
        MDraw *dr = &g_dbuf[rb][i];
        unsigned n = dr->count - dr->count % 3u, f = fill[dr->vsid];
        { uint64_t w[4] = { (uint64_t)dr->vsid << 32 | n, (uint64_t)(uint32_t)dr->ps << 32 | dr->tex, dr->gh, f };
          for (int q = 0; q < 4; q++){ dsig ^= w[q]; dsig *= 1099511628211ull; } }
        fill[dr->vsid] += n;
        int base = dr->ps % PSREFL, cube = base >= PSCUBE, b = base % PSCUBE;
        HDMatC m; rt_mat_of(dr, &m);
        RTEnt *e = ent[i];
        uint32_t midx = RT_SMAXE + ri;
        if (e && e->state == 2 && e->first + n <= RT_SMAXV && e->seen == frame && e->born != 0xFFFFFFFFu && ncp < 512 &&
            e->pad == 0){
            /* promoted this frame: static material + GPU copy */
            e->pad = 1; midx = e->matidx;
            rt_smat_update(e, dr, (const float *)(g_kbuf[rb] + dr->koff + 16), frame);
            cp[ncp][0] = f; cp[ncp][1] = e->first; cp[ncp][2] = n; ncp++;
        }
        HDMatC *dm = &mat[RT_SMAXE + ri];
        *dm = m;
        dm->tex0 = hd_rt_slot(m.tex0, &nt); dm->tex1 = hd_rt_slot(m.tex1, &nt); dm->tex2 = hd_rt_slot(m.tex2, &nt);
        rec[ri].rawoff = dr->rawoff; rec[ri].koff = dr->koff; rec[ri].first = f; rec[ri].mat = midx;
        rec[ri].ib = dr->ix_ib; rec[ri].base = dr->ix_base; rec[ri].isz = dr->ix_isz; rec[ri].pad = 0;
        for (unsigned q = 0; q < n; q++) item[f + q] = ri;
        ri++;
    }
    #undef RT_OK
    /* static materials: texture id -> this frame's slot */
    for (unsigned q = 0; q < g_rt_smn; q++){
        HDMatC *sm = &mat[q]; *sm = g_rt_smatc[q];
        if (g_rt_slit[q] == frame) sm->flags |= 0x20000u;
        sm->tex0 = hd_rt_slot(g_rt_smatc[q].tex0, &nt); sm->tex1 = hd_rt_slot(g_rt_smatc[q].tex1, &nt);
        sm->tex2 = hd_rt_slot(g_rt_smatc[q].tex2, &nt);
    }
    /* --- eviction sweep ---
     * The game also culls things inside the view (it does not draw small
     * objects far away; sector visibility).  Applying the "in view but not
     * drawn" rule out to 200 units made poles etc. get evicted and promoted
     * again and again (measured: 300+ evictions in 300 frames while the
     * camera moved).  Now:
     *  - FAR: farther than 900 units and unseen for 30 frames (ray range 600);
     *  - NEAR: right in front of the camera (< 50 units, less for small ones),
     *    in the middle of the view and not drawn for 60 frames = really gone
     *    (a knocked-over obstacle, a menu scene that closed);
     *  - SIBLING: inside rt_find (same geometry drawn with another texture/VS). */
    if (g_rt_cache){
        static int lg2 = -1; if (lg2 < 0){ const char *ev = ENVC("HD_RT_LOG"); lg2 = ev ? atoi(ev) : 0; }
        for (unsigned q = 0; q < RT_TABN; q++){
            RTEnt *e = &g_rt_tab[q];
            if (e->state == 0 || e->state == 9) continue;
            uint32_t age = frame - e->seen;
            if (e->state == 1){ if (age > 30u + 90u * e->vol){ e->state = 9; g_rt_tomb++; g_rt_ev_cand++; } continue; }
            if (age < 30u) continue;
            float dx = e->c[0] - iv[12], dy = e->c[1] - iv[13], dz = e->c[2] - iv[14];
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            int kill = dist > 900.f + e->r ? 1 : 0;
            if (!kill && age >= 60u){
                float qx = e->c[0]*vp[0] + e->c[1]*vp[4] + e->c[2]*vp[8]  + vp[12];
                float qy = e->c[0]*vp[1] + e->c[1]*vp[5] + e->c[2]*vp[9]  + vp[13];
                float qw = e->c[0]*vp[3] + e->c[1]*vp[7] + e->c[2]*vp[11] + vp[15];
                float lim = 60.f * e->r < 50.f ? 60.f * e->r : 50.f;
                if (qw > e->r + 3.f && qw < lim && fabsf(qx) < 0.6f * qw && fabsf(qy) < 0.6f * qw) kill = 2;
            }
            if (!kill) continue;
            uint32_t tex0 = g_rt_smatc[e->matidx].tex0, fl = g_rt_smatc[e->matidx].flags;
            if (!rt_evict(e)) break;
            if (kill == 1) g_rt_ev_far++; else g_rt_ev_frus++;
            if (lg2 >= 2 && (frame % 300u) < 100u)
                fprintf(stderr, "[RTEV] frame %u %s: gh %016llx n %u distance %.0f r %.1f centre (%.0f %.0f %.0f) sightings %u life %u age %u mat(tex %u flags %x)\n",
                        frame, kill == 1 ? "far" : "near", (unsigned long long)e->gh, e->n, dist, e->r,
                        e->c[0], e->c[1], e->c[2], e->sight, frame - e->born, age, tex0, fl);
        }
    }
    { static int lg3 = -1; if (lg3 < 0){ const char *ev = ENVC("HD_RT_DUMPTAB"); lg3 = ev ? atoi(ev) : 0; }
      if (lg3 > 0 && frame == (uint32_t)lg3){
          for (unsigned q = 0; q < RT_TABN; q++){ RTEnt *e = &g_rt_tab[q];
              if (e->state != 2 && e->state != 3) continue;
              float dw = 0; for (int a = 0; a < 12; a++) dw += fabsf(e->W[a] - ((a == 0 || a == 4 || a == 8) ? 1.f : 0.f));
              fprintf(stderr, "[RTTAB] %016llx %016llx n %u c %.2f %.2f %.2f r %.2f age %u seen %u vol %u unit %d t %.2f %.2f %.2f tex %u %u flags %x aref %.3f\n",
                      (unsigned long long)e->gh, (unsigned long long)e->rk, e->n, e->c[0], e->c[1], e->c[2], e->r, frame - e->seen, e->sight, e->vol,
                      dw < 1e-3f, e->W[9], e->W[10], e->W[11], g_rt_smatc[e->matidx].tex0, g_rt_smatc[e->matidx].tex1,
                      g_rt_smatc[e->matidx].flags, g_rt_smatc[e->matidx].prm[0]); }
          fprintf(stderr, "[RTTAB] camera %.2f %.2f %.2f\n", iv[12], iv[13], iv[14]);
      } }
    { static int lg = -1; if (lg < 0) lg = ENVC("HD_RT_LOG") ? 1 : 0;
      static unsigned long long lp, le, lsib;
      if (lg && frame % 300u == 0u){
          unsigned n1 = 0, n2 = 0, n3 = 0;
          for (unsigned q = 0; q < RT_TABN; q++){ unsigned st = g_rt_tab[q].state; n1 += st == 1; n2 += st == 2; n3 += st == 3; }
          unsigned long long pr = atomic_load(&g_rt_promo), ev = atomic_load(&g_rt_evict);
          fprintf(stderr, "[RTLOG] frame %u: dynamic %u triangles/%u draws, store %u verts (%u dead), candidates %u pending %u static %u | "
                  "in 300 frames promoted %llu evicted %llu (far %u, near %u, sibling %llu, candidate %u) | compactions %llu resets %llu rehashes %llu\n",
                  frame, total / 3, nrec, g_rt_scur, g_rt_sdead, n1, n2, n3, pr - lp, ev - le,
                  g_rt_ev_far, g_rt_ev_frus, atomic_load(&g_rt_evsib) - lsib, g_rt_ev_cand, atomic_load(&g_rt_compact), atomic_load(&g_rt_reset), atomic_load(&g_rt_rehash));
          lp = pr; le = ev; lsib = atomic_load(&g_rt_evsib); g_rt_ev_far = g_rt_ev_frus = g_rt_ev_cand = 0;
          if (g_rt_fixcnt){ const uint32_t *fc = (const uint32_t *)[g_rt_fixcnt contents];
              char ln[512]; int o = 0;
              for (int g = 1; g < HD_MAXVSI && o < 470; g++) if (fc[2 + g * 2] || fc[3 + g * 2])
                  o += snprintf(ln + o, sizeof ln - (size_t)o, " vs_%d:%u/%u", g, fc[2 + g * 2], fc[3 + g * 2]);
              fprintf(stderr, "[RTLOG]   vertex check (total): NaN/infinite %u triangles, very far %u triangles | VS (NaN/far):%s\n", fc[0], fc[1], o ? ln : " -"); }
          { char ln[512]; int o = 0;      /* per-frame captured vertices per VS (which shader goes into the structure) */
            for (int g = 1; g < HD_MAXVSI && o < 480; g++) if (cnt[g]) o += snprintf(ln + o, sizeof ln - (size_t)o, " vs_%d:%u", g, cnt[g]);
            fprintf(stderr, "[RTLOG]   dynamic capture (VS:verts):%s\n", o ? ln : " -"); }
      } }
    /* --- GPU: capture --- */
    if (total){
        id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: vertex capture (compute)");
        [ce setBuffer:g_mtlrvb offset:0 atIndex:1];
        [ce setBuffer:g_mtlkb offset:0 atIndex:2];
        [ce setBuffer:g_rt_item[ring] offset:0 atIndex:3];
        [ce setBuffer:g_rt_rec[ring] offset:0 atIndex:4];
        [ce setBuffer:g_rt_pos offset:0 atIndex:5];
        [ce setBuffer:g_rt_attr offset:0 atIndex:6];
        [ce setBuffer:(g_pool_fr[rb] ? g_pool_fr[rb] : g_mtlrvb) offset:0 atIndex:8];   /* indexed records */
        for (int g = 1; g < HD_MAXVSI; g++){
            if (!cnt[g]) continue;
            struct { float invV[16]; float prm[4]; uint32_t first, count, p0, p1; } C;
            memcpy(C.invV, iv, sizeof C.invV);
            C.prm[0] = ipx; C.prm[1] = ipy; C.prm[2] = C.prm[3] = 0.f;
            C.first = off[g]; C.count = cnt[g]; C.p0 = C.p1 = 0;
            [ce setComputePipelineState:g_hd_vsk[g]];
            [ce setBytes:&C length:sizeof C atIndex:7];
            NSUInteger tw = g_hd_vsk[g].maxTotalThreadsPerThreadgroup;
            [ce dispatchThreadgroups:MTLSizeMake((cnt[g] + tw - 1) / tw, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        }
        if (g_rt_fixk && g_rt_fixcnt){
            [ce setComputePipelineState:g_rt_fixk];
            [ce setBuffer:g_rt_pos offset:0 atIndex:0];
            [ce setBuffer:g_rt_fixcnt offset:0 atIndex:1];
            NSUInteger tw = g_rt_fixk.maxTotalThreadsPerThreadgroup;
            for (int g = 1; g < HD_MAXVSI; g++){            /* per VS: so the counters tell which shader it is */
                if (!cnt[g]) continue;
                static float poison = -1.f;      /* HD_RT_POISON=<value>: causality test (no check is done) */
                if (poison < 0.f){ const char *ev = ENVC("HD_RT_POISON"); poison = ev ? (float)atof(ev) : 0.f; }
                struct { uint32_t ntri; float lim; uint32_t first, slot; } F = { cnt[g] / 3u, poison > 0.f ? -poison : 100000.f, off[g], (uint32_t)g };
                if (poison > 0.f && g != 3) continue;        /* only the first triangle of one VS (world) */
                [ce setBytes:&F length:sizeof F atIndex:2];
                [ce dispatchThreadgroups:MTLSizeMake((cnt[g] / 3u + tw - 1) / tw, 1, 1)
                   threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
            }
        }
        [ce endEncoding];
    }
    static int stage = -1;
    if (stage < 0){ const char *e = ENVC("HD_RT_STAGE"); stage = e ? atoi(e) : 3; }
    int st = (g_ab_mode == 3) ? 2 : (g_ab_mode == 4) ? 1 : stage;      /* HD_RT_AB slice */
    if (st == 1) return 0;
    /* --- GPU: copy to the static store + zero the evicted (degenerate triangles) --- */
    if (ncp || g_rt_nzr || g_rt_need_clear){
        id<MTLBlitCommandEncoder> be = pp_blit(cb, "RT: copy to the static store");
        if (g_rt_need_clear){ g_rt_need_clear = 0;
            [be fillBuffer:g_rt_sposb[0] range:NSMakeRange(0, RT_SMAXV * 12) value:0];
            [be fillBuffer:g_rt_sposb[1] range:NSMakeRange(0, RT_SMAXV * 12) value:0]; }
        for (unsigned q = 0; q < ncp; q++){
            [be copyFromBuffer:g_rt_pos sourceOffset:(NSUInteger)cp[q][0] * 12 toBuffer:g_rt_spos
             destinationOffset:(NSUInteger)cp[q][1] * 12 size:(NSUInteger)cp[q][2] * 12];
            [be copyFromBuffer:g_rt_attr sourceOffset:(NSUInteger)cp[q][0] * 64 toBuffer:g_rt_sattr
             destinationOffset:(NSUInteger)cp[q][1] * 64 size:(NSUInteger)cp[q][2] * 64];
        }
        for (unsigned q = 0; q < g_rt_nzr; q++)
            [be fillBuffer:g_rt_spos range:NSMakeRange((NSUInteger)g_rt_zr[q][0] * 12, (NSUInteger)g_rt_zr[q][1] * 12) value:0];
        [be endEncoding];
    }
    /* --- GPU: sub-structures + top structure (separate items in the profile) --- */
    id<MTLAccelerationStructureCommandEncoder> ae = nil;
    int have_dyn = total >= 3;
    if (have_dyn){
        dsig ^= (uint64_t)total; dsig *= 1099511628211ull;
        int refit = hd_rt_refit_on() && dsig == g_rt_dsig && g_rt_dsince < RT_REFIT_MAX;
        ae = pp_accel(cb, refit ? "RT: dynamic structure (refit)" : "RT: dynamic structure build");
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = g_rt_pos; gd.vertexStride = 12; gd.triangleCount = total / 3; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd]; ad.usage = hd_rt_dyn_usage();
        if (refit){
            [ae refitAccelerationStructure:g_rt_as descriptor:ad destination:g_rt_as
                             scratchBuffer:g_rt_scratch scratchBufferOffset:0];
            g_rt_dsince++; atomic_fetch_add(&g_rt_nrefit, 1ull);
        } else {
            [ae buildAccelerationStructure:g_rt_as descriptor:ad scratchBuffer:g_rt_scratch scratchBufferOffset:0];
            g_rt_dsince = 0; atomic_fetch_add(&g_rt_nbuild, 1ull);
        }
        g_rt_dsig = dsig;
        [ae endEncoding]; ae = nil;
    } else g_rt_dsince = 0xFFFFFFFFu;
    if (g_rt_sdirty && g_rt_scur >= 3 && (g_rt_sbuilt == 0 || frame - g_rt_last_sbuild >= RT_SREBUILD)){
        ae = pp_accel(cb, "RT: static structure build");
        MTLAccelerationStructureTriangleGeometryDescriptor *gd =
            [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        gd.vertexBuffer = g_rt_spos; gd.vertexStride = 12; gd.triangleCount = g_rt_scur / 3; gd.opaque = YES;
        MTLPrimitiveAccelerationStructureDescriptor *ad = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        ad.geometryDescriptors = @[gd]; ad.usage = MTLAccelerationStructureUsageNone;
        [ae buildAccelerationStructure:g_rt_sas descriptor:ad scratchBuffer:g_rt_sscratch scratchBufferOffset:0];
        g_rt_sbuilt = g_rt_scur; g_rt_last_sbuild = frame; g_rt_sdirty = 0;
        for (unsigned q = 0; q < RT_TABN; q++) if (g_rt_tab[q].state == 2 && g_rt_tab[q].pad) g_rt_tab[q].state = 3;
        for (unsigned q = 0; q < g_rt_mpend_n && g_rt_mfree_n < RT_SMAXE; q++) g_rt_mfree[g_rt_mfree_n++] = g_rt_mpend[q];
        g_rt_mpend_n = 0;
        atomic_fetch_add(&g_rt_sbuilds, 1ull);
        [ae endEncoding]; ae = nil;
    }
    int have_st = g_rt_sbuilt >= 3;
    if (!have_dyn && !have_st) return 0;
    ae = pp_accel(cb, "RT: top-level structure");
    { MTLAccelerationStructureInstanceDescriptor *id2 = (MTLAccelerationStructureInstanceDescriptor *)[g_rt_inst contents];
      /* instance 0 is always the structure that exists; the id (accelerationStructureIndex) tells which store it is */
      id2[0].accelerationStructureIndex = have_dyn ? 0u : 1u;
      id2[1].accelerationStructureIndex = 1u;
      MTLInstanceAccelerationStructureDescriptor *td = [MTLInstanceAccelerationStructureDescriptor descriptor];
      td.instancedAccelerationStructures = @[g_rt_as, g_rt_sas];
      td.instanceCount = (have_dyn && have_st) ? 2 : 1; td.instanceDescriptorBuffer = g_rt_inst;
      [ae buildAccelerationStructure:g_rt_tlas descriptor:td scratchBuffer:g_rt_tscratch scratchBufferOffset:0];
      g_rt_only_static = !have_dyn; }
    [ae endEncoding];
    if (st == 2) return 0;
    /* texture table */
    [g_rt_argenc setArgumentBuffer:g_rt_arg[ring] offset:0];
    for (unsigned q = 0; q < nt; q++) [g_rt_argenc setTexture:g_rt_texs[q] atIndex:q];
    *ntex = nt;
    atomic_fetch_add(&g_rt_frames, 1ull);
    atomic_fetch_add(&g_rt_tris, (unsigned long long)((total + g_rt_sbuilt) / 3));
    atomic_fetch_add(&g_rt_draws, (unsigned long long)ri);
    atomic_fetch_add(&g_rt_dynv, (unsigned long long)total);
    atomic_fetch_add(&g_rt_statv, (unsigned long long)g_rt_sbuilt);
    return (total + g_rt_sbuilt) / 3;
}
static int g_hd_dump_now;       /* this frame was picked by DUMPFRAME (diagnostic) */
static void hd_rt_trace(id<MTLCommandBuffer> cb, const void *par, size_t parsz, id<MTLTexture> zt,
                        NSUInteger W, NSUInteger H, unsigned nt){
    id<MTLComputeCommandEncoder> ce = pp_compute(cb, "RT: ray kernel (compute)");
    [ce setComputePipelineState:g_rt_kern];
    [ce setTexture:g_rt_out atIndex:0];
    [ce setTexture:zt atIndex:1];
    [ce setTexture:g_hd_g[0] atIndex:2];
    [ce setTexture:g_hd_g[2] atIndex:4];
    [ce setBytes:par length:parsz atIndex:0];
    [ce setAccelerationStructure:g_rt_tlas atBufferIndex:1];
    [ce useResource:g_rt_as usage:MTLResourceUsageRead];
    [ce useResource:g_rt_sas usage:MTLResourceUsageRead];
    [ce setBuffer:g_rt_mat[g_rt_ring] offset:0 atIndex:3];
    [ce setBuffer:g_rt_attr offset:0 atIndex:4];
    [ce setBuffer:g_rt_arg[g_rt_ring] offset:0 atIndex:6];
    [ce setBuffer:g_rt_sattr offset:0 atIndex:7];
    [ce useResources:g_rt_texs count:nt usage:MTLResourceUsageRead];
    NSUInteger tw = g_rt_kern.threadExecutionWidth, th = g_rt_kern.maxTotalThreadsPerThreadgroup / tw;
    [ce dispatchThreadgroups:MTLSizeMake((W + tw - 1) / tw, (H + th - 1) / th, 1)
       threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
    [ce endEncoding];
}
#endif /* NFS_ENHANCED: SSR, RT */
/* ---- MSAA: needless store / resolve ------------------------------------
 * Measured (PASSPROF, a 1080p race frame): the screen passes are the largest
 * item of GPU time; a frame has 5 multisampled screen passes and EACH of them
 * stored and resolved color + 3 G-buffers + Z at 4x:
 *     0  Clear only (it moves straight on to the reflection targets)
 *     13 scene (part 1)     14 = off-screen intermediate pass
 *     15 scene (part 2)     -> composite -> post-process chain
 *     39 post-process quad  40 UI
 * Three fixes:
 *  1) A screen pass that holds only a Clear is not opened (the next screen
 *     pass clears on first use anyway).
 *  2) The store action is chosen when the pass CLOSES (MTLStoreActionUnknown):
 *     if another multisampled screen pass follows, STORE only; if a copy
 *     that reads the screen (composite / post-process) or the end of the
 *     frame follows, RESOLVE only.  The G-buffer is resolved only right
 *     before the composite.
 *  3) The screen passes AFTER the first copy from the screen (post-process
 *     quad, UI) are single-sampled: the composite and post-process write the
 *     already resolved image to all samples alike, so MSAA has no information
 *     there.
 * Z was not touched (the twin sync logic is separate).  HD_MSLAZY=0 is the
 * old behavior. */
static int hd_mslazy(void){
    static int v = -1; if (v < 0){ const char *e = ENVC("HD_MSLAZY"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* When the pass OPENS: will a resolved image be needed after this pass?
 * Metal rule: if a resolve target is bound, the store action must be one that
 * includes a resolve, so the resolve decision must be made at open (the store
 * at close).  Returns: bit0 color must be resolved, bit1 a screen copy reads
 * it (G must be resolved too). */
static unsigned hd_ms_predict(int rb, unsigned i){
    unsigned n = g_dn[rb], zid = DEPTH_ZID(g_dbuf[rb][i].depth), j = i + 1;
    if (!hd_mslazy()) return 3u;
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];                 /* end of the pass */
        if (x->is_blit || x->target || DEPTH_ZID(x->depth) != zid) break;
        if (x->is_clear && (x->clr_flags & 2u) && zid) break; }
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];
        if (x->is_blit){
            if (!x->blit_src) return 3u;                                  /* the screen is read */
            if (!x->target) return 1u;                                    /* copy to the screen: be careful */
            continue; }
        if (!x->target) return 0u;                                        /* another multisampled screen pass */
    }
    return 1u;                                                            /* end of frame: present */
}
/* Can this multisampled screen pass open with MEMORYLESS attachments (the
 * caller checks the load)?  If, after the pass ends and before the screen is
 * read (composite / post-process copy), the same frame has no other screen
 * pass and no copy to the screen, no sample needs to be stored.  The body
 * detection is the same as hd_ms_predict; the encode loop closes the pass at
 * the same places too. */
static int hd_ms_memless_ok(int rb, unsigned i){
    unsigned n = g_dn[rb], zid = DEPTH_ZID(g_dbuf[rb][i].depth), j = i + 1;
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];                 /* end of the pass */
        if (x->is_blit || x->target || DEPTH_ZID(x->depth) != zid) break;
        if (x->is_clear && (x->clr_flags & 2u) && zid) break; }
    for (; j < n; j++){ const MDraw *x = &g_dbuf[rb][j];
        if (x->is_blit){
            if (!x->blit_src) return 1;                                   /* the screen is read: single-sample after that */
            if (!x->target) return 0;                                     /* copy to the screen: stored to MS color */
            continue; }
        if (!x->target) return 0;                                         /* another screen pass: it loads */
    }
    return 1;                                                             /* end of frame: resolve only */
}
static int hd_ms_memless(void){ return hd_ml_hw() && !g_hd_ml_off && hd_mslazy(); }
static int hd_mszstore_all(void){              /* HD_MSZSTORE=1: MS depth always stored (old) */
    static int v = -1; if (v < 0){ const char *e = ENVC("HD_MSZSTORE"); v = (e && *e == '1'); }
    return v;
}
/* When the pass CLOSES: must the samples be stored?  Returns: if a resolve is
 * needed although none was attached at open (the guess missed), bit0 color /
 * bit1 G: the caller encodes a separate resolve pass with hd_ms_fallback
 * (its counter is in the report; should be 0). */
static _Atomic unsigned long long g_hd_msfb;
static int hd_ms_end(id<MTLRenderCommandEncoder> enc, int rb, unsigned k, int c_ms, int c_gb, unsigned att){
    if (!c_ms || !hd_mslazy()){ g_hd_msp.on = 0; return 0; }
    unsigned n = g_dn[rb], k0 = k; int store = 0, res = 0, reader = 0, pend = 0;
    for (; k < n; k++){ const MDraw *x = &g_dbuf[rb][k];
        if (x->is_blit){
            if (!x->blit_src){ res = 1; reader = 1; break; }
            if (!x->target){ res = 1; store = 1; break; }
            continue; }
        if (!x->target){ store = 1; break; }
    }
    if (k >= n) res = 1;
    if (g_hd_msp.on && g_hd_msp.ml){
        /* Memoryless pass: the actions were fixed at open (resolve only, or
         * DontCare; set*StoreAction may only be called on a pass opened
         * with Unknown).  Here we only check that the prediction held: if
         * it did not, the samples are lost this frame (the next pass opens
         * cleared) and memoryless mode is switched off for good. */
        if (store || !(att & 1u) || (reader && c_gb && !(att & 2u))){
            atomic_fetch_add(&g_hd_ml_miss, 1ull); g_hd_ml_off = 1; }
        g_hd_msp.on = 0;
        return 0;
    }
    if (att & 1u) [enc setColorStoreAction:store ? MTLStoreActionStoreAndMultisampleResolve
                                                 : MTLStoreActionMultisampleResolve atIndex:0];
    else { [enc setColorStoreAction:(store || res) ? MTLStoreActionStore : MTLStoreActionDontCare atIndex:0];
           if (res) pend |= 1; }
#if NFS_ENHANCED
    if (c_gb){
        MTLStoreAction g;
        if (att & 2u) g = store ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
        else { g = (store || reader) ? MTLStoreActionStore : MTLStoreActionDontCare; if (reader) pend |= 2; }
        for (int gi = 0; gi < HD_NG; gi++) [enc setColorStoreAction:g atIndex:(NSUInteger)(1 + gi)];
    }
#endif
    /* MS DEPTH: used to be an unconditional Store -- 133 MB of memory writes
     * per frame at 4K (~12 GB/s at ~95 fps).  These samples are only loaded
     * by a later multisampled screen pass opened with the same Z; the
     * measured frames have no such pass and the game clears Z every frame.
     * Now: store if it is reused in the same frame; do not store if it is
     * cleared or the screen was read / the frame ended -- but if the pass
     * was opened without a clear (with a load), the game relies on
     * persistence, so store. */
    if (g_hd_msp.on && g_hd_msp.dz){
        unsigned zid = g_hd_msp.zid; int sd = -1;
        for (unsigned q = k0; q < n && sd < 0; q++){ const MDraw *x = &g_dbuf[rb][q];
            if (x->is_blit){ if (!x->blit_src) break; continue; }        /* screen was read: single-sampled after this */
            if (DEPTH_ZID(x->depth) != zid) continue;
            if (x->is_clear && (x->clr_flags & 2u)) sd = 0;               /* cleared before reuse */
            else if (!x->target) sd = 1;                                  /* a new MS pass with the same Z loads it */
        }
        if (sd < 0) sd = !g_hd_msp.zclr;
        if (hd_mszstore_all()) sd = 1;
        [enc setDepthStoreAction:g_hd_msp.res ? (sd ? MTLStoreActionStoreAndMultisampleResolve
                                                    : MTLStoreActionMultisampleResolve)
                                              : (sd ? MTLStoreActionStore : MTLStoreActionDontCare)];
        if (zid < 64u) g_hd_msz_kept[zid] = (uint8_t)sd;
        atomic_fetch_add(sd ? &g_hd_mszst : &g_hd_mszskip, 1ull);
    }
    g_hd_msp.on = 0;
    return pend;
}
static void hd_ms_fallback(id<MTLCommandBuffer> cb, id<MTLTexture> scr, int pend){
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (!g_hd_mscol) return;
    rp.colorAttachments[0].texture = g_hd_mscol; rp.colorAttachments[0].resolveTexture = scr;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
#if NFS_ENHANCED
    if (pend & 2) for (int gi = 0; gi < HD_NG; gi++){
        if (!g_hd_gms[gi] || !g_hd_g[gi]) continue;
        rp.colorAttachments[1 + gi].texture = g_hd_gms[gi]; rp.colorAttachments[1 + gi].resolveTexture = g_hd_g[gi];
        rp.colorAttachments[1 + gi].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[1 + gi].storeAction = MTLStoreActionStoreAndMultisampleResolve; }
#endif
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    atomic_fetch_add(&g_hd_msfb, 1ull);
}
#if NFS_ENHANCED
/* End-of-scene composite: ray tracing (if available) or SSR.  i = the draw
 * index where the scene ends; scr = the resolved screen texture (drawable). */
static void hd_composite(id<MTLCommandBuffer> cb, int rb, unsigned i, id<MTLTexture> scr,
                         unsigned zid, int refl_drawn){
    id<MTLTexture> zt = (zid && zid < MAXZ) ? g_ztex[zid] : nil;
    int ms = hd_msaa_n() > 1 && g_hd_mscol && !hd_mslazy();     /* in lazy mode it is single-sampled after this */
    id<MTLRenderPipelineState> sp = ms ? g_hd_ssr_pso_ms : g_hd_ssr_pso;
    float ivp[16], vp[16], iv[16];
    if (!refl_drawn || !g_hd_cam_ok[rb] || !zt || !sp || !g_hd_g[0] ||
        zt.width != scr.width || zt.height != scr.height) return;
    memcpy(vp, &g_hd_cam[rb][32], sizeof vp);
    for (int r4 = 0; r4 < 4; r4++) vp[r4*4] *= g_hd_cam_k[rb];   /* Hor+ */
    if (!hd_inv4(vp, ivp) || !hd_inv4(&g_hd_cam[rb][0], iv)) return;

    struct { float vp[16], ivp[16], cam[4], prm[4], sz[4], cam2[4], cb[3][4], fogp[4]; } sp2;
    memset(sp2.fogp, 0, sizeof sp2.fogp);
    static float cl = -1.f, st = -1.f, md = -1.f, rmd = -1.f, rlk = -1.f;
    if (cl < 0.f){ const char *e = ENVC("HD_SSR_CARLOD"); cl = e ? (float)atof(e) : 1.0f;
                   e = ENVC("HD_SSR_STEPS"); st = e ? (float)atof(e) : 48.f;
                   e = ENVC("HD_SSR_MAXD"); md = e ? (float)atof(e) : 120.f;
                   e = ENVC("HD_RT_MAXD"); rmd = e ? (float)atof(e) : 600.f;
                   e = ENVC("HD_RT_LOD"); rlk = e ? (float)atof(e) : 0.05f; }
    memcpy(sp2.vp, vp, sizeof vp); memcpy(sp2.ivp, ivp, sizeof ivp);
    sp2.cam[0] = iv[12]; sp2.cam[1] = iv[13]; sp2.cam[2] = iv[14];
    sp2.cam[3] = ENVC("HD_SSR_DEBUG") ? 1.f + (float)atoi(ENVC("HD_SSR_DEBUG")) : 1.f;
    sp2.prm[0] = g_hd_cam[rb][16 + 10];       /* P[2][2] */
    sp2.prm[1] = g_hd_cam[rb][16 + 14];       /* P[3][2] */
    sp2.prm[2] = 1.f; sp2.prm[3] = md;
    sp2.sz[0] = (float)scr.width; sp2.sz[1] = (float)scr.height; sp2.sz[2] = st; sp2.sz[3] = 0.6f;
    sp2.cam2[0] = cl; sp2.cam2[1] = rmd; sp2.cam2[2] = rlk;
    sp2.cam2[3] = ENVC("HD_RT_PLAIN") ? 2.f : ENVC("HD_RT_NOTEX") ? 1.f : 0.f;     /* diagnostic */
    if (g_ab_mode == 2) sp2.cam2[3] = 2.f;
    for (int a3 = 0; a3 < 3; a3++){ memcpy(sp2.cb[a3], &g_hd_cube[rb][a3*3], 12); sp2.cb[a3][3] = 0.f; }
    static int rsc_env = -1;  /* HD_RT_SCALE=1..4: ray buffer divisor (1 = full resolution) */
    /* If not given, AUTOMATIC: the ray count must not grow with resolution (2 at
     * 1080p, 3 at 2560 and 3024 wide, 4 at 4K -> always a ray buffer ~1000 pixels
     * wide).  Measured (3024x1964, dry weather): tracing + shading ~6 ms at scale 2. */
    if (rsc_env < 0){ const char *e = ENVC("HD_RT_SCALE"); int v = e ? atoi(e) : 0; rsc_env = (v >= 1 && v <= 4) ? v : 0; }
    int rsc = rsc_env;
    if (!rsc){ int a = (int)((scr.width + 480) / 960); rsc = a < 2 ? 2 : a > 4 ? 4 : a; }
    sp2.cb[0][3] = (float)rsc;

    /* ray tracing: capture + build the structure + cast the rays (SSR on failure) */
    unsigned rt_tris = 0, rt_nt = 0;
    if (hd_rt_on() && g_ab_mode != 5 && g_ab_mode != 6){
        hd_rt_init();
        float p00 = g_hd_cam[rb][16] * g_hd_cam_k[rb], p11 = g_hd_cam[rb][16 + 5];
        if (g_rt_pso && p00 != 0.f && p11 != 0.f)
            { rt_tris = hd_rt_frame(cb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &rt_nt, sp2.fogp); sp2.cb[1][3] = (float)g_rt_only_static; }
        if (rt_tris){
            NSUInteger ow = (scr.width + rsc - 1) / rsc, oh = (scr.height + rsc - 1) / rsc;
            if (!g_rt_out || g_rt_out.width != ow || g_rt_out.height != oh){
                MTLTextureDescriptor *od = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                    width:ow height:oh mipmapped:NO];
                od.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
                od.storageMode = MTLStorageModePrivate;
                g_rt_out = [g_dev newTextureWithDescriptor:od];
            }
            /* HD_RTPROBE2 (on the DUMPFRAME frame): the SAME kernel + the SAME data,
             * timed alone OUTSIDE the frame.  For comparison with the in-frame cost. */
            if (g_hd_dump_now && ENVC("HD_RTPROBE2")){
                static int once = 0;
                if (!once){ once = 1;
                    id<MTLCommandBuffer> pb = [g_queue commandBuffer]; unsigned nt2 = 0;
                    unsigned t2 = hd_rt_frame(pb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &nt2, sp2.fogp);
                    [pb commit]; [pb waitUntilCompleted];
                    fprintf(stderr, "[RT2] capture+build (separate buffer): %.2f ms, %u triangles\n",
                            (pb.GPUEndTime - pb.GPUStartTime) * 1e3, t2);
                    double best = 1e30, sum = 0;
                    for (int it = 0; it < 9; it++){
                        id<MTLCommandBuffer> tb = [g_queue commandBuffer];
                        hd_rt_trace(tb, &sp2, sizeof sp2, zt, ow, oh, nt2);
                        [tb commit]; [tb waitUntilCompleted];
                        double msx = (tb.GPUEndTime - tb.GPUStartTime) * 1e3;
                        if (it == 0) continue;
                        if (msx < best) best = msx; sum += msx;
                    }
                    fprintf(stderr, "[RT2] ray kernel (outside the frame, alone): best %.2f ms, avg %.2f ms\n", best, sum / 8);
                    { rt_tris = hd_rt_frame(cb, rb, i, iv, vp, ivp, 1.f / p00, 1.f / p11, &rt_nt, sp2.fogp); sp2.cb[1][3] = (float)g_rt_only_static; }
                }
            }
            /* --- HALF RAYS -----------------------------------------------------
             * The ray kernel (~2.5 ms at 3024x1964) is the largest item of the
             * reflection system.  Each frame traces only one half of a
             * checkerboard; the other half is the previous frame's result left
             * in the ray texture (8 ms old at 120 fps).  The composite's 4-tap
             * upscale already blends the two.  A newly created texture is
             * traced in full that frame.  HD_RT_HALF=0 is the old behavior. */
            static int half = -1; static unsigned hpar; static id<MTLTexture> hseen;
            if (half < 0){ const char *e = ENVC("HD_RT_HALF"); half = (e && *e == '0') ? 0 : 1; }
            NSUInteger tw2 = ow;
            if (half && hseen == g_rt_out){ sp2.cb[2][3] = (float)(1u + (hpar++ & 1u)); tw2 = (ow + 1) / 2; }
            hseen = g_rt_out;
            hd_rt_trace(cb, &sp2, sizeof sp2, zt, tw2, oh, rt_nt);
            sp2.cb[2][3] = 0.f;
            sp = ms ? g_rt_pso_ms : g_rt_pso;
        }
    }
    /* HD_COMP=0 (diagnostic): skip the scene copy + composite pass.  Together with
     * HD_RT_STAGE, to measure the GPU cost of each step (no reflection in the image). */
    { static int comp = -1; if (comp < 0){ const char *e = ENVC("HD_COMP"); comp = (e && *e == '0') ? 0 : 1; }
      if (!comp) return;
      if (g_ab_mode == 1 || g_ab_mode == 2 || g_ab_mode == 3 || g_ab_mode == 4 || g_ab_mode == 6) return; }
    int fb = rt_tris && !ms && g_rt_pso_fb;        /* see COPY-FREE COMPOSITE */
    if (fb){
        MTLRenderPassDescriptor *sr = [MTLRenderPassDescriptor renderPassDescriptor];
        sr.colorAttachments[0].texture = scr;
        sr.colorAttachments[0].loadAction = MTLLoadActionLoad;
        sr.colorAttachments[0].storeAction = MTLStoreActionStore;
        pp_rp(sr, "composite: RT (copy-free)");
        id<MTLRenderCommandEncoder> se = [cb renderCommandEncoderWithDescriptor:sr];
        [se setRenderPipelineState:g_rt_pso_fb];
        [se setFragmentTexture:zt atIndex:1];
        [se setFragmentTexture:g_hd_g[0] atIndex:2];
        [se setFragmentTexture:g_hd_g[1] atIndex:3];
        [se setFragmentTexture:g_hd_g[2] atIndex:4];
        [se setFragmentTexture:g_rt_out atIndex:5];
        [se setFragmentBytes:&sp2 length:sizeof sp2 atIndex:0];
        [se drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [se endEncoding];
        atomic_fetch_add(&g_hd_ssr_n, 1ull);
        return;
    }
    if (!g_hd_scenecopy || g_hd_scenecopy.width != scr.width || g_hd_scenecopy.height != scr.height){
        MTLTextureDescriptor *cd = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:scr.pixelFormat
            width:scr.width height:scr.height mipmapped:YES];
        cd.mipmapLevelCount = 4;     /* SSR car: slightly soft reflection */
        cd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        cd.storageMode = MTLStorageModePrivate;
        g_hd_scenecopy = [g_dev newTextureWithDescriptor:cd];
    }
    id<MTLBlitCommandEncoder> sc = pp_blit(cb, "composite: scene copy");
    [sc copyFromTexture:scr sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
             sourceSize:MTLSizeMake(scr.width, scr.height, 1)
              toTexture:g_hd_scenecopy destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0,0,0)];
    if (!rt_tris) [sc generateMipmapsForTexture:g_hd_scenecopy];     /* only SSR reads them */
    [sc endEncoding];
    MTLRenderPassDescriptor *sr = [MTLRenderPassDescriptor renderPassDescriptor];
    if (ms){ sr.colorAttachments[0].texture = g_hd_mscol;
             sr.colorAttachments[0].resolveTexture = scr;
             sr.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve; }
    else { sr.colorAttachments[0].texture = scr;
           sr.colorAttachments[0].storeAction = MTLStoreActionStore; }
    sr.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pp_rp(sr, rt_tris ? "composite: RT" : "composite: SSR");
    id<MTLRenderCommandEncoder> se = [cb renderCommandEncoderWithDescriptor:sr];
    [se setRenderPipelineState:sp];
    [se setFragmentTexture:g_hd_scenecopy atIndex:0];
    [se setFragmentTexture:zt atIndex:1];
    [se setFragmentTexture:g_hd_g[0] atIndex:2];
    [se setFragmentTexture:g_hd_g[1] atIndex:3];
    [se setFragmentTexture:g_hd_g[2] atIndex:4];
    if (rt_tris) [se setFragmentTexture:g_rt_out atIndex:5];
    [se setFragmentBytes:&sp2 length:sizeof sp2 atIndex:0];
    [se drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [se endEncoding];
    atomic_fetch_add(&g_hd_ssr_n, 1ull);
}
/* tex_new_id: a recycled id must not inherit HD per-texture state. */
static void hd_tex_forget(unsigned id_){
    g_hd_road[id_] = 0; g_hd_dirty[id_] = 0; g_hd_ow[id_] = 0; g_hd_oh[id_] = 0;
    g_rt_tstamp[id_] = 0; g_rt_tslot[id_] = 0;
}
#else
static void hd_tex_forget(unsigned id_){ (void)id_; }
#endif
/* --- PASS MERGING (both builds) ---------------------------------------------
 * At 3024x1964 the normal build drew the screen in 5 separate passes per frame
 * (PASSPROF content dump, 2026-10-15): an empty pass holding only a Clear, the
 * main scene, a second scene pass with a SINGLE draw because the light-flare
 * target came in between, the post-processing quad, and a separate UI pass
 * only because the Z binding changed.  Every separate pass reads and writes
 * full-screen color (+ Z) (~24 MB each).  Three changes, none of which alters
 * a pixel:
 *  1) pass_reorder: the pass that splits the scene is deferred (below, with
 *     its rules).
 *  2) pass_clear_only: a screen pass holding only a Clear is not opened; the
 *     screen is cleared with the same color on its first real use anyway,
 *     and the Z clear waits in g_zpend.
 *  3) A draw that neither tests nor writes Z does not open a new pass just
 *     because the Z surface differs.
 * 1 and 2 used to be HD only.  PASSMERGE=0 turns off all three. */
static int pass_merge_on(void){
    static int v = -1;
    if (v < 0){ const char *e = ENVC("PASSMERGE"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* ---- DEFER THE PASS THAT SPLITS THE SCENE ------------------------------
 * Measured (PASSPROF content dump, race frame): the main scene is 674 draws
 * in one screen pass; then an off-screen pass of 23 draws (light flares:
 * they TEST the scene's Z, do not write Z), then a SINGLE draw to the
 * screen, then post-processing.  That single draw made the scene open a
 * second screen pass: 4x color + 3 G-buffers were stored and loaded back
 * (~200 MB of writes + ~200 MB of reads).
 * Pattern:  ... screen | R = off-screen run (one target T) | S = screen run ...
 * If R does not write / clear Z and S does not write Z, does not sample T
 * and uses the same Z surface, the two runs swap places: S writes the
 * screen's color (R cannot read it), R writes only T (S does not read it),
 * neither changes Z -> the result is identical pixel for pixel, but the
 * scene is ONE pass.  HD_REORDER=0 turns it off. */
static _Atomic unsigned long long g_hd_reord;
static void pass_reorder(int rb){
    static int on = -1; if (on < 0){ const char *e = ENVC("HD_REORDER"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !pass_merge_on()) return;
    unsigned n = g_dn[rb]; MDraw *D = g_dbuf[rb];
    static MDraw tmp[64];
    for (unsigned a = 1; a + 1 < n; a++){
        const MDraw *p = &D[a - 1];
        if (p->target || p->is_blit || p->is_clear) continue;                 /* previous: a screen draw */
        if (!D[a].target || D[a].is_blit || D[a].is_clear) continue;          /* a: start of the off-screen run */
        unsigned T = D[a].target, tid = TGT_ID(T), zs = DEPTH_ZID(p->depth), b = a, c;
        int ok = 1;
        for (; b < n; b++){ const MDraw *x = &D[b];
            if (x->target != T || x->is_blit) break;
            if (x->is_clear || (x->depth & 2u)){ ok = 0; break; } }
        if (!ok || b >= n || b - a > 512u) continue;
        for (c = b; c < n && c - b < 64u; c++){ const MDraw *x = &D[c];
            if (x->target || x->is_blit || x->is_clear) break;
            if (DEPTH_ZID(x->depth) != zs || (x->depth & 2u) ||
                x->tex == tid || x->tex1 == tid || x->tex2 == tid || x->tex3 == tid){ ok = 0; break; } }
        if (!ok || c == b) continue;
        if (c < n && !D[c].target && !D[c].is_blit && !D[c].is_clear) continue;   /* S exceeded 64: no gain */
        unsigned ns = c - b, nr = b - a;
        memcpy(tmp, &D[b], (size_t)ns * sizeof(MDraw));
        memmove(&D[a + ns], &D[a], (size_t)nr * sizeof(MDraw));
        memcpy(&D[a], tmp, (size_t)ns * sizeof(MDraw));
        atomic_fetch_add(&g_hd_reord, 1ull);
        a = c - 1;
    }
}
/* 1 if the draw at i would open a screen pass holding only a Clear: the first
 * real work after it is on another target, and the screen is drawn to again
 * before it is read. */
static int pass_clear_only(int rb, unsigned i){
    unsigned n = g_dn[rb], j = i;
    const MDraw *d0 = &g_dbuf[rb][i];
    if (!hd_mslazy()) return 0;
    if (!pass_merge_on() || d0->target || !d0->is_clear || d0->is_blit) return 0;
    while (j < n && g_dbuf[rb][j].is_clear && !g_dbuf[rb][j].target && !g_dbuf[rb][j].is_blit) j++;
    if (j >= n) return 0;
    if (!g_dbuf[rb][j].target) return 0;                          /* the pass continues with a real draw (or a screen blit) */
    for (unsigned k = j; k < n; k++){ const MDraw *x = &g_dbuf[rb][k];
        if (x->is_blit){ if (!x->blit_src || !x->target) return 0; continue; }
        if (!x->target) return 1;
    }
    return 0;
}

/* A REAL cube texture.  The game draws the car environment map to the 6
 * FACES one by one (measured: SetRenderTarget with 6 different cube
 * surfaces), then binds the texture to stage 0 and samples the reflection.
 * This used to create a plain 2D texture, so all six faces were written on
 * top of each other into the same surface. */
unsigned metal_create_texture_cube(unsigned edge, unsigned levels,
                                   unsigned d3dfmt, unsigned is_rt){
    if (!g_dev || (g_texn >= MAXTEX && !g_ntfree)) return 0;
    unsigned blk, bpb;
    MTLPixelFormat pf = d3dfmt_to_mtl(d3dfmt, &blk, &bpb);
    if (pf == MTLPixelFormatInvalid) return 0;
    if (!levels) levels = 1;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pf
                              size:edge mipmapped:(levels > 1)];
    td.mipmapLevelCount = levels;
    td.usage = MTLTextureUsageShaderRead | (is_rt ? MTLTextureUsageRenderTarget : 0);
    /* NOT Private: the game also writes the cube faces with
     * LockRect/UnlockRect, and replaceRegion is invalid on a Private
     * texture (measured: _validateReplaceRegion assert, crash in frame 9). */
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    if (!t) return 0;
    tex_zero_all(t, edge, edge, levels, 6, blk, bpb);
    unsigned id_ = tex_new_id();
    if (!id_) return 0;
    g_tex[id_] = t;
    g_tex_cube[id_] = 1;
    g_tex_isrt[id_] = is_rt ? 1 : 0;
    atomic_fetch_add(&g_tex_created, 1); if (g_perflog) perflog_event(PL_TEX, 0, 0);
    return id_;
}
int metal_tex_is_cube(unsigned id_){
    return (id_ && id_ < MAXTEX) ? g_tex_cube[id_] : 0;
}
/* texrep.m: the device (loader queue) and a texture by id (main thread) */
_Static_assert(MAXTEX == TEXREP_MAXTEX, "texrep.h TEXREP_MAXTEX");
id<MTLDevice> metal_device(void){ return g_dev; }
id<MTLTexture> metal_tex_object(unsigned id_){ return (id_ && id_ < MAXTEX) ? g_tex[id_] : nil; }

void metal_upload_texture_face(unsigned id_, unsigned level, unsigned face,
                               const void *data, unsigned pitch,
                               unsigned w, unsigned h, unsigned d3dfmt){
    if (!id_ || id_ >= MAXTEX || !g_tex[id_] || !data) return;
    unsigned blk, bpb;
    if (d3dfmt_to_mtl(d3dfmt, &blk, &bpb) == MTLPixelFormatInvalid) return;
    unsigned lw = w >> level, lh = h >> level;
    if (!lw) lw = 1; if (!lh) lh = 1;
    MTLRegion rg = MTLRegionMake2D(0, 0, lw, lh);
    struct timespec _u0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_u0);
    if (g_tex_cube[id_]){
        if (face > 5u) face = 0u;
        [g_tex[id_] replaceRegion:rg mipmapLevel:level slice:face
                        withBytes:data bytesPerRow:pitch bytesPerImage:0];
    } else {
        [g_tex[id_] replaceRegion:rg mipmapLevel:level withBytes:data bytesPerRow:pitch];
    }
    if (g_perflog){ struct timespec _u1; clock_gettime(CLOCK_MONOTONIC, &_u1);
        perflog_event(PL_TEXUP, (uint64_t)(_u1.tv_sec - _u0.tv_sec) * 1000000000ull + (uint64_t)_u1.tv_nsec - (uint64_t)_u0.tv_nsec,
                      (uint64_t)pitch * (blk == 1 ? lh : (lh + blk - 1) / blk)); }
    if (level < 16u) g_tex_lvlmask[id_] |= 1u << level;
    atomic_fetch_add(&g_tex_uploaded, 1);
}
void metal_upload_texture(unsigned id_, unsigned level, const void *data,
                          unsigned pitch, unsigned w, unsigned h, unsigned d3dfmt){
    metal_upload_texture_face(id_, level, 0u, data, pitch, w, h, d3dfmt);
}
/* Compare the declared mip count with the levels ACTUALLY uploaded.
 * A level never uploaded = undefined memory; when that level is picked at
 * triangle edges it shows on screen as 1-pixel boxes/diagonals. */
void metal_miplevel_report(void){
    unsigned bad = 0, tot = 0;
    for (unsigned i = 1; i < MAXTEX && i < g_texn; i++){
        if (!g_tex[i]) continue;
        unsigned lv = (unsigned)g_tex[i].mipmapLevelCount;
        if (lv <= 1) continue;
        tot++;
        unsigned want = (lv >= 16u) ? 0xFFFFu : ((1u << lv) - 1u);
        if ((g_tex_lvlmask[i] & want) != want){
            if (bad < 6)
                fprintf(stderr,"    texture %u: %u levels declared, uploaded mask 0x%04x\n",
                        i, lv, g_tex_lvlmask[i]);
            bad++;
        }
    }
    fprintf(stderr,"  mip levels        : %u multi-level textures, %u of them INCOMPLETE\n", tot, bad);
}
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned d3dfmt){
    unsigned blk, bpb;
    if (d3dfmt_to_mtl(d3dfmt, &blk, &bpb) == MTLPixelFormatInvalid) return 0;
    unsigned lw = w >> level; if (!lw) lw = 1;
    if (blk == 1) return lw * bpb;
    return ((lw + blk - 1) / blk) * bpb;
}
unsigned metal_level_rows(unsigned h, unsigned level, unsigned d3dfmt){
    unsigned blk, bpb;
    if (d3dfmt_to_mtl(d3dfmt, &blk, &bpb) == MTLPixelFormatInvalid) return 0;
    unsigned lh = h >> level; if (!lh) lh = 1;
    if (blk == 1) return lh;
    return (lh + blk - 1) / blk;
}

/* D3DCOLOR (0xAARRGGBB, in memory B,G,R,A) -> uchar4 */
/* prim: 4=TRIANGLELIST 5=TRIANGLESTRIP 6=TRIANGLEFAN */
static NSString *kPrelude = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VOut { float4 pos [[position]]; float4 col0; float4 col1;\n"
"              float4 uv0; float4 uv1; float4 uv2; float4 uv3;\n"
"              float fog; };\n"
"struct PSPar { float aref; float usec; float pad0; float pad1;\n"
"               float4 proj; float4 c[32]; float4 fog; };\n"
#if NFS_ENHANCED
/* HD: color + G-buffer (c1..c3 = G0..G2; contents: see G-BUFFER PACKING) */
"struct PSOut { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]];\n"
"               float4 c3 [[color(3)]]; };\n"
/* ray-tracing capture (must match vsemu.c vsk_N and metal.m HDRecC/HDMatC) */
"struct HDRec { uint rawoff; uint koff; uint first; uint mat; uint ib; int base; uint isz; uint pad; };\n"
"struct HDAttr { float4 col0; float4 uv1; float4 uv2; float2 uv0; float fog; float pad; };\n"
"struct HDCap { float4x4 invV; float4 prm; uint first; uint count; uint p0; uint p1; };\n"
#endif
;

static NSString *kShaderSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn  { float4 pos [[attribute(0)]]; float4 col0 [[attribute(1)]];\n"
"              float4 col1 [[attribute(2)]];\n"
"              float4 uv0 [[attribute(3)]]; float4 uv1 [[attribute(4)]];\n"
"              float4 uv2 [[attribute(5)]]; float4 uv3 [[attribute(6)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 col0; float4 col1;\n"
"              float4 uv0; float4 uv1; float4 uv2; float4 uv3;\n"
"              float fog; };\n"
"struct PSPar { float aref; float usec; float pad0; float pad1;\n"
"               float4 proj; float4 c[32]; float4 fog; };\n"
"vertex VOut vmain(VIn v [[stage_in]]) {\n"
"  VOut o;\n"
"  o.pos = v.pos; o.col0 = v.col0; o.col1 = v.col1;\n"
"  o.uv0 = v.uv0; o.uv1 = v.uv1; o.uv2 = v.uv2; o.uv3 = v.uv3;\n"
"  o.fog = 1.0;\n"
"  return o;\n"
"}\n"
"fragment float4 fmain(VOut i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
"                      texture2d<float> t1 [[texture(1)]],\n"
"                      texture2d<float> t2 [[texture(2)]],\n"
"                      texture2d<float> t3 [[texture(3)]],\n"
"                      sampler sm [[sampler(0)]],\n"
"                      constant PSPar &P [[buffer(0)]]) {\n"
"  float4 c = i.col0 * t.sample(sm, i.uv0.xy);\n"
"  if (c.a < P.aref) discard_fragment();\n"
"  c.rgb = mix(P.fog.rgb, c.rgb, mix(1.0, saturate(i.fog), P.fog.a));\n"
"  return c;\n"
"}\n"
"fragment float4 fmain_nd(VOut i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
"                      texture2d<float> t1 [[texture(1)]],\n"
"                      texture2d<float> t2 [[texture(2)]],\n"
"                      texture2d<float> t3 [[texture(3)]],\n"
"                      sampler sm [[sampler(0)]],\n"
"                      constant PSPar &P [[buffer(0)]]) {\n"
"  float4 c = i.col0 * t.sample(sm, i.uv0.xy);\n"
"  c.rgb = mix(P.fog.rgb, c.rgb, mix(1.0, saturate(i.fog), P.fog.a));\n"
"  return c;\n"
"}\n";

void metal_draw_tv(const TVert *src, unsigned nverts, unsigned prim, unsigned tex){
    if (!g_vbuf[0] || nverts < 3) return;
    unsigned tris = (prim == 4) ? nverts/3 : nverts - 2;
    if (!tris) return;
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_vn[b] + tris*3 <= MAXV && g_dn[b] < MAXD){
        MVert *dst = g_vbuf[b] + g_vn[b];
        for (unsigned t = 0; t < tris; t++){
            unsigned idx[3];
            if (prim == 6){ idx[0]=0;     idx[1]=t+1; idx[2]=t+2; }        /* FAN   */
            else if (prim == 5){                                          /* STRIP */
                if (t & 1){ idx[0]=t+1; idx[1]=t;   idx[2]=t+2; }
                else      { idx[0]=t;   idx[1]=t+1; idx[2]=t+2; }
            }
            else { idx[0]=t*3; idx[1]=t*3+1; idx[2]=t*3+2; }              /* LIST  */
            for (int k = 0; k < 3; k++) *dst++ = src[idx[k]];
        }
        /* CAREFUL: the draw buffer is reused ACROSS FRAMES.
         * If a new field is added and not assigned here, the previous
         * frame's value stays.  is_blit was such a field: a slot that was
         * a blit once counted as a blit forever and replaced the real
         * draw with a stale texture (the screen turned into a mosaic
         * over time).  Zero the whole record before filling it. */
        memset(&g_dbuf[b][g_dn[b]], 0, sizeof g_dbuf[b][g_dn[b]]);
        g_dbuf[b][g_dn[b]].first  = g_vn[b];
        g_dbuf[b][g_dn[b]].count  = tris*3;
        g_dbuf[b][g_dn[b]].tex    = tex;
        g_dbuf[b][g_dn[b]].target = g_cur_target;
        g_dbuf[b][g_dn[b]].is_clear = 0;
        g_dbuf[b][g_dn[b]].blend = g_cur_blend;
        g_dbuf[b][g_dn[b]].bcol  = g_cur_bcol;
        g_dbuf[b][g_dn[b]].cull  = g_cur_cull;
        g_dbuf[b][g_dn[b]].depth = g_cur_depth;
        g_dbuf[b][g_dn[b]].tex1 = g_cur_tex1;
        g_dbuf[b][g_dn[b]].tex2 = g_cur_tex2;
        g_dbuf[b][g_dn[b]].tex3 = g_cur_tex3;
        g_dbuf[b][g_dn[b]].ps   = (g_cur_ps > 0 && g_cur_ps < PSCUBE &&
                                   metal_tex_is_cube(tex))
                                    ? g_cur_ps + PSCUBE : g_cur_ps;
#if NFS_ENHANCED
        { unsigned tx4[4] = { tex, g_cur_tex1, g_cur_tex2, g_cur_tex3 };
          g_dbuf[b][g_dn[b]].ps = hd_refl_ps(g_dbuf[b][g_dn[b]].ps, g_cur_target, tx4); }
#endif
        g_dbuf[b][g_dn[b]].psci = psc_put(b);
        memcpy(g_dbuf[b][g_dn[b]].c0, g_cur_psc[0], sizeof g_cur_psc[0]);
        g_dbuf[b][g_dn[b]].alpha_ref = g_cur_aref;
        memcpy(g_dbuf[b][g_dn[b]].fogv, g_cur_fog, sizeof g_cur_fog);
        memcpy(g_dbuf[b][g_dn[b]].proj, g_cur_proj, sizeof g_cur_proj);
        g_dbuf[b][g_dn[b]].vsid = 0;
        g_dbuf[b][g_dn[b]].rawoff = 0;
        g_dbuf[b][g_dn[b]].koff = 0;
        memcpy(g_dbuf[b][g_dn[b]].smp, g_cur_smp, sizeof g_cur_smp);
        g_dn[b]++;
        g_vn[b] += tris*3;
    }
    pthread_mutex_unlock(&g_qlock);
}

/* --- guest-side API (may be called from any thread) --- */
void metal_set_clear_color(unsigned argb){
    atomic_store(&g_clear_r, ((argb >> 16) & 0xFF) / 255.0f);
    atomic_store(&g_clear_g, ((argb >>  8) & 0xFF) / 255.0f);
    atomic_store(&g_clear_b, ((argb      ) & 0xFF) / 255.0f);
}
void metal_begin_frame(void){ }

/* Guest: blend state for the following draws. */
void metal_set_blend(unsigned key, float alpha_ref){
    g_cur_blend = key; g_cur_aref = alpha_ref;
}
void metal_set_blendcolor(unsigned argb){ g_cur_bcol = argb; }
void metal_set_fog(unsigned enable, unsigned argb){
    static int off = -1;
    if (off < 0){ const char *e = ENVC("NOFOG"); off = (e && *e && *e!='0'); }
    if (off) enable = 0;
    pthread_mutex_lock(&g_qlock);
    g_cur_fog[0] = (float)((argb>>16)&0xFFu)/255.0f;
    g_cur_fog[1] = (float)((argb>> 8)&0xFFu)/255.0f;
    g_cur_fog[2] = (float)( argb      &0xFFu)/255.0f;
    g_cur_fog[3] = enable ? 1.0f : 0.0f;
    pthread_mutex_unlock(&g_qlock);
}
void metal_set_cull(unsigned m){ g_cur_cull = (m >= 1u && m <= 3u) ? m : 3u; }
/* Guest: depth test/write state.  3D models used to be drawn without a
 * Z-buffer, in submission order -> polygons that should have been behind
 * the body were drawn over it, leaving black blotches. */
void metal_set_depth(unsigned mode){ g_cur_dmode = mode & 63u; g_cur_depth = g_cur_dmode | (g_cur_zid << 8); }
/* Guest: SetDepthStencilSurface.  0 = no Z surface (in D3D the Z test/write
 * has no effect), 1 = implicit surface, >=2 created by CreateDepthStencilSurface. */
void metal_set_zsurf(unsigned zid){
    if (zid >= MAXZ) zid = 0;
    g_cur_zid = zid; g_cur_depth = g_cur_dmode | (g_cur_zid << 8);
}
/* Device Reset: the game calls it when it changes resolution/detail.
 * Changing a setting used to crash because it was not implemented. */
unsigned g_want_w = 0, g_want_h = 0;
/* Metal may not be up yet when the device is created; we store the
 * requested size and use it when the window opens. */
void metal_want_size(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    g_want_w = w; g_want_h = h;
}
/* --- Retina: draw at the real pixel density ---------------------------
 * The game's largest mode is 1280x960.  On a 2x screen that surface was
 * shown scaled up twofold, so every pixel was resampled and blurred
 * (obvious next to the Windows output).
 * Fix: draw the surface with as many pixels as the screen has from the start.
 * THE RATIO IS KEPT EXACTLY (an exact multiple of the reduced ratio is
 * chosen), so the clip-space correction (aspfix) never kicks in -- pure
 * sharpness.  Called from the guest thread, so it does NOT TOUCH NSScreen
 * (only safe on the main thread); it reads the value the main thread
 * measured and stored. */
static unsigned u_gcd(unsigned a, unsigned b){
    while (b){ unsigned t = a % b; a = b; b = t; } return a;
}
/* The window style in ONE place: if the usable-area calculation does not use
 * the same style as the window itself, the title bar allowance is off. */
#define NFSU_WIN_STYLE (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | \
                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
/* The screen's usable CONTENT area (points) and scale.  Measured with
 * AppKit on the main thread and stored.  Guessing was NOT ENOUGH: the
 * title bar was assumed to be 28 points, on a notched screen it was 32;
 * the 3-point difference cropped the window and brought back the
 * resampling (the blur). */
static _Atomic int g_scr_cw = 0, g_scr_ch = 0, g_scr_s100 = 0;
/* Display refresh rate for framerate.c (NFSU2_FPS=auto, the presenter): the
 * lower of two signals.
 *   - The screen: maximumFramesPerSecond of the window's screen
 *     (CGDisplayModeGetRefreshRate reads 0 on an adaptive ProMotion panel).
 *     Re-read at start-up, when the window changes screen or backing scale,
 *     on a display reconfiguration (NSApplicationDidChangeScreenParameters-
 *     Notification: e.g. a refresh rate picked in System Settings) and on a
 *     Low Power Mode change (NSProcessInfoPowerStateDidChangeNotification).
 *   - The display link: its nominal refresh (CADisplayLink.duration).  The
 *     link asks for the screen's maximum rate (vsync_link_range), so a longer
 *     duration means the system drives the panel slower than the screen
 *     reports, e.g. Low Power Mode capping ProMotion.  A new link rate counts
 *     once it has held for 0.5 s.
 * Main thread only writes; metal_display_hz() is read by the guest thread. */
static _Atomic int g_display_hz = 0;
static int g_screen_hz = 0, g_link_hz = 0;      /* main thread; 0 = unknown */
static int g_link_sim_hz = 0;                   /* REFRESHSIM link:<hz> (test) */
int metal_display_hz(void){ return atomic_load(&g_display_hz); }
static void display_hz_apply(const char *why){
    int hz = g_screen_hz > 0 ? g_screen_hz : 60;
    if (g_link_hz > 0 && g_link_hz < hz - 1) hz = g_link_hz;
    int old = atomic_exchange(&g_display_hz, hz);
    if (old && old != hz) fprintf(stderr, "metal: display refresh %d Hz -> %d Hz (%s)\n", old, hz, why);
}
static void vsync_link_range(void);
static void display_hz_update(NSScreen *sc){
    if (!sc) return;
    int hz = 60;
    if (@available(macOS 12.0, *)) hz = (int)[sc maximumFramesPerSecond];
    if (hz <= 0) hz = 60;
    if (hz != g_screen_hz){
        g_screen_hz = hz;
        g_link_hz = 0;                          /* the link re-measures the new mode */
        vsync_link_range();
    }
    display_hz_apply("screen maximumFramesPerSecond");
}
/* Physical panel pixels per point x1000 (NFSU2_RENDER=panel).  In a scaled
 * ("More Space") mode the backing store is larger than the panel: 1800 pt
 * wide at 2x = 3600 backing pixels on a 3024-pixel panel = 1.68 per point. */
static _Atomic int g_scr_ppp1000 = 0;
/* Native pixel width of the display: the mode the system flags as native,
 * else the widest mode it offers. */
static size_t panel_pixel_width(CGDirectDisplayID d){
    size_t best = 0, native = 0;
    CFDictionaryRef opt = (__bridge CFDictionaryRef)
        @{ (__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES };
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(d, opt);
    if (modes){
        for (CFIndex i = 0; i < CFArrayGetCount(modes); i++){
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
            size_t pw = CGDisplayModeGetPixelWidth(m);
            if (pw > best) best = pw;
            if ((CGDisplayModeGetIOFlags(m) & 0x02000000u) && pw > native) native = pw;  /* kDisplayModeNativeFlag */
        }
        CFRelease(modes);
    }
    return native ? native : best;
}
/* Main thread: the screen the window is on (before it exists: the main
 * screen).  Called at start-up and whenever the window changes screen or
 * backing scale. */
static void scr_cache(void){
    @autoreleasepool {
        NSScreen *sc = (g_win && [g_win screen]) ? [g_win screen] : [NSScreen mainScreen];
        if (!sc) return;
        display_hz_update(sc);
        NSRect c = [NSWindow contentRectForFrameRect:[sc visibleFrame]
                                           styleMask:NFSU_WIN_STYLE];
        atomic_store(&g_scr_cw, (int)c.size.width);
        atomic_store(&g_scr_ch, (int)c.size.height);
        atomic_store(&g_scr_s100, (int)lround([sc backingScaleFactor] * 100.0));
        CGDirectDisplayID did = [[[sc deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
        size_t pw = panel_pixel_width(did);
        double ppp = (pw && [sc frame].size.width > 0) ? (double)pw / [sc frame].size.width : 0.0;
        atomic_store(&g_scr_ppp1000, (int)lround(ppp * 1000.0));
        static int shown = 0;
        if (shown++ < 4)
            fprintf(stderr, "metal: screen %.0fx%.0f pt, backing x%.2f, panel %zu px wide (%.3f px/pt)\n",
                    [sc frame].size.width, [sc frame].size.height, [sc backingScaleFactor], pw, ppp);
    }
}
/* For widescreen.c (NFSU2_RENDER): backing pixels per point and physical
 * panel pixels per point of the window's screen (0 = unknown). */
void metal_screen_metrics(double *backing, double *panel){
    if (backing) *backing = atomic_load(&g_scr_s100) / 100.0;
    if (panel)   *panel   = atomic_load(&g_scr_ppp1000) / 1000.0;
}
double ws_max_pixels(void);              /* widescreen.c: NFSMAXPIX */
void metal_native_size(unsigned gw, unsigned gh, unsigned *ow, unsigned *oh){
    if (!ow || !oh) return;
    *ow = gw; *oh = gh;
    if (!gw || !gh) return;
    if (atomic_load(&g_fs)){
        double fw = atomic_load(&g_fs_pw), fh = atomic_load(&g_fs_ph);
        if (fw >= 640.0 && fh >= 480.0){
            double maxpix = ws_max_pixels();
            if (maxpix > 1.0e5 && fw * fh > maxpix){
                double k = sqrt(maxpix / (fw * fh)); fw = floor(fw * k); fh = floor(fh * k); }
            *ow = ((unsigned)fw) & ~1u; *oh = ((unsigned)fh) & ~1u;
            return;
        }
    }
    double s = 1.0, aw, ah;
    int cw = atomic_load(&g_scr_cw), ch = atomic_load(&g_scr_ch);
    int s100 = atomic_load(&g_scr_s100);
    if (cw > 0 && ch > 0 && s100 > 0){
        /* exactly the bound win_apply_size will apply */
        aw = cw; ah = ch; s = s100 / 100.0;
    } else {
        /* No cached value: estimate with CoreGraphics (thread-safe). */
        CGDirectDisplayID d = CGMainDisplayID();
        CGRect b = CGDisplayBounds(d);                /* in POINTS */
        if (b.size.width < 320.0 || b.size.height < 240.0) return;
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
        if (m){ size_t pw = CGDisplayModeGetPixelWidth(m);
                if (pw) s = (double)pw / b.size.width;
                CGDisplayModeRelease(m); }
        /* menu bar / notch (<=32) + title bar (<=32) */
        aw = b.size.width; ah = b.size.height - 64.0;
    }
    if (s < 1.0) s = 1.0;
    if (aw < 320.0 || ah < 240.0) return;
    unsigned g = u_gcd(gw, gh); if (!g) return;
    unsigned ax = gw / g, ay = gh / g;                /* reduced ratio */
    double u = fmin(aw * s / (double)ax, ah * s / (double)ay);
    /* GPU budget bound (NFSMAXPIX, widescreen.h): the same default as the
     * display mode rows, so the native backing store is never capped while
     * a 5K/6K display still is. */
    double maxpix = ws_max_pixels();
    if (maxpix > 1.0e5){
        double up = sqrt(maxpix / ((double)ax * (double)ay));
        if (up < u) u = up;
    }
    unsigned units = (unsigned)u;
    if (!units) return;
    unsigned w = ax * units, h = ay * units;
    if (w <= gw || h <= gh || w > 8192u || h > 8192u) return;   /* no gain */
    *ow = w; *oh = h;
}
/* --- sampler cache --------------------------------------------------- */
#define MAXSMP 64
static struct { unsigned key; id<MTLSamplerState> st; } g_smpc[MAXSMP];
static int g_smpc_n = 0;
unsigned long g_smp_overflow = 0;
static id<MTLSamplerState> samp_for(unsigned k){
    for (int i = 0; i < g_smpc_n; i++) if (g_smpc[i].key == k) return g_smpc[i].st;
    unsigned mn=k&3u, mg=(k>>2)&3u, mp=(k>>4)&3u;
    unsigned au=(k>>6)&7u, av=(k>>9)&7u, al=(k>>12)&7u;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    /* D3DTEXF: 1=POINT 2=LINEAR 3=ANISOTROPIC */
    sd.minFilter = (mn==1u) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    sd.magFilter = (mg==1u) ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    /* MIPLIN: make a D3DTEXF_POINT mip trilinear.  POINT mip picks the
     * level HARD; at a triangle edge the derivative quad sticks out, so the
     * LOD shifts by a hair and jumps to the next mip in a 2-pixel band
     * right along the edge -> a visible line around every quad and along
     * the diagonal two triangles share. */
    static int mlin = -1;
    if (mlin < 0){ const char *e = ENVC("MIPLIN"); mlin = (e && *e && *e!='0'); }
    sd.mipFilter = ENVC("MIPOFF") ? MTLSamplerMipFilterNotMipmapped
                 : (mp==0u) ? MTLSamplerMipFilterNotMipmapped
                 : (mp==1u && !mlin) ? MTLSamplerMipFilterNearest
                            : MTLSamplerMipFilterLinear;
    NSUInteger an = (NSUInteger)1u << al;
    if (an < 1) an = 1; if (an > 16) an = 16;
    /* LODMAX=<n>: clamp the mip choice to level n.  To measure HOW FAR the
     * LOD jumps at the edge line. */
    { static float lmx = -1.f;
      if (lmx < 0.f){ const char *e = ENVC("LODMAX"); lmx = e ? (float)atof(e) : 1e9f; }
      if (lmx < 1e8f) sd.lodMaxClamp = lmx; }
    /* bits 15-17: texrep.m's LOD clamp for replaced textures (texrep_skey_slow) */
    if ((k >> 15) & 7u) sd.lodMaxClamp = fminf(sd.lodMaxClamp, (float)((k >> 15) & 7u));
    { static int noan = -1;
      if (noan < 0){ const char *e = ENVC("NOANISO"); noan = (e && *e && *e!='0'); }
      sd.maxAnisotropy = (!noan && (mn==3u || mg==3u)) ? an : 1;
      /* texture_filtering = anisotropic: the game's own g_TextureFiltering 2
       * turns exactly these samplers (LINEAR with MAXANISOTROPY 16) into
       * ANISOTROPIC 16x; doing it here gives the same frame without changing
       * the player's registry. */
      if (!noan && mn == 2u && an > 1 && enh_aniso()) sd.maxAnisotropy = an; }
    /* D3DTADDRESS: 1=WRAP 2=MIRROR 3=CLAMP 4=BORDER 5=MIRRORONCE */
    MTLSamplerAddressMode am[6] = { MTLSamplerAddressModeRepeat,
        MTLSamplerAddressModeRepeat, MTLSamplerAddressModeMirrorRepeat,
        MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeClampToZero,
        MTLSamplerAddressModeMirrorClampToEdge };
    sd.sAddressMode = am[au < 6u ? au : 1u];
    sd.tAddressMode = am[av < 6u ? av : 1u];
    id<MTLSamplerState> st = [g_dev newSamplerStateWithDescriptor:sd];
    if (!st) return g_samp;
    if (g_smpc_n < MAXSMP){ g_smpc[g_smpc_n].key = k; g_smpc[g_smpc_n].st = st; g_smpc_n++; }
    else { g_smp_overflow++;      /* cache full: a NEW object on every draw */
           static int warned = 0;
           if (!warned){ warned = 1;
               fprintf(stderr,"metal: SAMPLER CACHE FULL (%d) - "
                       "making a new object on every draw\n", MAXSMP); } }
    return st;
}

/* Must be called on the main thread.  Sizes the window so it shows the
 * w x h PIXEL drawing surface 1:1: content = pixels / screen scale.
 * The content used to be set to w x h POINTS directly; on 2x Retina that
 * meant scaling a 1280-pixel surface up to a 2560-pixel area -- the image
 * was blurry.  Now 1 game pixel = 1 screen pixel. */
/* The window size is INDEPENDENT of the resolution: on first open it is
 * sized once to the screen's usable area, after that only the user
 * changes it.  A resolution change only changes the drawing surface
 * (quality); the layer scales it to the window keeping the aspect ratio.
 * (User request 2026-09-27: "fixed window size, the game should shrink
 * as resolution" -- that is how Windows does it too.)  The settings menu
 * rows are derived from the window's aspect ratio, so normally no bars
 * appear (see com.c modes_refresh). */
static _Atomic int g_win_sized = 0;
/* Main thread only: content size (pt) the drawing size was last chosen for,
 * and "fullscreen animation running" (windowDidResize must not reset then). */
static CGFloat g_applied_cw = 0, g_applied_ch = 0;
static int g_fs_transition = 0;
static void win_apply_size(unsigned w, unsigned h){
    if (!g_layer) return;
    CGFloat s = g_win ? [g_win backingScaleFactor]
                      : [[NSScreen mainScreen] backingScaleFactor];
    if (s < 1.0) s = 1.0;
    g_layer.contentsScale = s;          /* left alone it stays 1.0 */
    g_layer.drawableSize  = CGSizeMake((CGFloat)w, (CGFloat)h);
    atomic_store(&g_draw_w, (int)w); atomic_store(&g_draw_h, (int)h);
    if (!g_win) return;
    int fs = atomic_load(&g_fs) || ([g_win styleMask] & NSWindowStyleMaskFullScreen);
    if (!fs && !g_win_sized){
        NSScreen *sc = [g_win screen] ?: [NSScreen mainScreen];
        NSRect vis = [sc visibleFrame];
        NSRect cr  = [g_win contentRectForFrameRect:
                        NSMakeRect(0, 0, vis.size.width, vis.size.height)];
        [g_win setContentSize:NSMakeSize(floor(cr.size.width), floor(cr.size.height))];
        g_win_sized = 1;
    }
    NSSize c = [[g_win contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    if (!fs){ g_applied_cw = c.width; g_applied_ch = c.height; }
    int exact = fabs(c.width * s - (CGFloat)w) < 1.0 && fabs(c.height * s - (CGFloat)h) < 1.0;
    fprintf(stderr, "metal: %s: drawable %ux%u pixels, content %.0fx%.0f points (scale %.2g) -> %s\n",
            fs ? "fullscreen" : "window", w, h, (double)c.width, (double)c.height, (double)s,
            exact ? "1:1" : "scaled to the window");
}
/* The VIEW (pixels) the settings menu rows are based on: in fullscreen the
 * fullscreen content, in a window the window's current content; if the
 * window has not been sized yet, the size it will get on first open.
 * Called from the guest thread, does not touch AppKit. */
void metal_view_size(unsigned *ow, unsigned *oh){
    double sc = atomic_load(&g_scr_s100) / 100.0; if (sc < 1.0) sc = 1.0;
    double w = 0, h = 0;
    if (atomic_load(&g_fs) && atomic_load(&g_fs_pw) > 0){
        w = atomic_load(&g_fs_pw); h = atomic_load(&g_fs_ph);
    } else if (g_win_sized){
        w = atomic_load(&g_cont_w) * sc; h = atomic_load(&g_cont_h) * sc;
    } else {
        w = atomic_load(&g_scr_cw) * sc; h = atomic_load(&g_scr_ch) * sc;
    }
    if (w < 320.0 || h < 240.0){ *ow = *oh = 0; return; }
    *ow = ((unsigned)lround(w)) & ~1u; *oh = ((unsigned)lround(h)) & ~1u;
}
void metal_resize(unsigned w, unsigned h){
    if (!w || !h || w > 8192u || h > 8192u) return;
    dispatch_async(dispatch_get_main_queue(), ^{ win_apply_size(w, h); });
}
void metal_set_stage_tex(unsigned t1, unsigned t2, unsigned t3){
    g_cur_tex1 = t1; g_cur_tex2 = t2; g_cur_tex3 = t3;
}
/* stages 4-7 (com.c, a game with 8 stages): the Mac renderer has 4 */
void metal_set_stage_hi(const unsigned t[4], const unsigned k[4]){ (void)t; (void)k; }
void metal_set_depth_bias(unsigned b){ (void)b; }
unsigned metal_create_texture_depth(unsigned w, unsigned h, unsigned f){ (void)w; (void)h; (void)f; return 0; }
int metal_tex_is_depth(unsigned id){ (void)id; return 0; }
/* The guest's active pixel shader and its constants. */
void metal_set_ps(int psid, const float *c32){
    g_cur_ps = psid;
    if (c32 && memcmp(g_cur_psc, c32, sizeof g_cur_psc)){
        memcpy(g_cur_psc, c32, sizeof g_cur_psc); g_psc_ver++; }
}
void metal_set_texproj(const float p4[4]){ memcpy(g_cur_proj, p4, sizeof g_cur_proj); }
/* --- GPU vertex shaders --------------------------------------------------
 * Measured: ~45% of the run time was in the vs_run interpreter on the CPU.
 * The game's own bytecode is translated to MSL and compiled here. */
#define MAXVSF 512
static id<MTLFunction> g_vsfn[MAXVSF];
void metal_register_vs(int vsid, const char *msl){
    if (vsid <= 0 || vsid >= MAXVSF || !msl || !g_dev || g_vsfn[vsid]) return;
    @autoreleasepool {
        NSString *src = [kPrelude stringByAppendingString:
                         [NSString stringWithUTF8String:msl]];
        NSError *err = nil;
        struct timespec _c0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_c0);
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        if (g_perflog){ struct timespec _c1; clock_gettime(CLOCK_MONOTONIC, &_c1);
            perflog_event(PL_VS, (uint64_t)(_c1.tv_sec - _c0.tv_sec) * 1000000000ull + (uint64_t)_c1.tv_nsec - (uint64_t)_c0.tv_nsec, 0); }
        if (!lib){
            static int shown = 0;
            if (shown < 5){ shown++;
                fprintf(stderr, "vertex shader %d did not compile: %s\n", vsid,
                        err ? [[err localizedDescription] UTF8String] : "?"); }
            return;
        }
        char nm[32]; snprintf(nm, sizeof nm, "vs_%d", vsid);
        g_vsfn[vsid] = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
        if (!g_vsfn[vsid])
            fprintf(stderr, "vertex shader %d: function not found\n", vsid);
#if NFS_ENHANCED
        if (vsid < HD_MAXVSI && hd_rt_on()){
            snprintf(nm, sizeof nm, "vsk_%d", vsid);
            id<MTLFunction> kf = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
            NSError *ke = nil;
            if (kf) g_hd_vsk[vsid] = [g_dev newComputePipelineStateWithFunction:kf error:&ke];
            if (!g_hd_vsk[vsid]) fprintf(stderr, "[HD] capture kernel vs_%d could not be built: %s\n", vsid,
                                         ke ? [[ke localizedDescription] UTF8String] : "no function");
        }
#endif
    }
}
int metal_vs_ready(int vsid){
    return (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid] != nil) ? 1 : 0;
}

/* Queue the raw (untransformed) vertices + constants; the GPU does the
 * vertex processing.  kdata = c0..c[nk-1], nk = vs_kmax. */
/* Under g_qlock: place the constant block (reuse it if repeated) and fill
 * the draw record.  The caller sets up the vertex source (rawoff / index
 * fields, count) and increments g_dn.  NULL = constant arena full. */
static MDraw *raw_rec(int b, unsigned stride, unsigned tex, int vsid,
                      const float *kdata, unsigned nk){
    unsigned ksz = 16u + nk * 16u;
    /* Do not upload the constant block again if it repeats: the game draws
     * the parts of one object back to back, the block is mostly the same. */
    static unsigned last_koff[2], last_ksz[2]; static int last_vs[2];
    unsigned koff;
    uint8_t hdr[16]; memset(hdr, 0, sizeof hdr); memcpy(hdr, &stride, 4);
    /* Per-stage TEXCOORDINDEX (2 bits each) -> the vertex shader makes the
     * selection itself; on the pixel side the coordinate stays a plain
     * interpolant. */
    { uint32_t ts = 0;
      for (int q = 0; q < 4; q++)
          ts |= ((uint32_t)((int)g_cur_proj[q]) & 3u) << (8*q);
      memcpy(hdr + 4, &ts, 4); }
    if (last_ksz[b] == ksz && last_vs[b] == vsid && g_kn[b] &&
        !memcmp(g_kbuf[b] + last_koff[b], hdr, 16) &&
        !memcmp(g_kbuf[b] + last_koff[b] + 16, kdata, nk * 16u)){
        koff = last_koff[b];
    } else {
        koff = (g_kn[b] + 255u) & ~255u;
        if (koff + ksz > MAXKB){ g_ov_kb++; return NULL; }
        memcpy(g_kbuf[b] + koff, hdr, 16);
        memcpy(g_kbuf[b] + koff + 16, kdata, nk * 16u);
        g_kn[b] = koff + ksz;
        last_koff[b] = koff; last_ksz[b] = ksz; last_vs[b] = vsid;
    }
    MDraw *D = &g_dbuf[b][g_dn[b]];
    memset(D, 0, sizeof *D);          /* reused slot: see metal_draw_tv */
    D->tex = tex; D->target = g_cur_target;
    memcpy(D->smp, g_cur_smp, sizeof D->smp);
    D->blend = g_cur_blend; D->depth = g_cur_depth; D->alpha_ref = g_cur_aref;
    D->bcol  = g_cur_bcol; D->cull = g_cur_cull;
    memcpy(D->fogv, g_cur_fog, sizeof D->fogv);
    D->tex1 = g_cur_tex1; D->tex2 = g_cur_tex2; D->tex3 = g_cur_tex3;
    D->ps = (g_cur_ps > 0 && g_cur_ps < PSCUBE && metal_tex_is_cube(tex))
              ? g_cur_ps + PSCUBE : g_cur_ps;
#if NFS_ENHANCED
    { unsigned tx4[4] = { tex, g_cur_tex1, g_cur_tex2, g_cur_tex3 };
      D->ps = hd_refl_ps(D->ps, g_cur_target, tx4); }
#endif
    D->psci = psc_put(b); memcpy(D->c0, g_cur_psc[0], sizeof D->c0);
    memcpy(D->proj, g_cur_proj, sizeof D->proj);
    D->vsid = vsid; D->koff = koff; D->ksz = ksz;
    return D;
}
int metal_draw_raw(const void *src, unsigned nverts, unsigned stride,
                   unsigned prim, unsigned tex, int vsid,
                   const float *kdata, unsigned nk){
    if (!g_rvbuf[0] || nverts < 3 || !stride || stride > 256u) return 0;
    unsigned tris = (prim == 4) ? nverts/3 : nverts - 2;
    if (!tris) return 0;
    const uint8_t *S = (const uint8_t *)src;
    unsigned need = tris * 3u * stride;

    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    unsigned roff = (g_rvn[b] + 255u) & ~255u;
    if (roff + need > MAXRV){ g_ov_rv++; pthread_mutex_unlock(&g_qlock); return 0; }
    if (g_dn[b] >= MAXD){ pthread_mutex_unlock(&g_qlock); return 0; }
    MDraw *D = raw_rec(b, stride, tex, vsid, kdata, nk);
    if (!D){ pthread_mutex_unlock(&g_qlock); return 0; }

    uint8_t *dst = g_rvbuf[b] + roff;
    /* TRIANGLE LIST: the source is already contiguous, one block copy is
     * enough.  This used to call memcpy PER VERTEX (measured: ~1250 draws
     * x ~90 vertices per frame = 112 thousand small memcpys/frame; in the
     * profile _platform_memmove was 4.6% of the guest thread).  Strips and
     * fans share vertices, so they still need gathering. */
    if (prim == 4){
        memcpy(dst, S, need);
    } else
    for (unsigned t = 0; t < tris; t++){
        unsigned idx[3];
        if (prim == 6){ idx[0]=0; idx[1]=t+1; idx[2]=t+2; }               /* FAN   */
        else if (prim == 5){                                              /* STRIP */
            if (t & 1){ idx[0]=t+1; idx[1]=t; idx[2]=t+2; }
            else      { idx[0]=t;   idx[1]=t+1; idx[2]=t+2; }
        }
        else { idx[0]=t*3; idx[1]=t*3+1; idx[2]=t*3+2; }                  /* LIST  */
        for (int k = 0; k < 3; k++){
            memcpy(dst, S + (size_t)idx[k]*stride, stride);
            dst += stride;
        }
    }
    g_rvn[b] = roff + need;
    D->first = 0; D->count = tris*3; D->rawoff = roff;
    g_dn[b]++;
    pthread_mutex_unlock(&g_qlock);
    return 1;
}
/* Indexed triangle list from the copies in the pool (see PERSISTENT VERTEX /
 * INDEX POOL).  vbo: pool offset of vertex 0, ibo: offset of the first index. */
int metal_draw_ix(unsigned vbo, unsigned stride, unsigned ibo, unsigned isz, unsigned nidx,
                  int base, unsigned tex, int vsid, const float *kdata, unsigned nk,
                  unsigned long long gh){
    if (!g_pool || nidx < 3 || !stride || stride > 256u || (isz != 2u && isz != 4u)) return 0;
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_dn[b] >= MAXD){ pthread_mutex_unlock(&g_qlock); return 0; }
    MDraw *D = raw_rec(b, stride, tex, vsid, kdata, nk);
    if (!D){ pthread_mutex_unlock(&g_qlock); return 0; }
    D->count = nidx - nidx % 3u; D->rawoff = vbo;
    D->ix_isz = isz; D->ix_ib = ibo; D->ix_base = base; D->gh = gh ? gh : 1ull;
    g_dn[b]++;
    pthread_mutex_unlock(&g_qlock);
    return 1;
}
void metal_raw_overflow(unsigned *rv, unsigned *kb){
    if (rv) *rv = g_ov_rv;
    if (kb) *kb = g_ov_kb;
}

/* Compile and keep the MSL source from the translator (once per id). */
/* CAREFUL: the parameter cannot be named "id" - it would shadow ObjC's id
 * type and "id<MTLLibrary>" could no longer be parsed. */
void metal_register_ps(int psid, const char *msl){
    { const char *pd = ENVC("PSDUMP");          /* comma lists supported */
      if (pd){ char want[16]; int n = snprintf(want, sizeof want, "%d", psid);
        for (const char *q = pd; q && *q; ){
            while (*q==',' || *q==' ') q++;
            if (!*q) break;
            if (!strncmp(q, want, (size_t)n) && (q[n]==0 || q[n]==',' || q[n]==' ')){
                fprintf(stderr, "=== ps %d MSL ===\n%s\n=== end ===\n", psid, msl);
                break; }
            while (*q && *q != ',') q++; } } }
    /* PSISO=<psid>:<expr> : send one of the shader's INTERMEDIATE TERMS
     * straight to the output.  Instead of guessing which input blows up we
     * see it on screen (t[0] diffuse, t[1] headlight texture, t[2] ramp,
     * v[0] vertex color, P.c[0] constant). */
    char *iso = 0;
    { const char *pe = ENVC("PSISO");
      if (pe && *pe){ const char *cl = strchr(pe, ':');
        /* PSISO=*:  -> every shader outputs a CONSTANT color derived from
         * its own id, so we can read off the screen which ps drew which
         * surface. */
        char autoex[96];
        if (cl && pe[0] == '*' && cl == pe + 1){
            /* Alpha must stay ORIGINAL: fixing it breaks blending and
             * the frame falls apart completely. */
            snprintf(autoex, sizeof autoex,
                     "%.4f, %.4f, %.4f, res.a",
                     (double)((psid * 97) % 251) / 250.0,
                     (double)((psid * 53) % 251) / 250.0,
                     (double)((psid * 29) % 251) / 250.0);
            cl = pe + 1;
        } else autoex[0] = 0;
        if (cl && (autoex[0] || atoi(pe) == psid)){
            const char *ex = autoex[0] ? autoex : cl + 1;
            size_t need = strlen(msl) + strlen(ex) + 128;
            iso = (char *)malloc(need);
            if (iso){
                const char *rp = strstr(msl, "return res;");
                const char *dc = strstr(msl, "if (res.a < P.aref) discard_fragment();");
                if (rp){
                    size_t head = (size_t)(rp - msl);
                    memcpy(iso, msl, head); iso[head] = 0;
                    (void)dc;   /* the alpha test is kept: the silhouette stays right */
                    snprintf(iso + head, need - head, "return float4(%s);\n}\n", ex);
                    msl = iso;
                    fprintf(stderr, "  [psiso] ps %d -> %s\n", psid, ex);
                } else { free(iso); iso = 0; }
            }
        } } }
    if (psid <= 0 || psid >= MAXPS || !msl || !g_dev || g_psfn[psid]){ free(iso); return; }
    @autoreleasepool {
        NSString *src = [kPrelude stringByAppendingString:
                         [NSString stringWithUTF8String:msl]];
        NSError *err = nil;
        struct timespec _c0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_c0);
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
        if (g_perflog){ struct timespec _c1; clock_gettime(CLOCK_MONOTONIC, &_c1);
            perflog_event(PL_PS, (uint64_t)(_c1.tv_sec - _c0.tv_sec) * 1000000000ull + (uint64_t)_c1.tv_nsec - (uint64_t)_c0.tv_nsec, 0); }
        if (!lib){
            static int shown = 0;
            if (shown < 3){ shown++;
                fprintf(stderr, "ps%d did not compile: %s\n", psid,
                        err ? [[err localizedDescription] UTF8String] : "?"); }
            return;
        }
        char nm[32]; snprintf(nm, sizeof nm, "ps_%d", psid);
        g_psfn[psid] = [lib newFunctionWithName:[NSString stringWithUTF8String:nm]];
        if (!g_psfn[psid]) fprintf(stderr, "ps%d: function not found\n", psid);
        g_psfn_nd[psid] = g_psfn[psid];
        { const char *at = "if (res.a < P.aref) discard_fragment();";
          const char *hit = strstr(msl, at);
          if (hit && g_psfn[psid] && hsrfix_on()){
              size_t L = strlen(msl), la = strlen(at);
              char *m2 = (char *)malloc(L + 1);
              if (m2){
                  memcpy(m2, msl, (size_t)(hit - msl));
                  memcpy(m2 + (hit - msl), hit + la, L - (size_t)(hit - msl) - la + 1);
                  NSString *src2 = [kPrelude stringByAppendingString:[NSString stringWithUTF8String:m2]];
                  NSError *e2 = nil;
                  id<MTLLibrary> lib2 = [g_dev newLibraryWithSource:src2 options:nil error:&e2];
                  id<MTLFunction> f2 = lib2 ? [lib2 newFunctionWithName:[NSString stringWithUTF8String:nm]] : nil;
                  if (f2) g_psfn_nd[psid] = f2;
                  free(m2);
              } } }
    }
    free(iso);
}

/* D3DCMPFUNC (1..8) -> Metal compare function */
static MTLCompareFunction cmp_of(unsigned f){
    switch (f){
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
static id<MTLDepthStencilState> dss_for(unsigned mode){
    unsigned k = mode & 63u;
    if (g_dss[k]) return g_dss[k];
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    if (k & 1u) dd.depthCompareFunction = cmp_of((k >> 2) & 7u);
    else        dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = (k & 2u) ? YES : NO;
    g_dss[k] = [g_dev newDepthStencilStateWithDescriptor:dd];
    return g_dss[k];
}
/* Guest: the following draws go to this target (0 = screen). */
void metal_set_render_target(unsigned texid){
    pthread_mutex_lock(&g_qlock);
    g_cur_target = texid;
    pthread_mutex_unlock(&g_qlock);
}
/* Guest: clear the active target (applied at the right place in draw order). */
/* StretchRect: the game copies the scene FROM THE BACK BUFFER into a
 * texture and post-processing (bleach bypass, motion blur, DOF) reads that
 * texture.  It was NEVER implemented (measured: 2261 calls in a session)
 * -> the source texture stayed empty and the composite came out black.
 * We put the copy into the draw list as an ORDERED entry; at present time
 * it runs as a full-screen quad (scaling and format conversion for free). */
void metal_blit(unsigned src_tex, unsigned dst_tex){
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_dn[b] < MAXD){
        MDraw *D = &g_dbuf[b][g_dn[b]];
        memset(D, 0, sizeof *D);
        D->target = dst_tex; D->blit_src = src_tex; D->is_blit = 1;
        g_dn[b]++;
    }
    pthread_mutex_unlock(&g_qlock);
}

/* The web renderer copies only a source rectangle (gpu_web.c) and reads a
 * lockable render target back for the game (the DVD and magazine covers);
 * here the whole source is copied and the lock buffer keeps what it has. */
void metal_blit_rect(unsigned src_tex, unsigned dst_tex, const int *r){ (void)r; metal_blit(src_tex, dst_tex); }
int metal_read_texture(unsigned id, void *dst, unsigned pitch, unsigned rows){ (void)id; (void)dst; (void)pitch; (void)rows; return 0; }

void metal_clear_target_ex(unsigned argb, unsigned flags, float z);
void metal_clear_target(unsigned argb){ metal_clear_target_ex(argb, 1u, 1.f); }
void metal_clear_target_ex(unsigned argb, unsigned flags, float z){
    pthread_mutex_lock(&g_qlock);
    int b = g_cur;
    if (g_dn[b] < MAXD){
        memset(&g_dbuf[b][g_dn[b]], 0, sizeof g_dbuf[b][g_dn[b]]);
        g_dbuf[b][g_dn[b]].first = 0; g_dbuf[b][g_dn[b]].count = 0;
        g_dbuf[b][g_dn[b]].tex = 0;   g_dbuf[b][g_dn[b]].target = g_cur_target;
        { static unsigned ov = 0xFFFFFFFFu;      /* CLEARCOL=0xAARRGGBB: diagnostic */
          if (ov == 0xFFFFFFFFu){ const char *e=ENVC("CLEARCOL");
                                  ov = e ? (unsigned)strtoul(e,0,0) : 0u; }
          if (ov) argb = ov; }
        g_dbuf[b][g_dn[b]].clear_argb = argb;
        g_dbuf[b][g_dn[b]].is_clear = 1;
        g_dbuf[b][g_dn[b]].clr_flags = flags;
        g_dbuf[b][g_dn[b]].clr_z = z;
        g_dbuf[b][g_dn[b]].depth = g_cur_depth;
        g_dn[b]++;
    }
    pthread_mutex_unlock(&g_qlock);
}

void metal_present(void){
    if (!g_layer || !g_vbuf[0]){ tex_free_due(1); return; }   /* headless: nothing draws */
    /* Mailbox (NFSU2_FPS=uncapped, or a content rate above the display's,
     * framerate.c): a newer frame replaces one the main thread has not
     * started to encode; only a frame being encoded is waited for (its
     * buffer is the one the guest would write next). */
    int hfr_mailbox(void);
    int mailbox = hfr_mailbox();
    /* The wait below can take a refresh or more when the GPU is behind:
     * the guest's other threads (audio streaming, loaders) must not starve
     * meanwhile, so the guest lock is released around it. */
    int unlocked = 0;
    pthread_mutex_lock(&g_qlock);
    if (g_ready >= 0 && (!mailbox || g_enc_rb >= 0)){
        void guest_wait_begin(void);
        pthread_mutex_unlock(&g_qlock);
        guest_wait_begin(); unlocked = 1;
        pthread_mutex_lock(&g_qlock);
    }
    if (mailbox && g_ready >= 0 && g_enc_rb < 0){
        atomic_fetch_add(&g_frames_replaced, 1);       /* undisplayed frame dropped */
    } else
    /* If the previous frame is not on screen yet, WAIT.  Without the wait
     * the guest overwrites the next frame and the screen goes blank at
     * intervals (the "goes black and comes back" bug the user saw).     */
    if (g_ready >= 0){
        struct timespec ts, _w0; clock_gettime(CLOCK_MONOTONIC,&_w0);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 60L * 1000000L;             /* at most 60 ms */
        if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int late = 0;
        while (g_ready >= 0){
            /* Time is up but the main thread is still ENCODING the previous
             * frame: do not give up, wait.  The swap would point g_cur at the
             * buffer it is reading (see g_enc_rb).  The signal comes as soon
             * as encoding ends. */
            if (late && g_enc_rb >= 0){ pthread_cond_wait(&g_qcond, &g_qlock); continue; }
            if (mailbox && g_enc_rb < 0) break;     /* encoding done: replace the pending frame */
            if (pthread_cond_timedwait(&g_qcond, &g_qlock, &ts) != 0){
                if (g_enc_rb >= 0){
                    if (!late){ late = 1;
                        unsigned long n = atomic_fetch_add(&g_enc_late, 1ul);
                        if (n < 8) g_enc_late_f[n] = atomic_load(&g_frames_drawn) + 1; }
                    continue;
                }
                atomic_fetch_add(&g_frames_dropped, 1);
                break;                             /* do not get stuck */
            }
        }
        { struct timespec _w1; clock_gettime(CLOCK_MONOTONIC,&_w1);
          atomic_fetch_add(&g_wait_present_ns,
            (unsigned long long)(_w1.tv_sec-_w0.tv_sec)*1000000000ull
            + (unsigned long long)_w1.tv_nsec - (unsigned long long)_w0.tv_nsec); }
    }
    g_pool_fr[g_cur] = g_pool;       /* this frame's indexed draws are in this pool */
    g_gframe++;
    g_ready = g_cur;                 /* the main thread draws this buffer */
    g_cur ^= 1;                      /* the guest keeps writing to the other one */
    g_vn[g_cur] = 0; g_dn[g_cur] = 0;
    g_rvn[g_cur] = 0; g_kn[g_cur] = 0; g_pscn[g_cur] = 0;
#if NFS_ENHANCED
    g_hd_cam_ok[g_cur] = 0;
#endif
    atomic_store(&g_frame_pending, 1);
    pthread_cond_signal(&g_rcond);      /* the main thread picks it up at once */
    pthread_mutex_unlock(&g_qlock);
    if (unlocked){ void guest_wait_end(void); guest_wait_end(); }
    tex_free_due(0);                 /* with the guest lock held again (g_tfree) */
}

long metal_frames_dropped(void){ return atomic_load(&g_frames_dropped); }
unsigned long long metal_wait_present_ns(void){ return atomic_load(&g_wait_present_ns); }
int metal_thermal_state(void){ return (int)[[NSProcessInfo processInfo] thermalState]; }

/* PERFLOG screenshot: a full-size copy of the drawable (GPU blit, shared
 * texture); once it completes, a background queue reads every k-th pixel
 * and writes a JPEG ~480 pixels wide.  No work on the game threads; a new
 * image is not taken before the previous one is written. */
static id<MTLTexture> g_pl_tex;
static _Atomic int g_pl_busy = 0;
static void pl_thumb(id<MTLCommandBuffer> cb, id<MTLTexture> src, long fno){
    if (atomic_load(&g_pl_busy)) return;
    NSUInteger W = src.width, H = src.height;
    if (g_pl_tex && (g_pl_tex.width != W || g_pl_tex.height != H)) g_pl_tex = nil;
    if (!g_pl_tex){
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                       width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        g_pl_tex = [g_dev newTextureWithDescriptor:td];
        if (!g_pl_tex) return;
    }
    atomic_store(&g_pl_busy, 1);
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
             sourceSize:MTLSizeMake(W, H, 1) toTexture:g_pl_tex destinationSlice:0
       destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
    [bl endEncoding];
    id<MTLTexture> tex = g_pl_tex;
    double tms = perflog_now_us() / 1e3;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b){
        (void)b;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            NSUInteger k = (W + 479) / 480, w = W / k, h = H / k;
            uint8_t *row = malloc(W * 4), *out = malloc(w * h * 4);
            if (row && out){
                for (NSUInteger y = 0; y < h; y++){
                    [tex getBytes:row bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, y * k, W, 1) mipmapLevel:0];
                    for (NSUInteger x = 0; x < w; x++) memcpy(out + (y * w + x) * 4, row + x * k * 4, 4);
                }
                CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
                CGContextRef cg = CGBitmapContextCreate(out, w, h, 8, w * 4, cs,
                    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
                if (cg){
                    CGImageRef img = CGBitmapContextCreateImage(cg);
                    char path[1100];
                    snprintf(path, sizeof path, "%s/thumbs/%09.0f_%ld.jpg", g_pl_dir, tms, fno);
                    CFStringRef sp = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
                    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, sp, kCFURLPOSIXPathStyle, false);
                    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, NULL);
                    if (dst){ CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
                    CFRelease(url); CFRelease(sp); CGImageRelease(img); CGContextRelease(cg);
                }
                CGColorSpaceRelease(cs);
            }
            free(row); free(out);
            atomic_store(&g_pl_busy, 0);
        });
    }];
}

/* Called from the main thread BEFORE the guest starts.
 * The game creates its first textures before the window opens; if the
 * device was not ready at that point, the textures silently got lost. */
void metal_early_init(void){
    if (g_dev) return;
    /* Measure the screen geometry on the main thread BEFORE the guest thread
     * starts: the guest picks the drawing size in CreateDevice from it
     * (boot.c calls this function before pthread_create). */
    [NSApplication sharedApplication];
    scr_cache();
    /* App Nap / timer coalescing OFF.  While the game window is not in
     * front, macOS drops the process to background QoS and the work moves
     * to the EFFICIENCY cores -- measured: same binary, same scene, 36 ms
     * per frame instead of 10 ms (3.4x).  clang from the same shell ran at
     * full speed, so it was the APP being throttled, not the machine. */
    { static id act; if (!act)
        act = [[NSProcessInfo processInfo] beginActivityWithOptions:
                 NSActivityUserInitiated | NSActivityLatencyCritical
                 reason:@"rendering game frames"];
      (void)act; }
    dump_init();
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev){ fprintf(stderr, "metal: no device found\n"); return; }
    g_queue = [g_dev newCommandQueue];
    @autoreleasepool {
        g_vbuf[0] = malloc(sizeof(MVert)*MAXV); g_vbuf[1] = malloc(sizeof(MVert)*MAXV);
        g_dbuf[0] = malloc(sizeof(MDraw)*MAXD); g_dbuf[1] = malloc(sizeof(MDraw)*MAXD);
        g_pscb[0] = malloc(sizeof *g_pscb[0] * MAXD); g_pscb[1] = malloc(sizeof *g_pscb[1] * MAXD);
        for (int i = 0; i < NFB; i++){           /* see "per-frame buffer ring" */
            g_fb_vb[i]  = fb_fit(nil, 0, sizeof(MVert)*MAXV);
            g_fb_rvb[i] = fb_fit(nil, 0, MAXRV);
            g_fb_kb[i]  = fb_fit(nil, 0, MAXKB);
        }
        g_fb_sem = dispatch_semaphore_create(NFB);
        g_mtlvb = g_fb_vb[0]; g_mtlrvb = g_fb_rvb[0]; g_mtlkb = g_fb_kb[0];
        /* Fixed full-screen quad for StretchRect copies. */
        g_blitvb = [g_dev newBufferWithLength:sizeof(MVert)*6
                                      options:MTLResourceStorageModeShared];
        if (g_blitvb){
            MVert q[6]; memset(q, 0, sizeof q);
            const float px[6] = {-1, 1, 1,-1, 1,-1}, py[6] = { 1, 1,-1, 1,-1,-1};
            const float tu[6] = { 0, 1, 1, 0, 1, 0}, tv3[6] = { 0, 0, 1, 0, 1, 1};
            for (int i2 = 0; i2 < 6; i2++){
                q[i2].x = px[i2]; q[i2].y = py[i2]; q[i2].z = 0.f; q[i2].w = 1.f;
                q[i2].r = q[i2].g = q[i2].b = q[i2].a = 1.f;
                for (int k2 = 0; k2 < 4; k2++){
                    q[i2].t[k2][0] = tu[i2]; q[i2].t[k2][1] = tv3[i2];
                    q[i2].t[k2][2] = 0.f;    q[i2].t[k2][3] = 1.f;
                }
            }
            memcpy([g_blitvb contents], q, sizeof q);
        }
        fpscounter_init(g_dev);
        g_rvbuf[0] = malloc(MAXRV); g_rvbuf[1] = malloc(MAXRV);
        g_kbuf[0]  = malloc(MAXKB); g_kbuf[1]  = malloc(MAXKB);
        NSError *err = nil;
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:kShaderSrc options:nil error:&err];
        g_lib = lib;
        if (!lib){
            fprintf(stderr,"metal: shader did not compile: %s\n",
                    [[err localizedDescription] UTF8String]);
        } else {
            MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[0].format = MTLVertexFormatFloat4;
            vd.attributes[0].offset = 0;  vd.attributes[0].bufferIndex = 0;
            vd.attributes[1].format = MTLVertexFormatFloat4;
            vd.attributes[1].offset = 16; vd.attributes[1].bufferIndex = 0;
            vd.attributes[2].format = MTLVertexFormatFloat4;   /* oD1 */
            vd.attributes[2].offset = 32; vd.attributes[2].bufferIndex = 0;
            vd.attributes[3].format = MTLVertexFormatFloat4;   /* oT0 */
            vd.attributes[3].offset = 48; vd.attributes[3].bufferIndex = 0;
            vd.attributes[4].format = MTLVertexFormatFloat4;   /* oT1 */
            vd.attributes[4].offset = 64; vd.attributes[4].bufferIndex = 0;
            vd.attributes[5].format = MTLVertexFormatFloat4;   /* oT2 */
            vd.attributes[5].offset = 80; vd.attributes[5].bufferIndex = 0;
            vd.attributes[6].format = MTLVertexFormatFloat4;   /* oT3 */
            vd.attributes[6].offset = 96; vd.attributes[6].bufferIndex = 0;
            vd.layouts[0].stride = sizeof(MVert);
            vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
            g_vdesc = vd;

            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction   = [lib newFunctionWithName:@"vmain"];
            pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
            pd.vertexDescriptor = vd;
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            pd.colorAttachments[0].blendingEnabled = YES;
            pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            NSError *e3 = nil;
            g_pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e3];
            if (!g_pso) fprintf(stderr,"metal: pipeline could not be built: %s\n",
                                [[e3 localizedDescription] UTF8String]);
            g_pso_ms_desc = [pd copy];              /* msaa: built when first needed */
            if (hd_msaa_n() > 1) (void)hd_pso_ms(); /* build now for the start-up sample count */
#if NFS_ENHANCED
            if (hd_ssr_on()){
                NSError *e4 = nil;
                id<MTLLibrary> sl = [g_dev newLibraryWithSource:hd_gb_src(kSSRSrc) options:nil error:&e4];
                if (!sl) fprintf(stderr, "[HD] SSR shader did not compile: %s\n",
                                 [[e4 localizedDescription] UTF8String]);
                else {
                    MTLRenderPipelineDescriptor *sd2 = [MTLRenderPipelineDescriptor new];
                    sd2.vertexFunction = [sl newFunctionWithName:@"hd_fs_vert"];
                    sd2.fragmentFunction = [sl newFunctionWithName:@"hd_ssr_frag"];
                    sd2.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                    g_hd_ssr_pso = [g_dev newRenderPipelineStateWithDescriptor:sd2 error:&e4];
                    if (hd_msaa_n() > 1){
                        sd2.rasterSampleCount = (NSUInteger)hd_msaa_n();
                        g_hd_ssr_pso_ms = [g_dev newRenderPipelineStateWithDescriptor:sd2 error:&e4];
                    }
                    if (!g_hd_ssr_pso) fprintf(stderr, "[HD] SSR pipeline could not be built: %s\n",
                                               [[e4 localizedDescription] UTF8String]);
                }
            }
#endif
        }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        /* MIPOFF=1: turn off mip selection (diagnostic).  To tell whether a
         * coarse mip gets picked and leaves a 1-pixel line when the
         * derivative estimate jumps at triangle edges. */
        sd.mipFilter = ENVC("MIPOFF") ? MTLSamplerMipFilterNotMipmapped
                                        : MTLSamplerMipFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeRepeat;
        sd.tAddressMode = MTLSamplerAddressModeRepeat;
        g_samp = [g_dev newSamplerStateWithDescriptor:sd];

        MTLTextureDescriptor *wd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                  width:1 height:1 mipmapped:NO];
        wd.usage = MTLTextureUsageShaderRead; wd.storageMode = MTLStorageModeShared;
        g_white = [g_dev newTextureWithDescriptor:wd];
        unsigned px = 0xFFFFFFFFu;
        [g_white replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0
                     withBytes:&px bytesPerRow:4];
    }
    fprintf(stderr, "metal: device ready (%s), draw path %s\n",
            [[g_dev name] UTF8String], g_pso ? "READY" : "MISSING");
}

/* --- pipeline cache keyed by blend state --------------------------------
 * D3D9's fixed-function blend settings are baked into the pipeline in
 * Metal, so every distinct combination needs its own pipeline.          */
#define MAXPSO 2048         /* + MSAA (and in the research build G-buffer) bits */
static struct { uint64_t key; id<MTLRenderPipelineState> pso; } g_psocache[MAXPSO];
static unsigned g_pson = 0;

static MTLBlendFactor d3d_blend(unsigned f, int is_alpha){
    (void)is_alpha;
    switch (f){
    case 1:  return MTLBlendFactorZero;
    case 2:  return MTLBlendFactorOne;
    case 3:  return MTLBlendFactorSourceColor;
    case 4:  return MTLBlendFactorOneMinusSourceColor;
    case 5:  return MTLBlendFactorSourceAlpha;
    case 6:  return MTLBlendFactorOneMinusSourceAlpha;
    case 7:  return MTLBlendFactorDestinationAlpha;
    case 8:  return MTLBlendFactorOneMinusDestinationAlpha;
    case 9:  return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    /* 12/13 = BOTHSRCALPHA / BOTHINVSRCALPHA (legacy, SRCBLEND only) */
    case 12: return MTLBlendFactorSourceAlpha;
    case 13: return MTLBlendFactorOneMinusSourceAlpha;
    /* 14/15 = BLENDFACTOR / INVBLENDFACTOR.  These were MISSING and both
     * fell back to One: the post-processing composite (SRC=14, DEST=15)
     * did "scene + scene" instead of a crossfade, and in a race the ground
     * was blown-out white from edge to edge. */
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation d3d_blendop(unsigned op){
    switch (op){
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static id<MTLRenderPipelineState> pso_for(uint64_t key){
    for (unsigned i = 0; i < g_pson; i++)
        if (g_psocache[i].key == key) return g_psocache[i].pso;
    if (!g_lib || g_pson >= MAXPSO) return g_pso;

    /* key: blend(0..19) | ps<<20 | vs<<32 | (HD) msaa<<48 | G<<49 | log2(samples)<<50 | no-discard<<52 */
    int psid = (int)((key >> 20) & 0xFFFu);
    int vsid = (int)((key >> 32) & 0xFFFu);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    /* A GPU vertex shader reads its own vertex data from the buffer:
     * NO vertexDescriptor is given (otherwise Metal expects stage_in). */
    pd.vertexFunction   = (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid])
                          ? g_vsfn[vsid]
                          : [g_lib newFunctionWithName:@"vmain"];
    int nd = (int)((key >> 52) & 1u);          /* alpha test off: the variant without discard */
    pd.fragmentFunction = (psid > 0 && psid < MAXPS && g_psfn[psid])
                          ? ((nd && g_psfn_nd[psid]) ? g_psfn_nd[psid] : g_psfn[psid])
                          : [g_lib newFunctionWithName:(nd ? @"fmain_nd" : @"fmain")];
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    pd.vertexDescriptor = (vsid > 0 && vsid < MAXVSF && g_vsfn[vsid]) ? nil : g_vdesc;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    unsigned en   = key & 1u;
    unsigned sf   = (key >> 1) & 31u;
    unsigned df   = (key >> 6) & 31u;
    unsigned op   = (key >> 11) & 7u;
    unsigned mask = (key >> 14) & 15u;
    pd.colorAttachments[0].blendingEnabled = en ? YES : NO;
    pd.colorAttachments[0].sourceRGBBlendFactor        = d3d_blend(sf, 0);
    pd.colorAttachments[0].destinationRGBBlendFactor   = d3d_blend(df, 0);
    pd.colorAttachments[0].sourceAlphaBlendFactor      = d3d_blend(sf, 1);
    pd.colorAttachments[0].destinationAlphaBlendFactor = d3d_blend(df, 1);
    pd.colorAttachments[0].rgbBlendOperation   = d3d_blendop(op);
    pd.colorAttachments[0].alphaBlendOperation = d3d_blendop(op);
    MTLColorWriteMask cw = 0;
    if (mask & 1) cw |= MTLColorWriteMaskRed;
    if (mask & 2) cw |= MTLColorWriteMaskGreen;
    if (mask & 4) cw |= MTLColorWriteMaskBlue;
    if (mask & 8) cw |= MTLColorWriteMaskAlpha;
    pd.colorAttachments[0].writeMask = cw;
    if ((key >> 48) & 1u) pd.rasterSampleCount = ((key >> 50) & 3u) ? (NSUInteger)1u << ((key >> 50) & 3u)
                                                                    : (NSUInteger)hd_msaa_n();
#if NFS_ENHANCED
    if ((key >> 49) & 1u){
        /* G-buffer: blended draws and the fixed-function shader (ps 0,
         * single output) do not write it -- the data of the surface
         * underneath stays. */
        MTLColorWriteMask gm = (en || psid == 0) ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
        for (int gi = 0; gi < HD_NG; gi++){
            pd.colorAttachments[1 + gi].pixelFormat = hd_gfmt(gi);
            pd.colorAttachments[1 + gi].writeMask = gm; }
    }
#endif

    NSError *e2 = nil;
    struct timespec _o0; if (g_perflog) clock_gettime(CLOCK_MONOTONIC, &_o0);
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e2];
    if (g_perflog){ struct timespec _o1; clock_gettime(CLOCK_MONOTONIC, &_o1);
        perflog_event(PL_PSO, (uint64_t)(_o1.tv_sec - _o0.tv_sec) * 1000000000ull + (uint64_t)_o1.tv_nsec - (uint64_t)_o0.tv_nsec, 0); }
    if (!ps) return g_pso;
    g_psocache[g_pson].key = key;
    g_psocache[g_pson].pso = ps;
    g_pson++;
    return ps;
}

/* DUMPTEX=<id>: draw the texture onto a full-screen quad and write it as PNG.
 * A cube texture is written as six PNGs, one per face (tag ...0..5). */
static void dump_one_texture_face(unsigned id_, long frame, int face);
static void dump_one_texture(unsigned id_, long frame){
    if (!g_dev || !g_pso || id_ >= MAXTEX || !g_tex[id_]) return;
    if (g_tex_cube[id_]){
        for (int f = 0; f < 6; f++) dump_one_texture_face(id_, frame, f);
        return;
    }
    dump_one_texture_face(id_, frame, -1);
}
static void dump_one_texture_face(unsigned id_, long frame, int face){
    (void)frame;
    NSUInteger w = g_tex[id_].width, h = g_tex[id_].height;
    if (!w || !h) return;
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                              width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> off = [g_dev newTextureWithDescriptor:td];
    if (!off) return;
    MVert q[6]; memset(q, 0, sizeof q);
    { const float px[6] = {-1, 1, 1,-1, 1,-1}, py[6] = { 1, 1,-1, 1,-1,-1};
      const float tu[6] = { 0, 1, 1, 0, 1, 0}, tv2[6] = { 0, 0, 1, 0, 1, 1};
      for (int i = 0; i < 6; i++){
          q[i].x = px[i]; q[i].y = py[i]; q[i].z = 0.f; q[i].w = 1.f;
          q[i].r = q[i].g = q[i].b = q[i].a = 1.f;
          for (int k = 0; k < 4; k++){
              q[i].t[k][0] = tu[i]; q[i].t[k][1] = tv2[i];
              q[i].t[k][2] = 0.f;   q[i].t[k][3] = 1.f;
          }
      } }
    memcpy([g_mtlvb contents], q, sizeof q);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = off;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_pso];
    [enc setVertexBuffer:g_mtlvb offset:0 atIndex:0];
    [enc setFragmentSamplerState:g_samp atIndex:0];
    /* DUMPMIP=<level>: show only that mip level (through a texture view).
     * To check the uploaded mip contents directly. */
    id<MTLTexture> srct = g_tex[id_];
    long tag = 90000 + (long)id_;
    if (face >= 0){
        srct = [g_tex[id_] newTextureViewWithPixelFormat:g_tex[id_].pixelFormat
                    textureType:MTLTextureType2D levels:NSMakeRange(0, 1)
                         slices:NSMakeRange((NSUInteger)face, 1)];
        if (!srct) return;
        tag = 900000 + (long)id_ * 10 + face;
    }
    { const char *ml = ENVC("DUMPMIP");
      if (ml){ NSUInteger lv = (NSUInteger)atoi(ml);
        if (lv < g_tex[id_].mipmapLevelCount){
            id<MTLTexture> vw = [g_tex[id_] newTextureViewWithPixelFormat:g_tex[id_].pixelFormat
                                    textureType:MTLTextureType2D
                                         levels:NSMakeRange(lv,1)
                                         slices:NSMakeRange(0,1)];
            if (vw){ srct = vw; tag = 95000 + (long)id_*10 + (long)lv; }
        } else fprintf(stderr,"metal: texture %u has no level %lu (%lu in total)\n",
                       id_,(unsigned long)lv,(unsigned long)g_tex[id_].mipmapLevelCount); } }
    [enc setFragmentTexture:srct atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    fprintf(stderr, "metal: texture %u (%lux%lu, %lu levels) dump\n", id_,
            (unsigned long)w, (unsigned long)h,
            (unsigned long)g_tex[id_].mipmapLevelCount);
    dump_texture(off, tag);
}

/* The game draws its own cursor; if the macOS cursor is visible too there
 * are two cursors on screen.  Hide the system cursor while inside the game
 * area, bring it back on leaving. */
static int g_cur_hidden = 0;
static void cursor_hide_update(int inside){
    if (inside && !g_cur_hidden){ [NSCursor hide];   g_cur_hidden = 1; }
    else if (!inside && g_cur_hidden){ [NSCursor unhide]; g_cur_hidden = 0; }
}
@interface NfsuView : NSView @end
@implementation NfsuView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = g_dev;
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;   /* must be readable for frame dumps */
    return l;
}
@end

/* The fullscreen preference is remembered across runs.  NFSFULL=1/0
 * overrides it (measurement runs with NFSFULL=0 do not depend on the
 * user's preference).
 * A test run (NOFOCUS, as tools/autorun.sh and tools/apptest.sh start the
 * game) never reads or writes the player's choice, which lives in the
 * player's own preferences (org.nfsu2.nativeport, shared by every copy of
 * the app): it starts in a window unless NFSFULL says otherwise.  Before,
 * an app test inherited NFSUFullScreen = 1, went full screen on the
 * player's display and saved 0 back when it left. */
static int fs_pref_test_run(void){
    const char *nf = ENVC("NOFOCUS");
    return nf && *nf && *nf != '0';
}
static void fs_pref_save(int on){
    const char *e = ENVC("NFSFULL");
    if (e && *e) return;                 /* a forced mode must not change the preference */
    if (fs_pref_test_run()) return;
    [[NSUserDefaults standardUserDefaults] setBool:on ? YES : NO forKey:@"NFSUFullScreen"];
}
static int fs_pref_load(void){
    const char *e = ENVC("NFSFULL");
    if (e && *e) return e[0] != '0';
    if (fs_pref_test_run()) return 0;
    return [[NSUserDefaults standardUserDefaults] boolForKey:@"NFSUFullScreen"] ? 1 : 0;
}
/* lifecycle.c: quit and activation are handed to the game as window
 * messages (WM_CLOSE, WM_ACTIVATE/WM_ACTIVATEAPP), like Windows does. */
void lifecycle_request_quit(const char *why);
void lifecycle_set_active(int on, const char *why);
void lifecycle_tick(void);
static void cursor_hide_if_inside(void){
    if (!g_win || ![NSApp isActive]) return;
    NSPoint p = [g_win mouseLocationOutsideOfEventStream];
    cursor_hide_update([g_win isKeyWindow] && NSPointInRect(p, [[g_win contentView] frame]));
}
/* Vsync clock for the frame pacer (framerate.c).  A CADisplayLink on the
 * game view reports every refresh's timestamp (CACurrentMediaTime, i.e.
 * mach_absolute_time) and follows the window to other screens.  The pacer
 * puts its slot grid half a refresh after these vsyncs and paces with the
 * measured period rather than the nominal one: this "120 Hz" panel runs at
 * 120.0004 Hz, and an external one at 119.88 Hz would otherwise slowly
 * fill the presentation queue.  The callback only runs when the main loop
 * runs the run loop, which is fine: the timestamps are the vsyncs'. */
@interface NfsuVsync : NSObject @end
@implementation NfsuVsync
/* The link's nominal refresh as a display-rate signal (display_hz_apply). */
static void link_hz_note(double dur){
    int hz = g_link_sim_hz > 0 ? g_link_sim_hz
           : (dur > 0.001 && dur < 0.1) ? (int)lround(1.0 / dur) : 0;
    static int cand = -1; static double since = 0;
    double now = CACurrentMediaTime();
    if (hz != cand){ cand = hz; since = now; return; }
    if (hz == g_link_hz || now - since < 0.5) return;
    if (g_link_hz || (hz && hz < g_screen_hz - 1))
        fprintf(stderr, "metal: display link refresh %d Hz (screen reports %d Hz)\n", hz, g_screen_hz);
    g_link_hz = hz;
    display_hz_apply("display link");
}
- (void)tick:(id)link {
    void hfr_note_vsync(double ts, double duration);
    if (@available(macOS 14.0, *)){
        CADisplayLink *l = link;
        hfr_note_vsync(l.timestamp, l.duration);
        link_hz_note(l.duration);
    }
}
@end
static NfsuVsync *g_vs_obj;
static id g_vs_link;
/* Ask for the screen's maximum rate.  Left at its default, an adaptive panel
 * could run the link slower while the game renders slowly (a load), and the
 * lower link rate would then pull NFSU2_FPS=auto down with it. */
static void vsync_link_range(void){
    if (@available(macOS 14.0, *)){
        if (!g_vs_link || g_screen_hz <= 0) return;
        CADisplayLink *l = g_vs_link;
        float m = (float)g_screen_hz;
        l.preferredFrameRateRange = CAFrameRateRangeMake(m, m, m);
    }
}
static void vsync_link_start(NSView *v){
    if (ENVC("NFSU2_NO_VSYNC_CLOCK")) return;
    if (@available(macOS 14.0, *)){
        g_vs_obj = [NfsuVsync new];
        CADisplayLink *l = [v displayLinkWithTarget:g_vs_obj selector:@selector(tick:)];
        [l addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        g_vs_link = l;
        vsync_link_range();
    }
}
/* A display reconfiguration (mode, resolution, refresh rate, arrangement)
 * that need not move the window to another screen or change its backing
 * scale, so neither windowDidChangeScreen: nor
 * windowDidChangeBackingProperties: runs.  The refresh rate is re-read;
 * changed screen metrics (the panel density of a scaled mode, which
 * NFSU2_RENDER=panel uses) also reset the device. */
static void screen_params_changed(const char *why){
    int s0 = atomic_load(&g_scr_s100), p0 = atomic_load(&g_scr_ppp1000);
    scr_cache();
    int s1 = atomic_load(&g_scr_s100), p1 = atomic_load(&g_scr_ppp1000);
    fprintf(stderr, "app: %s: backing x%.2f, panel %.3f px/pt, refresh %d Hz\n", why,
            s1 / 100.0, p1 / 1000.0, atomic_load(&g_display_hz));
    if (s1 != s0 || p1 != p0){
        fprintf(stderr, "app: screen metrics changed -> device reset\n");
        com_request_reset(); }
}
/* Low Power Mode on or off.  Whether macOS lowers the screen's
 * maximumFramesPerSecond then or only runs the panel slower is not known
 * here; the screen is re-read and the display link covers the other case. */
static void power_state_changed(void){
    int lpm = 0;
    if (@available(macOS 12.0, *)) lpm = [[NSProcessInfo processInfo] isLowPowerModeEnabled] ? 1 : 0;
    scr_cache();
    fprintf(stderr, "app: Low Power Mode %s, refresh %d Hz\n", lpm ? "on" : "off", atomic_load(&g_display_hz));
}

@interface NfsuDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> @end
@implementation NfsuDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s { (void)s; return YES; }
/* The close button is a WM_CLOSE: the window stays until the game has shut
 * itself down (the main loop then ends, see lifecycle_guest_exited). */
- (BOOL)windowShouldClose:(NSWindow *)w { (void)w;
    lifecycle_request_quit("window close button");
    return NO; }
- (void)applicationDidBecomeActive:(NSNotification *)n { (void)n;
    /* A NOFOCUS test run is never meant to be the active app, but the system
     * can still activate it (seen twice, at start-up and in a menu, probably
     * when another app quit): give the activation straight back so the
     * user's keys stay with the user's app. */
    { const char *nf = ENVC("NOFOCUS");
      if (nf && *nf && *nf != '0' && [NSApp isActive]){
          fprintf(stderr, "app: NOFOCUS run was activated by the system -> deactivating\n");
          [NSApp deactivate];
          return; } }
    if (g_win && [g_win isMiniaturized]) return;          /* still in the Dock */
    lifecycle_set_active(1, "app became active");
    cursor_hide_if_inside(); }
/* A minimised window pauses the game like a minimised one on Windows
 * (WM_SIZE / IsIconic there, an inactive app here). */
- (void)windowDidMiniaturize:(NSNotification *)n { (void)n;
    lifecycle_set_active(0, "window minimized"); }
- (void)windowDidDeminiaturize:(NSNotification *)n { (void)n;
    if ([NSApp isActive]) lifecycle_set_active(1, "window restored"); }
/* Keys held while switching away never see their key-up: release them all
 * (on Windows the game drops its key state on WM_ACTIVATEAPP the same way). */
- (void)applicationDidResignActive:(NSNotification *)n { (void)n;
    kbdmac_release_all("app resigned active");
    mouse_release_all();
    cursor_hide_update(0);
    lifecycle_set_active(0, "app resigned active"); }
/* When the user enlarges the window or goes fullscreen the content size
 * changes; if the mouse mapping does not know, the cursor drifts. */
/* On entering fullscreen: measure the content area, save the preference
 * and have the game Reset to the new size (see g_fs). */
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    NSWindow *w = [n object];
    g_fs_transition = 0;
    NSSize c = [[w contentView] bounds].size;
    CGFloat s = [w backingScaleFactor]; if (s < 1.0) s = 1.0;
    atomic_store(&g_fs_pw, (int)lround(c.width * s));
    atomic_store(&g_fs_ph, (int)lround(c.height * s));
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    atomic_store(&g_fs, 1);
    fs_pref_save(1);
    fprintf(stderr, "app: fullscreen %.0fx%.0f points = %dx%d pixels -> rebuilding the device\n",
            (double)c.width, (double)c.height, atomic_load(&g_fs_pw), atomic_load(&g_fs_ph));
    com_request_reset(); }
- (void)windowDidExitFullScreen:(NSNotification *)n { (void)n;
    g_fs_transition = 0;
    atomic_store(&g_fs, 0);
    fs_pref_save(0);
    fprintf(stderr, "app: back to windowed mode -> rebuilding the device\n");
    com_request_reset(); }
/* Another window of ours (About, a panel) gets the keys from now on. */
- (void)windowDidResignKey:(NSNotification *)n { (void)n; cursor_hide_update(0);
    kbdmac_release_all("window resigned key"); mouse_release_all(); }
/* The user resized the window by hand: pick the drawing resolution again
 * for the new aspect (through the game's own Reset, see com_request_reset). */
- (void)windowDidEndLiveResize:(NSNotification *)n {
    NSSize c = [[[n object] contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width); atomic_store(&g_cont_h, (int)c.height);
    g_applied_cw = c.width; g_applied_ch = c.height;
    if (!atomic_load(&g_fs)) com_request_reset(); }
- (void)windowDidResize:(NSNotification *)n {
    NSWindow *w = [n object];
    NSSize c = [[w contentView] bounds].size;
    atomic_store(&g_cont_w, (int)c.width);
    atomic_store(&g_cont_h, (int)c.height);
    /* A resize that is not a live drag (zoom button, window tiling, a
     * script) ends here without windowDidEndLiveResize: pick the drawing
     * size for the new shape as well.  Fullscreen transitions resize in
     * steps and reset once when they finish. */
    if (![w inLiveResize] && !g_fs_transition && !atomic_load(&g_fs) &&
        g_win_sized && (fabs(c.width - g_applied_cw) >= 1.0 || fabs(c.height - g_applied_ch) >= 1.0)){
        fprintf(stderr, "app: window content %.0fx%.0f pt -> device reset\n", c.width, c.height);
        g_applied_cw = c.width; g_applied_ch = c.height;
        com_request_reset(); } }
/* The window moved to a screen with another scale factor (or the display
 * mode changed): new backing pixels, new panel density. */
- (void)windowDidChangeBackingProperties:(NSNotification *)n {
    NSWindow *w = [n object];
    CGFloat old = [[[n userInfo] objectForKey:NSBackingPropertyOldScaleFactorKey] doubleValue];
    scr_cache();
    if (g_layer) g_layer.contentsScale = [w backingScaleFactor];
    fprintf(stderr, "app: backing scale %.2f -> %.2f -> device reset\n", (double)old,
            (double)[w backingScaleFactor]);
    com_request_reset(); }
- (void)windowDidChangeScreen:(NSNotification *)n { (void)n;
    scr_cache();
    fprintf(stderr, "app: window changed screen -> device reset\n");
    com_request_reset(); }
- (void)windowWillEnterFullScreen:(NSNotification *)n { (void)n; g_fs_transition = 1; }
- (void)windowWillExitFullScreen:(NSNotification *)n { (void)n; g_fs_transition = 1; }
/* A transition AppKit gives up on ends here, not in the Did handlers above;
 * left set, the flag would keep windowDidResize from ever resetting again. */
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { (void)w; g_fs_transition = 0; }
- (void)windowDidFailToExitFullScreen:(NSWindow *)w { (void)w; g_fs_transition = 0; }
/* Cmd-Q: do NOT terminate directly.  Cutting the guest thread off midway
 * opens a shutdown race (see sub_0050d510).  We tell the draw loop to "stop"
 * and return CANCEL; once the loop exits, main() writes the report and ends.
 *
 * LATER was tried and is UNUSABLE: while waiting for the reply AppKit runs
 * its OWN run loop, our draw loop never gets control back -> the shutdown
 * hangs.  Cancel's only side effect is a warning at logout; the app quits
 * by itself right after that anyway. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)s {
    (void)s; lifecycle_request_quit("Cmd-Q / Dock / logout");
    return NSTerminateCancel; }
@end

/* Must be called on the main thread. Opens the window and runs the NSApp
 * loop; does not return (until the app quits).                          */
void metal_run_main_loop(void){
    @autoreleasepool {
        metal_early_init();
        if (!g_dev) return;

        [NSApplication sharedApplication];
        /* NOFOCUS=1 (test runs): no Dock icon, no focus stealing; the window
         * opens behind.  The user's keyboard does not go to the test window. */
        [NSApp setActivationPolicy:(ENVC("NOFOCUS") && ENVC("NOFOCUS")[0] != '0')
                                   ? NSApplicationActivationPolicyAccessory
                                   : NSApplicationActivationPolicyRegular];
        NfsuDelegate *del = [NfsuDelegate new];
        [NSApp setDelegate:del];
        /* We do not call [NSApp run] (the event loop is driven by hand below).
         * finishLaunching is normally run's first job: it registers the
         * AppleEvent handlers.  Without it the app never answers "Quit" (Dock >
         * Quit, logout, osascript quit) -- the process stays hung. */
        [NSApp finishLaunching];

        /* Menu bar.  Since the event loop is driven by hand below, AppKit
         * does not set it up by itself: in an app without a menu Cmd-Q,
         * Cmd-H, Cmd-M do not work and the window looks "stuck". */
        {   NSMenu *bar = [NSMenu new];
            NSMenuItem *appItem = [NSMenuItem new];
            [bar addItem:appItem];
            NSMenu *appMenu = [NSMenu new];
            [appMenu addItemWithTitle:@"About NFSU2"
                               action:@selector(orderFrontStandardAboutPanel:)
                        keyEquivalent:@""];
            [appMenu addItem:[NSMenuItem separatorItem]];
            [appMenu addItemWithTitle:@"Hide NFSU2" action:@selector(hide:) keyEquivalent:@"h"];
            [appMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:)
                        keyEquivalent:@"m"];
            [appMenu addItemWithTitle:@"Toggle Full Screen"
                               action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
            /* macOS's standard shortcut Ctrl+Cmd+F (without a menu item it
             * was bound to nothing); the item stays hidden in the menu. */
            { NSMenuItem *fs2 = [appMenu addItemWithTitle:@"Toggle Full Screen (Ctrl+Cmd+F)"
                                   action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
              [fs2 setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagCommand];
              if (@available(macOS 13.0, *)){
                  [fs2 setHidden:YES]; [fs2 setAllowsKeyEquivalentWhenHidden:YES]; } }
            fpscounter_add_menu(appMenu);      /* Cmd-N: frame-rate counter */
            texrep_add_menu(appMenu);          /* Cmd-T, shown once a texture pack is in use */
            [appMenu addItem:[NSMenuItem separatorItem]];
            [appMenu addItemWithTitle:@"Quit NFSU2" action:@selector(terminate:)
                        keyEquivalent:@"q"];
            [appItem setSubmenu:appMenu];
            [NSApp setMainMenu:bar];
        }

        /* The window size used to be FIXED at 1280x720: even when the game
         * asked CreateDevice for another resolution (g_RacingResolution in the
         * registry) the window did not change.  Since the device can be
         * created BEFORE Metal, the requested size is read here. */
        unsigned rw = g_want_w ? g_want_w : 1280u, rh = g_want_h ? g_want_h : 720u;
        /* If the game has not created its device yet, the placeholder is
         * Retina-scaled too; otherwise the window first opens at half size
         * (640x360 points) and grows when CreateDevice arrives.  Since nearly
         * all of the game's modes are 4:3, the placeholder has a 1280x960
         * aspect: in the most common case the window never jumps at CreateDevice. */
        if (!g_want_w){ const char *nv = ENVC("NFSNATIVE");
            if (!(nv && nv[0] == '0')){ rw = 1280u; rh = 960u;
                                         metal_native_size(1280u, 960u, &rw, &rh); } }
        NSRect frame = NSMakeRect(0, 0, (CGFloat)rw, (CGFloat)rh);
        g_win = [[NSWindow alloc] initWithContentRect:frame
                    styleMask:NFSU_WIN_STYLE
                      backing:NSBackingStoreBuffered defer:NO];
        [g_win setTitle:NFS_ENHANCED ? @"Need for Speed Underground 2 HD - native ARM64"
                                     : @"Need for Speed Underground 2 - native ARM64"];
        [g_win setDelegate:del];
        [g_win setRestorable:NO];        /* no Saved Application State */

        NfsuView *view = [[NfsuView alloc] initWithFrame:frame];
        [view setWantsLayer:YES];
        [g_win setContentView:view];
        g_layer = (CAMetalLayer *)view.layer;
        /* Needed so that StretchRect can read from the back buffer. */
        g_layer.framebufferOnly = NO;
        /* NOVSYNC=1: turn vsync off.  For MEASUREMENT only -- with it off
         * the image tears and 1:1 fidelity is lost.
         * Display sync stays on in every paced NFSU2_FPS mode (framerate.c
         * paces the frames; a composited window cannot tear anyway); only
         * NOVSYNC=1 and NFSU2_FPS=uncapped present immediately. */
        { int hfr_uncapped(void);
          if (ENVC("NOVSYNC") || hfr_uncapped()) g_layer.displaySyncEnabled = NO; }

        /* The green button goes fullscreen.  The drawing surface stays at
         * the size the game asked for; scale it up KEEPING the aspect ratio,
         * otherwise a 5:4 image is stretched across a wide screen. */
        [g_win setCollectionBehavior:[g_win collectionBehavior] |
                NSWindowCollectionBehaviorFullScreenPrimary];
        g_layer.contentsGravity = kCAGravityResizeAspect;
        g_win.backgroundColor = [NSColor blackColor];

        /* Match the window to the drawing surface one to one (see win_apply_size). */
        win_apply_size(rw, rh);
        [g_win center];
        display_hz_update([g_win screen] ? [g_win screen] : [NSScreen mainScreen]);
        vsync_link_start(view);
        /* Refresh changes that do not move the window (see display_hz_apply).
         * The main loop runs the main queue when it pumps events. */
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *nt){ (void)nt; screen_params_changed("display reconfigured"); }];
        if (@available(macOS 12.0, *))
            [[NSNotificationCenter defaultCenter]
                addObserverForName:NSProcessInfoPowerStateDidChangeNotification object:nil
                             queue:[NSOperationQueue mainQueue]
                        usingBlock:^(NSNotification *nt){ (void)nt; power_state_changed(); }];
        [g_win setAcceptsMouseMovedEvents:YES];
        /* NOFOCUS=1: open in the background and do NOT take focus from the
         * app the user is working in (for automated test/measurement runs). */
        if (ENVC("NOFOCUS") && ENVC("NOFOCUS")[0] != '0'){
            /* NOFOCUS=2: the window is NEVER visible (fully transparent, lets
             * the mouse through).  The window server still has it on screen,
             * so drawables and DUMPFRAME work as usual; with =1 it was behind
             * but visible. */
            if (ENVC("NOFOCUS")[0] == '2'){ [g_win setAlphaValue:0.0];
                                            [g_win setIgnoresMouseEvents:YES];
                                            [g_win setHasShadow:NO]; }
            [g_win orderBack:nil];
        }
        else { [g_win makeKeyAndOrderFront:nil];
               [NSApp activateIgnoringOtherApps:YES]; }
        if (fs_pref_load()) [g_win toggleFullScreen:nil];

        /* the draw resources were set up in metal_early_init() */
        fprintf(stderr, "metal: window open (%s), draw path %s\n",
                [[g_dev name] UTF8String], g_pso ? "READY" : "MISSING");

        /* QUITFRAMES=<n>: exit CLEANLY after n presented frames.  The ALARM
         * path calls _exit from the signal handler; that path skips atexit
         * (and the PGO profile write). */
        long quitat = 0;
        { const char *e = ENVC("QUITFRAMES"); if (e && *e) quitat = atol(e); }

        /* Draw loop: present the queued frame at 60 Hz */
        while (!atomic_load(&g_should_quit)){
            if (quitat && atomic_load(&g_frames_drawn) >= quitat){
                atomic_store(&g_should_quit, 1); break; }
            lifecycle_tick();
            rt_free_pending();
            /* FSFAKE=<frame>[,...]: fake a fullscreen transition as THE GAME
             * sees it, without touching the window (view = screen size, device
             * loss); real fullscreen is not reliable in the background (test). */
            { static long ff[8]; static int fn = -1, fi = 0;
              if (fn < 0){ fn = 0; const char *e = ENVC("FSFAKE");
                  for (const char *p = e; p && *p && fn < 8; ){
                      ff[fn++] = atol(p); while (*p && *p != ',') p++; if (*p) p++; } }
              if (fi < fn && atomic_load(&g_frames_drawn) >= ff[fi]){
                  fi++;
                  int on = !atomic_load(&g_fs);
                  if (on){ NSScreen *sc = [NSScreen mainScreen]; CGFloat k = [sc backingScaleFactor];
                           NSRect fr = [sc frame]; CGFloat top = 0;
                           if (@available(macOS 12.0, *)) top = [sc safeAreaInsets].top;
                           atomic_store(&g_fs_pw, (int)lround(fr.size.width * k));
                           atomic_store(&g_fs_ph, (int)lround((fr.size.height - top) * k)); }
                  atomic_store(&g_fs, on);
                  fprintf(stderr, "app: FSFAKE frame %ld -> %s (%dx%d)\n", atomic_load(&g_frames_drawn),
                          on ? "fullscreen" : "window", atomic_load(&g_fs_pw), atomic_load(&g_fs_ph));
                  com_request_reset(); } }
            /* FSSIM=<frame>:<k|m|c>[,...]: queue SYNTHETIC input at that frame
             * (test): k = fn+F, m = Ctrl+Cmd+F, c = a click on the green button,
             * p = Cmd+N (frame-rate counter).
             * The events pass through the event loop below like real input. */
            { static long sf[8]; static char sm[8]; static int sn = -1, si = 0;
              if (sn < 0){ sn = 0; const char *e = ENVC("FSSIM");
                  for (const char *p = e; p && *p && sn < 8; ){
                      sf[sn] = atol(p); while (*p && *p != ':' && *p != ',') p++;
                      sm[sn] = (*p == ':') ? p[1] : 'k'; sn++;
                      while (*p && *p != ',') p++; if (*p) p++; } }
              if (si < sn && atomic_load(&g_frames_drawn) >= sf[si]){
                  char md = sm[si++]; NSInteger wn = [g_win windowNumber];
                  fprintf(stderr, "app: FSSIM frame %ld mode %c\n", atomic_load(&g_frames_drawn), md);
                  if (md == 'k' || md == 'm' || md == 'p'){
                      NSEventModifierFlags fl = md == 'k' ? NSEventModifierFlagFunction
                          : md == 'p' ? NSEventModifierFlagCommand
                          : (NSEventModifierFlagControl | NSEventModifierFlagCommand);
                      NSString *ch = md == 'p' ? @"n" : @"f";      /* kVK_ANSI_N 0x2D, kVK_ANSI_F 0x03 */
                      for (int ud = 0; ud < 2; ud++){
                          NSEvent *ke = [NSEvent keyEventWithType:ud ? NSEventTypeKeyUp : NSEventTypeKeyDown
                              location:NSZeroPoint modifierFlags:fl timestamp:0 windowNumber:wn
                              context:nil characters:ch charactersIgnoringModifiers:ch
                              isARepeat:NO keyCode:md == 'p' ? 0x2D : 0x03];
                          [NSApp postEvent:ke atStart:NO]; }
                  } else {
                      NSButton *zb = [g_win standardWindowButton:NSWindowZoomButton];
                      NSPoint cp = zb ? [zb convertPoint:NSMakePoint(NSMidX([zb bounds]), NSMidY([zb bounds])) toView:nil]
                                      : NSZeroPoint;
                      for (int ud = 0; ud < 2; ud++){
                          NSEvent *me = [NSEvent mouseEventWithType:ud ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDown
                              location:cp modifierFlags:0 timestamp:0 windowNumber:wn context:nil
                              eventNumber:0 clickCount:1 pressure:ud ? 0.f : 1.f];
                          [NSApp postEvent:me atStart:NO]; }
                  } } }
            /* QUITSIM=<frame>:<k|t|c> (test): quit the way a user does, through
             * AppKit -- k = a Cmd-Q key event into the queue (menu key
             * equivalent), t = [NSApp terminate:] (Dock > Quit, logout),
             * c = [window performClose:] (the close button). */
            { static long qf = -1; static char qm = 0;
              if (qf == -1){ qf = -2; const char *e = ENVC("QUITSIM");
                  if (e && *e){ qf = atol(e); const char *c2 = strchr(e, ':'); qm = c2 ? c2[1] : 'k'; } }
              if (qf >= 0 && atomic_load(&g_frames_drawn) >= qf){
                  fprintf(stderr, "app: QUITSIM frame %ld mode %c\n", atomic_load(&g_frames_drawn), qm);
                  qf = -2;
                  if (qm == 't') [NSApp terminate:nil];
                  else if (qm == 'c') [g_win performClose:nil];
                  else {
                      NSEvent *ke = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
                          modifierFlags:NSEventModifierFlagCommand timestamp:0 windowNumber:[g_win windowNumber]
                          context:nil characters:@"q" charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:0x0C];
                      [NSApp postEvent:ke atStart:NO];
                  } } }
            /* REFRESHSIM=<frame>:<what>[,...] (test): run the refresh-rate
             * paths without touching the user's display settings.  what =
             * "params" (a display reconfiguration), "power" (a Low Power Mode
             * change) or "link:<hz>" (the display link reports <hz> from now
             * on; link:0 = its real rate again). */
            { static long rf[8]; static char rwh[8][16]; static int rn = -1, ri = 0;
              if (rn < 0){ rn = 0; const char *e = ENVC("REFRESHSIM");
                  for (const char *p = e; p && *p && rn < 8; ){
                      rf[rn] = atol(p); rwh[rn][0] = 0;
                      const char *q = strchr(p, ':'), *end = strchr(p, ',');
                      if (q && (!end || q < end)){
                          size_t l = end ? (size_t)(end - q - 1) : strlen(q + 1);
                          if (l > 15) l = 15;
                          memcpy(rwh[rn], q + 1, l); rwh[rn][l] = 0; }
                      rn++; while (*p && *p != ',') p++; if (*p) p++; } }
              if (ri < rn && atomic_load(&g_frames_drawn) >= rf[ri]){
                  const char *w = rwh[ri++];
                  fprintf(stderr, "app: REFRESHSIM frame %ld -> %s\n", atomic_load(&g_frames_drawn), w);
                  if (!strcmp(w, "power")) power_state_changed();
                  else if (!strncmp(w, "link:", 5)){
                      /* applied at once: the link may not tick at all (it
                       * stops while the display sleeps); while it ticks it
                       * keeps reporting the simulated rate */
                      g_link_sim_hz = atoi(w + 5); g_link_hz = g_link_sim_hz;
                      display_hz_apply("display link (REFRESHSIM)"); }
                  else screen_params_changed("display reconfigured (REFRESHSIM)");
              } }
            /* WINSIM=<frame>:<w>x<h>[,...]: resize the window content to w x h
             * points at that frame, like a zoom or tiling resize (test; with
             * NOFOCUS=2 the window is invisible).  <w>x<h> = "s" instead
             * re-runs the screen-change path (screen metrics + reset). */
            { static long wf[8]; static double ww[8], wh[8]; static int wn = -1, wi = 0;
              if (wn < 0){ wn = 0; const char *e = ENVC("WINSIM");
                  for (const char *p = e; p && *p && wn < 8; ){
                      wf[wn] = atol(p); ww[wn] = wh[wn] = 0;
                      const char *q = strchr(p, ':');
                      if (q && q[1] != 's') sscanf(q + 1, "%lfx%lf", &ww[wn], &wh[wn]);
                      wn++; while (*p && *p != ',') p++; if (*p) p++; } }
              if (wi < wn && atomic_load(&g_frames_drawn) >= wf[wi]){
                  int k = wi++;
                  fprintf(stderr, "app: WINSIM frame %ld -> %s %.0fx%.0f\n", atomic_load(&g_frames_drawn),
                          ww[k] > 0 ? "resize" : "screen change", ww[k], wh[k]);
                  if (ww[k] >= 160 && wh[k] >= 120) [g_win setContentSize:NSMakeSize(ww[k], wh[k])];
                  else [del windowDidChangeScreen:[NSNotification notificationWithName:NSWindowDidChangeScreenNotification object:g_win]];
              } }
            /* MOUSESIM=<frame>:<x>,<y>[:c][;...]: at that frame post a mouse move
             * (and with :c a left click) to the window content at x, y given
             * as fractions of the content width/height from the top left
             * (test of the point -> game cursor mapping; goes through the
             * normal event path below). */
            { static long mf[16]; static double mx[16], my[16]; static char mc[16];
              static int mn = -1, mi = 0;
              static long up_at = -1; static NSPoint up_p;      /* release 8 frames later */
              if (up_at >= 0 && atomic_load(&g_frames_drawn) >= up_at){
                  up_at = -1;
                  [NSApp postEvent:[NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:up_p
                      modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime]
                      windowNumber:[g_win windowNumber] context:nil eventNumber:0 clickCount:1
                      pressure:0.f] atStart:NO]; }
              if (mn < 0){ mn = 0; const char *e = ENVC("MOUSESIM");
                  for (const char *p = e; p && *p && mn < 16; ){
                      mf[mn] = atol(p); mx[mn] = my[mn] = 0.5; mc[mn] = 0;
                      const char *q = strchr(p, ':');
                      if (q){ sscanf(q + 1, "%lf,%lf", &mx[mn], &my[mn]);
                              const char *q2 = strchr(q + 1, ':');
                              const char *end = strchr(p, ';');
                              if (q2 && (!end || q2 < end) && q2[1] == 'c') mc[mn] = 1; }
                      mn++; while (*p && *p != ';') p++; if (*p) p++; } }
              if (mi < mn && atomic_load(&g_frames_drawn) >= mf[mi]){
                  int k = mi++;
                  NSRect cr = [[g_win contentView] frame];
                  NSPoint lp = NSMakePoint(cr.origin.x + mx[k] * cr.size.width,
                                           cr.origin.y + (1.0 - my[k]) * cr.size.height);
                  NSInteger wn = [g_win windowNumber];
                  NSEventType ts[2] = { NSEventTypeMouseMoved, NSEventTypeLeftMouseDown };
                  if (mc[k]){ up_at = atomic_load(&g_frames_drawn) + 8; up_p = lp; }
                  for (int t = 0; t < (mc[k] ? 2 : 1); t++){
                      NSEvent *me = [NSEvent mouseEventWithType:ts[t] location:lp modifierFlags:0
                          timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:wn context:nil
                          eventNumber:0 clickCount:1 pressure:t == 1 ? 1.f : 0.f];
                      [NSApp postEvent:me atStart:NO]; }
                  int gx, gy; mouse_from_point(lp.x, lp.y, &gx, &gy);
                  fprintf(stderr, "app: MOUSESIM frame %ld at %.3f,%.3f (%.0f,%.0f pt)%s -> game %d,%d\n",
                          atomic_load(&g_frames_drawn), mx[k], my[k], lp.x, lp.y,
                          mc[k] ? " click" : "", gx, gy);
              } }
            /* autotest.c "click <x> <y>": the same left click as MOUSESIM's
             * ":c", requested by the input script (fractions of the content
             * from the top left), released 8 frames later. */
            { static long cup = -1; static NSPoint cp;
              NSInteger wn = [g_win windowNumber];
              if (cup >= 0 && atomic_load(&g_frames_drawn) >= cup){
                  cup = -1;
                  [NSApp postEvent:[NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:cp modifierFlags:0
                      timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:wn context:nil
                      eventNumber:0 clickCount:1 pressure:0.f] atStart:NO]; }
              if (atomic_exchange(&g_click_req, 0)){
                  NSRect cr = [[g_win contentView] frame];
                  cp = NSMakePoint(cr.origin.x + g_click_x * cr.size.width, cr.origin.y + (1.0 - g_click_y) * cr.size.height);
                  NSEventType ts[2] = { NSEventTypeMouseMoved, NSEventTypeLeftMouseDown };
                  for (int t = 0; t < 2; t++)
                      [NSApp postEvent:[NSEvent mouseEventWithType:ts[t] location:cp modifierFlags:0
                          timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:wn context:nil
                          eventNumber:0 clickCount:1 pressure:t == 1 ? 1.f : 0.f] atStart:NO];
                  cup = atomic_load(&g_frames_drawn) + 8;
                  int gx, gy; mouse_from_point(cp.x, cp.y, &gx, &gy);
                  fprintf(stderr, "app: script click frame %ld at %.3f,%.3f -> game %d,%d\n",
                          atomic_load(&g_frames_drawn), g_click_x, g_click_y, gx, gy);
              } }
            /* FSTOGGLE=<frame>[,<frame>...]: toggle fullscreen at those frames (test) */
            { static long tg[8]; static int tn = -1, ti = 0;
              if (tn < 0){ tn = 0; const char *e = ENVC("FSTOGGLE");
                  for (const char *p = e; p && *p && tn < 8; ){
                      tg[tn++] = atol(p); while (*p && *p != ',') p++; if (*p) p++; } }
              if (ti < tn && atomic_load(&g_frames_drawn) >= tg[ti]){
                  ti++; fprintf(stderr, "app: FSTOGGLE frame %ld\n", atomic_load(&g_frames_drawn));
                  [g_win toggleFullScreen:nil]; } }
            @autoreleasepool {
                kbdmac_tick();                   /* synthesised keys (autotest ns*) into the queue */
                NSEvent *ev;
                while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES])){
                    NSEventType et = [ev type];
                    if (et == NSEventTypeKeyDown || et == NSEventTypeKeyUp || et == NSEventTypeFlagsChanged){
                        /* keys for another window of ours (About, a panel) */
                        if ([ev window] && [ev window] != g_win){ [NSApp sendEvent:ev]; continue; }
                        /* macOS's fullscreen shortcut fn/Globe+F: it carries
                         * no Command, so it used to reach the game as "F".  Only
                         * the key down: an F the game holds and that is
                         * released while fn is down must still get its key
                         * up (kbdmac_handle drops an F up it never saw go
                         * down). */
                        if (et == NSEventTypeKeyDown && [ev keyCode] == 0x03 &&
                            ([ev modifierFlags] & NSEventModifierFlagFunction)){
                            if (![ev isARepeat]) [g_win toggleFullScreen:nil];
                            continue; }
                        /* The game's keys (keyboard_mac.m).  Command shortcuts
                         * go to the menu, never to the game (Cmd-Q, Cmd-H,
                         * Cmd-M, Cmd-F, Ctrl-Cmd-F). */
                        if (!kbdmac_handle(ev)) [NSApp sendEvent:ev];
                        continue;
                    }
                    /* A mouse event OUTSIDE the game area (title bar:
                     * close/minimise/fullscreen buttons, dragging the window,
                     * resizing from an edge) must go to AppKit.  All clicks
                     * used to be swallowed by the game; the green button did
                     * not work. */
                    int m_ev = (et == NSEventTypeMouseMoved || et == NSEventTypeLeftMouseDragged ||
                                et == NSEventTypeRightMouseDragged || et == NSEventTypeLeftMouseDown ||
                                et == NSEventTypeLeftMouseUp || et == NSEventTypeRightMouseDown ||
                                et == NSEventTypeRightMouseUp || et == NSEventTypeOtherMouseDown ||
                                et == NSEventTypeOtherMouseUp || et == NSEventTypeOtherMouseDragged);
                    /* buttons: 0 left, 1 right, 2 middle, 3.. the others (DIMOUSESTATE2 has 8) */
                    int mb = (et == NSEventTypeLeftMouseDown || et == NSEventTypeLeftMouseUp) ? 0
                           : (et == NSEventTypeRightMouseDown || et == NSEventTypeRightMouseUp) ? 1
                           : (et == NSEventTypeOtherMouseDown || et == NSEventTypeOtherMouseUp)
                             ? (int)MIN([ev buttonNumber], 7) : -1;
                    int m_up = (et == NSEventTypeLeftMouseUp || et == NSEventTypeRightMouseUp ||
                                et == NSEventTypeOtherMouseUp);
                    if (m_ev){
                        int inside = [ev window] == g_win &&
                            NSPointInRect([ev locationInWindow], [[g_win contentView] frame]);
                        if (m_up && mb >= 0) mouse_button(mb, 0, [ev modifierFlags]);
                        if (!inside){
                            if (et == NSEventTypeMouseMoved) cursor_hide_update(0);
                            [NSApp sendEvent:ev]; continue; }
                    }
                    if (et == NSEventTypeMouseMoved || et == NSEventTypeLeftMouseDragged ||
                        et == NSEventTypeRightMouseDragged || et == NSEventTypeOtherMouseDragged){
                        NSPoint pt = [ev locationInWindow];
                        cursor_hide_update([ev window] == g_win &&
                            NSPointInRect(pt, [[g_win contentView] frame]) && [g_win isKeyWindow]);
                        int nx, ny; mouse_from_point(pt.x, pt.y, &nx, &ny);
                        double msx, msy; mouse_delta_scale(&msx, &msy);
                        atomic_fetch_add(&g_mouse_dx, (int)([ev deltaX] * msx));
                        atomic_fetch_add(&g_mouse_dy, (int)([ev deltaY] * msy));
                        atomic_store(&g_mouse_x, nx);
                        atomic_store(&g_mouse_y, ny);
                        kbd_post_mousemove(mouse_mk([ev modifierFlags]), mouse_lp());
                        continue;
                    }
                    if (mb >= 0){ if (!m_up) mouse_button(mb, 1, [ev modifierFlags]); continue; }
                    if (et == NSEventTypeScrollWheel){
                        /* 120 = one notch (WHEEL_DELTA); fractions of a line add up */
                        static double acc = 0;
                        acc += [ev deltaY] * 120.0;
                        int dz = (int)acc;
                        acc -= dz;
                        if (dz){
                            atomic_fetch_add(&g_mouse_dz, dz);
                            kbd_post(0x020Au, ((uint32_t)(dz & 0xFFFF) << 16) | mouse_mk([ev modifierFlags]),
                                     mouse_lp());                         /* WM_MOUSEWHEEL */
                        }
                        continue;
                    }
                    [NSApp sendEvent:ev];
                }

                /* Also service the run-loop sources outside the event queue.
                 * AppleEvents (Dock > Quit, the "quit" command, logout)
                 * arrive here; if only nextEventMatching... is called the
                 * app never answers a Quit. */
                [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                         beforeDate:[NSDate distantPast]];

                /* AUTOKEY=<dik>[,<dik>...]: periodic synthetic key presses to
                 * measure whether the game reacts to input. */
                /* AUTOCLICK=1: periodic synthetic left click + small mouse move.
                 * For measuring whether input reaches the game. */
                if (ENVC("AUTOCLICK")){
                    long f = atomic_load(&g_frames_drawn);
                    long ph = f % 120;
                    if (ph == 0){ atomic_fetch_add(&g_mouse_dx, 3);
                                  atomic_fetch_add(&g_mouse_dy, 2); }
                    if (ph == 60) atomic_fetch_or(&g_mouse_btn, 1u);
                    if (ph == 70) atomic_fetch_and(&g_mouse_btn, ~1u);
                }
                /* AUTOSEQ="<frame>:<dik>,<frame>:<dik>,...": press a key once
                 * at the given frame (for 10 frames).  AUTOKEY presses all
                 * the time; walking the menus to reach a given scene needed
                 * a scriptable sequence. */
                /* AUTOSEQ="<frame>:<dik>[:<dur>],...": hold the key for <dur>
                 * frames from that frame on (default 8).  The duration was
                 * needed: without driving the car we cannot see the
                 * headlights hit the surface. */
                { enum { SQMAX = 64 };
                  static int sqinit = 0; static long sqf[SQMAX], sqd[SQMAX];
                  static unsigned char sqk[SQMAX]; static int sqn = 0;
                  if (!sqinit){ sqinit = 1;
                    const char *e = ENVC("AUTOSEQ");
                    const char *q = e;
                    for (; q && *q && sqn < SQMAX; ){
                        while (*q==',' || *q==' ') q++;
                        if (!*q) break;
                        sqf[sqn] = strtol(q, 0, 0);
                        const char *c2 = strchr(q, ':');
                        if (!c2) break;
                        sqk[sqn] = (unsigned char)strtoul(c2+1, 0, 0);
                        const char *c3 = strchr(c2+1, ':');
                        const char *comma = strchr(q, ',');
                        sqd[sqn] = (c3 && (!comma || c3 < comma)) ? strtol(c3+1, 0, 0) : 8;
                        if (sqd[sqn] < 1) sqd[sqn] = 8;
                        sqn++;
                        while (*q && *q != ',') q++;
                    }
                    /* do not truncate SILENTLY past the limit (in one test the
                     * trailing Enters were lost and the game seemed "stuck") */
                    while (q && (*q == ',' || *q == ' ')) q++;
                    if (q && *q) fprintf(stderr, "app: WARNING AUTOSEQ cut at %d entries, the rest ignored\n", SQMAX);
                  }
                  if (sqn){
                    long f = atomic_load(&g_frames_drawn);
                    for (int q = 0; q < sqn; q++){
                        int on = (f >= sqf[q] && f < sqf[q] + sqd[q]);
                        if (on || f == sqf[q] + sqd[q])
                            kbd_set_key(KSRC_LEGACY, sqk[q], on);
                    }
                  }
                }
                { static int akinit = 0; static unsigned char akk[64]; static int akn = 0;
                  if (!akinit){ akinit = 1;
                    const char *e = ENVC("AUTOKEY");
                    for (const char *q = e; q && *q && akn < 64; ){
                        while (*q==',' || *q==' ') q++;
                        if (!*q) break;
                        akk[akn++] = (unsigned char)strtoul(q, 0, 0);
                        while (*q && *q != ',') q++;
                    }
                  }
                  /* AUTOKEYSTOP=<frame>: stop pressing after this frame.
                   * With AUTOKEY pressing Enter all the time the pre-race
                   * CUTSCENE was skipped; this was needed to see it. */
                  { static long akstop = -2;
                    if (akstop == -2){ const char *e = ENVC("AUTOKEYSTOP");
                        akstop = (e && *e) ? strtol(e, 0, 0) : -1; }
                    if (akn){
                      long f = atomic_load(&g_frames_drawn);
                      if (akstop >= 0 && f > akstop){
                          for (int q = 0; q < akn; q++) kbd_set_key(KSRC_LEGACY, akk[q], 0);
                      } else {
                          int slot = (int)((f / 30) % akn);
                          int on = (f % 30) < 10;
                          for (int q = 0; q < akn; q++)
                              kbd_set_key(KSRC_LEGACY, akk[q], on && q == slot);
                      }
                    } }
                }
                int have_new;
                pthread_mutex_lock(&g_qlock); have_new = (g_ready >= 0);
                pthread_mutex_unlock(&g_qlock);
                if (!have_new && atomic_load(&g_frame_pending)) g_skip_empty++;
                if (have_new && atomic_exchange(&g_frame_pending, 0)){
                    struct timespec _d0,_d1; clock_gettime(CLOCK_MONOTONIC,&_d0);
                    id<CAMetalDrawable> d = [g_layer nextDrawable];
                    clock_gettime(CLOCK_MONOTONIC,&_d1);
                    atomic_fetch_add(&g_wait_drawable_ns,
                      (unsigned long long)(_d1.tv_sec-_d0.tv_sec)*1000000000ull
                      + (unsigned long long)_d1.tv_nsec - (unsigned long long)_d0.tv_nsec);
                    if (d){
                        struct timespec _e0; clock_gettime(CLOCK_MONOTONIC,&_e0);
                        texrep_main_frame(g_queue);     /* optional texture packs (texrep.m) */
                        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
                        pp_begin(atomic_load(&g_frames_drawn) + 1);
                        int ab_count = 0;
                        g_ab_mode = ab_frame(atomic_load(&g_frames_drawn) + 1, &ab_count);
                        int ab_mode = g_ab_mode;
                        /* Do not overwrite a buffer the GPU may still be reading:
                         * take a free set for this frame (wait without the lock). */
                        int fbi = fb_acquire();
                        pthread_mutex_lock(&g_qlock);
                        int rb = g_ready;
                        g_enc_rb = rb;
                        /* Release the lock HERE.  It used to be held for the
                         * whole frame encode (0.4-2 ms) and then for the vertex
                         * copy as well; metal_draw_raw takes the same lock per
                         * draw, so the guest thread blocked on every draw in
                         * that window -- and that overlap is exactly what
                         * double buffering is for.  Safe: once g_enc_rb is set,
                         * rb belongs to the main thread only; the guest always
                         * writes the other one (g_cur) and metal_present does
                         * not swap while g_enc_rb >= 0 (not even on timeout). */
                        pthread_mutex_unlock(&g_qlock);
                        if (rb >= 0){
                            g_fb_vb[fbi]  = fb_fit(g_fb_vb[fbi],  sizeof(MVert)*(NSUInteger)g_vn[rb], sizeof(MVert)*MAXV);
                            g_fb_rvb[fbi] = fb_fit(g_fb_rvb[fbi], g_rvn[rb], MAXRV);
                            g_fb_kb[fbi]  = fb_fit(g_fb_kb[fbi],  g_kn[rb],  MAXKB);
                        }
                        g_mtlvb = g_fb_vb[fbi]; g_mtlrvb = g_fb_rvb[fbi]; g_mtlkb = g_fb_kb[fbi];
                        if (rb >= 0){
                            fb_put(g_mtlvb,  g_vbuf[rb],  sizeof(MVert)*(NSUInteger)g_vn[rb]);
                            fb_put(g_mtlrvb, g_rvbuf[rb], g_rvn[rb]);
                            fb_put(g_mtlkb,  g_kbuf[rb],  g_kn[rb]);
                        }

                        hd_msaa_frame(d.texture.width, d.texture.height);   /* see MSAA BY RESOLUTION */
                        /* Draws are ordered by target; each change of target
                         * opens a new render pass. */
                        id<MTLRenderCommandEncoder> enc = nil;
                        unsigned cur_t = 0xFFFFFFFFu;
                        unsigned cur_z = 0xFFFFFFFFu;   /* the pass's Z surface */
                        /* --- encoder state cache ----------------------------
                         * A frame has ~1350 draws; each one bound 4 samplers,
                         * the 532-byte constant buffer and 4 textures UNCONDITIONALLY.
                         * Rebinding what did not change is not free in Metal. */
                        id<MTLRenderPipelineState> c_pso = nil;
                        unsigned c_smp[4] = {~0u,~0u,~0u,~0u};
                        unsigned c_tex[4] = {~0u,~0u,~0u,~0u};
                        unsigned c_bcol = ~0u;
                        unsigned c_cull = ~0u;
                        unsigned c_depth = ~0u;
                        int c_zon = 0;                  /* the pass has a real Z surface */
                        int c_ms = 0;                   /* (msaa) the pass is multisampled */
                        int c_gb = 0;                   /* (HD research) the pass carries the G-buffer */
                        int hd_post = 0;                /* (msaa) first copy from the screen done: post-processing stage */
                        unsigned c_att = 0;             /* (msaa) resolve target bound to the pass: bit0 colour, bit1 G */
                        (void)hd_post; (void)c_att;
                        if (rb >= 0) pass_reorder(rb);  /* defer the pass that splits the scene (see PASS MERGING) */
#if NFS_ENHANCED
                        int hd_scene_open = 1;          /* main scene until the first copy from the screen */
                        int hd_gb_first = 1;            /* G-buffer used for the first time this frame */
                        int hd_refl_drawn = 0;          /* a reflection variant was drawn this frame */
                        unsigned hd_scene_zid = 0;      /* the main scene's Z surface */
#endif
                        memset(g_hd_zms_used, 0, sizeof g_hd_zms_used);
                        struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } c_fp;
                        int c_fp_valid = 0;
                        unsigned c_psci = ~0u;          /* c_fp's ps-constant pool index */
                        unsigned char touched[MAXTEX + 1];   /* bit (texture*8 + face) */
                        memset(touched, 0, sizeof touched);
                        int screen_touched = 0;

                        long fno_pre = atomic_load(&g_frames_drawn) + 1;
                        if (g_hsr_ab == 1 && gpuw_in(fno_pre)) g_hsr_cur = (int)(((fno_pre - g_gpuw_a) / 120) & 1);
                        if (ENVC("FRAMELOG") && rb >= 0 && (fno_pre % 10) == 0)
                            fprintf(stderr,"  [frame %3ld] draws=%u verts=%u\n",
                                    fno_pre, g_dn[rb], g_vn[rb]);
                        long dreq = atomic_exchange(&g_dump_req_frame, -1);
                        g_dump_cur_guest = dreq;
                        int  dumpnow = (g_dump_n && dump_wanted(fno_pre)) || dreq >= 0;
                        /* GLYPHDUMP: every quad's screen box + UVs.
                         * Goal: decode the TEXT the game draws to read its state. */
                        if (ENVC("GLYPHDUMP") && dumpnow && rb >= 0){
                            fprintf(stderr,"  [glyph] frame %ld, %u draws\n", fno_pre, g_dn[rb]);
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                if (dr->count < 3) continue;
                                MVert *v0 = &g_vbuf[rb][dr->first];
                                float xmin=1e30f,xmax=-1e30f,ymin=1e30f,ymax=-1e30f;
                                float umin=1e30f,vmin=1e30f,umax=-1e30f,vmax=-1e30f;
                                for (unsigned q=0;q<dr->count;q++){
                                    float w2 = v0[q].w != 0.f ? v0[q].w : 1.f;
                                    float X=v0[q].x/w2, Y=v0[q].y/w2;
                                    if(X<xmin)xmin=X; if(X>xmax)xmax=X;
                                    if(Y<ymin)ymin=Y; if(Y>ymax)ymax=Y;
                                    if(v0[q].t[0][0]<umin)umin=v0[q].t[0][0];
                                    if(v0[q].t[0][0]>umax)umax=v0[q].t[0][0];
                                    if(v0[q].t[0][1]<vmin)vmin=v0[q].t[0][1];
                                    if(v0[q].t[0][1]>vmax)vmax=v0[q].t[0][1];
                                }
                                fprintf(stderr,"    G %u %.5f %.5f %.5f %.5f %.6f %.6f %.6f %.6f %.3f %.3f %.3f\n",
                                        dr->tex, xmin,xmax,ymin,ymax, umin,umax,vmin,vmax,
                                        v0[0].r, v0[0].g, v0[0].b);
                            }
                        }
                        if (dumpnow && rb >= 0){
                            fprintf(stderr,"  [frame %ld] %u draws, %u verts\n",
                                    fno_pre, g_dn[rb], g_vn[rb]);
                            { unsigned hist[16]; memset(hist,0,sizeof hist);
                              unsigned other=0;
                              for (unsigned q=0;q<g_dn[rb];q++){
                                  unsigned tg=g_dbuf[rb][q].target;
                                  if (tg<16) hist[tg]++; else other++; }
                              fprintf(stderr,"    targets:");
                              for (unsigned q=0;q<16;q++) if (hist[q])
                                  fprintf(stderr," %s=%u", q?"":"screen", hist[q]),
                                  (q? fprintf(stderr,"(tex%u)",q):0);
                              if (other) fprintf(stderr," other=%u", other);
                              fprintf(stderr,"\n"); }
                            for (unsigned i = 0; i < g_dn[rb] && i < 200; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                fprintf(stderr,"    #%u target=%u tex=%u verts=%u blend=%05x ps=%d smp=%04x",
                                        i, dr->target, dr->tex, dr->count, dr->blend, dr->ps, dr->smp[0]);
                                if (dr->count >= 3){
                                    MVert *v0 = &g_vbuf[rb][dr->first];
                                    /* the quad's bounds + colour: to see why it is not visible */
                                    float xmin=1e30f,xmax=-1e30f,ymin=1e30f,ymax=-1e30f;
                                    for (unsigned q=0;q<dr->count;q++){
                                        float w2 = v0[q].w != 0.f ? v0[q].w : 1.f;
                                        float X = v0[q].x/w2, Y = v0[q].y/w2;
                                        if(X<xmin)xmin=X; if(X>xmax)xmax=X;
                                        if(Y<ymin)ymin=Y; if(Y>ymax)ymax=Y;
                                    }
                                    fprintf(stderr,"  x[%.3g..%.3g] y[%.3g..%.3g]"
                                            " colour=(%.2g %.2g %.2g a=%.2g) uv=(%.3g,%.3g)",
                                            xmin,xmax,ymin,ymax,
                                            v0[0].r,v0[0].g,v0[0].b,v0[0].a, v0[0].t[0][0], v0[0].t[0][1]);
                                }
                                fprintf(stderr,"\n");
                            }
                        }
                        /* SLIVER=1: find triangles that span the screen but
                         * have an area close to zero.  The 1-pixel diagonal
                         * lines on screen come from these; we measure which
                         * draw call produces them instead of guessing. */
                        /* DRAWV=<n>: print ALL vertices of draw number n.
                         * Whether two triangles have a seam on their shared
                         * edge can only be told from the raw values. */
                        /* DRAWSUM: group the frame's draws by the key
                         * (target,texture,ps,blend).  Shows the structure at a
                         * glance instead of thousands of lines. */
                        if (rb >= 0 && ENVC("DRAWSUM") && dump_wanted(fno_pre)){
                            typedef struct { unsigned tg,tx,t1,t2,bl,sm; int ps;
                                             unsigned nd,nv; float x0,x1,y0,y1; } Grp;
                            static Grp gp[192]; int gn = 0;
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                int k = -1;
                                for (int q=0;q<gn;q++)
                                    if (gp[q].tg==dr->target && gp[q].tx==dr->tex &&
                                        gp[q].t1==dr->tex1 && gp[q].t2==dr->tex2 &&
                                        gp[q].bl==dr->blend && gp[q].ps==dr->ps &&
                                        gp[q].sm==dr->smp[0]) { k=q; break; }
                                if (k < 0){ if (gn >= 192) continue; k = gn++;
                                    gp[k].tg=dr->target; gp[k].tx=dr->tex; gp[k].bl=dr->blend;
                                    gp[k].t1=dr->tex1; gp[k].t2=dr->tex2;
                                    gp[k].ps=dr->ps; gp[k].sm=dr->smp[0];
                                    gp[k].nd=gp[k].nv=0;
                                    gp[k].x0=gp[k].y0=1e30f; gp[k].x1=gp[k].y1=-1e30f; }
                                gp[k].nd++; gp[k].nv += dr->count;
                                if (dr->vsid <= 0 && dr->count >= 3){
                                    MVert *v0 = &g_vbuf[rb][dr->first];
                                    for (unsigned q=0;q<dr->count;q++){
                                        float w2 = v0[q].w != 0.f ? v0[q].w : 1.f;
                                        float X=v0[q].x/w2, Y=v0[q].y/w2;
                                        if(X<gp[k].x0)gp[k].x0=X; if(X>gp[k].x1)gp[k].x1=X;
                                        if(Y<gp[k].y0)gp[k].y0=Y; if(Y>gp[k].y1)gp[k].y1=Y;
                                    }
                                }
                            }
                            fprintf(stderr,"  [summary] frame %ld: %u draws, %d groups\n",
                                    fno_pre, g_dn[rb], gn);
                            for (int pass=0; pass<gn && pass<192; pass++){
                                int best=-1; unsigned bv=0;
                                for (int q=0;q<gn;q++) if (gp[q].nv>bv){ bv=gp[q].nv; best=q; }
                                if (best<0) break;
                                Grp *g = &gp[best];
                                { unsigned tw=0,th=0,dw=0,dh=0;
                                  if (g->tx && g->tx<MAXTEX && g_tex[g->tx]){
                                      tw=(unsigned)g_tex[g->tx].width; th=(unsigned)g_tex[g->tx].height; }
                                  if (g->tg && g->tg<MAXTEX && g_tex[g->tg]){
                                      dw=(unsigned)g_tex[g->tg].width; dh=(unsigned)g_tex[g->tg].height; }
                                  unsigned aw=0,ah=0;
                                  if (g->t1 && g->t1<MAXTEX && g_tex[g->t1]){
                                      aw=(unsigned)g_tex[g->t1].width; ah=(unsigned)g_tex[g->t1].height; }
                                  fprintf(stderr,"    target=%u(%ux%u) tex=%u(%ux%u) tex1=%u(%ux%u) tex2=%u"
                                        " ps=%d blend=%05x smp=%04x  %u draws %u verts\n",
                                        g->tg,dw,dh,g->tx,tw,th,g->t1,aw,ah,g->t2,
                                        g->ps,g->bl,g->sm,g->nd,g->nv); }
                                g->nv = 0;
                            }
                        }
                        /* DRAWLIST=1 (with DUMPFRAME): every draw's full state +
                         * constant hashes; to find the first differing draw of two runs. */
                        if (rb >= 0 && ENVC("DRAWLIST") && dumpnow){   /* also at script dumps */
                            for (unsigned di = 0; di < g_dn[rb]; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                uint64_t hp = 1469598103934665603ull, hk = 1469598103934665603ull;
                                const uint8_t *pp = (const uint8_t *)g_pscb[rb][dr->psci];
                                for (size_t q = 0; q < sizeof g_pscb[rb][0]; q++){ hp ^= pp[q]; hp *= 1099511628211ull; }
                                if (dr->vsid > 0 && g_kbuf[rb]){
                                    const uint8_t *kp = (const uint8_t *)g_kbuf[rb] + dr->koff;
                                    for (unsigned q = 0; q < dr->ksz && dr->koff + q < MAXKB; q++){ hk ^= kp[q]; hk *= 1099511628211ull; }
                                }
                                fprintf(stderr,"  [dl] %4u t=%x z=%x cull=%u bl=%05x ps=%d vs=%d tx=%u,%u,%u,%u n=%u clr=%u blit=%u psc=%016llx vk=%016llx\n",
                                        di, dr->target, dr->depth, dr->cull, dr->blend, dr->ps, dr->vsid,
                                        dr->tex, dr->tex1, dr->tex2, dr->tex3, dr->count, dr->is_clear, dr->is_blit,
                                        (unsigned long long)hp, (unsigned long long)hk);
                            }
                        }
                        if (rb >= 0 && ENVC("DRAWV") && dump_wanted(fno_pre)){
                            unsigned di = (unsigned)atoi(ENVC("DRAWV"));
                            if (di < g_dn[rb]){
                                MDraw *dr = &g_dbuf[rb][di];
                                fprintf(stderr,"  [verts] draw#%u vsid=%d verts=%u tex=%u\n",
                                        di, dr->vsid, dr->count, dr->tex);
                                if (dr->vsid <= 0){
                                    MVert *v0 = &g_vbuf[rb][dr->first];
                                    for (unsigned q = 0; q < dr->count && q < 12; q++)
                                        fprintf(stderr,"    v%u  pos=(%.9g, %.9g, %.9g, %.9g)"
                                                "  uv0=(%.9g, %.9g)\n", q,
                                                v0[q].x, v0[q].y, v0[q].z, v0[q].w,
                                                v0[q].t[0][0], v0[q].t[0][1]);
                                }
                            }
                        }
#if NFS_ENHANCED
                        if (rb >= 0 && ENVC("HD_RTPROBE") && dump_wanted(fno_pre))
                            hd_rt_probe(rb, d.texture, fno_pre);
                        g_hd_dump_now = dump_wanted(fno_pre);
                        /* CAMLOG=1 (with DUMPFRAME): the frame's camera + the vertex
                         * constants of the first few draws of world geometry. */
                        if (rb >= 0 && ENVC("CAMLOG") && dump_wanted(fno_pre)){
                            fprintf(stderr, "  [camera] frame %ld valid=%d\n", fno_pre, g_hd_cam_ok[rb]);
                            static const char *mn[3] = { "view", "projection", "product" };
                            for (int m = 0; m < 3; m++){
                                fprintf(stderr, "    %s\n", mn[m]);
                                for (int r = 0; r < 4; r++)
                                    fprintf(stderr, "      %11.5f %11.5f %11.5f %11.5f\n",
                                            g_hd_cam[rb][m*16+r*4], g_hd_cam[rb][m*16+r*4+1],
                                            g_hd_cam[rb][m*16+r*4+2], g_hd_cam[rb][m*16+r*4+3]); }
                            int shown = 0;
                            for (unsigned di = 0; di < g_dn[rb] && shown < 4; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                if (dr->target || dr->vsid <= 0 || dr->is_clear || dr->is_blit || dr->count < 30) continue;
                                const float *k = (const float *)(g_kbuf[rb] + dr->koff + 16);
                                unsigned nk = (dr->ksz - 16u) / 16u; if (nk > 16) nk = 16;
                                fprintf(stderr, "    draw#%u ps=%d vs=%d verts=%u constants c0..c%u:\n",
                                        di, dr->ps, dr->vsid, dr->count, nk ? nk-1 : 0);
                                for (unsigned q = 0; q < nk; q++)
                                    fprintf(stderr, "      c%-2u %11.5f %11.5f %11.5f %11.5f\n", q,
                                            k[q*4], k[q*4+1], k[q*4+2], k[q*4+3]);
                                shown++;
                            }
                        }
#endif
                        /* DRAWVPS=<ps>: dump the vertices of ALL draws that use that
                         * shader, together with all four texture coordinate sets. */
                        if (rb >= 0 && ENVC("DRAWVPS") && dump_wanted(fno_pre)){
                            int want = atoi(ENVC("DRAWVPS"));
                            for (unsigned di = 0; di < g_dn[rb]; di++){
                                MDraw *dr = &g_dbuf[rb][di];
                                if (dr->ps != want) continue;
                                fprintf(stderr,"  [verts-ps] draw#%u ps=%d vsid=%d verts=%u target=%u tex=%u,%u,%u,%u\n",
                                        di, dr->ps, dr->vsid, dr->count, dr->target,
                                        dr->tex, dr->tex1, dr->tex2, dr->tex3);
                                if (dr->vsid > 0) continue;
                                MVert *v0 = &g_vbuf[rb][dr->first];
                                for (unsigned q = 0; q < dr->count && q < 6; q++)
                                    fprintf(stderr,"    v%u pos=(%.5g,%.5g,%.5g,%.5g) t0=(%.6g,%.6g) t1=(%.6g,%.6g)"
                                            " t2=(%.6g,%.6g) t3=(%.6g,%.6g)\n", q,
                                            v0[q].x, v0[q].y, v0[q].z, v0[q].w,
                                            v0[q].t[0][0], v0[q].t[0][1], v0[q].t[1][0], v0[q].t[1][1],
                                            v0[q].t[2][0], v0[q].t[2][1], v0[q].t[3][0], v0[q].t[3][1]);
                            }
                        }
                        if (rb >= 0 && ENVC("PSHIST") && dump_wanted(fno_pre)){
                            /* Draw/triangle count per ps within the frame.  The
                             * DRAWSUM group table fills up at 192 and the LAST
                             * passes were not listed; this histogram covers all. */
                            struct { int ps; unsigned nd, nv, bl; } h[128]; int hn = 0;
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                int k = -1;
                                for (int q = 0; q < hn; q++) if (h[q].ps == dr->ps){ k = q; break; }
                                if (k < 0){ if (hn >= 128) continue; k = hn++;
                                            h[k].ps = dr->ps; h[k].nd = h[k].nv = 0;
                                            h[k].bl = dr->blend; }
                                h[k].nd++; h[k].nv += dr->count;
                            }
                            fprintf(stderr, "  [pshist] frame %ld: %u draws, %d distinct ps\n",
                                    fno_pre, g_dn[rb], hn);
                            for (int q = 0; q < hn; q++)
                                fprintf(stderr, "     ps=%-4d %5u draws %7u verts  blend=%05x\n",
                                        h[q].ps, h[q].nd, h[q].nv, h[q].bl);
                        }
                        if (rb >= 0 && ENVC("SLIVER") && dump_wanted(fno_pre)){
                            int nrep = 0;
                            for (unsigned i = 0; i < g_dn[rb] && nrep < 24; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                if (dr->vsid > 0 || dr->count < 3) continue;
                                MVert *v0 = &g_vbuf[rb][dr->first];
                                for (unsigned t = 0; t + 2 < dr->count && nrep < 24; t += 3){
                                    float X[3], Y[3];
                                    for (int q = 0; q < 3; q++){
                                        float w2 = v0[t+q].w != 0.f ? v0[t+q].w : 1.f;
                                        X[q] = v0[t+q].x / w2; Y[q] = v0[t+q].y / w2;
                                    }
                                    float ar = 0.5f * fabsf((X[1]-X[0])*(Y[2]-Y[0]) -
                                                            (X[2]-X[0])*(Y[1]-Y[0]));
                                    float sp = 0.f;
                                    for (int q = 0; q < 3; q++){
                                        int r2 = (q+1)%3;
                                        float dx = X[r2]-X[q], dy = Y[r2]-Y[q];
                                        float L = sqrtf(dx*dx+dy*dy); if (L > sp) sp = L;
                                    }
                                    if (sp > 0.5f && ar < 2e-4f){
                                        nrep++;
                                        fprintf(stderr,"  [sliver] draw#%u tri%u tex=%u"
                                                " blend=%05x ps=%d verts=%u  length=%.3f area=%.2e\n"
                                                "      (%.4f,%.4f) (%.4f,%.4f) (%.4f,%.4f)"
                                                "  colour=(%.2f %.2f %.2f a=%.2f)\n",
                                                i, t/3, dr->tex, dr->blend, dr->ps, dr->count,
                                                sp, ar, X[0],Y[0], X[1],Y[1], X[2],Y[2],
                                                v0[t].r, v0[t].g, v0[t].b, v0[t].a);
                                    }
                                }
                            }
                            if (!nrep) fprintf(stderr,"  [sliver] frame %ld: none found\n", fno_pre);
                        }
                        if (rb >= 0 && g_pso){
                            for (unsigned i = 0; i < g_dn[rb]; i++){
                                MDraw *dr = &g_dbuf[rb][i];
                                { static int sk = -2;
                                  if (sk == -2){ const char *e=ENVC("SKIPDRAW"); sk = e?atoi(e):-1; }
                                  if (sk >= 0 && (int)i == sk) continue; }
                                /* SKIPPS=<n>[,<n>...]: skip ALL draws that use this
                                 * pixel shader.  The most direct way to isolate what
                                 * one pass contributes. */
                                { static int spinit = 0; static int sps[16]; static int spn = 0;
                                  if (!spinit){ spinit = 1;
                                    const char *e = ENVC("SKIPPS");
                                    for (const char *q = e; q && *q && spn < 16; ){
                                        while (*q==',' || *q==' ') q++;
                                        if (!*q) break;
                                        sps[spn++] = atoi(q);
                                        while (*q && *q != ',') q++; } }
                                  int skip = 0;
                                  for (int q = 0; q < spn; q++) if (sps[q] == dr->ps) skip = 1;
                                  if (skip) continue; }
                                if (dr->is_blit){
                                    /* valid or not: hd_ms_end may have seen this copy and not stored the samples */
                                    if (!dr->blit_src && hd_mslazy() && (NFS_ENHANCED || hd_msaa_n() > 1)) hd_post = 1;
                                    /* Resolve the source/target texture; screen = drawable */
                                    id<MTLTexture> st2 = dr->blit_src
                                        ? ((dr->blit_src < MAXTEX) ? g_tex[dr->blit_src] : nil)
                                        : d.texture;
                                    id<MTLTexture> dt2 = dr->target
                                        ? ((TGT_ID(dr->target) < MAXTEX)
                                             ? g_tex[TGT_ID(dr->target)] : nil)
                                        : d.texture;
                                    if (st2 && dt2 && st2 != dt2){
                                        if (ENVC("BLITLOG")){
                                            static unsigned seen[32]; static int sn = 0;
                                            unsigned key = (dr->blit_src<<16) ^ dr->target;
                                            int nw = 1;
                                            for (int q=0;q<sn;q++) if (seen[q]==key) nw = 0;
                                            if (nw && sn < 32){ seen[sn++] = key;
                                                fprintf(stderr,"  [blit] texture %u (%lux%lu) -> texture %u (%lux%lu)  scale %.2fx\n",
                                                    dr->blit_src,(unsigned long)st2.width,(unsigned long)st2.height,
                                                    dr->target,(unsigned long)dt2.width,(unsigned long)dt2.height,
                                                    (double)st2.width/(double)(dt2.width?dt2.width:1)); }
                                        }
                                        if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                                  [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu; cur_z = 0xFFFFFFFFu;
                                                  if (pe) hd_ms_fallback(cb, d.texture, pe); }
#if NFS_ENHANCED
                                        /* SSR composite: the main scene is copied from the screen for
                                         * the FIRST time (post-processing begins).  Scene + G-buffer + Z are ready. */
                                        if (st2 == d.texture && hd_scene_open){
                                            hd_scene_open = 0;
                                            hd_composite(cb, rb, i, d.texture, hd_scene_zid, hd_refl_drawn);
                                        }
#endif
                                        /* MSAA: the screen contents live in the multisampled
                                         * texture.  A copy written straight to the drawable
                                         * would be OVERWRITTEN by the next screen pass's
                                         * resolve; draw the copy into the multisampled
                                         * texture as a quad and resolve. */
                                        if (dt2 == d.texture && hd_msaa_n() > 1 && hd_pso_ms() && !hd_post){
                                            g_hd_mscol = hd_ms_tex(g_hd_mscol, d.texture.pixelFormat,
                                                                   d.texture.width, d.texture.height);
                                            MTLRenderPassDescriptor *mp =
                                                [MTLRenderPassDescriptor renderPassDescriptor];
                                            mp.colorAttachments[0].texture = g_hd_mscol;
                                            mp.colorAttachments[0].resolveTexture = d.texture;
                                            mp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                                            mp.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                                            pp_rp(mp, "blit -> screen (MSAA quad)");
                                            id<MTLRenderCommandEncoder> me =
                                                [cb renderCommandEncoderWithDescriptor:mp];
                                            [me setRenderPipelineState:hd_pso_ms()];
                                            [me setVertexBuffer:g_blitvb offset:0 atIndex:0];
                                            [me setFragmentSamplerState:g_samp atIndex:0];
                                            [me setFragmentTexture:st2 atIndex:0];
                                            { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } bf;
                                              memset(&bf, 0, sizeof bf);
                                              [me setFragmentBytes:&bf length:sizeof bf atIndex:0]; }
                                            [me drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                                            [me endEncoding];
                                            screen_touched = 1;
                                            atomic_fetch_add(&g_hd_msblit, 1ull);
                                            continue;
                                        }
                                        /* At 1:1 and the same format use a REAL copy.
                                         * SAMPLING the drawable with a fullscreen quad
                                         * gave stale contents at tile (32x32) borders:
                                         * a rectangular mosaic in the sky and on glass
                                         * facades.  (Measured: the block edges sit at
                                         * exact multiples of 32/64 pixels.)  copyFromTexture
                                         * is both correct and cheaper. */
                                        if (st2.width == dt2.width && st2.height == dt2.height &&
                                            st2.pixelFormat == dt2.pixelFormat &&
                                            !ENVC("BLITQUAD")){
                                            id<MTLBlitCommandEncoder> bc = pp_blit(cb, "blit (same-size copy)");
                                            [bc copyFromTexture:st2 sourceSlice:0 sourceLevel:0
                                                   sourceOrigin:MTLOriginMake(0,0,0)
                                                     sourceSize:MTLSizeMake(st2.width, st2.height, 1)
                                                      toTexture:dt2 destinationSlice:0
                                               destinationLevel:0
                                              destinationOrigin:MTLOriginMake(0,0,0)];
                                            [bc endEncoding];
                                            if (dr->target){ unsigned tk_=TGT_ID(dr->target)*8u+TGT_FACE(dr->target);
                                                     touched[tk_>>3] |= 1u<<(tk_&7); }
                                            else screen_touched = 1;
                                            continue;
                                        }
                                        /* Scaled blit: sampling the drawable
                                         * DIRECTLY gives stale contents at tile
                                         * borders (32/64 px mosaic).  First
                                         * take a real copy into a scratch
                                         * texture, then sample that, scaled. */
                                        if (st2 == d.texture){
                                            static id<MTLTexture> scratch;
                                            if (!scratch || scratch.width != st2.width ||
                                                scratch.height != st2.height ||
                                                scratch.pixelFormat != st2.pixelFormat){
                                                MTLTextureDescriptor *sd2 =
                                                  [MTLTextureDescriptor
                                                    texture2DDescriptorWithPixelFormat:st2.pixelFormat
                                                    width:st2.width height:st2.height mipmapped:NO];
                                                sd2.usage = MTLTextureUsageShaderRead |
                                                            MTLTextureUsageRenderTarget;
                                                sd2.storageMode = MTLStorageModePrivate;
                                                scratch = [g_dev newTextureWithDescriptor:sd2];
                                            }
                                            if (scratch){
                                                id<MTLBlitCommandEncoder> bc2 = pp_blit(cb, "blit (scratch copy)");
                                                [bc2 copyFromTexture:st2 sourceSlice:0 sourceLevel:0
                                                        sourceOrigin:MTLOriginMake(0,0,0)
                                                          sourceSize:MTLSizeMake(st2.width,st2.height,1)
                                                           toTexture:scratch destinationSlice:0
                                                    destinationLevel:0
                                                   destinationOrigin:MTLOriginMake(0,0,0)];
                                                [bc2 endEncoding];
                                                st2 = scratch;
                                            }
                                        }
                                        MTLRenderPassDescriptor *bp =
                                            [MTLRenderPassDescriptor renderPassDescriptor];
                                        bp.colorAttachments[0].texture = dt2;
                                        bp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                                        bp.colorAttachments[0].storeAction = MTLStoreActionStore;
                                        { char pn[48]; snprintf(pn, sizeof pn, "blit quad -> %lux%lu", (unsigned long)dt2.width, (unsigned long)dt2.height);
                                          pp_rp(bp, pn); }
                                        id<MTLRenderCommandEncoder> be =
                                            [cb renderCommandEncoderWithDescriptor:bp];
                                        [be setRenderPipelineState:g_pso];
                                        [be setVertexBuffer:g_blitvb offset:0 atIndex:0];
                                        [be setFragmentSamplerState:g_samp atIndex:0];
                                        [be setFragmentTexture:st2 atIndex:0];
                                        { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } bf;
                                          memset(&bf, 0, sizeof bf);
                                          [be setFragmentBytes:&bf length:sizeof bf atIndex:0]; }
                                        [be drawPrimitives:MTLPrimitiveTypeTriangle
                                               vertexStart:0 vertexCount:6];
                                        [be endEncoding];
                                        if (dr->target){ unsigned tk_=TGT_ID(dr->target)*8u+TGT_FACE(dr->target);
                                                     touched[tk_>>3] |= 1u<<(tk_&7); }
                                        else screen_touched = 1;
                                    }
                                    continue;
                                }
                                /* Clear(ZBUFFER): clear that Z surface at the start
                                 * of the next pass.  If a pass with the same surface
                                 * is open, close it so the clear order is kept. */
                                if (dr->is_clear && (dr->clr_flags & 2u)){
                                    unsigned zc = DEPTH_ZID(dr->depth);
                                    if (zc && zc < MAXZ){
                                        g_zpend[zc] = 1;
                                        g_zpend_v[zc] = dr->clr_z < 0.f ? 0.f : dr->clr_z > 1.f ? 1.f : dr->clr_z;
                                        /* the multisampled twin consumes its own clear separately */
                                        if (zc < 64u){ g_hd_zpend_ms[zc] = 1; g_hd_zpend_msv[zc] = g_zpend_v[zc]; }
                                        if (enc && cur_z == zc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                                  [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu;
                                                  if (pe) hd_ms_fallback(cb, d.texture, pe); }
                                    }
                                }
#if NFS_ENHANCED
                                /* HD blur draw: from a road target to a road target.
                                 * If the source's mips are stale, close the pass,
                                 * generate them, reopen with the same target (Load). */
                                int hd_blur = !dr->is_clear && hd_is_road(dr->tex) &&
                                              hd_is_road(TGT_ID(dr->target));
                                if (hd_blur && g_hd_dirty[dr->tex]){
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil; cur_t = 0xFFFFFFFFu;
                                              if (pe) hd_ms_fallback(cb, d.texture, pe); }
                                    id<MTLBlitCommandEncoder> mb = pp_blit(cb, "road reflection mip generation");
                                    [mb generateMipmapsForTexture:g_tex[dr->tex]];
                                    [mb endEncoding];
                                    g_hd_dirty[dr->tex] = 0;
                                    atomic_fetch_add(&g_hd_mipgen, 1ull);
                                }
#endif
                                /* see PASS MERGING: a draw that does not use Z does not split the pass over its Z surface */
                                int zsplit = DEPTH_ZID(dr->depth) != cur_z &&
                                             !(enc && pass_merge_on() && !dr->is_clear && !(dr->depth & 3u));
                                /* a screen pass with only Clears in it: do not open it (the Z clear was recorded above) */
                                if (!enc || dr->target != cur_t || zsplit)
                                    if (!hd_post && !screen_touched && pass_clear_only(rb, i)) continue;
                                if (dr->target != cur_t || zsplit){
                                    if (enc){ int pe = hd_ms_end(enc, rb, i, c_ms, c_gb, c_att);
                                              [enc endEncoding]; enc = nil;
                                              if (pe) hd_ms_fallback(cb, d.texture, pe); }
#if NFS_ENHANCED
                                    if (hd_is_road(TGT_ID(dr->target))) g_hd_dirty[TGT_ID(dr->target)] = 1;
#endif
                                    id<MTLTexture> tgt = d.texture;
                                    if (dr->target) freed_use(TGT_ID(dr->target), 1);
                                    if (dr->target && TGT_ID(dr->target) < MAXTEX &&
                                        g_tex[TGT_ID(dr->target)])
                                        tgt = g_tex[TGT_ID(dr->target)];
                                    unsigned tk0 = TGT_ID(dr->target)*8u + TGT_FACE(dr->target);
                                    int first_use = dr->target
                                        ? !(touched[tk0>>3] & (1u<<(tk0&7)))
                                        : !screen_touched;
                                    MTLRenderPassDescriptor *rp =
                                        [MTLRenderPassDescriptor renderPassDescriptor];
                                    rp.colorAttachments[0].texture = tgt;
                                    if (TGT_FACE(dr->target))
                                        rp.colorAttachments[0].slice = TGT_FACE(dr->target) - 1u;
                                    rp.colorAttachments[0].loadAction =
                                        first_use ? MTLLoadActionClear : MTLLoadActionLoad;
                                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                                    rp.colorAttachments[0].clearColor = MTLClearColorMake(
                                        atomic_load(&g_clear_r), atomic_load(&g_clear_g),
                                        atomic_load(&g_clear_b), 1.0);
                                    c_ms = 0;
                                    int c_ml = 0;                   /* see MEMORYLESS MSAA ATTACHMENTS */
                                    g_hd_msp.on = 0; g_hd_msp.ml = 0; g_hd_msp.dz = 0;
                                    if (!dr->target && hd_msaa_n() > 1 && !hd_post){
                                        unsigned zq = DEPTH_ZID(dr->depth);
                                        /* no attachment will be loaded (colour first use, G first
                                         * time, depth to be cleared) and no sample will be stored */
                                        c_ml = hd_ms_memless() && first_use &&
#if NFS_ENHANCED
                                               (hd_gb_first || !(hd_scene_open && hd_ssr_on())) &&
#endif
                                               !(zq && zq < 64u && g_hd_msz_seen[zq] && g_hd_msz_kept[zq] &&
                                                 !g_hd_zpend_ms[zq]) &&
                                               hd_ms_memless_ok(rb, i);
                                        id<MTLTexture> msc;
                                        if (c_ml) msc = g_hd_mscol_ml = hd_ms_tex_ml(g_hd_mscol_ml, tgt.pixelFormat,
                                                                                     tgt.width, tgt.height);
                                        else      msc = g_hd_mscol = hd_ms_tex(g_hd_mscol, tgt.pixelFormat,
                                                                               tgt.width, tgt.height);
                                        if (!msc) c_ml = 0;
                                        if (msc){
                                            c_ms = 1;
                                            c_att = hd_ms_predict(rb, i);
                                            if (c_ml) atomic_fetch_add(&g_hd_ml_pass, 1ull);
                                            rp.colorAttachments[0].texture = msc;
                                            if (c_att & 1u) rp.colorAttachments[0].resolveTexture = tgt;
                                            rp.colorAttachments[0].storeAction =
                                                  c_ml ? ((c_att & 1u) ? MTLStoreActionMultisampleResolve   /* memoryless: final */
                                                                       : MTLStoreActionDontCare)
                                                : hd_mslazy() ? MTLStoreActionUnknown             /* at close: hd_ms_end */
                                                : MTLStoreActionStoreAndMultisampleResolve;
                                            atomic_fetch_add(&g_hd_mspass, 1ull);
                                        }
                                    }
                                    c_gb = 0;
#if NFS_ENHANCED
                                    if (!dr->target && hd_scene_open && hd_ssr_on()){
                                        NSUInteger ns = c_ms ? (NSUInteger)hd_msaa_n() : 1u;
                                        for (int gi = 0; gi < HD_NG; gi++){
                                            g_hd_g[gi] = hd_tex(g_hd_g[gi], hd_gfmt(gi), tgt.width, tgt.height, 1);
                                            if (ns > 1 && c_ml) g_hd_gms_ml[gi] = hd_ms_tex_ml(g_hd_gms_ml[gi], hd_gfmt(gi), tgt.width, tgt.height);
                                            else if (ns > 1) g_hd_gms[gi] = hd_tex(g_hd_gms[gi], hd_gfmt(gi), tgt.width, tgt.height, ns);
                                        }
                                        for (int gi = 0; gi < HD_NG; gi++){
                                            MTLRenderPassColorAttachmentDescriptor *ga = rp.colorAttachments[1 + gi];
                                            ga.texture = (ns > 1) ? (c_ml ? g_hd_gms_ml[gi] : g_hd_gms[gi]) : g_hd_g[gi];
                                            if (ns > 1){ if (c_att & 2u) ga.resolveTexture = g_hd_g[gi];
                                                         ga.storeAction = c_ml ? ((c_att & 2u) ? MTLStoreActionMultisampleResolve
                                                                                                : MTLStoreActionDontCare)
                                                                       : hd_mslazy() ? MTLStoreActionUnknown
                                                                       : MTLStoreActionStoreAndMultisampleResolve; }
                                            else ga.storeAction = MTLStoreActionStore;
                                            ga.loadAction = hd_gb_first ? MTLLoadActionClear : MTLLoadActionLoad;
                                            ga.clearColor = (gi == 2 && hd_gpack())     /* packed: zero direction = 512 */
                                                ? MTLClearColorMake(512.0 / 1023.0, 512.0 / 1023.0, 512.0 / 1023.0, 0)
                                                : MTLClearColorMake(0, 0, 0, 0);
                                        }
                                        hd_gb_first = 0;
                                        hd_scene_zid = DEPTH_ZID(dr->depth);
                                        c_gb = 1;
                                    }
#endif
                                    /* Z: the bound Z surface's own texture (see g_ztex).
                                     * With no surface, or one whose size does not match
                                     * the target, a scratch texture is bound and the Z
                                     * test is turned off (the pipelines always expect
                                     * Depth32Float). */
                                    unsigned zid = DEPTH_ZID(dr->depth);
                                    id<MTLTexture> ztex = nil;
                                    int zclear = 0; float zval = 1.f;
                                    if (zid && zid < MAXZ){
                                        if (atomic_exchange(&g_zforget[zid], 0)){ g_ztex[zid] = nil;
                                            if (zid < 64u){ g_hd_msz[zid] = nil; g_hd_msz_seen[zid] = 0; g_hd_msz_kept[zid] = 0; }
                                        }
                                        id<MTLTexture> zt = g_ztex[zid];
                                        if (!zt || zt.width != tgt.width || zt.height != tgt.height){
                                            /* the implicit surface (1) is sized with the screen;
                                             * created surfaces with the target they are first
                                             * used with.  Later use with a target of another
                                             * size: scratch. */
                                            if (!zt || zid == 1u){
                                                MTLTextureDescriptor *zd =
                                                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                                        MTLPixelFormatDepth32Float
                                                        width:tgt.width height:tgt.height mipmapped:NO];
                                                zd.usage = MTLTextureUsageRenderTarget
#if NFS_ENHANCED
                                                         | MTLTextureUsageShaderRead   /* SSR reads the depth */
#endif
                                                         ;
                                                zd.storageMode = MTLStorageModePrivate;
                                                g_ztex[zid] = zt = [g_dev newTextureWithDescriptor:zd];
                                                zclear = 1;
                                            } else { zt = nil; atomic_fetch_add(&g_z_mismatch, 1ull); }
                                        }
                                        ztex = zt;
                                        if (ztex && g_zpend[zid] && !c_ms){ zclear = 1; zval = g_zpend_v[zid]; g_zpend[zid] = 0; }
                                    }
                                    if (!ztex){
                                        if (!zid) atomic_fetch_add(&g_z_null, 1ull);
                                        if (!g_depth_tex ||
                                            g_depth_tex.width  != tgt.width ||
                                            g_depth_tex.height != tgt.height){
                                            MTLTextureDescriptor *zd =
                                                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                                    MTLPixelFormatDepth32Float
                                                    width:tgt.width height:tgt.height mipmapped:NO];
                                            zd.usage = MTLTextureUsageRenderTarget;
                                            zd.storageMode = MTLStorageModePrivate;
                                            g_depth_tex = [g_dev newTextureWithDescriptor:zd];
                                        }
                                    }
                                    id<MTLTexture> zres = nil;     /* depth resolve target (msaa) */
                                    if (c_ms){
                                        if (ztex && zid < 64u){
                                            zres = ztex;
                                            /* wants to load without a clear, but the last pass did not store */
                                            int lost = g_hd_msz_seen[zid] && !g_hd_msz_kept[zid] && !g_hd_zpend_ms[zid];
                                            if (c_ml){
                                                g_hd_msz_ml = hd_ms_tex_ml(g_hd_msz_ml, MTLPixelFormatDepth32Float,
                                                                           tgt.width, tgt.height);
                                                ztex = g_hd_msz_ml;
                                                zclear = 1;                     /* memoryless: always cleared */
                                            } else {
                                                id<MTLTexture> old = g_hd_msz[zid];
                                                g_hd_msz[zid] = hd_ms_tex(old, MTLPixelFormatDepth32Float,
                                                                          tgt.width, tgt.height);
                                                zclear = (g_hd_msz[zid] != old);       /* new: undefined */
                                                ztex = g_hd_msz[zid];
                                            }
                                            zval = 1.f;
                                            if (g_hd_zpend_ms[zid]){ zclear = 1; zval = g_hd_zpend_msv[zid]; g_hd_zpend_ms[zid] = 0; }
                                            if (lost){ zclear = 1; atomic_fetch_add(&g_hd_mszlost, 1ull); }
                                            g_hd_msz_seen[zid] = 1;
                                            g_hd_zms_used[zid] = 1;
                                            g_hd_msp.dz = 1; g_hd_msp.zid = zid; g_hd_msp.zclr = zclear;
                                        } else {
                                            /* screen pass without depth: always Clear + DontCare */
                                            g_hd_msdepth_scratch = hd_ml_hw()
                                                ? hd_ms_tex_ml(g_hd_msdepth_scratch, MTLPixelFormatDepth32Float, tgt.width, tgt.height)
                                                : hd_ms_tex(g_hd_msdepth_scratch, MTLPixelFormatDepth32Float, tgt.width, tgt.height);
                                        }
                                    }
                                    if (ztex){
                                        rp.depthAttachment.texture = ztex;
                                        rp.depthAttachment.loadAction = zclear ? MTLLoadActionClear : MTLLoadActionLoad;
                                        rp.depthAttachment.storeAction = MTLStoreActionStore;
                                        rp.depthAttachment.clearDepth = zval;
                                        /* RESOLVE the MSAA depth into the single-sample twin: passes
                                         * that draw to a single-sample target with the same Z after
                                         * the screen (light flares etc.) then see the scene's depth.
                                         * The twin now reflects the MS state, so its pending clear
                                         * is void. */
                                        if (zres && zres.width == ztex.width && zres.height == ztex.height &&
                                            !(ENVC("HD_ZRESOLVE") && ENVC("HD_ZRESOLVE")[0] == '0')){
                                            rp.depthAttachment.resolveTexture = zres;
                                            rp.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
                                            rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
                                            g_zpend[zid] = 0;
                                            g_hd_zsdirty[zid] = 0;
                                            atomic_fetch_add(&g_hd_zres, 1ull);
                                        }
                                        /* Storing the MS depth: memoryless decides at open (never
                                         * stored), Private chooses at close (hd_ms_end) */
                                        if (c_ms && g_hd_msp.dz && c_ml){
                                            rp.depthAttachment.storeAction = rp.depthAttachment.resolveTexture
                                                ? MTLStoreActionMultisampleResolve : MTLStoreActionDontCare;
                                            g_hd_msz_kept[g_hd_msp.zid] = 0;
                                            atomic_fetch_add(&g_hd_mszskip, 1ull);
                                            g_hd_msp.dz = 0;
                                        } else if (c_ms && g_hd_msp.dz && hd_mslazy()){
                                            g_hd_msp.res = rp.depthAttachment.resolveTexture != nil;
                                            rp.depthAttachment.storeAction = MTLStoreActionUnknown;
                                        } else g_hd_msp.dz = 0;
                                    } else if (g_depth_tex){
                                        rp.depthAttachment.texture = g_depth_tex;
                                        if (c_ms) rp.depthAttachment.texture = g_hd_msdepth_scratch;
                                        rp.depthAttachment.loadAction = MTLLoadActionClear;
                                        rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                                        rp.depthAttachment.clearDepth = 1.0;
                                    }
                                    cur_z = zid;
                                    c_zon = ztex != nil;
                                    g_hd_msp.on = c_ms; g_hd_msp.ml = c_ml;
                                    if (!c_ms) g_hd_msp.dz = 0;
                                    if (g_pp_sb){ char pn[48];
                                        if (!dr->target) snprintf(pn, sizeof pn, "SCREEN%s%s", c_ms ? " +MSAA" : "",
#if NFS_ENHANCED
                                                                  c_gb ? " +G-buffer" : "");
#else
                                                                  "");
#endif
                                        else snprintf(pn, sizeof pn, "%s #%u %lux%lu", TGT_FACE(dr->target) ? "cube face" : "target",
                                                      TGT_ID(dr->target), (unsigned long)tgt.width, (unsigned long)tgt.height);
                                        pp_rp(rp, pn);
                                        { static long ff = -1;        /* pass contents in the first profiled frame */
                                          if (ff < 0) ff = fno_pre;
                                          if (ff == fno_pre){
                                              unsigned nd = 0, nz = 0, nzt = 0, na = 0, zz = DEPTH_ZID(dr->depth);
                                              for (unsigned q = i; q < g_dn[rb]; q++){ MDraw *x = &g_dbuf[rb][q];
                                                  if (x->is_blit || x->target != dr->target || DEPTH_ZID(x->depth) != zz) break;
                                                  if (x->is_clear){ if (q > i && (x->clr_flags & 2u)) break; continue; }
                                                  nd++; if (x->depth & 2u) nz++; if (x->depth & 1u) nzt++; if (x->alpha_ref > 0.f) na++; }
                                              fprintf(stderr, "  [content] draw#%u %s: zid %u, %u draws, Z writing %u, Z testing %u, alpha tested %u, first ps %d tex %u\n",
                                                      i, pn, zz, nd, nz, nzt, na, dr->ps, dr->tex); } } }
                                    enc = [cb renderCommandEncoderWithDescriptor:rp];
                                    atomic_fetch_add(&g_pass_n, 1ull);
                                    [enc setVertexBuffer:g_mtlvb offset:0 atIndex:0];
                                    [enc setFragmentSamplerState:g_samp atIndex:0];
                                    /* new encoder = all state reset */
                                    c_pso = nil; c_bcol = ~0u; c_depth = ~0u; c_fp_valid = 0;
                                    c_cull = ~0u;
                                    /* In D3D the front face is CLOCKWISE.  CULLWIND=1
                                     * flips it (needed if our vertex path flips
                                     * Y). */
                                    { static int cw = -1;
                                      if (cw < 0){ const char *e = ENVC("CULLWIND");
                                                   cw = (e && *e && *e != '0') ? 1 : 0; }
                                      [enc setFrontFacingWinding:cw ? MTLWindingCounterClockwise
                                                                    : MTLWindingClockwise]; }
                                    for (int q4=0;q4<4;q4++){ c_smp[q4]=~0u; c_tex[q4]=~0u; }
                                    if (dr->target){ unsigned tk_=TGT_ID(dr->target)*8u+TGT_FACE(dr->target);
                                                     touched[tk_>>3] |= 1u<<(tk_&7); }
                                    else screen_touched = 1;
                                    cur_t = dr->target;
                                }
                                if (dr->is_clear || !dr->count) continue;
                                /* PSINFO=<n>: print the target/texture/blend of the
                                 * first draws that use this ps. */
                                { static int pi = -2; static int shown = 0;
                                  if (pi == -2){ const char *e=ENVC("PSINFO"); pi = e?atoi(e):-1; }
                                  if (pi >= 0 && dr->ps == pi && shown < 6){ shown++;
                                    fprintf(stderr,"  [psinfo] ps=%d target=%u tex=%u(%lux%lu) tex1=%u"
                                            " blend=%05x verts=%u  c0=(%.3f %.3f %.3f %.3f)\n",
                                            dr->ps, dr->target, dr->tex,
                                            (dr->tex && dr->tex<MAXTEX && g_tex[dr->tex])?(unsigned long)g_tex[dr->tex].width:0ul,
                                            (dr->tex && dr->tex<MAXTEX && g_tex[dr->tex])?(unsigned long)g_tex[dr->tex].height:0ul,
                                            dr->tex1, dr->blend, dr->count,
                                            (double)dr->c0[0],(double)dr->c0[1],
                                            (double)dr->c0[2],(double)dr->c0[3]); } }
#if NFS_ENHANCED
                                if (dr->ps >= PSREFL && c_gb){ hd_refl_drawn = 1; atomic_fetch_add(&g_hd_refl_draws, 1ull); }
#endif
                                if (g_hd_msn > 1){              /* msaa diagnostics (report) */
                                /* The same Z surface was used on the screen this frame
                                 * (through its multisampled twin) and is now Z TESTED on a
                                 * single-sample target: the twins share no contents, so
                                 * this draw cannot see the scene's depth.  Count it. */
                                /* The reverse: a single-sample pass WROTE Z, then the screen
                                 * (MS twin) tests against that depth -- the twin cannot see it. */
                                if (!c_ms && c_zon && cur_z < 64u && (dr->depth & 2u)) g_hd_zsdirty[cur_z] = 1;
                                if (c_ms && c_zon && cur_z < 64u && (dr->depth & 1u) && g_hd_zsdirty[cur_z])
                                    atomic_fetch_add(&g_hd_zrev, 1ull);
                                if (!c_ms && c_zon && cur_z < 64u && g_hd_zms_used[cur_z] && (dr->depth & 1u)){
                                    atomic_fetch_add(&g_hd_zshare, 1ull);
                                    if (ENVC("ZSHARELOG")){
                                        static unsigned zt[16], zz[16], zn[16], zps[16]; static int zk = 0;
                                        int q, f = -1;
                                        for (q = 0; q < zk; q++) if (zt[q] == dr->target && zz[q] == cur_z && zps[q] == (unsigned)dr->ps) f = q;
                                        if (f < 0 && zk < 16){ f = zk++; zt[f] = dr->target; zz[f] = cur_z; zps[f] = (unsigned)dr->ps; zn[f] = 0;
                                            id<MTLTexture> tt = (TGT_ID(dr->target) < MAXTEX) ? g_tex[TGT_ID(dr->target)] : nil;
                                            fprintf(stderr, "  [zshare] target=%u (%lux%lu) z=%u ps=%d vs=%d blend=%05x verts=%u\n",
                                                    dr->target, (unsigned long)tt.width, (unsigned long)tt.height,
                                                    cur_z, dr->ps, dr->vsid, dr->blend, dr->count); }
                                        if (f >= 0) zn[f]++;
                                    }
                                }
                                }
                                { uint64_t k2 = (uint64_t)(dr->blend & 0xFFFFFu)
                                               | ((uint64_t)(dr->ps & 0xFFF) << 20)
                                               | ((uint64_t)(dr->vsid & 0xFFF) << 32)
                                               | ((uint64_t)(c_ms & 1) << 48)
                                               | ((uint64_t)(c_gb & 1) << 49);
                                  if (c_ms) k2 |= (uint64_t)hd_msaa_l2() << 50;   /* see MSAA BY RESOLUTION */
                                  if (dr->alpha_ref <= 0.f && hsr_draw_on()) k2 |= 1ull << 52;
                                  id<MTLRenderPipelineState> ps2 = pso_for(k2);
                                  if (ps2 != c_pso){ c_pso = ps2;
                                      [enc setRenderPipelineState:ps2]; } }
                                /* BLENDFACTOR: encoder state, not pipeline state;
                                 * reset at every change of target. */
                                if (dr->cull != c_cull){ c_cull = dr->cull;
                                    [enc setCullMode:(dr->cull == 1u) ? MTLCullModeNone
                                                   : (dr->cull == 2u) ? MTLCullModeFront
                                                                      : MTLCullModeBack]; }
                                if (dr->bcol != c_bcol){ c_bcol = dr->bcol;
                                    [enc setBlendColorRed:(float)((dr->bcol>>16)&0xFFu)/255.f
                                                    green:(float)((dr->bcol>>8)&0xFFu)/255.f
                                                     blue:(float)(dr->bcol&0xFFu)/255.f
                                                    alpha:(float)((dr->bcol>>24)&0xFFu)/255.f]; }
                                for (int si = 0; si < 4; si++){
                                    unsigned sk = texrep_skey(si == 0 ? dr->tex : si == 1 ? dr->tex1 : si == 2 ? dr->tex2 : dr->tex3,
                                                              dr->smp[si]);
#if NFS_ENHANCED
                                    /* mip filter LINEAR (bits 4-5 = 2): a level() read is
                                     * ignored on a sampler without mips. */
                                    if (hd_blur) sk = (sk & ~0x30u) | 0x20u;
#endif
                                    if (sk != c_smp[si]){ c_smp[si] = sk;
                                        [enc setFragmentSamplerState:samp_for(sk) atIndex:si]; } }
                                { unsigned dm = c_zon ? (dr->depth & 63u) : 0u;
                                  if (dm != c_depth){ c_depth = dm;
                                      [enc setDepthStencilState:dss_for(dm)]; } }
                                { struct { float aref, usec, p0, p1; float proj[4]; float c[PSC_N][4]; float fogv[4]; } fp;
                                  fp.aref = dr->alpha_ref;
                                  fp.usec = 0.f; fp.p0 = fp.p1 = 0.f;
#if NFS_ENHANCED
                                  if (hd_blur){
                                      /* scale = Metal texture / the size the game thinks; keep
                                       * the sample spacing at ~1 texel at level L. */
                                      id<MTLTexture> st = g_tex[dr->tex];
                                      float sx = g_hd_ow[dr->tex] ? (float)st.width  / g_hd_ow[dr->tex] : 1.f;
                                      float sy = g_hd_oh[dr->tex] ? (float)st.height / g_hd_oh[dr->tex] : 1.f;
                                      float sc = sx > sy ? sx : sy;
                                      if (sc > 1.f){
                                          float L = enh_blur() * log2f(sc);
                                          fp.p0 = exp2f(L) / sc;     /* offset scale (0..1] */
                                          fp.p1 = L;                 /* mip level */
                                      }
                                      atomic_fetch_add(&g_hd_blurdraw, 1ull);
                                  }
#endif
                                  memcpy(fp.proj, dr->proj, sizeof fp.proj);
                                  memcpy(fp.fogv, dr->fogv, sizeof fp.fogv);
                                  /* same pool index + same small fields: no need to build
                                   * and compare the 548-byte block */
                                  if (!(c_fp_valid && dr->psci == c_psci &&
                                        !memcmp(&fp, &c_fp, offsetof(__typeof__(fp), c)) &&
                                        !memcmp(fp.fogv, c_fp.fogv, sizeof fp.fogv))){
                                      memcpy(fp.c, g_pscb[rb][dr->psci], sizeof fp.c);
                                      if (!c_fp_valid || memcmp(&fp, &c_fp, sizeof fp)){
                                          memcpy(&c_fp, &fp, sizeof fp); c_fp_valid = 1;
                                          [enc setFragmentBytes:&fp length:sizeof fp atIndex:0]; }
                                      c_psci = dr->psci; } }
                                { unsigned ts[4] = { dr->tex, dr->tex1, dr->tex2, dr->tex3 };
                                  for (int q4=0;q4<4;q4++){
                                      if (ts[q4] == c_tex[q4]) continue;
                                      c_tex[q4] = ts[q4];
                                      unsigned tt = ts[q4];
                                      freed_use(tt, 0);
                                      id<MTLTexture> txn = (tt && tt < MAXTEX && g_tex[tt])
                                                           ? texrep_pick(tt, g_tex[tt]) : g_white;
                                      [enc setFragmentTexture:txn atIndex:(NSUInteger)q4];
                                  } }
                                atomic_fetch_add(&g_draw_n, 1ull);
                                if (dr->vsid > 0 && dr->ix_isz){
                                    id<MTLBuffer> pl = g_pool_fr[rb];
                                    [enc setVertexBuffer:pl offset:dr->rawoff atIndex:1];
                                    [enc setVertexBuffer:g_mtlkb  offset:dr->koff   atIndex:2];
                                    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:dr->count
                                                     indexType:(dr->ix_isz == 4u) ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                                   indexBuffer:pl indexBufferOffset:dr->ix_ib
                                                 instanceCount:1 baseVertex:dr->ix_base baseInstance:0];
                                } else if (dr->vsid > 0){
                                    [enc setVertexBuffer:g_mtlrvb offset:dr->rawoff atIndex:1];
                                    [enc setVertexBuffer:g_mtlkb  offset:dr->koff   atIndex:2];
                                    [enc drawPrimitives:MTLPrimitiveTypeTriangle
                                            vertexStart:0 vertexCount:dr->count];
                                } else {
                                    [enc drawPrimitives:MTLPrimitiveTypeTriangle
                                            vertexStart:dr->first vertexCount:dr->count];
                                }
                            }
                            { long tt = (long)(g_vn[rb]/3);
                              for (unsigned i = 0; i < g_dn[rb]; i++)
                                  if (g_dbuf[rb][i].vsid > 0) tt += (long)(g_dbuf[rb][i].count/3);
                              atomic_fetch_add(&g_tris_drawn, tt); }
                        }
                        if (enc){ int pe = rb >= 0 ? hd_ms_end(enc, rb, g_dn[rb], c_ms, c_gb, c_att) : 0;
                                  [enc endEncoding]; enc = nil;
                                  if (pe) hd_ms_fallback(cb, d.texture, pe); }
                        /* if nothing was drawn to the screen, at least clear it */
                        if (!screen_touched){
                            MTLRenderPassDescriptor *rp =
                                [MTLRenderPassDescriptor renderPassDescriptor];
                            rp.colorAttachments[0].texture = d.texture;
                            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                            rp.colorAttachments[0].clearColor = MTLClearColorMake(
                                atomic_load(&g_clear_r), atomic_load(&g_clear_g),
                                atomic_load(&g_clear_b), 1.0);
                            id<MTLRenderCommandEncoder> e2 =
                                [cb renderCommandEncoderWithDescriptor:rp];
                            [e2 endEncoding];
                        }
                        pthread_mutex_lock(&g_qlock);
                        g_ready = -1; g_enc_rb = -1;
                        pthread_cond_signal(&g_qcond);
                        pthread_mutex_unlock(&g_qlock);
                        /* TESTFILL: fill with a known colour to verify the
                         * capture path.  If the PNG comes out magenta, capture works. */
                        if (ENVC("TESTFILL")){
                            MTLRenderPassDescriptor *rp =
                                [MTLRenderPassDescriptor renderPassDescriptor];
                            rp.colorAttachments[0].texture = d.texture;
                            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                            rp.colorAttachments[0].clearColor =
                                MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
                            id<MTLRenderCommandEncoder> e3 =
                                [cb renderCommandEncoderWithDescriptor:rp];
                            [e3 endEncoding];
                        }
                        if (g_perflog && atomic_exchange(&g_pl_thumb_req, 0)) pl_thumb(cb, d.texture, fno_pre);
                        /* frame-rate counter (off by default, fpscounter.m): after the
                         * game's frame; after the dump copy unless NFSU2_FPS_COUNTER_DUMP=1 */
                        int fc_px = fpscounter_on() && rb >= 0;
                        double fc_ppp = atomic_load(&g_cont_w) > 0 ? (double)d.texture.width / atomic_load(&g_cont_w) : 2.0;
                        unsigned fc_bw = 0, fc_bh = 0;
                        if (fc_px){ double bs = g_win ? [g_win backingScaleFactor] : 2.0;
                            fc_bw = (unsigned)lround(atomic_load(&g_cont_w) * bs);
                            fc_bh = (unsigned)lround(atomic_load(&g_cont_h) * bs); }
                        if (fc_px && fpscounter_in_dumps()){
                            fpscounter_draw(cb, d.texture, fc_ppp, fc_bw, fc_bh); fc_px = 0; }
                        /* Take a readable copy BEFORE presenting: reading back from a
                         * presented drawable's texture is not reliable. */
                        if (dumpnow){
                            /* recreate if the size changed (fullscreen transition) */
                            if (g_capture && (g_capture.width != d.texture.width ||
                                              g_capture.height != d.texture.height)) g_capture = nil;
                            if (!g_capture){
                                MTLTextureDescriptor *td =
                                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                        MTLPixelFormatBGRA8Unorm
                                        width:d.texture.width height:d.texture.height
                                        mipmapped:NO];
                                td.usage = MTLTextureUsageShaderRead;
                                td.storageMode = MTLStorageModeShared;
                                g_capture = [g_dev newTextureWithDescriptor:td];
                            }
                            if (g_capture){
                                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                                [bl copyFromTexture:d.texture sourceSlice:0 sourceLevel:0
                                       sourceOrigin:MTLOriginMake(0,0,0)
                                         sourceSize:MTLSizeMake(d.texture.width,
                                                                d.texture.height,1)
                                          toTexture:g_capture destinationSlice:0
                                   destinationLevel:0
                                  destinationOrigin:MTLOriginMake(0,0,0)];
                                [bl endEncoding];
                            }
                        }
                        if (fc_px) fpscounter_draw(cb, d.texture, fc_ppp, fc_bw, fc_bh);
                        { struct timespec _e1; clock_gettime(CLOCK_MONOTONIC,&_e1);
                          unsigned long long dt =
                              (unsigned long long)(_e1.tv_sec-_e0.tv_sec)*1000000000ull
                            + (unsigned long long)_e1.tv_nsec - (unsigned long long)_e0.tv_nsec;
                          atomic_fetch_add(&g_enc_ns, dt);
                          if (gpuw_in(fno_pre)){ atomic_fetch_add(&g_encw_ns, dt); atomic_fetch_add(&g_encw_n, 1ull); } }
                        int in_win = gpuw_in(fno_pre);
                        int hsr_m = -1;
                        if (g_hsr_ab == 1 && in_win){ long r = fno_pre - g_gpuw_a;
                            if ((r % 120) >= 30) hsr_m = g_hsr_cur; }
                        pp_end(cb);
                        [cb addCompletedHandler:^(id<MTLCommandBuffer> b2){
                            /* a frame that ended in an error: GPU times come back 0 and spoil the measurements */
                            if (b2.status == MTLCommandBufferStatusError){
                                unsigned long ne = atomic_fetch_add(&g_cb_err, 1ul);
                                if (ne < 5) fprintf(stderr, "metal: COMMAND BUFFER ERROR: %s\n",
                                                    [[b2.error localizedDescription] UTF8String]); }
                            fb_release(fbi);        /* GPU done: the set can be reused */
                            double g = (b2.GPUEndTime - b2.GPUStartTime) * 1e9;
                            if (g > 0.0 && g < 1e9){
                                unsigned long long ns = (unsigned long long)g;
                                if (in_win){ atomic_fetch_add(&g_gpuw_ns, ns); atomic_fetch_add(&g_gpuw_n, 1ull); }
                                if (ab_mode >= 0 && ab_count){ atomic_fetch_add(&g_ab_ns[ab_mode], ns); atomic_fetch_add(&g_ab_cnt[ab_mode], 1ull); }
                                if (hsr_m >= 0){ atomic_fetch_add(&g_hsr_ns[hsr_m], ns); atomic_fetch_add(&g_hsr_cnt[hsr_m], 1ull); }
                                atomic_fetch_add(&g_gpu_ns, ns);
                                atomic_fetch_add(&g_gpu_n, 1ull);
                                fpscounter_note_gpu(g / 1e6);
                                if (g_perflog) perflog_gpu(fno_pre, g);
                                unsigned long long mx = atomic_load(&g_gpu_max_ns);
                                while (ns > mx &&
                                       !atomic_compare_exchange_weak(&g_gpu_max_ns, &mx, ns)) {}
                            } }];
                        /* Content-rate pacing (framerate.c): at least the content
                         * period minus half a refresh after the previous drawable,
                         * so 60 fps on a 120 Hz panel holds every frame for exactly
                         * two refreshes.  presentedTime feeds the interval stats. */
                        { double hfr_present_min_duration(void);
                          void hfr_note_presented(double);
                          double md = hfr_present_min_duration();
                          [d addPresentedHandler:^(id<MTLDrawable> pd){ hfr_note_presented(pd.presentedTime);
                                                                        fpscounter_note_presented(pd.presentedTime); }];
                          if (md > 0.0) [cb presentDrawable:d afterMinimumDuration:md];
                          else [cb presentDrawable:d];
                          void hfr_note_present_call(void); hfr_note_present_call();
                          fpscounter_note_present(); }
                        [cb commit];
                        long fno = atomic_fetch_add(&g_frames_drawn, 1) + 1;
                        if (g_perflog){ struct timespec _p1; clock_gettime(CLOCK_MONOTONIC, &_p1);
                            /* number of distinct textures (+ targets) bound in the frame: the
                             * kernel cost of a command buffer submit grows with the resource count */
                            static uint32_t pl_st[MAXTEX]; static uint32_t pl_gen = 0;
                            unsigned pl_nt = 0; pl_gen++;
                            for (unsigned q = 0; rb >= 0 && q < g_dn[rb]; q++){
                                const MDraw *x = &g_dbuf[rb][q];
                                unsigned ids[5] = { x->tex, x->tex1, x->tex2, x->tex3, TGT_ID(x->target) };
                                for (int w = 0; w < 5; w++){ unsigned id_ = ids[w];
                                    if (id_ && id_ < MAXTEX && pl_st[id_] != pl_gen){ pl_st[id_] = pl_gen; pl_nt++; } }
                            }
                            perflog_render_frame(fno, pl_nt,
                                (uint64_t)(_d1.tv_sec - _d0.tv_sec) * 1000000000ull + (uint64_t)_d1.tv_nsec - (uint64_t)_d0.tv_nsec,
                                (uint64_t)(_p1.tv_sec - _e0.tv_sec) * 1000000000ull + (uint64_t)_p1.tv_nsec - (uint64_t)_e0.tv_nsec,
                                rb >= 0 ? g_dn[rb] : 0u, rb >= 0 ? g_vn[rb] : 0u); }
                        /* FPSCSV=<path>: per-frame time + workload.  To see WHERE
                         * the stutter is without guessing. */
                        { static FILE *fp = NULL; static int init = 0;
                          static uint64_t prev = 0;
                          if (!init){ init = 1; const char *e = ENVC("FPSCSV");
                              if (e && *e){ fp = fopen(e, "w");
                                  if (fp) fprintf(fp, "frame,ms,draws,verts,tris\n"); } }
                          if (fp){ struct timespec _ts;
                              clock_gettime(CLOCK_MONOTONIC, &_ts);
                              uint64_t now = (uint64_t)_ts.tv_sec*1000000ull
                                           + (uint64_t)_ts.tv_nsec/1000ull;
                              if (prev) fprintf(fp, "%ld,%.2f,%u,%u,%u\n", fno,
                                    (double)(now - prev)/1000.0,
                                    rb>=0?g_dn[rb]:0u, rb>=0?g_vn[rb]:0u,
                                    rb>=0?(g_vn[rb]/3u):0u);
                              prev = now;
                              if ((fno % 240) == 0) fflush(fp); } }
                        if (dumpnow){
                            [cb waitUntilCompleted];
                            if (g_capture) dump_texture(g_capture, fno_pre);
                            g_dump_cur_guest = -1;
                            { const char *dt = ENVC("DUMPTEX");   /* comma list */
                              for (const char *q = dt; q && *q; ){
                                  while (*q==',' || *q==' ') q++;
                                  if (!*q) break;
                                  unsigned tn = (unsigned)strtoul(q, 0, 0);
                                  dump_one_texture(tn, fno_pre*1000 + tn);
                                  while (*q && *q != ',') q++; } }
                        }
                        (void)fno;
                    }
                }
                /* SLEEP until a frame is ready (no polling).  The 4 ms
                 * cap is only there to drain the NSEvent queue: when a
                 * frame arrives, cond_signal wakes us at once. */
                { struct timespec ts, _f0, _f1; clock_gettime(CLOCK_MONOTONIC,&_f0);
                  clock_gettime(CLOCK_REALTIME, &ts);
                  ts.tv_nsec += 4L * 1000 * 1000;
                  if (ts.tv_nsec >= 1000000000L){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                  pthread_mutex_lock(&g_qlock);
                  if (g_ready < 0) pthread_cond_timedwait(&g_rcond, &g_qlock, &ts);
                  pthread_mutex_unlock(&g_qlock);
                  clock_gettime(CLOCK_MONOTONIC,&_f1);
                  atomic_fetch_add(&g_wait_frame_ns,
                    (unsigned long long)(_f1.tv_sec-_f0.tv_sec)*1000000000ull
                    + (unsigned long long)_f1.tv_nsec - (unsigned long long)_f0.tv_nsec); }
            }
        }
        fprintf(stderr, "metal: empty frames skipped = %ld\n", g_skip_empty);
        if (atomic_load(&g_frames_replaced))
            fprintf(stderr, "metal: mailbox: %ld undisplayed frames replaced by newer ones\n",
                    atomic_load(&g_frames_replaced));
        fprintf(stderr, "metal: buffer ring: in %lu frames the previous frame was still on the GPU "
                "(one buffer would have been overwritten), waited %lu times for a free set (%.1f ms in total)\n",
                atomic_load(&g_fb_overlap), atomic_load(&g_fb_wait),
                (double)atomic_load(&g_fb_wait_ns) / 1e6);
        { NSUInteger v = 0, r = 0, k = 0;
          for (int i = 0; i < NFB; i++){ v += g_fb_vb[i].length; r += g_fb_rvb[i].length; k += g_fb_kb[i].length; }
          fprintf(stderr, "metal: buffer ring size (all %d sets): verts %.1f MB + raw verts %.1f MB + constants %.1f MB, grew %lu times\n",
                  NFB, v / 1048576.0, r / 1048576.0, k / 1048576.0, atomic_load(&g_fb_grow)); }
        fprintf(stderr, "metal: vertex/index pool: %.1f MB (used %.1f MB), %lu mirrors live, %lu built (%.1f MB copied), %lu dropped, table full %lu\n",
                g_pool ? g_pool.length / 1048576.0 : 0.0, g_pool_top / 1048576.0, g_mir_live, g_mir_build,
                g_mir_bytes / 1048576.0, g_mir_drop, g_mir_full);
        { uint64_t fb = 0; for (int i = 0; i < g_pfn; i++) fb += g_pf[i].size;
          fprintf(stderr, "metal: pool free list: %d blocks, %.1f MB, %lu blocks did not fit the list; textures: %u ids, %lu released, %u free ids, queue full %lu\n",
                  g_pfn, fb / 1048576.0, g_pf_lost, g_texn, g_tex_released, g_ntfree, g_tfq_full); }
        fprintf(stderr, "metal: command buffer errors: %lu frames\n", atomic_load(&g_cb_err));
        { unsigned long n = atomic_load(&g_enc_late);
          fprintf(stderr, "metal: encoding took over 60 ms (formerly a swap onto the buffer being encoded): %lu times", n);
          for (unsigned long q = 0; q < n && q < 8; q++) fprintf(stderr, "%s%ld", q ? "," : "  frames: ", g_enc_late_f[q]);
          fprintf(stderr, "\n"); }
        fprintf(stderr, "metal: render targets released after Reset: %lu (used afterwards: as texture %lu, as target %lu)\n",
                g_rt_freed, g_freed_use_smp, g_freed_use_tgt);
        fprintf(stderr, "metal: Z surface: %llu passes without Z, %llu passes with a size mismatch (scratch), textures released after Reset %lu\n",
                (unsigned long long)g_z_null, (unsigned long long)g_z_mismatch, g_z_swept);
        fprintf(stderr, "metal: window closed (%ld frames drawn)\n",
                atomic_load(&g_frames_drawn));
    }
}

/* Frame budget report: GPU busy time, CPU encoding time, pass/draw counts.
 * "What are we spending the hardware on" can only be answered with these
 * three seen together. */
void metal_budget_report(void){
    if (g_hsr_ab == 1){
        for (int m = 0; m < 2; m++){ unsigned long long n = atomic_load(&g_hsr_cnt[m]);
            fprintf(stderr, "  HSRAB %s : %llu frames, GPU avg %.3f ms/frame\n", m ? "no-discard ON     " : "old (discard)     ",
                    n, n ? (double)atomic_load(&g_hsr_ns[m]) / n / 1e6 : 0.0); } }
#if NFS_ENHANCED
    fprintf(stderr, "[HD] road blur: %llu draws, %llu mip generations\n",
            atomic_load(&g_hd_blurdraw), atomic_load(&g_hd_mipgen));
    { unsigned long long f = atomic_load(&g_rt_frames);
      fprintf(stderr, "[HD] ray tracing: %llu frames, per frame avg %.0f triangles / %.0f dynamic draws; skipped without a kernel %llu, overflow %llu\n",
              f, f ? (double)atomic_load(&g_rt_tris) / f : 0.0, f ? (double)atomic_load(&g_rt_draws) / f : 0.0,
              atomic_load(&g_rt_skipk), atomic_load(&g_rt_over));
      fprintf(stderr, "[HD] RT cache: per frame avg dynamic %.0f triangles + static %.0f triangles; promoted %llu, evicted %llu, resets %llu, static builds %llu\n",
              f ? (double)atomic_load(&g_rt_dynv) / 3 / f : 0.0, f ? (double)atomic_load(&g_rt_statv) / 3 / f : 0.0,
              atomic_load(&g_rt_promo), atomic_load(&g_rt_evict), atomic_load(&g_rt_reset), atomic_load(&g_rt_sbuilds));
      fprintf(stderr, "[HD] RT dynamic structure: %llu full builds, %llu in-place updates (refit %s)\n",
              atomic_load(&g_rt_nbuild), atomic_load(&g_rt_nrefit), hd_rt_refit_on() ? "ON" : "OFF");
      fprintf(stderr, "[HD] RT cache: compactions %llu, rehashes %llu, store %u verts (%u dead)\n",
              atomic_load(&g_rt_compact), atomic_load(&g_rt_rehash), g_rt_scur, g_rt_sdead);
      if (g_rt_fixcnt){ const uint32_t *fc = (const uint32_t *)[g_rt_fixcnt contents];
          fprintf(stderr, "[HD] RT vertex check: %s; triangles disabled: NaN/infinite %u, very far %u (per frame avg %.1f)\n",
                  g_rt_fixk ? "ON" : "OFF", fc[0], fc[1], f ? (double)(fc[0] + fc[1]) / (double)f : 0.0); } }
    fprintf(stderr, "[HD] SSR: %llu composites, %llu reflection draws\n",
            atomic_load(&g_hd_ssr_n), atomic_load(&g_hd_refl_draws));
#endif
    if (NFS_ENHANCED || atomic_load(&g_hd_mspass)){
    fprintf(stderr, "[HD] MSAA lazy store: %s, mispredicted (extra resolve pass) %llu; deferred intermediate passes %llu\n",
            hd_mslazy() ? "ON" : "OFF", atomic_load(&g_hd_msfb), atomic_load(&g_hd_reord));
    fprintf(stderr, "[HD] MSAA %dx: %llu screen passes, %llu blits to the screen, %llu Z resolves,"
            " Z sharing: %llu screen->target draws, %llu target->screen draws (invisible)\n",
            g_hd_msn, atomic_load(&g_hd_mspass), atomic_load(&g_hd_msblit),
            atomic_load(&g_hd_zres), atomic_load(&g_hd_zshare), atomic_load(&g_hd_zrev));
    fprintf(stderr, "[HD] MSAA attachments: %llu passes MEMORYLESS, mispredicted %llu%s | MS depth: %llu stored, "
            "%llu not stored, %llu times a load without clear was asked but the samples were lost (should be 0)\n",
            atomic_load(&g_hd_ml_pass), atomic_load(&g_hd_ml_miss), g_hd_ml_off ? " (memoryless mode TURNED OFF)" : "",
            atomic_load(&g_hd_mszst), atomic_load(&g_hd_mszskip), atomic_load(&g_hd_mszlost));
    }
    unsigned long long n  = atomic_load(&g_gpu_n);
    unsigned long long gs = atomic_load(&g_gpu_ns);
    unsigned long long mx = atomic_load(&g_gpu_max_ns);
    unsigned long long es = atomic_load(&g_enc_ns);
    long fr = atomic_load(&g_frames_drawn);
    if (!fr) return;
    fprintf(stderr, "  --- frame budget ---\n");
    if (n) fprintf(stderr, "  GPU busy          : avg %.2f ms/frame, at most %.2f ms (%llu frames measured)\n",
                   (double)gs/(double)n/1e6, (double)mx/1e6, n);
    fprintf(stderr, "  CPU encoding      : avg %.2f ms/frame (writing the draw list to Metal)\n",
            (double)es/(double)fr/1e6);
    pp_report();
    if (g_ab_on > 0){
        fprintf(stderr, "  --- step-by-step measurement (HD_RT_AB) ---\n");
        for (int m = 0; m < AB_N; m++){ unsigned long long c = atomic_load(&g_ab_cnt[m]);
            if (c) fprintf(stderr, "  [AB] %-48s GPU %7.2f ms/frame (%llu frames)\n", g_ab_name[m], (double)atomic_load(&g_ab_ns[m]) / (double)c / 1e6, c); } }
    { unsigned long long wn = atomic_load(&g_gpuw_n), en = atomic_load(&g_encw_n);
      if (wn && en) fprintf(stderr, "  WINDOW %ld-%ld    : GPU %.3f ms/frame, CPU encoding %.3f ms/frame (%llu frames)\n",
              g_gpuw_a, g_gpuw_b, (double)atomic_load(&g_gpuw_ns)/(double)wn/1e6,
              (double)atomic_load(&g_encw_ns)/(double)en/1e6, wn); }
    fprintf(stderr, "  guest WAIT        : %.2f ms/frame (for the previous frame to reach the screen)\n"
                    "  main thread wait  : %.2f ms/frame frame-ready, %.2f ms/frame nextDrawable (vsync)\n",
            (double)atomic_load(&g_wait_present_ns)/(double)fr/1e6,
            (double)atomic_load(&g_wait_frame_ns)/(double)fr/1e6,
            (double)atomic_load(&g_wait_drawable_ns)/(double)fr/1e6);
    fprintf(stderr, "  render passes     : %.1f/frame,  draw calls: %.0f/frame\n",
            (double)atomic_load(&g_pass_n)/(double)fr,
            (double)atomic_load(&g_draw_n)/(double)fr);
    fprintf(stderr, "  pipelines         : %u of %d cache entries\n", g_pson, MAXPSO);
}

/* Size of a texture/render target by Metal id (0x0 if unknown). */
void metal_tex_size(unsigned id_, unsigned *w, unsigned *h){
    *w = *h = 0;
    if (!id_ || id_ >= MAXTEX || !g_tex[id_]) return;
    *w = (unsigned)g_tex[id_].width; *h = (unsigned)g_tex[id_].height;
}
