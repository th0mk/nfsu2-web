/* whine.c -- each local player's gearbox whine, as ropi's Underground 2
 * Transmission Whine has it (UG2WhineGen 1.0.2: NFS Carbon's BMW M3 GTR
 * gearbox whine on Underground 2; source https://github.com/ropi336/UG2WhineGen,
 * "No Rights Reserved").  gearbox_whine = on (NFSU2_GEARBOX_WHINE=1); off by
 * default, the original game: nothing of the game read, nothing loaded,
 * nothing mixed.
 *
 * The mod's sounds are not in the game build: tools/whinepack.py puts the
 * three of its 1.0.2 release in the game folder's MODPACKS/WHINE (16-bit mono
 * WAVs), and the server serves them with the rest of the game.  With the
 * switch on they are read on a thread of their own at the first frame (so
 * only then fetched), and the mixer plays nothing until they are in.  A game
 * folder without them: no whine, and one line in the log.  Each looped from
 * the start over the game's sound:
 *   car_whine.wav          the whine, its pitch rising with the speed
 *   car_whine_2.wav        a second layer at its own pitch, louder with speed
 *   car_whine_reverse.wav  in reverse instead of both
 * What the mod does every 1/60 s (its ProcessWhine thread), here every frame
 * on the game thread with its per-tick steps scaled by the frame's time, for
 * each local player (split screen's 2 to 4, co-op's 2: split_render_limit)
 * a whine of its own:
 *   - the player's car: [PlayersByNumber[i] + 4] (this build keeps the
 *     player table at SPLIT_PLAYERS_INDEX); its speed +0x430 (m/s, times 3.6:
 *     km/h; the engine sound's) and its gear +0x434 (0 reverse, 1 neutral,
 *     2 first ..);
 *   - nothing unless the game is in the world (GameFlowManager [0x8654A4]
 *     6); silent while the world stands (paused, a menu over it, photo mode:
 *     the frame's world step [0x865154] 0), as the mod is while paused;
 *   - the level: the game's Master times Engine Volume (0x83AA28 +0 and
 *     +0x14), reached over about 12 ticks (the mod's fadeSnd), times LEVEL;
 *   - under 1 km/h the whine at pitch 0.3 and the rest silent; else the
 *     whine and reverse layer at 0.3 + 0.7 km/h / 330 + min(0.5
 *     sqrt(km/h / 70), 0.6) + m, m creeping 0.001 a tick towards 0.02 (gear
 *     - 1) as the car speeds up (down while it slows), the second layer at 1;
 *   - volumes: the whine 0.030, the second layer 0.015 km/h / 140, both off
 *     in reverse; the reverse layer 0.050 there only; all times the level;
 *   - an upshift: the whine and the second layer fade out over 0.1 s and
 *     stay silent 0.2 s more (the mod waits on, while the clutch is in, up
 *     to half a second; here the 0.3 s alone), and m jumps to its new gear's
 *     value.
 * Not here: the mod's car list (CarList: the whine on every car, as its
 * default ALL), its Volume (LEVEL instead), its F1 switch and its chimes
 * (the page's setting instead), and its lowering to 0.4 during a phone call
 * or cutscene.
 *
 * LEVEL: the mod plays its sounds through an audio engine of its own beside
 * the game's, both at full scale into the system's mix; the mixers here play
 * the game's sound buffers at their DirectSound volumes and nothing more
 * (audio_web.c, audio.m), so the mod's own gains keep its balance against
 * the game: the whine 11 to 13 dB under the engine alone while driving, lost
 * under it at speed.  Here it plays at 165 % of that.  Measured in a quick
 * race at 60 fps with every volume but Master and Engine at 0
 * (tests/scripts/whine_engine.txt, SNDDUMP): the whine about -32 dBFS (its
 * gains, its sounds' level and LEVEL; on minus off within 1 dB of that at
 * the start line), the engine alone -24 to -27 dBFS while driving and -22
 * at the line: the whine about 8 dB under the engine up to 90 km/h, 5 to 6
 * dB at 90 to 120 km/h, 10 dB at the line.  Its peak is at most 0.11 of
 * full scale (330 km/h), so it brings no clipping of its own: in a race with
 * every volume at 100 % the game's own sound reaches full scale without it.
 *
 * Threads: the game thread writes the targets, one word each; the mixer
 * smooths every change (so a word of the frame before beside one of this
 * frame is harmless), snaps each to its target once within a hair of it (no
 * denormals in a voice left to fade forever), and falls silent when they are
 * older than STALE_MS (a parked game). */
#include "whine.h"
#include "cpu.h"
#include "splitscreen_mem.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

