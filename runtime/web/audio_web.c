/* The DirectSound mixer for the browser: runtime/audio.m without CoreAudio.
 *
 * The game mixes everything itself into one looping DirectSound buffer (EA's
 * SND library); com.c's
 * DirectSound HLE registers that buffer here and the mixer below plays it:
 * the same slot table, the same mix_into (nearest-neighbour resampling), only
 * the output differs, and the playback rate: constant here, where audio.m's
 * write-position servo and SNDSYNC rate ratio made the music's pitch wander
 * (both stay behind SNDSERVO=1 / SNDSYNC=1, see mix_into).
 *
 * On the Mac a CoreAudio render callback pulled 512-frame slices.  Here a
 * dedicated "audio pump" pthread mixes 512-frame slices into a float32 ring
 * in shared memory (audio_proto.h) that the page's AudioWorklet drains, so
 * the mixer keeps its pthread mutex (a worklet may not block) and its
 * callback-sized steps (the servo's constants are per callback).  The pump
 * paces itself by the ring: it tops it up to the target fill and sleeps on
 * the worklet's read counter, which the worklet notifies every render
 * quantum.  That locks the play cursors to the output device clock, as the
 * AudioUnit callback did.
 *
 * The DirectSound play cursor must move in real time, always: if it stands
 * still the EA library tears its buffer down and recreates it (after ~100
 * polls; seen on the Mac: 120 buffers in 4 s, then a crash at 0x727030), and
 * the movie player takes its clock from it.  In a browser the output can be
 * missing or stop at any time: no page attached yet, an AudioContext that is
 * suspended until the first user gesture or later interrupted, a stalled
 * device, NOSOUND (settings: sound = off).  So whenever the worklet has not
 * consumed anything for STALL_MS (or two of its gaps, below), or the page
 * reports a context that is not running, the pump falls back to the wall
 * clock: it keeps mixing slices at 44.1 kHz real time and drops them.  Cursors
 * keep moving; nothing is heard.  A pump that slept past its time (a busy
 * machine; a background tab, whose timers wake it seldom) finds a backlog: a
 * game that asks for it (game.h sound_wall_catchup: Most Wanted, whose sound
 * thread rebuilds DirectSound under its own feet after 100 polls without a
 * cursor move) has all of it made up, and its cursors run on the wall clock
 * between the slices too (wall_ahead_locked), so they keep real time however
 * seldom the pump wakes; Underground 2 keeps the old rule, a backlog over
 * 100 ms dropped (a pump woken every 150 ms moves the cursors at 8 % of real
 * time that way; that game is parked in a background tab anyway).
 *
 * Chrome runs the worklet once per render quantum, but in bursts: two output
 * callbacks' worth back to back (measured headless), so 8 quanta every 23 ms
 * at a base latency of 11.6 ms, and on a phone with a base latency of ~90 ms
 * (2026-10-25 reports) ~60 quanta every ~180 ms.  With a 60 ms stall test and a
 * 23 ms ring that phone's sound was dropped more than it was played
 * (reproduced with audio_latency 45 and 90).  So the pump learns the worklet's
 * rhythm: a stall is two of its longest gaps without a read (RHYTHM_*), and
 * when its bursts are big (over twice the start target) the ring holds one
 * and a slice (BURST_*; why the ring is 16384 frames now) and the pump mixes
 * at a steady pace (PACE) instead of refilling a burst at once.  That keeps
 * the DirectSound play cursor moving in real time: a cursor that stood still
 * and then jumped by a burst left the game's writer, which follows it, behind
 * (a skip every burst).  A desktop's bursts keep the old refill.
 *
 *   NOSOUND=1                 no output; the pump still runs (wall clock)
 *   SNDSYNC=1                 audio.m's rate ratio between game and real time
 *   SNDSERVO=1                audio.m's write-position servo (+-3 % trim)
 *   NFSU2_AUDIO_TARGET=<n>    ring fill in frames (default 1024 = 23 ms); worklet
 *                             underruns raise it a slice each, SHRINK_QUIET_S
 *                             without one lower it a slice, down to this (or
 *                             to a burst and a slice, if more)
 *   NFSU2_AUDIO_STATUS=<s>    an "[audio]" status line every s seconds
 *                             (default 5 when NFSU2_STATUS_EVERY is set)
 *   NFSU2_AUDIO_CATCHUP=0|1   the wall clock's backlog rule, whatever the
 *                             game's (sound_wall_catchup): 1 made up, 0 dropped
 *                             over 100 ms (an A/B in one build)
 *   NFSU2_AUDIO_WAKE_MS=<n>   test-only: on the wall clock the pump sleeps n ms
 *                             between wake-ups, as a background tab's timers do
 *   NFSU2_AUDIO_NEAREST=1     test-only: a buffer at another rate by the nearest
 *                             sample, as before the resampler (rs_table; an A/B)
 *   SNDDUMP, SNDCSV, SNDSTAT, SNDGAP   the Mac's diagnostics, unchanged */
#include "cpu.h"                 /* g_mem: guest memory is linear memory from 0 */
#include "game.h"                /* g_game.sound_wall_catchup */
#include "host_abi.h"
#include "audio_proto.h"
#include "horn.h"
#include "carsfx.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <stdatomic.h>

uint64_t host_us(void);          /* the game's virtual clock (win32.c) */

static double mono_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static int env_on(const char *n){ const char *e = getenv(n); return e && *e && *e != '0'; }

/* ---- rate ratio (SNDSYNC), as audio.m; off by default here ----------------
 * audio.m made consumption follow the game's virtual clock (never faster than
 * real time, cap 1.0) for a game that ran below 60 fps on a fixed 16.7 ms
 * clock.  With the real-time clock (win32.c VCLOCK) the ratio only moves when
 * a frame runs past the 50 ms clock cap, and then it bends the pitch: every
 * loading hitch slowed the music to ~0.65 for a second.  The game's writer
 * follows the play cursor (below), so nothing needs the ratio: SNDSYNC=1
 * brings it back. */
static double g_rate_ratio = 1.0;
static int    g_sync_mode = -1;
/* the status line's window: the playback rate's extremes since the last line */
static double g_win_rmin = 9.0, g_win_rmax = 0.0, g_win_tmin = 9.0, g_win_tmax = 0.0;
static double g_win_fmin = 9.0, g_win_fmax = 0.0; static uint64_t g_win_over = 0;
/* how far ahead of the mixer the game has written (ms, the game's share of the
 * sound's delay), and the slices mixed before the game wrote them (each one a
 * glitch): the status line's window and the whole run */
