/* texrep_web.c -- texture packs in the browser: TEXPACKS/<name>.pak in the
 * game folder (tools/texpacks.py builds them), picked with settings.ini
 * ui_textures = original | uhud (NFSU2_UI_TEXTURES).  So far one pack:
 * LunicAura106's "Upscaled HUD", the menus, HUD, fonts, icons and logos
 * redrawn at four times their size.
 *
 * The game keeps reading its own files and sees every texture at its own
 * size.  When it uploads level 0 of a texture the pack has (the same CRC-32
 * and Adler-32, size and format), the GPU texture becomes the pack's, at the
 * pack's size (gpu_web.c metal_replace_texture), and the game's further
 * uploads to it are dropped.  The game samples textures with coordinates of
 * 0 to 1, so the larger one is drawn where the original was; where the game
 * measures a texture in texels (its fonts' glyph tables, the mouse cursor)
 * it still measures its own.  That is the work the mod's plugin
 * (NFSU2HDContentSupport.asi) does on a PC, where the installer writes the
 * larger textures into the game's files.  A texture whose content differs
 * (a language pack's own fonts) is not the pack's and stays as it is.
 *
 * The Mac's texrep.m binds its replacements in the draw encoder; here the
 * texture is made again instead, so the page needs nothing new.  Off by
 * default: with the option unset every hook returns at its first lines,
 * but for the one texture built in (the pause menu's Photo Mode icon), whose
 * size and format they look for always. */
#include "texrep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

int fileio_resolve_game(const char *win, char *out, size_t outsz);                  /* fileio.c */
void metal_replace_texture(unsigned id, unsigned w, unsigned h, unsigned levels, unsigned f, const uint8_t *data);
void metal_upload_texture(unsigned id, unsigned level, const void *data, unsigned pitch, unsigned w, unsigned h, unsigned f);
unsigned metal_level_pitch(unsigned w, unsigned level, unsigned f);                 /* gpu_web.c */
unsigned metal_level_rows(unsigned h, unsigned level, unsigned f);

/* tools/texpacks.py's entry: the game's texture (key, size, D3DFORMAT), the pack's, and where its levels are */
typedef struct { uint32_t crc, adler; uint16_t ow, oh; uint32_t ofmt; uint16_t w, h; uint32_t fmt, levels, off, size, name; } Ent;
_Static_assert(sizeof(Ent) == 40, "TEXPACKS entry");

static int g_init, g_on, g_shown = 1, g_log;
static char g_pack[32];
static FILE *g_fp;
static Ent *g_ent; static unsigned g_n;
static char *g_names; static uint32_t g_names_at, g_names_len;
static uint64_t *g_shape; static unsigned g_nshape;      /* the originals' w | h << 16 | fmt << 32, sorted */
static uint8_t *g_used;                                   /* per entry: shown at least once */
static uint8_t *g_buf; static size_t g_bufcap;
/* per texture id: st 0 = no texture of the pack has its size and format, 1 = may be one, 2 = the pack's now */
static struct { uint16_t w, h; uint8_t levels, st; uint32_t fmt; int32_t ent; } g_t[TEXREP_MAXTEX];
static unsigned long g_hashed, g_shown_n, g_given_back;
static unsigned long long g_hashed_bytes;

/* ---- the key: CRC-32 and Adler-32 (zlib's) of the level as uploaded ---------------------------- */
static uint32_t g_crc_tab[4][256];
static void crc_init(void){
    static int done;
    if (done) return;
    done = 1;
    for (uint32_t i = 0; i < 256; i++){
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        g_crc_tab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 4; t++) g_crc_tab[t][i] = (g_crc_tab[t - 1][i] >> 8) ^ g_crc_tab[0][g_crc_tab[t - 1][i] & 0xFF];
}
static uint32_t crc32(const uint8_t *p, size_t n){
    uint32_t c = 0xFFFFFFFFu;
    for (; n >= 4; n -= 4, p += 4){
        c ^= (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        c = g_crc_tab[3][c & 0xFF] ^ g_crc_tab[2][(c >> 8) & 0xFF] ^ g_crc_tab[1][(c >> 16) & 0xFF] ^ g_crc_tab[0][c >> 24];
    }
    while (n--) c = (c >> 8) ^ g_crc_tab[0][(c ^ *p++) & 0xFF];
    return ~c;
}
static uint32_t adler32(const uint8_t *p, size_t n){
    uint32_t a = 1, b = 0;
    while (n){
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--){ a += *p++; b += a; }
        a %= 65521u; b %= 65521u;
    }
    return b << 16 | a;
}

