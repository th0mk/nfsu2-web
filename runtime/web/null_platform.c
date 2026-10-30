/* wasm: stand-ins for the Mac-only platform files (metal.m, audio.m,
 * gamepad.m, keyboard_mac.m, discord.c, perflog.c, roam_bsd.c, texrep.m,
 * fpscounter.m).
 *
 * Every function is weak: the real browser implementations (gpu_web.c,
 * audio_web.c, input_web.c, ...) define the same names and replace these at
 * link time.  With only this file the game runs headless: draws are counted
 * and dropped, textures get ids and sizes, the DirectSound play cursor runs in
 * real time (the EA sound library tears its buffer down if the cursor stands
 * still), and there is no pad, no Discord, no free-roam transport. */
#include "cpu.h"
#include "gfx.h"
#include "gamepad.h"
#include "roam_plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#define WEAK __attribute__((weak))

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int env_u(const char *n, int d){ const char *e = getenv(n); return e && *e ? atoi(e) : d; }

/* ------------------------------------------------------------ graphics */
static _Atomic long g_frames, g_tris, g_tex_created, g_tex_uploaded;
static _Atomic unsigned long g_tex_released;
static _Atomic int g_quit;
#define NTEX 16384
static struct { unsigned w, h, cube; } g_tex[NTEX];
static unsigned g_texn = 1;
/* freed ids are reused: the game creates and frees ~19,000 textures in a
 * 20-minute soak, and a bump-only table ran out after 16,383 (3,066 creations
 * failed); gpu_web.c keeps its own free list */
static unsigned g_tex_free[NTEX], g_ntex_free;
static _Atomic int g_tex_lk;
static void tex_lock(void){ int z; do { z = 0; } while (!atomic_compare_exchange_weak(&g_tex_lk, &z, 1)); }
static void tex_unlock(void){ atomic_store(&g_tex_lk, 0); }
static unsigned g_game_w, g_game_h;

WEAK int  metal_available(void){ return 1; }
WEAK int  metal_window_ready(void){ return 1; }
WEAK void metal_begin_frame(void){ }
WEAK void metal_present(void){ atomic_fetch_add(&g_frames, 1); }
WEAK long metal_frames(void){ return atomic_load(&g_frames); }
WEAK long metal_frames_dropped(void){ return 0; }
WEAK long metal_tris(void){ return atomic_load(&g_tris); }
WEAK long metal_tex_created(void){ return atomic_load(&g_tex_created); }
WEAK long metal_tex_uploaded(void){ return atomic_load(&g_tex_uploaded); }
WEAK unsigned long metal_tex_released(void){ return atomic_load(&g_tex_released); }
WEAK void metal_request_quit(void){ atomic_store(&g_quit, 1); }
WEAK void metal_request_dump(long f, const char *label){ (void)f; (void)label; }
WEAK void metal_budget_report(void){ fprintf(stderr, "  [gpu] null renderer: %ld frames, %ld textures created, %u ids in use\n",
                                                (long)g_frames, (long)g_tex_created, g_texn - 1 - g_ntex_free); }
WEAK int  metal_display_hz(void){ return env_u("NFSU2_DISPLAY_HZ", 60); }

