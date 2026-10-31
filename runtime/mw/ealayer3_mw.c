/* Most Wanted's music in EALayer3: our own decoder, in SND's empty decoder slot (TRACKER M7-37, M7-19; GAME 7.1,
 * 7.4).
 *
 * The game's music, SOUND/PFDATA/MW_Music.mus, is 3,257 streams of EA-XA ADPCM.  tools/musicpack.py --game mw
 * re-encodes them as EALayer3, EA's MP3 variant, as it did Underground 2's: the same streams, headers and blocks, the
 * codec 0x17 instead of the ADPCM.  Underground 2's SND plays 0x17 itself; Most Wanted's (SND V9.06) maps the codec to
 * decoder slot 6 (0x822C84) but never fills that slot (its registrars 0x825ABA, 0x825A7D, 0x825A40 and 0x8259DB fill
 * slots 0 to 4), and the exe has no MPEG code.  So the runtime registers its own decoder there, after the game's
 * registrars ran: the MPEG audio layer III decoding is minimp3's (runtime/mw/minimp3.h, public domain), the rest is
 * here.
 *
 * SND's decoders (GAME 7.1): the table 0x9C2328 holds three functions a slot, one for each way a voice gets its data
 * (variant 2 is a stream's blocks, the others samples in memory), and 0x9C2388 the size of a voice's state for each.
 * When a voice starts, 0x822C41 allocates that state for each of its channels, sets its +0x1A to the channel, and calls
 * the variant's function (cdecl (state, params)): that sets the state's +0 to the voice's decode function and +4 to its
 * release, and from then on the mixer calls +0 (cdecl (state, n, -, float *out, -): n samples of the channel, as floats
 * on the 16-bit scale; it returns how many it gave) and, at the end, +4 (cdecl (state)).  A stream's decode function
 * gets its data a block (SCDl) at a time, with SND's 0x81E092 (stream, channel, &samples, &more): the channel's data
 * in the block, its sample count, and 0 in more for the first block after a stream header (SCHl; 0x81C675 takes the
 * flag from the stream's +0x24, which a header clears); and hands back what it used with 0x81E1E8 (stream, channel,
 * samples) at its next call.  The block's channel data starts where the block's offset table says (0x81C675).  This
 * is how the game's EA-XA stream decoder (0x826E00, 0x826CCC) works, and Underground 2's EALayer3 one (0x72820C,
 * 0x72802D) too.  The music's PathFinder plays a chain of segments (an EA Trax song is one) in one voice, one header
 * after the other, so a decoder starts afresh at each first block, as Underground 2's does (kept on, it played a few
 * milliseconds of silence at each segment's start: the encoder's own start, which the skip of 1105 is for).
 *
 * The streams (tools/musicpack.py; vgmstream's mpeg_custom_utils_ealayer3.c reads the same format): each MP3 granule
 * is a frame of its own, 0x00, 8 bits MPEG version / sample-rate index / channel mode / mode extension, 1 bit granule,
 * for granule 1 4 bits scfsi per channel, per channel the granule's 59 bits of side information, then its main data,
 * to a byte.  A PCM frame (0xEE, u16 offset, u16 count, count samples of every channel, s16, big-endian) replaces the
 * decoded samples from (end of the granule before it - offset) on.  The output starts 1105 samples into the decoding
 * (MP3's 529 and the encoder's 576), so output sample n is the encoder's sample n; a block holds whole MP3 frames
 * (granules 2k and 2k+1) and its sample count is what they add.  A stereo stream is one joint-stereo MP3 stream, both
 * channels' offsets naming it; a stream of four channels is two, channels 0+1 and 2+3.  The channels of a pair share
 * one decoder: the first of them to reach a block decodes it for both, the other takes its half from a queue.
 * NFSMW_EALAYER3=0 leaves the slot empty (the game then skips such a stream); NFSMW_EALAYER3_LOG=1 logs each voice. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD                 /* the same samples from every build: wasm has no SSE or NEON either */
#include "minimp3.h"

#define EL3_DELAY   1105                /* decoded samples before output sample 0 */
#define EL3_SCALE   32768.0f            /* minimp3's floats are -1..1, SND's the 16-bit scale */