static double g_win_lmin = 1e9, g_win_lmax = 0.0, g_lead_sum = 0.0, g_lead_max = 0.0;
static uint64_t g_lead_n = 0, g_starved = 0, g_win_starved = 0, g_skips = 0;
static _Atomic int g_reported = 0;   /* the game is ending (audio_report): its writer stops, no skip */
static int g_servo = 0;          /* SNDSERVO=1: audio.m's write-position trim */
static void update_rate_ratio(void){
    if (g_sync_mode < 0){
        const char *e = getenv("SNDSYNC");
        g_sync_mode = (e && *e) ? (*e != '0') : 0;
    }
    if (!g_sync_mode){ g_rate_ratio = 1.0; return; }
    static uint64_t lv = 0, lr = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t rus = (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
    uint64_t vus = host_us();
    if (lv && rus > lr){
        double dv = (double)(vus - lv), dr = (double)(rus - lr);
        if (dr > 1000.0){
            double k = dv / dr;
            if (k < 0.25) k = 0.25;
            if (k > 1.00) k = 1.00;      /* never faster than real time */
            g_rate_ratio += (k - g_rate_ratio) * 0.10;
        }
    }
    lv = vus; lr = rus;
}

#define OUT_RATE   AUDIO_RATE
#define OUT_CH     2
#define FRAMES     2048          /* largest slice mix_into takes */
#define SLICE      512           /* frames per mix_into call: the Mac's AudioUnit slice */
#define STALL_MS   60.0          /* no consumption for this long (or 2 rhythms): wall-clock pacing */
#define START_GRACE_S 0.5        /* ... except right after the consumer appeared (two ~180 ms gaps to learn) */
#define SHRINK_QUIET_S 20.0      /* no worklet underrun for this long: the ring target a slice lower */
#define RHYTHM_WIN_S 1.0         /* the worklet's rhythm: the longest gap between its reads in each second, */
#define RHYTHM_N   5             /* ... the median of the last RHYTHM_N seconds (a hitch is one second's) */
#define RHYTHM_WAKE_MS 20.0      /* a gap the pump slept through longer than this says nothing */
#define BURST_SPLIT_MS 3.0       /* reads further apart than this begin a new burst */
#define BURST_N    9             /* the time between bursts: the median of the last BURST_N */
#define PACE       1.05          /* ring mode: slices at most this much faster than real time (the target caps it) */
#define CATCHUP_S  0.03          /* ... and at most this much made up at once after the pump was late */
#define WALL_DROP_S 0.1          /* wall clock, the old rule: a backlog over this is dropped */
#define WALL_MIX_S 0.5           /* ... sound_wall_catchup's: a backlog up to this is mixed slice by slice, and the */
                                 /* cursors skip what is more (a frozen tab; nothing is heard on the wall clock) */

#define MAXSND 128
#define SETTLE_SLICES 90          /* ~1 s: a stream's start (a cleared buffer) is no glitch */
typedef struct {
    uint32_t data, bytes, rate, chans, bits;
    int      used, playing, loop;
    double   cursor;          /* source frames */
    float    vol;             /* 0..1 */
    uint32_t wpos;            /* end of the last region the game wrote (Unlock) */
    int      wseen;
    double   trim;            /* servo rate correction */
    double   target;          /* fill fraction the servo locks to (adapts) */
    double   freq;            /* SetFrequency ratio */
    uint64_t plays, stops, wr_bytes, wr_calls, wr_nonzero;
    uint64_t over;            /* slices in which the cursor overtook the writer */
    unsigned settled;         /* slices mixed since it played from the top (to SETTLE_SLICES) */
    int      starving;        /* the last slice was mixed before the game wrote it */
    double   wtime;           /* when the game last wrote it (mono_s) */
    float    down[8][2];      /* 3 to 8 channels: each one's share of left and right (downmix_gains) */
} SndBuf;

/* A buffer of 3 to 8 channels is mixed down to stereo: the channels in the
 * order of DirectSound's default layout for their number (KSAUDIO_SPEAKER_*:
 * the masks Most Wanted asks for, 0x33 for 4 and 0x3F for 6), each speaker with
 * the ITU-R BS.775 downmix's share (centre and surrounds at -3 dB, LFE left
 * out). */
static void downmix_gains(float g[8][2], uint32_t chans){
    static const uint32_t layout[9] = { 0, 0, 0, 0x7u, 0x33u, 0x37u, 0x3Fu, 0x13Fu, 0x63Fu };
    const float h = 0.70710678f;
    uint32_t mask = chans < 9u ? layout[chans] : 0u, k = 0;
    memset(g, 0, sizeof(float) * 16);
    for (uint32_t bit = 1u; bit && k < chans; bit <<= 1){
        if (!(mask & bit)) continue;
        float l = 0.f, r = 0.f;
        switch (bit){
        case 0x001u: l = 1.f; break;              /* front left */
        case 0x002u: r = 1.f; break;              /* front right */
        case 0x004u: l = r = h; break;            /* front centre */
        case 0x008u: break;                       /* LFE */
        case 0x010u: case 0x200u: l = h; break;   /* back left, side left */
        case 0x020u: case 0x400u: r = h; break;   /* back right, side right */
        case 0x100u: l = r = 0.5f; break;         /* back centre */
        }
        g[k][0] = l; g[k][1] = r; k++;
    }
}

static SndBuf g_snd[MAXSND];
static int    g_sndn = 1;                    /* 0 = no slot */
static int    g_sndfree[MAXSND], g_nsndfree = 0;
static int snd_new_id(void){                  /* g_slock held */
    if (g_nsndfree) return g_sndfree[--g_nsndfree];
    return g_sndn < MAXSND ? g_sndn++ : 0;
}
static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;
static int g_audio_on = 0;                   /* the output is wanted (not NOSOUND) */
static _Atomic int g_pump_started = 0;
static uint64_t g_mixed_frames = 0, g_active_peak = 0;
static uint64_t g_cursor_frames = 0;         /* g_slock: output frames the cursors moved (mix_into, skip_locked) */
static double wall_ahead_locked(void);       /* the wall clock's cursors between slices (below) */
static uint64_t g_replay = 0, g_reissue = 0;
static uint64_t g_out_frames = 0, g_out_zero = 0, g_gap_cb = 0;

static FILE *g_dump = NULL;      /* SNDDUMP=<path>: raw s16le stereo */
static FILE *g_csv  = NULL;      /* SNDCSV=<path>: per-slice servo state */
static uint64_t g_cb_no = 0;
static int g_sndstat = 0, g_sndgap = 0;     /* read once, not per slice (audio.m did) */

static AudioRing g_ring __attribute__((aligned(64)));
#define RH(i) g_ring.h[i]

/* ---- the DirectSound side (com.c), unchanged from audio.m ---------------- */
int audio_register(uint32_t data, uint32_t bytes, uint32_t rate,
                   uint32_t chans, uint32_t bits){
    if (!data || !bytes) return 0;
    if (rate < 4000u || rate > 192000u) rate = 44100u;
    if (chans < 1u || chans > 8u) chans = 1u;
    if (bits != 8u && bits != 16u) bits = 16u;
    pthread_mutex_lock(&g_slock);
    int id = snd_new_id();
    if (id){
        SndBuf *b = &g_snd[id];
        b->data = data; b->bytes = bytes; b->rate = rate;
        b->chans = chans; b->bits = bits;
        b->used = 1; b->playing = 0; b->loop = 0; b->cursor = 0.0; b->vol = 1.f;
        b->wpos = 0; b->wseen = 0; b->trim = 1.0; b->target = 0.0; b->over = 0;
        b->freq = 1.0; b->plays = b->stops = 0;
        b->wr_bytes = b->wr_calls = b->wr_nonzero = 0;
        downmix_gains(b->down, chans);
        fprintf(stderr, "  [sound] buffer #%d: %u Hz, %u channels, %u bit, %u bytes (%.2f s)\n",
                id, rate, chans, bits, bytes,
                (double)bytes / (double)(rate * chans * (bits/8u)));
    }
    pthread_mutex_unlock(&g_slock);
    return id;
}
void audio_play(int id, int loop){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){
        /* a buffer that already plays keeps its cursor: the game re-issues
         * Play on its stream all the time */
        if (!g_snd[id].playing){ g_snd[id].cursor = 0.0; g_snd[id].settled = 0; g_replay++; }
        else g_reissue++;
        g_snd[id].playing = 1;
        g_snd[id].loop = loop;
        g_snd[id].plays++;
    }
    pthread_mutex_unlock(&g_slock);
}
void audio_note_write(int id, uint32_t end_off){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].bytes){
        g_snd[id].wpos = end_off % g_snd[id].bytes;
        g_snd[id].wtime = mono_s();
        g_snd[id].wseen = 1;
    }
    pthread_mutex_unlock(&g_slock);
}
void audio_note_write_stat(int id, uint32_t n, int nonzero){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){ g_snd[id].wr_calls++; g_snd[id].wr_bytes += n;
                         if (nonzero) g_snd[id].wr_nonzero++; }
    pthread_mutex_unlock(&g_slock);
}
void audio_set_freq(int id, uint32_t hz){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].rate){
        if (!hz) g_snd[id].freq = 1.0;                      /* DSBFREQUENCY_ORIGINAL */
        else {
            double f = (double)hz / (double)g_snd[id].rate;
            if (f < 0.05) f = 0.05;
            if (f > 8.00) f = 8.00;
            g_snd[id].freq = f;
        }
    }
    pthread_mutex_unlock(&g_slock);
}
int audio_duplicate(int src){
    if (src <= 0 || src >= MAXSND) return 0;
    int id = 0;
    pthread_mutex_lock(&g_slock);
    if (g_snd[src].used && (id = snd_new_id()) != 0){
        g_snd[id] = g_snd[src];
        g_snd[id].playing = 0; g_snd[id].cursor = 0.0; g_snd[id].trim = 1.0;
        g_snd[id].target = 0.0; g_snd[id].over = 0;
        g_snd[id].plays = g_snd[id].stops = 0;
        g_snd[id].wr_bytes = g_snd[id].wr_calls = g_snd[id].wr_nonzero = 0;
    }
    pthread_mutex_unlock(&g_slock);
    return id;
}
/* com.c, last Release: the mixer holds g_slock while it reads guest PCM, so
 * once this returns the data can be freed. */
