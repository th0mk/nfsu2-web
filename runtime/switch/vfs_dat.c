/* vfs_dat.c -- the game folder inside nfsu2.dat, under the file calls the runtime already makes (stat, opendir/readdir, fopen
 * and, in nx_host.c, open).  With the file mounted, everything under sdmc:/switch/nfsu2/game/ is answered from its index and
 * read from it; without it nothing here does anything and the folder on the SD card is used as before.  The index logic is
 * dat_index.c (tested on the PC against the real folder). */
#include <switch.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "dat_index.h"

#define GAME_ROOT "/switch/nfsu2/game"

extern int __real_stat(const char *path, struct stat *st);
extern DIR *__real_opendir(const char *path);
extern struct dirent *__real_readdir(DIR *d);
extern int __real_closedir(DIR *d);
extern void __real_rewinddir(DIR *d);
extern FILE *__real_fopen(const char *path, const char *mode);
int __wrap_open(const char *path, int flags, ...);
ssize_t __wrap_read(int fd, void *buf, size_t n);
off_t __wrap_lseek(int fd, off_t off, int whence);
int __wrap_close(int fd);

static DatIndex g_ix;
static FsFile g_datf;
static int g_mounted;

/* the .dat is read from one of two places: a file on the SD card (g_datf), or, in the NSP build, a file inside the program's own
 * RomFS (g_st, the data storage of the current process; g_rombase is where the .dat's bytes start in it) */
static FsStorage g_st; static int g_use_st; static s64 g_rombase;
static int dat_rd(s64 off, void *buf, u64 n){
    u64 got = 0;
    if (g_use_st) return R_SUCCEEDED(fsStorageRead(&g_st, g_rombase + off, buf, n));
    return R_SUCCEEDED(fsFileRead(&g_datf, off, buf, n, FsReadOption_None, &got)) && got == n;
}
int vfs_dat_use_storage(void){ return g_use_st; }
FsStorage *vfs_dat_storage(void){ return &g_st; }

/* the header, the size check and the index of an opened .dat; 0 on success, else a message for the screen in err */
static int dat_mount_open(s64 size, char *err, size_t errsz){
    uint8_t hdr[DAT_HDR_BYTES];
    unsigned nfiles; uint64_t ioff, isize, total; uint32_t icrc;
    if (!dat_rd(0, hdr, sizeof hdr) || dat_read_header(hdr, &nfiles, &ioff, &isize, &total, &icrc)){
        snprintf(err, errsz, "nfsu2.dat esta corrompido (cabecalho).\nCopie o arquivo de novo para o cartao SD."); return 2; }
    if ((uint64_t)size != total){
        snprintf(err, errsz, "nfsu2.dat esta incompleto: %llu de %llu MB.\nA copia foi interrompida; copie o arquivo de novo.",
                 (unsigned long long)(size >> 20), (unsigned long long)(total >> 20)); return 3; }
    uint8_t *ib = malloc(isize ? isize : 1);
    if (!ib || !dat_rd((s64)ioff, ib, isize) || dat_crc32(0, ib, isize) != icrc){
        free(ib); snprintf(err, errsz, "nfsu2.dat esta corrompido (indice).\nCopie o arquivo de novo para o cartao SD."); return 4; }
    int rc = dat_parse_index(&g_ix, ib, isize, nfiles, total);
    free(ib);
    if (rc){ snprintf(err, errsz, "nfsu2.dat esta corrompido (indice, %d).", rc); return 5; }
    g_mounted = 1;
    fprintf(stderr, "[nx] nfsu2.dat (%s): %u files, %llu MB\n", g_use_st ? "inside the NSP's RomFS" : "SD card", nfiles, (unsigned long long)(total >> 20));
    return 0;
}

