/* roam_host.h -- free-roam multiplayer: what roam_game.c needs from its host,
 * for the native port (the guest's memory is the 4 GB arena at g_mem, guest
 * code is the lifted C, called through dispatch_indirect).
 *
 * roam_game.c is shared with the Windows companion (win/roam_asi), which
 * builds it with -DROAM_HOST_WIN and win/roam_asi/roam_host_win.h instead:
 * there the guest's memory is the process's own (a guest address is a
 * pointer) and guest code is the original x86 code, called through a
 * pointer.  Both headers provide the same names:
 *
 *   MR8/MR16/MR32/MRF32(a)  guest memory as lvalues;  GPTR(a)  a host pointer
 *   rh_committed(a)         1 when guest address a is readable and writable
 *   rh_call(c, fn, ecx, edx, n, args)
 *                           call guest function fn with ecx/edx and n stack
 *                           arguments (any convention: the stack is restored
 *                           afterwards); returns eax.  Only from the main-loop
 *                           hook, on the game's thread
 *   rlog(fmt, ...)          one log line (printf format)
 *   rh_opt(name)            an option (NFSU2_ROAM_*), or NULL
 *   rh_frame()              the frame number (logs)
 *   rh_heap_mb()            guest heap in use (live blocks), MB (0 when not known)
 *   rh_ns()                 a monotonic clock in ns (frame cost)
 *   rh_file, rh_fopen/rh_fprintf/rh_fflush   the pose log (test aid)
 *   rh_hud_marker(&x, &y)   a HUD marker's position (640x480 units from the
 *                           view's pixels) as the screen shows the HUD
 *   rh_horn_held(stick)     1 while the horn key or button is held (horn.h's horn_held)
 *   rh_horn_mine()          the player's horn (horn.h's horn_mine), -1: none here
 *   rh_horn_name(kind)      a horn's name (horn.h's horn_name)
 *   rh_horn_begin/rh_horn_sound/rh_horn_end
 *                           a frame of horns (horn.h's horn_begin, ...)
 *   rh_carsfx_begin/rh_carsfx_car/rh_carsfx_end
 *                           a frame of friends' nitrous and hydraulics, heard
 *                           (carsfx.h's carsfx_begin, ...)
 *   rh_no_profile(name)     1 when a profile name is the game's "No Profile"
 *                           (language.c's language_no_profile: in the game's
 *                           language)
 *   rh_drift_tuning(out)    1 with the 41 bytes of the dyno setup the player's
 *                           ride had before drift mode put its Drift setup in
 *                           (drift.c's drift_kept_tuning), 0 when it has its own
 *   ROAM_HOST_SCRIPT        defined when the input-script commands exist
 *   RH_FE_NODES             the most objects FEng draws of one package (its
 *                           level sorter's nodes)
 */
#ifndef ROAM_HOST_H
#define ROAM_HOST_H
#include "cpu.h"
#include "horn.h"
#include "carsfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <mach/mach_time.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);
uint32_t win32_heap_live(void);          /* win32.c: bytes in live heap blocks */
void ws_hud_marker(float *x, float *y);  /* widescreen.c */
int language_no_profile(const char *name);   /* language.c */
int drift_kept_tuning(uint8_t *out);     /* drift.c */

#define ROAM_HOST_SCRIPT 1
#define ROAM_HOOK_RET    0x00581475u     /* the return address guest calls see */
#define RH_FE_NODES      4096            /* objects FEng draws of one package: overrides.c's level sorter (the game: 512) */
#define GPTR(a)         (g_mem + (uint32_t)(a))
#define rh_committed(a)  mem_is_committed(a)
#define rlog(...)        fprintf(stderr, __VA_ARGS__)
#define rh_opt(k)        getenv(k)
#define rh_frame()       autotest_frame()
#define rh_heap_mb()     (win32_heap_live() >> 20)
#define rh_hud_marker(x, y) ws_hud_marker((x), (y))
#define rh_horn_held(stick) horn_held(stick)
#define rh_horn_mine()   horn_mine()
#define rh_horn_name(k)  horn_name(k)
#define rh_horn_begin(pos, fwd, vel)   horn_begin((pos), (fwd), (vel))
#define rh_horn_sound(who, name, kind, p, v) horn_sound((who), (name), (kind), (p), (v))
#define rh_horn_end()    horn_end()
#define rh_carsfx_begin(pos, fwd, vel)    carsfx_begin((pos), (fwd), (vel))
#define rh_carsfx_car(who, name, p, v, st) carsfx_car((who), (name), (p), (v), (st))
#define rh_carsfx_end()  carsfx_end()
#define rh_no_profile(n) language_no_profile(n)
#define rh_drift_tuning(out) drift_kept_tuning(out)
typedef FILE rh_file;
#ifdef __wasm__
/* The pose log (a test aid) in the browser: every stdio flush is a
 * synchronous round trip to the I/O worker and an OPFS write.  With stdio's
 * 1 KB buffer that was a flush every ~3 frames and made roam_game_frame cost
 * 0.3 ms a frame on average (max 36 ms) against 20 us without the log.  The
 * buffer holds what the periodic rh_fflush (every 240 frames, roam_game.c)
 * writes, so that stays the only flush: per frame one L row (our car) and
 * one R row per friend's car shown (at most 32, roam_game.c NSLOT), each
 * about 145 bytes (147 at most in the measured runs), with room to 256.
 * 64 KB was 229 frames with one friend. */
#define RH_POSELOG_BUF (240u * (1u + 32u) * 256u)
static inline FILE *rh_fopen(const char *p){
    FILE *f = fopen(p, "w");
    /* musl ignores the size when setvbuf gets no buffer: give it one (one
     * pose log per process, kept open until exit) */
    char *b = f ? malloc(RH_POSELOG_BUF) : NULL;
    if (b) setvbuf(f, b, _IOFBF, RH_POSELOG_BUF);
    return f;
}
#else
#define rh_fopen(p)      fopen((p), "w")
#endif
#define rh_fprintf       fprintf
#define rh_fflush        fflush

static inline uint64_t rh_ns(void){
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
/* call a guest function (any convention) from the main-loop hook; the CPU
 * state is restored afterwards */
static inline uint32_t rh_call(CPU *c, uint32_t fn, uint32_t ecx, uint32_t edx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 1024u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx; c->edx = edx;
    PUSH32(ROAM_HOOK_RET);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
#endif
