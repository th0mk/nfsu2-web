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
 * win32.c's GetTickCount of the exe (GameOps tick_monotonic) and game_ug2.c's NetTick (DirtySock's clock).
 *
 * What it keeps is the order of the answers, not the length of a wait.  An answer can be ahead of the calling
 * thread's own clock by up to the skew between the threads' clocks (plus a preemption between the clock read and
 * the exchange below): the latest answer may come from a thread whose clock is ahead.  So a wait timed on one thread
 * from a start read that was clamped up ends up to that much late, and one whose later read is clamped up ends up
 * to that much early; without the clamp only a wait started on one thread and checked on another was off by the
 * skew.  In the browser the skew is about a millisecond (half the best round trip; the virtual clock's out-of-order
 * answers are mostly 1 ms apart), small next to DirtySock's timers of 100 ms to 120 s, which need a later tick
 * minus an earlier one never to go negative. */
#ifndef TICKMONO_H
#define TICKMONO_H
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

/* last: 1 << 32 | the latest answer, 0 before the first (zero-initialised).  The bit above the answer, not an
 * answer of 0, says there is one: the counter passes 0 when it wraps, and while the latest answer was 0 a thread
 * whose clock is behind would take it as no answer yet and set it back. */
typedef struct { _Atomic uint64_t last; _Atomic uint32_t backs; } TickMono;
#define TICKMONO_SET (1ull << 32)

static inline uint32_t tick_mono(TickMono *t, uint32_t v, const char *what){
    uint64_t l = atomic_load(&t->last);
    for (;;){
        uint32_t a = (uint32_t)l;
        if (l && (int32_t)(v - a) <= 0){
            if (v != a){
                uint32_t n = atomic_fetch_add(&t->backs, 1u) + 1u;
                if (n <= 5u || n % 1000u == 0u)
                    fprintf(stderr, "[clock] %s: a read %u ms behind the latest answer gets that answer (%u so far)\n",
                            what, a - v, n);
            }
            return a;
        }
        if (atomic_compare_exchange_weak(&t->last, &l, TICKMONO_SET | v)) return v;
    }
}
#endif