/* ------------------------------------------------------------------------------------------------- the decoder ---
 * A pair: one MP3 stream (one or two channels) of an EALayer3 stream, decoded a block at a time into a queue each of
 * its channels reads at its own pace. */
typedef struct El3Pair {
    mp3dec_t dec;
    int nch;                            /* channels of the MP3 stream: 1 or 2 */
    int started;                        /* the first block was decoded (the decoder is not reset again) */
    long long pos;                      /* output index of the next decoded sample (starts at -EL3_DELAY) */
    unsigned blocks;                    /* blocks decoded */
    float *q;                           /* decoded samples, interleaved, not yet read by both channels */
    unsigned qcap, qlen;                /* in sample frames */
    unsigned rd[2];                     /* each channel's read position in q */
    int refs;
    unsigned readers;                   /* bit 0, bit 1: the channels that read the queue */
    uint32_t stream;                    /* SND's stream it decodes (the second channel finds it by that) */
} El3Pair;

typedef struct El3Bits { const uint8_t *p; uint32_t pos, end; } El3Bits;
static uint64_t el3_get(El3Bits *b, int n){
    uint64_t v = 0;
    for (int i = 0; i < n; i++, b->pos++)
        v = (v << 1) | (b->pos < b->end ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u : 0u);
    return v;
}
typedef struct El3Out { uint8_t *p; uint32_t pos; } El3Out;
static void el3_put(El3Out *o, uint64_t v, int n){
    for (int i = n - 1; i >= 0; i--, o->pos++)
        if ((v >> i) & 1u) o->p[o->pos >> 3] |= (uint8_t)(0x80u >> (o->pos & 7));
}

/* one granule frame at p (at most lim bytes): its header, scfsi, side information and main data; its size, or 0 */
typedef struct El3Gr {
    unsigned hdr, gr, scfsi[2], nch;
    uint64_t side[2];
    const uint8_t *data; uint32_t dbit, dlen[2];   /* main data: at bit dbit of data, dlen[ch] bits each */
} El3Gr;
static uint32_t el3_granule(const uint8_t *p, uint32_t lim, El3Gr *g){
    if (lim < 2 || p[0] != 0x00) return 0;
    El3Bits b = { p, 8, lim * 8u };
    g->hdr = (unsigned)el3_get(&b, 8);
    g->gr = (unsigned)el3_get(&b, 1);
    g->nch = ((g->hdr >> 2) & 3u) == 3u ? 1u : 2u;
    if ((g->hdr >> 6) != 3u || ((g->hdr >> 4) & 3u) == 3u) return 0;      /* MPEG-1, a valid sample rate */
    g->scfsi[0] = g->scfsi[1] = 0;
    if (g->gr) for (unsigned c = 0; c < g->nch; c++) g->scfsi[c] = (unsigned)el3_get(&b, 4);
    uint32_t bits = 0;
    for (unsigned c = 0; c < g->nch; c++){ g->side[c] = el3_get(&b, 59); g->dlen[c] = (uint32_t)(g->side[c] >> 47); bits += g->dlen[c]; }
    g->data = p; g->dbit = b.pos;
    uint32_t end = (b.pos + bits + 7u) / 8u;
    return end <= lim ? end : 0;
}

/* granules 0 and 1 of an MP3 frame as the MPEG frame they came from (main_data_begin 0, the largest bitrate: LAME
 * wrote them without the bit reservoir); its size, or 0 when it does not fit */