void audio_unregister(int id){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){
        g_snd[id].used = 0; g_snd[id].playing = 0; g_snd[id].data = 0;
        if (g_nsndfree < MAXSND) g_sndfree[g_nsndfree++] = id;
    }
    pthread_mutex_unlock(&g_slock);
}
void audio_stop(int id){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used){ g_snd[id].playing = 0; g_snd[id].stops++; }
    pthread_mutex_unlock(&g_slock);
}
/* GetCurrentPosition: the play cursor and a write cursor 50 ms ahead of it
 * (equal cursors made the game write where we play).  The game's writer adds
 * its own 30 ms and margin to the write cursor (sndlead.c), so this is heard
 * as delay on every sound; it is also what covers the game's sound thread
 * waiting for the guest lock, ~70 ms at a race's start.  Measured with four
 * career races at once (2026-10-24): 30 ms made 1-4 skips a run there, 12 ms
 * 4-5, 50 ms none. */
#define WRITE_LEAD_MS 50u
void audio_cursors(int id, uint32_t *play, uint32_t *write){
    uint32_t p = 0, w = 0;
    if (id > 0 && id < MAXSND){
        pthread_mutex_lock(&g_slock);
        if (g_snd[id].used && g_snd[id].bytes){
            SndBuf *b = &g_snd[id];
            uint32_t bpf = b->chans * (b->bits/8u);
            uint32_t sz  = b->bytes;
            double cur = b->cursor, ahead = b->playing ? wall_ahead_locked() : 0.0;
            if (ahead > 0.0 && bpf){                    /* the wall clock's, between slices */
                double nsrc = (double)(sz / bpf);
                cur += ahead * (double)b->rate / (double)OUT_RATE * g_rate_ratio * b->trim * b->freq;
                if (nsrc > 0.0 && cur >= nsrc) cur = fmod(cur, nsrc);
            }
            p = (uint32_t)(cur * (double)bpf) % sz;
            uint32_t lead = (g_snd[id].rate * WRITE_LEAD_MS / 1000u) * bpf;
            if (lead >= sz) lead = sz / 4u;
            w = (p + lead) % sz;
        }
        pthread_mutex_unlock(&g_slock);
    }
    if (play)  *play  = p;
    if (write) *write = w;
}
double audio_time(void){ return (double)g_mixed_frames / (double)OUT_RATE; }
uint32_t audio_cursor(int id){ uint32_t p; audio_cursors(id, &p, 0); return p; }
void audio_set_volume(int id, long millibel){
    if (id <= 0 || id >= MAXSND) return;
    float v = (millibel <= -10000L) ? 0.f : powf(10.f, (float)millibel / 2000.f);
    if (v > 1.f) v = 1.f;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used) g_snd[id].vol = v;
    pthread_mutex_unlock(&g_slock);
}
/* win32_ext.c GetMessageA: while the game is parked its clock stands still
 * and nothing refills the stream, so the output is muted; the cursors keep
 * moving (see audio.m). */
static _Atomic int g_out_muted = 0;
void audio_set_muted(int on){
    if (atomic_exchange(&g_out_muted, on ? 1 : 0) != (on ? 1 : 0)){
        atomic_store(&RH(AR_MUTED), on ? 1u : 0u);
        fprintf(stderr, "sound: output %s\n", on ? "muted (game inactive)" : "unmuted");
    }
}

/* ---- the resampler for a buffer at another rate ---------------------------
 * A buffer at 44,100 Hz played at its own frequency mixes as it always did,
 * sample for sample (the step is 1).  One at another rate, or one the game
 * gave another frequency (SetFrequency), is resampled band-limited instead of
 * by the nearest sample, which doubled every sample of a 22,050 Hz mix (Most
 * Wanted's at a low performance level, GAME.md 7.1) and mirrored its band
 * into the octave above: a 16-tap Blackman-windowed sinc in 256 phases, its
 * cut-off at the lower of the two Nyquist rates.  Measured on Most Wanted's
 * 22,050 Hz menu mix: 16.5-22 kHz holds -75 dB of the energy, -22 dB by the
 * nearest sample.  A looping buffer's taps wrap around it; a one-shot's
 * beyond its ends are silence. */
#define RS_TAPS   16
#define RS_PHASES 256
#define RS_CACHE  8
typedef struct { int key; float w[RS_PHASES + 1][RS_TAPS]; } RsTable;
static RsTable *g_rs[RS_CACHE];
static int g_rs_n = 0;
static int g_rs_nearest = 0;                  /* NFSU2_AUDIO_NEAREST=1 (test-only): the nearest sample, as before */
/* the weights for a cut-off of fc (of the source's Nyquist rate, 0 < fc <= 1),
 * fc in 1/64 steps; every phase's weights add up to 1 (g_slock held) */
static const RsTable *rs_table(double fc){
    int key = (int)(fc * 64.0 + 0.5);
    if (key < 1) key = 1;
    if (key > 64) key = 64;
    for (int i = 0; i < g_rs_n; i++) if (g_rs[i]->key == key) return g_rs[i];
    RsTable *t = g_rs_n < RS_CACHE ? malloc(sizeof *t) : g_rs[RS_CACHE - 1];
    if (!t) return 0;
    if (g_rs_n < RS_CACHE) g_rs[g_rs_n++] = t;
    double f = key / 64.0, half = RS_TAPS / 2;
    t->key = key;
    for (int p = 0; p <= RS_PHASES; p++){
        double frac = (double)p / RS_PHASES, sum = 0.0, w[RS_TAPS];
        for (int j = 0; j < RS_TAPS; j++){
            double x = (double)(j - (RS_TAPS / 2 - 1)) - frac;              /* the tap's distance from the position */
            double s = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * f * x) / (M_PI * f * x);
            double win = fabs(x) >= half ? 0.0 : 0.42 + 0.5 * cos(M_PI * x / half) + 0.08 * cos(2.0 * M_PI * x / half);
            w[j] = f * s * win; sum += w[j];
        }
        for (int j = 0; j < RS_TAPS; j++) t->w[p][j] = (float)(sum != 0.0 ? w[j] / sum : 0.0);
    }
    return t;
}

