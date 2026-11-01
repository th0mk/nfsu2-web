/* carsfx.c -- friends' engines, nitrous and hydraulics, heard: the game's
 * own sounds played at each friend's car over the game's sound.  See
 * carsfx.h.
 *
 * Engines: the game gives a car of the World an engine sound only through
 * its sound manager's pools (0x4690E0), made when the world loads: the
 * player's, up to 8 AI racers' (each for a racer record of the race's, so a
 * friend's car of our own, with its own record, gets none), with at most 3
 * AI engine banks loaded for all of them (0x476BD0, 0x463F60: none of them
 * the friend's car's).  So every friend's engine is played here, as the
 * game plays the player's own (CARSFX_PlayerEngine, 0x474030): two GIN
 * files of the car's engine sound, one recorded pulling up through the
 * rpm and one letting off down through it, each a layer at the volume the
 * friend's own game gives it, at the rpm it sounds there (both from the
 * friend's snapshots; the files from the friend's car and engine upgrades,
 * roam_game.c), and roam_game.c silences the game's AI engine on the
 * friend's cars that have one (the free-roam opponents' cars).  How loud: a
 * layer at its volume (0 .. 1) times K_ENG, the game's own output for the
 * player's engine against its voices' volumes (a solo drive, each moment's
 * volumes and rpm logged on the output's time axis: idle and letting off
 * within half a dB, pulling 1 to 3 dB under it, the road and the wind in
 * that), times OTHER, as the nitrous.
 *
 * The game sounds a car's nitrous and hydraulics from its sound state's
 * CARSFX_Nitrous and CARSFX_Hydraulics children.  The player's car has
 * both; a friend's car can have an AI racer's sound state, with the
 * nitrous but no hydraulics, and the game sounds that car's nitrous going
 * on and off but not its letting out (both measured), or none.  So the samples the game plays for the
 * player's car are played here for each friend's car, but for what the
 * game already sounds, as horn.c plays a horn: from SOUND/NOS's banks (EA
 * .abk files: a BNKl bank of EA-XA samples), read and decoded once.
 *
 * Which sample when, as the game plays them for the player's car (measured:
 * each sample of the banks replaced by a tone of its own and the game's
 * output recorded while the car does each thing; the indices are the
 * bank's, as the game counts them; the conditions are the game's, from its
 * updates 0x484F20 and 0x47DD40):
 *   nitrous on, not let out              Nitrous_01 3 once (1.8 s)
 *   ...off, or let out                   3 fades out over 1 s; 2 once (1 s)
 *   let out (0x5ECDB0: on, and under     6 once, and 4 looped as two voices
 *     5 mph or off the throttle)           (pitches 0.99 and 1.02, one to each
 *                                          side) until it stops
 *   a wheel rising (state 2)             Hydraulics_00 3 (a click), then 1 looped
 *                                          (the pump, pitch 1.06)
 *   a wheel lowering (state 3)           3, then 4 looped (the valve)
 *   a wheel stopping (state 0 or 1)      its loop stops; 2 once (a clunk)
 *   the car landing (car +0x408)         7 once, unless one still sounds
 * One-shots vary in pitch from 0.96 to 1.02 as the game's do.  How loud:
 * k_snd's gains, what the game gives each sample for the player's car seen
 * from the chase camera with its default volumes (each decoded sample
 * matched against the game's output: the nitrous within 1 dB; a wheel's
 * sounds, a voice per wheel, under the engine, within a few), times OTHER.
 * The game takes Nitrous_00 instead for some cars (0x465830, from the
 * nitrous child's +0xAC); 01 is the one measured.
 *
 * Where a car is heard from: as horn.c's horns, full within NEAR_M, then
 * falling with the distance until it is gone at FAR_M (the last FADE_M a
 * fade), from the side the car is on, duller the farther it is, its pitch
 * shifted by how fast it and the player come or go (Doppler).
 *
 * The player's own volumes: the game's Options > Audio sliders (G_VOLUMES,
 * floats 0 .. 1; a new profile has 1 for all of these) scale its own sounds
 * in its mix, each by its slider times Master (0x45C210, 0x471970: linear,
 * of 0x7FFF).  The levels above are the game's at those defaults, so a
 * friend's engine follows Engine Volume times Master and its nitrous and
 * hydraulics Sound Effects Volume times Master, read every frame: lowered,
 * they were as loud as ever over the game's own sound.
 *
 * Threads: the game thread decides (carsfx_begin, carsfx_car, carsfx_end)
 * and writes each voice's words under a sequence count; the mixer takes a
 * voice's words only when the count is even and the same before and after,
 * so it never mixes half an update, and smooths every change.  A voice not
 * set for STALE_MS (a parked game) falls silent.  The banks are read on a
 * thread of their own, the first time free roam asks, and the mixer plays
 * nothing until they are in; so are an engine's GIN files, the first time a
 * car has that engine (kept for the session: the game has 43 engine sounds,
 * about half a megabyte each decoded). */
#include "carsfx.h"
#include "cpu.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int fileio_resolve_game(const char *win, char *out, size_t outsz);     /* fileio.c */

#define OTHER     0.56f              /* another car: 5 dB under the player's own, as the game's nitrous for a friend's car right by it */
#define NEAR_M    8.f
#define FAR_M     150.f
#define FADE_M    30.f
#define SOUND_MPS 343.f
#define STALE_MS  250u
#define CARS      48                 /* friends' cars heard (roam_game.c NSLOT) */
#define F_LOOP    1u
#define F_STOP    2u
#define K_ENG     1.1f               /* a GIN layer's gain for its volume (0 .. 1), as the game plays the player's own (measured, below) */
#define ENGINES   48                 /* engine sounds kept (the game's table has 43) */
#define XFADE     96                 /* output samples: a grain that does not follow the last one fades in */
#define ENG_ATT   0.06f              /* s: an engine fades in, */
#define ENG_REL   0.15f              /* ...and out */
#define G_VOLUMES 0x0083aa28u        /* the game's volumes (floats): +0 Master, +0x10 Sound Effects, +0x14 Engine */