static int el3_frame(const El3Gr *g0, const El3Gr *g1, uint8_t *f, int cap){
    static const int rates[3] = { 44100, 48000, 32000 };
    int sr = (int)((g0->hdr >> 4) & 3u), size = 144 * 320000 / rates[sr];
    if (size > cap) return 0;
    memset(f, 0, (size_t)size);
    f[0] = 0xFF; f[1] = 0xFB;                                  /* MPEG-1, layer III, no CRC */
    f[2] = (uint8_t)((14u << 4) | ((unsigned)sr << 2));        /* 320 kbit/s, no padding */
    f[3] = (uint8_t)((g0->hdr & 0x0Fu) << 4);                  /* mode, mode extension */
    El3Out o = { f, 32 };
    el3_put(&o, 0, 9);                                         /* main_data_begin */
    el3_put(&o, 0, g0->nch == 1 ? 5 : 3);                      /* private bits */
    for (unsigned c = 0; c < g0->nch; c++) el3_put(&o, g1->scfsi[c], 4);
    for (unsigned c = 0; c < g0->nch; c++) el3_put(&o, g0->side[c], 59);
    for (unsigned c = 0; c < g0->nch; c++) el3_put(&o, g1->side[c], 59);
    const El3Gr *gs[2] = { g0, g1 };
    for (int k = 0; k < 2; k++){
        El3Bits b = { gs[k]->data, gs[k]->dbit, 0xFFFFFFFFu };
        for (unsigned c = 0; c < g0->nch; c++){
            uint32_t n = gs[k]->dlen[c];
            if ((o.pos + n + 7u) / 8u > (uint32_t)size) return 0;
            while (n){ int t = n > 56u ? 56 : (int)n; el3_put(&o, el3_get(&b, t), t); n -= (uint32_t)t; }
        }
    }
    return size;
}

static int el3_qroom(El3Pair *s, unsigned more){
    unsigned lo = s->readers == 2u ? s->rd[1] : s->readers == 3u && s->rd[1] < s->rd[0] ? s->rd[1] : s->rd[0];
    if (lo){                                                   /* drop what both channels have read */
        memmove(s->q, s->q + (size_t)lo * 2u, (size_t)(s->qlen - lo) * 2u * sizeof(float));
        s->qlen -= lo;
        for (int k = 0; k < 2; k++) s->rd[k] = s->rd[k] >= lo ? s->rd[k] - lo : 0;
    }
    if (s->qlen + more <= s->qcap) return 1;
    unsigned cap = s->qcap ? s->qcap : 4096u;
    while (cap < s->qlen + more) cap *= 2u;
    if (cap > (1u << 22)) return 0;                            /* a channel that stopped reading: give up */
    float *q = realloc(s->q, (size_t)cap * 2u * sizeof(float));
    if (!q) return 0;
    s->q = q; s->qcap = cap;
    return 1;
}

/* One block's data at p (at most lim bytes) for count output samples, appended to the queue.  first: the stream's
 * first block (the decoder starts afresh).  Returns 0, or why it failed. */
static const char *el3_block(El3Pair *s, const uint8_t *p, uint32_t lim, uint32_t count, int first){
    if (first || !s->started){ memset(&s->dec, 0, sizeof s->dec); s->pos = -EL3_DELAY; s->started = 1; }
    if (!el3_qroom(s, count + 1152u)) return "no room for the decoded samples";
    uint32_t at = 0, got = 0;
    float pcm[1152 * 2];
    uint8_t frame[1500];
    while (got < count){
        El3Gr g[2];
        struct { uint32_t at; unsigned after; } pcmf[8];
        int npcm = 0;
        for (unsigned k = 0; k < 2; k++){
            uint32_t n = el3_granule(p + at, lim - at, &g[k]);
            if (!n) return "not a granule frame where one should be";
            if (g[k].gr != k) return "granules out of order";
            at += n;
            while (at < lim && p[at] == 0xEE){                     /* PCM frames after this granule */
                if (at + 5u > lim || npcm == 8) return "a PCM frame past the block";
                uint32_t cnt = ((uint32_t)p[at + 3] << 8) | p[at + 4];
                pcmf[npcm].at = at; pcmf[npcm].after = k; npcm++;
                at += 5u + cnt * 2u * g[k].nch;
                if (at > lim) return "a PCM frame past the block";
            }
        }
        if (g[0].nch != g[1].nch || (g[0].hdr ^ g[1].hdr) & 0xF0u) return "the granules of a frame differ";
        int size = el3_frame(&g[0], &g[1], frame, (int)sizeof frame);
        if (!size) return "a frame too large to rebuild";
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&s->dec, frame, size, pcm, &info);
        unsigned nch = g[0].nch;
        if (n != 1152 || (unsigned)info.channels != nch){ memset(pcm, 0, sizeof pcm); }
        for (int i = 0; i < npcm; i++){
            const uint8_t *f = p + pcmf[i].at;
            int off = (f[1] << 8) | f[2], cnt = (f[3] << 8) | f[4];
            int start = 576 * ((int)pcmf[i].after + 1) - off;
            if (start < 0 || start + cnt > 1152) return "a PCM frame outside its granule";
            for (int j = 0; j < cnt * (int)nch; j++)
                pcm[(size_t)start * nch + (size_t)j] = (float)(int16_t)((f[5 + 2 * j] << 8) | f[6 + 2 * j]) / EL3_SCALE;
        }
        for (int i = 0; i < 1152 && got < count; i++){
            if (s->pos + i < 0) continue;
            float *d = s->q + (size_t)s->qlen * 2u;
            d[0] = pcm[(size_t)i * nch] * EL3_SCALE;
            d[1] = nch == 2 ? pcm[(size_t)i * 2u + 1u] * EL3_SCALE : d[0];
            s->qlen++; got++;
        }
        s->pos += 1152;                 /* a block's last frame may hold more than the stream (its end): dropped */
    }
    s->blocks++;
    return 0;
}

