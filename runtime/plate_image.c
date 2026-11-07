/* plate_image.c -- a license plate's picture from its text (plate_image.h).
 *
 * The letters are drawn from strokes: each a few lines on an 8 x 16 grid,
 * drawn as thick round-ended lines with soft edges (the distance from the
 * nearest line), slanted a little and condensed as the game's own plate
 * lettering is.  The game's plate (LICENSE_PLATE, 128x64 DXT1) holds
 * "UNDRGND 2" in raised black letters (a white edge up and to the left of
 * each stroke, a shadow down and to the right) over a light grey that grows
 * lighter from top to bottom: its rows 19-49 between columns 8 and 122 (the
 * letters, their lit edges and their shadow) are painted over, each row in
 * the grey of the blank between "UNDRGND" and "2" in that row, and our text
 * goes in rows 20-47, raised as the game's. */
#include "plate_image.h"
#include <math.h>
#include <string.h>

static int text_ok(const char *t, size_t n){
    if (n < 1 || n > PLATE_TEXT_MAX) return 0;
    for (size_t i = 0; i < n; i++) if (!((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9'))) return 0;
    return 1;
}

/* each level after the first: the average of the 2x2 below it */
static void make_mips(uint8_t *px, int w, int h, int mips){
    uint8_t *src = px;
    for (int m = 1; m < mips; m++){
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        uint8_t *dst = src + (size_t)w * (size_t)h * 4;
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int ch = 0; ch < 4; ch++){
            int x0 = 2 * x < w ? 2 * x : w - 1, x1 = 2 * x + 1 < w ? 2 * x + 1 : w - 1;
            int y0 = 2 * y < h ? 2 * y : h - 1, y1 = 2 * y + 1 < h ? 2 * y + 1 : h - 1;
            unsigned sum = src[((size_t)y0 * (size_t)w + (size_t)x0) * 4 + (size_t)ch] + src[((size_t)y0 * (size_t)w + (size_t)x1) * 4 + (size_t)ch]
                         + src[((size_t)y1 * (size_t)w + (size_t)x0) * 4 + (size_t)ch] + src[((size_t)y1 * (size_t)w + (size_t)x1) * 4 + (size_t)ch];
            dst[((size_t)y * (size_t)nw + (size_t)x) * 4 + (size_t)ch] = (uint8_t)((sum + 2) / 4);
        }
        src = dst; w = nw; h = nh;
    }
}

/* ---- DXT1 ---------------------------------------------------------------------------------------------------------- */
static void rgb565(uint16_t v, int *r, int *g, int *b){
    *r = (v >> 11 & 31) * 255 / 31; *g = (v >> 5 & 63) * 255 / 63; *b = (v & 31) * 255 / 31;
}
void plate_dxt1(const uint8_t *src, int w, int h, uint8_t *bgra){
    for (int by = 0; by < h / 4; by++) for (int bx = 0; bx < w / 4; bx++, src += 8){
        uint16_t c0 = (uint16_t)(src[0] | src[1] << 8), c1 = (uint16_t)(src[2] | src[3] << 8);
        uint32_t bits = (uint32_t)src[4] | (uint32_t)src[5] << 8 | (uint32_t)src[6] << 16 | (uint32_t)src[7] << 24;
        int r[4], g[4], b[4], a[4] = { 255, 255, 255, 255 };
        rgb565(c0, &r[0], &g[0], &b[0]); rgb565(c1, &r[1], &g[1], &b[1]);
        if (c0 > c1){
            r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
            r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
        } else {
            r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
            r[3] = g[3] = b[3] = 0; a[3] = 0;
        }
        for (int i = 0; i < 16; i++){
            int k = (int)(bits >> (2 * i) & 3u), x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
            uint8_t *p = bgra + ((size_t)y * (size_t)w + (size_t)x) * 4;
            p[0] = (uint8_t)b[k]; p[1] = (uint8_t)g[k]; p[2] = (uint8_t)r[k]; p[3] = (uint8_t)a[k];
        }
    }
}

/* ---- the letters --------------------------------------------------------------------------------------------- */
/* Each letter's lines on an 8 x 16 grid (x right, y down), the points of one
 * line after another, '|' between lines.  0 is narrower than O, 1 has a foot,
 * I has bars at both ends: they read apart on a plate. */
static const char *const strokes[36] = {
    "3,0 5,0 7,2 7,14 5,16 3,16 1,14 1,2 3,0",                                /* 0 */
    "1,3 4,0 4,16 | 1,16 7,16",                                               /* 1 */
    "0,3 0,2 2,0 6,0 8,2 8,6 0,16 8,16",                                      /* 2 */
    "0,0 8,0 3,7 6,7 8,9 8,14 6,16 2,16 0,14",                                /* 3 */
    "6,16 6,0 0,11 8,11",                                                     /* 4 */
    "8,0 0,0 0,7 6,7 8,9 8,14 6,16 2,16 0,14",                                /* 5 */
    "8,2 6,0 2,0 0,2 0,14 2,16 6,16 8,14 8,9 6,7 0,7",                        /* 6 */
    "0,0 8,0 8,2 3,16",                                                       /* 7 */
    "2,0 6,0 8,2 8,5 6,7 2,7 0,5 0,2 2,0 | 2,7 0,9 0,14 2,16 6,16 8,14 8,9 6,7",   /* 8 */
    "0,14 2,16 6,16 8,14 8,2 6,0 2,0 0,2 0,7 2,9 8,9",                        /* 9 */
    "0,16 0,3 3,0 5,0 8,3 8,16 | 0,9 8,9",                                    /* A */
    "0,16 0,0 6,0 8,2 8,5 6,7 0,7 | 6,7 8,9 8,14 6,16 0,16",                  /* B */
    "8,2 6,0 2,0 0,2 0,14 2,16 6,16 8,14",                                    /* C */
    "0,0 5,0 8,3 8,13 5,16 0,16 0,0",                                         /* D */
    "8,0 0,0 0,16 8,16 | 0,8 6,8",                                            /* E */
    "8,0 0,0 0,16 | 0,8 6,8",                                                 /* F */
    "8,2.5 6.5,0 2,0 0,2 0,14 2,16 6,16 8,14 8,9 4.5,9",                     /* G */
    "0,0 0,16 | 8,0 8,16 | 0,8 8,8",                                          /* H */
    "4,0 4,16 | 1,0 7,0 | 1,16 7,16",                                         /* I */
    "8,0 8,14 6,16 2,16 0,14 0,12",                                           /* J */
    "0,0 0,16 | 8,0 0,10 | 2.5,7 8,16",                                       /* K */
    "0,0 0,16 8,16",                                                          /* L */
    "0,16 0,0 4,9 8,0 8,16",                                                  /* M */
    "0,16 0,0 8,16 8,0",                                                      /* N */
    "3,0 5,0 8,3 8,13 5,16 3,16 0,13 0,3 3,0",                                /* O */
    "0,16 0,0 6,0 8,2 8,7 6,9 0,9",                                           /* P */
    "3,0 5,0 8,3 8,13 5,16 3,16 0,13 0,3 3,0 | 4.5,11 8,16",                  /* Q */
    "0,16 0,0 6,0 8,2 8,6 6,8 0,8 | 4,8 8,16",                                /* R */
    "8,2 6,0 2,0 0,2 0,6 2,8 6,8 8,10 8,14 6,16 2,16 0,14",                   /* S */
    "0,0 8,0 | 4,0 4,16",                                                     /* T */
    "0,0 0,14 2,16 6,16 8,14 8,0",                                            /* U */
    "0,0 4,16 8,0",                                                           /* V */
    "0,0 1.5,16 4,6 6.5,16 8,0",                                              /* W */
    "0,0 8,16 | 8,0 0,16",                                                    /* X */
    "0,0 4,8 8,0 | 4,8 4,16",                                                 /* Y */
    "0,0 8,0 0,16 8,16",                                                      /* Z */
};
#define GRID_W      8.f
#define GRID_H      16.f
#define CELL_W      0.5f              /* a letter's width, of its height */
#define CELL_GAP    0.1f              /* the gap after it, of its height */
#define STROKE      0.12f             /* the lines' thickness, of the height */
#define SLANT       0.1f              /* to the right, per pixel up */
#define SEG_MAX     16

typedef struct { float ax, ay, bx, by; } Seg;

static float num(const char **s){
    float v = 0.f, f = 0.f;
    while (**s >= '0' && **s <= '9'){ v = v * 10.f + (float)(**s - '0'); (*s)++; }
    if (**s == '.'){ (*s)++; for (float d = 0.1f; **s >= '0' && **s <= '9'; d *= 0.1f){ f += d * (float)(**s - '0'); (*s)++; } }
    return v + f;
}
/* a letter's lines, laid out in the box x0.. (its left), y0..y0+h: the grid's corners inset by the line's half
 * thickness r, slanted about the letter's middle */
static int letter(char ch, float x0, float y0, float w, float h, float r, Seg *out){
    const char *s = strokes[ch <= '9' ? ch - '0' : ch - 'A' + 10];
    int n = 0, have = 0; float px = 0.f, py = 0.f;
    while (*s && n < SEG_MAX){
        if (*s == ' '){ s++; continue; }
        if (*s == '|'){ have = 0; s++; continue; }
        float gx = num(&s); if (*s == ',') s++; float gy = num(&s);
        float y = y0 + r + gy / GRID_H * (h - 2.f * r);
        float x = x0 + r + gx / GRID_W * (w - 2.f * r) + SLANT * (y0 + h * 0.5f - y);
        if (have){ out[n].ax = px; out[n].ay = py; out[n].bx = x; out[n].by = y; n++; }
        px = x; py = y; have = 1;
    }
    return n;
}
static float seg_dist(const Seg *g, float x, float y){
    float dx = g->bx - g->ax, dy = g->by - g->ay, l = dx * dx + dy * dy;
    float t = l > 0.f ? ((x - g->ax) * dx + (y - g->ay) * dy) / l : 0.f;
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    float ex = g->ax + t * dx - x, ey = g->ay + t * dy - y;
    return sqrtf(ex * ex + ey * ey);
}
/* the text into px (w x h BGRA) in the box bx0..bx1 x by0..by1 (pixels, the ends excluded), in colour (b, g, r) at
 * alpha a, moved by (ox, oy) */
static void draw_text(uint8_t *px, int w, int h, const char *t, size_t n, float bx0, float by0, float bx1, float by1,
                      float ox, float oy, const uint8_t bgr[3], float a){
    float th = by1 - by0, cw = th * CELL_W, gap = th * CELL_GAP, r = th * STROKE * 0.5f;
    float total = (float)n * cw + (float)(n - 1) * gap + SLANT * th;
    if (total > bx1 - bx0){ float k = (bx1 - bx0) / total; cw *= k; gap *= k; total = bx1 - bx0; }   /* condensed */
    if (r > cw * 0.2f) r = cw * 0.2f;
    float x = bx0 + (bx1 - bx0 - total) * 0.5f + SLANT * th * 0.5f + ox;
    for (size_t k = 0; k < n; k++, x += cw + gap){
        Seg seg[SEG_MAX];
        int ns = letter(t[k], x, by0 + oy, cw, th, r, seg);
        int xa = (int)floorf(x - SLANT * th - 2.f), xb = (int)ceilf(x + cw + SLANT * th + 2.f);
        int ya = (int)floorf(by0 + oy - 1.f), yb = (int)ceilf(by1 + oy + 1.f);
        for (int py = ya < 0 ? 0 : ya; py < yb && py < h; py++) for (int qx = xa < 0 ? 0 : xa; qx < xb && qx < w; qx++){
            float d = 1e9f;
            for (int i = 0; i < ns; i++){ float e = seg_dist(&seg[i], (float)qx + 0.5f, (float)py + 0.5f); if (e < d) d = e; }
            float cover = r + 0.5f - d;                                /* soft edges: a pixel's worth */
            if (cover <= 0.f) continue;
            if (cover > 1.f) cover = 1.f;
            cover *= a;
            uint8_t *p = px + ((size_t)py * (size_t)w + (size_t)qx) * 4;
            for (int ch = 0; ch < 3; ch++) p[ch] = (uint8_t)((float)p[ch] + ((float)bgr[ch] - (float)p[ch]) * cover + 0.5f);
        }
    }
}

/* the game's plate, its number painted over, or a plain grey one with a dark rim */
#define BAND_X0     8                 /* the number, its lit edges and its shadow on the game's plate: columns 8-122, */
#define BAND_X1     123               /* rows 19-49 */
#define BAND_Y0     19
#define BAND_Y1     50
#define GAP_X0      107               /* the blank between "UNDRGND" and "2": the plate's own grey, row by row */
#define GAP_X1      111
#define TEXT_Y0     20                /* our text: rows 20-47, columns 9-121, as the game's */
#define TEXT_Y1     48
#define TEXT_X0     9
#define TEXT_X1     122
static void background(const uint8_t *stock, uint8_t *px){
    if (!stock){
        for (int y = 0; y < PLATE_H; y++) for (int x = 0; x < PLATE_W; x++){
            uint8_t *p = px + ((size_t)y * PLATE_W + (size_t)x) * 4;
            int rim = x < 4 || y < 4 || x >= PLATE_W - 4 || y >= PLATE_H - 4;
            p[0] = p[1] = p[2] = rim ? 66 : 200; p[3] = 255;
        }
        return;
    }
    memcpy(px, stock, PLATE_W * PLATE_H * 4);
    for (int y = BAND_Y0; y < BAND_Y1; y++){
        uint8_t *row = px + (size_t)y * PLATE_W * 4;
        unsigned sum[3] = { 0, 0, 0 };
        for (int x = GAP_X0; x < GAP_X1; x++) for (int ch = 0; ch < 3; ch++) sum[ch] += row[x * 4 + ch];
        for (int x = BAND_X0; x < BAND_X1; x++) for (int ch = 0; ch < 3; ch++)
            row[x * 4 + ch] = (uint8_t)((sum[ch] + (GAP_X1 - GAP_X0) / 2) / (GAP_X1 - GAP_X0));
    }
}

int plate_image(const char *text, size_t n, const uint8_t *stock, uint8_t *out){
    if (!text_ok(text, n)) return 0;
    /* raised letters as the game's: a shadow down and to the right, a lit edge up and to the left, the black */
    static const uint8_t ink[3] = { 10, 10, 10 }, light[3] = { 247, 247, 247 }, shade[3] = { 90, 90, 90 };
    background(stock, out);
    draw_text(out, PLATE_W, PLATE_H, text, n, TEXT_X0, TEXT_Y0, TEXT_X1, TEXT_Y1, 1.f, 1.f, shade, 0.5f);
    draw_text(out, PLATE_W, PLATE_H, text, n, TEXT_X0, TEXT_Y0, TEXT_X1, TEXT_Y1, -1.f, -1.f, light, 0.9f);
    draw_text(out, PLATE_W, PLATE_H, text, n, TEXT_X0, TEXT_Y0, TEXT_X1, TEXT_Y1, 0.f, 0.f, ink, 1.f);
    make_mips(out, PLATE_W, PLATE_H, PLATE_MIPS);
    return 1;
}
