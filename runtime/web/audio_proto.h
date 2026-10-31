/* The audio ring between the mixer's pump thread (runtime/web/audio_web.c)
 * and the page's AudioWorklet (web/src/audio/worklet.ts), registered with
 * nfsu2_register(HOST_KIND_AUDIO, ring, sizeof *ring).
 *
 * One producer (the pump) and one consumer (the worklet): the pump writes
 * float32 stereo frames at w and advances it, the worklet reads at r and
 * advances it.  Both counters only grow (mod 2^32); fill = w - r.  The page
 * (main thread) owns the context-state word.  The header is a plain array
 * of 32-bit words so the TypeScript side mirrors it by index:
 * web/src/audio/proto.ts carries the same numbers and web/tools/check-proto.mjs
 * compares the two files. */
#ifndef NFSU2_AUDIO_PROTO_H
#define NFSU2_AUDIO_PROTO_H
#include <stdint.h>
#include <stddef.h>

#define AUDIO_MAGIC        0x4E415544u  /* 'NAUD' */
#define AUDIO_VERSION      1u
#define AUDIO_RING_FRAMES  16384u       /* power of two: 372 ms at 44.1 kHz (a phone's 175 ms bursts, smoothly) */
#define AUDIO_RATE         44100u       /* the mixer's output rate */

/* header words */
enum {
    AR_MAGIC = 0,
    AR_VERSION = 1,
    AR_FRAMES = 2,       /* ring size in frames */
    AR_RATE = 3,         /* sample rate of the ring */
    AR_W = 4,            /* pump: frames written */
    AR_R = 5,            /* worklet: frames read (the pump waits on this word) */
    AR_UNDERRUNS = 6,    /* worklet: render quanta that found fewer frames than they needed */
    AR_CTX = 7,          /* page: AudioContext state (AR_CTX_*) */
    AR_QUANTA = 8,       /* worklet: process() calls */
    AR_TARGET = 9,       /* pump: ring fill it keeps, in frames */
    AR_MODE = 10,        /* pump: AR_MODE_* */
    AR_NOSOUND = 11,     /* pump: 1 = sound off (NOSOUND), the page plays nothing */
    AR_MUTED = 12,       /* pump: 1 = the game is parked, the output is silence */
    AR_PEAK = 13,        /* pump: peak of the last full second of output, 0..65536 */
    AR_RMS = 14,         /* pump: RMS of the last full second of output, 0..65536 */
    AR_LOUD = 15,        /* worklet: quanta whose output was not silent */
    AR_RESET = 16,       /* pump: bumped by audio_device_changed (autotest audioreset): the page rebuilds its context */
    AR_WALL_MS = 17,     /* pump: milliseconds mixed on the wall clock (no consumer) */
    AR_MIXED = 18,       /* pump: frames mixed (low 32 bits) */
    AR_PAGE_RATE = 19,   /* page: the context's real sample rate */
    AR_HDR_WORDS = 32
};
enum { AR_CTX_NONE = 0, AR_CTX_RUNNING = 1, AR_CTX_SUSPENDED = 2, AR_CTX_CLOSED = 3 };
enum { AR_MODE_IDLE = 0, AR_MODE_RING = 1, AR_MODE_WALL = 2 };

typedef struct {
    _Atomic uint32_t h[AR_HDR_WORDS];
    float pcm[AUDIO_RING_FRAMES * 2];     /* interleaved L, R */
} AudioRing;

#define AR_OFF_PCM 128   /* byte offset of pcm */
_Static_assert(offsetof(AudioRing, pcm) == AR_OFF_PCM, "audio ring layout");
_Static_assert((AUDIO_RING_FRAMES & (AUDIO_RING_FRAMES - 1)) == 0, "ring size: power of two");
#endif