int fileio_mod_pack(const char *rel, char *out, size_t outsz);   /* fileio.c: MODPACKS/<rel>'s host path */

#define G_FLOW     0x008654a4u        /* GameFlowManager: 6 in the world */
#define G_STEP     0x00865154u        /* this frame's world step (s), 0 while the world stands */
#define G_VOLUMES  0x0083aa28u        /* the game's volumes (floats 0..1): +0 Master, +0x14 Engine */
#define CAR_SPEED  0x430u             /* Car: speed (m/s), the engine sound's */
#define CAR_GEAR   0x434u             /* ...gear: 0 reverse, 1 neutral, 2 first .. */
#define STALE_MS   250u
#define SEATS      4                  /* local players with a whine of their own (split screen has at most 4) */
#define LEVEL      1.65f              /* 165 % of the mod's Volume (above: why, and measured) */
#define MAX_BYTES  (4u << 20)         /* a sound file bigger than this is not the mod's */

/* ---- the mod's sounds: MODPACKS/WHINE (tools/whinepack.py) ----------------------------- */
typedef struct { const uint8_t *pcm; uint32_t n; float rate; } Wav;
enum { W_WHINE, W_WHINE2, W_REVERSE, W_N };
static const char *const k_file[W_N] = { "WHINE/car_whine.wav", "WHINE/car_whine_2.wav", "WHINE/car_whine_reverse.wav" };

static uint32_t rd16(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p){ return rd16(p) | rd16(p + 2) << 16; }
/* a 16-bit mono PCM WAV's samples; n 0 when it is not one */
static Wav wav_parse(const uint8_t *b, uint32_t len){
    Wav w = { 0, 0, 0.f };
    if (len < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return w;
    uint32_t rate = 0, fmt_ok = 0;
    for (uint32_t o = 12; o + 8 <= len;){
        uint32_t sz = rd32(b + o + 4);
        if (sz > len - o - 8) sz = len - o - 8;
        if (!memcmp(b + o, "fmt ", 4) && sz >= 16)
            fmt_ok = rd16(b + o + 8) == 1 && rd16(b + o + 10) == 1 && rd16(b + o + 22) == 16, rate = rd32(b + o + 12);
        else if (!memcmp(b + o, "data", 4) && fmt_ok && rate){
            w.pcm = b + o + 8; w.n = sz / 2; w.rate = (float)rate;
            return w;
        }
        o += 8 + sz + (sz & 1u);
    }
    return w;
}
static Wav g_wav[W_N];
static uint8_t *g_file[W_N];
static char g_path[W_N][1100];
static int g_ready;                   /* 0 not read (yet), 1 read, -1 none (the mixer adds nothing); atomic */
static inline float wav_s(const Wav *w, uint32_t i){ return (float)(int16_t)rd16(w->pcm + 2u * i) * (1.f / 32768.f); }

static uint32_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* the loader thread: the three files whole (on the web the page fetches them now) */
static void *load_main(void *arg){
    (void)arg;
    uint32_t t0 = now_ms(), bytes = 0; double secs = 0;
    int ok = 1;
    for (int w = 0; w < W_N && ok; w++){
        FILE *f = fopen(g_path[w], "rb");
        long len = -1;
        if (f && !fseek(f, 0, SEEK_END) && (len = ftell(f)) > 44 && (uint32_t)len <= MAX_BYTES && !fseek(f, 0, SEEK_SET) &&
            (g_file[w] = malloc((size_t)len)) && fread(g_file[w], 1, (size_t)len, f) == (size_t)len)
            g_wav[w] = wav_parse(g_file[w], (uint32_t)len);
        if (f) fclose(f);
        if (g_wav[w].n < 4){ fprintf(stderr, "[whine] MODPACKS/%s is not a sound this reads: no whine\n", k_file[w]); ok = 0; }
        else { bytes += (uint32_t)len; secs += (double)g_wav[w].n / g_wav[w].rate; }
    }
    if (!ok){
        for (int w = 0; w < W_N; w++){ free(g_file[w]); g_file[w] = 0; g_wav[w].n = 0; }
        __atomic_store_n(&g_ready, -1, __ATOMIC_RELEASE);
        return 0;
    }
    fprintf(stderr, "[whine] the mod's sounds: %d (%.1f s, %u KB) in %u ms\n", W_N, secs, bytes >> 10, now_ms() - t0);
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    return 0;
}

/* ---- game thread -> mixer: one word per field ----------------------------------------- */
static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u){ float f; memcpy(&f, &u, 4); return f; }
#define GET(f)    __atomic_load_n(&(f), __ATOMIC_RELAXED)
#define PUT(f, v) __atomic_store_n(&(f), (v), __ATOMIC_RELAXED)
static struct { uint32_t gain, pitch; } g_in[SEATS][W_N];
static uint32_t g_stamp, g_level;     /* the targets' time (ms); the level (Master x Engine, 0 paused) */
static int g_on = -1;                 /* the setting (game thread) */