enum { S_ENGAGE, S_RELEASE, S_PURGE, S_PURGE_START, S_PUMP, S_CLUNK, S_CLICK, S_VALVE, S_THUMP, S_N };
static const char *const k_bank[2] = { "SOUND\\NOS\\Nitrous_01_MB.abk", "SOUND\\NOS\\Hydraulics_00_MB.abk" };
static const struct { int bank, idx, loop; float gain, pitch; const char *name; } k_snd[S_N] = {
    [S_ENGAGE]      = { 0, 3, 0, 0.055f, 1.f,   "nitrous on" },
    [S_RELEASE]     = { 0, 2, 0, 0.18f,  1.f,   "nitrous off" },
    [S_PURGE]       = { 0, 4, 1, 0.055f, 1.f,   "nitrous let out" },
    [S_PURGE_START] = { 0, 6, 0, 0.10f,  1.f,   "nitrous let out (start)" },
    [S_PUMP]        = { 1, 1, 1, 0.045f, 1.06f, "pump" },
    [S_CLUNK]       = { 1, 2, 0, 0.05f,  1.f,   "clunk" },
    [S_CLICK]       = { 1, 3, 0, 0.07f,  1.f,   "click" },
    [S_VALVE]       = { 1, 4, 1, 0.012f, 1.f,   "valve" },
    [S_THUMP]       = { 1, 7, 0, 0.04f,  1.f,   "landing" },
};

static uint32_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static int g_logged;
#define CLOG(...) do { if (__atomic_add_fetch(&g_logged, 1, __ATOMIC_RELAXED) <= 60) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- the banks ----------------------------------------------------------------- */
typedef struct { int16_t *pcm; uint32_t n, loop0, loop1, rate; } Sample;
static Sample g_smp[S_N];
static int g_ready;                                  /* 1: g_smp complete (release / acquire) */
static int g_load_started;

static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* EA-XA (version 2): frames of 28 samples, 15 bytes (a byte of filter and
 * shift, 14 of nibbles, high first) or, after the byte 0xEE, 61 (the two
 * history samples and 28 of PCM, big-endian) */
static int xa_decode(const uint8_t *d, size_t len, uint32_t n, int16_t *out){
    static const int c1t[4] = { 0, 240, 460, 392 }, c2t[4] = { 0, 0, -208, -220 };
    int h1 = 0, h2 = 0; size_t p = 0; uint32_t k = 0;
    while (k < n){
        if (p >= len) return 0;
        if (d[p] == 0xEE){
            if (p + 61 > len) return 0;
            h1 = (int16_t)(d[p + 1] << 8 | d[p + 2]); h2 = (int16_t)(d[p + 3] << 8 | d[p + 4]);
            for (int i = 0; i < 28 && k < n; i++) out[k++] = (int16_t)(d[p + 5 + 2 * i] << 8 | d[p + 6 + 2 * i]);
            p += 61;
            continue;
        }
        if (p + 15 > len) return 0;
        int c1 = c1t[d[p] >> 4 & 3], c2 = c2t[d[p] >> 4 & 3], sh = (d[p] & 15) + 8;
        for (int i = 0; i < 28 && k < n; i++){
            int nib = i & 1 ? d[p + 1 + i / 2] & 15 : d[p + 1 + i / 2] >> 4;
            int s = (((int32_t)((uint32_t)nib << 28) >> sh) + c1 * h1 + c2 * h2 + 128) >> 8;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            out[k++] = (int16_t)s; h2 = h1; h1 = s;
        }
        p += 15;
    }
    return 1;
}
/* sample idx of a bank: the BNKl chunk's table (from +0x14, each entry
 * relative to itself) points to its "PT" header: tags of a byte, a length
 * byte and a big-endian value (0x82 channels, 0x84 rate, 0x85 samples, 0x86
 * and 0x87 the loop, 0x88 the data, from the chunk), 0xFC..0xFE without
 * one, 0xFF the end */
static int bank_sample(const uint8_t *f, size_t len, int idx, Sample *o){
    size_t b = 0;
    while (b + 4 <= len && memcmp(f + b, "BNKl", 4)) b++;
    if (b + 0x14 > len || idx >= (int)(f[b + 6] | f[b + 7] << 8)) return 0;
    size_t e = b + 0x14 + 4u * (size_t)idx, q;
    if (e + 4 > len || !le32(f + e) || (q = e + le32(f + e)) + 4 > len || memcmp(f + q, "PT\0\0", 4)) return 0;
    uint32_t ch = 1, rate = 22050, n = 0, l0 = 0, l1 = 0, data = 0; int has_data = 0;
    for (q += 4; q < len;){
        uint8_t t = f[q++];
        if (t == 0xFF) break;
        if (t >= 0xFC) continue;
        if (q >= len) return 0;
        uint32_t nl = f[q++], v = 0;
        if (nl > 4 || q + nl > len) return 0;
        for (uint32_t i = 0; i < nl; i++) v = v << 8 | f[q + i];
        q += nl;
        switch (t){
        case 0x82: ch = v; break;
        case 0x84: rate = v; break;
        case 0x85: n = v; break;
        case 0x86: l0 = v; break;
        case 0x87: l1 = v; break;
        case 0x88: data = v; has_data = 1; break;
        }
    }
    if (ch != 1 || !has_data || !n || n > 1u << 22 || rate < 4000 || rate > 96000 || b + data >= len) return 0;
    int16_t *pcm = malloc((size_t)n * sizeof *pcm);
    if (!pcm) return 0;
    if (!xa_decode(f + b + data, len - b - data, n, pcm)){ free(pcm); return 0; }
    o->pcm = pcm; o->n = n; o->rate = rate;
    o->loop0 = l0 < n ? l0 : 0; o->loop1 = l1 > o->loop0 && l1 < n ? l1 + 1 : n;   /* the last loop sample is in it */
    return 1;
}
/* a file of the game's (a Windows path in its folder), whole: malloc'd, or 0 */
static uint8_t *game_file(const char *win, long *len){
    char path[1024]; FILE *fp = 0; uint8_t *f = 0;
    *len = 0;
    if (fileio_resolve_game(win, path, sizeof path) && (fp = fopen(path, "rb"))){
        if (!fseek(fp, 0, SEEK_END) && (*len = ftell(fp)) > 0 && *len < (16l << 20) && !fseek(fp, 0, SEEK_SET) &&
            (f = malloc((size_t)*len)) && fread(f, 1, (size_t)*len, fp) != (size_t)*len){ free(f); f = 0; }
        fclose(fp);
    }
    return f;
}
static void *load_main(void *arg){
    (void)arg;
    uint32_t t0 = now_ms(); int got = 0; double secs = 0;
    for (int b = 0; b < 2; b++){
        long len = 0; uint8_t *f = game_file(k_bank[b], &len);
        if (!f){ CLOG("[carsfx] %s: could not read it\n", k_bank[b]); continue; }
        for (int s = 0; s < S_N; s++){
            if (k_snd[s].bank != b) continue;
            if (bank_sample(f, (size_t)len, k_snd[s].idx, &g_smp[s])){ got++; secs += (double)g_smp[s].n / g_smp[s].rate; }
            else CLOG("[carsfx] %s: no sample %d\n", k_bank[b], k_snd[s].idx);
        }
        free(f);
    }
    CLOG("[carsfx] friends' nitrous and hydraulics: %d of %d samples (%.1f s) in %u ms\n", got, S_N, secs, now_ms() - t0);
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    return 0;
}

