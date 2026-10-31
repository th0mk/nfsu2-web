/* Unit tests of the runtime's EALayer3 decoder (runtime/mw/ealayer3_mw.c) off the game: its core on made-up blocks,
 * and its side in SND (a voice's create, decode and release) against a model of SND's stream in a guest address space
 * of its own.  The model does what the game does (GAME 7.1): the block parser 0x81C675 takes a block's channel
 * pointers from its offset table, keeps the block's address in the word before channel 0's data and the 31-bit count
 * with the "more" bit 31 in a ring entry of the stream (0x81DEEE: +4 the count, +8 + 4 * channel the pointers); 0x81E092
 * hands a channel its next entry: the count sign-extended from bit 30, the pointer, 0 when none is there.
 * web/test/mw-el3.test.mjs builds it with the sanitizers and runs each test:
 *   clang -std=c11 -g -fsanitize=address,undefined -Iruntime -o out/el3_unit tools/mw/el3_unit.c -lm
 *   out/el3_unit <test>          ->  "PASS <test>" on stdout, or "FAIL <test>: why" and exit code 1
 * The blocks hold granule frames of silence: side information all 0, so no main data (each frame decodes to 1152
 * samples whatever minimp3 makes of it). */
#define _GNU_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../runtime/mw/ealayer3_mw.c"

uint8_t *g_mem;
uint8_t g_cbits[1u << 15];
void dispatch_patch(uint32_t a, void (*f)(CPU *)){ (void)a; (void)f; }
void sub_008259db_orig(CPU *c){ (void)c; }
long autotest_frame(void){ return 0; }

static const char *g_test;
static void fail(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    printf("FAIL %s: ", g_test); vprintf(fmt, ap); printf("\n");
    va_end(ap); fflush(stdout); exit(1);
}
static void on_alarm(int s){ (void)s; static const char m[] = "FAIL: the decoder did not return (a hang)\n"; write(1, m, sizeof m - 1); _exit(1); }