/* ---- the mixer: audio.m's mix_into, float output ------------------------ */
static void mix_into(float *out, unsigned frames){
    static float acc[FRAMES * OUT_CH];
    if (frames > FRAMES) frames = FRAMES;
    memset(acc, 0, sizeof(float) * frames * OUT_CH);
    uint64_t active = 0;
    update_rate_ratio();
    if (g_rate_ratio < g_win_rmin) g_win_rmin = g_rate_ratio;
    if (g_rate_ratio > g_win_rmax) g_win_rmax = g_rate_ratio;

    pthread_mutex_lock(&g_slock);
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used || !b->playing || !b->bytes) continue;
        uint32_t bps  = b->bits / 8u;
        uint32_t bpf  = b->chans * bps;
        if (!bpf) continue;
        uint32_t nsrc = b->bytes / bpf;
        if (!nsrc) continue;
        /* audio.m locks the play cursor to the game's write position (a
         * PLL, +-3 %) against a free-running cursor drifting through the
         * unwritten region.  The game's writer follows the play cursor
         * instead: a quick race at a constant 1.03 kept the same fill (0.31
         * to 0.38 of the buffer) as one at 1.00, no overruns either way.  The
         * trim had nothing to correct; its target lagged each change of the
         * game's fill level by ~15 s, so it sat at 1.03 or 0.97 for half a
         * minute at a time: the music's tempo and pitch wandered by a
         * semitone.  Off by default (SNDSERVO=1 turns it on); the fill and
         * the overrun count are still measured for the status line. */
        if (b->wseen){
            uint32_t pbyte = ((uint32_t)(b->cursor * (double)bpf)) % b->bytes;
            uint32_t avail = (b->wpos + b->bytes - pbyte) % b->bytes;
            double frac = (double)avail / (double)b->bytes;
            double want;
            if (frac < g_win_fmin) g_win_fmin = frac;
            if (frac > g_win_fmax) g_win_fmax = frac;
            if (frac > 0.85) g_win_over++;
            if (frac > 0.85){
                b->over++;                         /* the cursor overtook the writer */
                want = 0.94;
            } else {
                if (b->target <= 0.0) b->target = frac;
                else b->target += (frac - b->target) * 0.0008;
                if (b->target < 0.12) b->target = 0.12;
                if (b->target > 0.60) b->target = 0.60;
                want = 1.0 + (frac - b->target) * 1.5;
                if (want < 0.97) want = 0.97;
                if (want > 1.03) want = 1.03;
            }
            b->trim += (want - b->trim) * 0.05;
            if (!g_servo) b->trim = 1.0;
        } else b->trim += (1.0 - b->trim) * 0.08;
        if (b->trim < 0.5 || b->trim > 2.0) b->trim = 1.0;
        if (b->wseen){
            if (b->trim < g_win_tmin) g_win_tmin = b->trim;
            if (b->trim > g_win_tmax) g_win_tmax = b->trim;
        }
        if (g_csv){
            uint32_t pb = ((uint32_t)(b->cursor * (double)bpf)) % b->bytes;
            uint32_t av = b->wseen ? ((b->wpos + b->bytes - pb) % b->bytes) : 0u;
            fprintf(g_csv, "%llu,%.4f,%d,%u,%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,",
                    (unsigned long long)g_cb_no,
                    (double)g_mixed_frames/(double)OUT_RATE, i, b->rate, b->bytes,
                    pb, b->wpos, av, (double)av/(double)b->bytes,
                    b->target, b->trim, g_rate_ratio);
        }
        double step = (double)b->rate / (double)OUT_RATE * g_rate_ratio * b->trim * b->freq;
        active++;
        if (b->settled < SETTLE_SLICES) b->settled++;
        else if (b->wseen && !atomic_load(&g_out_muted) && !atomic_load(&g_reported)){
            /* has the game written the part this slice plays?  (it writes at
             * most 50 + 30 + 200 ms ahead: more is the cursor past the writer) */
            uint32_t pbyte = ((uint32_t)(b->cursor * (double)bpf)) % b->bytes;
            uint32_t avail = (b->wpos + b->bytes - pbyte) % b->bytes;
            if ((double)avail > 0.3 * (double)b->rate * (double)bpf || (double)avail < step * (double)frames * (double)bpf){
                g_starved++; g_win_starved++;
                /* the first slice of a skip says how late the game's writer is */
                if (!b->starving && ++g_skips <= 20)
                    fprintf(stderr, "sound: a skip at %.1f s: the game last wrote its sound %.0f ms ago\n",
                            (double)g_mixed_frames / OUT_RATE, (mono_s() - b->wtime) * 1000.0);
                b->starving = 1;
            } else {
                b->starving = 0;
                double ms = (double)avail * 1000.0 / ((double)b->rate * (double)bpf);
                g_lead_sum += ms; g_lead_n++;
                if (ms > g_lead_max) g_lead_max = ms;
                if (ms < g_win_lmin) g_win_lmin = ms;
                if (ms > g_win_lmax) g_win_lmax = ms;
            }
        }
        const RsTable *rs = (b->rate != OUT_RATE || b->freq != 1.0) && !g_rs_nearest ? rs_table(step > 1.0 ? 1.0 / step : 1.0) : 0;
        for (unsigned f = 0; f < frames; f++){
            double sp = b->cursor + step * (double)f;
            if (sp >= (double)nsrc){
                if (b->loop) sp = fmod(sp, (double)nsrc);
                else { b->playing = 0; break; }
            }
            uint32_t si = (uint32_t)sp;
            if (rs){                               /* another rate: band-limited (rs_table) */
                const float *w = rs->w[(int)((sp - (double)si) * RS_PHASES + 0.5)];
                float ch[8] = { 0 };
                for (int j = 0; j < RS_TAPS; j++){
                    int64_t k = (int64_t)si + j - (RS_TAPS / 2 - 1);
                    if (k < 0 || k >= (int64_t)nsrc){
                        if (!b->loop) continue;
                        k = ((k % (int64_t)nsrc) + (int64_t)nsrc) % (int64_t)nsrc;
                    }
                    uint32_t off = b->data + (uint32_t)k * bpf;
                    for (uint32_t q = 0; q < b->chans; q++){
                        float s;
                        if (b->bits == 16u){ int16_t v; memcpy(&v, g_mem + off + q*bps, 2); s = (float)v / 32768.f; }
                        else s = ((float)g_mem[off + q*bps] - 128.f) / 128.f;
                        ch[q] += s * w[j];
                    }
                }
                float l, r;
                if (b->chans == 1u) l = r = ch[0];
                else if (b->chans == 2u){ l = ch[0]; r = ch[1]; }
                else { l = r = 0.f; for (uint32_t q = 0; q < b->chans; q++){ l += ch[q] * b->down[q][0]; r += ch[q] * b->down[q][1]; } }
                acc[f*OUT_CH] += l * b->vol; acc[f*OUT_CH + 1] += r * b->vol;
                continue;
            }
            if (b->chans > 2u){                    /* mixed down (downmix_gains) */
                float l = 0.f, r = 0.f;
                for (uint32_t k = 0; k < b->chans; k++){
                    uint32_t off = b->data + si*bpf + k*bps;
                    float s;
                    if (b->bits == 16u){ int16_t v; memcpy(&v, g_mem + off, 2); s = (float)v / 32768.f; }
                    else s = ((float)g_mem[off] - 128.f) / 128.f;
                    l += s * b->down[k][0]; r += s * b->down[k][1];
                }
                acc[f*OUT_CH] += l * b->vol; acc[f*OUT_CH + 1] += r * b->vol;
                continue;
            }
            for (int c = 0; c < OUT_CH; c++){
                uint32_t sc = (b->chans == 2u) ? (uint32_t)c : 0u;
                uint32_t off = b->data + si*bpf + sc*bps;
                float s;
                if (b->bits == 16u){
                    int16_t v; memcpy(&v, g_mem + off, 2);
                    s = (float)v / 32768.f;
                } else {
                    s = ((float)g_mem[off] - 128.f) / 128.f;
                }
                acc[f*OUT_CH + c] += s * b->vol;
            }
        }
        if (g_csv){
            uint32_t p0 = ((uint32_t)(b->cursor * (double)bpf)) % b->bytes;
            uint32_t nby = (uint32_t)(step * (double)frames * (double)bpf);
            if (nby > b->bytes) nby = b->bytes;
            uint32_t z = 0;
            for (uint32_t q = 0; q < nby; q++)
                if (!g_mem[b->data + ((p0 + q) % b->bytes)]) z++;
            fprintf(g_csv, "%.4f\n", nby ? (double)z/(double)nby : 0.0);
        }
        b->cursor += step * (double)frames;
        if (b->cursor >= (double)nsrc){
            if (b->loop) b->cursor = fmod(b->cursor, (double)nsrc);
            else { b->cursor = 0.0; b->playing = 0; }
        }
    }
    g_cursor_frames += frames;
    pthread_mutex_unlock(&g_slock);
    if (active > g_active_peak) g_active_peak = active;
    g_mixed_frames += frames;
    if (g_sndstat){
        static uint64_t next = 0;
        if (g_mixed_frames >= next){
            next = g_mixed_frames + (uint64_t)OUT_RATE * 2ull;
            pthread_mutex_lock(&g_slock);
            fprintf(stderr, "  [state] ");
            for (int i2 = 1; i2 < g_sndn; i2++){
                if (!g_snd[i2].used) continue;
                uint32_t nz = 0, lim = g_snd[i2].bytes < 8192u ? g_snd[i2].bytes : 8192u;
                for (uint32_t q = 0; q < lim; q++) if (g_mem[g_snd[i2].data + q]) nz++;
                fprintf(stderr, "#%d[%s%s %uHz data=%u%%] ", i2,
                        g_snd[i2].playing ? "play" : "stop",
                        g_snd[i2].loop ? "+loop" : "",
                        g_snd[i2].rate, lim ? (unsigned)(100u*nz/lim) : 0u);
            }
            fprintf(stderr, "\n");
            pthread_mutex_unlock(&g_slock);
        }
    }
    /* silent data while something plays = a region the game has not written */
    if (active){
        unsigned zero = 0;
        for (unsigned k = 0; k < frames * OUT_CH; k++) if (acc[k] == 0.f) zero++;
        g_out_frames += frames * OUT_CH;
        g_out_zero   += zero;
        if (zero > (frames * OUT_CH) * 3u / 4u){
            g_gap_cb++;
            static int shown = 0;
            if (g_sndgap && shown < 40 && g_mixed_frames > (uint64_t)OUT_RATE * 5ull){ shown++;
                fprintf(stderr, "  [gap] t=%.2f s\n", (double)g_mixed_frames / (double)OUT_RATE); }
        }
    }
    horn_mix(acc, frames, OUT_RATE);              /* free-roam horns over the game's sound */
    carsfx_mix(acc, frames, OUT_RATE);            /* ...and friends' nitrous and hydraulics */
    int muted = atomic_load(&g_out_muted);
    for (unsigned k = 0; k < frames * OUT_CH; k++){
        float v = muted ? 0.f : acc[k];
        if (v >  1.f) v =  1.f;
        if (v < -1.f) v = -1.f;
        out[k] = v;
    }
    g_cb_no++;
    if (g_dump){
        int16_t s16[FRAMES * OUT_CH];
        for (unsigned k = 0; k < frames * OUT_CH; k++) s16[k] = (int16_t)(out[k] * 32767.f);
        fwrite(s16, sizeof(int16_t), (size_t)frames * OUT_CH, g_dump);
    }
}

