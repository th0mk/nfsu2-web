/* movie_av1.c -- the movies in AV1 (tools/movies_av1.py): the game's own movie
 * player reads them, and the browser decodes their pictures.  Every game's
 * part of it; the game's own file hooks its player (movie_av1.h).
 *
 * Both games' players are RealCore's VP6 codec object with the On2 VP6
 * decoder statically linked behind three calls, all cdecl:
 *   decode(decoder, data, size, w, h)   decode a video chunk's payload (the
 *            chunk past its 8-byte header; also for the frames skipped on the
 *            way to the one shown)
 *   planes(decoder, &yv12)              post-process, then the picture's
 *            planes: 11 dwords {Y width, Y height, Y stride, UV width, UV
 *            height, UV stride, Y, U, V, the buffer, ?}; the codec's frame
 *            call keeps the buffer's address, no copy, and reads the three
 *            planes from it as one block of (w+96)*(h+96)*3/2 bytes: Y with a
 *            48-pixel border, then U and V with 24
 *   free(&decoder)                      free the decoder
 * An AV1 movie is the VP6 movie with each video chunk's payload swapped for
 * 'AV01', a flags byte (1: key frame), three zero bytes and one AV1 temporal
 * unit (a VP6 frame never starts with 'AV01').  Its decoder is still made
 * (from the header's size), so the player's own state stays as it was; a
 * chunk with the tag goes to the page instead (web/src/host/movie.ts, the I/O
 * worker's WebCodecs VideoDecoder), which writes the picture into a block of
 * that layout, and the planes call hands the player that block.  Timing,
 * sound, skipping, the end message and the subtitles stay the game's.
 *
 * The page plays the AV1 files only where the browser decodes AV1 (it asks
 * the I/O worker for MOVIES/AV1/<name>.vp6 in place of MOVIES/<name>.vp6), so
 * elsewhere, and on the Mac, no chunk has the tag and nothing here runs.
 * One line a movie in the log: "[movie] AV1 768x512: 1867 frames, ..." */
#include "cpu.h"
#include "movie_av1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __wasm__
#include "web/host_abi.h"
/* web/src/host/game-worker.ts: the I/O worker's VideoDecoder (movie.ts) */
__attribute__((import_module("nfsu2"), import_name("movie_open")))
int nfsu2_movie_open(int w, int h);
__attribute__((import_module("nfsu2"), import_name("movie_decode")))
int nfsu2_movie_decode(int handle, uint32_t payload, uint32_t len, uint32_t y, uint32_t u, uint32_t v,
                       int ystride, int uvstride);
__attribute__((import_module("nfsu2"), import_name("movie_close")))
int nfsu2_movie_close(int handle);
static double now_ms(void){ return nfsu2_now_ms(); }
#else
static int nfsu2_movie_open(int w, int h){ (void)w; (void)h; return -1; }
static int nfsu2_movie_decode(int hd, uint32_t p, uint32_t l, uint32_t y, uint32_t u, uint32_t v, int ys, int uvs){
    (void)hd; (void)p; (void)l; (void)y; (void)u; (void)v; (void)ys; (void)uvs; return -1; }
static int nfsu2_movie_close(int handle){ (void)handle; return 0; }
static double now_ms(void){ return 0; }
#endif

uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void gheap_free(int heap, uint32_t p);

#define TAG       0x31305641u      /* 'AV01' */
#define HANDLE(h) (((uint32_t)(h) & 0xffff0000u) == 0x4d560000u)   /* movie.ts: 'MV' << 16 | n */
#define BORDER    48u
#define SLOTS     4

typedef struct {
    uint32_t dec;                  /* the VP6 decoder it stands in for (0: free slot) */
    int handle;                    /* the page's decoder, or <0 if it has none */
    uint32_t buf, w, h, ys, uvs;   /* the picture block and its layout */
    uint32_t frames, missed;       /* decodes; those the page gave no picture for */
    double ms, max;                /* time in the page's decode */
} Movie;
static Movie g_mv[SLOTS];

