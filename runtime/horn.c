/* horn.c -- horns between players: the horn key, where each horn is heard
 * from, and car horns synthesised over the game's sound.  See horn.h.
 *
 * The PC game has no horn.  Its input names one (JOY_EVENT_HONK_HORN, 0xA2,
 * which no key or button map uses) and its audio a CARSFX_TrafficHorn class
 * (0x47E610), but none of its sound banks holds a horn: every sound in
 * SOUND/ was decoded (EA-XA, the BNKl banks of the .abk files) and looked
 * at, and the traffic bank has two engine loops only.  So the horns are made
 * here, eight kinds; the player picks theirs (NFSU2_HORN), and the others
 * hear that one (roam_game.c sends it with the car's look, racehorn.c with
 * each honk):
 *   dual       a car's dual horn: two tones a major third apart, each a buzzy
 *              band-limited wave with its upper harmonics lifted around
 *              2.7 kHz, summed through a soft clip that gives the pair its
 *              rasp, high-passed (the only horn before the others came)
 *   single     one such tone, higher and thinner: a small car's horn
 *   truck      a truck's air horn: two low brassy trumpets a minor third
 *              apart, the second a moment after the first, that blat up to
 *              pitch and sag when let go, with the air's hiss
 *   train      a train horn's five chimes (a B 6th chord: D# F# G# B D#),
 *              each speaking a little after the one below it
 *   klaxon     an old car's Klaxon: a toothed wheel spun up against a
 *              diaphragm, its pitch rising with the wheel ("ah-oo") and
 *              falling when let go ("gah"), through the trumpet's resonance
 *   cucaracha  a musical air horn: La Cucaracha while it is held, over again
 *   clown      a clown's rubber bulb horn, nasal reeds squeezed honk-honk
 *              while it is held, each honk bending up into its pitch
 *   trombone   a sad trombone: wah, wah, wah, waaah (falling a semitone a
 *              note, the last one shaking), through a mute that opens with
 *              each note
 * Each player's horn is pitched one of five ways (from the name), so players
 * with the same kind can still tell who honks.
 *
 * The key: N, which no control of the game's keyboard map (0x7FCAC0) uses,
 * and the controller's left stick click while the player's car has no
 * hydraulics (the callers say).  With the Hydraulics part (Car Specialties)
 * that click is the game's Hydraulic Pressurize, as H is on the keyboard
 * (the pad layout of dinput_pad.c; Hydraulic Bounce: the right stick, J).
 *
 * Where a horn is heard from: the player's own in the middle; another
 * player's from their car as the camera hears it, full within NEAR_M, then
 * falling with the distance until it is gone at FAR_M (the last 50 m a
 * fade), from the side the car is on, duller the farther it is, its pitch
 * shifted by how fast the car and the player come or go (Doppler).  All of
 * them as loud as the Horn Volume slider of the game's Audio options says
 * (hornmenu.c): 10 % to 100 % of that, times the game's Master Volume, as
 * the game's own sounds are their sliders times Master (none at 0).
 *
 * Threads: horn_begin/horn_sound/horn_end on the game thread, every frame
 * while horns may sound; the mixer (audio.m: the AudioUnit callback,
 * audio_web.c: the audio pump) reads each voice through relaxed atomics,
 * one word per field, and smooths every change, so a torn update is never
 * heard.  A voice that has not been set for 250 ms (a parked game) falls
 * silent. */
#include "horn.h"
#include "kbd.h"
#include "gamepad.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define DIK_N     0x31
#define LEVEL     0.55f              /* another player's dual horn within NEAR_M: about -8 dBFS at its peaks */
#define SELF      0.7f               /* the player's own horn, of that */
#define NEAR_M    8.f
#define FAR_M     250.f
#define SOUND_MPS 343.f
#define STALE_MS  250u
#define TBL       2048
#define TONES     5

/* The player's horn key (N unless NFSU2_KEY_HORN: dinput_kbd.c dikbd_site_key) and stick click (the left one unless
 * NFSU2_BUTTON_HORN: dinput_pad.c pad_horn_ours), and the touch controls' Horn button (DIK_TOUCH_HORN, none of the
 * game's).  The key and the click leave the horn when the player put one of the game's controls on them (dikbd_ours,
 * pad_horn_ours); a click the game has nothing on honks with hydraulics too. */
