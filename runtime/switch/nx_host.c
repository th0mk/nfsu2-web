/* nx_host.c -- libnx services the shared runtime reaches through runtime/switch/switch_plat.h, and the symbols the
 * browser build gets from the page (runtime/web/host_abi.h).  Compiled with libnx's headers; the runtime files are
 * compiled without them (their type names clash). */
#include <switch.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* the window the OpenGL renderer draws into */
void *nfsu2_native_window(void){
    NWindow *w = nwindowGetDefault();
    nwindowSetDimensions(w, 1280, 720);        /* the default window is 0x0 until told (the first GL run drew into nothing) */
    return w;
}

double nfsu2_now_ms(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ms the calling thread spent blocked in the host (reads from the SD card, waits on addresses): win32_ext.c subtracts it
 * from the time a thread held the guest lock, to tell the game's own work from waiting */
static __thread double tl_blocked_ms;
double nfsu2_blocked_ms(void){ return tl_blocked_ms; }

int nfsu2_wait32(int *p, int expected, long long timeout_ns){
    double t0 = nfsu2_now_ms();
    int r = (int)svcWaitForAddress(p, ArbitrationType_WaitIfEqual, expected, timeout_ns);
    tl_blocked_ms += nfsu2_now_ms() - t0;
    return r;
}
int nfsu2_notify(int *p, unsigned count){
    return (int)svcSignalToAddress(p, SignalType_Signal, 0, (int)count);
}

/* ---- cores ----
 * The game's main thread runs flat out (one core at 100%), and with the sound threads (audout refills a buffer every 21 ms
 * from a thread that runs ahead of it) and the renderer's, the scheduler was free to put any of them on its core.  The race ran
 * at 28 fps with sound against 40-49 without it.  So: the game's main thread has core 2 to itself (mask 0x4), the renderer
 * core 1, the sound threads core 0, and every other thread of the process (the game's own helpers, timers, loaders) 0 and 1
 * (mask 0x3).  An empty file /switch/nfsu2/nopin leaves the scheduler alone. */
extern int __real_pthread_create(pthread_t *th, const pthread_attr_t *at, void *(*fn)(void *), void *arg);
static int pin_enabled(void){
    static int v = -1;
    if (v < 0){ struct stat st; v = stat("sdmc:/switch/nfsu2/nopin", &st) != 0; }
    return v;
}
static unsigned g_pin_rc[8]; static unsigned g_pin_n;
void nfsu2_pin(unsigned mask){
    if (!pin_enabled()) return;
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, -2, mask);
    if (g_pin_n < 8){ g_pin_rc[g_pin_n++] = (mask << 24) | (rc & 0xFFFFFF); }
    static int logged[16]; if (mask < 16 && !logged[mask]){ logged[mask] = 1; fprintf(stderr, "[nx] core mask 0x%x: result 0x%x\n", mask, (unsigned)rc); }
}
typedef struct { void *(*fn)(void *); void *arg; } Tramp;
static void *tramp_main(void *p){
    Tramp t = *(Tramp *)p; free(p);
    nfsu2_pin(0x3);
    return t.fn(t.arg);
}
int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *at, void *(*fn)(void *), void *arg){
    Tramp *t = malloc(sizeof *t);
    if (!t) return __real_pthread_create(th, at, fn, arg);
    t->fn = fn; t->arg = arg;
    int rc = __real_pthread_create(th, at, tramp_main, t);
    if (rc != 0) free(t);
    return rc;
}

/* ---- host_abi.h: the page's imports ---- */
#define KINDS 16
static void *g_kind_ptr[KINDS]; static uint32_t g_kind_size[KINDS];
int nfsu2_register(int kind, void *ptr, uint32_t size){
    if (kind < 0 || kind >= KINDS) return -1;
    g_kind_ptr[kind] = ptr; g_kind_size[kind] = size;       /* the native host reads its mailboxes from here */
    return 0;
}
void *nfsu2_host_mailbox(int kind, uint32_t *size){
    if (kind < 0 || kind >= KINDS) return 0;
    if (size) *size = g_kind_size[kind];
    return g_kind_ptr[kind];
}
int nfsu2_stack_frames(void){ return 0; }
/* no video decoder yet: the game plays without its movies */
int nfsu2_movie_open(int w, int h){ (void)w; (void)h; return -1; }
int nfsu2_movie_decode(int hd, uint32_t p, uint32_t l, uint32_t y, uint32_t u, uint32_t v, int ys, int uvs){
    (void)hd; (void)p; (void)l; (void)y; (void)u; (void)v; (void)ys; (void)uvs; return -1; }
