/* host_linux.c -- the Switch benchmark's host on Linux (x86-64, glibc): the same headless run as nx_main.c + nx_host.c, for
 * a baseline on a PC and to debug without the console.  Built by runtime/switch/build-linux.sh (the runtime is compiled
 * as for the Switch: -D__SWITCH__ -include switch_plat.h, so the memory model and the platform branches are the same).
 *   out/linux/nfsu2_bench <SPEED2.EXE> <input script> [seconds] */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

int nfsu2_game_main(const char *exe);
const uint32_t *gpu_native_shot(unsigned *w, unsigned *h, unsigned *seq);
void gpu_native_start(void);

/* ---- PNG writer (stored deflate blocks, no compression) for the slideshow's pictures ---- */
static uint32_t crc_table[256];
static void crc_init(void){ for (uint32_t n = 0; n < 256; n++){ uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; } }
static uint32_t crc_upd(uint32_t c, const uint8_t *b, size_t n){ for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8); return c; }
static void put32(uint8_t *p, uint32_t v){ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n){
    uint8_t h[8]; put32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f);
    uint32_t c = crc_upd(0xffffffffu, (const uint8_t *)type, 4); if (n) { c = crc_upd(c, d, n); fwrite(d, 1, n, f); }
    uint8_t t[4]; put32(t, ~c); fwrite(t, 1, 4, f);
}
static void write_png(const char *path, const uint32_t *argb, unsigned w, unsigned h){
    crc_init();
    size_t rowb = 1 + (size_t)w * 3, rawn = rowb * h; uint8_t *raw = malloc(rawn);
    for (unsigned y = 0; y < h; y++){ uint8_t *r = raw + y * rowb; *r++ = 0; for (unsigned x = 0; x < w; x++){ uint32_t p = argb[(size_t)y * w + x]; *r++ = p >> 16; *r++ = p >> 8; *r++ = p; } }
    size_t nblk = (rawn + 65534) / 65535, zn = 2 + rawn + nblk * 5 + 4; uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78; *q++ = 1;
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < rawn; off += 65535){
        size_t n = rawn - off < 65535 ? rawn - off : 65535;
        *q++ = off + n >= rawn; *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = (~n) >> 8; memcpy(q, raw + off, n); q += n;
        for (size_t i = 0; i < n; i++){ a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
    }
    put32(q, b << 16 | a); q += 4;
    FILE *f = fopen(path, "wb"); if (!f) { free(raw); free(z); return; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; put32(ih, w); put32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)(q - z)); chunk(f, "IEND", NULL, 0); fclose(f); free(raw); free(z);
}
static void *saver(void *dir){
    unsigned last = 0;
    for (;;){
        usleep(300000);
        unsigned w, h, seq; const uint32_t *p = gpu_native_shot(&w, &h, &seq);
        if (p && seq != last){ last = seq; char path[600]; snprintf(path, sizeof path, "%s/shot_%03u.png", (char *)dir, seq); write_png(path, p, w, h); }
    }
    return NULL;
}

/* the OpenGL renderer draws into an off-screen surface here */
void *nfsu2_native_window(void){ return NULL; }

double nfsu2_now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static __thread double tl_blocked_ms;
double nfsu2_blocked_ms(void){ return tl_blocked_ms; }
int nfsu2_wait32(int *p, int expected, long long timeout_ns){
    double t0 = nfsu2_now_ms();
    struct timespec ts = { (time_t)(timeout_ns / 1000000000ll), (long)(timeout_ns % 1000000000ll) };
    long r = syscall(SYS_futex, p, FUTEX_WAIT_PRIVATE, expected, timeout_ns < 0 ? NULL : &ts, NULL, 0);
    tl_blocked_ms += nfsu2_now_ms() - t0;
    return (int)r;
}
int nfsu2_notify(int *p, unsigned count){
    return (int)syscall(SYS_futex, p, FUTEX_WAKE_PRIVATE, count > INT_MAX ? INT_MAX : (int)count, NULL, NULL, 0);
}
static void *g_kind_ptr[16]; static uint32_t g_kind_size[16];
int nfsu2_register(int kind, void *ptr, uint32_t size){
    if (kind < 0 || kind >= 16) return -1;
    g_kind_ptr[kind] = ptr; g_kind_size[kind] = size; return 0;
}
void *nfsu2_host_mailbox(int kind, uint32_t *size){
    if (kind < 0 || kind >= 16) return 0;
    if (size) *size = g_kind_size[kind];
    return g_kind_ptr[kind];
}
int nfsu2_stack_frames(void){ return 0; }
int nfsu2_movie_open(int w, int h){ (void)w; (void)h; return -1; }
int nfsu2_movie_decode(int hd, uint32_t p, uint32_t l, uint32_t y, uint32_t u, uint32_t v, int ys, int uvs){
    (void)hd; (void)p; (void)l; (void)y; (void)u; (void)v; (void)ys; (void)uvs; return -1; }
int nfsu2_movie_close(int handle){ (void)handle; return 0; }
void metal_request_click(double x, double y){ (void)x; (void)y; }
void input_web_frame(void){ }
float input_view_roll(void){ return 0.0f; }
void audio_test_stall(int ms){ (void)ms; }

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: %s SPEED2.EXE input-script [seconds]\n", argv[0]); return 2; }
    char save[512]; snprintf(save, sizeof save, "%s/nfsu2-linux-save", getenv("HOME") ? getenv("HOME") : "/tmp");
    char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", save); if (system(cmd)) return 1;
    setenv("NFSU2_SAVEDIR", save, 1);
    setenv("NFSU2_STATUS_EVERY", "120", 1);
    setenv("NFSU2_INPUT_SCRIPT", argv[2], 1);
    setenv("NFSU2_TIMEOUT", argc > 3 ? argv[3] : "180", 1);
    setenv("NFSU2_RENDERER", "webgpu", 1);
    setenv("NFSU2_SKIP3D", "1", 0);   /* (an explicit 0 in the environment wins, for comparisons) */
    setenv("GPUVS", "0", 0);          /* the game runs its vertex shaders on the CPU: the rasterizer gets finished vertices */
    static char shots[600]; snprintf(shots, sizeof shots, "%s/nfsu2-linux-shots", getenv("HOME") ? getenv("HOME") : "/tmp");
    snprintf(cmd, sizeof cmd, "rm -rf '%s'; mkdir -p '%s'", shots, shots); if (system(cmd)) return 1;
    pthread_t th; pthread_create(&th, NULL, saver, shots);
    gpu_native_start();
    return nfsu2_game_main(argv[1]);
}

/* the console pins threads to cores; here nothing to do */
void nfsu2_pin(unsigned mask){ (void)mask; }