/* ---- engines: the GIN files ------------------------------------------------------- */
/* A GIN file ("Gnsu20", SOUND\ENGINE): the engine recorded running up (or,
 * letting off, down) through its rpm, cut into grains of one engine cycle
 * each, and where the recording is at each of npt + 1 rpm evenly from rmin
 * to rmax:
 *   +0x08 float rmin, rmax; +0x10 u32 npt, grains, samples, rate;
 *   +0x20 u32 where[npt + 1], then u32 start[grains + 1] (in samples), then
 *   the samples: EA-XAS frames of 19 bytes for 32 samples, each a frame on
 *   its own: a word with the first two samples' top 12 bits (the filter in
 *   the low 4 bits of the first, the shift in the second's), then 15 bytes
 *   of 4-bit steps, high nibble first.
 * The game plays each layer a grain at a time (0x474030 through the SND
 * library's granular voice), the grain where the recording is at the rpm
 * next, on the file's own scale: rmin + (rpm - 1000) / 9000 x (rmax - rmin)
 * for the sound's rpm (0x46F9A0), and below rmin its first grain lower.
 * Here a grain also plays at the rpm's pitch over its own (within 0.75 ..
 * 1.33), so the rpm moves smoothly between grains, and a grain that does
 * not follow the last fades in over XFADE samples. */
typedef struct { int16_t *pcm; uint32_t n, rate, npt, ngr; float rmin, rmax; uint32_t *where, *start; float *grpm; } Gin;
static struct { char name[2][40]; int state; Gin g[2]; } g_eng[ENGINES];   /* state: 1 loading, 2 ready, 3 not there */
static int g_neng;

static float lef(const uint8_t *p){ uint32_t u = le32(p); float f; memcpy(&f, &u, 4); return f; }
static void gin_free(Gin *g){ free(g->pcm); free(g->where); free(g->start); free(g->grpm); memset(g, 0, sizeof *g); }
/* the rpm (the file's) at a place in its recording, from where[] */
static float gin_rpm_at(const Gin *g, float pos){
    for (uint32_t i = 0; i < g->npt; i++){
        float a = (float)g->where[i], b = (float)g->where[i + 1];
        if (a != b && (pos - a) * (pos - b) <= 0.f)
            return g->rmin + ((float)i + (pos - a) / (b - a)) * (g->rmax - g->rmin) / (float)g->npt;
    }
    return fabsf(pos - (float)g->where[0]) < fabsf(pos - (float)g->where[g->npt]) ? g->rmin : g->rmax;
}
/* the grain to play at rpm r (the file's) */
static uint32_t gin_grain(const Gin *g, float r){
    float t = (r - g->rmin) / (g->rmax - g->rmin) * (float)g->npt;
    if (!(t > 0.f)) t = 0.f;
    if (t > (float)g->npt) t = (float)g->npt;
    uint32_t i = (uint32_t)t < g->npt ? (uint32_t)t : g->npt - 1;
    float pos = (float)g->where[i] + ((float)g->where[i + 1] - (float)g->where[i]) * (t - (float)i);
    uint32_t lo = 0, hi = g->ngr - 1;
    while (lo < hi){ uint32_t m = (lo + hi + 1) / 2; if ((float)g->start[m] <= pos) lo = m; else hi = m - 1; }
    return lo;
}
static int gin_load(const char *name, Gin *g){
    static const int c1t[4] = { 0, 240, 460, 392 }, c2t[4] = { 0, 0, -208, -220 };
    char win[64]; long len = 0;
    memset(g, 0, sizeof *g);
    snprintf(win, sizeof win, "SOUND\\ENGINE\\%s", name);
    uint8_t *f = game_file(win, &len);
    if (!f) return 0;
    int good = 0;
    if (len >= 0x20 && !memcmp(f, "Gnsu20", 6)){
        g->rmin = lef(f + 8); g->rmax = lef(f + 12);
        g->npt = le32(f + 0x10); g->ngr = le32(f + 0x14); g->n = le32(f + 0x18); g->rate = le32(f + 0x1c);
        size_t data = 0x20 + 4u * ((size_t)g->npt + 1) + 4u * ((size_t)g->ngr + 1), frames = ((size_t)g->n + 31) / 32;
        good = g->rmin >= 100.f && g->rmax > g->rmin + 100.f && g->rmax < 20000.f && g->npt >= 1 && g->npt <= 1024 && g->ngr >= 1 &&
               g->ngr <= 16384 && g->n >= 1024 && g->n <= 1u << 22 && g->rate >= 8000 && g->rate <= 96000 && data + 19 * frames <= (size_t)len &&
               (g->where = malloc(4u * (g->npt + 1))) && (g->start = malloc(4u * (g->ngr + 1))) && (g->grpm = malloc(4u * g->ngr)) &&
               (g->pcm = malloc(2u * (size_t)g->n));
        for (uint32_t i = 0; good && i <= g->npt; i++) good = (g->where[i] = le32(f + 0x20 + 4 * i)) < g->n;
        for (uint32_t i = 0; good && i <= g->ngr; i++)
            good = (g->start[i] = le32(f + 0x20 + 4 * (g->npt + 1) + 4 * i)) <= g->n && (!i || g->start[i] > g->start[i - 1]);
        for (size_t fr = 0, k = 0; good && fr < frames; fr++){
            const uint8_t *d = f + data + 19 * fr;
            uint32_t h = le32(d);
            int c1 = c1t[h & 3], c2 = c2t[h & 3], sh = 20 - (int)(h >> 16 & 15);
            int h2 = (int16_t)(h & 0xfff0u), h1 = (int16_t)(h >> 16 & 0xfff0u);
            if (k < g->n) g->pcm[k++] = (int16_t)h2;
            if (k < g->n) g->pcm[k++] = (int16_t)h1;
            for (int i = 0; i < 30 && k < g->n; i++){
                int nib = i & 1 ? d[4 + i / 2] & 15 : d[4 + i / 2] >> 4;
                int s = ((nib > 7 ? nib - 16 : nib) * (1 << sh) + h1 * c1 + h2 * c2 + 128) >> 8;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                g->pcm[k++] = (int16_t)s; h2 = h1; h1 = s;
            }
        }
        for (uint32_t i = 0; good && i < g->ngr; i++) g->grpm[i] = gin_rpm_at(g, 0.5f * ((float)g->start[i] + (float)g->start[i + 1]));
    }
    free(f);
    if (!good) gin_free(g);
    return good;
}
static void *eng_load_main(void *arg){
    int e = (int)(intptr_t)arg;
    uint32_t t0 = now_ms();
    int good = gin_load(g_eng[e].name[0], &g_eng[e].g[0]) && gin_load(g_eng[e].name[1], &g_eng[e].g[1]);
    if (!good){ gin_free(&g_eng[e].g[0]); gin_free(&g_eng[e].g[1]); }
    CLOG("[carsfx] engine %s + %s: %s in %u ms\n", g_eng[e].name[0], g_eng[e].name[1], good ? "read" : "not there", now_ms() - t0);
    __atomic_store_n(&g_eng[e].state, good ? 2 : 3, __ATOMIC_RELEASE);
    return 0;
}
/* game thread: the engine of these GIN files, its index + 1 (being read or
 * not), 0 when it is not there */
