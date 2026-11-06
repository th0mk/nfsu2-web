/* CoreAudio output.
 *
 * Until now the DirectSound layer only did bookkeeping (written bytes were
 * counted, the play cursor advanced with elapsed time) but there was NO
 * SOUND OUTPUT AT ALL.  The game writes real PCM (measured: ~9 MB, 5000+
 * non-empty writes); here we mix those buffers and play them with AudioQueue.
 *
 * The mixer converts to 44.1 kHz, 2 channel, 16-bit output; the source
 * buffers can differ in rate/channels (nearest-neighbour resampling). */
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudio/CoreAudio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>
#include <stdatomic.h>
#include "horn.h"
#include "carsfx.h"
#include "whine.h"

extern uint8_t *g_mem;
uint64_t host_us(void);          /* the game's VIRTUAL clock (16.67 ms per frame) */

/* The game produces sound data in its own virtual time, while the mixer
 * consumed it in real time.  When the emulator stays below 60 fps the virtual
 * time falls behind and every lap of the buffer leaves an unwritten region ->
 * the sound cuts out at regular intervals (measured: the gap period is exactly
 * the buffer length).  Fix: tie consumption to the game's production rate. */
static double g_rate_ratio = 1.0;
static int    g_sync_mode = -1;      /* SNDSYNC: 1 = tie to the game's rate */
static void update_rate_ratio(void){
    if (g_sync_mode < 0){
        const char *e = getenv("SNDSYNC");
        g_sync_mode = (e && *e) ? (*e != '0') : 1;      /* default: on */
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
            /* CEILING 1.0: sound must not play FASTER THAN REAL TIME.  This
             * ratio came from the game's clock, and the clock (because of the
             * creep bug) ran up to 2x fast at times -> songs played fast.
             * The clock is fixed, but we make the link safe as well: this
             * way a clock bug can never again turn into a PITCH bug. */
            if (k > 1.00) k = 1.00;
            /* smooth: sudden frame-rate swings must not make the pitch jump */
            g_rate_ratio += (k - g_rate_ratio) * 0.10;
        }
    }
    lv = vus; lr = rus;
}

#define OUT_RATE   44100
#define OUT_CH     2
/* The emulator saturates the CPU, so 3x1024 (~70 ms) of buffering is not
 * enough; when a callback was late the queue ran dry and the sound cut out. */
#define NBUF       6
#define FRAMES     2048

#define MAXSND 128
typedef struct {
    uint32_t data, bytes, rate, chans, bits;
    int      used, playing, loop;
    double   cursor;          /* in source samples */
    float    vol;             /* 0..1 */
    uint32_t wpos;            /* end of the last byte the game wrote */
    int      wseen;           /* have we seen any write */
    double   trim;            /* rate correction (servo) */
    double   target;          /* fill ratio to lock to (adapts) */
    double   freq;            /* SetFrequency ratio (1.0 = original rate) */
    uint64_t plays, stops, wr_bytes, wr_calls, wr_nonzero;
    uint64_t over;            /* callbacks in which we passed the writer */
} SndBuf;

static SndBuf g_snd[MAXSND];
static int    g_sndn = 1;                    /* 0 = invalid id */
/* v12na: ids of released buffers are reused.  The game creates two
 * buffers per race load and releases them at the end; with ids never
 * reused the 128-entry table would have run out after ~60 loads and every
 * later sound would have been silent. */
static int    g_sndfree[MAXSND], g_nsndfree = 0;
static int snd_new_id(void){                  /* g_slock held */
    if (g_nsndfree) return g_sndfree[--g_nsndfree];
    return g_sndn < MAXSND ? g_sndn++ : 0;
}
static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;
static AudioQueueRef        g_q;
static AudioQueueBufferRef  g_qb[NBUF];
static AudioUnit            g_au;            /* low-latency path */
static int g_audio_on = 0;
static int g_backend = 0;                    /* 1 = AudioUnit, 2 = AudioQueue, 3 = silent clock */
static unsigned g_au_slice = 0;              /* frames per callback */
static uint64_t g_mixed_frames = 0, g_active_peak = 0;
static uint64_t g_replay = 0, g_reissue = 0, g_starved = 0, g_worst_gap_us = 0;
static uint64_t g_out_frames = 0, g_out_zero = 0, g_gap_cb = 0;