#define DIK_TOUCH_HORN 0xF2u              /* web/src/input/keymap.ts Nfsu2Horn: no keyboard has it */
__attribute__((weak)) int dikbd_ours(unsigned dik, const char *what){ (void)dik; (void)what; return 1; }
__attribute__((weak)) unsigned dikbd_site_key(const char *env, unsigned dflt){ (void)env; return dflt; }
__attribute__((weak)) int pad_horn_ours(int *button){ *button = PAD_LS; return 1; }
int horn_held(int stick){
    static unsigned dik = 0x100u;
    if (dik == 0x100u) dik = dikbd_site_key("NFSU2_KEY_HORN", DIK_N);
    if (kbd_key_down(DIK_TOUCH_HORN) || (dik && kbd_key_down(dik) && dikbd_ours(dik, "the horn"))) return 1;
    PadState s;
    int btn = -1, st = pad_horn_ours(&btn);
    return btn >= 0 && (st == 2 || (st == 1 && stick)) && gamepad_read(&s) && (s.buttons & 1u << btn);
}

/* ---- the kinds ------------------------------------------------------------------ */
/* a tune's note: semitones over the tones' pitch (REST: none), centiseconds,
 * 1: it shakes (vibrato) */
typedef struct { int8_t st; uint8_t cs, vib; } Note;
#define REST (-99)
static const Note CUCARACHA[] = {                    /* from G */
    { 0, 13, 0 }, { 0, 13, 0 }, { 0, 13, 0 }, { 5, 36, 0 }, { 9, 50, 0 }, { REST, 22, 0 },
    { 0, 13, 0 }, { 0, 13, 0 }, { 0, 13, 0 }, { 5, 36, 0 }, { 9, 50, 0 }, { REST, 22, 0 },
    { 5, 18, 0 }, { 5, 18, 0 }, { 4, 18, 0 }, { 4, 18, 0 }, { 2, 18, 0 }, { 2, 18, 0 }, { 0, 54, 0 }, { REST, 80, 0 } };
static const Note CLOWN[] = { { 0, 22, 0 }, { REST, 9, 0 }, { -3, 30, 0 }, { REST, 45, 0 } };
static const Note TROMBONE[] = { { 0, 42, 0 }, { -1, 42, 0 }, { -2, 42, 0 }, { -3, 170, 1 }, { REST, 90, 0 } };
#define TUNE(t) .tune = (t), .ntune = (int)(sizeof (t) / sizeof (t)[0])
#define GAP_S  0.035f                /* each note's end silent: its valve closes */

typedef struct {
    const char *name;
    int   n;                         /* tones */
    float f[TONES], a[TONES];        /* each tone's Hz and level */
    float dly[TONES];                /* s after the start each tone speaks (one trumpet after another) */
    /* the wave (tbl_init), one cycle at f[0]: harmonics 1..nh at 1/k^roll,
     * those near form Hz (width: how near) lifted up to 1 + lift times,
     * those past top (Hz, 0: none) falling away */
    int   nh; float roll, form, width, lift, top;
    float att, rel;                  /* the envelope's time constants (s) */
    float sag;                       /* pitch x (1 - sag (1 - envelope)): an air horn's rise to pitch and fall, the Klaxon's sweep */
    float drive, hiss;               /* into the soft clip; the air's noise */
    /* Hz: a resonance after the clip, 0: none; a band (the Klaxon's
     * diaphragm and trumpet), or with wah a low-pass that opens by wah Hz
     * with each note (a trombone's mute) */
    float res, wah;
    float level;                     /* as loud as the dual horn */
    int   swell;                     /* the level follows the envelope's 1.5th power (the Klaxon) */
    const Note *tune; int ntune;     /* the tones play this tune, over again */
    float gate, nsag, vib;           /* the tune: each note's rise and fall (s, 0: 4 ms), its pitch x (1 - nsag (1 - that)), the vibrato's depth */
} Kind;
/* the highest harmonic stays below 20 kHz at the highest pitch (x 1.12) and
 * a fast pass (x 1.4) */
