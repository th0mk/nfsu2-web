/* Force-included (-include) in every runtime file of the wasm build.
 * Small stand-ins for macOS-only APIs so the shared runtime sources need as
 * few #ifdefs as possible.  Only on the include path of web/build. */
#ifndef NFSU2_WASM_PLAT_H
#define NFSU2_WASM_PLAT_H
#define NFSU2_HOST_PLATFORM 1   /* no OS services: the page provides them (the shared runtime tests this, not __wasm__) */
#include <stdint.h>
#include <time.h>
#include <pthread.h>
/* macOS clock ids: one monotonic clock in the browser */
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC   /* no per-thread CPU clock: wall time (diagnostics only) */
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t id){
    struct timespec ts; clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline int pthread_setname_np(const char *name){ (void)name; return 0; }
/* pause(): park forever (used by the exit freeze thread) */
#include <unistd.h>
static inline int pause(void){ for (;;) sleep(3600); return -1; }
/* WASI has no time zone database (wasi-libc's local time is UTC): local
 * time comes from the page's zone instead (NFSU2_TZ, runtime/web/tz.c), for
 * GetTimeZoneInformation and FileTimeToLocalFileTime (win32_ext.c). */
extern const char *nfsu2_tzname[2];
struct tm *nfsu2_localtime_r(const time_t *t, struct tm *out);
struct tm *nfsu2_localtime(const time_t *t);
time_t nfsu2_mktime(struct tm *tm);
#define tzname nfsu2_tzname
#define localtime_r nfsu2_localtime_r
#define localtime nfsu2_localtime
#define mktime nfsu2_mktime
/* wasi-libc threads have no pthread_exit.  Every thread starts through a
 * trampoline that sets a jump point; pthread_exit longjmps back to it (wasm
 * exception handling unwinds the lifted frames).  runtime/web/thread_exit.c */
int nfsu2_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
_Noreturn void nfsu2_pthread_exit(void *value);
#define pthread_create nfsu2_pthread_create
#define pthread_exit nfsu2_pthread_exit
#endif
