/* audio_nx.c -- the sound of the game on the console.
 *
 * runtime/web/audio_web.c mixes everything the game plays (EA's own SND mixer into one DirectSound buffer, plus the horn and
 * the car sounds) into a float32 stereo ring at 44.1 kHz, which in the browser the page's AudioWorklet drains.  Here the
 * consumer is this thread: it takes the ring's frames, resamples them to the 48 kHz the console's audout takes (linear
 * interpolation), and plays them in 21 ms buffers.  What the worklet does for the pump (reads counter AR_R and wakes it,
 * AR_UNDERRUNS, the context state AR_CTX) is done here, so the pump runs in its ring mode and the DirectSound play cursors
 * follow the console's audio clock. */
#include <switch.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../web/audio_proto.h"

void *nfsu2_host_mailbox(int kind, uint32_t *size);
int nfsu2_notify(int *p, unsigned count);

#define OUT_RATE   48000u
#define OUT_FRAMES 1024u                 /* 21.3 ms: 4096 bytes, one page, as audout wants */
#define NBUF       4

static volatile int g_audio_open, g_audio_stop, g_audio_stopped;
static AudioOutBuffer g_buf[NBUF];
static int16_t *g_pcm[NBUF];

/* fills one buffer from the ring; returns the number of ring frames it took */
static void fill(AudioRing *R, int16_t *out, double *pos){
    uint32_t w = atomic_load_explicit(&R->h[AR_W], memory_order_acquire);
    uint32_t r = atomic_load_explicit(&R->h[AR_R], memory_order_relaxed);
    uint32_t avail = w - r;
    const double step = (double)R->h[AR_RATE] / (double)OUT_RATE;
    int muted = atomic_load(&R->h[AR_MUTED]) != 0;
    double p = *pos;
    unsigned i = 0;
    for (; i < OUT_FRAMES; i++){
        uint32_t k = (uint32_t)p;
        if (k + 1 >= avail) break;                           /* the ring ran dry */
        double f = p - (double)k;
        const float *a = &R->pcm[((r + k) & (AUDIO_RING_FRAMES - 1)) * 2];
        const float *b = &R->pcm[((r + k + 1) & (AUDIO_RING_FRAMES - 1)) * 2];
        for (int c = 0; c < 2; c++){
            float v = (float)(a[c] + (b[c] - a[c]) * f);
            if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
            out[i * 2 + c] = muted ? 0 : (int16_t)(v * 32767.f);
        }
        p += step;
    }
    if (i < OUT_FRAMES){
        atomic_fetch_add(&R->h[AR_UNDERRUNS], 1u);
        memset(&out[i * 2], 0, (OUT_FRAMES - i) * 4);
    }
    uint32_t used = (uint32_t)p;
    *pos = p - (double)used;
    if (used){
        atomic_store_explicit(&R->h[AR_R], r + used, memory_order_release);
        atomic_fetch_add(&R->h[AR_QUANTA], 1u);
        nfsu2_notify((int *)&R->h[AR_R], 1);                 /* the pump sleeps on this word */
    }
    if (i) atomic_fetch_add(&R->h[AR_LOUD], 1u);
}

void nfsu2_pin(unsigned mask);
static void *audio_thread(void *unused){
    (void)unused;
    nfsu2_pin(0x1);                  /* the sound: core 0, away from the game thread's */
    uint32_t sz; AudioRing *R;
    while (!(R = nfsu2_host_mailbox(2 /* HOST_KIND_AUDIO */, &sz))){ struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);              /* ahead of the game thread: a late buffer is a click */
    Result rc = audoutInitialize();
    if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
    if (R_FAILED(rc)){ fprintf(stderr, "[audio] audout failed: 0x%x (no sound)\n", (unsigned)rc); return NULL; }
    fprintf(stderr, "[audio] audout open: %u Hz, %u channels, ring %u Hz -> %u Hz\n", audoutGetSampleRate(), audoutGetChannelCount(),
            (unsigned)R->h[AR_RATE], OUT_RATE);
    g_audio_open = 1;
    atomic_store(&R->h[AR_PAGE_RATE], OUT_RATE);
    atomic_store(&R->h[AR_CTX], AR_CTX_RUNNING);
    double pos = 0.0;
    for (int i = 0; i < NBUF; i++){
        g_pcm[i] = memalign(0x1000, 0x1000);
        memset(g_pcm[i], 0, 0x1000);
        g_buf[i].next = NULL; g_buf[i].buffer = g_pcm[i]; g_buf[i].buffer_size = 0x1000;
        g_buf[i].data_size = OUT_FRAMES * 4; g_buf[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&g_buf[i]);                  /* silence first: the pump needs a moment to fill the ring */
    }
    /* audout plays its buffers in the order they were queued and gives them back in that order, so the n that came back are
     * the next n of the ring of buffers (the library hands back a pointer to the first only) */
    int next = 0;
    while (!g_audio_stop){
        AudioOutBuffer *rel = NULL; u32 n = 0;
        if (R_FAILED(audoutWaitPlayFinish(&rel, &n, 200000000ull)) || !n) continue;
        for (u32 k = 0; k < n; k++){
            AudioOutBuffer *b = &g_buf[next]; next = (next + 1) % NBUF;
            fill(R, (int16_t *)b->buffer, &pos);
            b->data_size = OUT_FRAMES * 4; b->data_offset = 0;
            audoutAppendAudioOutBuffer(b);
        }
    }
    audoutStopAudioOut();
    audoutExit();
    g_audio_open = 0; g_audio_stopped = 1;
    return NULL;
}

/* the player is leaving: the thread gives audout back before the services close under it */
void audio_nx_stop(void){
    g_audio_stop = 1;
    for (int i = 0; i < 50 && !g_audio_stopped && g_audio_open; i++){ struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
}

void audio_nx_start(void){
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u * 1024u);
    pthread_t th;
    if (pthread_create(&th, &at, audio_thread, NULL) == 0) pthread_detach(th);
    else fprintf(stderr, "[audio] the audio thread could not start\n");
}

/* report.txt: how the sound went */
void nfsu2_audio_report(FILE *f){
    uint32_t sz; AudioRing *R = nfsu2_host_mailbox(2, &sz);
    if (!R){ fprintf(f, "audio: no ring (the game never started its mixer)\n"); return; }
    fprintf(f, "audio: %s; %u buffers played, %u of them with sound, %u underruns, mixed %u frames, peak %u/65536\n",
            g_audio_open ? "audout open" : "NOT open", (unsigned)R->h[AR_QUANTA], (unsigned)R->h[AR_LOUD], (unsigned)R->h[AR_UNDERRUNS],
            (unsigned)R->h[AR_MIXED], (unsigned)R->h[AR_PEAK]);
}