static const Kind KINDS[HORN_KINDS] = {
    { .name = "dual", .n = 2, .f = { 405.f, 510.f }, .a = { 1.f, .9f },
      .nh = 24, .roll = 1.f, .form = 2700.f, .width = 1100.f, .lift = 1.5f,
      .att = .008f, .rel = .030f, .drive = 1.2f, .level = 1.f },
    { .name = "single", .n = 1, .f = { 480.f }, .a = { 1.f },
      .nh = 20, .roll = 1.f, .form = 3000.f, .width = 1000.f, .lift = 2.f,
      .att = .006f, .rel = .025f, .drive = 1.7f, .level = 1.035f },
    { .name = "truck", .n = 2, .f = { 185.f, 222.f }, .a = { 1.f, .85f }, .dly = { 0.f, .025f },
      .nh = 40, .roll = .8f, .form = 1100.f, .width = 700.f, .lift = 2.f, .top = 6000.f,
      .att = .030f, .rel = .050f, .sag = .06f, .drive = 1.5f, .hiss = .03f, .level = 1.175f },
    { .name = "train", .n = 5, .f = { 311.1f, 370.f, 415.3f, 493.9f, 622.3f }, .a = { .55f, .5f, .5f, .45f, .4f },
      .dly = { 0.f, .02f, .035f, .05f, .07f },
      .nh = 16, .roll = 1.f, .form = 1400.f, .width = 900.f, .lift = 1.2f, .top = 5000.f,
      .att = .050f, .rel = .080f, .sag = .03f, .drive = .9f, .hiss = .02f, .level = 1.29f },
    { .name = "klaxon", .n = 1, .f = { 380.f }, .a = { 1.f },
      .nh = 28, .roll = .5f, .width = 1.f, .top = 9000.f,
      .att = .180f, .rel = .150f, .sag = .72f, .drive = 2.2f, .res = 750.f, .level = 1.95f, .swell = 1 },
    { .name = "cucaracha", .n = 2, .f = { 392.f, 393.6f }, .a = { 1.f, .5f },
      .nh = 18, .roll = .8f, .form = 2000.f, .width = 1000.f, .lift = 1.5f, .top = 7000.f,
      .att = .012f, .rel = .040f, .sag = .02f, .drive = 1.4f, .hiss = .01f, .level = 1.f, TUNE(CUCARACHA) },
    { .name = "clown", .n = 1, .f = { 340.f }, .a = { 1.f },
      .nh = 30, .roll = .6f, .form = 1600.f, .width = 700.f, .lift = 3.f, .top = 8000.f,
      .att = .010f, .rel = .040f, .drive = 3.f, .level = 1.19f, TUNE(CLOWN), .gate = .015f, .nsag = .12f },
    { .name = "trombone", .n = 1, .f = { 233.1f }, .a = { 1.f },
      .nh = 36, .roll = 1.f, .width = 1.f, .top = 5000.f,
      .att = .020f, .rel = .060f, .drive = 1.2f, .res = 350.f, .wah = 1400.f, .level = 1.22f,
      TUNE(TROMBONE), .gate = .040f, .vib = .02f },
};

const char *horn_name(int kind){ return KINDS[kind >= 0 && kind < HORN_KINDS ? kind : 0].name; }
int horn_mine(void){
    static int mine = -1;
    if (mine < 0){
        const char *e = getenv("NFSU2_HORN");
        mine = 0;
        for (int k = 0; e && *e && k < HORN_KINDS; k++) if (!strcasecmp(e, KINDS[k].name)) mine = k;
        if (e && *e && strcasecmp(e, KINDS[mine].name)) fprintf(stderr, "[horn] NFSU2_HORN=%s: no such horn, the dual one\n", e);
        else fprintf(stderr, "[horn] my horn: %s\n", KINDS[mine].name);
    }
    return mine;
}
/* how long a voice let go takes to fade out (to -80 dB) before another horn takes it */
static uint32_t tail_ms(int kind){ return (uint32_t)(9.3f * KINDS[kind].rel * 1000.f) + 120u; }