/* NFSU2_MOVIE_LOG=1: the VP6 movies' layout and catch-ups too */
static int movie_log(void){
    static int on = -1;
    if (on < 0){ const char *e = getenv("NFSU2_MOVIE_LOG"); on = e && *e == '1'; }
    return on;
}

/* NFSU2_MOVIE_DUMP=i,j,...: the pictures the player gets for those frames of each movie (both kinds), as they are
 * in memory (the visible rows, bottom-up; Y, then U and V), into $HOME/moviedump_<movie>_<frame>.yuv, the movies
 * numbered as they start: a VP6 run and an AV1 run of the same scenes, frame for frame (tests) */
static struct { uint32_t dec, frames, n; } g_cnt[SLOTS];
static uint32_t g_movies;
static const char *dump_list(void){
    static const char *l = (const char *)-1;
    if (l == (const char *)-1){ l = getenv("NFSU2_MOVIE_DUMP"); if (l && !*l) l = 0; }
    return l;
}
static void count_frame(uint32_t dec){
    if (!dump_list()) return;
    for (int i = 0; i < SLOTS; i++) if (g_cnt[i].dec == dec){ g_cnt[i].frames++; return; }
    for (int i = 0; i < SLOTS; i++) if (!g_cnt[i].dec){ g_cnt[i].dec = dec; g_cnt[i].frames = 1; g_cnt[i].n = ++g_movies; return; }
}
static void dump_frame(uint32_t dec, uint32_t o){
    const char *l = dump_list();
    if (!l) return;
    for (int i = 0; i < SLOTS; i++) if (g_cnt[i].dec == dec){
        uint32_t f = g_cnt[i].frames - 1u, w = MR32(o), h = MR32(o + 4u);
        int hit = 0;
        for (const char *p = l; *p; ){ char *e; unsigned long v = strtoul(p, &e, 10); if (e == p) break; if (v == f) hit = 1; p = *e ? e + 1 : e; }
        if (!hit) return;
        char path[256]; const char *home = getenv("HOME");
        snprintf(path, sizeof path, "%s/moviedump_%u_%u.yuv", home ? home : ".", g_cnt[i].n, f);
        FILE *fp = fopen(path, "wb");
        if (!fp) return;
        for (uint32_t r = 0; r < h; r++) fwrite(g_mem + MR32(o + 24u) + r * MR32(o + 8u), 1, w, fp);
        for (uint32_t pl = 0; pl < 2u; pl++)
            for (uint32_t r = 0; r < h / 2u; r++) fwrite(g_mem + MR32(o + 28u + 4u * pl) + r * MR32(o + 20u), 1, w / 2u, fp);
        fclose(fp);
        fprintf(stderr, "[movie] dump: movie %u frame %u (%ux%u)\n", g_cnt[i].n, f, w, h);
        return;
    }
}
static void uncount(uint32_t dec){
    for (int i = 0; i < SLOTS; i++) if (g_cnt[i].dec == dec) g_cnt[i].dec = 0;
}

static Movie *find(uint32_t dec){
    for (int i = 0; i < SLOTS; i++) if (g_mv[i].dec == dec) return &g_mv[i];
    return 0;
}

static uint32_t plane_y(const Movie *m){ return m->buf + BORDER * m->ys + BORDER; }
static uint32_t plane_u(const Movie *m){ return m->buf + m->ys * (m->h + 2u * BORDER) + BORDER / 2u * m->uvs + BORDER / 2u; }
static uint32_t plane_v(const Movie *m){ return plane_u(m) + m->uvs * (m->h / 2u + BORDER); }