static unsigned fmt_blk(unsigned f, unsigned *bpb){
    switch (f){
    case 21: case 22: *bpb = 4; return 1;
    case 28: case 41: *bpb = 1; return 1;
    case 0x31545844: *bpb = 8; return 4;
    case 0x33545844: case 0x35545844: *bpb = 16; return 4;
    default: *bpb = 0; return 0;
    }
}
WEAK unsigned metal_level_pitch(unsigned w, unsigned level, unsigned f){
    unsigned bpb, blk = fmt_blk(f, &bpb); if (!blk) return 0;
    unsigned lw = w >> level; if (!lw) lw = 1;
    return blk == 1 ? lw * bpb : ((lw + blk - 1) / blk) * bpb;
}
WEAK unsigned metal_level_rows(unsigned h, unsigned level, unsigned f){
    unsigned bpb, blk = fmt_blk(f, &bpb); if (!blk) return 0;
    unsigned lh = h >> level; if (!lh) lh = 1;
    return blk == 1 ? lh : (lh + blk - 1) / blk;
}
static unsigned tex_new(unsigned w, unsigned h, unsigned cube){
    unsigned id = 0;
    tex_lock();
    if (g_ntex_free) id = g_tex_free[--g_ntex_free];
    else if (g_texn < NTEX) id = g_texn++;
    tex_unlock();
    if (!id) return 0;
    g_tex[id].w = w; g_tex[id].h = h; g_tex[id].cube = cube;
    atomic_fetch_add(&g_tex_created, 1); return id;
}
WEAK unsigned metal_create_texture_ex(unsigned w, unsigned h, unsigned levels, unsigned f, unsigned is_rt){
    (void)levels; (void)is_rt; unsigned b; if (!fmt_blk(f, &b)) return 0; return tex_new(w, h, 0);
}
WEAK unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned f){ return metal_create_texture_ex(w, h, levels, f, 0); }
WEAK unsigned metal_create_texture_cube(unsigned edge, unsigned levels, unsigned f, unsigned is_rt){
    (void)levels; (void)is_rt; unsigned b; if (!fmt_blk(f, &b)) return 0; return tex_new(edge, edge, 1);
}
WEAK int  metal_tex_is_cube(unsigned id){ return id && id < NTEX && g_tex[id].cube; }
WEAK void metal_tex_size(unsigned id, unsigned *w, unsigned *h){
    if (w) *w = id < NTEX ? g_tex[id].w : 0; if (h) *h = id < NTEX ? g_tex[id].h : 0; }
WEAK void metal_free_texture(unsigned id){
    if (!id || id >= NTEX) return;
    atomic_fetch_add(&g_tex_released, 1);
    tex_lock(); if (g_ntex_free < NTEX) g_tex_free[g_ntex_free++] = id; tex_unlock();
}
WEAK void metal_upload_texture_face(unsigned id, unsigned level, unsigned face, const void *data, unsigned pitch, unsigned w, unsigned h, unsigned f){
    (void)id; (void)level; (void)face; (void)data; (void)pitch; (void)w; (void)h; (void)f; atomic_fetch_add(&g_tex_uploaded, 1); }
WEAK void metal_upload_texture(unsigned id, unsigned level, const void *data, unsigned pitch, unsigned w, unsigned h, unsigned f){
    metal_upload_texture_face(id, level, 0, data, pitch, w, h, f); }
WEAK void metal_miplevel_report(void){ }
/* com.c marks an enlarged road-reflection target, which only the Mac's
 * research build (NFS_ENHANCED=1) makes: nothing to do here */
WEAK void metal_hd_mark_road(unsigned id_, unsigned ow, unsigned oh){ (void)id_; (void)ow; (void)oh; }
WEAK void metal_mem_stats(uint64_t *a, unsigned *live, unsigned *slots, uint64_t *bytes){
    if (a) *a = 0; if (live) *live = g_texn - 1 - g_ntex_free; if (slots) *slots = NTEX; if (bytes) *bytes = 0; }
WEAK void metal_reset_rts(void){ }
WEAK void metal_draw_tv(const TVert *src, unsigned n, unsigned prim, unsigned tex){ (void)src; (void)tex; atomic_fetch_add(&g_tris, prim == 4 ? n / 3 : (n > 2 ? n - 2 : 0)); }
WEAK int  metal_draw_raw(const void *src, unsigned n, unsigned stride, unsigned prim, unsigned tex, int vsid, const float *k, unsigned nk){
    (void)src; (void)stride; (void)tex; (void)vsid; (void)k; (void)nk;
    atomic_fetch_add(&g_tris, prim == 4 ? n / 3 : (n > 2 ? n - 2 : 0)); return 1; }
WEAK int  metal_draw_ix(unsigned vbo, unsigned stride, unsigned ibo, unsigned isz, unsigned nidx, int base, unsigned tex, int vsid,
                        const float *k, unsigned nk, unsigned long long gh){
    (void)vbo; (void)stride; (void)ibo; (void)isz; (void)base; (void)tex; (void)vsid; (void)k; (void)nk; (void)gh;
    atomic_fetch_add(&g_tris, nidx / 3); return 1; }
/* no mirror pool: com.c falls back to unpacking indexed draws itself */
WEAK int  metal_mirror(uint32_t key, uint32_t gen, const void *src, uint32_t size, unsigned isz, uint32_t *off, uint32_t *imax){
    (void)key; (void)gen; (void)src; (void)size; (void)isz; (void)off; (void)imax; return 0; }
