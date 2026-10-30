/* roam_host_win.h -- free-roam multiplayer: what runtime/roam_game.c needs
 * from its host, for the Windows companion NFSU2FreeRoam.asi (the original
 * 32-bit SPEED2.EXE v1.2 NA).  The names are those of runtime/roam_host.h
 * (the port); see there.
 *
 * Here the game's memory is the process's own: a guest address is a
 * pointer, and the guest functions are the game's original x86 code, called
 * through roam_win_call (roam_asi.c) with ecx/edx and stack arguments, any
 * convention (the stack pointer is restored after the call).  There is no C
 * runtime: the few C library functions roam_game.c uses come from
 * roam_libc.c, and no system header is included here (windows.h would bring
 * in names roam_game.c does not expect).
 */
#ifndef ROAM_HOST_WIN_H
#define ROAM_HOST_WIN_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef struct CPU CPU;                  /* the port's guest CPU: not used here (always 0) */

/* ---- guest memory: the process's own -------------------------------------- */
#define MR8(a)   (*(uint8_t  *)(uintptr_t)(uint32_t)(a))
#define MR16(a)  (*(uint16_t *)(uintptr_t)(uint32_t)(a))
#define MR32(a)  (*(uint32_t *)(uintptr_t)(uint32_t)(a))
#define MRF32(a) (*(float    *)(uintptr_t)(uint32_t)(a))
#define GPTR(a)  ((uint8_t *)(uintptr_t)(uint32_t)(a))
int      roam_win_committed(uint32_t a);          /* VirtualQuery, cached */
#define rh_committed(a) roam_win_committed(a)

/* ---- guest calls --------------------------------------------------------------- */
uint32_t roam_win_call(uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *args);
#define rh_call(c, fn, ecx, edx, n, args) ((void)(c), roam_win_call((fn), (ecx), (edx), (n), (args)))

/* ---- options, log, clocks ------------------------------------------------------- */
const char *roam_opt(const char *name);           /* environment, then the .ini */
void        roam_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
long        roam_win_frame_no(void);
uint64_t    roam_win_ns(void);
#define rlog(...)        roam_log(__VA_ARGS__)
#define rh_opt(k)        roam_opt(k)
#define rh_frame()       roam_win_frame_no()
#define rh_heap_mb()     0u
#define rh_hud_marker(x, y) ((void)(x), (void)(y))   /* the HUD as the game shows it */
#define RH_FE_NODES      512             /* the game's own level sorter (0x536520's 0x1000 bytes): nothing patched here */
#define rh_ns()          roam_win_ns()
/* no horn here: the companion neither sounds one nor plays friends', nor
 * their nitrous and hydraulics (carsfx.c) */
#define rh_horn_held(stick) ((void)(stick), 0)
#define rh_horn_mine()   (-1)
#define rh_horn_name(k)  ((void)(k), "another")
#define rh_horn_begin(pos, fwd, vel)   ((void)(pos), (void)(fwd), (void)(vel))
#define rh_horn_sound(who, name, kind, p, v) ((void)(who), (void)(name), (void)(kind), (void)(p), (void)(v))
#define rh_horn_end()    ((void)0)
#define rh_carsfx_begin(pos, fwd, vel)    ((void)(pos), (void)(fwd), (void)(vel))
#define rh_carsfx_car(who, name, p, v, st) ((void)(who), (void)(name), (void)(p), (void)(v), (void)(st))
#define rh_carsfx_end()  ((void)0)
/* the game's "No Profile" in English only (the port also knows the other
 * languages' names from the game's text, language.c) */
#define rh_no_profile(name) (!strcasecmp((name), "No Profile"))
/* no drift mode here (drift.c): the ride's dyno setup is always its own */
#define rh_drift_tuning(out) ((void)(out), 0)

/* the pose log (test aid, PoseLog in the .ini) */
typedef struct rh_file rh_file;
rh_file *rh_fopen(const char *path);
int      rh_fprintf(rh_file *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void     rh_fflush(rh_file *f);

/* ---- the C library subset (roam_libc.c) ------------------------------------------ */
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
long   strtol(const char *s, char **end, int base);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int    snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#endif
