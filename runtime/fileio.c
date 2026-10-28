/* Guest file I/O -> POSIX.  Maps Windows paths to the game folder and
 * resolves them case-insensitively.                                    */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
static uint64_t now_ns_io(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

void mem_commit(uint32_t,uint32_t);

static char g_root[1024] = ".";
static char g_cwd_rel[512] = "";           /* current directory inside the game */
uint64_t g_file_open=0, g_file_fail=0, g_file_read=0, g_bytes_read=0;
uint64_t g_overlapped_reads=0;
static int g_empty_next = 0;

void fileio_set_root(const char *exe_path){
    strncpy(g_root, exe_path, sizeof g_root - 1);
    char *slash = strrchr(g_root, '/');
    if (slash) *slash = 0; else strcpy(g_root, ".");
    fprintf(stderr,"file: game root folder = %s\n", g_root);
}

/* Turn a Windows path into a real path.
 *   "C:\\NFSU2\\TRACKS\\X.BUN" | "TRACKS\\X.BUN" | ".\\X" -> <root>/TRACKS/X.BUN
 * The virtual game folder is "C:\\NFSU2", so that prefix is dropped.
 * Components are resolved case-insensitively.                          */
/* For opens meant for writing, create the missing parent folders (on Windows
 * the game creates them separately; here the whole path must be ready). */
static void mkparents(const char *path){
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *q = tmp + 1; *q; q++){
        if (*q != '/') continue;
        *q = 0; mkdir(tmp, 0755); *q = '/';
    }
}

/* Drop "." and empty components and resolve ".." lexically, never above the
 * root: "..\\..\\x" must not reach outside the game folder (or the save
 * overlay), whatever the guest asks for. */
static void rel_normalize(char *rel){
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", rel);
    size_t k = 0; char *save = 0;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(0, "/", &save)){
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")){
            while (k > 0 && rel[k-1] != '/') k--;
            if (k > 0) k--;
            continue; }
        size_t l = strlen(tok);
        if (k + l + 2 > sizeof tmp) break;
        if (k) rel[k++] = '/';
        memcpy(rel + k, tok, l); k += l;
    }
    rel[k] = 0;
}

/* the guest path relative to the game folder, '/'-separated and normalized */
static void rel_of(const char *win, char *rel, size_t relsz){
    size_t k=0;
    const char *p = win;
    if (p[0] && p[1]==':') p += 2;                       /* drive letter   */
    while (*p=='\\' || *p=='/') p++;
    if (!strncasecmp(p,"NFSU2",5) && (p[5]=='\\'||p[5]=='/'||p[5]==0)){
        p += 5; while (*p=='\\'||*p=='/') p++;           /* virtual root   */
    }
    while (p[0]=='.' && (p[1]=='\\'||p[1]=='/')) p += 2;
    for (; *p && k<relsz-1; p++) rel[k++] = (*p=='\\') ? '/' : *p;
    rel[k]=0;
    rel_normalize(rel);
}
static int resolve_in(const char *root, const char *win, char *out, size_t outsz){
    char rel[1024];
    rel_of(win, rel, sizeof rel);
    if (!rel[0]){ snprintf(out,outsz,"%s",root); return 1; }

    snprintf(out, outsz, "%s/%s", root, rel);
    struct stat st;
    if (stat(out, &st)==0) return 1;

    /* component by component, case-insensitive */
    char cur[1024]; snprintf(cur, sizeof cur, "%s", root);
    char tmp[1024]; snprintf(tmp,sizeof tmp,"%s",rel);
    char *save=0; int missing=0;
    for (char *tok=strtok_r(tmp,"/",&save); tok; tok=strtok_r(0,"/",&save)){
        if (missing){ size_t l=strlen(cur); snprintf(cur+l,sizeof cur-l,"/%s",tok); continue; }
        char cand[1024]; snprintf(cand,sizeof cand,"%s/%s",cur,tok);
        if (stat(cand,&st)==0){ snprintf(cur,sizeof cur,"%s",cand); continue; }
        DIR *d = opendir(cur); int found=0;
        if (d){ struct dirent *e;
            while ((e=readdir(d))){
                if (!strcasecmp(e->d_name, tok)){
                    snprintf(cand,sizeof cand,"%s/%s",cur,e->d_name);
                    found=1; break; } }
            closedir(d); }
        if (found) snprintf(cur,sizeof cur,"%s",cand);
        else { snprintf(cand,sizeof cand,"%s/%s",cur,tok);
               snprintf(cur,sizeof cur,"%s",cand); missing=1; }
    }
    snprintf(out,outsz,"%s",cur);
    return !missing;
}

/* --- write root (save overlay) ------------------------------------------
 * The game writes its saves and profiles into ITS OWN folder.  With the data
 * embedded inside the .app, writing there breaks the bundle's signature; the
 * saves would also be wiped when the app is updated.  If a write root is set:
 *   - write / create / delete always go to the write root,
 *   - reads look in the write root first, otherwise in the game root,
 *   - a directory listing is the union of both.
 * If it is not set (from a terminal, or a bundle with its data outside)
 * everything stays in the single root as before. */
static char g_wroot[1024] = "";
void fileio_set_write_root(const char *dir){
    if (!dir || !*dir){ g_wroot[0] = 0; return; }
    snprintf(g_wroot, sizeof g_wroot, "%s", dir);
    mkparents(g_wroot); mkdir(g_wroot, 0755);
    fprintf(stderr,"file: save folder = %s\n", g_wroot);
}
/* runtime/module.c: a file in the game folder itself (never the save
 * overlay), such as server.dll; 0 if it does not exist. */
int fileio_resolve_game(const char *win, char *out, size_t outsz){
    struct stat st;
    return resolve_in(g_root, win, out, outsz) && stat(out, &st) == 0 && S_ISREG(st.st_mode);
}
/* The save overlay holds what the game wrote (its profile folder) and the
 * runtime's own files, yet every read looked there first.  In the browser each
 * step of that is a round trip to the I/O worker and an OPFS call (a stat,
 * then a listing of the overlay root for the case-insensitive match): ~1.5 to
 * 2 ms per open with the guest lock held, and a race streams its track by
 * opening TRACKS\STREAM*.BUN again and again, from the game thread too, which
 * cost it 120 Hz slots.  So whether the overlay root has a folder of the game
 * root (TRACKS, CARS, ...; the runtime makes none of those there) is
 * remembered until the next write-side operation (write_allowed). */