WEAK void metal_mirror_forget(uint32_t key){ (void)key; }
WEAK const uint8_t *metal_pool_cpu(void){ return NULL; }
WEAK void metal_raw_overflow(unsigned *rv, unsigned *kb){ if (rv) *rv = 0; if (kb) *kb = 0; }
WEAK void metal_register_ps(int id, const char *src){ (void)id; (void)src; }
WEAK void metal_ps_s0_2d(int id){ (void)id; }
WEAK void metal_register_vs(int id, const char *src){ (void)id; (void)src; }
WEAK int  metal_vs_ready(int id){ (void)id; return 1; }
WEAK void metal_blit(unsigned s, unsigned d){ (void)s; (void)d; }
WEAK void metal_blit_rect(unsigned s, unsigned d, const int *r){ (void)s; (void)d; (void)r; }
/* no GPU: the lock buffer keeps what it has */
WEAK int  metal_read_texture(unsigned id, void *dst, unsigned pitch, unsigned rows){ (void)id; (void)dst; (void)pitch; (void)rows; return 0; }
WEAK void metal_clear_target_ex(unsigned argb, unsigned flags, float z){ (void)argb; (void)flags; (void)z; }
WEAK void metal_clear_target(unsigned argb){ metal_clear_target_ex(argb, 1u, 1.f); }
WEAK void metal_set_blend(unsigned key, float aref){ (void)key; (void)aref; }
WEAK void metal_set_blendcolor(unsigned argb){ (void)argb; }
WEAK void metal_set_clear_color(unsigned argb){ (void)argb; }
WEAK void metal_set_cull(unsigned m){ (void)m; }
WEAK void metal_set_depth(unsigned m){ (void)m; }
WEAK void metal_set_ui_clip(float sy){ (void)sy; }
WEAK void metal_set_fog(unsigned e, unsigned argb){ (void)e; (void)argb; }
WEAK void metal_set_ps(int id, const float *c){ (void)id; (void)c; }
WEAK void metal_set_render_target(unsigned t){ (void)t; }
WEAK void metal_set_samplers(const unsigned k[4]){ (void)k; }
WEAK void metal_set_stage_tex(unsigned a, unsigned b, unsigned c3){ (void)a; (void)b; (void)c3; }
WEAK void metal_set_stage_hi(const unsigned t[4], const unsigned k[4]){ (void)t; (void)k; }
WEAK unsigned metal_create_texture_depth(unsigned w, unsigned h, unsigned f){ (void)w; (void)h; (void)f; return 0; }
WEAK int metal_tex_is_depth(unsigned id){ (void)id; return 0; }
WEAK void metal_set_depth_bias(unsigned b){ (void)b; }
WEAK void metal_set_bb_samples(unsigned n){ (void)n; }
WEAK void metal_set_texproj(const float p[4]){ (void)p; }
WEAK void metal_set_ui_aspect(float ax, float ay){ (void)ax; (void)ay; }
WEAK void metal_set_zsurf(unsigned z){ (void)z; }
WEAK void metal_z_forget(unsigned z){ (void)z; }
/* sizes: the page's canvas in device pixels (NFSU2_WIDTH/HEIGHT), else 1280x720 */
WEAK void metal_game_size(unsigned *w, unsigned *h){ if (w) *w = g_game_w; if (h) *h = g_game_h; }
WEAK void metal_set_game_size(unsigned w, unsigned h){ if (w && h && w <= 8192 && h <= 8192){ g_game_w = w; g_game_h = h; } }
WEAK void metal_want_size(unsigned w, unsigned h){ (void)w; (void)h; }
WEAK void metal_resize(unsigned w, unsigned h){ (void)w; (void)h; }
WEAK void metal_view_size(unsigned *ow, unsigned *oh){ *ow = (unsigned)env_u("NFSU2_WIDTH", 1280) & ~1u; *oh = (unsigned)env_u("NFSU2_HEIGHT", 720) & ~1u; }
WEAK void metal_native_size(unsigned gw, unsigned gh, unsigned *ow, unsigned *oh){ if (ow) *ow = gw; if (oh) *oh = gh; }
WEAK void metal_screen_metrics(double *backing, double *panel){ if (backing) *backing = 1.0; if (panel) *panel = 0.0; }
WEAK void metal_fill_mouse(unsigned char *dst, unsigned n){ memset(dst, 0, n); }
WEAK void metal_get_cursor(int *x, int *y){ if (x) *x = 0; if (y) *y = 0; }

/* --------------------------------------------------------------- audio */
/* The DirectSound buffers the game registers: no output, but the play
 * cursor advances in real time at the buffer's rate. */