static uint32_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static uint32_t fnv1a(const char *s){
    uint32_t h = 2166136261u;
    for (; s && *s; s++){ h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}
static int g_logged;
#define HLOG(...) do { if (__atomic_add_fetch(&g_logged, 1, __ATOMIC_RELAXED) <= 40) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---- game thread -> mixer: one word per field ------------------------------------ */
static struct { uint32_t tune, kind, gain, pan, pitch, stamp; } g_in[HORN_VOICES];
static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u){ float f; memcpy(&f, &u, 4); return f; }
#define PUT(f, v) __atomic_store_n(&(f), (v), __ATOMIC_RELAXED)
#define GET(f)    __atomic_load_n(&(f), __ATOMIC_RELAXED)
static void voice_set(int v, uint32_t tune, int kind, float gain, float pan, float pitch){
    PUT(g_in[v].tune, tune); PUT(g_in[v].kind, (uint32_t)kind); PUT(g_in[v].gain, fbits(gain)); PUT(g_in[v].pan, fbits(pan));
    PUT(g_in[v].pitch, fbits(pitch)); PUT(g_in[v].stamp, now_ms());
}
/* the volume and the game's Master Volume, as the mixer reads them: 1.0's
 * bits until the slider sets the one and the game thread reads the other */
static uint32_t g_vol = 0x3f800000u, g_master = 0x3f800000u;
void horn_set_volume(float v){
    if (!(v >= HORN_VOL_MIN)) v = HORN_VOL_MIN;
    if (v > 1.f) v = 1.f;
    PUT(g_vol, fbits(v));
}
/* photo mode's Horns off: every horn quiet while it is on (the voices fade as the volume does) */
static uint32_t g_mute;
void horn_mute(int on){ PUT(g_mute, on ? 1u : 0u); }
static void master_read(void){
    float m = horn_game_master();
    if (!(m >= 0.f)) m = 0.f;
    if (m > 1.f) m = 1.f;
    if (fbits(m) != GET(g_master)) fprintf(stderr, "[horn] the game's Master Volume at %.0f %%\n", 100.0 * m);
    PUT(g_master, fbits(m));
}