/* --- diagnostics: dump the mixer output and buffer state to disk --- */
static FILE *g_dump = NULL;      /* SNDDUMP=<path> : raw s16le stereo */
static FILE *g_csv  = NULL;      /* SNDCSV=<path>  : state per callback */
static uint64_t g_cb_no = 0;

int audio_register(uint32_t data, uint32_t bytes, uint32_t rate,
                   uint32_t chans, uint32_t bits){
    if (!data || !bytes) return 0;
    if (rate < 4000u || rate > 192000u) rate = 44100u;
    if (chans < 1u || chans > 2u) chans = 1u;
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
        /* CAUTION: do NOT RESET the cursor of a buffer that is already
         * playing.  The game calls Play again and again on its streaming
         * buffers; jumping back to the start on every call made the song
         * stutter "stop-start". */
        if (!g_snd[id].playing){ g_snd[id].cursor = 0.0; g_replay++; }
        else g_reissue++;
        g_snd[id].playing = 1;
        g_snd[id].loop = loop;
        g_snd[id].plays++;
    }
    pthread_mutex_unlock(&g_slock);
}
/* From the guest's Unlock: the END offset of the written region.  The mixer
 * keeps the play cursor a fixed distance behind it (servo) - so neither is an
 * unwritten region played (gap) nor does the cursor fall behind (latency). */