int nfsu2_movie_close(int handle){ (void)handle; return 0; }

/* ---- what newlib/libnx lack ---- */
int getrusage(int who, struct rusage *ru){
    (void)who; memset(ru, 0, sizeof *ru);
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);       /* wall time stands in for CPU time (diagnostics only) */
    ru->ru_utime.tv_sec = t.tv_sec; ru->ru_utime.tv_usec = t.tv_nsec / 1000;
    return 0;
}
int _getentropy_r(struct _reent *r, void *buf, size_t n){
    (void)r; randomGet(buf, n); return 0;
}
int pause(void){ for (;;) svcSleepThread(1000000000ll); return -1; }

/* ---- files ----
 * Read-only opens of files on the SD card (all of the game's data) do not go through libnx's descriptor layer: they open
 * the file with the file system service (fsFsOpenFile) and get a descriptor number of this file's own (SFD_BASE and up),
 * and read, seek, stat and pread on them are fsFileRead at an explicit offset.  libnx's own layer died twice in
 * _lseek_r with a null device on a descriptor it had handed out (devoptab_list[handle->device] == NULL, after 3 and 14
 * minutes, several threads opening and closing kept files).  Anything else (writes, other paths) stays on libnx's. */
extern int __real_open(const char *path, int flags, ...);
extern int __real_close(int fd);
extern ssize_t __real_read(int fd, void *buf, size_t n);
extern off_t __real_lseek(int fd, off_t off, int whence);
extern int __real_fstat(int fd, struct stat *st);
#define SFD_BASE 2000
#define SFD_MAX  1024
static struct { int used; FsFile f; s64 size, pos, base; int shared, st; } g_sf[SFD_MAX];      /* base: where the file starts (in nfsu2.dat; 0 for a file of its own); shared: the handle is the .dat's */
int vfs_dat_open(const char *path, FsFile *f, s64 *base, s64 *size);
int vfs_dat_use_storage(void);          /* the .dat is inside the NSP's RomFS: read it with fsStorageRead (vfs_dat.c) */
FsStorage *vfs_dat_storage(void);
static pthread_mutex_t g_sf_lock = PTHREAD_MUTEX_INITIALIZER;
static int sfd_ok(int fd){ return fd >= SFD_BASE && fd < SFD_BASE + SFD_MAX && g_sf[fd - SFD_BASE].used; }
static unsigned g_open_fail_logged;

