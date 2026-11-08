/* texrep.m -- optional texture replacement packs and the texture dump
 * (packs are built by tools/texpack).
 *
 * OFF BY DEFAULT.  Nothing below runs unless one of these is set; with
 * none of them every guest hook returns at its first line and the draw
 * encoder binds the game's own textures only (texrep_pick).
 *
 *   NFSU2_TEXTURES = original | upscaled     settings.ini "textures"
 *       upscaled: load the pack.  Unset + NFSU2_GRAPHICS=enhanced = upscaled.
 *       NFSU2_GRAPHICS=original: never (the game's own look).
 *       No pack found (or an empty one) = the same as original.
 *   NFSU2_TEXPACK  = <name> | <path>         settings.ini "texture_pack"
 *       a folder under <Application Support>/NFSU2/Textures/ (default
 *       "default"), or an absolute path
 *   NFSU2_TEXPARTS = list | all              settings.ini "texture_parts"
 *       pack categories to use (the pack's sub-folders): world, signs, cars,
 *       garage, sky, effects, frontend, hud, other.  Default world,signs,cars,
 *       garage (sky, effects, menus and HUD showed no gain or added grain)
 *   NFSU2_TEXDUMP  = on | <path>             settings.ini "texture_dump"
 *       write level 0 of every static texture the game uploads (once per
 *       content) as <dir>/<category>/<w>x<h>_<fmt>_<hash>.dds + index.csv:
 *       input for tools/texpack for what the offline scan cannot read
 *       (compressed front-end and car bundles)
 *   NFSU2_TEXBUDGET = <MB>                   settings.ini "texture_budget"
 *       GPU memory for replacements, loading ones included; beyond it the
 *       game's own textures stay (default: a sixth of the Mac's memory, at
 *       most 4096)
 *   NFSU2_TEXLOG = 1                         one line per replacement (debug)
 *
 * Pack layout (tools/texpack writes it): <pack>/<category>/<anything>_<hash>.dds,
 * where <hash> = 16 hex digits = XXH3-64 of the texture's level-0 bytes as
 * the game uploads them, seeded with w | h << 16 | D3DFORMAT << 32 (the
 * same value tools/texpack computes from the game files).  A file whose
 * levels stop above the original's size, in the original's block format
 * (DXT1 -> BC1, DXT3 -> BC2, DXT5 -> BC3, A8R8G8B8 -> BGRA8) and with a
 * size of w<<k x h<<k, is a STACKED replacement: the game's own levels go
 * underneath, unchanged, so wherever the game drew a texture at or below
 * its own resolution the GPU reads the game's own texels.  Any other DDS of
 * the same aspect ratio (BC1/2/3/7, BGRA8, RGBA8; a full mip chain or one
 * level) replaces the texture as a whole (hand-made replacements).
 *
 * The game never sees a different size: GetLevelDesc, LockRect and every
 * upload keep using the original Metal texture.  Only the sampling bind in
 * the draw encoder (metal.m, texrep_pick) picks the replacement.  Render
 * targets, cube maps, dynamic textures (content changed after the first
 * upload, or written through a surface: the car skin composite, FMV
 * frames), textures of 32x32 texels or less and movies are never replaced.
 *
 * Sharing.  The game creates new texture ids for the same textures on every
 * race load; ids with the same content (level 0 and the lower levels) show
 * one loaded replacement (Ent), so memory stays at the pack's unique
 * content however many races are loaded.
 *
 * Threads.  Guest thread: hashing at UnlockRect (level 0: the pack key; the
 * lower levels: who may share), lookup, per-texture state, the Ents and
 * the memory budget, publication (texrep_frame).  Loaders (three GCD
 * queues): file read, DDS parse, Metal texture creation, the pack's
 * levels.  Main thread
 * (texrep_main_frame, start of each frame's encode): the replacement table
 * the encoder reads, and a blit of the game's own levels under the pack's.
 * The loader never touches guest state and the guest never touches the
 * main thread's table: they hand over through two mutex-protected lists.
 *
 * xxhash.h: XXH3 by Yann Collet, BSD 2-Clause (vendored, v0.8.4). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Cocoa/Cocoa.h>
#include "cpu.h"
#include "game.h"
#include "texrep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#define XXH_INLINE_ALL
#include "xxhash.h"

id<MTLDevice> metal_device(void);                  /* metal.m */
id<MTLTexture> metal_tex_object(unsigned id_);     /* metal.m, main thread */
const char *fileio_last_read(void);                /* fileio.c */

/* ---- main-thread state the encoder reads -------------------------------- */
int g_texrep_frame_on;
__strong id<MTLTexture> g_texrep_tex[TEXREP_MAXTEX];
uint8_t g_texrep_k[TEXREP_MAXTEX];
static unsigned g_main_live;                       /* entries in g_texrep_tex */
static _Atomic int g_shown = 1;                    /* menu item / autotest "texrep" */
static _Atomic int g_pack_live;                    /* a pack is indexed (g_active), for the main thread */
static NSMenuItem *g_menu_item;                    /* main thread: the hidden menu item, until g_pack_live */

/* ---- configuration (guest thread, first hook call) ---------------------- */
static int g_init, g_active, g_dumping;            /* g_active: a pack is indexed */
static char g_packdir[1024], g_dumpdir[1024];
static unsigned g_parts;                           /* category bit mask */
static int g_verbose;
static unsigned long long g_budget;
static dispatch_queue_t g_q;                       /* dump writer */
#define NLQ 3
static dispatch_queue_t g_lq[NLQ];                 /* loaders (round robin) */
static unsigned g_lq_next;

enum { CAT_WORLD, CAT_SIGNS, CAT_CARS, CAT_GARAGE, CAT_SKY, CAT_EFFECTS, CAT_FRONTEND, CAT_HUD, CAT_OTHER, NCAT };
static const char *const k_cat[NCAT] = { "world", "signs", "cars", "garage", "sky", "effects", "frontend", "hud", "other" };
#define PARTS_DEFAULT ((1u << CAT_WORLD) | (1u << CAT_SIGNS) | (1u << CAT_CARS) | (1u << CAT_GARAGE))