void audio_note_write(int id, uint32_t end_off){
    if (id <= 0 || id >= MAXSND) return;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used && g_snd[id].bytes){
        g_snd[id].wpos = end_off % g_snd[id].bytes;
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
/* DirectSound SetFrequency: changes the playback sample rate.
 * The game sets the PITCH of effects such as the engine sound this way;
 * without it every sound plays at its original rate. */
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
/* A second sound sharing the same PCM data (DuplicateSoundBuffer).
 * The game plays SIMULTANEOUS effects this way; when the duplicate came
 * back empty they were all silent. */
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
/* com.c, last Release of a DirectSound buffer: the mixer holds g_slock while
 * it reads guest PCM, so once this returns the data can be freed. */
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
/* For the guest's GetCurrentPosition.
 * DirectSound returns two cursors: PLAY and WRITE.  The write cursor is
 * AHEAD of the play cursor (the safe limit the hardware is reading up to);
 * the game writes data from there on.  Returning both as equal made the game
 * write exactly where we were playing and the sound stutter "stop-start". */
#define WRITE_LEAD_MS 50u
void audio_cursors(int id, uint32_t *play, uint32_t *write){
    uint32_t p = 0, w = 0;
    if (id > 0 && id < MAXSND){
        pthread_mutex_lock(&g_slock);
        if (g_snd[id].used && g_snd[id].bytes){
            uint32_t bpf = g_snd[id].chans * (g_snd[id].bits/8u);
            uint32_t sz  = g_snd[id].bytes;
            p = (uint32_t)(g_snd[id].cursor * (double)bpf) % sz;
            uint32_t lead = (g_snd[id].rate * WRITE_LEAD_MS / 1000u) * bpf;
            if (lead >= sz) lead = sz / 4u;
            w = (p + lead) % sz;
        }
        pthread_mutex_unlock(&g_slock);
    }
    if (play)  *play  = p;
    if (write) *write = w;
}
/* The mixer's time axis (same t as the SNDDUMP file).  For aligning
 * diagnostic logs with the sound dump. */
double audio_time(void){ return (double)g_mixed_frames / (double)OUT_RATE; }
uint32_t audio_cursor(int id){ uint32_t p; audio_cursors(id, &p, 0); return p; }
void audio_set_volume(int id, long millibel){
    if (id <= 0 || id >= MAXSND) return;
    /* DirectSound: 0 = full volume, -10000 = silent (fully logarithmic) */
    float v = (millibel <= -10000L) ? 0.f : powf(10.f, (float)millibel / 2000.f);
    if (v > 1.f) v = 1.f;
    pthread_mutex_lock(&g_slock);
    if (g_snd[id].used) g_snd[id].vol = v;
    pthread_mutex_unlock(&g_slock);
}

/* win32_ext.c GetMessageA: while the game's main thread is parked there
 * (the Mac app is in the background, see lifecycle.c) the game's clock
 * stands still, so nothing refills the streaming buffer and it would loop
 * its last half second over and over.  The output is muted meanwhile.  The
 * cursors keep moving as usual: the game's sound thread polls them, and
 * cursors that stand still make it recreate its buffers without end (tried:
 * 120 new buffers in 4 s, then a crash in 0x727030). */
static _Atomic int g_out_muted = 0;
/* NFSU2_MUTE=1 (test runs, tools/autorun.sh and tools/apptest.sh): silent
 * speakers, everything else as usual.  Unlike NOSOUND the output unit runs,
 * so the mixer, the cursors and SNDDUMP behave exactly as with sound. */
static int g_test_mute = 0;
void audio_set_muted(int on){
    if (atomic_exchange(&g_out_muted, on ? 1 : 0) != (on ? 1 : 0))
        fprintf(stderr, "sound: output %s\n", on ? "muted (game inactive)" : "unmuted");
}

static void mix_into(int16_t *out, unsigned frames){
    static float acc[FRAMES * OUT_CH];
    if (frames > FRAMES) frames = FRAMES;
    memset(acc, 0, sizeof(float) * frames * OUT_CH);
    uint64_t active = 0;
    update_rate_ratio();

    pthread_mutex_lock(&g_slock);
    for (int i = 1; i < g_sndn; i++){
        SndBuf *b = &g_snd[i];
        if (!b->used || !b->playing || !b->bytes) continue;
        uint32_t bps  = b->bits / 8u;
        uint32_t bpf  = b->chans * bps;
        if (!bpf) continue;
        uint32_t nsrc = b->bytes / bpf;
        if (!nsrc) continue;
        /* ONE-WAY servo: the rate ratio (feedforward) already matches
         * consumption to production.  The servo only steps in and slows
         * down WHEN THE WRITTEN DATA AHEAD OF US RUNS LOW; that way an
         * unwritten region is never played.  A two-way servo had made
         * things worse when its target was picked wrong. */
        /* --- lock the play cursor to the game's WRITE cursor (PLL) ---
         * A free-running cursor, even with a 1% rate difference, slowly
         * drifts through the buffer and passes the unwritten region once
         * per lap.  Measured: 50-95 ms of silence exactly every 30 s, even
         * with a rate ratio of 0.99.  Period = buffer_length / rate_error,
         * so feedforward makes the error smaller but does NOT REMOVE it.
         * Fix: keep correcting the consumption rate so that the amount of
         * written data (avail) stays constant.  The target is the game's
         * own natural equilibrium -- picking a fixed number would be wrong
         * (0.25 was picked at first; the game runs between 0.30 and 0.64). */
        if (b->wseen){
            uint32_t bpf2 = b->chans * (b->bits/8u);
            uint32_t pbyte = ((uint32_t)(b->cursor * (double)bpf2)) % b->bytes;
            uint32_t avail = (b->wpos + b->bytes - pbyte) % b->bytes;
            double frac = (double)avail / (double)b->bytes;
            double want;
            if (frac > 0.85){
                /* the cursor PASSED the writer (avail looks large because
                 * of the modulo wrap).  Slow down hard instead of servoing
                 * toward the target, and leave the target alone. */
                b->over++;
                want = 0.94;
            } else {
                if (b->target <= 0.0) b->target = frac;
                else b->target += (frac - b->target) * 0.0008;   /* very slow */
                if (b->target < 0.12) b->target = 0.12;
                if (b->target > 0.60) b->target = 0.60;
                /* small Kp + narrow clamp: the pitch change stays below 3
                 * cents, i.e. inaudible; but the drift disappears entirely. */
                want = 1.0 + (frac - b->target) * 1.5;
                if (want < 0.97) want = 0.97;
                if (want > 1.03) want = 1.03;
            }
            b->trim += (want - b->trim) * 0.05;
        } else b->trim += (1.0 - b->trim) * 0.08;
        if (b->trim < 0.5 || b->trim > 2.0) b->trim = 1.0;
        if (g_csv){
            uint32_t bpf3 = b->chans * (b->bits/8u);
            uint32_t pb = ((uint32_t)(b->cursor * (double)bpf3)) % b->bytes;
            uint32_t av = b->wseen ? ((b->wpos + b->bytes - pb) % b->bytes) : 0u;
            fprintf(g_csv, "%llu,%.4f,%d,%u,%u,%u,%u,%u,%.4f,%.4f,%.4f,%.4f,",
                    (unsigned long long)g_cb_no,
                    (double)g_mixed_frames/(double)OUT_RATE, i, b->rate, b->bytes,
                    pb, b->wpos, av, (double)av/(double)b->bytes,
                    b->target, b->trim, g_rate_ratio);
        }
        double step = (double)b->rate / (double)OUT_RATE * g_rate_ratio * b->trim * b->freq;
        active++;
        for (unsigned f = 0; f < frames; f++){
            double sp = b->cursor + step * (double)f;
            if (sp >= (double)nsrc){
                if (b->loop) sp = fmod(sp, (double)nsrc);
                else { b->playing = 0; break; }
            }
            uint32_t si = (uint32_t)sp;
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
            /* how many bytes are zero in the source region this buffer READ? */
            uint32_t bpf4 = b->chans * (b->bits/8u);
            uint32_t p0 = ((uint32_t)(b->cursor * (double)bpf4)) % b->bytes;
            uint32_t nby = (uint32_t)(step * (double)frames * (double)bpf4);
            if (nby > b->bytes) nby = b->bytes;
            uint32_t z = 0;
            for (uint32_t q = 0; q < nby; q++)
                if (!g_mem[b->data + ((p0 + q) % b->bytes)]) z++;
            fprintf(g_csv, "%.4f\n", nby ? (double)z/(double)nby : 0.0);
        }
        b->cursor += step * (double)frames;
        if (nsrc){
            if (b->cursor >= (double)nsrc){
                if (b->loop) b->cursor = fmod(b->cursor, (double)nsrc);
                else { b->cursor = 0.0; b->playing = 0; }
            }
        }
    }
    pthread_mutex_unlock(&g_slock);
    if (active > g_active_peak) g_active_peak = active;
    g_mixed_frames += frames;
    /* Periodic state: which buffer is playing, does it hold data? */
    if (getenv("SNDSTAT")){
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

    /* Is the data we read really silent?  If so the game has NOT YET
     * WRITTEN that region (our cursor is passing the writer). */
    if (active){
        unsigned zero = 0;
        for (unsigned k = 0; k < frames * OUT_CH; k++) if (acc[k] == 0.f) zero++;
        g_out_frames += frames * OUT_CH;
        g_out_zero   += zero;
        if (zero > (frames * OUT_CH) * 3u / 4u){
            g_gap_cb++;
            static int shown = 0;
            if (getenv("SNDGAP") && shown < 40 &&
                g_mixed_frames > (uint64_t)OUT_RATE * 5ull){ shown++;
                fprintf(stderr, "  [gap] t=%.2f s\n",
                        (double)g_mixed_frames / (double)OUT_RATE);
            }
            if (0){
                fprintf(stderr, "  [gap] playing buffers:");
                pthread_mutex_lock(&g_slock);
                for (int i2 = 1; i2 < g_sndn; i2++)
                    if (g_snd[i2].used && g_snd[i2].playing)
                        fprintf(stderr, " #%d(cursor=%.0f/%u loop=%d)", i2,
                                g_snd[i2].cursor,
                                g_snd[i2].bytes / (g_snd[i2].chans*(g_snd[i2].bits/8u)),
                                g_snd[i2].loop);
                pthread_mutex_unlock(&g_slock);
                fprintf(stderr, "\n");
            }
        }
    }
    horn_mix(acc, frames, OUT_RATE);              /* free-roam horns over the game's sound */
    carsfx_mix(acc, frames, OUT_RATE);            /* ...and friends' nitrous and hydraulics */
    whine_mix(acc, frames, OUT_RATE);             /* ...and the gearbox whine (gearbox_whine) */
    int muted = atomic_load(&g_out_muted);
    for (unsigned k = 0; k < frames * OUT_CH; k++){
        float v = muted ? 0.f : acc[k];
        if (v >  1.f) v =  1.f;
        if (v < -1.f) v = -1.f;
        out[k] = (int16_t)(v * 32767.f);
    }
    g_cb_no++;
    if (g_dump) fwrite(out, sizeof(int16_t), (size_t)frames * OUT_CH, g_dump);
    if (g_test_mute) memset(out, 0, sizeof(int16_t) * frames * OUT_CH);
}

static uint64_t now_us_a(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000ull + (uint64_t)ts.tv_nsec/1000ull;
}
static void cb(void *ud, AudioQueueRef q, AudioQueueBufferRef buf){
    (void)ud;
    /* If the time between callbacks exceeds the buffer length, the queue
     * RAN DRY, i.e. the sound cut out.  This is measured. */
    { static uint64_t last = 0;
      uint64_t now = now_us_a();
      uint64_t budget = (uint64_t)FRAMES * 1000000ull / (uint64_t)OUT_RATE;
      if (last && now - last > budget * (uint64_t)NBUF) g_starved++;
      if (last){ uint64_t d = now - last; if (d > g_worst_gap_us) g_worst_gap_us = d; }
      last = now; }
    mix_into((int16_t *)buf->mAudioData, FRAMES);
    buf->mAudioDataByteSize = FRAMES * OUT_CH * sizeof(int16_t);
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

/* AudioUnit render callback.  The only difference from the AudioQueue path is
 * that the output goes straight into the device's ABL, not a queue buffer. */
static OSStatus au_cb(void *ud, AudioUnitRenderActionFlags *flags,
                      const AudioTimeStamp *ts, UInt32 bus, UInt32 nframes,
                      AudioBufferList *abl){
    (void)ud; (void)flags; (void)ts; (void)bus;
    { static uint64_t last = 0;
      uint64_t now = now_us_a();
      if (last){ uint64_t d = now - last; if (d > g_worst_gap_us) g_worst_gap_us = d; }
      last = now; }
    if (!abl || abl->mNumberBuffers < 1) return noErr;
    int16_t *out = (int16_t *)abl->mBuffers[0].mData;
    if (!out) return noErr;
    if (nframes > (UInt32)FRAMES) nframes = (UInt32)FRAMES;   /* acc array bound */
    if (nframes > g_au_slice) g_au_slice = nframes;
    mix_into(out, (unsigned)nframes);
    abl->mBuffers[0].mDataByteSize = nframes * OUT_CH * sizeof(int16_t);
    return noErr;
}

/* AudioQueue queues 6 x 2048 frames: 278.6 ms.  That was latency that does
 * NOT exist on Windows, added entirely by us, on top of the game's own
 * DirectSound latency (~275 ms).  AudioUnit runs at the device's own
 * slice (usually 512 frames = 11.6 ms). */
static int au_start(const AudioStreamBasicDescription *f){
    AudioComponentDescription d;
    memset(&d, 0, sizeof d);
    d.componentType         = kAudioUnitType_Output;
    d.componentSubType      = kAudioUnitSubType_DefaultOutput;
    d.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp) return 0;
    if (AudioComponentInstanceNew(comp, &g_au) != noErr) return 0;
    if (AudioUnitSetProperty(g_au, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, f, sizeof *f) != noErr) goto fail;
    { UInt32 mx = FRAMES;
      AudioUnitSetProperty(g_au, kAudioUnitProperty_MaximumFramesPerSlice,
                           kAudioUnitScope_Global, 0, &mx, sizeof mx); }
    { AURenderCallbackStruct rc; rc.inputProc = au_cb; rc.inputProcRefCon = NULL;
      if (AudioUnitSetProperty(g_au, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &rc, sizeof rc) != noErr) goto fail; }
    if (AudioUnitInitialize(g_au) != noErr) goto fail;
    if (AudioOutputUnitStart(g_au) != noErr){ AudioUnitUninitialize(g_au); goto fail; }
    return 1;
fail:
    AudioComponentInstanceDispose(g_au); g_au = NULL; return 0;
}

/* ---- default output device changes --------------------------------------
 * Headphones plugged in, AirPods connected, another output picked in Control
 * Center: the DefaultOutput unit follows the system default device by
 * itself, but one that disappears under it can leave the unit stopped.  So
 * every change is logged and, once the switch has settled, the unit is
 * restarted if it is no longer running, or rebuilt if that fails.  The
 * mixer state (g_snd) is untouched; playback just continues on the new
 * device.  autotest "audioreset" forces the rebuild path. */
#define AUDIO_ELEM_MAIN 0     /* kAudioObjectPropertyElementMain (macOS 12+) */
static AudioStreamBasicDescription g_fmt;
static dispatch_queue_t g_audio_q;
static uint64_t g_dev_changes = 0, g_au_restarts = 0, g_au_rebuilds = 0;
static void log_default_device(const char *why){
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal, AUDIO_ELEM_MAIN };
    AudioDeviceID dev = 0; UInt32 sz = sizeof dev;
    char name[128] = "?";
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) == noErr && dev){
        CFStringRef cs = NULL; sz = sizeof cs;
        AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                          AUDIO_ELEM_MAIN };
        if (AudioObjectGetPropertyData(dev, &na, 0, NULL, &sz, &cs) == noErr && cs){
            CFStringGetCString(cs, name, sizeof name, kCFStringEncodingUTF8); CFRelease(cs); }
    }
    fprintf(stderr, "sound: default output device: \"%s\" (id %u, %s)\n", name, (unsigned)dev, why);
}
static int null_out_start(const char *why);
static void null_out_stop(void);
/* the silent clock stands in for an output unit that could not be rebuilt:
 * the next device change tries the unit again (au_check) */