static uint32_t engine_of(const char *acc, const char *dcl){
    for (int e = 0; e < g_neng; e++)
        if (!strcmp(g_eng[e].name[0], acc) && !strcmp(g_eng[e].name[1], dcl))
            return __atomic_load_n(&g_eng[e].state, __ATOMIC_ACQUIRE) == 3 ? 0 : (uint32_t)e + 1;
    if (g_neng == ENGINES || strlen(acc) >= sizeof g_eng[0].name[0] || strlen(dcl) >= sizeof g_eng[0].name[1]) return 0;
    int e = g_neng++;
    strcpy(g_eng[e].name[0], acc); strcpy(g_eng[e].name[1], dcl);
    __atomic_store_n(&g_eng[e].state, 1, __ATOMIC_RELEASE);
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, eng_load_main, (void *)(intptr_t)e)) eng_load_main((void *)(intptr_t)e);
    pthread_attr_destroy(&a);
    return (uint32_t)e + 1;
}

/* ---- game thread -> mixer: a voice's words under a sequence count ------------------ */
typedef struct { uint32_t gen, snd, flags, fade_ms, delay, gain, pan, pitch, near, stamp; } VWords;
static struct { uint32_t seq; VWords w; } g_in[CARSFX_VOICES];
static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u){ float f; memcpy(&f, &u, 4); return f; }
#define PUT(f, v) __atomic_store_n(&(f), (v), __ATOMIC_RELAXED)
#define GET(f)    __atomic_load_n(&(f), __ATOMIC_RELAXED)
static void words_put(int k, const VWords *w){
    uint32_t s = g_in[k].seq;                        /* only this thread writes it */
    PUT(g_in[k].seq, s + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    PUT(g_in[k].w.gen, w->gen); PUT(g_in[k].w.snd, w->snd); PUT(g_in[k].w.flags, w->flags); PUT(g_in[k].w.fade_ms, w->fade_ms);
    PUT(g_in[k].w.delay, w->delay); PUT(g_in[k].w.gain, w->gain); PUT(g_in[k].w.pan, w->pan); PUT(g_in[k].w.pitch, w->pitch);
    PUT(g_in[k].w.near, w->near); PUT(g_in[k].w.stamp, w->stamp);
    __atomic_store_n(&g_in[k].seq, s + 2, __ATOMIC_RELEASE);
}
static int words_get(int k, VWords *w){
    uint32_t s = __atomic_load_n(&g_in[k].seq, __ATOMIC_ACQUIRE);
    if (s & 1) return 0;
    w->gen = GET(g_in[k].w.gen); w->snd = GET(g_in[k].w.snd); w->flags = GET(g_in[k].w.flags); w->fade_ms = GET(g_in[k].w.fade_ms);
    w->delay = GET(g_in[k].w.delay); w->gain = GET(g_in[k].w.gain); w->pan = GET(g_in[k].w.pan); w->pitch = GET(g_in[k].w.pitch);
    w->near = GET(g_in[k].w.near); w->stamp = GET(g_in[k].w.stamp);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&g_in[k].seq, __ATOMIC_RELAXED) == s;
}
/* ...and a car's engine's (by the car's place in g_car), the same way:
 * eng 0 is none; rpm on the sound's scale, each layer's gain, where it is
 * heard from (the car's gain, side and Doppler) */
