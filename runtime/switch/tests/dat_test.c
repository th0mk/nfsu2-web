/* dat_test.c -- nfsu2.dat against the folder it was made from: every file found (in any case) with its size, the content of
 * a sample checked by CRC, and every folder's listing the same as the real one.
 * cc -O1 -g runtime/switch/dat_index.c runtime/switch/tests/dat_test.c -o dat_test && ./dat_test nfsu2.dat /path/to/NFSU2 */
#include "../dat_index.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static FILE *g_dat; static DatIndex g_ix; static int g_fail, g_checked, g_dirs, g_crc;
static const char *g_root;
static int skip(const char *rel){
    if (!strncasecmp(rel, "Uninstall", 9) || !strncasecmp(rel, "Support", 7)) return 1;
    return !strcasecmp(rel, "Uninstall.exe") || !strcasecmp(rel, "SPEED22.EXE");
}
static void upcase(char *s){ for (; *s; s++) *s = (char)toupper((unsigned char)*s); }
static void walk(const char *rel){
    char dir[2048]; snprintf(dir, sizeof dir, "%s/%s", g_root, rel);
    DIR *d = opendir(dir); if (!d) return;
    char **names = NULL; int nn = 0;
    struct dirent *e;
    while ((e = readdir(d))){
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[2048]; snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        if (!rel[0] && skip(e->d_name)) continue;
        char full[4096]; snprintf(full, sizeof full, "%s/%s", g_root, r);
        struct stat st; if (stat(full, &st) != 0) continue;
        names = realloc(names, (nn + 1) * sizeof *names); names[nn++] = strdup(e->d_name);
        if (S_ISDIR(st.st_mode)){
            if (!dat_is_dir(&g_ix, r)){ printf("FAIL dir missing: %s\n", r); g_fail++; }
            char up[2048]; snprintf(up, sizeof up, "%s", r); upcase(up);
            if (!dat_is_dir(&g_ix, up)){ printf("FAIL dir (upper) missing: %s\n", up); g_fail++; }
            walk(r);
        } else {
            const DatEnt *x = dat_find(&g_ix, r);
            if (!x){ printf("FAIL file missing: %s\n", r); g_fail++; continue; }
            char bs[2048]; snprintf(bs, sizeof bs, "%s", r); for (char *c = bs; *c; c++) if (*c == '/') *c = '\\'; upcase(bs);
            const DatEnt *y = dat_find(&g_ix, bs);
            if (y != x){ printf("FAIL case/backslash lookup: %s\n", bs); g_fail++; }
            if ((uint64_t)st.st_size != x->size){ printf("FAIL size %s: %llu vs %llu\n", r, (unsigned long long)x->size, (unsigned long long)st.st_size); g_fail++; }
            g_checked++;
            if (g_checked % 25 == 0 || x->size > 50u << 20){      /* a sample, and every big file */
                FILE *f = fopen(full, "rb"); uint32_t c = 0; static unsigned char b[1 << 20]; size_t n;
                if (f){ while ((n = fread(b, 1, sizeof b, f)) > 0) c = dat_crc32(c, b, n); fclose(f); }
                unsigned char *copy = malloc(x->size ? x->size : 1); uint32_t c2 = 0;
                fseeko(g_dat, (off_t)x->off, SEEK_SET); if (fread(copy, 1, x->size, g_dat) != x->size) { printf("FAIL short read %s\n", r); g_fail++; }
                c2 = dat_crc32(0, copy, x->size); free(copy);
                g_crc++;
                if (c != x->crc || c2 != x->crc){ printf("FAIL crc %s: index %08x real %08x dat %08x\n", r, x->crc, c, c2); g_fail++; }
            }
        }
    }
    closedir(d);
    DatName *l = NULL; int ln = dat_list(&g_ix, rel, &l);
    if (ln != nn){ printf("FAIL listing %s: dat %d real %d\n", rel[0] ? rel : "(root)", ln, nn); g_fail++; }
    else for (int i = 0; i < nn; i++){
        int found = 0; for (int k = 0; k < ln; k++) if (!strcasecmp(l[k].name, names[i])) found = 1;
        if (!found){ printf("FAIL listing %s lacks %s\n", rel, names[i]); g_fail++; }
    }
    g_dirs++;
    if (l) dat_list_free(l, ln);
    for (int i = 0; i < nn; i++) free(names[i]);
    free(names);
}
int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: dat_test <nfsu2.dat> <game folder>\n"); return 2; }
    g_root = argv[2];
    g_dat = fopen(argv[1], "rb"); if (!g_dat){ perror("dat"); return 2; }
    unsigned char hdr[64]; if (fread(hdr, 1, 64, g_dat) != 64) return 2;
    unsigned nf; uint64_t io, is, tot; uint32_t icrc;
    int rc = dat_read_header(hdr, &nf, &io, &is, &tot, &icrc); if (rc){ printf("FAIL header %d\n", rc); return 1; }
    fseeko(g_dat, 0, SEEK_END); if ((uint64_t)ftello(g_dat) != tot){ printf("FAIL file size %lld vs header %llu\n", (long long)ftello(g_dat), (unsigned long long)tot); return 1; }
    unsigned char *ib = malloc(is); fseeko(g_dat, (off_t)io, SEEK_SET); if (fread(ib, 1, is, g_dat) != is) return 2;
    if (dat_crc32(0, ib, is) != icrc){ printf("FAIL index crc\n"); return 1; }
    rc = dat_parse_index(&g_ix, ib, is, nf, tot); if (rc){ printf("FAIL parse %d\n", rc); return 1; }
    /* negative cases */
    if (dat_find(&g_ix, "NOPE/NOTHING.BIN") || dat_is_dir(&g_ix, "NOPE") || dat_find(&g_ix, "CARS")) { printf("FAIL negative lookups\n"); g_fail++; }
    DatName *l; if (dat_list(&g_ix, "NOPE", &l) != -1){ printf("FAIL list of a missing folder\n"); g_fail++; }
    walk("");
    printf("%u files in the index; %d files looked up, %d folders listed, %d CRCs checked; %d failures\n", nf, g_checked, g_dirs, g_crc, g_fail);
    return g_fail ? 1 : 0;
}