/* ---- pack index: hash -> path ------------------------------------------- */
typedef struct { uint64_t h; uint32_t path; int32_t ent; uint8_t cat; } IdxEnt;   /* ent: its shared Ent, or -1 */
static IdxEnt *g_idx; static unsigned g_idx_cap, g_idx_n;
static char *g_paths; static size_t g_paths_n, g_paths_cap;
static unsigned g_idx_cat[NCAT], g_idx_skipped;
static uint32_t path_put(const char *p){
    size_t l = strlen(p) + 1;
    if (g_paths_n + l > g_paths_cap){
        g_paths_cap = g_paths_cap ? g_paths_cap * 2 : 1u << 20;
        while (g_paths_n + l > g_paths_cap) g_paths_cap *= 2;
        g_paths = realloc(g_paths, g_paths_cap);
    }
    memcpy(g_paths + g_paths_n, p, l);
    uint32_t off = (uint32_t)g_paths_n; g_paths_n += l;
    return off;
}
static void idx_grow(void);
static void idx_put(uint64_t h, const char *path, int cat){
    if (!h) h = 1;
    if (2 * (g_idx_n + 1) > g_idx_cap) idx_grow();
    for (unsigned i = (unsigned)(h & (g_idx_cap - 1)); ; i = (i + 1) & (g_idx_cap - 1)){
        if (!g_idx[i].h){ g_idx[i].h = h; g_idx[i].path = path_put(path); g_idx[i].cat = (uint8_t)cat; g_idx[i].ent = -1;
                          g_idx_n++; g_idx_cat[cat]++; return; }
        if (g_idx[i].h == h) return;               /* first one wins */
    }
}
static void idx_grow(void){
    unsigned oc = g_idx_cap; IdxEnt *o = g_idx;
    g_idx_cap = oc ? oc * 2 : 1u << 12;
    g_idx = calloc(g_idx_cap, sizeof *g_idx);
    for (unsigned i = 0; i < oc; i++) if (o[i].h){
        for (unsigned j = (unsigned)(o[i].h & (g_idx_cap - 1)); ; j = (j + 1) & (g_idx_cap - 1))
            if (!g_idx[j].h){ g_idx[j] = o[i]; break; }
    }
    free(o);
}
static IdxEnt *idx_get(uint64_t h){
    if (!g_idx_n) return 0;
    if (!h) h = 1;
    for (unsigned i = (unsigned)(h & (g_idx_cap - 1)); g_idx[i].h; i = (i + 1) & (g_idx_cap - 1))
        if (g_idx[i].h == h) return &g_idx[i];
    return 0;
}
/* file name ends in _<16 hex digits>.dds */
static int name_hash(const char *nm, uint64_t *h){
    size_t l = strlen(nm);
    if (l < 21 || strcasecmp(nm + l - 4, ".dds") || nm[l - 21] != '_') return 0;
    uint64_t v = 0;
    for (const char *p = nm + l - 20; p < nm + l - 4; p++){
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v << 4 | (uint64_t)d;
    }
    *h = v; return 1;
}
static void idx_scan_dir(const char *dir, int cat, int depth){
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e; char p[1400];
    while ((e = readdir(d))){
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        uint64_t h;
        if (name_hash(e->d_name, &h)){
            if ((g_parts >> cat) & 1u) idx_put(h, p, cat); else g_idx_skipped++;
        } else if (depth < 3 && e->d_type == DT_DIR) idx_scan_dir(p, cat, depth + 1);
        else if (depth < 3 && e->d_type == DT_UNKNOWN){
            struct stat st; if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) idx_scan_dir(p, cat, depth + 1); }
    }
    closedir(d);
}
static int cat_by_name(const char *s){
    for (int c = 0; c < NCAT; c++) if (!strcasecmp(s, k_cat[c])) return c;
    return -1;
}
/* category = the first folder below the pack; files directly in the pack
 * (or in an unknown folder) are "other" */
static void idx_scan(const char *pack){
    DIR *d = opendir(pack); if (!d) return;
    struct dirent *e; char p[1400];
    while ((e = readdir(d))){
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", pack, e->d_name);
        uint64_t h;
        if (name_hash(e->d_name, &h)){
            if ((g_parts >> CAT_OTHER) & 1u) idx_put(h, p, CAT_OTHER); else g_idx_skipped++;
            continue; }
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        int c = cat_by_name(e->d_name);
        idx_scan_dir(p, c < 0 ? CAT_OTHER : c, 1);
    }
    closedir(d);
}

static double now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static const char *support_dir(char *buf, size_t n){
    const char *as = getenv("NFSU2_APPSUPPORT"), *home = getenv("HOME");
    if (as && *as) snprintf(buf, n, "%s", as);
    else snprintf(buf, n, "%s/Library/Application Support/NFSU2", home ? home : "/tmp");
    return buf;
}
/* "name" -> <support>/Textures/name; absolute (or ./relative) paths as given */
static void resolve_dir(const char *v, const char *dflt, char *out, size_t n){
    char sup[1024];
    if (!v || !*v || !strcasecmp(v, "on") || !strcmp(v, "1")) v = dflt;
    if (v[0] == '/' || v[0] == '.' || v[0] == '~'){
        if (v[0] == '~' && getenv("HOME")) snprintf(out, n, "%s%s", getenv("HOME"), v + 1);
        else snprintf(out, n, "%s", v);
    } else snprintf(out, n, "%s/Textures/%s", support_dir(sup, sizeof sup), v);
    size_t l = strlen(out); while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}
static int env_on(const char *k){ const char *v = getenv(k); return v && *v && strcasecmp(v, "off") && strcmp(v, "0"); }

/* graphics = original is the game's own look whatever the other lines say
 * (settings.ini template, graphics-enhancements.md §2): a pack too */
int texrep_wanted(void){
    const char *t = getenv("NFSU2_TEXTURES"), *g = getenv("NFSU2_GRAPHICS");
    int up = t && *t ? (!strcasecmp(t, "upscaled") || !strcasecmp(t, "on") || !strcmp(t, "1"))
                     : (g && !strcasecmp(g, "enhanced"));
    if (up && g && !strcasecmp(g, "original")){
        static int said;
        if (!said++) fprintf(stderr, "[texrep] graphics = original: textures = upscaled ignored "
                                     "(graphics = custom or enhanced uses it)\n");
        return 0;
    }
    return up;
}

/* ---- per-texture state (guest thread only) ------------------------------ */
enum { S_NONE, S_PENDING, S_ACTIVE };
typedef struct {
    uint16_t w, h; uint8_t lev, ok, dyn, state, cat, k;
    uint8_t dumped;                                /* a dump file was written for hash */
    uint32_t fmt, gen, lvmask;
    uint64_t hash, lvh;                            /* lvh: the lower levels' content (sum of hashes) */
    int32_t ent;                                   /* the Ent it waits for (S_PENDING) or shows (S_ACTIVE) */
    long req_frame, last_up;                       /* g_frame of the pack hit / of the last level upload */
} TR;
static TR *g_tr;
static long g_frame;                               /* texrep_frame calls */

/* ---- replacements (guest thread only) --------------------------------------
 * One Ent per loaded pack file, shared by every texture id with the same
 * content: the game creates a new texture id for the same texture on every
 * race load, and without sharing each id would load its own copy (about
 * 0.3 GB more per race load at 4x).  An id shares an Ent only when its own
 * lower levels are the ones under the Ent's pack levels (same level count,
 * same uploaded levels, same content: lvh), so what a minified surface
 * shows stays the game's own texels.  Memory is counted per Ent, from the
 * moment its load starts (g_bytes), which is what texture_budget limits. */
enum { E_LOADING, E_READY, E_FAILED, E_BUDGET };
typedef struct {
    int32_t idx;                                   /* g_idx slot it is linked from (shareable), or -1 */
    uint32_t path;                                 /* pack file (g_paths offset) */
    uint8_t st, k, stacked, solo;                  /* solo: its lower levels changed after publication */
    uint8_t lev; uint32_t lvmask; uint64_t lvh;    /* the lower levels under it (set by the first id shown) */
    unsigned refs, waiters;                        /* ids showing it / ids waiting for it */
    void *tex;                                     /* +1 while E_READY */
    unsigned long long bytes;                      /* counted in g_bytes (E_BUDGET: what it needed) */
} Ent;
static Ent *g_ent; static unsigned g_ent_n, g_ent_cap; static int32_t g_ent_free = -1;
typedef struct { unsigned mid, gen; } Pend;        /* ids waiting for an Ent */
static Pend *g_pend; static unsigned g_pend_n, g_pend_cap;