/* the wall clock's long backlog: every playing buffer's cursor moves as far as
 * mix_into would have moved it in frames, nothing is read or mixed (the horns
 * and friends' car sounds, which free roam plays over the game's, wait) */
static uint64_t g_skipped_frames = 0;
static int g_wall_catchup = 0;                /* the wall clock's backlog rule (sound_wall_catchup, NFSU2_AUDIO_CATCHUP) */
static void skip_locked(uint64_t frames){            /* g_slock held */
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used || !b->playing || !b->bytes) continue;
        uint32_t bpf = b->chans * (b->bits / 8u);
        uint32_t nsrc = bpf ? b->bytes / bpf : 0u;
        if (!nsrc) continue;
        b->cursor += (double)b->rate / (double)OUT_RATE * g_rate_ratio * b->trim * b->freq * (double)frames;
        if (b->cursor >= (double)nsrc){
            if (b->loop) b->cursor = fmod(b->cursor, (double)nsrc);
            else { b->cursor = 0.0; b->playing = 0; }
        }
        b->starving = 0;
    }
    g_cursor_frames += frames;
}
static void skip_frames(uint64_t frames){
    update_rate_ratio();
    pthread_mutex_lock(&g_slock);
    skip_locked(frames);
    pthread_mutex_unlock(&g_slock);
    g_mixed_frames += frames;
    g_skipped_frames += frames;
}
/* The wall clock's cursors between the pump's slices (sound_wall_catchup).  A
 * cursor that moves only when the pump mixes moves in 11.6 ms steps, and in
 * steps as long as the pump's sleeps when its timers are coarse; Most
 * Wanted's sound thread, which polls every ~10 ms and faster while the game
 * loads, counted up to 24 polls without a move at a boot's load.  So on the
 * wall clock the cursors a game reads run on with the time since the frames
 * the pump mixed were due, at most WALL_INTERP_S ahead (nothing is heard
 * there; what the game writes ahead of them is never overtaken: it writes
 * ahead of the write cursor).  Leaving the wall clock for the ring, the pump
 * moves them on to where they were read last, so no cursor ever goes back. */
#define WALL_INTERP_S 0.25
static int      g_wall_interp = 0;            /* g_slock: the cursors run on between slices */
static uint64_t g_wall_base = 0;              /* g_slock: g_cursor_frames when the wall clock took over, */
static double   g_wall_since = 0.0;           /* ... and when (mono_s) */
static double wall_ahead_locked(void){        /* g_slock held: frames the cursors run ahead of the mixed ones */
    if (!g_wall_interp) return 0.0;
    double a = (mono_s() - g_wall_since) * OUT_RATE - (double)(g_cursor_frames - g_wall_base);
    return a < 0.0 ? 0.0 : a > OUT_RATE * WALL_INTERP_S ? OUT_RATE * WALL_INTERP_S : a;
}
static void wall_interp_begin(double now){
    pthread_mutex_lock(&g_slock);
    g_wall_interp = g_wall_catchup; g_wall_base = g_cursor_frames; g_wall_since = now;
    pthread_mutex_unlock(&g_slock);
}
static void wall_interp_end(void){
    pthread_mutex_lock(&g_slock);
    double a = wall_ahead_locked();
    uint64_t n = (uint64_t)ceil(a);
    if (n) skip_locked(n);
    g_wall_interp = 0;
    pthread_mutex_unlock(&g_slock);
    g_mixed_frames += n; g_skipped_frames += n;
}

/* ---- output statistics: what goes to the ring --------------------------- */
static struct {
    double sumsq; float peak; uint64_t n;            /* the current one-second window */
    float last_peak, last_rms;                       /* the last complete window */
    uint64_t slices, loud_slices;                    /* slices with any sample above 1 LSB */
    float max_peak;                                  /* over the whole run */
} g_st;
static void stats_add(const float *o, unsigned frames){
    float pk = 0.f; double ss = 0.0;
    for (unsigned k = 0; k < frames * OUT_CH; k++){ float a = fabsf(o[k]); if (a > pk) pk = a; ss += (double)o[k] * o[k]; }
    g_st.slices++;
    if (pk > 1.f / 32768.f) g_st.loud_slices++;
    if (pk > g_st.peak) g_st.peak = pk;
    if (pk > g_st.max_peak) g_st.max_peak = pk;
    g_st.sumsq += ss; g_st.n += frames * OUT_CH;
    if (g_st.n >= (uint64_t)OUT_RATE * OUT_CH){
        g_st.last_peak = g_st.peak;
        g_st.last_rms = (float)sqrt(g_st.sumsq / (double)g_st.n);
        atomic_store(&RH(AR_PEAK), (uint32_t)(g_st.last_peak * 65536.f));
        atomic_store(&RH(AR_RMS), (uint32_t)(g_st.last_rms * 65536.f));
        g_st.peak = 0.f; g_st.sumsq = 0.0; g_st.n = 0;
    }
}
static double dbfs(float v){ return v > 1e-7f ? 20.0 * log10((double)v) : -140.0; }

/* ---- the pump -------------------------------------------------------------- */
static uint64_t g_ring_frames = 0, g_wall_frames = 0;
static double   g_mix_s = 0.0, g_mix_max_s = 0.0;   /* time inside mix_into: total, longest slice */
static void mix_timed(float *out, unsigned frames){
    double t = mono_s();
    mix_into(out, frames);
    t = mono_s() - t;
    g_mix_s += t; if (t > g_mix_max_s) g_mix_max_s = t;
}
static uint64_t g_to_wall = 0, g_to_ring = 0, g_target_raised = 0, g_target_lowered = 0;
static double   g_wall_s = 0.0, g_pump_t0 = 0.0, g_ring_s0 = 0.0;
static unsigned g_target = 1024;
/* the least the target comes back down to: the start (NFSU2_AUDIO_TARGET), or
 * the size above one a lowering left that then underran */
static unsigned g_target_floor = 1024;
static unsigned g_target_start = 1024;
static double   g_status_every = 0.0;
/* the part of the ring in use: what the page's worklet reads (AR_PAGE_FRAMES) */
static unsigned g_ring_n = AUDIO_RING_FRAMES_OLD;
static unsigned target_max(void){ return g_ring_n - SLICE; }

/* the worklet's rhythm (RHYTHM_*): the longest gap between two of its reads in
 * each of the last seconds, and g_rhythm_ms, their median with the current
 * second's (0 until a gap was seen) */
static double g_gapwin[RHYTHM_N];
static int    g_ngapwin = 0;
static double g_rhythm_ms = 0.0;
static double median(double *v, int n){
    for (int i = 1; i < n; i++){ double x = v[i]; int j = i; for (; j > 0 && v[j-1] > x; j--) v[j] = v[j-1]; v[j] = x; }
    return v[n / 2];
}
static void rhythm_update(double cur){              /* cur < 0: the current second has none yet */
    double v[RHYTHM_N + 1]; int n = 0;
    for (int i = 0; i < g_ngapwin; i++) v[n++] = g_gapwin[i];
    if (cur >= 0.0) v[n++] = cur;
    if (n) g_rhythm_ms = median(v, n);
}
static void rhythm_window(double max){              /* a second ended: its longest gap joins the last ones */
    if (g_ngapwin == RHYTHM_N){ memmove(g_gapwin, g_gapwin + 1, sizeof g_gapwin[0] * (RHYTHM_N - 1)); g_ngapwin--; }
    g_gapwin[g_ngapwin++] = max;
    rhythm_update(-1.0);
}
static double stall_ms(void){ return 2.0 * g_rhythm_ms > STALL_MS ? 2.0 * g_rhythm_ms : STALL_MS; }