typedef struct { uint32_t gen, eng, rpm, g0, g1, gain, pan, dop, stamp; } EWords;
static struct { uint32_t seq; EWords w; } g_ein[CARS];
#define EW_N ((int)(sizeof(EWords) / 4))
static void ewords_put(int c, const EWords *w){
    const uint32_t *from = (const uint32_t *)w; uint32_t *to = (uint32_t *)&g_ein[c].w, s = g_ein[c].seq;
    PUT(g_ein[c].seq, s + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (int i = 0; i < EW_N; i++) PUT(to[i], from[i]);
    __atomic_store_n(&g_ein[c].seq, s + 2, __ATOMIC_RELEASE);
}
static int ewords_get(int c, EWords *w){
    uint32_t s = __atomic_load_n(&g_ein[c].seq, __ATOMIC_ACQUIRE), *to = (uint32_t *)w, *from = (uint32_t *)&g_ein[c].w;
    if (s & 1) return 0;
    for (int i = 0; i < EW_N; i++) to[i] = GET(from[i]);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&g_ein[c].seq, __ATOMIC_RELAXED) == s;
}

/* ---- the game thread: cars and their voices ---------------------------------------- */
/* a car keeps its sounds by their generations (0: none): a voice whose
 * one-shot has ended may sound for another car by now */
static struct { int used, car; uint32_t gen, snd, flags, fade_ms, delay, end_ms, free_at; float gain, pitch, pan; } g_vo[CARSFX_VOICES];
static struct {
    int used, seen;
    uint32_t who, egen; char name[24];          /* egen: its engine voice's generation (a new car: a new one) */
    carsfx_state st;
    uint32_t engage, purge[2], wheel[4], thump;      /* its sounds that it may stop */
    float gain, pan, doppler;                        /* where it is heard from, this frame */
} g_car[CARS];
static struct { int ok; float p[3], l[2], v[3]; } g_lis;
static uint32_t g_gen, g_rnd = 0x9e3779b9u, g_now;
static float g_vol_eng = 1.f, g_vol_sfx = 1.f;      /* the player's Engine and Sound Effects volumes, times Master */
/* one of the game's volumes (G_VOLUMES + off), 1 while it is not there or not one */
static float game_volume(uint32_t off){
    uint32_t a = G_VOLUMES + off;
    if (!mem_is_committed(a) || !mem_is_committed(a + 3u)) return 1.f;
    float v = MRF32(a);
    return v >= 0.f && v <= 1.f ? v : 1.f;
}
static float rnd_pitch(void){ g_rnd = g_rnd * 1664525u + 1013904223u; return 0.96f + 0.06f * (float)(g_rnd >> 8) / 16777216.f; }

static void voice_write(int k){
    const int c = g_vo[k].car;
    float pan = g_car[c].pan + g_vo[k].pan;
    VWords w = { g_vo[k].gen, g_vo[k].snd, g_vo[k].flags, g_vo[k].fade_ms, g_vo[k].delay,
                 fbits(g_vo[k].gain * g_car[c].gain * g_vol_sfx), fbits(pan < -1.f ? -1.f : pan > 1.f ? 1.f : pan), fbits(g_vo[k].pitch * g_car[c].doppler),
                 fbits(g_car[c].gain), g_now };
    words_put(k, &w);
}
static int voice_of(uint32_t gen){
    for (int k = 0; gen && k < CARSFX_VOICES; k++) if (g_vo[k].used && g_vo[k].gen == gen) return k;
    return -1;
}
/* a new sound of car c in a free voice (one let go has faded out first),
 * its gain and pitch on top of the sample's own, after delay frames of
 * silence; its generation, 0 when it does not sound */
static uint32_t voice_start(int c, int snd, float gain, float pitch, uint32_t delay){
    const Sample *S = &g_smp[snd];
    if (!S->n || g_car[c].gain < 0.003f) return 0;   /* not in the bank, or out of hearing */
    int k = 0;
    while (k < CARSFX_VOICES && (g_vo[k].used || (int32_t)(g_now - g_vo[k].free_at) < 0)) k++;
    if (k == CARSFX_VOICES) return 0;
    if (!++g_gen) ++g_gen;
    g_vo[k].used = 1; g_vo[k].car = c; g_vo[k].gen = g_gen; g_vo[k].snd = (uint32_t)snd;
    g_vo[k].flags = k_snd[snd].loop ? F_LOOP : 0; g_vo[k].fade_ms = 0; g_vo[k].delay = delay;
    g_vo[k].gain = k_snd[snd].gain * OTHER * gain; g_vo[k].pitch = k_snd[snd].pitch * pitch; g_vo[k].pan = 0.f;
    /* a one-shot ends by itself: let it go after its length at the lowest pitch (Doppler 0.5) */
    g_vo[k].end_ms = k_snd[snd].loop ? 0 : g_now + (uint32_t)(2000.0 * S->n / S->rate / g_vo[k].pitch) + delay / 44u + 100u;
    voice_write(k);
    CLOG("[carsfx] %s: %s, %.0f%% (voice %d)\n", g_car[c].name, k_snd[snd].name, 100.0 * g_car[c].gain, k);
    return g_gen;
}
static void voice_pan(uint32_t gen, float pan){                /* to one side of where the car is heard from */
    int k = voice_of(gen);
    if (k >= 0){ g_vo[k].pan = pan; voice_write(k); }
}
static void voice_stop(uint32_t *gen, uint32_t fade_ms){
    int k = voice_of(*gen);
    *gen = 0;
    if (k < 0) return;
    g_vo[k].flags |= F_STOP; g_vo[k].fade_ms = fade_ms ? fade_ms : 1u;
    voice_write(k);
    g_vo[k].used = 0; g_vo[k].free_at = g_now + fade_ms + STALE_MS;
}