static unsigned g_ov_gen = 1;
#define OVC_N 64
static struct { char name[64]; unsigned gen; signed char game_dir, in_ov; } g_ovc[OVC_N];
static int g_ovc_n;
static uint64_t g_ovc_skips;
static int overlay_cannot_have(const char *win){
    char rel[1024]; rel_of(win, rel, sizeof rel);
    char *sl = strchr(rel, '/');
    if (!sl) return 0;                                   /* a file at the top: always looked up */
    *sl = 0;
    if (!rel[0] || strlen(rel) >= sizeof g_ovc[0].name) return 0;
    int i;
    for (i = 0; i < g_ovc_n && strcasecmp(g_ovc[i].name, rel); i++) {}
    if (i == g_ovc_n){
        if (g_ovc_n == OVC_N) return 0;
        snprintf(g_ovc[i].name, sizeof g_ovc[i].name, "%s", rel);
        char p[1100]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", g_root, rel);
        g_ovc[i].game_dir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);   /* the game folder never changes */
        g_ovc[i].gen = 0; g_ovc_n++;
    }
    if (!g_ovc[i].game_dir) return 0;
    if (g_ovc[i].gen != g_ov_gen){
        char w[1100];
        g_ovc[i].in_ov = (signed char)resolve_in(g_wroot, rel, w, sizeof w);
        g_ovc[i].gen = g_ov_gen;
    }
    if (g_ovc[i].in_ov) return 0;
    g_ovc_skips++;
    return 1;
}
/* --- language pack (runtime/language.c) ---------------------------------
 * LANGPACKS/<language> in the game folder holds the files a language plays
 * with in place of the game folder's: a file there is read instead of the
 * game's file of the same path (case-insensitive), as long as the save
 * overlay has no copy of its own.  Its list is read once, so every other
 * open costs nothing more. */
static char g_lroot[1024];
static struct lfile { char *key, *rel; } *g_lf;     /* sorted by key: the path lower-cased */
static int g_lnf, g_lcap;
static int g_voices_original;
static int lfile_cmp(const void *a, const void *b){
    return strcmp(((const struct lfile *)a)->key, ((const struct lfile *)b)->key);
}
static void lpack_walk(const char *dir, const char *rel){
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))){
        if (e->d_name[0] == '.') continue;
        char p[1100], r[1024]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)){ lpack_walk(p, r); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (g_voices_original && (!strncasecmp(r, "speech/", 7) || !strncasecmp(r, "movies/", 7))) continue;
        if (g_lnf == g_lcap){
            g_lcap = g_lcap ? 2*g_lcap : 64;
            struct lfile *nf = realloc(g_lf, (size_t)g_lcap * sizeof *g_lf); if (!nf) break;
            g_lf = nf;
        }
        g_lf[g_lnf].rel = strdup(r); g_lf[g_lnf].key = strdup(r);
        for (char *k = g_lf[g_lnf].key; *k; k++) *k = (char)tolower((unsigned char)*k);
        g_lnf++;
    }
    closedir(d);
}
/* -> how many files the pack has (0: none, or no such folder) */
int fileio_set_language_pack(const char *rel){
    const char *v = getenv("NFSU2_VOICES");
    g_voices_original = (v && !strcasecmp(v, "original")) ? 1 : 0;
    if (!resolve_in(g_root, rel, g_lroot, sizeof g_lroot)) return 0;
    lpack_walk(g_lroot, "");
    qsort(g_lf, (size_t)g_lnf, sizeof *g_lf, lfile_cmp);
    return g_lnf;
}
static int lpack_resolve(const char *win, char *out, size_t outsz){
    if (!g_lnf) return 0;
    char rel[1024]; rel_of(win, rel, sizeof rel);
    for (char *k = rel; *k; k++) *k = (char)tolower((unsigned char)*k);
    struct lfile key = { rel, 0 };
    const struct lfile *f = bsearch(&key, g_lf, (size_t)g_lnf, sizeof *g_lf, lfile_cmp);
    if (!f) return 0;
    snprintf(out, outsz, "%s/%s", g_lroot, f->rel);
    return 1;
}
/* Read: the write root if the file is there, then the language pack, otherwise the game root. */
static int resolve(const char *win, char *out, size_t outsz){
    if (g_wroot[0] && !overlay_cannot_have(win)){
        char w[1024];
        if (resolve_in(g_wroot, win, w, sizeof w) && strcmp(w, g_wroot) != 0){
            snprintf(out, outsz, "%s", w); return 1; }
    }
    if (lpack_resolve(win, out, outsz)) return 1;
    return resolve_in(g_root, win, out, outsz);
}
/* Write: ALWAYS the write root if there is one. */
static int resolve_w(const char *win, char *out, size_t outsz){
    return resolve_in(g_wroot[0] ? g_wroot : g_root, win, out, outsz);
}
static int copy_file(const char *a, const char *b){
    FILE *fa = fopen(a, "rb"); if (!fa) return 0;
    mkparents(b);
    FILE *fb = fopen(b, "wb"); if (!fb){ fclose(fa); return 0; }
    char bufr[65536]; size_t k;
    while ((k = fread(bufr, 1, sizeof bufr, fa)) > 0) fwrite(bufr, 1, k, fb);
    fclose(fb); fclose(fa); return 1;
}
/* If a file to be written without truncation exists only in the game root,
 * copy it to the write root first; otherwise the game writes over an empty file. */
static void copy_up(const char *win, const char *wpath){
    if (!g_wroot[0]) return;
    struct stat st; if (stat(wpath, &st) == 0) return;
    char src[1024];
    if (resolve_in(g_root, win, src, sizeof src) && stat(src, &st) == 0 && S_ISREG(st.st_mode))
        copy_file(src, wpath);
}

/* --- write audit and read-only game folder ------------------------------
 * Every write-side operation (write-mode open, delete, mkdir, rmdir,
 * move/copy target, attribute change) is logged as "[write] <op> <guest> -> <host>",
 * so any run shows exactly what the game writes and where.  With
 * fileio_set_readonly_root(1) (the .app, which runs against the user's
 * original install) a write that would land inside the game folder is
 * refused instead, unless it is inside the save overlay. */
static int g_ro_root = 0;
static uint64_t g_wr_ops = 0, g_wr_blocked = 0, g_wr_in_root = 0;
static _Atomic int g_open_wr = 0;          /* write handles still open */
void fileio_set_readonly_root(int on){ g_ro_root = on; }
static int path_under(const char *path, const char *dir){
    size_t l = strlen(dir);
    return l && !strncmp(path, dir, l) && (path[l] == '/' || path[l] == 0);
}

