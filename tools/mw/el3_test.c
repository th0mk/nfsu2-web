/* The runtime's EALayer3 decoder (runtime/mw/ealayer3_mw.c) on one stream of a Most Wanted music pack
 * (tools/musicpack.py --game mw), off the game: the stream's blocks as SND hands them to it (each pair of channels
 * from its offset in the block), the samples as 16-bit, interleaved.
 *
 *   cc -O2 -Iruntime/mw -o out/el3_test tools/mw/el3_test.c -lm
 *   out/el3_test <MW_Music.mus> <stream offset> <out.raw>   ->  "channels samples rate" on stdout
 */
#include <math.h>
#define EL3_CORE_ONLY
#include "../../runtime/mw/ealayer3_mw.c"

static uint32_t be32(const uint8_t *p){ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

int main(int argc, char **argv){
    if (argc != 4){ fprintf(stderr, "usage: el3_test <MW_Music.mus> <stream offset> <out.raw>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f){ perror(argv[1]); return 1; }
    fseek(f, strtol(argv[2], 0, 0), SEEK_SET);
    size_t cap = 1u << 20, len = 0;
    uint8_t *d = malloc(cap);
    for (;;){                                          /* SCHl .. SCEl */
        uint8_t h[8];
        if (fread(h, 1, 8, f) != 8){ fprintf(stderr, "short stream\n"); return 1; }
        uint32_t sz = le32(h + 4);
        if (sz < 8){ fprintf(stderr, "bad chunk\n"); return 1; }
        while (len + sz > cap) d = realloc(d, cap *= 2);
        memcpy(d + len, h, 8);
        if (fread(d + len + 8, 1, sz - 8, f) != sz - 8){ fprintf(stderr, "short chunk\n"); return 1; }
        len += sz;
        if (!memcmp(h, "SCEl", 4)) break;
    }
    fclose(f);
    if (memcmp(d, "SCHl", 4) || memcmp(d + 8, "GSTR", 4)){ fprintf(stderr, "not a GSTR stream\n"); return 1; }
    unsigned nch = 1, rate = 0, codec = 0x0a;
    for (uint32_t i = 16; i < le32(d + 4) && d[i] != 0xff; ){
        unsigned t = d[i++];
        if (t >= 0xfc && t <= 0xfe) continue;
        unsigned n = d[i++], v = 0;
        for (unsigned k = 0; k < n; k++) v = v << 8 | d[i + k];
        if (t == 0x82) nch = v;
        if (t == 0x84) rate = v;
        if (t == 0xa0) codec = v;
        i += n;
    }
    if (codec != 0x17 || nch < 1 || nch > 8){ fprintf(stderr, "not an EALayer3 stream (codec %#x, %u channels)\n", codec, nch); return 1; }
    unsigned npair = (nch + 1) / 2;
    El3Pair *pairs = calloc(npair, sizeof *pairs);
    for (unsigned p = 0; p < npair; p++){ pairs[p].nch = nch - 2 * p >= 2 ? 2 : 1; pairs[p].readers = 1; }
    FILE *o = fopen(argv[3], "wb");
    unsigned long total = 0, blocks = 0;
    for (uint32_t at = le32(d + 4); at + 8 <= len; at += le32(d + at + 4)){
        if (memcmp(d + at, "SCDl", 4)) continue;
        uint32_t count = be32(d + at + 8);
        const uint8_t *base = d + at + 12 + 4 * nch;
        uint32_t lim = (uint32_t)(d + at + le32(d + at + 4) - base);
        for (unsigned p = 0; p < npair; p++){
            uint32_t off = be32(d + at + 12 + 8 * p);
            const char *why = el3_block(&pairs[p], base + off, lim - off, count, blocks == 0);
            if (why){ fprintf(stderr, "block %lu pair %u: %s\n", blocks, p, why); return 1; }
        }
        for (uint32_t i = 0; i < count; i++)
            for (unsigned c = 0; c < nch; c++){
                El3Pair *s = &pairs[c / 2];
                float v = s->q[(size_t)(s->rd[0] + i) * 2u + (c & 1u)];
                long r = lrintf(v);
                int16_t w = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
                fwrite(&w, 2, 1, o);
            }
        for (unsigned p = 0; p < npair; p++) pairs[p].rd[0] += count;
        total += count; blocks++;
    }
    fclose(o);
    printf("%u %lu %u\n", nch, total, rate);
    return 0;
}
