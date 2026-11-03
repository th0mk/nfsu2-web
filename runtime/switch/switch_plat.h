/* Force-included (-include) in every runtime file of the Switch build (devkitA64, newlib, libnx): small stand-ins for
 * the macOS APIs the shared runtime uses, as runtime/web/compat/wasm_plat.h does for the browser. */
#ifndef NFSU2_SWITCH_PLAT_H
#define NFSU2_SWITCH_PLAT_H
#define NFSU2_HOST_PLATFORM 1   /* no mmap/signals/BSD LAN: the native host provides them (same macro as wasm_plat.h) */
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC   /* wall time (diagnostics only) */
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t id){
    struct timespec ts; clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* macOS names the calling thread with one argument; the Switch has no thread names */
static inline int nfsu2_setname1(const char *name){ (void)name; return 0; }
#define pthread_setname_np nfsu2_setname1
/* BSD stat times: newlib's names (the file's birth time is its change time here) */
#define st_birthtimespec st_ctim
#define st_atimespec st_atim
#define st_mtimespec st_mtim
/* newlib's struct tm has no tm_gmtoff: the time zone is UTC until libnx's is read (read-only uses in win32_ext.c) */
#define tm_gmtoff tm_isdst * 0

/* wasm's memory.atomic.wait32 / notify (ticketlock.h, the page-contract files) are the kernel's address arbiter here
 * (runtime/switch/nx_host.c: svcWaitForAddress / svcSignalToAddress, firmware 4.0 and later).  libnx's own headers
 * are not included here: their types (s16, u8, Result ...) clash with the runtime's. */
int nfsu2_wait32(int *p, int expected, long long timeout_ns);
int nfsu2_notify(int *p, unsigned count);
#define __builtin_wasm_memory_atomic_wait32(p, e, t) nfsu2_wait32((p), (e), (t))
#define __builtin_wasm_memory_atomic_notify(p, n) nfsu2_notify((p), (n))
#endif