#define NSB 64
static struct { uint32_t data, bytes, rate, align; int used, playing, loop; double t0, pos0; } g_sb[NSB];
static double g_audio_t0 = -1;
WEAK void audio_init(void){ if (g_audio_t0 < 0) g_audio_t0 = now_s(); }
WEAK double audio_time(void){ if (g_audio_t0 < 0) g_audio_t0 = now_s(); return now_s() - g_audio_t0; }
WEAK int audio_register(uint32_t data, uint32_t bytes, uint32_t rate, uint32_t chans, uint32_t bits){
    if (!data || !bytes) return 0;
    for (int i = 1; i < NSB; i++) if (!g_sb[i].used){
        g_sb[i].used = 1; g_sb[i].data = data; g_sb[i].bytes = bytes; g_sb[i].rate = rate ? rate : 44100;
        g_sb[i].align = (chans ? chans : 2) * ((bits ? bits : 16) / 8); g_sb[i].playing = 0; g_sb[i].pos0 = 0; return i; }
    return 0;
}
static double sb_pos(int id){
    if (!g_sb[id].playing) return g_sb[id].pos0;
    double p = g_sb[id].pos0 + (now_s() - g_sb[id].t0) * g_sb[id].rate * g_sb[id].align;
    if (g_sb[id].loop) return fmod(p, (double)g_sb[id].bytes);
    if (p >= g_sb[id].bytes){ g_sb[id].playing = 0; g_sb[id].pos0 = 0; return 0; }
    return p;
}
WEAK void audio_play(int id, int loop){ if (id > 0 && id < NSB && g_sb[id].used){ g_sb[id].pos0 = sb_pos(id); g_sb[id].t0 = now_s(); g_sb[id].loop = loop; g_sb[id].playing = 1; } }
WEAK void audio_stop(int id){ if (id > 0 && id < NSB && g_sb[id].used){ g_sb[id].pos0 = sb_pos(id); g_sb[id].playing = 0; } }
WEAK void audio_unregister(int id){ if (id > 0 && id < NSB) g_sb[id].used = 0; }
WEAK int  audio_duplicate(int src){ if (src <= 0 || src >= NSB || !g_sb[src].used) return 0;
    int id = audio_register(g_sb[src].data, g_sb[src].bytes, g_sb[src].rate, g_sb[src].align / 2, 16); return id; }
WEAK void audio_cursors(int id, uint32_t *play, uint32_t *write){
    uint32_t p = 0, w = 0;
    if (id > 0 && id < NSB && g_sb[id].used){
        p = (uint32_t)sb_pos(id); p -= p % g_sb[id].align;
        w = (p + g_sb[id].align * (g_sb[id].rate / 100)) % g_sb[id].bytes;   /* write cursor 10 ms ahead */
    }
    if (play) *play = p; if (write) *write = w;
}
WEAK void audio_set_freq(int id, uint32_t hz){ if (id > 0 && id < NSB && g_sb[id].used && hz){ g_sb[id].pos0 = sb_pos(id); g_sb[id].t0 = now_s(); g_sb[id].rate = hz; } }
WEAK void audio_set_volume(int id, long mb){ (void)id; (void)mb; }
WEAK void audio_set_muted(int on){ (void)on; }
WEAK void audio_note_write(int id, uint32_t end){ (void)id; (void)end; }
WEAK void audio_note_write_stat(int id, uint32_t n, int nz){ (void)id; (void)n; (void)nz; }
WEAK void audio_device_changed(const char *why){ (void)why; }
WEAK void audio_report(void){ }

/* ------------------------------------------------------------- gamepad */
static PadState g_synth;
WEAK int  gamepad_env_flag(const char *nm, int dflt){ const char *e = getenv(nm); if (!e || !*e) return dflt; return !(e[0] == '0' || !strcmp(e, "off") || !strcmp(e, "no")); }
WEAK void gamepad_init(void){ }
WEAK int  gamepad_read(PadState *s){ if (g_synth.connected){ *s = g_synth; return 1; } memset(s, 0, sizeof *s); return 0; }
WEAK void gamepad_synth_connect(int on){ g_synth.connected = !!on; g_synth.wheel = on == 2; g_synth.synthetic = 1; g_synth.family = PADF_XBOX; snprintf(g_synth.name, sizeof g_synth.name, "script pad"); }
WEAK void gamepad_synth_button(int b, int down){ if (b >= 0 && b < PAD_NBUTTONS){ if (down) g_synth.buttons |= 1u << b; else g_synth.buttons &= ~(1u << b); } }
WEAK void gamepad_synth_axis(int a, float v){ if (a >= 0 && a < PAD_NAXES) g_synth.axis[a] = v; }
WEAK int  gamepad_button_by_name(const char *nm){
    static const char *n[] = { "A","B","X","Y","LB","RB","BACK","START","LS","RS","HOME","DUP","DDOWN","DLEFT","DRIGHT" };
    for (int i = 0; i < PAD_NBUTTONS; i++) if (!strcasecmp(nm, n[i])) return i; return -1; }