static int g_null_for_au;
/* NFSU2_AUDIO_FAIL_REBUILD=<n> (tests): the next n rebuilds fail, as when
 * the new default device cannot be opened (autotest audioreset) */
static int g_fail_rebuilds = -1;
static int au_start_rebuild(void){
    if (g_fail_rebuilds < 0){ const char *e = getenv("NFSU2_AUDIO_FAIL_REBUILD"); g_fail_rebuilds = e ? atoi(e) : 0; }
    if (g_fail_rebuilds > 0){ g_fail_rebuilds--; fprintf(stderr, "sound: rebuild made to fail (NFSU2_AUDIO_FAIL_REBUILD)\n"); return 0; }
    return au_start(&g_fmt);
}
static void au_rebuild(void){
    if (g_au){ AudioOutputUnitStop(g_au); AudioUnitUninitialize(g_au);
               AudioComponentInstanceDispose(g_au); g_au = NULL; }
    g_au_rebuilds++;
    if (au_start_rebuild()) fprintf(stderr, "sound: output unit rebuilt on the current default device\n");
    else {
        fprintf(stderr, "sound: output unit could NOT be rebuilt, sound is off until the next device change\n");
        /* g_audio_on stays 1 all the way (a CreateSoundBuffer in between must
         * not open a second output); 0 only if not even the clock runs */
        if (null_out_start("the output unit could not be rebuilt")) g_null_for_au = 1;
        else g_audio_on = 0;
    }
}
static void au_check(int rebuild){
    if (g_backend == 3 && g_null_for_au){
        /* a device came back (or another was picked): the unit again, the
         * silent clock stopped first so only one of them mixes */
        null_out_stop();
        g_au_rebuilds++;
        if (au_start_rebuild()){
            g_backend = 1; g_null_for_au = 0;
            fprintf(stderr, "sound: output unit rebuilt on the current default device, sound is back\n");
        } else if (!null_out_start("the output unit could still not be rebuilt")) g_audio_on = 0;
        return;
    }
    if (g_backend != 1) return;
    UInt32 run = 0, sz = sizeof run;
    if (!rebuild && g_au &&
        AudioUnitGetProperty(g_au, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0, &run, &sz) == noErr && run)
        return;
    if (!rebuild && g_au && AudioOutputUnitStart(g_au) == noErr){
        g_au_restarts++; fprintf(stderr, "sound: output unit had stopped, restarted\n"); return; }
    au_rebuild();
}
void audio_device_changed(const char *why){
    int rebuild = !strncmp(why, "autotest", 8);
    g_dev_changes++;
    if (!g_audio_q) return;
    dispatch_async(g_audio_q, ^{ log_default_device(why); });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), g_audio_q, ^{ au_check(rebuild); });
}
static void watch_default_device(void){
    g_audio_q = dispatch_queue_create("nfsu2.audio.device", DISPATCH_QUEUE_SERIAL);
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal, AUDIO_ELEM_MAIN };
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &a, g_audio_q,
        ^(UInt32 n, const AudioObjectPropertyAddress *ad){ (void)n; (void)ad;
            audio_device_changed("system default changed"); });
    log_default_device("at start");
}