#ifndef EL3_CORE_ONLY             /* tools/mw/el3_test.c takes the decoder alone */
#include "cpu.h"
/* ------------------------------------------------------------------------------------------------ in the game ---
 * The voice's state (SND allocates EL3_STATE bytes; +0x08..+0x1B are SND's own, +0x1A the channel): */
#define ST_DECODE   0x00u
#define ST_RELEASE  0x04u
#define ST_CHAN     0x1Au               /* byte: the channel (SND's) */
#define ST_PAIR     0x1Cu               /* our pair: its index + 1 in g_pairs */
#define ST_COUNT    0x20u               /* the current block's samples (0x81E092 writes it) */
#define ST_USED     0x24u               /* ... of which this channel gave */
#define ST_DONE     0x28u               /* samples given since the last hand-back (0x81E1E8) */
#define ST_STREAM   0x2Cu               /* SND's stream */
#define ST_SCHAN    0x30u               /* the channel as the stream knows it */
#define ST_MORE     0x34u               /* 0x81E092's "not the first block" */
#define ST_BLOCKS   0x38u               /* blocks this channel took */
#define ST_MAGIC    0x3Cu
#define EL3_STATE   0x40u
#define EL3_MAGIC   0x334C4145u         /* "EAL3" */

#define SND_TABLE   0x009c2328u         /* 3 functions a slot */
#define SND_SIZES   0x009c2388u         /* 3 state sizes a slot */
#define SND_SLOT    6u                  /* codec 0x17 (0x822C84) */
#define FN_VOICE_ID 0x00822476u         /* (voice): the voice's sample */
#define FN_CHANNEL  0x00822494u         /* (voice): its channel in the stream */
#define FN_STREAM   0x00826838u         /* (sample): its stream */
#define FN_BLOCK    0x0081e092u         /* (stream, channel, &samples, &more): the channel's data in the next block, 0 none */
#define FN_GIVEBACK 0x0081e1e8u         /* (stream, channel, samples) */
/* Our functions: addresses inside the int3 padding after 0x829410, which no code calls or jumps to, patched in
 * dispatch.c's table (dispatch_patch) */
#define FN_CREATE0  0x00829443u
#define FN_CREATE1  0x00829445u
#define FN_CREATE2  0x00829447u
#define FN_DECODE   0x00829449u
#define FN_RELEASE  0x0082944bu

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
void dispatch_patch(uint32_t a, void (*f)(CPU *));
long autotest_frame(void);

#define PAIRS 64
static El3Pair *g_pairs[PAIRS];
static El3Pair *g_last[4];              /* the pair each channel pair's first channel made last (for the second) */
static int g_log = -1;
static unsigned long g_voices, g_errors, g_calls;