/* ---- the pack ---------------------------------------------------------------------------------- */
static uint64_t shape(unsigned w, unsigned h, unsigned fmt){ return (uint64_t)w | (uint64_t)h << 16 | (uint64_t)fmt << 32; }
static int cmp_u64(const void *a, const void *b){
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static const char *ent_name(const Ent *e){
    return e->name >= g_names_at && e->name < g_names_at + g_names_len ? g_names + (e->name - g_names_at) : "?";
}
/* levels' bytes as gpu_web.c uploads them; 0 if the entry is not one it can make */
static size_t ent_bytes(const Ent *e){
    if (!e->w || !e->h || e->w > 4096 || e->h > 4096 || !e->levels || e->levels > 13) return 0;
    size_t n = 0;
    for (unsigned l = 0; l < e->levels; l++){
        size_t lv = (size_t)metal_level_pitch(e->w, l, e->fmt) * metal_level_rows(e->h, l, e->fmt);
        if (!lv) return 0;
        n += lv;
    }
    return n;
}
static void init(void){
    g_init = 1;
    const char *v = getenv("NFSU2_UI_TEXTURES");
    if (!v || !*v || !strcasecmp(v, "original")) return;
    size_t k = 0;
    for (; v[k] && k + 1 < sizeof g_pack; k++){
        char c = (char)tolower((unsigned char)v[k]);
        if (!isalnum((unsigned char)c) && c != '_' && c != '-'){ fprintf(stderr, "[texrep] ui_textures \"%s\": not a pack name\n", v); return; }
        g_pack[k] = c;
    }
    g_pack[k] = 0;
    g_log = getenv("NFSU2_TEXLOG") && *getenv("NFSU2_TEXLOG") == '1';
    char win[64], path[1024]; uint32_t hd[4];
    snprintf(win, sizeof win, "TEXPACKS\\%s.pak", g_pack);
    if (!fileio_resolve_game(win, path, sizeof path) || !(g_fp = fopen(path, "rb"))){
        fprintf(stderr, "[texrep] %s: the game folder has no TEXPACKS/%s.pak: the game's own textures\n", g_pack, g_pack);
        return;
    }
    if (fread(hd, sizeof hd, 1, g_fp) != 1 || memcmp(hd, "NTXP", 4) || hd[1] != 1 || !hd[2] || hd[2] > 65536u){
        fprintf(stderr, "[texrep] %s: not a texture pack this runtime reads\n", path); fclose(g_fp); g_fp = 0; return;
    }
    g_n = hd[2];
    g_ent = calloc(g_n, sizeof *g_ent); g_used = calloc(g_n, 1); g_shape = calloc(g_n, sizeof *g_shape);
    if (!g_ent || !g_used || !g_shape || fread(g_ent, sizeof *g_ent, g_n, g_fp) != g_n){
        fprintf(stderr, "[texrep] %s: unreadable\n", path); fclose(g_fp); g_fp = 0; return;
    }
    /* the names (for the log): from the entries to the first texture's levels */
    uint32_t first = UINT32_MAX;
    for (unsigned i = 0; i < g_n; i++) if (g_ent[i].off < first) first = g_ent[i].off;
    g_names_at = 16u + 40u * g_n;
    if (first > g_names_at && first - g_names_at < (1u << 20) && (g_names = malloc(first - g_names_at + 1))){
        g_names_len = (uint32_t)fread(g_names, 1, first - g_names_at, g_fp);
        g_names[g_names_len] = 0;
    }
    unsigned bad = 0;
    for (unsigned i = 0; i < g_n; i++){
        Ent *e = &g_ent[i];
        if (ent_bytes(e) != e->size){ e->ow = 0; bad++; continue; }   /* never matched */
        g_shape[g_nshape++] = shape(e->ow, e->oh, e->ofmt);
    }
    qsort(g_shape, g_nshape, sizeof *g_shape, cmp_u64);
    unsigned u = 0;
    for (unsigned i = 0; i < g_nshape; i++) if (!u || g_shape[i] != g_shape[u - 1]) g_shape[u++] = g_shape[i];
    g_nshape = u;
    crc_init();
    g_on = 1;
    fprintf(stderr, "[texrep] %s: %u textures (%u sizes)%s\n", path, g_n - bad, g_nshape,
            bad ? " (some entries unreadable, left out)" : "");
}
static int may_be(unsigned w, unsigned h, unsigned fmt){
    uint64_t s = shape(w, h, fmt);
    return bsearch(&s, g_shape, g_nshape, sizeof *g_shape, cmp_u64) != NULL;
}
static int find(uint32_t crc, uint32_t adler, unsigned w, unsigned h, unsigned fmt){
    unsigned lo = 0, hi = g_n;
    while (lo < hi){
        unsigned m = (lo + hi) / 2;
        if (g_ent[m].crc < crc || (g_ent[m].crc == crc && g_ent[m].adler < adler)) lo = m + 1; else hi = m;
    }
    for (; lo < g_n && g_ent[lo].crc == crc && g_ent[lo].adler == adler; lo++)
        if (g_ent[lo].ow == w && g_ent[lo].oh == h && g_ent[lo].ofmt == fmt) return (int)lo;
    return -1;
}
static const uint8_t *levels_of(const Ent *e){
    if (e->size > g_bufcap){
        uint8_t *b = realloc(g_buf, e->size);
        if (!b) return 0;
        g_buf = b; g_bufcap = e->size;
    }
    if (fseek(g_fp, (long)e->off, SEEK_SET) || fread(g_buf, 1, e->size, g_fp) != e->size){
        fprintf(stderr, "[texrep] %s: %s unreadable\n", g_pack, ent_name(e)); return 0;
    }
    return g_buf;
}

/* ---- built in, with or without a pack: the pause menu's Photo Mode icon (photo.c) ------------------
 * drawn in place of INGAME_ICON_VOIP_ON, which nothing in the PC game draws: white, its alpha from
 * photo_icon.h (tools/photoicon.py), at four times the original's size, with its mip levels */
#include "photo_icon.h"
static uint8_t g_bi[TEXREP_MAXTEX];                       /* per texture id: 1 the original's size and format, 2 the icon now */
static uint8_t g_bi_levels[TEXREP_MAXTEX];
static uint8_t *g_icon; static unsigned g_icon_levels;
static const uint8_t *icon_levels(void){
    if (g_icon) return g_icon;
    unsigned n = 0, lv = 0;
    for (unsigned w = PHOTO_ICON_W, h = PHOTO_ICON_H; ; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1){
        n += w * h; lv++;
        if (w == 1 && h == 1) break;
    }
    uint8_t *a = malloc(n), *argb = malloc((size_t)n * 4u);
    if (!a || !argb){ free(a); free(argb); return 0; }
    unsigned k = 0;
    for (size_t i = 0; i + 1 < sizeof photo_icon_rle && k < PHOTO_ICON_W * PHOTO_ICON_H; i += 2)
        for (unsigned r = 0; r < photo_icon_rle[i] && k < PHOTO_ICON_W * PHOTO_ICON_H; r++) a[k++] = photo_icon_rle[i + 1];
    /* the levels below: 2x2 box averages */
    uint8_t *src = a;
    for (unsigned w = PHOTO_ICON_W, h = PHOTO_ICON_H; w > 1 || h > 1; ){
        unsigned nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, dx = w > 1, dy = h > 1;
        uint8_t *dst = src + w * h;
        for (unsigned y = 0; y < nh; y++)
            for (unsigned x = 0; x < nw; x++){
                const uint8_t *p = src + (y << dy) * w + (x << dx);
                dst[y * nw + x] = (uint8_t)((p[0] + p[dx] + p[dy * w] + p[dy * w + dx] + 2) / 4);
            }
        src = dst; w = nw; h = nh;
    }
    for (unsigned i = 0; i < n; i++){ argb[i * 4] = argb[i * 4 + 1] = argb[i * 4 + 2] = 255; argb[i * 4 + 3] = a[i]; }
    free(a);
    g_icon_levels = lv;
    return g_icon = argb;
}
/* 1: this upload of level 0 was the icon's texture and is the icon now */
static int builtin_uploaded(unsigned mid, const uint8_t *data, size_t n){
    crc_init();
    int is = n == (size_t)PHOTO_ICON_KEY_W * PHOTO_ICON_KEY_H && crc32(data, n) == PHOTO_ICON_KEY_CRC &&
             adler32(data, n) == PHOTO_ICON_KEY_ADLER;
    if (is && g_bi[mid] == 2) return 1;
    const uint8_t *lv = is ? icon_levels() : 0;
    if (lv){
        metal_replace_texture(mid, PHOTO_ICON_W, PHOTO_ICON_H, g_icon_levels, 21u, lv);
        g_bi[mid] = 2;
        return 1;
    }
    if (g_bi[mid] != 2) return 0;
    /* the game wrote something else into it: its own texture again (gpu_web.c dropped this upload) */
    metal_replace_texture(mid, PHOTO_ICON_KEY_W, PHOTO_ICON_KEY_H, g_bi_levels[mid], PHOTO_ICON_KEY_FMT, NULL);
    metal_upload_texture(mid, 0, data, metal_level_pitch(PHOTO_ICON_KEY_W, 0, PHOTO_ICON_KEY_FMT),
                         PHOTO_ICON_KEY_W, PHOTO_ICON_KEY_H, PHOTO_ICON_KEY_FMT);
    g_bi[mid] = 1;
    return 0;
}

/* ---- copied at run time, with or without a pack: the crib menu's Save icon, for the pause menu's Save (savegame.c)
 * The crib menu's Save item draws the front end's texture 0x8ADA6FD4 (FRONTEND/FrontB.lzc), which the world does not
 * have.  INGAME_ICON_VOIP_MUTE (GLOBAL/InGameCommon.bun), which nothing in the PC game draws, gets its levels instead,
 * copied from the game's own upload of it: the front end comes before free roam (Resume Career goes through the
 * crib).  Both are 128 x 64 DXT3 and each is known by its level 0 (CRC-32, Adler-32; nothing else in the game data
 * has either).  Until the copy is there the slot keeps the game's own picture. */
#define CRIB_SAVE_CRC    0xe67ab8c3u
#define CRIB_SAVE_ADLER  0xa6278273u
#define VOIP_MUTE_CRC    0xdaa6fa6du
#define VOIP_MUTE_ADLER  0xb85b6f66u
#define CP_W             128u
#define CP_H             64u
#define CP_FMT           PHOTO_ICON_KEY_FMT      /* DXT3, as both are (and the photo icon's original) */
#define CP_LEVELS_MAX    8u                      /* 128 x 64 down to 1 x 1 */
static struct { unsigned src, dst, levels, got; int done, on; uint8_t *buf; } g_cp;
static size_t cp_level_bytes(unsigned l){ return (size_t)metal_level_pitch(CP_W, l, CP_FMT) * metal_level_rows(CP_H, l, CP_FMT); }
static size_t cp_off(unsigned l){ size_t o = 0; for (unsigned k = 0; k < l; k++) o += cp_level_bytes(k); return o; }
static void cp_show(void){
    if (!g_cp.done || !g_cp.dst || g_cp.on) return;
    metal_replace_texture(g_cp.dst, CP_W, CP_H, g_cp.levels, CP_FMT, g_cp.buf);
    g_cp.on = 1;
    fprintf(stderr, "[texrep] t%u: the crib's Save icon in INGAME_ICON_VOIP_MUTE, for the pause menu's Save\n", g_cp.dst);
}
/* 1: this upload was to the slot, which shows the copy (gpu_web.c drops the game's own) */
static int copied_uploaded(unsigned mid, unsigned level, const uint8_t *data, size_t n){
    if (!level && n == cp_level_bytes(0)){
        crc_init();
        uint32_t crc = crc32(data, n), adler = adler32(data, n);
        if (crc == CRIB_SAVE_CRC && adler == CRIB_SAVE_ADLER && !g_cp.done){
            if (!g_cp.buf) g_cp.buf = malloc(cp_off(CP_LEVELS_MAX));
            g_cp.src = g_cp.buf ? mid : 0; g_cp.got = 0;
            g_cp.levels = g_bi_levels[mid] < 1 ? 1u : g_bi_levels[mid] > CP_LEVELS_MAX ? CP_LEVELS_MAX : g_bi_levels[mid];
        } else if (crc == VOIP_MUTE_CRC && adler == VOIP_MUTE_ADLER){
            g_cp.dst = mid; g_cp.on = 0;
            cp_show();
            return g_cp.on;
        } else if (mid == g_cp.dst && g_cp.on){
            /* the game wrote something else into it: its own texture again (gpu_web.c dropped this upload) */
            metal_replace_texture(mid, CP_W, CP_H, g_bi_levels[mid] ? g_bi_levels[mid] : 1u, CP_FMT, NULL);
            metal_upload_texture(mid, 0, data, metal_level_pitch(CP_W, 0, CP_FMT), CP_W, CP_H, CP_FMT);
            g_cp.dst = 0; g_cp.on = 0;
            return 1;
        }
    }
    if (mid == g_cp.src && !g_cp.done && level < g_cp.levels && n >= cp_level_bytes(level)){
        memcpy(g_cp.buf + cp_off(level), data, cp_level_bytes(level));
        g_cp.got |= 1u << level;
        if (g_cp.got == (1u << g_cp.levels) - 1u){
            g_cp.done = 1;
            fprintf(stderr, "[texrep] t%u: the crib's Save icon kept (%u levels)\n", mid, g_cp.levels);
            cp_show();
        }
    }
    return mid == g_cp.dst && g_cp.on;
}
static void cp_gone(unsigned mid){
    if (mid == g_cp.src) g_cp.src = 0;
    if (mid == g_cp.dst){ g_cp.dst = 0; g_cp.on = 0; }
}

/* ---- the guest thread's hooks (com.c) ----------------------------------------------------------- */
void texrep_created(unsigned mid, unsigned w, unsigned h, unsigned levels, unsigned d3dfmt, unsigned usage, unsigned pool){
    (void)usage; (void)pool;
    cp_gone(mid);
    if (mid && mid < TEXREP_MAXTEX){
        g_bi[mid] = w == PHOTO_ICON_KEY_W && h == PHOTO_ICON_KEY_H && d3dfmt == PHOTO_ICON_KEY_FMT;
        g_bi_levels[mid] = (uint8_t)(levels > 255 ? 255 : levels);
    }
    if (!g_init) init();
    if (!g_on || !mid || mid >= TEXREP_MAXTEX) return;
    g_t[mid].w = (uint16_t)w; g_t[mid].h = (uint16_t)h; g_t[mid].levels = (uint8_t)(levels > 255 ? 255 : levels);
    g_t[mid].fmt = d3dfmt; g_t[mid].ent = -1;
    g_t[mid].st = w <= 0xFFFF && h <= 0xFFFF && may_be(w, h, d3dfmt);
}
void texrep_uploaded(unsigned mid, unsigned level, const uint8_t *data, unsigned pitch, unsigned rows){
    if (mid && mid < TEXREP_MAXTEX && g_bi[mid] && copied_uploaded(mid, level, data, (size_t)pitch * rows)) return;
    if (mid && mid < TEXREP_MAXTEX && g_bi[mid] && !level && builtin_uploaded(mid, data, (size_t)pitch * rows)) return;
    if (!g_on || !mid || mid >= TEXREP_MAXTEX || !g_t[mid].st || level) return;
    size_t n = (size_t)pitch * rows;
    uint32_t crc = crc32(data, n), adler = adler32(data, n);
    g_hashed++; g_hashed_bytes += n;
    int i = find(crc, adler, g_t[mid].w, g_t[mid].h, g_t[mid].fmt);
    if (i >= 0 && g_t[mid].st == 2 && g_t[mid].ent == i) return;     /* the same content again */
    const uint8_t *lv = i >= 0 && g_shown ? levels_of(&g_ent[i]) : 0;
    if (lv){
        const Ent *e = &g_ent[i];
        metal_replace_texture(mid, e->w, e->h, e->levels, e->fmt, lv);
        g_t[mid].st = 2; g_t[mid].ent = i; g_used[i] = 1; g_shown_n++;
        if (g_log) fprintf(stderr, "[texrep] t%u %s %ux%u -> %ux%u\n", mid, ent_name(e), e->ow, e->oh, e->w, e->h);
    } else if (g_t[mid].st == 2){
        /* the game wrote something else into it: its own texture again (gpu_web.c dropped this upload) */
        metal_replace_texture(mid, g_t[mid].w, g_t[mid].h, g_t[mid].levels, g_t[mid].fmt, NULL);
        metal_upload_texture(mid, 0, data, pitch, g_t[mid].w, g_t[mid].h, g_t[mid].fmt);
        g_t[mid].st = 1; g_t[mid].ent = -1; g_given_back++;
    }
}
void texrep_surface_written(unsigned mid){
    /* written through a surface (the car skin composite): dynamic, never the pack's */
    if (!g_on || !mid || mid >= TEXREP_MAXTEX || !g_t[mid].st) return;
    if (g_t[mid].st == 2){
        metal_replace_texture(mid, g_t[mid].w, g_t[mid].h, g_t[mid].levels, g_t[mid].fmt, NULL);
        g_given_back++;
        fprintf(stderr, "[texrep] t%u %s: written through a surface, the game's own again\n", mid, ent_name(&g_ent[g_t[mid].ent]));
    }
    g_t[mid].st = 0;
}
void texrep_released(unsigned mid){ cp_gone(mid); if (mid && mid < TEXREP_MAXTEX) g_t[mid].st = 0, g_bi[mid] = 0; }
void texrep_frame(void){ }
void texrep_report(void){
    if (!g_on) return;
    unsigned used = 0;
    for (unsigned i = 0; i < g_n; i++) used += g_used[i];
    fprintf(stderr, "[texrep] %s: %lu textures shown from the pack (%u of its %u), %lu given back to the game; "
            "%lu uploads hashed (%.1f MB)\n", g_pack, g_shown_n, used, g_n, g_given_back, g_hashed, g_hashed_bytes / 1048576.0);
    if (g_log)
        for (unsigned i = 0; i < g_n; i++)
            if (!g_used[i] && g_ent[i].ow) fprintf(stderr, "[texrep]   never shown: %s\n", ent_name(&g_ent[i]));
}

/* ---- any thread ---------------------------------------------------------------------------------- */
int  texrep_wanted(void){ return g_on; }
/* autotest "texrep on|off|toggle": textures uploaded from now on (those shown stay until the game loads them again) */
void texrep_set_shown(int on){ g_shown = on < 0 ? !g_shown : on != 0; fprintf(stderr, "[texrep] %s: %s\n", g_pack, g_shown ? "on" : "off"); }
int  texrep_shown(void){ return g_on && g_shown; }
int  texrep_status(char *buf, size_t n){
    if (!g_on || !n) return 0;
    int k = snprintf(buf, n, " trep=%lu", g_shown_n);
    return k < 0 ? 0 : k;
}
