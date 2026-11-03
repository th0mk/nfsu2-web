/* dat_index.h -- the game folder as one file (nfsu2.dat) and the index that finds things in it.  Plain C, no console calls:
 * the same code is tested on the PC against the real folder (tests/dat_test.c); vfs_dat.c puts it under the game's file calls.
 *
 * Layout (little endian):
 *   header, 64 bytes:  "NFU2DAT1", u32 version (1), u32 nfiles, u64 index offset, u64 index size, u64 total size (the whole
 *                      file: a copy that stopped half way is caught by the size), u32 CRC-32 of the index, zeros
 *   data:              every file's bytes, each at a 16-byte boundary
 *   index:             per file: u16 path length, the path ('/' separated, relative to the game folder, original case),
 *                      u64 offset, u64 size, u32 CRC-32 of the file's bytes
 * Names are found case-insensitively (the game asks for "CARS\350Z\GEOMETRY.BIN" and the like in any case). */
#ifndef NFSU2_DAT_INDEX_H
#define NFSU2_DAT_INDEX_H
#include <stdint.h>
#include <stddef.h>

#define DAT_HDR_BYTES 64u
#define DAT_VERSION   1u

typedef struct { char *path; char *lower; uint64_t off, size; uint32_t crc; } DatEnt;
typedef struct { DatEnt *e; unsigned n; char *pool; } DatIndex;      /* e sorted by lower */
typedef struct { const char *name; int is_dir; } DatName;

uint32_t dat_crc32(uint32_t crc, const void *p, size_t n);          /* start with 0, chain the result */
/* 0 on success */
int  dat_read_header(const uint8_t *hdr, unsigned *nfiles, uint64_t *index_off, uint64_t *index_size, uint64_t *total, uint32_t *index_crc);
int  dat_parse_index(DatIndex *ix, const uint8_t *buf, uint64_t size, unsigned nfiles, uint64_t total);
void dat_free(DatIndex *ix);
/* a file by its path relative to the game folder; NULL if it is not one */
const DatEnt *dat_find(const DatIndex *ix, const char *rel);
int  dat_is_dir(const DatIndex *ix, const char *rel);               /* "" is the game folder itself */
/* the entries directly in a folder (names as written in the index); free with dat_list_free; returns the count, -1 if rel is no folder */
int  dat_list(const DatIndex *ix, const char *rel, DatName **out);
void dat_list_free(DatName *l, int n);
#endif