static int el3_log(void){
    if (g_log < 0){ const char *e = getenv("NFSMW_EALAYER3_LOG"); g_log = e && *e && *e != '0'; }
    return g_log;
}
static void el3_error(uint32_t st, const char *why){
    if (++g_errors <= 20)
        fprintf(stderr, "[el3] f=%ld voice state %08x channel %u: %s (the rest of the stream is silent)\n",
                autotest_frame(), st, MR8(st + ST_CHAN), why);
}
static El3Pair *pair_of(uint32_t st){
    uint32_t i = MR32(st + ST_PAIR);
    return MR32(st + ST_MAGIC) == EL3_MAGIC && i >= 1u && i <= PAIRS ? g_pairs[i - 1u] : 0;
}
static uint32_t pair_new(uint32_t stream, int nch){
    for (uint32_t i = 0; i < PAIRS; i++){
        if (g_pairs[i]) continue;
        El3Pair *s = calloc(1, sizeof *s);
        if (!s) return 0;
        s->nch = nch; s->stream = stream; s->refs = 1;
        g_pairs[i] = s;
        return i + 1u;
    }
    return 0;
}
static void pair_drop(uint32_t i){
    if (i < 1u || i > PAIRS || !g_pairs[i - 1u]) return;
    El3Pair *s = g_pairs[i - 1u];
    if (--s->refs > 0) return;
    for (int k = 0; k < 4; k++) if (g_last[k] == s) g_last[k] = 0;
    free(s->q); free(s);
    g_pairs[i - 1u] = 0;
}

/* cdecl (state, n, -, float *out, -): n samples of the voice's channel */
static void el3_decode(CPU *c){
    uint32_t st = MR32(c->esp + 4u), out = MR32(c->esp + 16u);
    int32_t n = (int32_t)MR32(c->esp + 8u), given = 0;
    El3Pair *s = pair_of(st);
    uint32_t stream = MR32(st + ST_STREAM), sch = MR8(st + ST_SCHAN), ch = MR8(st + ST_CHAN);
    if (MR32(st + ST_DONE)){
        uint32_t a[3] = { stream, sch, MR32(st + ST_DONE) };
        call_guest(c, FN_GIVEBACK, 3, a);
        MR32(st + ST_DONE) = 0;
    }
    while (s && given < n){
        if (MR32(st + ST_USED) >= MR32(st + ST_COUNT)){
            uint32_t a[4] = { stream, sch, st + ST_COUNT, st + ST_MORE };
            uint32_t p = call_guest(c, FN_BLOCK, 4, a);
            if (!p || p == 0xFFFFFFFFu){                           /* nothing yet, or the stream's end */
                if (MR32(st + ST_DONE) && given < n) memset(&MRF32(out + 4u * (uint32_t)given), 0, 4u * (uint32_t)(n - given));
                break;
            }
            uint32_t j = MR32(st + ST_BLOCKS);
            MR32(st + ST_BLOCKS) = j + 1u;
            MR32(st + ST_USED) = 0;
            if (!MR32(st + ST_MORE) && el3_log() && ++g_calls <= 2000u)
                fprintf(stderr, "[el3] f=%ld stream %u channel %u: a header's first block (block %u, %u samples)\n",
                        autotest_frame(), stream, ch, j, MR32(st + ST_COUNT));
            if (s->blocks == j){                                   /* this channel is the first at this block */
                uint32_t lim = 0x10000u;
                while (lim && !mem_is_committed(p + lim - 1u)) lim >>= 1;
                const char *why = el3_block(s, &MR8(p), lim, MR32(st + ST_COUNT), !MR32(st + ST_MORE));
                if (why){ el3_error(st, why); MR32(st + ST_MAGIC) = 0; s = 0; break; }
            }
        }
        uint32_t k = MR32(st + ST_COUNT) - MR32(st + ST_USED);
        if (k > (uint32_t)(n - given)) k = (uint32_t)(n - given);
        unsigned side = ch & 1u, have = s->qlen - s->rd[side];
        for (uint32_t i = 0; i < k; i++)
            MRF32(out + 4u * ((uint32_t)given + i)) = i < have ? s->q[(size_t)(s->rd[side] + i) * 2u + side] : 0.0f;
        if (k > have){ el3_error(st, "the decoded samples ran out"); s->rd[side] = s->qlen; }
        else s->rd[side] += k;
        MR32(st + ST_USED) += k; MR32(st + ST_DONE) += k; given += (int32_t)k;
    }
    POP32(c->eip);
    c->eax = (uint32_t)given;
}

/* cdecl (state) */
static void el3_release(CPU *c){
    uint32_t st = MR32(c->esp + 4u);
    if (MR32(st + ST_MAGIC) == EL3_MAGIC) pair_drop(MR32(st + ST_PAIR));
    MR32(st + ST_MAGIC) = 0; MR32(st + ST_PAIR) = 0;
    POP32(c->eip);
}