static void whine_init(void){
    if (g_on >= 0) return;
    /* the page writes 1 or 0; the Mac app passes the line's own on/off */
    const char *e = getenv("NFSU2_GEARBOX_WHINE");
    g_on = e && *e && *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
    if (!g_on) return;
    int have = 1;
    for (int w = 0; w < W_N; w++) have = have && fileio_mod_pack(k_file[w], g_path[w], sizeof g_path[w]);
    if (!have){
        fprintf(stderr, "[whine] gearbox whine on, but the game folder has no MODPACKS/WHINE (tools/whinepack.py): no whine\n");
        __atomic_store_n(&g_ready, -1, __ATOMIC_RELEASE);
        return;
    }
    fprintf(stderr, "[whine] gearbox whine on: reading its sounds\n");
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, load_main, 0)) load_main(0);
    pthread_attr_destroy(&a);
}

/* one of the game's volumes (G_VOLUMES + off), 1 while it is not there or not one */
static float game_volume(uint32_t off){
    uint32_t a = G_VOLUMES + off;
    if (!mem_is_committed(a) || !mem_is_committed(a + 3u)) return 1.f;
    float v = MRF32(a);
    return v >= 0.f && v <= 1.f ? v : 1.f;
}

/* a local player's car, 0 for none */
static uint32_t seat_car(int s){
    uint32_t a = SPLIT_PLAYERS_INDEX + 4u * (uint32_t)s;
    uint32_t pl = mem_is_committed(a + 3u) ? MR32(a) : 0u;
    uint32_t car = pl && mem_is_committed(pl + 7u) ? MR32(pl + 4u) : 0u;
    return car && mem_is_committed(car + CAR_GEAR + 3u) ? car : 0u;
}

/* a player's whine state, as the mod's: its car, the gear before (0: none yet, as its gGearOld), the pitch's gear part
 * (m), the speed before and the upshift's silence left */
typedef struct { uint32_t car; int32_t gear_old; float mod, speed_last, shift; } Seat;
static Seat g_seat[SEATS];

static void put(int s, int w, float gain, float pitch){ PUT(g_in[s][w].gain, fbits(gain)); PUT(g_in[s][w].pitch, fbits(pitch)); }
static void seat_quiet(int s){
    g_seat[s] = (Seat){ 0 };
    for (int w = 0; w < W_N; w++) PUT(g_in[s][w].gain, fbits(0.f));
}

static void seat_step(int s, uint32_t car, float dt){
    Seat *S = &g_seat[s];
    if (car != S->car){
        *S = (Seat){ .car = car };
        fprintf(stderr, "[whine] player %d's car 0x%08x: its whine\n", s + 1, car);
    }
    float kmh = MRF32(car + CAR_SPEED) * 3.6f;
    int32_t gear = (int32_t)MR32(car + CAR_GEAR);
    if (!isfinite(kmh)) kmh = 0.f;
    int play = gear != 0;                                /* not in reverse */
    float ticks = dt * 60.f;                             /* the mod's 1/60 s steps in this frame */

    float pitch = 0.3f, pitch2 = 1.f;
    if (kmh < 1.f){
        pitch2 = 0.f;
    } else {
        float target = (float)(gear - 1) * 0.020f;
        if (target < 0.f) target = 0.f;
        /* the mod's gearJustChanged: an upshift, not the first gear seen */
        int up = S->gear_old != 0 && gear > S->gear_old;
        if (S->gear_old == 0 || gear != S->gear_old) S->gear_old = gear;
        if (up){ S->shift = 0.3f; S->mod = target; }
        if (S->speed_last > kmh && S->mod > target){ S->mod -= 0.001f * ticks; if (S->mod < target) S->mod = target; }
        if (S->speed_last < kmh && S->mod < target){ S->mod += 0.001f * ticks; if (S->mod > target) S->mod = target; }
        S->speed_last = kmh;
        float m = 0.5f * sqrtf(kmh / 70.f);
        pitch = 0.3f + kmh / 330.f * 0.7f + (m > 0.6f ? 0.6f : m) + S->mod;
    }
    float shifting = 1.f;
    if (S->shift > 0.f){ S->shift -= dt; shifting = 0.f; }
    float g1 = 0.030f * (float)play * shifting;
    float g2 = (kmh > 0.f ? kmh / 140.f : 0.f) * 0.015f * (float)play * shifting;
    float gr = 0.050f * (float)!play;
    put(s, W_WHINE, g1, pitch);
    put(s, W_WHINE2, kmh < 1.f ? 0.f : g2, pitch2);
    put(s, W_REVERSE, kmh < 1.f ? 0.f : gr, pitch);
}