/* No sound device (settings.ini "sound = off", NOSOUND=1, or no output unit
 * could be opened): a thread plays the mixer into nothing at the device's
 * pace, so the DirectSound cursors move on real time as with sound.  The
 * game's sound thread polls them; cursors that stand still make it create
 * its streaming buffer again and again while it holds the mutex the main
 * thread waits for: the game hung at the title splash (UG_LS_Splash, 15.9 s
 * without a Present, a 400 MB log of "[sound] buffer #1" lines in 70 s) since
 * buffers are released (feat/heap).  Nothing reaches CoreAudio. */
#define NULL_SLICE 512u
static atomic_int g_null_stop, g_null_live;      /* silent clock: asked to stop / its thread runs */
static void *null_out_main(void *ud){
    (void)ud;
    static int16_t out[NULL_SLICE * OUT_CH];
    const uint64_t slice_ns = (uint64_t)NULL_SLICE * 1000000000ull / (uint64_t)OUT_RATE;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t due = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    while (!atomic_load(&g_null_stop)){
        mix_into(out, NULL_SLICE);
        if (g_au_slice < NULL_SLICE) g_au_slice = NULL_SLICE;
        due += slice_ns;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        if (now > due + 100000000ull) due = now;          /* fell 100 ms behind: no burst */
        else if (due > now){
            struct timespec d = { (time_t)((due - now) / 1000000000ull), (long)((due - now) % 1000000000ull) };
            nanosleep(&d, NULL);
        }
    }
    atomic_store(&g_null_live, 0);
    return NULL;
}
static int null_out_start(const char *why){
    pthread_t t;
    atomic_store(&g_null_stop, 0); atomic_store(&g_null_live, 1);
    if (pthread_create(&t, NULL, null_out_main, NULL) != 0){
        atomic_store(&g_null_live, 0);
        fprintf(stderr, "sound: no output (%s), and the silent clock could not start\n", why); return 0; }
    pthread_detach(t);
    g_backend = 3; g_audio_on = 1;
    fprintf(stderr, "sound: no output (%s): the mixer runs on a silent clock\n", why);
    return 1;
}
/* waits until the clock's thread is out of mix_into (a slice is 10.7 ms;
 * logged if that takes more than a second) */