WEAK int  gamepad_axis_by_name(const char *nm){
    static const char *n[] = { "LX","LY","RX","RY","LT","RT" };
    for (int i = 0; i < PAD_NAXES; i++) if (!strcasecmp(nm, n[i])) return i; return -1; }

/* ------------------------------------------------------------ keyboard */
WEAK const char *kbdmac_layout_name(unsigned dik){ (void)dik; return NULL; }
WEAK int  kbdmac_synth_char(int type, uint32_t u){ (void)type; (void)u; return 0; }
WEAK int  kbdmac_synth_key(int type, const char *key){ (void)type; (void)key; return 0; }
WEAK void kbdmac_synth_simple(int type, double a, double b){ (void)type; (void)a; (void)b; }

/* ------------------------------------------------- Discord, perf log */
WEAK void discord_lifecycle_changed(void){ }
WEAK void discord_on_present(void){ }
WEAK void discord_shutdown(void){ }
int g_perflog = 0;
WEAK void perflog_guest_frame(void){ }

/* ------------------------------------------------- texture packs */
/* texrep.m (optional upscaled-texture packs, "textures = upscaled") is
 * Metal/Objective-C.  The browser's texture packs (ui_textures) are
 * texrep_web.c's; without it every guest-side hook is inert, as texrep.m is
 * with the option off.
 * The frame-rate counter (fpscounter.m) needs no stand-in: nothing outside
 * metal.m calls it, and the page draws its own (web/src/app overlay). */
WEAK void texrep_created(unsigned mid, unsigned w, unsigned h, unsigned levels, unsigned d3dfmt, unsigned usage, unsigned pool){
    (void)mid; (void)w; (void)h; (void)levels; (void)d3dfmt; (void)usage; (void)pool; }
WEAK void texrep_uploaded(unsigned mid, unsigned level, const uint8_t *data, unsigned pitch, unsigned rows){
    (void)mid; (void)level; (void)data; (void)pitch; (void)rows; }
WEAK void texrep_surface_written(unsigned mid){ (void)mid; }
WEAK void texrep_released(unsigned mid){ (void)mid; }
WEAK void texrep_frame(void){ }
WEAK void texrep_report(void){ }
WEAK int  texrep_wanted(void){ return 0; }
WEAK void texrep_set_shown(int on){ (void)on; }
WEAK int  texrep_shown(void){ return 0; }
WEAK int  texrep_status(char *buf, size_t n){ (void)buf; (void)n; return 0; }

/* ------------------------------------------------ free-roam transport */
WEAK int  roam_plat_start(const roam_plat_cfg *cfg, rc_session *s){ (void)cfg; (void)s; fprintf(stderr, "[roam] no transport in this build\n"); return -1; }
WEAK void roam_plat_stop(uint8_t reason){ (void)reason; }
WEAK void roam_plat_lock(void){ }
WEAK void roam_plat_unlock(void){ }
WEAK int64_t roam_plat_now_us(void){ return (int64_t)(now_s() * 1e6); }
WEAK uint32_t roam_plat_random(void){ uint32_t r; arc4random_buf(&r, sizeof r); return r; }
WEAK int  roam_plat_test_mode(void){ return 0; }
WEAK void roam_plat_get_stats(roam_plat_stats *o){ memset(o, 0, sizeof *o); }
WEAK int  roam_plat_parse_peers(const char *list, int tm, uint16_t defport, uint32_t *ip, uint16_t *port, int max){
    (void)list; (void)tm; (void)defport; (void)ip; (void)port; (void)max; return 0; }
WEAK const char *roam_ip_str(uint32_t ip, char *b){ sprintf(b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }

/* ------------------------------------------------------------- dlsym */
WEAK void *dlsym(void *h, const char *n){ (void)h; (void)n; return NULL; }
