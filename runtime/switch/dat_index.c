#include "dat_index.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint32_t dat_crc32(uint32_t crc, const void *p, size_t n){
    static uint32_t tab[256]; static int init;
    if (!init){ for (uint32_t i = 0; i < 256; i++){ uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; } init = 1; }
    const uint8_t *b = p; crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = tab[(crc ^ b[i]) & 255u] ^ (crc >> 8);
    return ~crc;
}
static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p){ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p){ return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

int dat_read_header(const uint8_t *h, unsigned *nfiles, uint64_t *index_off, uint64_t *index_size, uint64_t *total, uint32_t *index_crc){
    if (memcmp(h, "NFU2DAT1", 8) != 0) return 1;
    if (rd32(h + 8) != DAT_VERSION) return 2;
    *nfiles = rd32(h + 12); *index_off = rd64(h + 16); *index_size = rd64(h + 24); *total = rd64(h + 32); *index_crc = rd32(h + 40);
    if (*index_off < DAT_HDR_BYTES || *index_off + *index_size > *total) return 3;
    return 0;
}
static int cmp_ent(const void *a, const void *b){ return strcmp(((const DatEnt *)a)->lower, ((const DatEnt *)b)->lower); }
int dat_parse_index(DatIndex *ix, const uint8_t *buf, uint64_t size, unsigned nfiles, uint64_t total){
    memset(ix, 0, sizeof *ix);
    ix->e = calloc(nfiles ? nfiles : 1, sizeof *ix->e);
    ix->pool = malloc(size * 2 + 16);                      /* every path twice: as written and lower case */
    if (!ix->e || !ix->pool){ dat_free(ix); return 1; }
    uint64_t pos = 0; char *pp = ix->pool;
    for (unsigned i = 0; i < nfiles; i++){
        if (pos + 2 > size) { dat_free(ix); return 2; }
        unsigned pl = rd16(buf + pos); pos += 2;
        if (pos + pl + 20 > size || !pl){ dat_free(ix); return 2; }
        DatEnt *e = &ix->e[i];
        e->path = pp; memcpy(pp, buf + pos, pl); pp[pl] = 0; pp += pl + 1;
        e->lower = pp; for (unsigned k = 0; k < pl; k++) pp[k] = (char)tolower((unsigned char)e->path[k]); pp[pl] = 0; pp += pl + 1;
        pos += pl;
        e->off = rd64(buf + pos); e->size = rd64(buf + pos + 8); e->crc = rd32(buf + pos + 16); pos += 20;
        if (e->off + e->size > total) { dat_free(ix); return 3; }
    }
    ix->n = nfiles;
    qsort(ix->e, ix->n, sizeof *ix->e, cmp_ent);
    return 0;
}
void dat_free(DatIndex *ix){ free(ix->e); free(ix->pool); memset(ix, 0, sizeof *ix); }

/* the path as it is compared: lower case, '\\' as '/', no leading or trailing '/', "." parts dropped */
static void norm(const char *rel, char *out, size_t cap){
    size_t k = 0;
    while (*rel == '/' || *rel == '\\') rel++;
    for (; *rel && k + 1 < cap; rel++){
        char c = *rel == '\\' ? '/' : (char)tolower((unsigned char)*rel);
        if (c == '/' && (k == 0 || out[k - 1] == '/')) continue;
        if (c == '.' && (k == 0 || out[k - 1] == '/') && (rel[1] == '/' || rel[1] == '\\' || !rel[1])){ if (rel[1]) rel++; continue; }
        out[k++] = c;
    }
    while (k && out[k - 1] == '/') k--;
    out[k] = 0;
}
const DatEnt *dat_find(const DatIndex *ix, const char *rel){
    char key[1024]; norm(rel, key, sizeof key);
    unsigned lo = 0, hi = ix->n;
    while (lo < hi){ unsigned mid = (lo + hi) / 2; int c = strcmp(ix->e[mid].lower, key); if (!c) return &ix->e[mid]; if (c < 0) lo = mid + 1; else hi = mid; }
    return NULL;
}
/* the first entry whose path starts with "key/": entries are sorted, so the folder's files are consecutive */
static unsigned first_under(const DatIndex *ix, const char *key, size_t kl){
    unsigned lo = 0, hi = ix->n;
    while (lo < hi){ unsigned mid = (lo + hi) / 2; if (strncmp(ix->e[mid].lower, key, kl) < 0) lo = mid + 1; else hi = mid; }
    return lo;
}
int dat_is_dir(const DatIndex *ix, const char *rel){
    char key[1024]; norm(rel, key, sizeof key);
    if (!key[0]) return 1;
    size_t kl = strlen(key); key[kl] = '/'; key[kl + 1] = 0;
    unsigned i = first_under(ix, key, kl + 1);
    return i < ix->n && !strncmp(ix->e[i].lower, key, kl + 1);
}
int dat_list(const DatIndex *ix, const char *rel, DatName **out){
    char key[1024]; norm(rel, key, sizeof key);
    size_t kl = strlen(key);
    if (kl){ key[kl++] = '/'; key[kl] = 0; }
    unsigned i = first_under(ix, key, kl);
    DatName *r = malloc((ix->n + 1) * sizeof *r); if (!r) return -1;
    int n = 0;
    for (; i < ix->n && !strncmp(ix->e[i].lower, key, kl); i++){
        const char *rest = ix->e[i].path + kl, *sl = strchr(rest, '/');
        size_t l = sl ? (size_t)(sl - rest) : strlen(rest);
        /* a folder is listed once: its files are consecutive in the sorted index */
        if (sl && n && r[n - 1].is_dir && strlen(r[n - 1].name) == l && !strncasecmp(r[n - 1].name, rest, l)) continue;
        char *nm = malloc(l + 1); if (!nm){ dat_list_free(r, n); return -1; }
        memcpy(nm, rest, l); nm[l] = 0;
        r[n].name = nm; r[n].is_dir = sl != NULL; n++;
    }
    if (!n && kl && !dat_is_dir(ix, rel)){ free(r); return -1; }
    *out = r; return n;
}
void dat_list_free(DatName *l, int n){ for (int i = 0; i < n; i++) free((char *)l[i].name); free(l); }