/* the worklet's bursts (BURST_*): g_burst_ms, the median time from the start
 * of one to the start of the next; a late burst does not make the next one
 * bigger, so this, not the longest gap, says how much one takes */
static double g_bper[BURST_N];
static int    g_nbper = 0, g_ibper = 0;
static double g_burst_ms = 0.0;
static void burst_add(double ms){
    g_bper[g_ibper] = ms; g_ibper = (g_ibper + 1) % BURST_N;
    if (g_nbper < BURST_N) g_nbper++;
    double v[BURST_N];
    memcpy(v, g_bper, sizeof v[0] * (size_t)g_nbper);
    double m = median(v, g_nbper);                  /* sorts v */
    /* the first two (the start, ~0.3 s): the shorter, as a load's hitch is no burst */
    g_burst_ms = g_nbper < 3 ? v[0] : m;
}
/* the ring target that holds one burst and a slice: only for a worklet whose
 * bursts are more than twice the start target.  A desktop's (two callbacks of
 * 10-12 ms) are about the start target, and the pump tops the ring up between
 * the two; the underruns below cover what that misses, as before. */
static unsigned burst_need(void){
    if (!g_nbper) return 0;
    double f = g_burst_ms * OUT_RATE / 1000.0;
    if (f <= 2.0 * g_target_start) return 0;
    unsigned need = (((unsigned)f + 127u) & ~127u) + SLICE;     /* whole quanta: less churn */
    return need < target_max() ? need : target_max();
}
/* autotest "audiostall MS" (test-only): the pump mixes nothing for MS ms, so
 * the worklet underruns as on a starved device */
static _Atomic int g_test_stall_ms = 0;
void audio_test_stall(int ms){ atomic_store(&g_test_stall_ms, ms > 0 ? ms : 1); }
static _Atomic uint32_t g_never = 0;          /* a word nothing notifies: a plain timed sleep */
/* the wall clock's long sleeps (g_wall_catchup), and NFSU2_AUDIO_WAKE_MS (test-only) */
static int g_wake_ms = 0;
static uint64_t g_wall_skips = 0;

static void status_line(double now, int mode){
    uint32_t w = atomic_load(&RH(AR_W)), r = atomic_load(&RH(AR_R));
    char cur[160] = ""; int cl = 0;
    pthread_mutex_lock(&g_slock);
    for (int i = 1; i < g_sndn && cl < (int)sizeof cur - 32; i++)
        if (g_snd[i].used && g_snd[i].playing){
            uint32_t bpf = g_snd[i].chans * (g_snd[i].bits / 8u);
            cl += snprintf(cur + cl, sizeof cur - (size_t)cl, " #%d@%u", i,
                           (uint32_t)(g_snd[i].cursor * (double)bpf) % (g_snd[i].bytes ? g_snd[i].bytes : 1u));
        }
    pthread_mutex_unlock(&g_slock);
    static const char *const ctxn[] = { "none", "running", "suspended", "closed" };
    uint32_t ctx = atomic_load(&RH(AR_CTX));
    static double last_t = 0, last_mix = 0;
    double el = now - (last_t ? last_t : g_pump_t0), cpu = el > 0 ? (g_mix_s - last_mix) / el * 100.0 : 0.0;
    last_t = now; last_mix = g_mix_s;
    fprintf(stderr, "[audio] t=%.1fs mode=%s ctx=%s fill=%u target=%u rhythm=%.0fms burst=%.0fms mixed=%.1fs wall=%.1fs mixcpu=%.2f%%"
            " peak=%.1fdBFS rms=%.1fdBFS loud=%.0f%% underruns=%u worklet_loud=%u muted=%d ratio=%.3f..%.3f trim=%.3f..%.3f"
            " frac=%.2f..%.2f over=%llu lead=%.0f..%.0fms starved=%llu play:%s\n",
            now - g_pump_t0, mode == AR_MODE_RING ? "ring" : mode == AR_MODE_WALL ? "wall" : "idle",
            ctx < 4 ? ctxn[ctx] : "?", w - r, g_target, g_rhythm_ms, g_burst_ms, (double)g_mixed_frames / OUT_RATE, g_wall_s, cpu,
            dbfs(g_st.last_peak), dbfs(g_st.last_rms),
            g_st.slices ? 100.0 * (double)g_st.loud_slices / (double)g_st.slices : 0.0,
            atomic_load(&RH(AR_UNDERRUNS)), atomic_load(&RH(AR_LOUD)), atomic_load(&g_out_muted),
            g_win_rmin > g_win_rmax ? 1.0 : g_win_rmin, g_win_rmin > g_win_rmax ? 1.0 : g_win_rmax,
            g_win_tmin > g_win_tmax ? 1.0 : g_win_tmin, g_win_tmin > g_win_tmax ? 1.0 : g_win_tmax,
            g_win_fmin > g_win_fmax ? 0.0 : g_win_fmin, g_win_fmin > g_win_fmax ? 0.0 : g_win_fmax,
            (unsigned long long)g_win_over, g_win_lmin > g_win_lmax ? 0.0 : g_win_lmin, g_win_lmax,
            (unsigned long long)g_win_starved, cl ? cur : " -");
    g_win_rmin = g_win_tmin = g_win_fmin = 9.0; g_win_rmax = g_win_tmax = g_win_fmax = 0.0; g_win_over = 0;
    g_win_lmin = 1e9; g_win_lmax = 0.0; g_win_starved = 0;
}

/* wait until *addr != expect or the timeout: the worklet notifies AR_R */
static void wait_word(_Atomic uint32_t *addr, uint32_t expect, double max_s){
    if (max_s <= 0) return;
    __builtin_wasm_memory_atomic_wait32((int32_t *)addr, (int32_t)expect, (int64_t)(max_s * 1e9));
}