#define NULLFILE "sdmc:/switch/nfsu2/null.bin"
int __wrap_open(const char *path, int flags, ...){
    int mode = 0;
    if (flags & O_CREAT){ va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    /* the game opens /dev/null for files it does not find: an empty file on the SD card stands in */
    if (!strncmp(path, "/dev/null", 9)){
        FILE *z = fopen(NULLFILE, "ab"); if (z) fclose(z);
        path = NULLFILE; flags = O_RDONLY; mode = 0;
    }
    const char *p = !strncmp(path, "sdmc:", 5) ? path + 5 : path;
    /* the game folder inside nfsu2.dat, when it is mounted */
    if ((flags & O_ACCMODE) == O_RDONLY && !(flags & (O_CREAT | O_TRUNC | O_APPEND))){
        FsFile df; s64 base = 0, vsize = 0;
        int vr = vfs_dat_open(path, &df, &base, &vsize);
        if (vr == 0){ errno = ENOENT; return -1; }
        if (vr == 1){
            pthread_mutex_lock(&g_sf_lock);
            for (int i = 0; i < SFD_MAX; i++) if (!g_sf[i].used){
                g_sf[i].used = 1; g_sf[i].f = df; g_sf[i].size = vsize; g_sf[i].pos = 0; g_sf[i].base = base; g_sf[i].shared = 1; g_sf[i].st = vfs_dat_use_storage();
                pthread_mutex_unlock(&g_sf_lock);
                return SFD_BASE + i;
            }
            pthread_mutex_unlock(&g_sf_lock);
            errno = EMFILE; return -1;
        }
    }
    if ((flags & O_ACCMODE) == O_RDONLY && !(flags & (O_CREAT | O_TRUNC | O_APPEND)) && p[0] == '/'){
        FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
        FsFile ff; s64 size = 0;
        if (fs){
            if (R_FAILED(fsFsOpenFile(fs, p, FsOpenMode_Read, &ff))){ errno = ENOENT; return -1; }
            fsFileGetSize(&ff, &size);
            pthread_mutex_lock(&g_sf_lock);
            for (int i = 0; i < SFD_MAX; i++) if (!g_sf[i].used){
                g_sf[i].used = 1; g_sf[i].f = ff; g_sf[i].size = size; g_sf[i].pos = 0; g_sf[i].base = 0; g_sf[i].shared = 0; g_sf[i].st = 0;
                pthread_mutex_unlock(&g_sf_lock);
                return SFD_BASE + i;
            }
            pthread_mutex_unlock(&g_sf_lock);
            fsFileClose(&ff);
            if (g_open_fail_logged++ < 5) fprintf(stderr, "[nx] too many open files (%d)\n", SFD_MAX);
            errno = EMFILE; return -1;
        }
    }
    return __real_open(path, flags, mode);
}
int __wrap_close(int fd){
    if (fd >= SFD_BASE){
        pthread_mutex_lock(&g_sf_lock);
        int ok = sfd_ok(fd);
        if (ok){ if (!g_sf[fd - SFD_BASE].shared) fsFileClose(&g_sf[fd - SFD_BASE].f); g_sf[fd - SFD_BASE].used = 0; }
        pthread_mutex_unlock(&g_sf_lock);
        if (!ok){ errno = EBADF; return -1; }
        return 0;
    }
    return __real_close(fd);
}
static unsigned g_slow_reads;
unsigned nfsu2_status_flow(void);
static struct { unsigned long n; double bytes, ms, max_ms; } g_rd[8];            /* by the game's flow: 3 front end, 4/5 loading, 6 race */
static pthread_mutex_t g_rd_lock = PTHREAD_MUTEX_INITIALIZER;
/* the read at an explicit offset: every read of a game file ends up here */
/* The destination of an fsFileRead is a "map alias" buffer in the service's request: the kernel takes those pages away from
 * this process until the reply, so another thread touching the guest memory there (a loader polling its buffer, the debug
 * hook, the sound thread) takes a data abort.  The reads therefore go through a buffer of our own (1 MB at a time) and are
 * copied into the guest's.  The first race on the console died a few ms after a 7.9 MB read. */
#define BOUNCE (1u << 20)
static unsigned char *g_bounce;
static ssize_t sf_read_at(int fd, void *buf, size_t n, s64 off){
    double t0 = nfsu2_now_ms();
    ssize_t r = -1;
    pthread_mutex_lock(&g_sf_lock);
    if (!sfd_ok(fd)) errno = EBADF;
    else {
        int i = fd - SFD_BASE; u64 got = 0;
        if (off >= g_sf[i].size) r = 0;
        else {
            if (!g_bounce) g_bounce = memalign(0x1000, BOUNCE);
            if (off + (s64)n > g_sf[i].size) n = (size_t)(g_sf[i].size - off);     /* a file inside the .dat ends where its bytes do */
            size_t done = 0; r = 0;
            while (done < n && g_bounce){
                size_t want = n - done < BOUNCE ? n - done : BOUNCE;
                got = 0;
                if (g_sf[i].st){ if (R_FAILED(fsStorageRead(vfs_dat_storage(), g_sf[i].base + off + (s64)done, g_bounce, want))){ if (!done) r = -1; errno = EIO; break; } got = want; }
                else if (R_FAILED(fsFileRead(&g_sf[i].f, g_sf[i].base + off + (s64)done, g_bounce, want, FsReadOption_None, &got))){ if (!done) r = -1; errno = EIO; break; }
                memcpy((unsigned char *)buf + done, g_bounce, got);
                done += got; r = (ssize_t)done;
                if (got < want) break;
            }
            if (!g_bounce){ r = -1; errno = ENOMEM; }
        }
    }
    pthread_mutex_unlock(&g_sf_lock);
    double dt = nfsu2_now_ms() - t0;
    tl_blocked_ms += dt;
    { unsigned fl = nfsu2_status_flow(); if (fl > 7) fl = 7;
      pthread_mutex_lock(&g_rd_lock); g_rd[fl].n++; g_rd[fl].bytes += r > 0 ? (double)r : 0.0; g_rd[fl].ms += dt; if (dt > g_rd[fl].max_ms) g_rd[fl].max_ms = dt; pthread_mutex_unlock(&g_rd_lock); }
    if (dt > 100.0 && g_slow_reads++ < 30) fprintf(stderr, "[nx] slow read: fd %d, %zu bytes at %lld took %.0f ms\n", fd, n, (long long)off, dt);
    return r;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off){
    if (fd >= SFD_BASE) return sf_read_at(fd, buf, n, off);
    off_t cur = __real_lseek(fd, 0, SEEK_CUR);        /* not one of ours: libnx's descriptor (a file opened for writing) */
    ssize_t r = -1;
    if (cur >= 0 && __real_lseek(fd, off, SEEK_SET) >= 0){ r = __real_read(fd, buf, n); __real_lseek(fd, cur, SEEK_SET); }
    return r;
}
ssize_t __wrap_read(int fd, void *buf, size_t n){
    if (fd < SFD_BASE) return __real_read(fd, buf, n);
    pthread_mutex_lock(&g_sf_lock);
    s64 pos = sfd_ok(fd) ? g_sf[fd - SFD_BASE].pos : -1;
    pthread_mutex_unlock(&g_sf_lock);
    if (pos < 0){ errno = EBADF; return -1; }
    ssize_t r = sf_read_at(fd, buf, n, pos);
    if (r > 0){ pthread_mutex_lock(&g_sf_lock); if (sfd_ok(fd)) g_sf[fd - SFD_BASE].pos = pos + r; pthread_mutex_unlock(&g_sf_lock); }
    return r;
}
off_t __wrap_lseek(int fd, off_t off, int whence){
    if (fd < SFD_BASE) return __real_lseek(fd, off, whence);
    off_t r = -1;
    pthread_mutex_lock(&g_sf_lock);
    if (!sfd_ok(fd)) errno = EBADF;
    else {
        int i = fd - SFD_BASE;
        s64 np = whence == SEEK_SET ? off : whence == SEEK_CUR ? g_sf[i].pos + off : g_sf[i].size + off;
        if (np < 0) errno = EINVAL; else { g_sf[i].pos = np; r = (off_t)np; }
    }
    pthread_mutex_unlock(&g_sf_lock);
    return r;
}
int __wrap_fstat(int fd, struct stat *st){
    if (fd < SFD_BASE) return __real_fstat(fd, st);
    int r = -1;
    pthread_mutex_lock(&g_sf_lock);
    if (!sfd_ok(fd)) errno = EBADF;
    else { memset(st, 0, sizeof *st); st->st_mode = S_IFREG | 0444; st->st_size = (off_t)g_sf[fd - SFD_BASE].size; st->st_nlink = 1; r = 0; }
    pthread_mutex_unlock(&g_sf_lock);
    return r;
}

/* ---- what input_web.c and audio_web.c define and the runtime calls (the real
 * renderer, pad and sound of the native host replace these) ---- */
void metal_request_click(double x, double y){ (void)x; (void)y; }
/* the pad: nx_main.c (another thread) pushes key events, the guest thread applies them at each Present (the same hook the
 * browser build uses for its text entry), so the guest's message queue is only ever touched by guest threads */
void autotest_key(unsigned dik, int down);
static volatile unsigned g_pk_head, g_pk_tail; static unsigned short g_pk[64];
void nfsu2_pad_key(unsigned dik, int down){
    unsigned h = g_pk_head;
    if (h - g_pk_tail >= 64) return;
    g_pk[h & 63] = (unsigned short)((dik & 0xFF) | (down ? 0x100 : 0));
    __sync_synchronize(); g_pk_head = h + 1;
}
void nx_keys_drain(void){      /* text_nx.c's input_web_frame() calls it */
    while (g_pk_tail != g_pk_head){ unsigned short e = g_pk[g_pk_tail & 63]; g_pk_tail++; autotest_key(e & 0xFF, (e >> 8) & 1); }
}
float input_view_roll(void){ return 0.0f; }

/* ---- the console's controller as the game's virtual pad (runtime/gamepad.h, dinput_pad.c) ----------------------------
 * The main thread reads the HID pad (nx_main.c) and hands the state here; the game thread's gamepad_read() takes a copy.
 * dinput_pad.c does the rest: the layout the game gets (triggers throttle and brake, the left stick steers, L nitrous, R the
 * handbrake, ...) and the front end's keystrokes.  The console has no analog triggers: ZL and ZR are all or nothing. */
/* the game's PadState (gamepad.h) has libnx's name too: renamed while its header is read */
#define PadState GamePadState
#include "../gamepad.h"
#undef PadState
static pthread_mutex_t g_padmx = PTHREAD_MUTEX_INITIALIZER;
static GamePadState g_padstate;
void nfsu2_pad_state(uint32_t buttons, const float *axis){
    pthread_mutex_lock(&g_padmx);
    g_padstate.connected = 1; g_padstate.family = PADF_NINTENDO; g_padstate.wheel = 0;
    snprintf(g_padstate.name, sizeof g_padstate.name, "Nintendo Switch");
    g_padstate.buttons = buttons;
    memcpy(g_padstate.axis, axis, sizeof g_padstate.axis);
    pthread_mutex_unlock(&g_padmx);
}
static void stick_deadzone(float *x, float *y, float dz){
    float r = sqrtf(*x * *x + *y * *y);
    if (r <= dz){ *x = *y = 0.f; return; }
    float k = (r >= 1.f ? 1.f : (r - dz) / (1.f - dz)) / r;
    *x *= k; *y *= k;
    if (*x > 1.f) *x = 1.f; if (*x < -1.f) *x = -1.f;
    if (*y > 1.f) *y = 1.f; if (*y < -1.f) *y = -1.f;
}
int gamepad_read(GamePadState *s){
    pthread_mutex_lock(&g_padmx);
    *s = g_padstate;
    pthread_mutex_unlock(&g_padmx);
    stick_deadzone(&s->axis[PAD_AXIS_LX], &s->axis[PAD_AXIS_LY], 0.12f);    /* the console's sticks rest a little off centre */
    stick_deadzone(&s->axis[PAD_AXIS_RX], &s->axis[PAD_AXIS_RY], 0.12f);
    return s->connected;
}

/* report.txt: the pieces other files know (nx_main.c writes the file) */
void nfsu2_audio_report(FILE *f);
void nfsu2_text_report(FILE *f);
void nfsu2_report_extra(FILE *f){
    for (int i = 0; i < 8; i++)
        if (g_rd[i].n) fprintf(f, "file reads in flow %d: %lu reads, %.1f MB, %.0f ms in total (%.2f ms each), the longest %.0f ms\n", i, g_rd[i].n,
                               g_rd[i].bytes / 1048576.0, g_rd[i].ms, g_rd[i].ms / g_rd[i].n, g_rd[i].max_ms);
    fprintf(f, "core masks asked:");
    for (unsigned i = 0; i < g_pin_n; i++) fprintf(f, " 0x%x->0x%x", g_pin_rc[i] >> 24, g_pin_rc[i] & 0xFFFFFF);
    fprintf(f, "%s\n", g_pin_n ? "" : " none (nopin)");
    nfsu2_audio_report(f);
    nfsu2_text_report(f);
    { extern void touch_nx_report(FILE *); touch_nx_report(f); }
    { extern void lan_nx_report(FILE *f); lan_nx_report(f); }
    extern unsigned long nfsu2_gpu_shader_fails(void);
    extern void nfsu2_gpu_cache_report(FILE *f);
    nfsu2_gpu_cache_report(f);
    fprintf(f, "graphics: shader failures %lu\n", nfsu2_gpu_shader_fails());
}

/* ---- rumble ----
 * dinput_pad.c mixes the game's force-feedback effects into two motor levels (strong: the low band, weak: the high band) and
 * hands them to gamepad_rumble() at every poll.  The Joy-Con and Pro controller have a linear actuator on each side: both get the
 * two levels, as a low band at 160 Hz and a high band at 320 Hz.  Sets of devices are made for the handheld, a Joy-Con pair
 * and a Pro controller, and sent to all (the ones not there answer with an error that is ignored).  The motors are stopped when
 * the game stops asking (a pause) and at exit.  An empty file /switch/nfsu2/norumble turns it off. */
#define NSETS 7
static HidVibrationDeviceHandle g_vib[NSETS][2];
static int g_vib_n[NSETS], g_vib_ok[NSETS], g_vib_on;
static Result g_vib_rc[NSETS];
static float g_vib_s, g_vib_w; static double g_vib_t;
static const struct { const char *name; HidNpadIdType id; HidNpadStyleTag style; int n; } k_vsets[NSETS] = {
    { "handheld", HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 2 }, { "joy-con pair", HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 2 },
    { "pro controller", HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 2 }, { "joy-con left", HidNpadIdType_No1, HidNpadStyleTag_NpadJoyLeft, 1 },
    { "joy-con right", HidNpadIdType_No1, HidNpadStyleTag_NpadJoyRight, 1 }, { "player 2 pair", HidNpadIdType_No2, HidNpadStyleTag_NpadJoyDual, 2 },
    { "player 2 pro", HidNpadIdType_No2, HidNpadStyleTag_NpadFullKey, 2 } };
void nfsu2_rumble_init(void){
    struct stat st;
    if (stat("sdmc:/switch/nfsu2/norumble", &st) == 0){ fprintf(stderr, "[nx] rumble: off (norumble)\n"); return; }
    hidPermitVibration(true);
    for (int i = 0; i < NSETS; i++){
        g_vib_n[i] = k_vsets[i].n;
        g_vib_rc[i] = hidInitializeVibrationDevices(g_vib[i], k_vsets[i].n, k_vsets[i].id, k_vsets[i].style);
        g_vib_ok[i] = R_SUCCEEDED(g_vib_rc[i]); if (g_vib_ok[i]) g_vib_on = 1;
    }
    bool perm = false; hidIsVibrationPermitted(&perm);
    fprintf(stderr, "[nx] rumble: permitted %d;", perm ? 1 : 0);
    for (int i = 0; i < NSETS; i++) fprintf(stderr, " %s %s(0x%x)", k_vsets[i].name, g_vib_ok[i] ? "ok" : "no", (unsigned)g_vib_rc[i]);
    fprintf(stderr, "\n");
}
static void vib_send(float strong, float weak){
    if (strong < 0.f) strong = 0.f; if (strong > 1.f) strong = 1.f;
    if (weak < 0.f) weak = 0.f; if (weak > 1.f) weak = 1.f;
    HidVibrationValue v[2] = { { strong, 160.0f, weak, 320.0f }, { strong, 160.0f, weak, 320.0f } };
    for (int i = 0; i < NSETS; i++) if (g_vib_ok[i]) hidSendVibrationValues(g_vib[i], v, g_vib_n[i]);
}
int gamepad_rumble_ok(void){ return g_vib_on; }
void gamepad_rumble(float strong, float weak){
    if (!g_vib_on) return;
    double now = nfsu2_now_ms();
    if (strong == g_vib_s && weak == g_vib_w && now - g_vib_t < 120.0) return;      /* only on a change, and again every 120 ms */
    g_vib_s = strong; g_vib_w = weak; g_vib_t = now;
    vib_send(strong, weak);
}
/* the main thread, every loop: the game stopped asking (paused, parked): the motors stop */
void nfsu2_rumble_tick(void){
    if (!g_vib_on || (g_vib_s == 0.f && g_vib_w == 0.f)) return;
    if (nfsu2_now_ms() - g_vib_t > 300.0){ g_vib_s = g_vib_w = 0.f; vib_send(0.f, 0.f); }
}
void nfsu2_rumble_stop(void){ if (g_vib_on){ g_vib_s = g_vib_w = 0.f; vib_send(0.f, 0.f); } }