/* ---------------------------------------------------------------------------------------------------- blocks --- */
/* one MP3 frame: granules 0 and 1 of a stereo MPEG-1 stream at 44.1 kHz (header 0xC0), their side information 0 */
#define FRAME 35
static size_t put_frame(uint8_t *d){
    memset(d, 0, FRAME);
    d[1] = 0xC0;                        /* granule 0: 0x00, header, granule bit 0, 2 x 59 bits: 135 bits, 17 bytes */
    d[17 + 1] = 0xC0; d[17 + 2] = 0x80; /* granule 1: ..., granule bit 1, 2 x 4 bits scfsi, 2 x 59 bits: 18 bytes */
    return FRAME;
}
static uint32_t be32(const uint8_t *p){ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wle32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
/* samples a stream's first block of n frames gives (the decoder's start skips EL3_DELAY) */
static uint32_t first_count(unsigned n){ return 1152u * n - EL3_DELAY; }

/* ---------------------------------------------------------------------------------------------- SND's stream --- */
#define SND_STREAMS 0x009c1f44u         /* the stream objects by SND's stream index */
#define SO_CHANS    0x62u               /* byte: the stream's channels */
#define SO_READ     0x28u               /* word a channel: the ring entry it takes next */
#define SO_RING     0x7cu               /* the ring: 32 bytes an entry */
#define GUEST      (32u << 20)         /* the guest memory this test maps (SND_STREAMS is in it) */
#define STACK       0x00010000u
#define STATES      0x00020000u         /* voice states, EL3_STATE bytes each */
#define PARAMS      0x0002f000u
#define OUT         0x00030000u
#define OBJS        0x00040000u         /* SND's stream objects, 0x1000 bytes each */
#define BLOCKS      0x00100000u         /* the blocks, one after the other */
#define SO_SEQ      0x04u
#define SO_AVAIL    0x34u               /* word a channel: entries it has not taken */
#define SO_RSIZE    0x40u               /* word: the ring's entries */
#define SO_WRITE    0x46u               /* word: the next entry the parser fills */
#define SO_FIRST    0x70u               /* (this model's own) the next block is the first after a header */
static uint32_t g_blocks = BLOCKS, g_stream;

static uint32_t so_of(uint32_t stream){ return MR32(SND_STREAMS + 4u * stream); }
static void snd_stream(uint32_t stream, unsigned nch){
    uint32_t so = OBJS + 0x1000u * stream;
    memset(&MR8(so), 0, 0x1000);
    MR32(SND_STREAMS + 4u * stream) = so;
    MR8(so + SO_CHANS) = (uint8_t)nch;
    MR16(so + SO_RSIZE) = 16;
    MR8(so + SO_FIRST) = 1;
}
/* a block as the stream reader leaves it in memory, then 0x81C675's work on it; returns its address */
static uint32_t snd_block_put(uint32_t stream, const uint8_t *blk, uint32_t len){
    uint32_t so = so_of(stream), nch = MR8(so + SO_CHANS), b = g_blocks;
    memcpy(&MR8(b), blk, len);
    g_blocks = (b + len + 15u) & ~15u;
    uint32_t count = be32(blk + 8) & 0x7FFFFFFFu, p[6] = {0};
    for (uint32_t k = 0; k < nch; k++) p[k] = b + 12u + 4u * nch + be32(blk + 12 + 4 * k);
    if (!count) return b;
    MR32(p[0] - 4u) = b;                /* the block's address before channel 0's data (for its release) */
    MR32(b) = 0;                        /* the tag: a link of SND's */
    uint32_t w = MR16(so + SO_WRITE), e = so + SO_RING + 32u * w;
    MR32(e) = MR32(so + SO_SEQ);
    MR32(e + 4u) = (MR8(so + SO_FIRST) ? 0u : 0x80000000u) | count;
    for (uint32_t k = 0; k < nch; k++){ MR32(e + 8u + 4u * k) = p[k]; MR16(so + SO_AVAIL + 2u * k)++; }
    MR16(so + SO_WRITE) = (uint16_t)((w + 1u) % MR16(so + SO_RSIZE));
    MR8(so + SO_FIRST) = 0;
    return b;
}
/* 0x81E092 (stream, channel, &samples, &more) */
static uint32_t snd_block_get(uint32_t stream, uint32_t ch, uint32_t pcount, uint32_t pmore){
    uint32_t so = so_of(stream);
    if (!MR16(so + SO_AVAIL + 2u * ch)) return 0;
    uint32_t i = (uint32_t)(int32_t)(int16_t)MR16(so + SO_READ + 2u * ch), e = so + SO_RING + 32u * i, v = MR32(e + 4u);
    MR32(pcount) = (uint32_t)((int32_t)(v << 1) >> 1);
    MR32(pmore) = (uint32_t)((int32_t)v >> 31);
    MR16(so + SO_READ + 2u * ch) = (uint16_t)(i + 1u >= MR16(so + SO_RSIZE) ? 0u : i + 1u);
    MR16(so + SO_AVAIL + 2u * ch)--;
    uint32_t p = MR32(e + 8u + 4u * ch);
    return p ? p : 0xFFFFFFFFu;
}
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *a){
    (void)c; (void)nargs;
    switch (fn){
    case FN_VOICE_ID: return a[0];
    case FN_STREAM:   return g_stream;
    case FN_CHANNEL:  return a[0] & 0xFFu;            /* the test's voices: 0x100 * n + channel */
    case FN_BLOCK:    return snd_block_get(a[0], a[1], a[2], a[3]);
    case FN_GIVEBACK: return 0;
    }
    fail("call_guest(%08x)", fn);
    return 0;
}

/* a block of the stream: count samples, each pair of channels' data (a pair's frames after the other's) */
static uint32_t block(uint32_t stream, uint32_t count, unsigned nch, const unsigned *frames, uint32_t cut){
    uint8_t blk[4096];
    unsigned npair = (nch + 1u) / 2u;
    uint32_t at = 12u + 4u * nch, data = at;
    memcpy(blk, "SCDl", 4); wbe32(blk + 8, count);
    for (unsigned k = 0; k < npair; k++){
        for (unsigned c = 2u * k; c < 2u * k + 2u && c < nch; c++) wbe32(blk + 12 + 4 * c, at - data);
        for (unsigned f = 0; f < frames[k]; f++) at += (uint32_t)put_frame(blk + at);
    }
    wle32(blk + 4, cut ? cut : at);     /* the chunk's size: cut short, a block that ends inside its data */
    return snd_block_put(stream, blk, at);
}