/* mounts nfsu2.dat from the SD card; 0 on success, else a message for the screen in err */
int vfs_dat_mount(const char *path, char *err, size_t errsz){
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    const char *p = !strncmp(path, "sdmc:", 5) ? path + 5 : path;
    if (!fs || R_FAILED(fsFsOpenFile(fs, p, FsOpenMode_Read, &g_datf))){ snprintf(err, errsz, "nfsu2.dat nao pode ser aberto"); return 1; }
    s64 size = 0; fsFileGetSize(&g_datf, &size);
    g_use_st = 0;
    return dat_mount_open(size, err, errsz);
}

/* the NSP build: nfsu2.dat is the one file in the root of the program's RomFS.  Returns 0 mounted, 1 no RomFS / no such file (an NRO run
 * from the homebrew menu has none: the caller then looks on the SD card), else a message for the screen like vfs_dat_mount.
 * The RomFS layout (switchbrew "RomFS"): header { u64 headerSize, dirHash{off,size}, dirMeta{off,size}, fileHash{off,size}, fileMeta{off,size},
 * u64 fileDataOff }; a file entry in fileMeta is { u32 parent, sibling; u64 off, size; u32 hashSibling, nameLen; char name[] } (name padded to 4). */
int vfs_dat_mount_romfs(char *err, size_t errsz){
    if (R_FAILED(fsOpenDataStorageByCurrentProcess(&g_st))) return 1;
    u64 h[10];
    if (R_FAILED(fsStorageRead(&g_st, 0, h, sizeof h)) || h[0] < sizeof h){ fsStorageClose(&g_st); return 1; }
    u64 metaOff = h[7], metaSize = h[8], dataOff = h[9];
    if (metaSize == 0 || metaSize > (16u << 20)){ fsStorageClose(&g_st); return 1; }
    uint8_t *meta = malloc(metaSize);
    if (!meta || R_FAILED(fsStorageRead(&g_st, (s64)metaOff, meta, metaSize))){ free(meta); fsStorageClose(&g_st); return 1; }
    s64 foff = -1, fsize = 0;
    for (u64 o = 0; o + 0x20 <= metaSize; ){
        uint32_t nlen; uint64_t eoff, esz; memcpy(&eoff, meta + o + 8, 8); memcpy(&esz, meta + o + 16, 8); memcpy(&nlen, meta + o + 28, 4);
        if (o + 0x20 + nlen > metaSize) break;
        if (nlen == 9 && !memcmp(meta + o + 0x20, "nfsu2.dat", 9)){ foff = (s64)(dataOff + eoff); fsize = (s64)esz; break; }
        o += 0x20 + ((nlen + 3u) & ~3u);
    }
    free(meta);
    if (foff < 0){ fsStorageClose(&g_st); return 1; }
    g_use_st = 1; g_rombase = foff;
    int rc = dat_mount_open(fsize, err, errsz);
    if (rc) g_use_st = 0;
    return rc;
}
int vfs_dat_mounted(void){ return g_mounted; }

/* the path under the game folder: 1 and the relative part in *rel, 0 for anything else (or nothing mounted) */
static int under_root(const char *path, const char **rel){
    if (!g_mounted) return 0;
    if (!strncmp(path, "sdmc:", 5)) path += 5;
    size_t l = strlen(GAME_ROOT);
    if (strncmp(path, GAME_ROOT, l) || (path[l] && path[l] != '/')) return 0;
    path += l; while (*path == '/') path++;
    *rel = path; return 1;
}
/* 1 a file of the .dat (f, base, size filled: the .dat's own handle, shared), 0 under the folder but not there, -1 not ours */
int vfs_dat_open(const char *path, FsFile *f, s64 *base, s64 *size){
    const char *rel; if (!under_root(path, &rel)) return -1;
    const DatEnt *e = dat_find(&g_ix, rel);
    if (!e) return 0;
    *f = g_datf; *base = (s64)e->off + (g_use_st ? g_rombase : 0); *size = (s64)e->size;
    return 1;
}

