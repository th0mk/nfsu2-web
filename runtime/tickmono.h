/* tickmono.h -- a millisecond tick that never steps back between threads, as Windows' GetTickCount never does.
 *
 * Each guest thread reads its own clock: in the browser every guest thread runs on its own web worker, whose clock
 * is set to the page's only to within about half the best round trip of its 'clock' messages (web/src/host
 * game-worker.ts), and the virtual clock (win32.c host_ms) can answer threads out of order.  So two threads can read
 * the same moment a millisecond or more apart, and a later read on one can be less than an earlier read on another.
 * DirtySock's CommUDP (both games) stamps each datagram it takes with the tick on one thread and takes a later tick
 * on another minus that stamp UNSIGNED: one step back is 2^32 ms of silence, past its 120 s timeout, and it drops
 * the race link with a DISC.
 *
 * tick_mono answers v, or the latest answer any thread was given when v is behind it (compared modulo 2^32, as the
 * counter wraps).  The first five reads behind it, and every 1000th, are logged as "[clock] <what>: ...".  Users:
 * win32.c's GetTickCount of the exe (GameOps tick_monotonic) and game_ug2.c's NetTick (DirtySock's clock). */
#ifndef TICKMONO_H
#define TICKMONO_H
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { _Atomic uint32_t last, backs; } TickMono;   /* zero-initialised: no answer yet */

static inline uint32_t tick_mono(TickMono *t, uint32_t v, const char *what){
    uint32_t l = atomic_load(&t->last);
    for (;;){
        if (l && (int32_t)(v - l) <= 0){
            if (v != l){
                uint32_t n = atomic_fetch_add(&t->backs, 1u) + 1u;
                if (n <= 5u || n % 1000u == 0u)
                    fprintf(stderr, "[clock] %s: a read %u ms behind the latest answer gets that answer (%u so far)\n",
                            what, l - v, n);
            }
            return l;
        }
        if (atomic_compare_exchange_weak(&t->last, &l, v)) return v;
    }
}
#endif
