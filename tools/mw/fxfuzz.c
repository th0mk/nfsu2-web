/* fxfuzz.c -- robustness test for runtime/mw/fx2.c and tools/mw/fxdump.c:
 * truncated and corrupted copies of each effect go through fx2_parse and the
 * dump (written to /dev/null).  Each copy sits in a buffer of exactly its
 * size, so under ASan a read past it stops the run, and UBSan stops on
 * undefined behaviour (-fno-sanitize-recover=all).
 *
 *   clang -std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=all \
 *         -DFXDUMP_NO_MAIN -Iruntime/mw -o out/mw/fx/bin/fxfuzz tools/mw/fxfuzz.c tools/mw/fxdump.c runtime/mw/fx2.c
 *   out/mw/fx/bin/fxfuzz <copies per file> <file.fxo>...
 *
 * Per file: the whole file (it must parse), every truncation that cuts off
 * 1..64 bytes, 64 random truncations, then the given number of random
 * copies: 1-8 flipped bits; 1-4 dwords set to a value a parser trips on (0,
 * 1, all ones, the sign bit, the file size, a comment or end token, a random
 * value, a neighbour's value plus or minus a little), anywhere or in the
 * table area; or one of those followed by a truncation.  One copy in eight
 * is aimed instead, from the parsed original: a resource turned into an
 * array selector (usage 2) with a name size inside its data, or a sampler
 * block state pointed back at its own parameter's typedef and value (a loop
 * the parser must stop at its nesting limit).  The seed is fixed, so a run
 * repeats exactly.
 */
#include "fx2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fxdump(FILE *f, const char *name, const uint8_t *buf, uint32_t size, char *err, uint32_t errlen);

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void){ g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint32_t below(uint32_t n){ return n ? rnd() % n : 0; }

static FILE *g_null;
static unsigned long g_ok, g_refused;

/* one copy of n bytes: parse and dump it */
static void run(const char *name, const uint8_t *src, uint32_t n){
    uint8_t *b = malloc(n ? n : 1);
    if (!b){ fprintf(stderr, "fxfuzz: out of memory\n"); exit(1); }
    memcpy(b, src, n);
    char err[256];
    if (fxdump(g_null, name, b, n, err, sizeof err) == 0) g_ok++; else g_refused++;
    free(b);
}

static void put32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint32_t value(const uint8_t *b, uint32_t n, uint32_t at){
    switch (below(12)){
    case 0: return 0;
    case 1: return 1;
    case 2: return 0xFFFFFFFFu;
    case 3: return 0x80000000u;
    case 4: return 0x7FFFFFFFu;
    case 5: return n;
    case 6: return n / 4;
    case 7: return 0xFFFEu | below(0x8000) << 16;         /* a comment token */
    case 8: return FX2_END;
    case 9: return below(8);
    case 10: return rnd();
    default: return fx2_rd32(b + at) + below(9) - 4;      /* near what was there */
    }
}

/* aimed at the parsed original fx (file offsets are body offsets + 8): 1 when b was changed */
static int aim(const fx2_effect *fx, uint8_t *b, uint32_t n){
    if (below(2) && fx->nres){                              /* a selector: usage 2, S2 inside the data */
        const fx2_res *r = &fx->res[below(fx->nres)];
        uint32_t at = (uint32_t)(r->data - fx->file);
        if (r->size < 4 || at < 8) return 0;
        put32(b + at - 8, 2);
        put32(b + at, 4 * below(r->size / 4));
        return 1;
    }
    for (uint32_t k = 0, i = below(fx->nparams ? fx->nparams : 1); k < fx->nparams; k++, i = (i + 1) % fx->nparams){
        const fx2_param *pa = &fx->params[i];               /* a sampler block state that loops back */
        if (pa->v.kind != FX2_V_SAMPLER || pa->t->elements || !pa->v.blocks[0].nstates) continue;
        uint32_t at = 8 + pa->v.off + 4 + 16 * below(pa->v.blocks[0].nstates);
        if (at + 16 > n) return 0;
        put32(b + at + 8, pa->t->off);
        put32(b + at + 12, pa->v.off);
        return 1;
    }
    return 0;
}

static void fuzz_file(const char *path, const uint8_t *src, uint32_t n, unsigned copies){
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    unsigned long ok0 = g_ok;
    run(name, src, n);
    if (g_ok == ok0){ fprintf(stderr, "fxfuzz: %s: the file itself does not parse\n", path); exit(1); }
    for (uint32_t cut = 1; cut <= 64 && cut <= n; cut++) run(name, src, n - cut);
    for (int k = 0; k < 64; k++) run(name, src, below(n));

    fx2_effect fx;
    char err[256];
    if (fx2_parse(&fx, src, n, err, sizeof err)){ fprintf(stderr, "fxfuzz: %s: %s\n", path, err); exit(1); }
    uint32_t tables = n >= 8 ? 8 + fx2_rd32(src + 4) : 0;  /* the table area's file offset */
    if (tables >= n) tables = 0;
    uint8_t *b = malloc(n);
    if (!b){ fprintf(stderr, "fxfuzz: out of memory\n"); exit(1); }
    for (unsigned c = 0; c < copies; c++){
        memcpy(b, src, n);
        uint32_t len = n, how = below(4);
        if (!below(8) && aim(&fx, b, n)){ run(name, b, len); continue; }
        if (how == 0 || n < 4){
            for (uint32_t k = 0, m = 1 + below(8); k < m && n; k++){ uint32_t bit = below(n * 8); b[bit / 8] ^= (uint8_t)(1u << bit % 8); }
        } else {
            uint32_t lo = how == 2 ? tables : 0, span = (n - lo) / 4;
            for (uint32_t k = 0, m = 1 + below(4); k < m && span; k++){
                uint32_t at = lo + 4 * below(span);
                put32(b + at, value(b, n, at));
            }
            if (how == 3) len = below(n + 1);
        }
        run(name, b, len);
    }
    free(b);
    fx2_free(&fx);
}

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: fxfuzz <copies per file> <file.fxo>...\n"); return 2; }
    unsigned copies = (unsigned)strtoul(argv[1], NULL, 10);
    if (!(g_null = fopen("/dev/null", "w"))){ perror("/dev/null"); return 1; }
    unsigned long total = 0;
    for (int i = 2; i < argc; i++){
        FILE *in = fopen(argv[i], "rb");
        if (!in){ perror(argv[i]); return 1; }
        fseek(in, 0, SEEK_END);
        long sz = ftell(in);
        fseek(in, 0, SEEK_SET);
        uint8_t *src = malloc(sz > 0 ? (size_t)sz : 1);
        if (!src || sz < 0 || fread(src, 1, (size_t)sz, in) != (size_t)sz){ fprintf(stderr, "fxfuzz: %s: cannot read\n", argv[i]); return 1; }
        fclose(in);
        unsigned long before = g_ok + g_refused;
        fuzz_file(argv[i], src, (uint32_t)sz, copies);
        total += g_ok + g_refused - before;
        free(src);
    }
    fclose(g_null);
    printf("fxfuzz: %lu copies of %d effects: %lu parsed and dumped, %lu refused by the parser, no crash\n",
           total, argc - 2, g_ok, g_refused);
    return 0;
}