void whine_frame(void){
    whine_init();
    if (!g_on || __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) < 0) return;
    int flow6 = mem_is_committed(G_FLOW + 3u) && MR32(G_FLOW) == 6u;
    float dt = flow6 ? MRF32(G_STEP) : 0.f;
    int run = flow6 && dt > 0.f && dt < 0.25f;
    int n = split_render_limit();
    if (n < 1) n = 1;
    if (n > SEATS) n = SEATS;
    PUT(g_stamp, now_ms());
    PUT(g_level, fbits(run ? game_volume(0x00u) * game_volume(0x14u) * LEVEL : 0.f));
    for (int s = 0; s < SEATS; s++){
        uint32_t car = flow6 && s < n ? seat_car(s) : 0u;
        if (!car){ if (g_seat[s].car || !flow6) seat_quiet(s); continue; }
        if (run) seat_step(s, car, dt);                  /* the world standing: as it was, silent by the level */
    }
}

/* ---- mixer thread ----------------------------------------------------------------------- */
static struct { double pos; float gain, pitch; } g_v[SEATS][W_N];
static float g_lvl;

/* x one step of one-pole smoothing (k) towards t, and t itself once within a hair of it */
static inline float glide(float x, float t, float k){
    x += (t - x) * k;
    return fabsf(t - x) < 1e-6f ? t : x;
}

void whine_mix(float *acc, unsigned frames, unsigned rate){
    if (!rate || !frames || __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) != 1) return;
    float fs = (float)rate;
    int stale = now_ms() - GET(g_stamp) > STALE_MS;
    /* the level as the mod's fadeSnd (about 12 ticks), the gains over 0.1 s as its fade on an upshift, the pitch
     * over 20 ms; all as one-pole smoothing, each sample */
    float k_lvl = 1.f - expf(-1.f / (0.070f * fs)), k_gain = 1.f - expf(-1.f / (0.035f * fs)), k_pitch = 1.f - expf(-1.f / (0.020f * fs));
    float lvl_t = stale ? 0.f : bitsf(GET(g_level));
    if (!(lvl_t >= 0.f && lvl_t <= 4.f)) lvl_t = 0.f;
    if (g_lvl == 0.f && lvl_t == 0.f) return;            /* silent, and staying so: the voices keep their place */
    float gt[SEATS][W_N], pt[SEATS][W_N];
    int live[SEATS * W_N], nl = 0;
    for (int s = 0; s < SEATS; s++) for (int w = 0; w < W_N; w++){
        gt[s][w] = bitsf(GET(g_in[s][w].gain)); pt[s][w] = bitsf(GET(g_in[s][w].pitch));
        if (!(gt[s][w] >= 0.f && gt[s][w] < 1.f)) gt[s][w] = 0.f;
        if (!(pt[s][w] >= 0.f && pt[s][w] < 4.f)) pt[s][w] = 0.f;
        if (gt[s][w] > 0.f || g_v[s][w].gain > 0.f) live[nl++] = s * W_N + w;   /* the rest: silent, and staying so */
    }
    for (unsigned i = 0; i < frames; i++){
        g_lvl = glide(g_lvl, lvl_t, k_lvl);
        float out = 0.f;
        for (int j = 0; j < nl; j++){
            int s = live[j] / W_N, w = live[j] % W_N;
            const Wav *W = &g_wav[w];
            float g = g_v[s][w].gain = glide(g_v[s][w].gain, gt[s][w], k_gain);
            float p = g_v[s][w].pitch = glide(g_v[s][w].pitch, pt[s][w], k_pitch);
            double pos = g_v[s][w].pos;
            uint32_t i0 = (uint32_t)pos, i1 = i0 + 1u >= W->n ? 0u : i0 + 1u;
            float t = (float)(pos - (double)i0), a = wav_s(W, i0);
            out += (a + (wav_s(W, i1) - a) * t) * g;
            pos += (double)(p * W->rate / fs);
            if (pos >= (double)W->n) pos = fmod(pos, (double)W->n);
            g_v[s][w].pos = pos;
        }
        out *= g_lvl;
        acc[2u * i] += out; acc[2u * i + 1u] += out;
    }
}