/* ---- loader -> guest ------------------------------------------------------ */
typedef struct { int32_t ei; unsigned k, stacked, fail; void *tex; unsigned long long bytes; double ms; } Ready;
static pthread_mutex_t g_rmx = PTHREAD_MUTEX_INITIALIZER;
static Ready *g_ready; static unsigned g_ready_n, g_ready_cap;
/* ---- guest -> main ---------------------------------------------------------- */
enum { OP_SET, OP_CLEAR, OP_COPY };
/* OP_SET: tex = a +1 reference for this id; stacked = copy this id's own
 * levels under the pack's (the first id shown of an Ent; the others share them) */
typedef struct { int op; unsigned mid, k, level, stacked; void *tex; } Op;
static pthread_mutex_t g_omx = PTHREAD_MUTEX_INITIALIZER;
static Op *g_ops; static unsigned g_ops_n, g_ops_cap;
static void op_post(Op o){
    pthread_mutex_lock(&g_omx);
    if (g_ops_n == g_ops_cap){ g_ops_cap = g_ops_cap ? 2 * g_ops_cap : 256;
                               g_ops = realloc(g_ops, g_ops_cap * sizeof *g_ops); }
    g_ops[g_ops_n++] = o;
    pthread_mutex_unlock(&g_omx);
}

/* ---- statistics ----------------------------------------------------------- */
static unsigned long g_n_created, g_n_eligible, g_n_hashed, g_n_hit, g_n_loaded, g_n_pub, g_n_drop,
                     g_n_fail, g_n_shape, g_n_budget, g_n_stale, g_n_dyn, g_n_dumped, g_n_unlinked,
                     g_n_shared, g_n_private, g_n_unshare, g_n_relevel, g_n_tex_peak, g_n_tex_live;
static unsigned long g_why[3];                                    /* published: all levels / uploads stopped / 60 frames */
static _Atomic unsigned long long g_bytes;                        /* every live Ent, loading ones included */
static unsigned long long g_bytes_peak;
static double g_hash_ms, g_load_ms_sum, g_load_ms_max;           /* load: loader thread */
static unsigned long long g_hash_bytes;
static double g_hash_ms_mark, g_frame_ms_max; static unsigned long g_frames_over1;   /* guest time in texrep per frame */
static unsigned long g_lat_hist[8];                               /* publish latency in frames: 0,1,2,3-4,5-8,9-16,17-64,>64 */
static long g_lat_max;
static unsigned long g_pub_race, g_load_race;                     /* while GameFlow = 6 (race / free roam) */
static unsigned long g_mipstat[4];                                /* main thread: replaced binds by mip filter */
static int in_race(void){ return mem_is_committed(G_FLOW) && MR32(G_FLOW) == 6u; }

static unsigned long long phys_mem(void){
    uint64_t m = 0; size_t l = sizeof m;
    if (sysctlbyname("hw.memsize", &m, &l, NULL, 0) != 0) m = 8ull << 30;
    return m;
}

static void tr_init(void){
    if (g_init) return;
    g_init = 1;
    g_verbose = env_on("NFSU2_TEXLOG");
    const char *dv = getenv("NFSU2_TEXDUMP");
    if (dv && *dv && strcasecmp(dv, "off") && strcmp(dv, "0")){
        resolve_dir(dv, "dump", g_dumpdir, sizeof g_dumpdir);
        g_dumping = 1;
    }
    int want = texrep_wanted();
    if (want){
        resolve_dir(getenv("NFSU2_TEXPACK"), "default", g_packdir, sizeof g_packdir);
        const char *pv = getenv("NFSU2_TEXPARTS");
        if (!pv || !*pv) g_parts = PARTS_DEFAULT;
        else if (!strcasecmp(pv, "all")) g_parts = (1u << NCAT) - 1u;
        else {
            char tmp[256]; snprintf(tmp, sizeof tmp, "%s", pv); char *sv = 0;
            for (char *t = strtok_r(tmp, ", ", &sv); t; t = strtok_r(0, ", ", &sv)){
                int c = cat_by_name(t);
                if (c >= 0) g_parts |= 1u << c;
                else fprintf(stderr, "[texrep] texture_parts: unknown part \"%s\" ignored\n", t);
            }
        }
        const char *bv = getenv("NFSU2_TEXBUDGET");
        unsigned long long auto_b = phys_mem() / 6; if (auto_b > (4096ull << 20)) auto_b = 4096ull << 20;
        g_budget = (bv && atof(bv) > 0) ? (unsigned long long)(atof(bv) * 1048576.0) : auto_b;
        double t0 = now_ms();
        struct stat st;
        if (stat(g_packdir, &st) != 0 || !S_ISDIR(st.st_mode))
            fprintf(stderr, "[texrep] textures = upscaled, but there is no pack at %s: the game's own textures stay\n", g_packdir);
        else {
            idx_scan(g_packdir);
            fprintf(stderr, "[texrep] pack %s: %u replacement(s) indexed in %.1f ms (", g_packdir, g_idx_n, now_ms() - t0);
            for (int c = 0; c < NCAT; c++) if (g_idx_cat[c]) fprintf(stderr, " %s %u", k_cat[c], g_idx_cat[c]);
            fprintf(stderr, " ; %u in parts not enabled), GPU memory budget %.0f MB\n", g_idx_skipped, g_budget / 1048576.0);
            if (!g_idx_n) fprintf(stderr, "[texrep] nothing to replace: the game's own textures stay\n");
        }
        g_active = g_idx_n > 0;
        atomic_store(&g_pack_live, g_active);
    }
    if (g_active || g_dumping){
        g_tr = calloc(TEXREP_MAXTEX, sizeof *g_tr);
        dispatch_queue_attr_t at = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
        g_q = dispatch_queue_create("nfsu2.texrep.dump", at);
        for (int i = 0; i < NLQ; i++) g_lq[i] = dispatch_queue_create("nfsu2.texrep.load", at);
    }
    if (g_dumping){
        char p[1200];
        snprintf(p, sizeof p, "%s", g_dumpdir);
        for (char *q = p + 1; *q; q++) if (*q == '/'){ *q = 0; mkdir(p, 0755); *q = '/'; }
        mkdir(p, 0755);
        fprintf(stderr, "[texrep] dumping static textures to %s\n", g_dumpdir);
    }
    if (!g_active && !g_dumping) g_init = 2;        /* inert */
}

