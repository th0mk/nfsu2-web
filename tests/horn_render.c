/* horn_render.c -- one horn of runtime/horn.c rendered offline, for
 * web/test/horn-preview.test.mjs (which compares it with the start page's
 * port, web/src/app/horn-preview.ts):
 *   horn_render <kind> <held s> <rate> <out.f32>
 * the player's own horn (in the middle, at its own level, at the kind's own
 * pitch) held that long and let go, through horn.c's game-thread calls and
 * its mixer in blocks of 10 ms, as the game drives them; the left channel
 * until the voice is silent, as raw float32.  A clock of its own (horn.c
 * times its voices by the monotonic clock): every block is 10 ms later. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint64_t g_fake_ns = 1000000000u;
static int fake_clock_gettime(clockid_t id, struct timespec *ts){
    (void)id; ts->tv_sec = (time_t)(g_fake_ns / 1000000000u); ts->tv_nsec = (long)(g_fake_ns % 1000000000u); return 0;
}
#define clock_gettime(a, b) fake_clock_gettime((a), (b))
#include "kbd.h"
#include "gamepad.h"
int kbd_key_down(unsigned dik){ (void)dik; return 0; }
int gamepad_read(PadState *s){ (void)s; return 0; }
float horn_game_master(void){ return 1.f; }          /* the game's Master Volume: full */
#include "horn.c"

int main(int argc, char **argv){
    if (argc != 5){ fprintf(stderr, "usage: horn_render <kind> <held s> <rate> <out.f32>\n"); return 2; }
    int kind = -1;
    for (int k = 0; k < HORN_KINDS; k++) if (!strcmp(argv[1], horn_name(k))) kind = k;
    double held = atof(argv[2]);
    unsigned rate = (unsigned)atoi(argv[3]), blk = rate / 100u;
    if (kind < 0 || !(held > 0) || rate < 8000u){ fprintf(stderr, "horn_render: bad arguments\n"); return 2; }
    char name[16] = "";                                  /* a name whose pitch is the kind's own */
    for (int i = 0; i < 1000; i++){ snprintf(name, sizeof name, "p%d", i); if (detune(fnv1a(name)) == 1.f) break; }
    FILE *f = fopen(argv[4], "wb");
    if (!f){ perror(argv[4]); return 1; }
    float *st = calloc(2u * blk, sizeof *st), *mono = calloc(blk, sizeof *mono);
    for (unsigned b = 0; b < 60u * 100u; b++){
        int on = (double)b * blk < held * rate;
        horn_begin(0, 0, 0);
        if (on) horn_sound(1, name, kind, 0, 0);
        horn_end();
        memset(st, 0, 2u * blk * sizeof *st);
        horn_mix(st, blk, rate);
        if (!on && !g_v[0].live) break;                  /* the voice is silent */
        for (unsigned i = 0; i < blk; i++) mono[i] = st[2 * i];
        fwrite(mono, sizeof *mono, blk, f);
        g_fake_ns += 10000000u;
    }
    fclose(f);
    return 0;
}