/* ---- the game thread: which horn has which voice ---------------------------------- */
static struct { int used, sounded, kind; uint32_t who, tune, free_at; float pan, pitch; } g_vo[HORN_VOICES];
static struct { int ok; float p[3], l[2], v[3]; } g_lis;
#define PREVIEW_WHO 0xfffffffeu
static char g_self[64];                                  /* the name the player's own horn last sounded with (its pitch) */
void horn_begin(const float *pos, const float *fwd, const float *vel){
    master_read();
    g_lis.ok = pos && fwd;
    if (!g_lis.ok) return;
    for (int a = 0; a < 3; a++){ g_lis.p[a] = pos[a]; g_lis.v[a] = vel ? vel[a] : 0.f; }
    float lx = -fwd[1], ly = fwd[0], ln = sqrtf(lx * lx + ly * ly);   /* the camera's left, level (z is up) */
    if (ln > 0.001f){ g_lis.l[0] = lx / ln; g_lis.l[1] = ly / ln; } else { g_lis.l[0] = 0.f; g_lis.l[1] = 1.f; }
}
int horn_sound(uint32_t who, const char *name, int kind, const float *p, const float *v){
    float gain = SELF, pan = 0.f, pitch = 1.f, dist = 0.f;
    if (kind < 0 || kind >= HORN_KINDS) kind = 0;        /* a newer build's horn */
    if (p){
        if (!g_lis.ok) return -1;
        float d[3] = { p[0] - g_lis.p[0], p[1] - g_lis.p[1], p[2] - g_lis.p[2] };
        dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(dist < FAR_M)) return -1;
        gain = dist > NEAR_M ? NEAR_M / dist : 1.f;
        if (dist > FAR_M - 50.f) gain *= (FAR_M - dist) / 50.f;
        if (dist > 0.1f){
            float vl = 0.f, vs = 0.f;
            pan = -(d[0] * g_lis.l[0] + d[1] * g_lis.l[1]) / dist;
            for (int a = 0; a < 3; a++){ vl += g_lis.v[a] * d[a] / dist; vs += (v ? v[a] : 0.f) * d[a] / dist; }
            /* c + the player's speed towards the horn over c + the horn's speed away */
            pitch = (SOUND_MPS + vl) / (SOUND_MPS + vs);
            if (!(pitch >= 0.5f)) pitch = 0.5f;
            if (pitch > 2.f) pitch = 2.f;
        }
    }
    if (!p && who != PREVIEW_WHO && name) snprintf(g_self, sizeof g_self, "%s", name);
    int k = 0, start = 0;
    while (k < HORN_VOICES && !(g_vo[k].used && g_vo[k].who == who)) k++;
    if (k == HORN_VOICES){
        uint32_t now = now_ms();
        for (k = 0; k < HORN_VOICES; k++) if (!g_vo[k].used && (!g_vo[k].free_at || (int32_t)(now - g_vo[k].free_at) >= 0)) break;
        if (k == HORN_VOICES) return -1;
        g_vo[k].used = 1; g_vo[k].who = who; g_vo[k].tune = fnv1a(name); g_vo[k].kind = kind; g_vo[k].free_at = 0; start = 1;
        if (p) HLOG("[horn] %s's horn, %.0f m away (%s)\n", name && *name ? name : "a player", (double)dist, KINDS[kind].name);
        else HLOG("[horn] my horn (%s)\n", KINDS[kind].name);
    }
    g_vo[k].sounded = 1; g_vo[k].pan = pan; g_vo[k].pitch = pitch;
    voice_set(k, g_vo[k].tune, g_vo[k].kind, gain, pan, pitch);   /* a voice keeps its kind until it is let go */
    return start;
}
void horn_end(void){
    uint32_t now = now_ms();
    for (int k = 0; k < HORN_VOICES; k++){
        if (g_vo[k].sounded){ g_vo[k].sounded = 0; continue; }
        if (g_vo[k].used){ g_vo[k].used = 0; g_vo[k].free_at = now + tail_ms(g_vo[k].kind); }
        voice_set(k, g_vo[k].tune, g_vo[k].kind, 0.f, g_vo[k].pan, g_vo[k].pitch > 0.f ? g_vo[k].pitch : 1.f);   /* fades as it was */
    }
}
/* the player's own horn, sounded once: the mixer lets it go STALE_MS later
 * (or the next horn_end, when free roam or a race has the voices) */
void horn_preview(void){ master_read(); horn_sound(PREVIEW_WHO, g_self, horn_mine(), 0, 0); }

/* ---- the mixer ----------------------------------------------------------------- */
static float g_tbl[HORN_KINDS][TBL + 1];
static int g_tbl_ok;
static void tbl_init(void){
    for (int h = 0; h < HORN_KINDS; h++){
        const Kind *K = &KINDS[h];
        double peak = 0.0;
        for (int i = 0; i < TBL; i++){
            double x = 2.0 * M_PI * i / TBL, s = 0.0;
            for (int k = 1; k <= K->nh; k++){
                double d = ((double)K->f[0] * k - K->form) / K->width, t = K->top > 0.f ? (double)K->f[0] * k / K->top : 0.0;
                s += sin(k * x) / pow(k, K->roll) * (1.0 + K->lift * exp(-d * d)) * (t > 1.0 ? exp(1.0 - t * t) : 1.0);
            }
            g_tbl[h][i] = (float)s;
            if (fabs(s) > peak) peak = fabs(s);
        }
        for (int i = 0; i < TBL; i++) g_tbl[h][i] = (float)(g_tbl[h][i] / peak);
        g_tbl[h][TBL] = g_tbl[h][0];
    }
    g_tbl_ok = 1;
}
static inline float wave(const float *tb, float ph){     /* ph in [0, 1) */
    float x = ph * TBL; int i = (int)x;
    return tb[i] + (tb[i + 1] - tb[i]) * (x - (float)i);
}
static float detune(uint32_t tune){
    static const float k[5] = { 1.f, 0.94f, 1.06f, 0.89f, 1.12f };
    return k[(tune * 2654435761u >> 16) % 5u];
}