/* cdecl (state, params): params +0x08 the stream's channels, +0x1C the voice */
static void el3_create(CPU *c, int variant){
    uint32_t st = MR32(c->esp + 4u), pr = MR32(c->esp + 8u);
    uint32_t ch = MR8(st + ST_CHAN), nch = MR32(pr + 8u), voice = MR32(pr + 0x1Cu);
    for (uint32_t o = 0x1Cu; o < EL3_STATE; o += 4u) MR32(st + o) = 0;
    MR32(st + ST_DECODE) = FN_DECODE;
    MR32(st + ST_RELEASE) = FN_RELEASE;
    g_voices++;
    if (variant != 2){
        if (++g_errors <= 20)
            fprintf(stderr, "[el3] f=%ld an EALayer3 sample in memory (variant %d), not a stream: silent\n", autotest_frame(), variant);
        POP32(c->eip); c->eax = 0; return;
    }
    uint32_t id = call_guest(c, FN_VOICE_ID, 1, &voice);
    MR32(st + ST_STREAM) = call_guest(c, FN_STREAM, 1, &id);
    MR8(st + ST_SCHAN) = (uint8_t)call_guest(c, FN_CHANNEL, 1, &voice);
    unsigned pi = ch / 2u < 4u ? ch / 2u : 3u;
    int share = 0;
    if (ch & 1u && g_last[pi] && g_last[pi]->stream == MR32(st + ST_STREAM)){
        for (uint32_t i = 0; i < PAIRS; i++)
            if (g_pairs[i] == g_last[pi]){
                g_pairs[i]->refs++; g_pairs[i]->readers |= 2u; MR32(st + ST_PAIR) = i + 1u; share = 1; break;
            }
    }
    if (!share){
        uint32_t i = pair_new(MR32(st + ST_STREAM), nch - (ch & ~1u) >= 2u ? 2 : 1);
        MR32(st + ST_PAIR) = i;
        if (i) g_pairs[i - 1u]->readers = 1u << (ch & 1u);
        if (i && !(ch & 1u)) g_last[pi] = g_pairs[i - 1u];
        if (!i) el3_error(st, "no room for another decoder");
    }
    MR32(st + ST_MAGIC) = MR32(st + ST_PAIR) ? EL3_MAGIC : 0u;
    if (el3_log())
        fprintf(stderr, "[el3] f=%ld voice %u channel %u of %u: stream %u, %s (%lu voices so far)\n", autotest_frame(),
                voice, ch, nch, MR32(st + ST_STREAM), share ? "shares the first channel's decoder" : "its own decoder", g_voices);
    POP32(c->eip);
    c->eax = 0;
}
static void el3_create0(CPU *c){ el3_create(c, 0); }
static void el3_create1(CPU *c){ el3_create(c, 1); }
static void el3_create2(CPU *c){ el3_create(c, 2); }

/* SND's last decoder registrar (slots 3 and 4, MicroTalk), from its init 0x823071: then slot 6 */
void sub_008259db_orig(CPU *c);
void sub_008259db(CPU *c){
    sub_008259db_orig(c);
    const char *e = getenv("NFSMW_EALAYER3");
    if (e && *e == '0') return;
    static int patched;
    if (!patched){
        patched = 1;
        dispatch_patch(FN_CREATE0, el3_create0); dispatch_patch(FN_CREATE1, el3_create1);
        dispatch_patch(FN_CREATE2, el3_create2); dispatch_patch(FN_DECODE, el3_decode);
        dispatch_patch(FN_RELEASE, el3_release);
        fprintf(stderr, "[el3] EALayer3 decoder in SND's slot %u (codec 0x17)\n", SND_SLOT);
    }
    const uint32_t fn[3] = { FN_CREATE0, FN_CREATE1, FN_CREATE2 };
    for (uint32_t v = 0; v < 3u; v++){
        MR32(SND_TABLE + 12u * SND_SLOT + 4u * v) = fn[v];
        MR32(SND_SIZES + 12u * SND_SLOT + 4u * v) = EL3_STATE;
    }
}
#endif
