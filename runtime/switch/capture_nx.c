/* capture_nx.c -- with the file /switch/nfsu2/capture (empty), the picture the game draws is saved as /switch/nfsu2/capture.png
 * every few seconds (the latest only), so a test in yuzu or on a console without a screen to look at can be seen: the GL renderer
 * reads the window back every NFSU2_NX_SHOTS frames (gpu_gl.c glr_want_shots) and capture_nx_poll, in the app's loop, writes it as
 * a PNG (RGB, stored deflate blocks: no compressor needed, 2.7 MB). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const uint32_t *gpu_native_shot(unsigned *w, unsigned *h, unsigned *seq);

static uint32_t g_crct[256];
static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n){
    if (!g_crct[1]) for (uint32_t i = 0; i < 256; i++){ uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; g_crct[i] = v; }
    c = ~c; while (n--) c = g_crct[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v){ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n, uint32_t *crc_in){
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    uint32_t c = crc32_upd(0, (const uint8_t *)type, 4); if (n) { c = crc32_upd(c, d, n); fwrite(d, 1, n, f); }
    uint8_t t[4]; be32(t, crc_in ? *crc_in : c); fwrite(t, 1, 4, f);
}

static int write_png(const char *path, const uint32_t *px, unsigned w, unsigned h){
    size_t row = 1 + (size_t)w * 3, raw = row * h;
    uint8_t *z = malloc(2 + raw + (raw / 65535 + 2) * 5 + 4); if (!z) return -1;
    size_t zp = 0; z[zp++] = 0x78; z[zp++] = 0x01;
    uint32_t a = 1, b = 0;                                   /* adler32 of the raw scanlines, made while the blocks are cut */
    size_t done = 0; uint8_t *line = malloc(row); if (!line){ free(z); return -1; }
    /* the raw data is cut in stored blocks of up to 65535 bytes; build it a scanline at a time into a small window */
    uint8_t *win = malloc(65535 + row); size_t wn = 0; if (!win){ free(z); free(line); return -1; }
    for (unsigned y = 0; y < h; y++){
        line[0] = 0;
        for (unsigned x = 0; x < w; x++){ uint32_t p = px[(size_t)y * w + x]; line[1 + 3 * x] = (p >> 16) & 255; line[2 + 3 * x] = (p >> 8) & 255; line[3 + 3 * x] = p & 255; }
        memcpy(win + wn, line, row); wn += row;
        for (size_t i = 0; i < row; i++){ a = (a + line[i]) % 65521u; b = (b + a) % 65521u; }
        while (wn >= 65535 || (y == h - 1 && wn)){
            size_t n = wn > 65535 ? 65535 : wn; int last = (done + n == raw);
            z[zp++] = last ? 1 : 0; z[zp++] = n & 255; z[zp++] = n >> 8; z[zp++] = ~n & 255; z[zp++] = (~n >> 8) & 255;
            memcpy(z + zp, win, n); zp += n; done += n; memmove(win, win + n, wn - n); wn -= n;
        }
    }
    be32(z + zp, (b << 16) | a); zp += 4;
    char tmp[200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f){ free(z); free(line); free(win); return -1; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A }; fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; be32(ihdr, w); be32(ihdr + 4, h); ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13, NULL); chunk(f, "IDAT", z, (uint32_t)zp, NULL); chunk(f, "IEND", NULL, 0, NULL);
    int ok = fclose(f) == 0;
    free(z); free(line); free(win);
    if (!ok) return -1;
    unlink(path); return rename(tmp, path);                    /* FAT: no rename over an existing file */
}

void capture_nx_poll(void){
    static unsigned last_seq; static time_t last_t; static int on = -1;
    if (on < 0) on = getenv("NFSU2_NX_SHOTS") != NULL;
    if (!on) return;
    time_t now = time(NULL); if (now - last_t < 3) return;
    unsigned w = 0, h = 0, seq = 0; const uint32_t *shot = gpu_native_shot(&w, &h, &seq);
    if (!shot || !w || !h || seq == last_seq) return;
    last_seq = seq; last_t = now;
    write_png("/switch/nfsu2/capture.png", shot, w, h);
}