/* the voice's functions, called as SND's mixer does (cdecl) */
static CPU g_cpu;
static void call(void (*f)(CPU *), int n, const uint32_t *a){
    g_cpu.esp = STACK - 4u * (uint32_t)(n + 1);
    MR32(g_cpu.esp) = 0xDEADBEEFu;
    for (int i = 0; i < n; i++) MR32(g_cpu.esp + 4u + 4u * (uint32_t)i) = a[i];
    f(&g_cpu);
    if (g_cpu.eip != 0xDEADBEEFu) fail("a function did not return to its caller");
}
static uint32_t voice_state(unsigned i){ return STATES + EL3_STATE * i; }
static void create(uint32_t st, uint32_t stream, unsigned ch, unsigned nch, unsigned voice){
    memset(&MR8(st), 0, EL3_STATE);
    MR8(st + ST_CHAN) = (uint8_t)ch;
    MR32(PARAMS + 8u) = nch; MR32(PARAMS + 0x1Cu) = 0x100u * voice + ch;
    g_stream = stream;
    uint32_t a[2] = { st, PARAMS };
    call(el3_create2, 2, a);
}
static int32_t decode(uint32_t st, int32_t n){
    uint32_t a[5] = { st, (uint32_t)n, 0, OUT, 0 };
    call(el3_decode, 5, a);
    return (int32_t)g_cpu.eax;
}
static void release(uint32_t st){ uint32_t a[1] = { st }; call(el3_release, 1, a); }
static unsigned pairs_taken(void){ unsigned n = 0; for (int i = 0; i < PAIRS; i++) n += g_pairs[i] != 0; return n; }
static int has_decoder(uint32_t st){ return MR32(st + ST_MAGIC) == EL3_MAGIC; }

/* ------------------------------------------------------------------------------------------------------ tests --- */
/* the block's sample count: one with bit 31 set looped forever in el3_qroom (cap 2^31 * 2 = 0), one near 2^32
 * wrapped count + 1152 to a few samples' room and then wrote all it decoded past the queue */
static void t_count(void){
    uint8_t d[8 * FRAME];
    for (int f = 0; f < 8; f++) put_frame(d + f * FRAME);
    const uint32_t bad[] = { 0x80000000u, 0xC0000000u, 0xFFFFFF00u, 0xFFFFFFFFu, (1u << 22) + 1u };
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++){
        El3Pair s; memset(&s, 0, sizeof s); s.nch = 2; s.readers = 1;
        alarm(5);
        const char *why = el3_block(&s, d, sizeof d, bad[i], 1);
        alarm(0);
        if (!why) fail("count 0x%08x decoded (%u samples queued)", bad[i], s.qlen);
        free(s.q);
    }
    El3Pair s; memset(&s, 0, sizeof s); s.nch = 2; s.readers = 1;
    const char *why = el3_block(&s, d, sizeof d, first_count(8), 1);
    if (why || s.qlen != first_count(8)) fail("8 frames: %s, %u samples", why ? why : "ok", s.qlen);
    free(s.q);
}

/* a voice whose stream goes through the model: blocks of 2-3 frames, both channels reading in steps of 300, all
 * their samples, no error, and no decoder left after its release */
static void t_stream(void){
    snd_stream(0, 2);
    const unsigned f2[1] = { 2 }, f3[1] = { 3 };
    uint32_t total = first_count(2);
    block(0, first_count(2), 2, f2, 0);
    for (int i = 0; i < 4; i++){ block(0, 1152u * 3u, 2, f3, 0); total += 1152u * 3u; }
    uint32_t s0 = voice_state(0), s1 = voice_state(1);
    create(s0, 0, 0, 2, 1); create(s1, 0, 1, 2, 1);
    if (!has_decoder(s0) || !has_decoder(s1) || pairs_taken() != 1) fail("the voice's channels have no shared decoder");
    uint32_t got[2] = {0, 0};
    for (int r = 0; r < 100; r++) for (int c = 0; c < 2; c++) got[c] += (uint32_t)decode(c ? s1 : s0, 300);
    if (got[0] != total || got[1] != total || g_errors) fail("%u and %u samples of %u, %lu errors", got[0], got[1], total, g_errors);
    release(s0); release(s1);
    if (pairs_taken()) fail("%u decoders left after the release", pairs_taken());
}

int main(int argc, char **argv){
    if (argc != 2){ fprintf(stderr, "usage: el3_unit <test>\n"); return 2; }
    g_test = argv[1];
    g_mem = mmap(0, GUEST, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED){ perror("mmap"); return 2; }
    memset(g_cbits, 0xFF, GUEST >> 17);                     /* all of it committed */
    signal(SIGALRM, on_alarm);
    static const struct { const char *name; void (*f)(void); } T[] = {
        { "count", t_count }, { "stream", t_stream },
    };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++)
        if (!strcmp(T[i].name, g_test)){ T[i].f(); printf("PASS %s\n", g_test); return 0; }
    fprintf(stderr, "no test %s\n", g_test);
    return 2;
}
