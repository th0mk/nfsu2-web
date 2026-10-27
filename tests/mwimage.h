/* mwimage.h -- speed.exe's image for the native tests that run Most Wanted's
 * lifted code (tests/mwmemmove.c, tests/mworacle.c): the PE headers and every
 * section copied to their virtual addresses, as the Windows loader maps the
 * exe at its preferred base (0x00400000, so no relocation is applied).  The
 * import table is left as the file has it: nothing the tests run calls an
 * import.  The caller commits [base, base + size) of guest memory first
 * (mwimage_parse without copying gives the size). */
#ifndef MWIMAGE_H
#define MWIMAGE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t va, vsize, raw, chr; char name[9]; } MwSec;
typedef struct { uint32_t base, size, entry, nsec; MwSec sec[16]; } MwImage;

static uint32_t mwi_u32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t mwi_u16(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* parse the exe's bytes d[0..n) into *im; with copy also copy the image to
 * mem + base (mem: guest address 0, which is NULL on wasm).  0 = done, else a
 * message on stderr */
static int mwimage_parse(const uint8_t *d, size_t n, int copy, uint8_t *mem, MwImage *im){
    uint32_t pe = n > 0x40 ? mwi_u32(d + 0x3C) : 0;
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z' || (size_t)pe + 0xF8 > n || memcmp(d + pe, "PE\0\0", 4)){
        fprintf(stderr, "speed.exe: not a PE file\n"); return 1; }
    const uint8_t *fh = d + pe + 4, *oh = fh + 20;
    uint32_t nsec = mwi_u16(fh + 2), ohsz = mwi_u16(fh + 16);
    memset(im, 0, sizeof *im);
    im->base = mwi_u32(oh + 28); im->size = mwi_u32(oh + 56); im->entry = im->base + mwi_u32(oh + 16);
    uint32_t hdrs = mwi_u32(oh + 60);
    if (nsec > 16 || (size_t)pe + 24 + ohsz + 40 * nsec > n){ fprintf(stderr, "speed.exe: bad section table\n"); return 1; }
    if (copy) memcpy(mem + im->base, d, hdrs < n ? hdrs : n);
    for (uint32_t i = 0; i < nsec; i++){
        const uint8_t *s = oh + ohsz + 40 * i;
        MwSec *q = &im->sec[im->nsec++];
        memcpy(q->name, s, 8); q->name[8] = 0;
        q->vsize = mwi_u32(s + 8); q->va = im->base + mwi_u32(s + 12); q->chr = mwi_u32(s + 36);
        uint32_t raw = mwi_u32(s + 16), off = mwi_u32(s + 20), cnt = raw < q->vsize ? raw : q->vsize;
        q->raw = raw;
        if ((size_t)off + cnt > n){ fprintf(stderr, "speed.exe: section %s past the end of the file\n", q->name); return 1; }
        if (copy) memcpy(mem + q->va, d + off, cnt);
    }
    return 0;
}
/* the whole file <path> (NULL: stdin) in a malloc'd buffer, its size in *n */
static uint8_t *mwimage_read(const char *path, size_t *n){
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f){ fprintf(stderr, "%s: cannot open\n", path); return 0; }
    size_t cap = 8u << 20; uint8_t *d = malloc(cap); *n = 0;
    for (size_t r; d && (r = fread(d + *n, 1, cap - *n, f)) > 0; )
        if ((*n += r) == cap) d = realloc(d, cap *= 2);
    if (path) fclose(f);
    if (!d || !*n){ fprintf(stderr, "%s: cannot read\n", path ? path : "stdin"); free(d); return 0; }
    return d;
}
#endif