/* --- the game folder's files, opened again and again ----------------------
 * Before each read it queues, the game asks for the file's size the CRT's
 * way (0x57C5B0 -> 0x6F7C54: open, size, close), and its track streamer opens
 * TRACKS\STREAML4RA.BUN again for each section.  A friend's car in free roam
 * is about ten such opens on the game thread (its VINYLS.BIN five times):
 * among a crowd's cars coming and going (tests/scripts/roam_crowd_jumps.txt,
 * 195 cars in 85 s) 1,975 opens on the game thread and 1,304 on the
 * streamer's.  In the browser each step is a round trip to the I/O worker
 * with the guest lock held -- the stat that resolves the path, the open and
 * its fdstat, the size, the close: ~0.7 ms an open, ~7 ms a friend's car on
 * the game thread, 2.6 s of the 85 with the lock held.  The game folder (and
 * a language pack) does not change, so a file of it keeps its path and size
 * here, and a closed handle's descriptor stays open for the next open of the
 * file (GF_FDS at most), its position kept in the handle (ReadFile reads at
 * it).  Any write-side operation (write_allowed) forgets it all, and while a
 * handle is open for writing it is not used: the save overlay may then get a
 * file of such a name.  Web only: a native open takes microseconds. */
#ifdef __wasm__
#define GF_CACHE 1
#endif
#ifdef GF_CACHE
#define GF_N    1024                    /* names (a power of two); 3/4 full: all forgotten */
#define GF_FDS  256                     /* closed handles' descriptors kept open */
static struct { char *win, *real; int64_t size; } g_gf[GF_N];
static int g_gf_n; static unsigned g_gf_gen = 1;
static struct { int fd, name; uint32_t used; } g_gfd[GF_FDS];
static int g_gfd_n; static uint32_t g_gfd_tick;
static uint64_t g_gf_hits, g_gf_fds;
static int gf_find(const char *win){
    uint32_t h = 2166136261u;
    for (const char *s = win; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (uint32_t i = h & (GF_N - 1); g_gf[i].win; i = (i + 1) & (GF_N - 1))
        if (!strcmp(g_gf[i].win, win)) return (int)i;
    return -(int)(h & (GF_N - 1)) - 1;                 /* not there: -(where its probe starts) - 1 */
}
static void gf_forget(void){
    if (!g_gf_n) return;
    for (int i = 0; i < g_gfd_n; i++) close(g_gfd[i].fd);
    g_gfd_n = 0;
    for (int i = 0; i < GF_N; i++){ free(g_gf[i].win); free(g_gf[i].real); g_gf[i].win = g_gf[i].real = 0; }
    g_gf_n = 0; g_gf_gen++;                              /* handles open now give their descriptors back closed */
}
static int gf_add(const char *win, const char *real, int64_t size){
    if (g_gf_n >= GF_N * 3 / 4) gf_forget();
    int i = gf_find(win);
    if (i >= 0) return i;
    i = -i - 1;
    while (g_gf[i].win) i = (i + 1) & (GF_N - 1);
    if (!(g_gf[i].win = strdup(win)) || !(g_gf[i].real = strdup(real))){ free(g_gf[i].win); g_gf[i].win = 0; return -1; }
    g_gf[i].size = size; g_gf_n++;
    return i;
}
/* a kept descriptor of that file (the latest kept), or -1 */
static int gf_fd_take(int name){
    int best = -1;
    for (int i = 0; i < g_gfd_n; i++) if (g_gfd[i].name == name && (best < 0 || g_gfd[i].used > g_gfd[best].used)) best = i;
    if (best < 0) return -1;
    int fd = g_gfd[best].fd;
    g_gfd[best] = g_gfd[--g_gfd_n];
    return fd;
}
static void gf_fd_keep(int name, int fd){
    int k = g_gfd_n;
    if (k == GF_FDS){                                    /* full: the least recently kept is closed */
        k = 0;
        for (int i = 1; i < g_gfd_n; i++) if (g_gfd[i].used < g_gfd[k].used) k = i;
        close(g_gfd[k].fd);
    } else g_gfd_n++;
    g_gfd[k].fd = fd; g_gfd[k].name = name; g_gfd[k].used = ++g_gfd_tick;
}
#else
static void gf_forget(void){}
#endif

static int write_allowed(const char *op, const char *win, const char *host){
    int in_root = path_under(host, g_root) && !(g_wroot[0] && path_under(host, g_wroot));
    int blocked = in_root && g_ro_root;
    g_wr_ops++; g_ov_gen++;                  /* the overlay may change: forget what it lacked */
    gf_forget();                             /* and the game folder's files kept (GF_CACHE) */
    if (in_root) g_wr_in_root++;
    if (blocked) g_wr_blocked++;
    if (g_wr_ops <= 2000)
        fprintf(stderr, "[write] %s \"%s\" -> \"%s\"%s\n", op, win, host,
                blocked ? "  BLOCKED (game folder is read-only)" : in_root ? "  (inside game folder)" : "");
    return !blocked;
}
int fileio_open_writers(void){ return g_open_wr; }

/* --- handle table --- */
#define FH_BASE 0x00900000u
#define FH_MAX  256
typedef struct { int fd; int empty; int used; char path[512]; DIR *dir; char pat[128];
                 DIR *dir2; char path1[512];        /* dir2: game root (while the save overlay is active) */
                 int wr;                            /* wr: opened for writing (g_open_wr) */
                 int gf; unsigned gf_gen; int64_t pos, size; } FH;   /* gf: a game folder file's name + 1 (GF_CACHE) */
static FH g_fh[FH_MAX];
static int g_fh_peak = 0; static uint64_t g_fh_full = 0;   /* v12na: leak check */
static uint32_t fh_new(void){
    for (int i=1;i<FH_MAX;i++) if(!g_fh[i].used){ memset(&g_fh[i],0,sizeof g_fh[i]);
        g_fh[i].used=1; g_fh[i].fd=-1;
        { int n = 0; for (int k = 1; k < FH_MAX; k++) n += g_fh[k].used; if (n > g_fh_peak) g_fh_peak = n; }
        return FH_BASE+(uint32_t)i; }
    if (!g_fh_full++) fprintf(stderr, "  [file] WARNING: handle table full (%d)\n", FH_MAX - 1);
    return 0;
}
static FH *fh_get(uint32_t h){
    if (h<FH_BASE || h>=FH_BASE+FH_MAX) return 0;
    FH *f=&g_fh[h-FH_BASE]; return f->used?f:0;
}
int fileio_is_handle(uint32_t h){ return fh_get(h)!=0; }
int fileio_open_handles(void){ int n = 0; for (int k = 1; k < FH_MAX; k++) n += g_fh[k].used; return n; }
/* Per-thread last error for failing calls (win32.c, TEB+0x34). */
void win32_set_last_error(CPU *, uint32_t);
static void set_err(CPU *c, uint32_t e){ win32_set_last_error(c, e); }
static uint32_t errno_to_win(int e){
    switch (e){ case ENOENT: return 2u; case ENOTDIR: return 3u; case EACCES: case EPERM: case EROFS: return 5u;
                case EEXIST: return 80u; case EMFILE: case ENFILE: return 4u; case ENOSPC: return 112u;
                default: return 31u; }                           /* ERROR_GEN_FAILURE */
}

static uint32_t A(CPU *c,int i){ return MR32(c->esp + 4u*(uint32_t)i); }
#define GS(a) ((const char*)(g_mem + (uint32_t)(a)))
#define INVALID_HANDLE 0xFFFFFFFFu

/* Give the next name of a directory scan: the write root first, then the game root. */
/* Windows wildcard matching (* and ?, case-insensitive; "*.*" matches everything). */
static int wild_match(const char *p, const char *s){
    if (!strcmp(p, "*.*") || !strcmp(p, "*")) return 1;
    for (; *p; p++, s++){
        if (*p == '*'){
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (wild_match(p + 1, s)) return 1;
            return wild_match(p + 1, s);
        }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return *s == 0;
}
/* v12na: the pattern is now applied and the full path is returned too (for
 * attributes/size).  Every entry used to be returned regardless of the
 * pattern and as a "file"; the game never saw its profile folders
 * (FILE_ATTRIBUTE_DIRECTORY). */
static int find_next(FH *f, char *nm, size_t nsz, char *full, size_t fsz){
    struct dirent *e;
    if (f->dir){
        while ((e = readdir(f->dir))){
            if (e->d_name[0] == '.') continue;
            if (!wild_match(f->pat, e->d_name)) continue;
            snprintf(nm, nsz, "%s", e->d_name);
            snprintf(full, fsz, "%s/%s", f->path1, e->d_name); return 1; }
    }
    if (f->dir2){
        while ((e = readdir(f->dir2))){
            if (e->d_name[0] == '.') continue;
            if (!wild_match(f->pat, e->d_name)) continue;
            if (f->path1[0]){               /* already given from the write root */
                char p[1024]; struct stat st;
                snprintf(p, sizeof p, "%s/%s", f->path1, e->d_name);
                if (stat(p, &st) == 0) continue; }
            snprintf(nm, nsz, "%s", e->d_name);
            snprintf(full, fsz, "%s/%s", f->path, e->d_name); return 1; }
    }
    return 0;
}
/* WIN32_FIND_DATAA: +0 attr, +4/+12/+20 FILETIME, +28 sizeHigh, +32 sizeLow, +44 cFileName */
static void find_fill(uint32_t fd_, const char *nm, const char *full){
    if (!fd_) return;
    mem_commit(fd_, 320); memset(g_mem + fd_, 0, 320);
    struct stat st; int ok = stat(full, &st) == 0;
    MR32(fd_) = (ok && S_ISDIR(st.st_mode)) ? 0x10u : 0x80u;
    if (ok){
        uint64_t ft = ((uint64_t)st.st_mtime + 11644473600ull) * 10000000ull;
        for (int k = 0; k < 3; k++){ MR32(fd_ + 4 + 8*k) = (uint32_t)ft; MR32(fd_ + 8 + 8*k) = (uint32_t)(ft >> 32); }
        if (!S_ISDIR(st.st_mode)){ MR32(fd_ + 28) = (uint32_t)((uint64_t)st.st_size >> 32); MR32(fd_ + 32) = (uint32_t)st.st_size; }
    }
    snprintf((char*)(g_mem + fd_ + 44), 260, "%s", nm);
    if (getenv("FILELIST")) fprintf(stderr, "  [file] Find -> \"%s\" attr=0x%x\n", nm, MR32(fd_));
}

/* texrep.m: the file the guest read last (texture categories for the dump) */
static char g_last_read[512];
const char *fileio_last_read(void){ return g_last_read[0] ? g_last_read : 0; }
int fileio_call(CPU *c, const char *n, uint32_t *out){
    uint32_t r = 0;
    if (!strcmp(n,"CreateFileA")){
        uint32_t pn=A(c,0), access=A(c,1), disp=A(c,4);
        char real[1024];
        int flags;
        if (disp==2 /*CREATE_ALWAYS*/ || disp==1 /*CREATE_NEW*/) flags=O_RDWR|O_CREAT|O_TRUNC;
        else if (disp==4 /*OPEN_ALWAYS*/) flags=O_RDWR|O_CREAT;
        else flags = (access & 0x40000000u) ? O_RDWR : O_RDONLY;
#ifdef GF_CACHE
        if (flags == O_RDONLY && !g_open_wr){
            int i = gf_find(GS(pn));
            if (i >= 0){
                int fd = gf_fd_take(i);
                if (fd >= 0) g_gf_fds++;
                else fd = open(g_gf[i].real, O_RDONLY);
                uint32_t h = fd >= 0 ? fh_new() : 0;
                if (h){
                    FH *f = fh_get(h);
                    f->fd = fd; f->gf = i + 1; f->gf_gen = g_gf_gen; f->size = g_gf[i].size;
                    snprintf(f->path, sizeof f->path, "%s", g_gf[i].real);
                    g_file_open++; g_gf_hits++;
                    *out = h; return 1;
                }
                if (fd >= 0) close(fd);
            }
        }
#endif
        int ok, wblock = 0;
        if (flags != O_RDONLY){
            static const char *const dn[] = { "?", "CREATE_NEW", "CREATE_ALWAYS", "OPEN_EXISTING",
                                              "OPEN_ALWAYS", "TRUNCATE_EXISTING" };
            char op[64];
            ok = resolve_w(GS(pn), real, sizeof real);
            snprintf(op, sizeof op, "CreateFileA(%s, 0x%08x)", disp < 6 ? dn[disp] : "?", access);
            wblock = !write_allowed(op, GS(pn), real);
            if (!wblock && !(flags & O_TRUNC)) copy_up(GS(pn), real);
        } else ok = resolve(GS(pn), real, sizeof real);
        if ((flags & O_CREAT) && !wblock) mkparents(real);
        /* Language suffix fallback: this version has the "_fr.vp6" suffix
         * built into the movie path, but the folder holds "_en" files.  If
         * the requested file is missing, try "_en" and the unsuffixed name. */
        /* (resolve found the path, so it exists: no second stat, which is
         * another round trip to the I/O worker in the browser) */
        struct stat _st;
        if (!(flags & O_CREAT) && !ok){
            char alt[1024]; const char *dot = strrchr(real, '.');
            if (dot && dot - real > 3){
                size_t base = (size_t)(dot - real);
                const char *sfx[] = { "_en", "", "_dn", "_du", "_sw" };
                for (int q = 0; q < 5; q++){
                    /* remove the current language suffix (if any) */
                    size_t cut = base;
                    if (base >= 3 && real[base-3] == '_') cut = base - 3;
                    snprintf(alt, sizeof alt, "%.*s%s%s",
                             (int)cut, real, sfx[q], dot);
                    if (stat(alt, &_st) == 0){
                        if (getenv("VERBOSE") || getenv("FILELIST"))
                            fprintf(stderr,"  [file] language fallback: \"%s\" -> \"%s\"\n",
                                    real, alt);
                        snprintf(real, sizeof real, "%s", alt);
                        break;
                    }
                }
            }
        }
        /* NOMOVIES=1: ignore video files.  There is no VP6 decoder; if the
         * game tries to play the video and waits, it hangs there. */
        int fd;
        { const char *ext = strrchr(real, '.');
          const char *nm2 = getenv("NOMOVIES");
          int movie_mode = (nm2 && *nm2 && *nm2 != '0') ? atoi(nm2) : 0;
          /* mode 3: skip only the "blank" movie (real movies still play) */
          const char *bn2 = strrchr(real, '/'); bn2 = bn2 ? bn2+1 : real;
          if (wblock) fd = -1;
          else if (movie_mode == 3 && !strncasecmp(bn2, "blank", 5)) fd = -1;
          else if (movie_mode == 1 && ext &&
              (!strcasecmp(ext,".vp6") || !strcasecmp(ext,".vp8") ||
               !strcasecmp(ext,".bik") || !strcasecmp(ext,".mpg")))
              fd = -1;
          else if (movie_mode == 3 && !strncasecmp(bn2, "blank", 5)) { }
          else {
              fd = open(real, flags, 0644);
              /* mode 2: open it but make it look empty -> the player reports "done" */
              if (fd >= 0 && movie_mode == 2 && ext &&
                  (!strcasecmp(ext,".vp6") || !strcasecmp(ext,".vp8") ||
                   !strcasecmp(ext,".bik") || !strcasecmp(ext,".mpg")))
                  g_empty_next = 1;
          } }
        /* v1.2 NA (HOODLUM) disc check: if the game folder has an empty
         * "foobar" file, the Disc 2 prompt is skipped (0x57CAC0("foobar")).
         * To leave the data folder alone the file is virtual: /dev/null (empty, EOF). */
        if (fd < 0 && flags == O_RDONLY){
            const char *bn3 = strrchr(real, '/'); bn3 = bn3 ? bn3+1 : real;
            if (!strcasecmp(bn3, "foobar")){
                fd = open("/dev/null", O_RDONLY);
                static int said = 0;
                if (!said++) fprintf(stderr, "  [file] virtual empty \"foobar\" (disk check skipped)\n");
            } }
        { static int shown=0; if ((getenv("VERBOSE")||getenv("FILELIST")) && (shown<200 || flags != O_RDONLY)){ shown++;
            fprintf(stderr,"  [file] CreateFileA(\"%s\") -> \"%s\"  %s\n",
                    GS(pn), real, fd>=0?"OPEN":"FAILED"); } }
        if (fd<0){ g_file_fail++; r=INVALID_HANDLE; set_err(c, wblock ? 5u : errno_to_win(errno));
            fprintf(stderr,"  [file] CANNOT OPEN: \"%s\" -> \"%s\" (disp=%u access=0x%x)\n",
                    GS(pn), real, disp, access); }
        else {
            uint32_t h=fh_new();
            if(!h){ close(fd); r=INVALID_HANDLE; }
            else { FH*f=fh_get(h); f->fd=fd; f->empty = g_empty_next; g_empty_next = 0;
                   snprintf(f->path,sizeof f->path,"%s",real);
                   if (flags != O_RDONLY){ f->wr = 1; g_open_wr++; }
#ifdef GF_CACHE
                   /* a file of the game folder or the language pack, not a movie (NOMOVIES may empty those) */
                   const char *ext = strrchr(real, '.');
                   struct stat st;
                   if (flags == O_RDONLY && ok && !f->empty && !g_open_wr &&
                       ((path_under(real, g_root) && !(g_wroot[0] && path_under(real, g_wroot))) ||
                        (g_lroot[0] && path_under(real, g_lroot))) &&
                       !(ext && (!strcasecmp(ext,".vp6") || !strcasecmp(ext,".vp8") || !strcasecmp(ext,".bik") ||
                                 !strcasecmp(ext,".mpg") || !strcasecmp(ext,".ivf") || !strcasecmp(ext,".webm"))) &&
                       fstat(fd, &st) == 0 && S_ISREG(st.st_mode)){
                       int i = gf_add(GS(pn), real, (int64_t)st.st_size);
                       if (i >= 0){ f->gf = i + 1; f->gf_gen = g_gf_gen; f->size = (int64_t)st.st_size; }
                   }
#endif
                   g_file_open++; r=h; }
        }
    }
    else if (!strcmp(n,"ReadFile")){
#ifdef __wasm__
        /* In the browser a read of the install folder can wait for the network
         * (a cache miss of the page's I/O worker, web/src/host/io-worker.ts).
         * The guest lock is released meanwhile, as Windows lets the other
         * threads run during a blocking ReadFile: the loader held it for up
         * to 280 ms per read and stalled the game thread and the DirectSound
         * feeder.  The FH table is static, so f stays valid. */
        void guest_wait_begin(void); void guest_wait_end(void);
#define READ_UNLOCKED(stmt) do { guest_wait_begin(); stmt; guest_wait_end(); } while (0)
#else
#define READ_UNLOCKED(stmt) do { stmt; } while (0)
#endif
        /* (hFile, lpBuffer, nBytes, lpNumberRead, lpOverlapped)
         * With an OVERLAPPED: the offset is taken from the struct, the result
         * is written to it and hEvent is signalled.  Without that the game
         * waits forever for the asynchronous read to finish.              */
        FH*f=fh_get(A(c,0)); uint32_t buf=A(c,1), len=A(c,2), pnr=A(c,3), ov=A(c,4);
        if(!f||f->fd<0){ if(pnr) MR32(pnr)=0; r=0; }
        else { if(len) mem_commit(buf,len);
               ssize_t got;
               uint64_t _rd0 = now_ns_io();
               if (ov && mem_is_committed(ov+19)){
                   uint64_t off = (uint64_t)MR32(ov+8) | ((uint64_t)MR32(ov+12) << 32);
                   { int fd = f->fd; READ_UNLOCKED(got = pread(fd, g_mem+buf, len, (off_t)off)); }
                   if(got<0) got=0;
                   MR32(ov+0) = 0;                      /* Internal = STATUS_SUCCESS */
                   MR32(ov+4) = (uint32_t)got;          /* InternalHigh = bytes read */
                   uint32_t hev = MR32(ov+16);
                   if (hev){ void win32_signal_event(uint32_t); win32_signal_event(hev); }
                   { const char *mf2 = getenv("MUTEFILE");
                     const char *ms2 = getenv("MUTESIZE");
                     const char *mn2 = getenv("MUTEMIN");
                     long msz2 = ms2 ? atol(ms2) : 0, mmin2 = mn2 ? atol(mn2) : 0;
                     if (mf2 && *mf2 && got > 0 && strstr(f->path, mf2)
                         && (mmin2 ? ((long)len >= mmin2) : (!msz2 || (long)len == msz2)))
                         memset(g_mem+buf, 0, (size_t)got); }
                   g_overlapped_reads++;
               } else if (f->empty){
                   got = 0;                       /* immediate end of file */
               } else if (f->gf){                 /* a kept descriptor: the position is the handle's */
                   { int fd = f->fd; off_t at = (off_t)f->pos; READ_UNLOCKED(got = pread(fd, g_mem+buf, len, at)); }
                   if(got<0) got=0;
                   f->pos += got;
               } else {
                   { int fd = f->fd; READ_UNLOCKED(got = read(fd, g_mem+buf, len)); }
                   if(got<0) got=0;
               }
               /* MUTEFILE=<part>: fill the reads of the matching file with
                * ZEROS.  To measure objectively whether sound effects remain
                * once the music stream (sdat.viv) is silenced.  We do not
                * return end of file, so the streaming logic keeps running
                * normally -- only the content goes silent. */
               { const char *mf = getenv("MUTEFILE");
                 const char *ms = getenv("MUTESIZE");
                 const char *mn = getenv("MUTEMIN");
                 long msz = ms ? atol(ms) : 0, mmin = mn ? atol(mn) : 0;
                 if (mf && *mf && got > 0 && strstr(f->path, mf)
                     && (mmin ? ((long)len >= mmin) : (!msz || (long)len == msz)))
                     memset(g_mem+buf, 0, (size_t)got); }
               if(pnr) MR32(pnr)=(uint32_t)got;
               if (getenv("READLOG")){
                   const char *bn = strrchr(f->path,'/'); bn = bn?bn+1:f->path;
                   double audio_time(void);
                   fprintf(stderr,"  [read] t=%8.3f %-28s wanted=%-9u got=%-9ld time=%.2f ms\n",
                           audio_time(), bn, len, (long)got,
                           (double)(now_ns_io() - _rd0)/1e6);
               }
               if (got > 0) snprintf(g_last_read, sizeof g_last_read, "%s", f->path);
               g_file_read++; g_bytes_read += (uint64_t)got; r=1; }
    }
    else if (!strcmp(n,"GetOverlappedResult")){
        /* (hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait) */
        uint32_t ov=A(c,1), pn=A(c,2);
        if (ov && pn && mem_is_committed(ov+7)) MR32(pn) = MR32(ov+4);
        r=1; }
    else if (!strcmp(n,"HasOverlappedIoCompleted")){ r=1; }
    else if (!strcmp(n,"WriteFile")){
        FH*f=fh_get(A(c,0)); uint32_t buf=A(c,1), len=A(c,2), pnw=A(c,3);
        if(!f||f->fd<0){
            uint32_t h=A(c,0);
            if(h>=0x10u && h<=0x13u && len){ mem_commit(buf,len);
                /* Whole lines, on stderr: in order with the runtime's own
                 * log (stdout is block-buffered in a log file), one prefix per
                 * line however the CRT splits its writes (server.dll's
                 * LOGFILE is "$stderr"). */
                static char lb[512]; static size_t ln = 0;
                for (uint32_t i = 0; i < len; i++){
                    char ch = (char)MR8(buf + i);
                    if (ch != '\n' && ch != '\r' && ln < sizeof lb - 1){ lb[ln++] = ch; continue; }
                    if (ch == '\r') continue;
                    if (ln){ fprintf(stderr, "[guest] %.*s\n", (int)ln, lb); ln = 0; }
                } }
            if(pnw) MR32(pnw)=len; r=1; }
        else { ssize_t w=write(f->fd, g_mem+buf, len); if(w<0)w=0;
               if(pnw) MR32(pnw)=(uint32_t)w; r=1; }
    }
    else if (!strcmp(n,"SetFilePointer")){
        FH*f=fh_get(A(c,0)); int32_t dist=(int32_t)A(c,1); uint32_t method=A(c,3);
        if(!f||f->fd<0) r=0xFFFFFFFFu;
        else if (f->gf){
            int64_t p = (method==1 ? f->pos : method==2 ? f->size : 0) + dist;
            if (p < 0) r=0xFFFFFFFFu; else { f->pos = p; r=(uint32_t)p; }
        }
        else { int wh = method==1?SEEK_CUR : (method==2?SEEK_END:SEEK_SET);
               off_t p = lseek(f->fd, dist, wh);
               r = (p<0)?0xFFFFFFFFu:(uint32_t)p; }
    }
    else if (!strcmp(n,"GetFileSize")){
        FH*f=fh_get(A(c,0)); uint32_t ph=A(c,1);
        if(!f||f->fd<0) r=0xFFFFFFFFu;
        else if (f->gf){ r=(uint32_t)f->size; if(ph) MR32(ph)=(uint32_t)((uint64_t)f->size>>32); }
        else { struct stat st; if(fstat(f->fd,&st)==0){ r=(uint32_t)st.st_size;
                 if(ph) MR32(ph)=(uint32_t)(((uint64_t)st.st_size)>>32); } else r=0xFFFFFFFFu; }
    }
    else if (!strcmp(n,"GetFileTime")){         /* (hFile, lpCreation, lpLastAccess, lpLastWrite) */
        /* server.dll stamps its version string with SPEED2.EXE's write time */
        FH*f=fh_get(A(c,0)); struct stat st;
        if(!f||f->fd<0||fstat(f->fd,&st)!=0) r=0;
        else {
#ifdef __wasm__
            /* WASI has no birth time: the status-change time stands in */
            const struct timespec *ts[3] = { &st.st_ctim, &st.st_atim, &st.st_mtim };
#else
            const struct timespec *ts[3] = { &st.st_birthtimespec, &st.st_atimespec, &st.st_mtimespec };
#endif
            for (int k = 0; k < 3; k++){ uint32_t p=A(c,1+k); if(!p) continue;
                uint64_t ft = ((uint64_t)ts[k]->tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts[k]->tv_nsec / 100u;
                mem_commit(p,8); MR32(p)=(uint32_t)ft; MR32(p+4)=(uint32_t)(ft>>32); }
            r=1; }
    }
    else if (!strcmp(n,"SetEndOfFile")){
        FH*f=fh_get(A(c,0));
        if(f&&f->fd>=0){ off_t p=lseek(f->fd,0,SEEK_CUR); r=(ftruncate(f->fd,p)==0); } else r=0;
    }
    else if (!strcmp(n,"FlushFileBuffers")){ FH*f=fh_get(A(c,0)); if(f&&f->fd>=0) fsync(f->fd); r=1; }
    else if (!strcmp(n,"CloseHandle")){
        FH*f=fh_get(A(c,0));
        if(!f) return 0;                       /* not ours - let the other layer handle it */
#ifdef GF_CACHE
        if(f->fd>=0 && f->gf && f->gf_gen == g_gf_gen && !g_open_wr) gf_fd_keep(f->gf - 1, f->fd);
        else
#endif
        if(f->fd>=0) close(f->fd);
        if(f->dir) closedir(f->dir);
        if(f->wr) g_open_wr--;
        f->used=0; r=1;
    }
    else if (!strcmp(n,"GetFileAttributesA")){
        char real[1024]; resolve(GS(A(c,0)), real, sizeof real);
        struct stat st;
        if(stat(real,&st)!=0){ r=0xFFFFFFFFu; set_err(c, errno_to_win(errno)); }
        else r = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;   /* DIRECTORY / NORMAL */
        if (getenv("FILELIST")) fprintf(stderr, "  [file] GetFileAttributesA(\"%s\") -> 0x%x\n", GS(A(c,0)), r);
    }
    else if (!strcmp(n,"SetFileAttributesA")){
        /* Accepted and ignored: attributes never reach the host file. */
        fprintf(stderr, "[write] SetFileAttributesA \"%s\" (ignored, nothing written)\n", GS(A(c,0)));
        r=1; }
    else if (!strcmp(n,"DeleteFileA")){
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        if (!write_allowed("DeleteFileA", GS(A(c,0)), real)){ r = 0; set_err(c, 5u); }   /* ERROR_ACCESS_DENIED */
        else { r = (unlink(real)==0); if (!r) set_err(c, errno_to_win(errno)); }
    }
    else if (!strcmp(n,"GetFullPathNameA")){
        /* (lpFileName, nBufferLength, lpBuffer, lpFilePart)  v12na: was a stub
         * returning 0 with the buffer untouched -> the CRT (_fullpath,
         * 0x75FF1E) handed garbage to the profile code, which then
         * enumerated a garbage path (0xFF bytes, then a "*" pattern) and never
         * saved/found profiles.
         * Current directory is the virtual "C:\NFSU2" (GetCurrentDirectoryA). */
        const char *src = GS(A(c,0)); uint32_t nb = A(c,1), buf = A(c,2), pfp = A(c,3);
        char full[1024];
        int abs_ = (src[0] && src[1] == ':') || src[0] == '\\' || src[0] == '/';
        if (abs_ && src[0] != '\\' && src[0] != '/') snprintf(full, sizeof full, "%s", src);
        else if (abs_) snprintf(full, sizeof full, "C:%s", src);
        else snprintf(full, sizeof full, "C:\\NFSU2\\%s", src);
        for (char *q = full; *q; q++) if (*q == '/') *q = '\\';
        uint32_t len = (uint32_t)strlen(full);
        if (!buf || nb < len + 1) r = len + 1;
        else {
            mem_commit(buf, len + 1); memcpy(g_mem + buf, full, len + 1);
            if (pfp){ const char *ls = strrchr(full, '\\');
                      mem_commit(pfp, 4);
                      MR32(pfp) = (ls && ls[1]) ? buf + (uint32_t)(ls + 1 - full) : 0u; }
            r = len;
        }
        if (getenv("FILELIST")) fprintf(stderr, "  [file] GetFullPathNameA(\"%s\") -> \"%s\"\n", src, full);
    }
    else if (!strcmp(n,"SHGetFolderPathA")){
        /* (hwnd, csidl, hToken, dwFlags, pszPath) -> S_OK
         * We return the virtual root; resolve() maps it to the game folder.
         * If it comes back empty the game cannot find its save/profile folder. */
        uint32_t pp=A(c,4);
        if (pp){ mem_commit(pp, 260); snprintf((char*)(g_mem+pp), 260, "C:\\NFSU2"); }
        r=0; }
    else if (!strcmp(n,"CreateDirectoryA")){
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        struct stat st;
        if (stat(real, &st) == 0 && S_ISDIR(st.st_mode)) r = 1;      /* nothing to write */
        else if (!write_allowed("CreateDirectoryA", GS(A(c,0)), real)) r = 0;
        else { mkparents(real); r = (mkdir(real,0755)==0 || errno==EEXIST); }
        if (getenv("FILELIST")) fprintf(stderr, "  [file] CreateDirectoryA(\"%s\") -> %u\n", GS(A(c,0)), r);
    }
    else if (!strcmp(n,"RemoveDirectoryA")){
        /* Profile Manager > Delete: DeleteFileA of the save, then this on
         * its folder.  Left unimplemented, the empty folder stayed and the
         * profile came back on the next launch ("Unable to load <NAME>").
         * Only an overlay folder can go (no whiteouts); never a root. */
        char real[1024]; resolve_w(GS(A(c,0)), real, sizeof real);
        if (!strcmp(real, g_wroot[0] ? g_wroot : g_root)){ r = 0; set_err(c, 5u); }
        else if (!write_allowed("RemoveDirectoryA", GS(A(c,0)), real)){ r = 0; set_err(c, 5u); }
        else { r = (rmdir(real)==0);
               if (!r) set_err(c, errno == ENOTEMPTY ? 145u : errno_to_win(errno)); }   /* ERROR_DIR_NOT_EMPTY */
        if (getenv("FILELIST")) fprintf(stderr, "  [file] RemoveDirectoryA(\"%s\") -> %u\n", GS(A(c,0)), r);
    }
    else if (!strcmp(n,"MoveFileA")){
        char a[1024],b[1024]; resolve(GS(A(c,0)),a,sizeof a); resolve_w(GS(A(c,1)),b,sizeof b);
        if (!write_allowed("MoveFileA (target)", GS(A(c,1)), b)) r = 0;
        else {
            mkparents(b);
            /* If the source is in the game root (read-only), copy instead of moving. */
            if ((g_wroot[0] && !path_under(a, g_wroot)) || (g_ro_root && path_under(a, g_root)))
                r = (uint32_t)copy_file(a, b);
            else r = write_allowed("MoveFileA (source)", GS(A(c,0)), a) && rename(a,b)==0;
        }
    }
    else if (!strcmp(n,"CopyFileA")){
        char a[1024],b[1024]; resolve(GS(A(c,0)),a,sizeof a); resolve_w(GS(A(c,1)),b,sizeof b);
        r = write_allowed("CopyFileA (target)", GS(A(c,1)), b) ? (uint32_t)copy_file(a, b) : 0;
    }
    /* --- directory scan: WIN32_FIND_DATAA (320 bytes), +0 attr, +44 name --- */
    else if (!strcmp(n,"FindFirstFileA")){
        const char *spec = GS(A(c,0));
        char win[1024]; snprintf(win,sizeof win,"%s",spec);
        char *bs = strrchr(win,'\\'); char *fs2 = strrchr(win,'/');
        char *sep = bs>fs2?bs:fs2;
        char pat[128]="*";
        if (sep){ snprintf(pat,sizeof pat,"%s",sep+1); *sep=0; } else { snprintf(pat,sizeof pat,"%s",win); win[0]=0; }
        /* With the save overlay active, the folder in the write root is
         * scanned first, then the one in the game root (no name is given twice). */
        char real[1024], wreal[1024]; wreal[0] = 0;
        if (win[0]) resolve_in(g_root, win, real, sizeof real); else snprintf(real,sizeof real,"%s",g_root);
        if (g_wroot[0]){
            if (win[0]) resolve_in(g_wroot, win, wreal, sizeof wreal);
            else snprintf(wreal, sizeof wreal, "%s", g_wroot); }
        DIR *d1 = wreal[0] ? opendir(wreal) : 0;
        DIR *d2 = opendir(real);
        if(!d1 && !d2){ r=INVALID_HANDLE; set_err(c, 3u); }   /* ERROR_PATH_NOT_FOUND */
        else { uint32_t h=fh_new(); FH*f=fh_get(h);
               if(!f){ if(d1) closedir(d1); if(d2) closedir(d2); r=INVALID_HANDLE; }
               else { f->dir=d1; f->dir2=d2; snprintf(f->pat,sizeof f->pat,"%s",pat);
                      snprintf(f->path1,sizeof f->path1,"%s",wreal);
                      snprintf(f->path,sizeof f->path,"%s",real);
                      char nm[260], full[1300];
                      if (!find_next(f, nm, sizeof nm, full, sizeof full)){
                          if(f->dir) closedir(f->dir); if(f->dir2) closedir(f->dir2);
                          f->used=0; r=INVALID_HANDLE; set_err(c, 2u); }   /* ERROR_FILE_NOT_FOUND */
                      else { find_fill(A(c,1), nm, full); r=h; } } }
        if (getenv("FILELIST")) fprintf(stderr, "  [file] FindFirstFileA(\"%s\") -> %s\n", spec,
                                        r == INVALID_HANDLE ? "NONE" : "found");
    }
    else if (!strcmp(n,"FindNextFileA")){
        FH*f=fh_get(A(c,0)); r=0; char nm[260], full[1300];
        if(f && (f->dir || f->dir2) && find_next(f, nm, sizeof nm, full, sizeof full)){
            find_fill(A(c,1), nm, full); r=1; }
        else set_err(c, 18u);                                    /* ERROR_NO_MORE_FILES */
    }
    else if (!strcmp(n,"FindClose")){
        FH*f=fh_get(A(c,0));
        if(f){ if(f->dir) closedir(f->dir); if(f->dir2) closedir(f->dir2); f->used=0; } r=1;
    }
    else return 0;
    *out = r;
    return 1;
}

void fileio_report(void){
    fprintf(stderr,"  file: %llu write operations, %llu inside the game folder, %llu blocked (read-only)%s\n",
            (unsigned long long)g_wr_ops, (unsigned long long)g_wr_in_root,
            (unsigned long long)g_wr_blocked, g_ro_root ? "" : "  [game folder writable]");
    if (g_overlapped_reads)
        fprintf(stderr,"  file: %llu asynchronous (overlapped) reads\n",
                (unsigned long long)g_overlapped_reads);
    fprintf(stderr,"  file: %llu opened, %llu could not be opened, %llu reads, %.1f MB; %llu save overlay lookups skipped\n",
            (unsigned long long)g_file_open,(unsigned long long)g_file_fail,
            (unsigned long long)g_file_read,(double)g_bytes_read/1048576.0, (unsigned long long)g_ovc_skips);
#ifdef GF_CACHE
    fprintf(stderr,"  file: %llu opens of the game folder's files known already, %llu of them on a kept descriptor (%d names, %d kept)\n",
            (unsigned long long)g_gf_hits, (unsigned long long)g_gf_fds, g_gf_n, g_gfd_n);
#endif
    { int n = 0; for (int k = 1; k < FH_MAX; k++) n += g_fh[k].used;
      fprintf(stderr,"  file: %d handles open now, at most %d (limit %d), table full %llu times\n",
              n, g_fh_peak, FH_MAX - 1, (unsigned long long)g_fh_full); }
}