int __wrap_stat(const char *path, struct stat *st){
    const char *rel;
    if (!under_root(path, &rel)) return __real_stat(path, st);
    memset(st, 0, sizeof *st);
    const DatEnt *e = dat_find(&g_ix, rel);
    if (e){ st->st_mode = S_IFREG | 0444; st->st_size = (off_t)e->size; st->st_nlink = 1; return 0; }
    if (dat_is_dir(&g_ix, rel)){ st->st_mode = S_IFDIR | 0555; st->st_nlink = 2; return 0; }
    errno = ENOENT; return -1;
}

/* folders: a handle of ours (told apart from libnx's by the table) holding the listing */
typedef struct { DatName *l; int n, i; struct dirent de; } VDir;
#define MAXV 32
static VDir *g_vd[MAXV];
static pthread_mutex_t g_vd_lock = PTHREAD_MUTEX_INITIALIZER;
static VDir *vd_of(DIR *d){
    VDir *r = NULL;
    pthread_mutex_lock(&g_vd_lock);
    for (int i = 0; i < MAXV; i++) if ((void *)g_vd[i] == (void *)d) { r = g_vd[i]; break; }
    pthread_mutex_unlock(&g_vd_lock);
    return r;
}
DIR *__wrap_opendir(const char *path){
    const char *rel;
    if (!under_root(path, &rel)) return __real_opendir(path);
    DatName *l = NULL; int n = dat_list(&g_ix, rel, &l);
    if (n < 0){ errno = ENOENT; return NULL; }
    VDir *v = calloc(1, sizeof *v); if (!v){ dat_list_free(l, n); errno = ENOMEM; return NULL; }
    v->l = l; v->n = n;
    pthread_mutex_lock(&g_vd_lock);
    for (int i = 0; i < MAXV; i++) if (!g_vd[i]){ g_vd[i] = v; pthread_mutex_unlock(&g_vd_lock); return (DIR *)v; }
    pthread_mutex_unlock(&g_vd_lock);
    dat_list_free(l, n); free(v); errno = EMFILE; return NULL;
}
struct dirent *__wrap_readdir(DIR *d){
    VDir *v = vd_of(d);
    if (!v) return __real_readdir(d);
    if (v->i >= v->n) return NULL;
    memset(&v->de, 0, sizeof v->de);
    strncpy(v->de.d_name, v->l[v->i].name, sizeof v->de.d_name - 1);
    v->de.d_ino = (ino_t)(v->i + 1);
    v->i++;
    return &v->de;
}
void __wrap_rewinddir(DIR *d){ VDir *v = vd_of(d); if (v) v->i = 0; else __real_rewinddir(d); }
int __wrap_closedir(DIR *d){
    pthread_mutex_lock(&g_vd_lock);
    VDir *v = NULL;
    for (int i = 0; i < MAXV; i++) if ((void *)g_vd[i] == (void *)d){ v = g_vd[i]; g_vd[i] = NULL; break; }
    pthread_mutex_unlock(&g_vd_lock);
    if (!v) return __real_closedir(d);
    dat_list_free(v->l, v->n); free(v);
    return 0;
}

/* fopen "r" of a file in the .dat: a stdio stream over the same descriptors (the exe is loaded this way) */
static int ck_read(void *c, char *b, size_t n){ return (int)__wrap_read((int)(intptr_t)c, b, n); }
static fpos_t ck_seek(void *c, fpos_t off, int whence){ return (fpos_t)__wrap_lseek((int)(intptr_t)c, (off_t)off, whence); }
static int ck_close(void *c){ return __wrap_close((int)(intptr_t)c); }
FILE *__wrap_fopen(const char *path, const char *mode){
    const char *rel;
    if (!under_root(path, &rel) || mode[0] != 'r' || strchr(mode, '+')) return __real_fopen(path, mode);
    int fd = __wrap_open(path, O_RDONLY);
    if (fd < 0) return NULL;
    FILE *f = funopen((void *)(intptr_t)fd, ck_read, NULL, ck_seek, ck_close);
    if (!f) __wrap_close(fd);
    return f;
}
