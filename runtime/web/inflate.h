/* inflate.h -- raw deflate (RFC 1951) unpacked into a buffer of known size: the texture packs' version 2 entries
 * (texrep_web.c), which tools/texpacks.py writes with zlib's raw deflate.  inflate_raw unpacks one whole stream,
 * with stored, fixed and dynamic blocks: 1 when it filled exactly out's n bytes and the last block ended there, 0 for
 * a stream that is short, long or broken.  Each code is looked up in a table of 2^15 entries, made per block.  Not
 * reentrant (the tables are static); web/test/inflate.test.mjs checks it against Node's zlib. */
#ifndef NFSU2_INFLATE_H
#define NFSU2_INFLATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INF_BITS 15                               /* the longest code deflate has */

typedef struct { const uint8_t *p, *end; uint64_t bits; unsigned n; size_t past; } InfBits;
/* at least k bits in the buffer; past the stream's end zeros, counted in past */
static inline void inf_need(InfBits *b, unsigned k){
    while (b->n < k){
        uint64_t v = 0;
        if (b->p < b->end) v = *b->p++; else b->past++;
        b->bits |= v << b->n; b->n += 8;
    }
}
static inline unsigned inf_get(InfBits *b, unsigned k){
    if (!k) return 0;
    inf_need(b, k);
    unsigned v = (unsigned)(b->bits & ((1u << k) - 1u));
    b->bits >>= k; b->n -= k;
    return v;
}
/* a canonical code's table from its code lengths: per INF_BITS-bit peek, length << 9 | symbol, 0 for no code
 * (an incomplete code's gaps); 0 if the lengths are more than a code can have */
static int inf_table(uint16_t *tab, const uint8_t *len, unsigned n){
    unsigned count[16] = {0}, next[16];
    for (unsigned i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    int left = 1;
    for (unsigned l = 1; l < 16; l++){ left = left * 2 - (int)count[l]; if (left < 0) return 0; }
    next[1] = 0;
    for (unsigned l = 1; l < 15; l++) next[l + 1] = (next[l] + count[l]) << 1;
    memset(tab, 0, sizeof(uint16_t) << INF_BITS);
    for (unsigned s = 0; s < n; s++){
        unsigned l = len[s], c, r = 0;
        if (!l) continue;
        c = next[l]++;
        for (unsigned k = 0; k < l; k++) r |= ((c >> k) & 1u) << (l - 1u - k);   /* deflate sends codes high bit first */
        for (unsigned j = r; j < (1u << INF_BITS); j += 1u << l) tab[j] = (uint16_t)(l << 9 | s);
    }
    return 1;
}
static inline int inf_sym(InfBits *b, const uint16_t *tab){
    inf_need(b, INF_BITS);
    unsigned e = tab[b->bits & ((1u << INF_BITS) - 1u)];
    if (!e) return -1;
    b->bits >>= e >> 9; b->n -= e >> 9;
    return (int)(e & 511u);
}
static int inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t n){
    static const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                       115, 131, 163, 195, 227, 258};
    static const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                       1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
                                     13, 13};
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    static uint16_t lit[1u << INF_BITS], dist[1u << INF_BITS];
    InfBits b = { in, in + inlen, 0, 0, 0 };
    size_t o = 0;
    unsigned last;
    do {
        last = inf_get(&b, 1);
        unsigned type = inf_get(&b, 2);
        if (type == 0){                                          /* stored */
            inf_get(&b, b.n & 7u);                               /* to the next byte */
            unsigned len = inf_get(&b, 16), nlen = inf_get(&b, 16);
            if ((len ^ 0xFFFFu) != nlen || len > n - o) return 0;
            while (len--) out[o++] = (uint8_t)inf_get(&b, 8);
        } else if (type == 1 || type == 2){
            uint8_t lens[288 + 32];
            unsigned nlit, ndist;
            if (type == 1){                                      /* the fixed codes */
                nlit = 288; ndist = 30;
                memset(lens, 8, 144); memset(lens + 144, 9, 112); memset(lens + 256, 7, 24); memset(lens + 280, 8, 8);
                memset(lens + 288, 5, 30);
            } else {                                             /* codes of the block's own */
                uint8_t cl[19] = {0};
                nlit = inf_get(&b, 5) + 257; ndist = inf_get(&b, 5) + 1;
                unsigned ncl = inf_get(&b, 4) + 4;
                if (nlit > 286 || ndist > 30) return 0;
                for (unsigned i = 0; i < ncl; i++) cl[ORDER[i]] = (uint8_t)inf_get(&b, 3);
                if (!inf_table(dist, cl, 19)) return 0;          /* (dist holds the code lengths' code for now) */
                for (unsigned i = 0; i < nlit + ndist; ){
                    int s = inf_sym(&b, dist);
                    if (s < 0) return 0;
                    if (s < 16){ lens[i++] = (uint8_t)s; continue; }
                    unsigned v = 0, rep;
                    if (s == 16){ if (!i) return 0; v = lens[i - 1]; rep = 3 + inf_get(&b, 2); }
                    else if (s == 17) rep = 3 + inf_get(&b, 3);
                    else rep = 11 + inf_get(&b, 7);
                    if (rep > nlit + ndist - i) return 0;
                    while (rep--) lens[i++] = (uint8_t)v;
                }
                if (!lens[256]) return 0;                        /* no end of block */
            }
            if (!inf_table(lit, lens, nlit) || !inf_table(dist, lens + nlit, ndist)) return 0;
            for (;;){
                int s = inf_sym(&b, lit);
                if (s < 0) return 0;
                if (s < 256){
                    if (o >= n) return 0;
                    out[o++] = (uint8_t)s;
                    continue;
                }
                if (s == 256) break;
                if ((s -= 257) >= 29) return 0;
                size_t len = LBASE[s] + inf_get(&b, LEXT[s]);
                int d = inf_sym(&b, dist);
                if (d < 0 || d >= 30) return 0;
                size_t back = DBASE[d] + inf_get(&b, DEXT[d]);
                if (back > o || len > n - o) return 0;
                uint8_t *q = out + o;
                const uint8_t *r = q - back;
                if (back >= len) memcpy(q, r, len);
                else for (size_t k = 0; k < len; k++) q[k] = r[k];   /* a run: it copies what it writes */
                o += len;
            }
        } else return 0;
        if (b.past > 8) return 0;                                /* read past the stream's end */
    } while (!last);
    return o == n && b.past * 8u <= b.n;                         /* the zeros past the end are still unread */
}
#endif