static void null_out_stop(void){
    atomic_store(&g_null_stop, 1);
    for (int i = 0; atomic_load(&g_null_live); i++){
        if (i == 1000) fprintf(stderr, "sound: the silent clock is slow to stop (over a second)\n");
        struct timespec d = { 0, 1000000L }; nanosleep(&d, NULL); }
}

void audio_init(void){
    if (g_audio_on) return;
    { const char *e = getenv("NOSOUND"); if (e && *e && *e != '0'){ null_out_start("sound = off / NOSOUND"); return; } }
    { const char *e = getenv("NFSU2_MUTE"); g_test_mute = e && *e && *e != '0';
      if (g_test_mute) fprintf(stderr, "sound: speakers silent (NFSU2_MUTE; the mixer runs as usual)\n"); }
    { const char *d = getenv("SNDDUMP");
      if (d && *d){ g_dump = fopen(d, "wb");
          fprintf(stderr, "sound: output dump -> %s (%d Hz s16le %d channels)\n", d, OUT_RATE, OUT_CH); } }
    { const char *d = getenv("SNDCSV");
      if (d && *d){ g_csv = fopen(d, "w");
          if (g_csv) fprintf(g_csv, "cb,t,id,rate,bytes,pbyte,wpos,avail,frac,target,trim,ratio,zero\n"); } }
    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate       = OUT_RATE;
    f.mFormatID         = kAudioFormatLinearPCM;
    f.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = OUT_CH;
    f.mBitsPerChannel   = 16;
    f.mFramesPerPacket  = 1;
    f.mBytesPerFrame    = OUT_CH * sizeof(int16_t);
    f.mBytesPerPacket   = f.mBytesPerFrame;
    /* SNDBACKEND=queue goes back to the old (high-latency) path. */
    { const char *e = getenv("SNDBACKEND");
      if (!(e && *e == 'q') && au_start(&f)){
          g_backend = 1; g_audio_on = 1; g_fmt = f;
          fprintf(stderr, "sound: CoreAudio output open (AudioUnit, %d Hz, %d channels)\n",
                  OUT_RATE, OUT_CH);
          watch_default_device();
          return;
      } }
    if (AudioQueueNewOutput(&f, cb, NULL, NULL, NULL, 0, &g_q) != noErr){
        fprintf(stderr, "sound: AudioQueue could not be opened\n"); null_out_start("no output device"); return;
    }
    for (int i = 0; i < NBUF; i++){
        if (AudioQueueAllocateBuffer(g_q, FRAMES*OUT_CH*sizeof(int16_t), &g_qb[i]) != noErr)
            continue;
        memset(g_qb[i]->mAudioData, 0, FRAMES*OUT_CH*sizeof(int16_t));
        g_qb[i]->mAudioDataByteSize = FRAMES*OUT_CH*sizeof(int16_t);
        AudioQueueEnqueueBuffer(g_q, g_qb[i], 0, NULL);
    }
    AudioQueueSetParameter(g_q, kAudioQueueParam_Volume, 1.0f);
    if (AudioQueueStart(g_q, NULL) != noErr){
        fprintf(stderr, "sound: AudioQueue could not be started\n"); null_out_start("no output device"); return;
    }
    g_audio_on = 1; g_backend = 2;
    fprintf(stderr, "sound: CoreAudio output open (AudioQueue, %d Hz, %d channels)\n",
            OUT_RATE, OUT_CH);
}

void audio_report(void){
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
            g_rate_ratio, g_sync_mode ? "" : "  [SNDSYNC=0: off]");
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
    fprintf(stderr, "  sound output path : %s, callback %u frames (%.1f ms queue latency)\n",
            g_backend == 1 ? "AudioUnit" : g_backend == 2 ? "AudioQueue" : g_backend == 3 ? "silent clock" : "none",
            g_backend != 2 ? g_au_slice : (unsigned)(FRAMES*NBUF),
            (g_backend != 2 ? (double)g_au_slice : (double)(FRAMES*NBUF))
                * 1000.0 / (double)OUT_RATE);
    fprintf(stderr, "  sound output device: %llu default-device changes, %llu restarts, %llu rebuilds\n",
            (unsigned long long)g_dev_changes, (unsigned long long)g_au_restarts,
            (unsigned long long)g_au_rebuilds);
    fprintf(stderr, "  sound dropouts    : %llu starved, longest callback gap %.1f ms"
            " (buffer %.1f ms)\n",
            (unsigned long long)g_starved, g_worst_gap_us/1000.0,
            (double)FRAMES*NBUF*1000.0/(double)OUT_RATE);
}