void carsfx_begin(const float *pos, const float *fwd, const float *vel){
    g_now = now_ms();
    float master = game_volume(0), ve = master * game_volume(0x14u), vs = master * game_volume(0x10u);
    if (ve != g_vol_eng || vs != g_vol_sfx)
        fprintf(stderr, "[carsfx] friends' engines at %.0f %%, their nitrous and hydraulics at %.0f %% (the game's volumes)\n",
                100.0 * ve, 100.0 * vs);
    g_vol_eng = ve; g_vol_sfx = vs;
    if (!g_load_started){
        g_load_started = 1;
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &a, load_main, 0)) load_main(0);
        pthread_attr_destroy(&a);
    }
    g_lis.ok = pos && fwd;
    if (g_lis.ok){
        for (int a = 0; a < 3; a++){ g_lis.p[a] = pos[a]; g_lis.v[a] = vel ? vel[a] : 0.f; }
        float lx = -fwd[1], ly = fwd[0], ln = sqrtf(lx * lx + ly * ly);        /* the camera's left, level (z is up) */
        if (ln > 0.001f){ g_lis.l[0] = lx / ln; g_lis.l[1] = ly / ln; } else { g_lis.l[0] = 0.f; g_lis.l[1] = 1.f; }
    }
    for (int c = 0; c < CARS; c++) g_car[c].seen = 0;
    for (int k = 0; k < CARSFX_VOICES; k++)                  /* one-shots that have ended */
        if (g_vo[k].used && g_vo[k].end_ms && (int32_t)(g_now - g_vo[k].end_ms) >= 0){ g_vo[k].used = 0; g_vo[k].free_at = g_now; }
}

void carsfx_car(uint32_t who, const char *name, const float *p, const float *v, const carsfx_state *st){
    if (!g_lis.ok || !p || !st) return;
    int c = 0;
    while (c < CARS && !(g_car[c].used && g_car[c].who == who)) c++;
    if (c == CARS){
        for (c = 0; c < CARS && g_car[c].used; c++) {}
        if (c == CARS) return;
        memset(&g_car[c], 0, sizeof g_car[c]);
        g_car[c].used = 1; g_car[c].who = who; g_car[c].st.thumps = st->thumps;
        if (!++g_gen) ++g_gen;
        g_car[c].egen = g_gen;
        snprintf(g_car[c].name, sizeof g_car[c].name, "%s", name && *name ? name : "a friend");
    }
    g_car[c].seen = 1;
    /* where it is heard from (horn.c's horn_sound) */
    float d[3] = { p[0] - g_lis.p[0], p[1] - g_lis.p[1], p[2] - g_lis.p[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), g = 0.f, pan = 0.f, dop = 1.f;
    if (dist < FAR_M){
        g = dist > NEAR_M ? NEAR_M / dist : 1.f;
        if (dist > FAR_M - FADE_M) g *= (FAR_M - dist) / FADE_M;
    }
    if (dist > 0.1f){
        float vl = 0.f, vs = 0.f;
        pan = -(d[0] * g_lis.l[0] + d[1] * g_lis.l[1]) / dist;
        for (int a = 0; a < 3; a++){ vl += g_lis.v[a] * d[a] / dist; vs += (v ? v[a] : 0.f) * d[a] / dist; }
        /* c + the player's speed towards the car over c + the car's speed away */
        dop = (SOUND_MPS + vl) / (SOUND_MPS + vs);
        if (!(dop >= 0.5f)) dop = 0.5f;
        if (dop > 2.f) dop = 2.f;
    }
    g_car[c].gain = g; g_car[c].pan = pan; g_car[c].doppler = dop;

    /* its engine: the mixer plays it while it is given (and its GIN files are in) */
    uint32_t e = g > 0.f && st->gin[0] && st->gin[1] && st->eng > 0.f ? engine_of(st->gin[0], st->gin[1]) : 0;
    EWords ew = { g_car[c].egen, e, fbits(st->eng), fbits(K_ENG * OTHER * g_vol_eng * st->eng_gain[0]),
                  fbits(K_ENG * OTHER * g_vol_eng * st->eng_gain[1]),
                  fbits(g), fbits(pan), fbits(dop), g_now };
    ewords_put(c, &ew);
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) != 1) return;

    /* nitrous (0x484F20): on and not let out, the boost (unless the game
     * sounds it); let out, the purge */
    const carsfx_state *o = &g_car[c].st;
    int boost = st->nos && !st->purge && !st->game_boost, was_boost = o->nos && !o->purge && !o->game_boost;
    int purge = st->nos && st->purge, was_purge = o->nos && o->purge;
    if (boost && !was_boost){
        voice_stop(&g_car[c].engage, 60);
        g_car[c].engage = voice_start(c, S_ENGAGE, 1.f, rnd_pitch(), 0);
    } else if (!boost && was_boost){
        voice_stop(&g_car[c].engage, 1000);
        voice_start(c, S_RELEASE, 1.f, rnd_pitch(), 0);
    }
    if (purge && !was_purge){
        voice_start(c, S_PURGE_START, 1.f, rnd_pitch(), 0);
        g_car[c].purge[0] = voice_start(c, S_PURGE, 1.f, 0.99f, 0);
        g_car[c].purge[1] = voice_start(c, S_PURGE, 1.f, 1.017f, 0);
        voice_pan(g_car[c].purge[0], -0.45f); voice_pan(g_car[c].purge[1], 0.45f);
    } else if (!purge && was_purge){
        voice_stop(&g_car[c].purge[0], 120); voice_stop(&g_car[c].purge[1], 120);
    }
    /* hydraulics (0x47DD40): a wheel set rising or lowering clicks and runs
     * its pump or valve until it stops, and stopping clunks; a pair's sounds
     * start a frame apart, as the game's do (it takes a wheel a frame) */
    uint32_t stagger = 0;
    for (int w = 0; w < 4; w++){
        int s = st->hyd >> (2 * w) & 3, was = o->hyd >> (2 * w) & 3;
        if (s == was) continue;
        uint32_t *vw = &g_car[c].wheel[w];
        if (s >= 2){
            voice_stop(vw, 40);
            voice_start(c, S_CLICK, 1.f, rnd_pitch(), stagger);
            *vw = voice_start(c, s == 2 ? S_PUMP : S_VALVE, 1.f, 1.f, stagger);
            stagger += 700;                          /* 16 ms at 44.1 kHz */
        } else if (voice_of(*vw) >= 0){
            voice_stop(vw, 40);
            voice_start(c, S_CLUNK, 1.f, rnd_pitch(), stagger);
            stagger += 700;
        }
    }
    /* a landing, unless the last one still sounds */
    if (st->thumps != o->thumps && voice_of(g_car[c].thump) < 0)
        g_car[c].thump = voice_start(c, S_THUMP, 1.f, rnd_pitch(), 0);
    g_car[c].st = *st;

    for (int k = 0; k < CARSFX_VOICES; k++) if (g_vo[k].used && g_vo[k].car == c) voice_write(k);
}