/* ---- formats ---------------------------------------------------------------- */
#define FMT_DXT1 0x31545844u
#define FMT_DXT3 0x33545844u
#define FMT_DXT5 0x35545844u
static MTLPixelFormat d3d_pf(uint32_t f){
    switch (f){
    case FMT_DXT1: return MTLPixelFormatBC1_RGBA;
    case FMT_DXT3: return MTLPixelFormatBC2_RGBA;
    case FMT_DXT5: return MTLPixelFormatBC3_RGBA;
    case 21: case 22: return MTLPixelFormatBGRA8Unorm;
    }
    return MTLPixelFormatInvalid;
}
static const char *fmt_name(uint32_t f){
    switch (f){ case FMT_DXT1: return "DXT1"; case FMT_DXT3: return "DXT3"; case FMT_DXT5: return "DXT5";
                case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; }
    return "?";
}
/* bytes per row / rows of a level */
static void pf_level(MTLPixelFormat pf, unsigned w, unsigned h, size_t *row, size_t *rows){
    switch (pf){
    case MTLPixelFormatBC1_RGBA: *row = (size_t)((w + 3) / 4) * 8; *rows = (h + 3) / 4; return;
    case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC7_RGBAUnorm:
        *row = (size_t)((w + 3) / 4) * 16; *rows = (h + 3) / 4; return;
    default: *row = (size_t)w * 4; *rows = h; return;
    }
}
static unsigned full_levels(unsigned w, unsigned h){
    unsigned n = 1; for (unsigned q = w > h ? w : h; q > 1; q >>= 1) n++;
    return n;
}

/* ---- DDS -------------------------------------------------------------------- */
typedef struct { unsigned w, h, levels; MTLPixelFormat pf; size_t off; } Dds;
static uint32_t rd32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return v; }
static int dds_parse(const uint8_t *b, size_t n, Dds *d){
    if (n < 128 || memcmp(b, "DDS ", 4) || rd32(b + 4) != 124) return 0;
    d->h = rd32(b + 12); d->w = rd32(b + 16);
    d->levels = rd32(b + 28); if (!d->levels) d->levels = 1;
    if (rd32(b + 112) & 0x200u) return 0;                    /* cube map */
    uint32_t pff = rd32(b + 80), four = rd32(b + 84), bits = rd32(b + 88), rm = rd32(b + 92), am = rd32(b + 104);
    d->off = 128; d->pf = MTLPixelFormatInvalid;
    if (pff & 4u){
        if (four == FMT_DXT1) d->pf = MTLPixelFormatBC1_RGBA;
        else if (four == FMT_DXT3 || four == 0x32545844u) d->pf = MTLPixelFormatBC2_RGBA;
        else if (four == FMT_DXT5 || four == 0x34545844u) d->pf = MTLPixelFormatBC3_RGBA;
        else if (four == 0x30315844u && n >= 148){            /* "DX10" */
            uint32_t dx = rd32(b + 128); d->off = 148;
            if (rd32(b + 132) != 3u) return 0;                /* TEXTURE2D only */
            switch (dx){                                      /* the game is not sRGB-aware: _SRGB = UNORM */
            case 71: case 72: d->pf = MTLPixelFormatBC1_RGBA; break;
            case 74: case 75: d->pf = MTLPixelFormatBC2_RGBA; break;
            case 77: case 78: d->pf = MTLPixelFormatBC3_RGBA; break;
            case 98: case 99: d->pf = MTLPixelFormatBC7_RGBAUnorm; break;
            case 87: case 91: d->pf = MTLPixelFormatBGRA8Unorm; break;
            case 28: case 29: d->pf = MTLPixelFormatRGBA8Unorm; break;
            }
        }
    } else if ((pff & 0x40u) && bits == 32){
        d->pf = rm == 0x00ff0000u ? MTLPixelFormatBGRA8Unorm : rm == 0x000000ffu ? MTLPixelFormatRGBA8Unorm
              : MTLPixelFormatInvalid;
        (void)am;
    }
    if (d->pf == MTLPixelFormatInvalid || !d->w || !d->h || d->w > 16384 || d->h > 16384) return 0;
    unsigned fl = full_levels(d->w, d->h);
    if (d->levels > fl) d->levels = fl;
    size_t need = 0;
    for (unsigned l = 0; l < d->levels; l++){
        unsigned lw = d->w >> l, lh = d->h >> l; if (!lw) lw = 1; if (!lh) lh = 1;
        size_t row, rows; pf_level(d->pf, lw, lh, &row, &rows); need += row * rows;
    }
    return d->off + need <= n;
}

/* ---- loader (GCD queue) ----------------------------------------------------- */
static void ready_post(Ready r){
    pthread_mutex_lock(&g_rmx);
    if (g_ready_n == g_ready_cap){ g_ready_cap = g_ready_cap ? 2 * g_ready_cap : 256;
                                   g_ready = realloc(g_ready, g_ready_cap * sizeof *g_ready); }
    g_ready[g_ready_n++] = r;
    pthread_mutex_unlock(&g_rmx);
}
static uint8_t *read_file(const char *path, size_t *n){
    int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    struct stat st; if (fstat(fd, &st) != 0 || st.st_size < 128 || st.st_size > (1ll << 31)){ close(fd); return 0; }
    uint8_t *b = malloc((size_t)st.st_size);
    size_t got = 0;
    while (b && got < (size_t)st.st_size){
        ssize_t k = read(fd, b + got, (size_t)st.st_size - got);
        if (k <= 0) break;
        got += (size_t)k;
    }
    close(fd);
    if (!b || got != (size_t)st.st_size){ free(b); return 0; }
    *n = got; return b;
}
/* Ready.fail: 0 = loaded, 1: shape / format, 2: read / parse, 3: over the
 * budget (bytes = what it needed).  The bytes of a loaded texture stay
 * counted in g_bytes until its Ent is freed (texrep_frame). */
static void load_job(int32_t ei, const char *path, unsigned w, unsigned h, unsigned olev, uint32_t fmt){
    double t0 = now_ms();
    Ready r = { ei, 0, 0, 2, NULL, 0, 0 };
    size_t n = 0; uint8_t *b = read_file(path, &n);
    Dds d;
    if (!b || !dds_parse(b, n, &d)){ free(b); r.ms = now_ms() - t0; ready_post(r); return; }
    r.fail = 1;
    /* same aspect ratio, not smaller, at most 16x (the sampler's LOD clamp has 3 bits) */
    if (d.w < w || d.h < h || (unsigned long long)d.w * h != (unsigned long long)d.h * w || d.w > (w << 4)){
        free(b); r.ms = now_ms() - t0; ready_post(r); return; }
    unsigned k = 0; while ((w << k) < d.w && k < 4) k++;
    int pow2 = (w << k) == d.w && (h << k) == d.h;
    int stacked = pow2 && k >= 1 && d.levels == k && d.pf == d3d_pf(fmt);
    unsigned tot = stacked ? k + olev : d.levels;
    unsigned fl = full_levels(d.w, d.h); if (tot > fl) tot = fl;
    /* sampler clamp (texrep_skey_slow) needs log2(scale) of a power-of-two scale */
    unsigned kk = pow2 ? k : 0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:d.pf
                                   width:d.w height:d.h mipmapped:(tot > 1)];
    td.mipmapLevelCount = tot;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLDevice> dev = metal_device();
    MTLSizeAndAlign sa = dev ? [dev heapTextureSizeAndAlignWithDescriptor:td] : (MTLSizeAndAlign){0, 0};
    unsigned long long want = sa.size;
    /* reserve first: three loaders at once can never pass the budget together */
    if (atomic_fetch_add(&g_bytes, want) + want > g_budget){
        atomic_fetch_sub(&g_bytes, want);
        free(b); r.fail = 3; r.bytes = want; r.ms = now_ms() - t0; ready_post(r); return; }
    id<MTLTexture> t = dev ? [dev newTextureWithDescriptor:td] : nil;
    if (!t){ atomic_fetch_sub(&g_bytes, want); free(b); r.fail = 2; r.ms = now_ms() - t0; ready_post(r); return; }
    unsigned long long got = t.allocatedSize;      /* the reservation becomes the real size */
    if (got > want) atomic_fetch_add(&g_bytes, got - want); else atomic_fetch_sub(&g_bytes, want - got);
    size_t off = d.off;
    for (unsigned l = 0; l < d.levels && l < tot; l++){
        unsigned lw = d.w >> l, lh = d.h >> l; if (!lw) lw = 1; if (!lh) lh = 1;
        size_t row, rows; pf_level(d.pf, lw, lh, &row, &rows);
        [t replaceRegion:MTLRegionMake2D(0, 0, lw, lh) mipmapLevel:l withBytes:b + off bytesPerRow:row];
        off += row * rows;
    }
    free(b);
    r.fail = 0; r.k = kk; r.stacked = (unsigned)stacked; r.bytes = got;
    r.tex = (__bridge_retained void *)t;
    r.ms = now_ms() - t0;
    ready_post(r);
}