static Movie *open_movie(uint32_t dec, uint32_t w, uint32_t h){
    Movie *m = find(0);
    if (!m) return 0;
    memset(m, 0, sizeof *m);
    m->w = w; m->h = h; m->ys = w + 2u * BORDER; m->uvs = m->ys / 2u;
    uint32_t ysz = m->ys * (h + 2u * BORDER), csz = m->uvs * (h / 2u + BORDER);
    m->buf = gheap_alloc(1, ysz + 2u * csz, 0);
    if (!m->buf){ fprintf(stderr, "[movie] no memory for a %ux%u picture\n", w, h); return 0; }
    memset(g_mem + m->buf, 16, ysz);                       /* black, as a frame not decoded yet */
    memset(g_mem + m->buf + ysz, 128, 2u * csz);
    m->dec = dec;
    int hd = nfsu2_movie_open((int)w, (int)h);
    m->handle = HANDLE(hd) ? hd : -1;
    if (m->handle < 0) fprintf(stderr, "[movie] AV1 %ux%u: this page has no AV1 decoder, the movie stays black\n", w, h);
    return m;
}

static void close_movie(Movie *m, uint32_t catchups){
    if (m->handle >= 0) nfsu2_movie_close(m->handle);
    fprintf(stderr, "[movie] AV1 %ux%u: %u frames, %u without a picture, decode %.2f ms a frame (max %.1f), %u catch-ups\n",
            m->w, m->h, m->frames, m->missed, m->frames ? m->ms / m->frames : 0.0, m->max, catchups);
    gheap_free(1, m->buf);
    memset(m, 0, sizeof *m);
}

int movie_av1_decode(uint32_t dec, uint32_t data, uint32_t size, uint32_t w, uint32_t h){
    count_frame(dec);
    if (size < 8u || MR32(data) != TAG) return 0;
    Movie *m = find(dec);
    if (!m) m = open_movie(dec, w, h);
    if (m){
        m->frames++;
        if (m->handle < 0) m->missed++;
        else {
            double t0 = now_ms();
            int r = nfsu2_movie_decode(m->handle, (uint32_t)(uintptr_t)(g_mem + data), size,
                                       (uint32_t)(uintptr_t)(g_mem + plane_y(m)), (uint32_t)(uintptr_t)(g_mem + plane_u(m)),
                                       (uint32_t)(uintptr_t)(g_mem + plane_v(m)), (int)m->ys, (int)m->uvs);
            double dt = now_ms() - t0;
            m->ms += dt; if (dt > m->max) m->max = dt;
            if (r) m->missed++;    /* no picture in time: the last one stays */
        }
    }
    return 1;
}

int movie_av1_planes(uint32_t dec, uint32_t o){
    Movie *m = find(dec);
    if (!m) return 0;
    MR32(o) = m->w; MR32(o + 4u) = m->h; MR32(o + 8u) = m->ys;
    MR32(o + 12u) = m->w / 2u; MR32(o + 16u) = m->h / 2u; MR32(o + 20u) = m->uvs;
    MR32(o + 24u) = plane_y(m); MR32(o + 28u) = plane_u(m); MR32(o + 32u) = plane_v(m);
    MR32(o + 36u) = m->buf;
    dump_frame(dec, o);
    return 1;
}

/* a VP6 movie's planes, once a decoder (the layout above) */
void movie_av1_vp6_planes(uint32_t dec, uint32_t o){
    static uint32_t seen;
    if (movie_log() && dec != seen){
        seen = dec;
        uint32_t b = MR32(o + 36u);
        fprintf(stderr, "[movie] VP6 %ux%u strides %u/%u, planes at +%u/+%u/+%u of the buffer\n",
                MR32(o), MR32(o + 4u), MR32(o + 8u), MR32(o + 20u), MR32(o + 24u) - b, MR32(o + 28u) - b, MR32(o + 32u) - b);
    }
    dump_frame(dec, o);
}

void movie_av1_free(uint32_t slot, uint32_t catchups){
    Movie *m = slot ? find(MR32(slot)) : 0;
    if (slot) uncount(MR32(slot));
    if (m && m->dec) close_movie(m, catchups);
    else if (movie_log() && slot && MR32(slot)) fprintf(stderr, "[movie] VP6: %u catch-ups\n", catchups);
}