void carsfx_end(void){
    for (int c = 0; c < CARS; c++){
        if (!g_car[c].used || g_car[c].seen) continue;
        for (int k = 0; k < CARSFX_VOICES; k++){ uint32_t x = g_vo[k].gen; if (g_vo[k].used && g_vo[k].car == c) voice_stop(&x, 200); }
        EWords off = { g_car[c].egen, 0, 0, 0, 0, 0, 0, 0, g_now };
        ewords_put(c, &off);
        g_car[c].used = 0;
    }
}

/* ---- the mixer ----------------------------------------------------------------- */
static struct {
    int live;
    uint32_t gen, snd, delay;
    double pos;                                      /* in the sample, at its own rate */
    float env, gl, gr, pitch, lp;
    VWords last;                                     /* the last words taken whole */
} g_v[CARSFX_VOICES];
/* sample k of S as a voice plays it, full scale 1: a loop wraps into its
 * loop, a one-shot is silent past its ends.  The voices read four of them
 * a frame (Catmull-Rom): the samples run at 22 kHz, and two-point
 * interpolation would take 5.6 dB off at 9.5 kHz, where the purge is. */
static inline float at(const Sample *S, int loop, int64_t k){
    if (loop && k >= (int64_t)S->loop1) k -= (int64_t)(S->loop1 - S->loop0);
    return k < 0 || k >= (int64_t)S->n ? 0.f : (float)S->pcm[k] * (1.f / 32768.f);
}

/* the engines: per car, its two layers a grain at a time (see the GIN files) */
typedef struct { uint32_t grain; int fade; double pos, old; } Layer;   /* its grain, where; a grain fading out */
static struct {
    int live;
    uint32_t gen, eng;                               /* the car's engine voice and the engine it plays */
    float rpm, g[2], dop, gl, gr, lp, env;
    Layer L[2];
    EWords last;
} g_ev[CARS];
static inline float gat(const Gin *G, int64_t k){ return k < 0 || k >= (int64_t)G->n ? 0.f : (float)G->pcm[k] * (1.f / 32768.f); }
static inline float gcr(const Gin *G, double pos){
    int64_t j = (int64_t)pos; float fr = (float)(pos - (double)j);
    float p0 = gat(G, j - 1), p1 = gat(G, j), p2 = gat(G, j + 1), p3 = gat(G, j + 2);
    return p1 + 0.5f * fr * (p2 - p0 + fr * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + fr * (3.f * (p1 - p2) + p3 - p0)));
}
static inline float gin_scale(const Gin *G, float rpm){ return G->rmin + (rpm - 1000.f) * (1.f / 9000.f) * (G->rmax - G->rmin); }
static void engines_mix(float *acc, unsigned frames, float fs, uint32_t now){
    float k_smooth = 1.f - expf(-1.f / (0.020f * fs)), k_rpm = 1.f - expf(-1.f / (0.030f * fs));
    float att = 1.f / (ENG_ATT * fs), rel = 1.f / (ENG_REL * fs);
    for (int c = 0; c < CARS; c++){
        EWords w;
        if (ewords_get(c, &w)) g_ev[c].last = w; else w = g_ev[c].last;
        uint32_t e = w.eng;
        int ready = e && e <= ENGINES && __atomic_load_n(&g_eng[e - 1].state, __ATOMIC_ACQUIRE) == 2;
        int stale = (int32_t)(now - w.stamp) > (int32_t)STALE_MS;
        float trpm = bitsf(w.rpm), tg0 = bitsf(w.g0), tg1 = bitsf(w.g1), gain = bitsf(w.gain), pan = bitsf(w.pan), tdop = bitsf(w.dop);
        float th = (pan + 1.f) * (float)M_PI * 0.25f, tgl = gain * cosf(th) * (float)M_SQRT2, tgr = gain * sinf(th) * (float)M_SQRT2;
        if (!g_ev[c].live){
            if (!ready || stale || !w.gen) continue;
            g_ev[c].live = 1; g_ev[c].gen = w.gen; g_ev[c].eng = e; g_ev[c].env = 0.f; g_ev[c].rpm = trpm; g_ev[c].dop = tdop;
            g_ev[c].g[0] = tg0; g_ev[c].g[1] = tg1; g_ev[c].gl = tgl; g_ev[c].gr = tgr; g_ev[c].lp = 0.f;
            for (int l = 0; l < 2; l++){
                const Gin *G = &g_eng[e - 1].g[l];
                g_ev[c].L[l].grain = gin_grain(G, gin_scale(G, trpm)); g_ev[c].L[l].pos = G->start[g_ev[c].L[l].grain]; g_ev[c].L[l].fade = 0;
            }
        }
        /* another car in its place, another engine, none, or a parked game: it fades out (and a new one starts after) */
        int off = !ready || stale || w.gen != g_ev[c].gen || e != g_ev[c].eng;
        const Gin *Gs = g_eng[g_ev[c].eng - 1].g;
        float k_lp = 0.15f + 0.85f * (gain * 1.7f > 1.f ? 1.f : gain * 1.7f);   /* far cars duller, as the voices */
        float rpm = g_ev[c].rpm, g0 = g_ev[c].g[0], g1 = g_ev[c].g[1], dop = g_ev[c].dop, gl = g_ev[c].gl, gr = g_ev[c].gr, lp = g_ev[c].lp, env = g_ev[c].env;
        for (unsigned i = 0; i < frames; i++){
            rpm += (trpm - rpm) * k_rpm; dop += (tdop - dop) * k_smooth;
            g0 += (tg0 - g0) * k_smooth; g1 += (tg1 - g1) * k_smooth; gl += (tgl - gl) * k_smooth; gr += (tgr - gr) * k_smooth;
            if (off){ env -= rel; if (env <= 0.f){ env = 0.f; g_ev[c].live = 0; break; } }
            else if (env < 1.f){ env += att; if (env > 1.f) env = 1.f; }
            float s = 0.f;
            for (int l = 0; l < 2; l++){
                const Gin *G = &Gs[l];
                Layer *L = &g_ev[c].L[l];
                float r = gin_scale(G, rpm);
                if (L->pos >= (double)G->start[L->grain + 1]){              /* the grain is over: the one at the rpm next */
                    uint32_t ng = gin_grain(G, r);
                    if (ng != L->grain + 1){ L->old = L->pos; L->fade = XFADE; L->pos = G->start[ng]; }
                    L->grain = ng;
                }
                float pitch = r / G->grpm[L->grain];
                pitch = pitch < 0.75f ? 0.75f : pitch > 1.33f ? 1.33f : pitch;
                double step = (double)((float)G->rate / fs * pitch * dop);
                float x = gcr(G, L->pos);
                if (L->fade){ float f = (float)L->fade / (float)XFADE; x = x * (1.f - f) + gcr(G, L->old) * f; L->old += step; L->fade--; }
                L->pos += step;
                s += x * (l ? g1 : g0);
            }
            lp += (s - lp) * k_lp;
            float o = lp * env;
            acc[2 * i] += o * gl; acc[2 * i + 1] += o * gr;
        }
        g_ev[c].rpm = rpm; g_ev[c].g[0] = g0; g_ev[c].g[1] = g1; g_ev[c].dop = dop; g_ev[c].gl = gl; g_ev[c].gr = gr; g_ev[c].lp = lp; g_ev[c].env = env;
    }
}