/* ---- dump (GCD queue) ---------------------------------------------------------- */
static int cat_of_src(const char *s){
    if (!s) return CAT_OTHER;
    const char *p = s;
    if (strcasestr(p, "DYNTEX")) return CAT_SKY;              /* LOC4DYNTEX.BIN: clouds */
    if (!strncasecmp(p, "CARS/", 5)) return CAT_CARS;
    if (!strncasecmp(p, "TRACKS/", 7) || !strncasecmp(p, "NIS/", 4)) return CAT_WORLD;
    if (!strncasecmp(p, "FRONTEND/PLATFORMS/", 19)) return CAT_GARAGE;
    if (!strncasecmp(p, "FRONTEND/", 9) || !strncasecmp(p, "LANGUAGES/", 10)) return CAT_FRONTEND;
    if (!strncasecmp(p, "GLOBAL/InGame", 13) || !strncasecmp(p, "GLOBAL/HUD", 10)) return CAT_HUD;
    if (!strncasecmp(p, "GLOBAL/", 7)) return CAT_FRONTEND;
    return CAT_OTHER;
}
/* the file the guest read last, relative to the game folder */
static void src_rel(char *out, size_t n){
    const char *lr = fileio_last_read();
    out[0] = 0; if (!lr) return;
    static const char *const tops[] = { "/CARS/", "/TRACKS/", "/NIS/", "/FRONTEND/", "/GLOBAL/", "/LANGUAGES/", "/MOVIES/" };
    const char *best = 0;
    for (unsigned i = 0; i < sizeof tops / sizeof tops[0]; i++){
        const char *p = strcasestr(lr, tops[i]); if (p && (!best || p < best)) best = p; }
    snprintf(out, n, "%s", best ? best + 1 : lr);
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
}
static void dump_write(TR *t, unsigned mid, const uint8_t *data, size_t len, unsigned pitch){
    (void)mid;
    uint8_t *copy = malloc(len); if (!copy) return;
    memcpy(copy, data, len);
    unsigned w = t->w, h = t->h, lev = t->lev; uint32_t fmt = t->fmt; uint64_t hh = t->hash; int cat = t->cat;
    char src[256]; src_rel(src, sizeof src);
    char *srcd = strdup(src);
    g_n_dumped++;
    dispatch_async(g_q, ^{
        char dir[1200], p[1400];
        snprintf(dir, sizeof dir, "%s/%s", g_dumpdir, k_cat[cat]); mkdir(dir, 0755);
        snprintf(p, sizeof p, "%s/%ux%u_%s_%016llx.dds", dir, w, h, fmt_name(fmt), (unsigned long long)hh);
        struct stat st;
        if (stat(p, &st) != 0){
            uint8_t hd[128]; memset(hd, 0, sizeof hd);
            memcpy(hd, "DDS ", 4);
            uint32_t v[32]; memset(v, 0, sizeof v);
            int bc = fmt == FMT_DXT1 || fmt == FMT_DXT3 || fmt == FMT_DXT5;
            v[1] = 124; v[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (bc ? 0x80000u : 0x8u);
            v[3] = h; v[4] = w; v[5] = bc ? (uint32_t)len : pitch; v[7] = 1;
            v[19] = 32;
            if (bc){ v[20] = 0x4; v[21] = fmt; }
            else { v[20] = fmt == 21 ? 0x41u : 0x40u; v[22] = 32; v[23] = 0x00ff0000u; v[24] = 0x0000ff00u;
                   v[25] = 0x000000ffu; v[26] = fmt == 21 ? 0xff000000u : 0u; }
            v[27] = 0x1000;
            memcpy(hd + 4, v + 1, 124);
            char tmp[1500]; snprintf(tmp, sizeof tmp, "%s.tmp", p);
            FILE *f = fopen(tmp, "wb");
            if (f){ fwrite(hd, 1, 128, f); fwrite(copy, 1, len, f);
                    if (fclose(f) == 0) rename(tmp, p); else unlink(tmp); }
            snprintf(p, sizeof p, "%s/index.csv", g_dumpdir);
            int fresh = stat(p, &st) != 0;
            FILE *ix = fopen(p, "a");
            if (ix){ if (fresh) fprintf(ix, "hash,w,h,fmt,levels,category,source\n");
                     fprintf(ix, "%016llx,%u,%u,%s,%u,%s,%s\n", (unsigned long long)hh, w, h, fmt_name(fmt), lev,
                             k_cat[cat], srcd); fclose(ix); }
        }
        free(copy); free(srcd);
    });
}
/* a dumped texture turned out to be dynamic: take the file back */
static void dump_unlink(TR *t, uint64_t hh){
    unsigned w = t->w, h = t->h; uint32_t fmt = t->fmt; int cat = t->cat;
    g_n_unlinked++;
    dispatch_async(g_q, ^{
        char p[1400];
        snprintf(p, sizeof p, "%s/%s/%ux%u_%s_%016llx.dds", g_dumpdir, k_cat[cat], w, h, fmt_name(fmt), (unsigned long long)hh);
        unlink(p);
    });
}

/* ---- replacements: Ent bookkeeping (guest thread) ------------------------------ */
static int32_t ent_new(int32_t idx, uint32_t path){
    int32_t ei;
    if (g_ent_free >= 0){ ei = g_ent_free; g_ent_free = g_ent[ei].idx; }
    else {
        if (g_ent_n == g_ent_cap){ g_ent_cap = g_ent_cap ? 2 * g_ent_cap : 1024;
                                   g_ent = realloc(g_ent, g_ent_cap * sizeof *g_ent); }
        ei = (int32_t)g_ent_n++;
    }
    memset(&g_ent[ei], 0, sizeof g_ent[ei]);
    g_ent[ei].idx = idx; g_ent[ei].path = path; g_ent[ei].st = E_LOADING;
    if (idx >= 0) g_idx[idx].ent = ei;
    return ei;
}
static void ent_free(int32_t ei){
    Ent *en = &g_ent[ei];
    if (en->tex){ CFBridgingRelease(en->tex); en->tex = NULL; atomic_fetch_sub(&g_bytes, en->bytes); g_n_tex_live--; }
    if (en->idx >= 0 && g_idx[en->idx].ent == ei) g_idx[en->idx].ent = -1;
    en->idx = g_ent_free; g_ent_free = ei; en->st = E_FAILED; en->refs = en->waiters = 0;
}
/* free an Ent nobody shows or waits for (a loading one when its load ends).
 * A failed load stays linked as a marker so its file is not read again; a
 * budget failure is retried once enough memory is free (texrep_uploaded). */
static void ent_unref(int32_t ei){
    Ent *en = &g_ent[ei];
    if (en->refs || en->waiters || en->st == E_LOADING) return;
    if ((en->st == E_FAILED || en->st == E_BUDGET) && en->idx >= 0) return;
    ent_free(ei);
}
static void load_start(int32_t ei, const TR *t){
    unsigned w = t->w, h = t->h, olev = t->lev; uint32_t fmt = t->fmt;
    const char *path = g_paths + g_ent[ei].path;
    g_n_loaded++;
    dispatch_async(g_lq[g_lq_next++ % NLQ], ^{ load_job(ei, path, w, h, olev, fmt); });
}
static void pend_add(unsigned mid, unsigned gen){
    if (g_pend_n == g_pend_cap){ g_pend_cap = g_pend_cap ? 2 * g_pend_cap : 256;
                                 g_pend = realloc(g_pend, g_pend_cap * sizeof *g_pend); }
    g_pend[g_pend_n++] = (Pend){ mid, gen };
}
/* this id stops showing / waiting for its replacement: the game's own texture shows */
static void tr_drop(TR *t, unsigned mid){
    if (t->state == S_ACTIVE){
        op_post((Op){ OP_CLEAR, mid, 0, 0, 0, NULL });
        g_ent[t->ent].refs--; ent_unref(t->ent);
    } else if (t->state == S_PENDING){
        g_ent[t->ent].waiters--; ent_unref(t->ent);
    }
    t->state = S_NONE; t->gen++; t->ent = -1;
}

/* ---- guest hooks -------------------------------------------------------------- */
void texrep_created(unsigned mid, unsigned w, unsigned h, unsigned levels,
                    unsigned d3dfmt, unsigned usage, unsigned pool){
    if (g_init == 2) return;
    tr_init(); if (g_init == 2 || !mid || mid >= TEXREP_MAXTEX) return;
    TR *t = &g_tr[mid];
    if (t->state != S_NONE || t->hash) texrep_released(mid);     /* id reused */
    uint32_t gen = t->gen + 1;
    memset(t, 0, sizeof *t);
    t->gen = gen; t->ent = -1;
    g_n_created++;
    t->w = (uint16_t)(w > 65535 ? 65535 : w); t->h = (uint16_t)(h > 65535 ? 65535 : h);
    t->lev = (uint8_t)(levels > 15 ? 15 : levels); t->fmt = d3dfmt;
    char src[256]; src_rel(src, sizeof src);
    t->cat = (uint8_t)cat_of_src(src);
    /* static game textures only: managed pool, no usage flags (render
     * target, dynamic, autogen mips), a block or 32-bit format, more than
     * 32x32 texels (ramps and lookup tables), no movie frames */
    t->ok = pool == 1u && usage == 0u && d3d_pf(d3dfmt) != MTLPixelFormatInvalid
         && w >= 16 && h >= 16 && w * h > 1024u && w <= 4096 && h <= 4096
         && strncasecmp(src, "MOVIES/", 7) != 0;
    if (t->ok) g_n_eligible++;
}
static void tr_dynamic(TR *t, unsigned mid){
    t->dyn = 1; g_n_dyn++;
    if (t->dumped){ dump_unlink(t, t->hash); t->dumped = 0; }
    if (t->state != S_NONE){ g_n_drop++; tr_drop(t, mid); }
}
void texrep_surface_written(unsigned mid){
    if (g_init != 1 || !mid || mid >= TEXREP_MAXTEX) return;
    TR *t = &g_tr[mid];
    if (!t->ok || t->dyn) return;
    tr_dynamic(t, mid);
}
void texrep_released(unsigned mid){
    if (g_init != 1 || !mid || mid >= TEXREP_MAXTEX) return;
    TR *t = &g_tr[mid];
    tr_drop(t, mid);
    t->ok = 0;
}
void texrep_uploaded(unsigned mid, unsigned level, const uint8_t *data, unsigned pitch, unsigned rows){
    if (g_init != 1 || !mid || mid >= TEXREP_MAXTEX || !data) return;
    TR *t = &g_tr[mid];
    if (!t->ok || t->dyn) return;
    t->last_up = g_frame;
    if (level){
        if (level >= 16) return;
        double t0 = now_ms();
        size_t len = (size_t)pitch * rows;
        int again = (t->lvmask >> level) & 1u;
        if (!again){ t->lvmask |= 1u << level; t->lvh += XXH3_64bits_withSeed(data, len, level); }
        else g_n_relevel++;
        g_hash_ms += now_ms() - t0; g_hash_bytes += len;
        if (t->state == S_ACTIVE && t->k){
            Ent *en = &g_ent[t->ent];
            if (en->refs > 1){
                /* the Ent's lower levels are another id's: this one goes back
                 * to its own texture rather than change what the others show */
                g_n_unshare++; tr_drop(t, mid);
            } else {
                /* a stacked replacement keeps the game's level up to date */
                op_post((Op){ OP_COPY, mid, t->k, level, 1, NULL });
                if (again) en->solo = 1;               /* content may differ now: nobody shares it */
                else { en->lvmask = t->lvmask; en->lvh = t->lvh; }
            }
        }
        return;
    }
    double t0 = now_ms();
    size_t len = (size_t)pitch * rows;
    uint64_t seed = (uint64_t)t->w | (uint64_t)t->h << 16 | (uint64_t)t->fmt << 32;
    uint64_t hh = XXH3_64bits_withSeed(data, len, seed);
    g_n_hashed++; g_hash_bytes += len;
    if (t->hash && t->hash != hh){
        /* new content under the same texture: it is dynamic, the game's own
         * texture shows from now on (and a dump of the old content goes) */
        tr_dynamic(t, mid);
        g_hash_ms += now_ms() - t0;
        return;
    }
    int first = !t->hash;
    t->hash = hh; t->lvmask |= 1u;
    if (first && g_dumping){ dump_write(t, mid, data, len, pitch); t->dumped = 1; }
    if (first && g_active && t->state == S_NONE){
        IdxEnt *e = idx_get(hh);
        if (e){
            int32_t ei = e->ent;
            if (ei >= 0 && g_ent[ei].st == E_FAILED){ if (g_ent[ei].bytes == 1) g_n_shape++; else g_n_fail++; ei = -2; }
            else if (ei >= 0 && g_ent[ei].st == E_BUDGET){
                if (atomic_load(&g_bytes) + g_ent[ei].bytes > g_budget){ g_n_budget++; ei = -2; }
                else { ent_free(ei); ei = -1; }            /* memory was freed: try again */
            }
            if (ei != -2){
                g_n_hit++;
                if (ei < 0 || g_ent[ei].solo){                /* nothing to share: load it */
                    ei = ent_new(ei < 0 ? (int32_t)(e - g_idx) : -1, e->path);
                    if (g_ent[ei].idx < 0) g_n_private++;
                    load_start(ei, t);
                }
                t->state = S_PENDING; t->gen++; t->req_frame = g_frame; t->ent = ei;
                g_ent[ei].waiters++;
                pend_add(mid, t->gen);
                if (in_race()) g_load_race++;
            }
        }
    }
    g_hash_ms += now_ms() - t0;
}
static void lat_note(long d){
    int b = d <= 0 ? 0 : d == 1 ? 1 : d == 2 ? 2 : d <= 4 ? 3 : d <= 8 ? 4 : d <= 16 ? 5 : d <= 64 ? 6 : 7;
    g_lat_hist[b]++; if (d > g_lat_max) g_lat_max = d;
}
void texrep_frame(void){
    if (g_init != 1) return;
    double t0 = now_ms();
    g_frame++;
    /* finished loads */
    pthread_mutex_lock(&g_rmx);
    Ready *rs = g_ready; unsigned n = g_ready_n;
    g_ready = NULL; g_ready_n = g_ready_cap = 0;
    pthread_mutex_unlock(&g_rmx);
    for (unsigned i = 0; i < n; i++){
        Ready r = rs[i];
        Ent *en = &g_ent[r.ei];
        g_load_ms_sum += r.ms; if (r.ms > g_load_ms_max) g_load_ms_max = r.ms;
        if (r.fail){
            if (g_verbose) fprintf(stderr, "[texrep] %s: %s\n", en->idx >= 0 ? g_paths + g_idx[en->idx].path : "(private)",
                                   r.fail == 1 ? "size or format does not fit, skipped"
                                   : r.fail == 3 ? "over the memory budget, skipped" : "cannot read the file");
            en->st = r.fail == 3 ? E_BUDGET : E_FAILED;
            en->bytes = r.fail == 3 ? r.bytes : r.fail;   /* E_FAILED: 1 = shape, 2 = read */
            unsigned wt = en->waiters;
            if (r.fail == 1) g_n_shape += wt; else if (r.fail == 3) g_n_budget += wt; else g_n_fail += wt;
        } else {
            en->st = E_READY; en->tex = r.tex; en->k = (uint8_t)r.k; en->stacked = (uint8_t)r.stacked;
            en->bytes = r.bytes;
            if (++g_n_tex_live > g_n_tex_peak) g_n_tex_peak = g_n_tex_live;
            unsigned long long b = atomic_load(&g_bytes); if (b > g_bytes_peak) g_bytes_peak = b;
        }
        if (!en->waiters){ if (!r.fail) g_n_stale++; ent_unref(r.ei); }   /* nobody waits any more */
    }
    free(rs);
    /* ids waiting for an Ent */
    unsigned keep = 0;
    for (unsigned i = 0; i < g_pend_n; i++){
        Pend p = g_pend[i];
        TR *t = &g_tr[p.mid];
        if (t->gen != p.gen || t->state != S_PENDING) continue;          /* dropped meanwhile */
        Ent *en = &g_ent[t->ent];
        if (en->st == E_LOADING){ g_pend[keep++] = p; continue; }
        if (en->st != E_READY){                                          /* failed: stays original */
            int32_t ei = t->ent; t->state = S_NONE; t->ent = -1; en->waiters--; ent_unref(ei);
            continue;
        }
        /* stacked: publish once the game's levels are in (they are copied
         * under the pack's), once it stopped uploading (some textures never
         * get all their levels), or after 60 frames; a level that comes
         * later is copied then (OP_COPY) */
        unsigned want = t->lev >= 16 ? 0xFFFFu : (1u << t->lev) - 1u;
        int why = (t->lvmask & want) == want ? 0 : g_frame - t->last_up >= 2 ? 1 : g_frame - t->req_frame >= 60 ? 2 : -1;
        if (en->stacked && why < 0){ g_pend[keep++] = p; continue; }
        if (!en->stacked) why = 0;
        int own = en->refs == 0;
        if (!own && (en->solo || en->lev != t->lev || en->lvmask != t->lvmask || en->lvh != t->lvh)){
            /* the same pack file, other lower levels: this id gets its own copy */
            int32_t old = t->ent;
            en->waiters--;
            int32_t ei = ent_new(-1, en->path); g_n_private++;     /* (en may move: realloc) */
            t->ent = ei; g_ent[ei].waiters++;
            load_start(ei, t);
            ent_unref(old);
            g_pend[keep++] = p;
            continue;
        }
        if (own){ en->lev = t->lev; en->lvmask = t->lvmask; en->lvh = t->lvh; }
        else g_n_shared++;
        en->waiters--; en->refs++;
        t->state = S_ACTIVE; t->k = en->k;
        op_post((Op){ OP_SET, p.mid, en->k, 0, own && en->stacked, (void *)CFRetain(en->tex) });
        g_n_pub++; g_why[why]++; lat_note(g_frame - t->req_frame);
        if (in_race()) g_pub_race++;
        if (g_verbose) fprintf(stderr, "[texrep] f=%ld %ux%u %s %016llx -> x%u%s%s\n", g_frame, t->w, t->h,
                               fmt_name(t->fmt), (unsigned long long)t->hash, 1u << en->k,
                               en->stacked ? " (stacked)" : "", own ? "" : " (shared)");
    }
    g_pend_n = keep;
    double ms = now_ms() - t0 + g_hash_ms - g_hash_ms_mark;   /* this frame's drain + hashing since the last frame */
    g_hash_ms_mark = g_hash_ms;
    if (ms > g_frame_ms_max) g_frame_ms_max = ms;
    if (ms > 1.0) g_frames_over1++;
}

/* ---- main thread ---------------------------------------------------------------- */
void texrep_main_frame(id<MTLCommandQueue> q){
    Op *ops = NULL; unsigned n = 0;
    if (g_ops_n){                                        /* racy peek; the lock decides */
        pthread_mutex_lock(&g_omx);
        ops = g_ops; n = g_ops_n; g_ops = NULL; g_ops_n = g_ops_cap = 0;
        pthread_mutex_unlock(&g_omx);
    }
    id<MTLCommandBuffer> cb = nil; id<MTLBlitCommandEncoder> bl = nil;
    for (unsigned i = 0; i < n; i++){
        Op o = ops[i];
        if (o.mid >= TEXREP_MAXTEX){ if (o.tex) CFBridgingRelease(o.tex); continue; }
        if (o.op == OP_SET){
            id<MTLTexture> t = (__bridge_transfer id<MTLTexture>)o.tex;
            id<MTLTexture> orig = metal_tex_object(o.mid);
            if (!orig) continue;
            if (o.stacked && orig.pixelFormat == t.pixelFormat){
                NSUInteger nl = orig.mipmapLevelCount;
                if (o.k + nl > t.mipmapLevelCount) nl = t.mipmapLevelCount - o.k;
                if (!cb){ cb = [q commandBuffer]; cb.label = @"texrep"; bl = [cb blitCommandEncoder]; }
                if (nl) [bl copyFromTexture:orig sourceSlice:0 sourceLevel:0 toTexture:t
                           destinationSlice:0 destinationLevel:o.k sliceCount:1 levelCount:nl];
            }
            if (!g_texrep_tex[o.mid]) g_main_live++;
            g_texrep_tex[o.mid] = t; g_texrep_k[o.mid] = (uint8_t)o.k;
        } else if (o.op == OP_CLEAR){
            if (g_texrep_tex[o.mid]){ g_texrep_tex[o.mid] = nil; g_main_live--; }
        } else if (o.op == OP_COPY){
            id<MTLTexture> t = g_texrep_tex[o.mid], orig = metal_tex_object(o.mid);
            if (t && orig && orig.pixelFormat == t.pixelFormat && o.level < orig.mipmapLevelCount
                && o.k + o.level < t.mipmapLevelCount){
                if (!cb){ cb = [q commandBuffer]; cb.label = @"texrep"; bl = [cb blitCommandEncoder]; }
                [bl copyFromTexture:orig sourceSlice:0 sourceLevel:o.level toTexture:t
                   destinationSlice:0 destinationLevel:o.k + o.level sliceCount:1 levelCount:1];
            }
        }
    }
    free(ops);
    if (cb){ [bl endEncoding]; [cb commit]; }       /* before this frame's command buffer */
    g_texrep_frame_on = g_main_live && atomic_load(&g_shown);
    if (g_menu_item && atomic_load(&g_pack_live)){
        [g_menu_item setHidden:NO]; g_menu_item = nil;
        fprintf(stderr, "[texrep] app menu: \"Upscaled Textures\" (Cmd-T) shown\n");
    }
}
/* sampler key for a replaced texture (metal.m samp_for decodes bits 15-17):
 * where the game's sampler has no mip filter it read only the original's
 * level 0; the replacement gets linear mip filtering with the LOD clamped
 * at level k (= the original's level 0), so magnified it blends the pack's
 * levels and minified it still reads the game's own texels, as before. */
unsigned texrep_skey_slow(unsigned sk, unsigned k){
    unsigned mp = (sk >> 4) & 3u;
    g_mipstat[mp]++;
    if (mp == 0 && k) sk = (sk & ~0x30u) | 0x20u | ((k & 7u) << 15);
    return sk;
}

/* ---- toggle ---------------------------------------------------------------------- */
void texrep_set_shown(int on){
    int v = on < 0 ? !atomic_load(&g_shown) : !!on;
    atomic_store(&g_shown, v);
    fprintf(stderr, "[texrep] upscaled textures %s\n", v ? "shown" : "hidden (the game's own)");
}
int texrep_shown(void){ return atomic_load(&g_shown); }
@interface TexrepMenuTarget : NSObject
- (void)toggle:(NSMenuItem *)it;
@end
@implementation TexrepMenuTarget
- (void)toggle:(NSMenuItem *)it {
    texrep_set_shown(-1);
    [it setState:texrep_shown() ? NSControlStateValueOn : NSControlStateValueOff];
}
@end
static TexrepMenuTarget *g_menu_target;
/* the item (and its shortcut) stays hidden until a pack with something to
 * replace is indexed (texrep_main_frame): no pack, an empty one or parts
 * that match nothing = no menu item */
void texrep_add_menu(NSMenu *m){
    if (!texrep_wanted()) return;
    if (!g_menu_target) g_menu_target = [TexrepMenuTarget new];
    /* Cmd-T alone: Shift, Control and Option are game keys (gear up, gear
     * down, nitrous), and a modifier pressed before Command reaches the game
     * (keyboard_mac.m); Shift-Cmd-T sent a Shift press to the game whenever
     * Shift went down first.  A key pressed with Command never reaches it. */
    NSMenuItem *it = [m addItemWithTitle:@"Upscaled Textures" action:@selector(toggle:) keyEquivalent:@"t"];
    [it setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [it setTarget:g_menu_target];
    [it setState:texrep_shown() ? NSControlStateValueOn : NSControlStateValueOff];
    if (atomic_load(&g_pack_live)){ fprintf(stderr, "[texrep] app menu: \"Upscaled Textures\" (Cmd-T) shown\n"); return; }
    [it setHidden:YES];                       /* hidden items get no key equivalent */
    g_menu_item = it;
}

/* [status] line fragment (autotest.c) while a pack is in use: replacement
 * memory now / its peak and the loaded replacements; nothing otherwise */
int texrep_status(char *buf, size_t n){
    if (g_init != 1 || !g_active || !n) return 0;
    int k = snprintf(buf, n, " trep=%.0f/%.0fMB/%lu", atomic_load(&g_bytes) / 1048576.0, g_bytes_peak / 1048576.0,
                     g_n_tex_live);
    return k < 0 ? 0 : k;
}

/* ---- report ------------------------------------------------------------------------ */
void texrep_report(void){
    if (g_init != 1) return;
    if (g_active){
        fprintf(stderr, "[texrep] textures %lu created, %lu eligible, %lu hashed (%.1f MB, %.1f ms); "
                "pack hits %lu: %lu shown (%lu sharing a texture already loaded), %lu dropped (content changed), "
                "%lu went back to their own levels; %lu skipped (size/format), %lu over budget, %lu unreadable; dynamic %lu\n",
                g_n_created, g_n_eligible, g_n_hashed, g_hash_bytes / 1048576.0, g_hash_ms,
                g_n_hit, g_n_pub, g_n_shared, g_n_drop, g_n_unshare, g_n_shape, g_n_budget, g_n_fail, g_n_dyn);
        fprintf(stderr, "[texrep] replacement GPU memory %.1f MB (peak %.1f MB, budget %.0f MB) in %lu loaded texture(s) "
                "(peak %lu; %lu loads, %lu of them a private copy, %lu not needed any more); loader %.1f ms in total, "
                "slowest %.2f ms; lower levels uploaded again %lu\n",
                atomic_load(&g_bytes) / 1048576.0, g_bytes_peak / 1048576.0, g_budget / 1048576.0, g_n_tex_live,
                g_n_tex_peak, g_n_loaded, g_n_private, g_n_stale, g_load_ms_sum, g_load_ms_max, g_n_relevel);
        fprintf(stderr, "[texrep] publish latency (frames) 0:%lu 1:%lu 2:%lu 3-4:%lu 5-8:%lu 9-16:%lu 17-64:%lu >64:%lu, "
                "max %ld (all levels in %lu, uploads stopped %lu, 60 frames %lu); in race/free roam: %lu requested, %lu shown\n",
                g_lat_hist[0], g_lat_hist[1], g_lat_hist[2], g_lat_hist[3], g_lat_hist[4], g_lat_hist[5],
                g_lat_hist[6], g_lat_hist[7], g_lat_max, g_why[0], g_why[1], g_why[2], g_load_race, g_pub_race);
        fprintf(stderr, "[texrep] guest thread time in texrep per frame: max %.3f ms, %lu frame(s) over 1 ms of %ld; "
                "replaced binds by mip filter none/point/linear %lu/%lu/%lu\n",
                g_frame_ms_max, g_frames_over1, g_frame, g_mipstat[0], g_mipstat[1], g_mipstat[2]);
    }
    if (g_dumping)
        fprintf(stderr, "[texrep] dump: %lu texture(s) written to %s (%lu taken back: dynamic)\n",
                g_n_dumped, g_dumpdir, g_n_unlinked);
    if (g_q) dispatch_sync(g_q, ^{});                /* finish pending dump writes */
}