static void *pump_main(void *unused){
    (void)unused;
    static float scratch[SLICE * OUT_CH];
    int mode = AR_MODE_IDLE;
    double now = mono_s();
    g_pump_t0 = now;
    double t_r = now, wall_t0 = now, next_status = now + g_status_every;
    uint64_t wall_done = 0;
    uint32_t r_seen = atomic_load(&RH(AR_R)), under_seen = 0;
    double t_quiet = now, t_lowered = 0;        /* the last underrun or lowering; the last lowering */
    int lowered = 0;                            /* the target is one a lowering left */
    double grace_end = 0, t_loop = now;         /* no stall before grace_end; the last time round the loop */
    double t_win = now, win_max = -1.0;         /* this second of the rhythm, its longest gap (-1: none) */
    double t_burst = -1.0;                      /* the start of the worklet's last burst (-1: not known) */
    double t_due = 0;                           /* ring mode: when the next slice is due at the pace */
    uint32_t fill_seen = 0;                     /* the fill the last time round */
    int need_lines = 0;
    for (;;){
        now = mono_s();
        double slept = (now - t_loop) * 1000.0; t_loop = now;
        if (g_status_every > 0 && now >= next_status){ status_line(now, mode); next_status = now + g_status_every; }
        int stall = atomic_exchange(&g_test_stall_ms, 0);
        if (stall){
            fprintf(stderr, "sound: audiostall: the pump mixes nothing for %d ms (test-only)\n", stall);
            wait_word(&g_never, 0, stall / 1000.0);
            continue;
        }
        uint32_t r = atomic_load(&RH(AR_R)), w = atomic_load(&RH(AR_W));
        uint32_t fill = w - r;
        if (fill > g_ring_n) fill = g_ring_n;      /* never: one producer, one consumer */
        if (r != r_seen){
            /* what the time since its last read says about the worklet, unless
             * that read was before this ring session, the ring had too little
             * for a quantum (the pump's fault), or the pump slept through it */
            int valid = mode == AR_MODE_RING && t_r >= g_ring_s0 && fill_seen >= 128u && slept < RHYTHM_WAKE_MS;
            double gap = (now - t_r) * 1000.0;
            if (valid && gap > win_max){ win_max = gap; rhythm_update(win_max); }
            if (gap >= BURST_SPLIT_MS){                         /* a burst begins */
                if (valid && t_burst >= g_ring_s0) burst_add((now - t_burst) * 1000.0);
                t_burst = valid ? now : -1.0;
            }
            r_seen = r; t_r = now;
        }
        fill_seen = fill;
        if (now - t_win >= RHYTHM_WIN_S){
            if (win_max >= 0.0) rhythm_window(win_max);
            t_win = now; win_max = -1.0;
        }
        int running = atomic_load(&RH(AR_CTX)) == AR_CTX_RUNNING;
        /* the worklet is consuming: it moved r recently (within two of its
         * longest gaps), or the ring has room it has not been asked to drain yet */
        int consumer = g_audio_on && running &&
                       (now < grace_end || (now - t_r) * 1000.0 < stall_ms() || fill + SLICE <= g_target);
        if (consumer){
            if (mode != AR_MODE_RING){
                if (mode == AR_MODE_WALL){ g_to_ring++; wall_interp_end(); }
                /* the page says which part of the ring its worklet reads
                 * before it reports a running context */
                uint32_t pf = atomic_load(&RH(AR_PAGE_FRAMES));
                g_ring_n = pf >= AUDIO_RING_FRAMES_OLD && pf <= AUDIO_RING_FRAMES && !(pf & (pf - 1u)) ? pf : AUDIO_RING_FRAMES_OLD;
                if (g_target > target_max()) g_target = target_max();
                if (g_target_floor > g_target) g_target_floor = g_target;
                fprintf(stderr, "sound: output paced by the AudioWorklet (ring %u frames, target %u = %.1f ms)\n",
                        g_ring_n, g_target, g_target * 1000.0 / OUT_RATE);
                mode = AR_MODE_RING; atomic_store(&RH(AR_MODE), AR_MODE_RING);
                g_ring_s0 = now; under_seen = atomic_load(&RH(AR_UNDERRUNS)); t_quiet = now;
                /* a worklet that was just connected takes a moment to run
                 * its first quantum, and one that reads in bursts its first
                 * gaps to learn: do not call that a stall */
                grace_end = now + START_GRACE_S;
                t_burst = -1.0; t_due = now;
            }
            /* a burst must fit in the ring: no underrun every burst while the
             * slice-at-a-time raise below catches up */
            unsigned need = burst_need();
            if (g_target < need){
                g_target = need;
                atomic_store(&RH(AR_TARGET), g_target);
                if (++need_lines <= 10)
                    fprintf(stderr, "sound: the worklet reads in bursts %.0f ms apart: the ring holds one, target %u frames (%.1f ms)\n",
                            g_burst_ms, g_target, g_target * 1000.0 / OUT_RATE);
            }
            /* underruns once the output has settled: keep more in the ring.
             * SHRINK_QUIET_S without one: a slice less again, down to the
             * floor.  An underrun at a size a lowering left, however much
             * later, makes the size before it the floor: a device that needs
             * the room (a hitch every minute, say) gets one click more than
             * before, not one after every lowering. */
            uint32_t un = atomic_load(&RH(AR_UNDERRUNS));
            /* ...and not below a burst and a quantum: one whose length jitters
             * across a rounding step does not move it every SHRINK_QUIET_S */
            unsigned keep = need ? need + 128u : 0u;
            unsigned lowest = g_target_floor > keep ? g_target_floor : keep;
            if (un != under_seen){
                if (now - g_ring_s0 > 1.0 && g_target + SLICE <= target_max()){
                    if (lowered){
                        g_target_floor = g_target + SLICE;
                        fprintf(stderr, "sound: worklet underrun %.0f s after the ring target was lowered: it stays at %u frames (%.1f ms) or more\n",
                                now - t_lowered, g_target_floor, g_target_floor * 1000.0 / OUT_RATE);
                    }
                    lowered = 0;
                    g_target += SLICE; g_target_raised++;
                    atomic_store(&RH(AR_TARGET), g_target);
                    fprintf(stderr, "sound: worklet underrun (%u so far): ring target raised to %u frames (%.1f ms)\n",
                            un, g_target, g_target * 1000.0 / OUT_RATE);
                }
                under_seen = un; t_quiet = now;
            } else if (g_target > lowest && now - t_quiet >= SHRINK_QUIET_S){
                g_target = g_target - SLICE > lowest ? g_target - SLICE : lowest;
                g_target_lowered++; t_quiet = t_lowered = now; lowered = 1;
                atomic_store(&RH(AR_TARGET), g_target);
                fprintf(stderr, "sound: no worklet underrun for %.0f s: ring target lowered to %u frames (%.1f ms)\n",
                        SHRINK_QUIET_S, g_target, g_target * 1000.0 / OUT_RATE);
            }
            if (fill + SLICE > g_target){              /* full: no time is banked while it waits */
                if (t_due < now) t_due = now;
                wait_word(&RH(AR_R), r, 0.004);
                continue;
            }
            /* room: with big bursts, a slice when it is due at the pace, or at
             * once when the ring runs low (a burst came early, the pump was
             * late); with a desktop's, at once, as ever (the cursor's steps
             * are small next to the game's write-ahead) */
            if (need && fill >= SLICE && now < t_due){
                double s = t_due - now;
                wait_word(&RH(AR_R), r, s < 0.004 ? s : 0.004);
                continue;
            }
            float *dst = &g_ring.pcm[(w & (g_ring_n - 1u)) * OUT_CH];
            /* SLICE divides the ring size: a slice never wraps */
            mix_timed(dst, SLICE);
            stats_add(dst, SLICE);
            atomic_store(&RH(AR_W), w + SLICE);
            atomic_store(&RH(AR_MIXED), (uint32_t)g_mixed_frames);
            g_ring_frames += SLICE;
            if (t_due < now - CATCHUP_S) t_due = now - CATCHUP_S;
            t_due += (double)SLICE / (OUT_RATE * PACE);
            continue;
        }
        /* no consumer: keep the cursors moving on the wall clock */
        if (mode != AR_MODE_WALL){
            if (mode == AR_MODE_RING) g_to_wall++;
            fprintf(stderr, "sound: %s: the mixer runs on the wall clock (DirectSound cursors keep moving, output dropped)\n",
                    !g_audio_on ? "sound is off (NOSOUND)" : !running ? "no running AudioContext" : "the AudioWorklet stopped consuming");
            mode = AR_MODE_WALL; atomic_store(&RH(AR_MODE), AR_MODE_WALL);
            wall_t0 = now; wall_done = 0;
            wall_interp_begin(now);
        }
        double due = (now - wall_t0) * OUT_RATE - (double)wall_done;
        if (!g_wall_catchup){
            if (due > OUT_RATE * WALL_DROP_S){     /* the pump was starved: do not burst-mix the backlog */
                wall_done = (uint64_t)((now - wall_t0) * OUT_RATE) - SLICE;
                due = SLICE;
            }
        } else if (due > OUT_RATE * WALL_MIX_S){   /* asleep for long: skip to the last WALL_DROP_S, mix that */
            uint64_t skip = (uint64_t)(due - OUT_RATE * WALL_DROP_S) / SLICE * SLICE;
            skip_frames(skip);
            wall_done += skip; g_wall_frames += skip; g_wall_skips++;
            if (g_wall_skips <= 10)
                fprintf(stderr, "sound: the pump slept %.0f ms on the wall clock: the DirectSound cursors skip %.0f ms of it\n",
                        due * 1000.0 / OUT_RATE, (double)skip * 1000.0 / OUT_RATE);
            due -= (double)skip;
        }
        if (due >= SLICE){
            mix_timed(scratch, SLICE);
            stats_add(scratch, SLICE);
            wall_done += SLICE; g_wall_frames += SLICE;
            g_wall_s = (double)g_wall_frames / OUT_RATE;
            atomic_store(&RH(AR_WALL_MS), (uint32_t)(g_wall_s * 1000.0));
            atomic_store(&RH(AR_MIXED), (uint32_t)g_mixed_frames);
            continue;
        }
        double sleep_s = (SLICE - due) / OUT_RATE;
        if (sleep_s > 0.005) sleep_s = 0.005;     /* notice a consumer coming back */
        if (g_wake_ms > 0){ wait_word(&g_never, 0, g_wake_ms / 1000.0); continue; }   /* test-only */
        wait_word(&RH(AR_R), r, sleep_s);
    }
    return 0;
}