void carsfx_mix(float *acc, unsigned frames, unsigned rate){
    if (!rate || !frames) return;
    uint32_t now = now_ms();
    engines_mix(acc, frames, (float)rate, now);
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) != 1) return;
    float fs = (float)rate, k_smooth = 1.f - expf(-1.f / (0.020f * fs)), att = 1.f / (0.004f * fs);
    for (int v = 0; v < CARSFX_VOICES; v++){
        VWords w;
        if (words_get(v, &w)) g_v[v].last = w; else w = g_v[v].last;
        if (!w.gen) continue;
        int stale = (int32_t)(now - w.stamp) > (int32_t)STALE_MS, stop = (w.flags & F_STOP) || stale;
        float gain = bitsf(w.gain), pan = bitsf(w.pan), tp = bitsf(w.pitch), near = bitsf(w.near);
        float th = (pan + 1.f) * (float)M_PI * 0.25f;   /* equal power, both sides at gain in the middle */
        float tgl = gain * cosf(th) * (float)M_SQRT2, tgr = gain * sinf(th) * (float)M_SQRT2;
        if (w.gen != g_v[v].gen){                        /* a new sound in this voice */
            g_v[v].gen = w.gen; g_v[v].live = !stop && w.snd < S_N && g_smp[w.snd].n;
            g_v[v].snd = w.snd; g_v[v].delay = w.delay; g_v[v].pos = 0.0;
            g_v[v].env = 0.f; g_v[v].gl = tgl; g_v[v].gr = tgr; g_v[v].pitch = tp; g_v[v].lp = 0.f;
        }
        if (!g_v[v].live) continue;
        const Sample *S = &g_smp[g_v[v].snd];
        int loop = (w.flags & F_LOOP) != 0;
        float rel = 1.f / ((float)(stale ? 50u : w.fade_ms ? w.fade_ms : 1u) * 0.001f * fs);
        /* far cars lose their top: a one-pole low-pass from ~1.5 kHz (160 m) up to none (within 20 m) */
        float k_lp = 0.15f + 0.85f * (near * 1.7f > 1.f ? 1.f : near * 1.7f);
        float base = (float)S->rate / fs;
        double pos = g_v[v].pos; float env = g_v[v].env, gl = g_v[v].gl, gr = g_v[v].gr, pt = g_v[v].pitch, lp = g_v[v].lp;
        uint32_t delay = g_v[v].delay;
        int live = 1;
        for (unsigned i = 0; i < frames; i++){
            if (delay){ delay--; continue; }
            pt += (tp - pt) * k_smooth; gl += (tgl - gl) * k_smooth; gr += (tgr - gr) * k_smooth;
            if (stop){ env -= rel; if (env <= 0.f){ live = 0; break; } }
            else if (env < 1.f){ env += att; if (env > 1.f) env = 1.f; }
            int64_t j = (int64_t)pos; float fr = (float)(pos - (double)j);
            float p0 = at(S, loop, j - 1), p1 = at(S, loop, j), p2 = at(S, loop, j + 1), p3 = at(S, loop, j + 2);
            float s = p1 + 0.5f * fr * (p2 - p0 + fr * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + fr * (3.f * (p1 - p2) + p3 - p0)));
            lp += (s - lp) * k_lp;
            float o = lp * env;
            acc[2 * i] += o * gl; acc[2 * i + 1] += o * gr;
            pos += (double)(base * pt);
            if (loop){ while (pos >= (double)S->loop1) pos -= (double)(S->loop1 - S->loop0); }
            else if (pos >= (double)S->n){ live = 0; break; }
        }
        g_v[v].pos = pos; g_v[v].env = env; g_v[v].gl = gl; g_v[v].gr = gr; g_v[v].pitch = pt; g_v[v].lp = lp; g_v[v].delay = delay;
        g_v[v].live = live;
    }
}
