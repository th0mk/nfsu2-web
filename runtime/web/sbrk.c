/* The runtime heap's break (wasi-libc's dlmalloc takes its memory from sbrk).
 *
 * The module declares a small minimum memory (web/build.sh MIN_MB: just past
 * the runtime's data and main stack), so that every byte the heap takes comes
 * through here and its break says what the game has used of its memory (the
 * [status] line's brk=; the page gives 1 GB, host/memory.ts).
 * dlmalloc begins with [__heap_base, __heap_end), where wasm-ld puts
 * __heap_end at that declared minimum, and asks sbrk for the rest.  wasi-libc's
 * own sbrk grows the memory from its current end, so with the page's usual
 * 1 GB everything between the minimum and 1 GB would never be used and the
 * memory would grow past 1 GB at once.  This one hands out the memory from
 * __heap_end on, in order, and grows the memory only past what the page gave:
 * the heap stays one contiguous run, as large before any growth as it always
 * was.  dlmalloc calls it with its global lock held; nothing else moves the
 * break.  sbrk(0) is the break: the top of everything the heap ever took.
 *
 * With the guest heap's window above the runtime (guest_span.h, a computer's
 * browser) the heap goes on past the window's end (and GWIN_GUARD more: at
 * 2 GB, where the page reads its pointers as unsigned, host/game-worker.ts
 * u32args) instead of into it: the
 * piece below the window is left unused and dlmalloc takes the memory after
 * the window as a segment of its own (its sys_alloc adds a segment for a
 * break that does not continue the last).  The runtime kept below ~1020 MB in
 * every measured game, so that is room it should never need. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "../guest_span.h"

extern char __heap_end;
static uintptr_t g_brk;

static uint64_t g_stepped;                            /* bytes left below the window by the step over it */
/* (mem.c, test runs only: NFSU2_TEST_HIGH_RUNTIME) the next growth steps over the window whatever its size, so the
 * runtime's heap is at 2 GB or more from then on, as after a long session, and the page's handling of such pointers
 * is tried */
int g_sbrk_high = 0;

void *sbrk(intptr_t increment){
    if (!g_brk) g_brk = (uintptr_t)&__heap_end;
    if (increment < 0){
        /* dlmalloc never trims here (the memory never shrinks); it gives back only the piece a growth got when it
         * could not get the rest (its sys_alloc's partial failure), which may follow a step over the window: the
         * break goes back over that piece, never back into the window */
        uint32_t whi = __atomic_load_n(&g_gwin_hi, __ATOMIC_ACQUIRE);
        uint64_t back = (uint64_t)-(int64_t)increment, top = whi ? (uint64_t)whi + GWIN_GUARD : 0;
        uint64_t floor = top && g_brk >= top ? top : (uintptr_t)&__heap_end;
        if (g_brk - floor < back){ errno = EINVAL; return (void *)-1; }
        uint64_t was = g_brk; g_brk = (uintptr_t)(g_brk - back);
        return (void *)(uintptr_t)was;
    }
    /* in 64 bits: a 4 GB memory's size does not fit a wasm32 size_t */
    uint64_t old = g_brk, end = old + (uint64_t)increment;
    /* (the top first: mem.c sets the bottom before it) */
    uint32_t whi = __atomic_load_n(&g_gwin_hi, __ATOMIC_ACQUIRE), wlo = __atomic_load_n(&g_gwin_lo, __ATOMIC_ACQUIRE);
    if (increment && whi && old < (uint64_t)whi + GWIN_GUARD && (end > wlo || g_sbrk_high)){
        old = (uint64_t)whi + GWIN_GUARD; end = old + (uint64_t)increment; }
    uint64_t have = (uint64_t)__builtin_wasm_memory_size(0) * 65536u;
    if (end > 0xFFFF0000u){ errno = ENOMEM; return (void *)-1; }
    if (end > have && __builtin_wasm_memory_grow(0, (size_t)((end - have + 65535u) / 65536u)) == SIZE_MAX){
        errno = ENOMEM; return (void *)-1;
    }
    if (old != g_brk) g_stepped += old - g_brk;
    g_brk = (uintptr_t)end;
    return (void *)(uintptr_t)old;
}

/* the break (the top of all the heap ever took) and the memory's size, in bytes, for the status line (autotest.c) */
uint64_t heap_break(void){ return g_brk ? g_brk : (uintptr_t)&__heap_end; }
/* what the runtime's heap has taken: the break less what the step over the guest heap's window left out */
uint64_t heap_taken(void){ return heap_break() - g_stepped; }
uint64_t memory_bytes(void){ return (uint64_t)__builtin_wasm_memory_size(0) * 65536u; }
