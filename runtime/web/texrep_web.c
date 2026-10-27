/* texrep_web.c -- texture packs in the browser: TEXPACKS/<name>.pak in the
 * game folder (tools/texpacks.py builds them), picked with settings.ini
 * ui_textures = original | uhud (NFSU2_UI_TEXTURES): LunicAura106's
 * "Upscaled HUD", the menus, HUD, fonts, icons and logos redrawn at four
 * times their size; and minimap = original | detailed (NFSU2_MAP_TEXTURES):
 * JeansBig's "Detailed Map", the minimap's and world map's maps redrawn at
 * 2048x2048.  Both can be on: their entries are looked up together.
 *
 * A pack can have TEXPACKS/<name>.tracks beside it: changes to the game's
 * Tracks table (TrackInfo, loaded from GLOBAL\GLOBALB.LZC), which the
 * Detailed Map's installer makes in that file on a PC to frame its maps
 * (the URL tracks' are placed and scaled differently).  Here they are
 * written into the table in memory at every frame, while the pack is on.
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
#include "cpu.h"
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

/* the packs picked, each a TEXPACKS/<name>.pak; their entries are merged into one table, sorted by key */
#define MAXPACKS 2
typedef struct { char name[32]; FILE *fp; char *names; uint32_t names_at, names_len; } Pack;
static Pack g_packs[MAXPACKS]; static unsigned g_npacks;
static int g_init, g_on, g_shown = 1, g_log;
static char g_pack[72];                                   /* the packs' names, for the log */
static Ent *g_ent; static uint8_t *g_src; static unsigned g_n;   /* g_src: an entry's pack */
/* the packs' .tracks: a TrackInfo field of the track numbered num set to v */
typedef struct { int16_t num; uint16_t off; float v; } TrackEdit;
static TrackEdit *g_edits; static unsigned g_nedits;
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
    const Pack *p = &g_packs[g_src[e - g_ent]];
    return p->names && e->name >= p->names_at && e->name < p->names_at + p->names_len ? p->names + (e->name - p->names_at) : "?";
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
static int cmp_ent(const void *a, const void *b){
    const Ent *x = a, *y = b;
    return x->crc != y->crc ? (x->crc < y->crc ? -1 : 1) : x->adler != y->adler ? (x->adler < y->adler ? -1 : 1) : 0;
}
/* TrackInfo's fields a .tracks file may set (tools/texpacks.py TRACK_FIELDS), all floats */
static const struct { const char *name; uint16_t off; } TRACK_FIELDS[] = {
    {"TrackMapCalibrationOffsetX", 0xAC}, {"TrackMapCalibrationOffsetY", 0xB0}, {"TrackMapCalibrationZoomIn", 0xC0},
    {"MenuMapZoomOffsetX", 0x118}, {"MenuMapZoomOffsetY", 0x11C}, {"MenuMapZoomWidth", 0x120},
};
static void read_tracks(const char *name){
    char win[64], path[1024], line[160], field[48];
    snprintf(win, sizeof win, "TEXPACKS\\%s.tracks", name);
    FILE *f = fileio_resolve_game(win, path, sizeof path) ? fopen(path, "r") : 0;
    if (!f) return;
    unsigned n0 = g_nedits;
    while (fgets(line, sizeof line, f)){
        int num; float v; unsigned k;
        if (line[0] == '#' || sscanf(line, "%d %47s %f", &num, field, &v) != 3) continue;
        for (k = 0; k < sizeof TRACK_FIELDS / sizeof *TRACK_FIELDS && strcmp(field, TRACK_FIELDS[k].name); k++);
        if (k == sizeof TRACK_FIELDS / sizeof *TRACK_FIELDS){ fprintf(stderr, "[texrep] %s: %s: not a field this reads\n", path, field); continue; }
        TrackEdit *e = realloc(g_edits, (g_nedits + 1) * sizeof *g_edits);
        if (!e) break;
        g_edits = e; g_edits[g_nedits++] = (TrackEdit){(int16_t)num, TRACK_FIELDS[k].off, v};
    }
    fclose(f);
    fprintf(stderr, "[texrep] %s: %u changes to the Tracks table\n", path, g_nedits - n0);
}
/* TEXPACKS/<pack>.pak for the setting env: its value names the pack, "original" (or unset) is none */
static void open_pack(const char *env){
    const char *v = getenv(env);
    if (!v || !*v || !strcasecmp(v, "original") || g_npacks == MAXPACKS) return;
    Pack *p = &g_packs[g_npacks];
    size_t k = 0;
    for (; v[k] && k + 1 < sizeof p->name; k++){
        char c = (char)tolower((unsigned char)v[k]);
        if (!isalnum((unsigned char)c) && c != '_' && c != '-'){ fprintf(stderr, "[texrep] %s \"%s\": not a pack name\n", env, v); return; }
        p->name[k] = c;
    }
    p->name[k] = 0;
    char win[64], path[1024]; uint32_t hd[4];
    snprintf(win, sizeof win, "TEXPACKS\\%s.pak", p->name);
    if (!fileio_resolve_game(win, path, sizeof path) || !(p->fp = fopen(path, "rb"))){
        fprintf(stderr, "[texrep] %s: the game folder has no TEXPACKS/%s.pak: the game's own textures\n", p->name, p->name);
        return;
    }
    if (fread(hd, sizeof hd, 1, p->fp) != 1 || memcmp(hd, "NTXP", 4) || hd[1] != 1 || !hd[2] || hd[2] > 65536u){
        fprintf(stderr, "[texrep] %s: not a texture pack this runtime reads\n", path); fclose(p->fp); p->fp = 0; return;
    }
    unsigned n = hd[2];
    Ent *ent = realloc(g_ent, (g_n + n) * sizeof *g_ent);
    if (ent) g_ent = ent;
    uint8_t *src = ent ? realloc(g_src, g_n + n) : 0;
    if (src) g_src = src;
    if (!ent || !src || fread(g_ent + g_n, sizeof *g_ent, n, p->fp) != n){
        fprintf(stderr, "[texrep] %s: unreadable\n", path); fclose(p->fp); p->fp = 0; return;
    }
    memset(g_src + g_n, (int)g_npacks, n);
    /* the names (for the log): from the entries to the first texture's levels */
    uint32_t first = UINT32_MAX;
    for (unsigned i = g_n; i < g_n + n; i++) if (g_ent[i].off < first) first = g_ent[i].off;
    p->names_at = 16u + 40u * n;
    if (first > p->names_at && first - p->names_at < (1u << 20) && (p->names = malloc(first - p->names_at + 1))){
        p->names_len = (uint32_t)fread(p->names, 1, first - p->names_at, p->fp);
        p->names[p->names_len] = 0;
    }
    unsigned bad = 0;
    for (unsigned i = g_n; i < g_n + n; i++)
        if (ent_bytes(&g_ent[i]) != g_ent[i].size){ g_ent[i].ow = 0; bad++; }   /* never matched */
    fprintf(stderr, "[texrep] %s: %u textures%s\n", path, n - bad, bad ? " (some entries unreadable, left out)" : "");
    g_n += n;
    snprintf(g_pack + strlen(g_pack), sizeof g_pack - strlen(g_pack), "%s%s", g_npacks ? "+" : "", p->name);
    g_npacks++;
    read_tracks(p->name);
}
static void init(void){
    g_init = 1;
    g_log = getenv("NFSU2_TEXLOG") && *getenv("NFSU2_TEXLOG") == '1';
    open_pack("NFSU2_UI_TEXTURES");
    open_pack("NFSU2_MAP_TEXTURES");
    if (!g_n) return;
    /* one table: sorted by key, with each entry's pack beside it */
    struct { Ent e; uint8_t src; } *both = malloc(g_n * sizeof *both);
    g_used = calloc(g_n, 1); g_shape = calloc(g_n, sizeof *g_shape);
    if (!both || !g_used || !g_shape){ fprintf(stderr, "[texrep] out of memory\n"); free(both); return; }
    for (unsigned i = 0; i < g_n; i++){ both[i].e = g_ent[i]; both[i].src = g_src[i]; }
    qsort(both, g_n, sizeof *both, cmp_ent);              /* cmp_ent reads the Ent at the start */
    for (unsigned i = 0; i < g_n; i++){ g_ent[i] = both[i].e; g_src[i] = both[i].src; }
    free(both);
    for (unsigned i = 0; i < g_n; i++)
        if (g_ent[i].ow) g_shape[g_nshape++] = shape(g_ent[i].ow, g_ent[i].oh, g_ent[i].ofmt);
    qsort(g_shape, g_nshape, sizeof *g_shape, cmp_u64);
    unsigned u = 0;
    for (unsigned i = 0; i < g_nshape; i++) if (!u || g_shape[i] != g_shape[u - 1]) g_shape[u++] = g_shape[i];
    g_nshape = u;
    crc_init();
    g_on = 1;
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
    const Pack *p = &g_packs[g_src[e - g_ent]];
    if (fseek(p->fp, (long)e->off, SEEK_SET) || fread(g_buf, 1, e->size, p->fp) != e->size){
        fprintf(stderr, "[texrep] %s: %s unreadable\n", p->name, ent_name(e)); return 0;
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

/* ---- the guest thread's hooks (com.c) ----------------------------------------------------------- */
void texrep_created(unsigned mid, unsigned w, unsigned h, unsigned levels, unsigned d3dfmt, unsigned usage, unsigned pool){
    (void)usage; (void)pool;
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
void texrep_released(unsigned mid){ if (mid && mid < TEXREP_MAXTEX) g_t[mid].st = 0, g_bi[mid] = 0; }
/* the .tracks changes, into the Tracks table (TrackInfo[], 0x128 bytes each, +0x8A the int16 number) */
#define G_TRACKS   0x00883da0u
#define G_TRACKS_N 0x00883da4u
void texrep_frame(void){
    static uint32_t tab, n;
    static int32_t *at;                                   /* per edit: its track's index in the table, -1 none */
    static float *was;                                    /* per edit: the game's own value */
    if (!g_nedits || !mem_is_committed(G_TRACKS)) return;
    uint32_t t = MR32(G_TRACKS), c = MR32(G_TRACKS_N);
    if (!t || !c || c > 2000 || !mem_is_committed(t) || !mem_is_committed(t + c * 0x128u - 1)) return;
    if (t != tab || c != n){                              /* (again) loaded: find each edit's track */
        if (!at && !(at = malloc(g_nedits * sizeof *at))) return;
        if (!was && !(was = malloc(g_nedits * sizeof *was))) return;
        unsigned found = 0;
        for (unsigned k = 0; k < g_nedits; k++){
            at[k] = -1;
            for (uint32_t i = 0; i < c; i++)
                if ((int16_t)MR16(t + i * 0x128u + 0x8Au) == g_edits[k].num){
                    at[k] = (int32_t)i; was[k] = MRF32(t + i * 0x128u + g_edits[k].off); found++; break;
                }
        }
        tab = t; n = c;
        fprintf(stderr, "[texrep] Tracks table: %u tracks, %u of %u changes have their track\n", c, found, g_nedits);
    }
    for (unsigned k = 0; k < g_nedits; k++)
        if (at[k] >= 0) MRF32(t + (uint32_t)at[k] * 0x128u + g_edits[k].off) = g_shown ? g_edits[k].v : was[k];
}
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