static struct {
    int   live, kind;                                    /* sounding or fading out; its kind, kept until it is silent */
    float f[TONES], ph[TONES];                           /* the tones: cycles per sample at pitch 1, phases (cycles) */
    uint32_t age, on[TONES];                             /* samples since the start; when each tone speaks */
    float env, gl, gr, pitch, hx, hy, lp, lo, bd;        /* lo, bd: the resonance (a state-variable filter) */
    int   note; uint32_t left, in;                       /* the tune: the note, samples left of it and into it, */
    float ratio, gate, gs, vph;                          /* its pitch, on/off, that smoothed, the vibrato's phase (cycles) */
    uint32_t rng;
    uint32_t frames; float peak[2];                      /* the log line at the end */
} g_v[HORN_VOICES];

void horn_mix(float *acc, unsigned frames, unsigned rate){
    if (!rate || !frames) return;
    if (!g_tbl_ok) tbl_init();
    uint32_t now = now_ms();
    float fs = (float)rate;
    float k_smooth = 1.f - expf(-1.f / (0.020f * fs));
    float k_hp = 1.f / (1.f + 2.f * (float)M_PI * 180.f / fs), rise = 1.f / (0.030f * fs);
    uint32_t gap = (uint32_t)(GAP_S * fs), vib_in = (uint32_t)(0.25f * fs);
    float vol = GET(g_mute) ? 0.f : bitsf(GET(g_vol)) * bitsf(GET(g_master));   /* in the voices' gains, smoothed with them */
    for (int v = 0; v < HORN_VOICES; v++){
        float gain = bitsf(GET(g_in[v].gain));
        if (now - GET(g_in[v].stamp) > STALE_MS) gain = 0.f;
        if (!g_v[v].live){
            if (!(gain > 0.f)) continue;
            memset(&g_v[v], 0, sizeof g_v[v]);
            g_v[v].live = 1;
            uint32_t kd = GET(g_in[v].kind);
            g_v[v].kind = kd < HORN_KINDS ? (int)kd : 0;
            float t = detune(GET(g_in[v].tune));            /* a voice keeps its pitch until it is silent */
            for (int i = 0; i < KINDS[g_v[v].kind].n; i++){
                g_v[v].f[i] = KINDS[g_v[v].kind].f[i] * t / fs;
                g_v[v].on[i] = (uint32_t)(KINDS[g_v[v].kind].dly[i] * fs);
            }
            g_v[v].pitch = bitsf(GET(g_in[v].pitch));
            g_v[v].note = -1; g_v[v].ratio = 1.f; g_v[v].rng = 0x9e3779b9u * (uint32_t)(v + 1);
        }
        const Kind *K = &KINDS[g_v[v].kind];
        const float *tb = g_tbl[g_v[v].kind];
        float k_att = 1.f - expf(-1.f / (K->att * fs)), k_rel = 1.f - expf(-1.f / (K->rel * fs));
        float k_gate = 1.f - expf(-1.f / ((K->gate > 0.f ? K->gate : 0.004f) * fs)), dvib = 5.f / fs;
        float svf = K->res > 0.f ? 2.f * sinf((float)M_PI * K->res / fs) : 0.f, wah = 2.f * (float)M_PI * K->wah / fs;
        float pan = bitsf(GET(g_in[v].pan)), tp = bitsf(GET(g_in[v].pitch));
        float th = (pan + 1.f) * (float)M_PI * 0.25f;     /* equal power */
        float tgl = gain * cosf(th) * vol, tgr = gain * sinf(th) * vol, te = gain > 0.f ? 1.f : 0.f;
        /* far horns lose their top: a one-pole low-pass from ~1.5 kHz (gain 0.05, 160 m) up to none (gain 0.6) */
        float k_lp = 0.15f + 0.85f * (gain * 1.7f > 1.f ? 1.f : gain * 1.7f);
        float env = g_v[v].env, gl = g_v[v].gl, gr = g_v[v].gr, pt = g_v[v].pitch;
        float hx = g_v[v].hx, hy = g_v[v].hy, lp = g_v[v].lp, lo = g_v[v].lo, bd = g_v[v].bd, pk0 = g_v[v].peak[0], pk1 = g_v[v].peak[1];
        uint32_t age = g_v[v].age;
        for (unsigned i = 0; i < frames; i++, age++){
            pt += (tp - pt) * k_smooth;
            float bend = pt, amp = 1.f;
            if (K->sag > 0.f) bend *= 1.f - K->sag * (1.f - env);
            if (K->tune){
                if (!g_v[v].left){
                    const Note *nt = &K->tune[g_v[v].note = (g_v[v].note + 1) % K->ntune];
                    g_v[v].left = (uint32_t)(nt->cs * 0.01f * fs); g_v[v].in = 0;
                    g_v[v].gate = nt->st != REST;
                    if (g_v[v].gate > 0.f) g_v[v].ratio = exp2f(nt->st / 12.f);
                }
                if (g_v[v].left-- <= gap) g_v[v].gate = 0.f;
                float gs = g_v[v].gs += (g_v[v].gate - g_v[v].gs) * k_gate;
                bend *= g_v[v].ratio * (1.f - K->nsag * (1.f - gs)); amp = gs;
                if (K->tune[g_v[v].note].vib && g_v[v].in++ > vib_in){   /* the shake comes in over 0.3 s */
                    float d = (float)(g_v[v].in - vib_in) / (0.3f * fs);
                    if ((g_v[v].vph += dvib) >= 1.f) g_v[v].vph -= 1.f;
                    bend *= 1.f + K->vib * (d < 1.f ? d : 1.f) * sinf(2.f * (float)M_PI * g_v[v].vph);
                }
            }
            float s = 0.f;
            for (int t = 0; t < K->n; t++){
                if (age < g_v[v].on[t]) continue;
                float *ph = &g_v[v].ph[t];
                *ph += g_v[v].f[t] * bend; if (*ph >= 1.f) *ph -= 1.f;
                float w = wave(tb, *ph) * K->a[t], r = (float)(age - g_v[v].on[t]) * rise;
                if (g_v[v].on[t] && r < 1.f) w *= r;          /* a later trumpet speaks over 30 ms */
                s += w;
            }
            if (K->hiss > 0.f){
                uint32_t x = g_v[v].rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; g_v[v].rng = x;
                s += K->hiss * ((float)(int32_t)x * (1.f / 2147483648.f));
            }
            s *= K->drive;
            s = s / (1.f + fabsf(s));
            if (svf > 0.f){
                float fc = svf + wah * amp;                  /* the mute opens with the note */
                lo += fc * bd; float hi = s - lo - 0.5f * bd; bd += fc * hi;
                s = K->wah > 0.f ? lo : 0.3f * s + 0.5f * bd;
            }
            hy = k_hp * (hy + s - hx); hx = s;
            lp += (hy - lp) * k_lp;
            env += (te - env) * (te > env ? k_att : k_rel);
            gl += (tgl - gl) * k_smooth; gr += (tgr - gr) * k_smooth;
            float o = lp * (K->swell ? env * sqrtf(env) : env) * amp * LEVEL * K->level, oL = o * gl, oR = o * gr;
            acc[2 * i] += oL; acc[2 * i + 1] += oR;
            if (fabsf(oL) > pk0) pk0 = fabsf(oL);
            if (fabsf(oR) > pk1) pk1 = fabsf(oR);
        }
        g_v[v].env = env; g_v[v].gl = gl; g_v[v].gr = gr; g_v[v].pitch = pt; g_v[v].age = age;
        g_v[v].hx = hx; g_v[v].hy = hy; g_v[v].lp = lp; g_v[v].lo = lo; g_v[v].bd = bd; g_v[v].peak[0] = pk0; g_v[v].peak[1] = pk1;
        g_v[v].frames += frames;
        if (te == 0.f && env < 1e-4f){
            g_v[v].live = 0;
            HLOG("[horn] voice %d (%s): %.2f s, peak left %.1f dBFS, right %.1f dBFS\n", v, K->name, (double)g_v[v].frames / fs,
                 pk0 > 1e-6f ? 20.0 * log10(pk0) : -120.0, pk1 > 1e-6f ? 20.0 * log10(pk1) : -120.0);
        }
    }
}
