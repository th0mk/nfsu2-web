// The audio ring's layout: the page side of runtime/web/audio_proto.h (same
// names, same numbers; web/tools/check-proto.mjs compares the two files).
// The header is an array of 32-bit words; the PCM follows it.

export const AUDIO_MAGIC = 0x4E415544;
export const AUDIO_VERSION = 1;
export const AUDIO_RING_FRAMES = 16384;
export const AUDIO_RATE = 44100;

export const AR_MAGIC = 0;
export const AR_VERSION = 1;
export const AR_FRAMES = 2;
export const AR_RATE = 3;
export const AR_W = 4;            // pump: frames written
export const AR_R = 5;            // worklet: frames read (notified for the pump)
export const AR_UNDERRUNS = 6;    // worklet
export const AR_CTX = 7;          // page: AudioContext state
export const AR_QUANTA = 8;       // worklet: process() calls
export const AR_TARGET = 9;       // pump: ring fill target, frames
export const AR_MODE = 10;        // pump: 0 idle, 1 ring paced, 2 wall clock
export const AR_NOSOUND = 11;     // pump: sound off
export const AR_MUTED = 12;       // pump: the game is parked
export const AR_PEAK = 13;        // pump: last second's peak, 0..65536
export const AR_RMS = 14;         // pump: last second's RMS, 0..65536
export const AR_LOUD = 15;        // worklet: non-silent quanta
export const AR_RESET = 16;       // pump: rebuild the context when this changes
export const AR_WALL_MS = 17;     // pump: ms mixed without a consumer
export const AR_MIXED = 18;       // pump: frames mixed (low 32 bits)
export const AR_PAGE_RATE = 19;   // page: the context's sample rate
export const AR_HDR_WORDS = 32;

export const AR_CTX_NONE = 0, AR_CTX_RUNNING = 1, AR_CTX_SUSPENDED = 2, AR_CTX_CLOSED = 3;
export const AR_MODE_IDLE = 0, AR_MODE_RING = 1, AR_MODE_WALL = 2;
export const AR_OFF_PCM = 128;