void audio_init(void){
    if (atomic_exchange(&g_pump_started, 1)) return;
    g_audio_on = !env_on("NOSOUND");
    g_sndstat = getenv("SNDSTAT") != NULL;
    g_sndgap = getenv("SNDGAP") != NULL;
    g_servo = env_on("SNDSERVO");
    { const char *e = getenv("NFSU2_AUDIO_TARGET"); int t = e && *e ? atoi(e) : 0;
      if (t >= SLICE && t <= (int)(AUDIO_RING_FRAMES - SLICE)) g_target = g_target_floor = g_target_start = (unsigned)t; }   /* any size: a slice is mixed when fill + SLICE <= target */
    { const char *e = getenv("NFSU2_AUDIO_STATUS");
      if (e && *e) g_status_every = atof(e);
      else if (getenv("NFSU2_STATUS_EVERY")) g_status_every = 5.0; }
    { const char *e = getenv("NFSU2_AUDIO_CATCHUP");
      g_wall_catchup = e && *e ? *e != '0' : g_game.sound_wall_catchup;
      e = getenv("NFSU2_AUDIO_WAKE_MS"); g_wake_ms = e && *e ? atoi(e) : 0;
      if (g_wake_ms < 0 || g_wake_ms > 5000) g_wake_ms = 0;
      if (g_wake_ms) fprintf(stderr, "sound: NFSU2_AUDIO_WAKE_MS: on the wall clock the pump wakes every %d ms (test-only)\n", g_wake_ms);
      g_rs_nearest = env_on("NFSU2_AUDIO_NEAREST");
      if (g_rs_nearest) fprintf(stderr, "sound: NFSU2_AUDIO_NEAREST: buffers at another rate by the nearest sample (test-only)\n"); }
    { const char *d = getenv("SNDDUMP");
      if (d && *d){ g_dump = fopen(d, "wb");
          fprintf(stderr, "sound: output dump -> %s (%d Hz s16le %d channels)\n", d, OUT_RATE, OUT_CH); } }
    { const char *d = getenv("SNDCSV");
      if (d && *d){ g_csv = fopen(d, "w");
          if (g_csv) fprintf(g_csv, "cb,t,id,rate,bytes,pbyte,wpos,avail,frac,target,trim,ratio,zero\n"); } }
    RH(AR_MAGIC) = AUDIO_MAGIC; RH(AR_VERSION) = AUDIO_VERSION;
    RH(AR_FRAMES) = AUDIO_RING_FRAMES; RH(AR_RATE) = OUT_RATE;
    RH(AR_TARGET) = g_target; RH(AR_NOSOUND) = g_audio_on ? 0u : 1u;
    nfsu2_register(HOST_KIND_AUDIO, &g_ring, (uint32_t)sizeof g_ring);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256u * 1024u);
    pthread_t th;
    if (pthread_create(&th, &at, pump_main, 0) != 0){
        fprintf(stderr, "sound: the audio pump thread could not start: the DirectSound cursors stand still\n");
        return;
    }
    pthread_detach(th);
    fprintf(stderr, "sound: %s (audio pump thread, %d Hz float stereo ring at 0x%08x, %u frames)\n",
            g_audio_on ? "Web Audio output open" : "sound OFF (NOSOUND), the mixer still runs",
            OUT_RATE, (unsigned)(uintptr_t)&g_ring, AUDIO_RING_FRAMES);
}

/* autotest "audioreset" (on the Mac: the CoreAudio device-change path).  The
 * browser follows the default output device by itself; here the page is
 * asked to rebuild its AudioContext, which exercises the stall fallback. */
static uint64_t g_dev_changes = 0;
void audio_device_changed(const char *why){
    g_dev_changes++;
    atomic_fetch_add(&RH(AR_RESET), 1u);
    fprintf(stderr, "sound: output reset requested (%s): the page rebuilds its AudioContext\n", why);
}

void audio_report(void){
    atomic_store(&g_reported, 1);
    if (g_dump){ fflush(g_dump); fclose(g_dump); g_dump = NULL; }
    if (g_csv){  fflush(g_csv);  fclose(g_csv);  g_csv  = NULL; }
    fprintf(stderr, "  sound output      : %s, %llu frames mixed, "
            "at most %llu buffers at once, %d registered\n",
            g_audio_on ? "on" : "OFF",
            (unsigned long long)g_mixed_frames,
            (unsigned long long)g_active_peak, g_sndn - 1);
    fprintf(stderr, "  sound Play calls  : %llu started from the top, %llu were already playing\n",
            (unsigned long long)g_replay, (unsigned long long)g_reissue);
    fprintf(stderr, "  sound rate ratio : %.3f (game virtual time / real time)%s\n",
            g_rate_ratio, g_sync_mode ? "" : "  [off; SNDSYNC=1 turns it on]");
    fprintf(stderr, "  sound data gaps   : %.1f%% of samples silent, %llu callbacks mostly empty\n",
            g_out_frames ? 100.0*(double)g_out_zero/(double)g_out_frames : 0.0,
            (unsigned long long)g_gap_cb);
    fprintf(stderr, "  --- per sound buffer ---\n");
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used) continue;
        fprintf(stderr, "   #%-3d %6u Hz %uch %5.2f s   Play=%-4llu Stop=%-4llu  "
                "writes=%llu (%llu non-empty, %.1f KB)  %s\n",
                i, b->rate, b->chans,
                (double)b->bytes/(double)(b->rate*b->chans*(b->bits/8u)),
                (unsigned long long)b->plays, (unsigned long long)b->stops,
                (unsigned long long)b->wr_calls, (unsigned long long)b->wr_nonzero,
                b->wr_bytes/1024.0, b->playing ? "PLAYING" : "stopped");
    }
    { uint64_t ov = 0; for (int i = 1; i < g_sndn; i++) ov += g_snd[i].over;
      fprintf(stderr, "  sound cursor overrun: in %llu callbacks the play cursor passed the writer\n",
              (unsigned long long)ov); }
    fprintf(stderr, "  sound write-ahead : the game wrote %.1f ms ahead of the mixer on average, at most %.1f ms; "
            "%llu slices mixed before the game wrote them, in %llu skips\n",
            g_lead_n ? g_lead_sum / (double)g_lead_n : 0.0, g_lead_max, (unsigned long long)g_starved,
            (unsigned long long)g_skips);
    fprintf(stderr, "  sound output path : AudioWorklet ring, %u-frame slices, target %u frames (%.1f ms), "
            "%u underruns, target grew %llu times and came down %llu times (to %u frames at least); "
            "the worklet's reads up to %.0f ms apart, its bursts %.0f ms apart, a ring of %u frames\n",
            SLICE, g_target, g_target * 1000.0 / OUT_RATE, atomic_load(&RH(AR_UNDERRUNS)),
            (unsigned long long)g_target_raised, (unsigned long long)g_target_lowered, g_target_floor,
            g_rhythm_ms, g_burst_ms, g_ring_n);
    fprintf(stderr, "  sound clock backup: %.1f s on the ring, %.1f s on the wall clock (no output), %llu times ring->wall, %llu times wall->ring\n",
            (double)g_ring_frames / OUT_RATE, g_wall_s, (unsigned long long)g_to_wall, (unsigned long long)g_to_ring);
    if (g_wall_catchup)
        fprintf(stderr, "  sound wall clock  : every backlog made up (sound_wall_catchup), %.1f s of it skipped in %llu long sleeps\n",
                (double)g_skipped_frames / OUT_RATE, (unsigned long long)g_wall_skips);
    fprintf(stderr, "  sound mixer       : %.3f s in total (%.2f%% of the mixed time), longest slice %.3f ms\n",
            g_mix_s, g_mixed_frames ? 100.0 * g_mix_s / ((double)g_mixed_frames / OUT_RATE) : 0.0, g_mix_max_s * 1000.0);
    fprintf(stderr, "  sound output level: peak %.1f dBFS, %.1f%% of slices not silent, worklet %u non-silent quanta\n",
            dbfs(g_st.max_peak), g_st.slices ? 100.0 * (double)g_st.loud_slices / (double)g_st.slices : 0.0,
            atomic_load(&RH(AR_LOUD)));
    fprintf(stderr, "  sound output device: %llu reset requests (the browser follows the default device)\n",
            (unsigned long long)g_dev_changes);
}
