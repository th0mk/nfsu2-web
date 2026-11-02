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
 * break.  sbrk(0) is the break: the top of everything the heap ever took. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

extern char __heap_end;
static uintptr_t g_brk;

void *sbrk(intptr_t increment){
    if (!g_brk) g_brk = (uintptr_t)&__heap_end;
    if (increment < 0) abort();                       /* the memory never shrinks (dlmalloc never trims here) */
    /* in 64 bits: a 4 GB memory's size does not fit a wasm32 size_t */
    uint64_t old = g_brk, end = old + (uint64_t)increment;
    uint64_t have = (uint64_t)__builtin_wasm_memory_size(0) * 65536u;
    if (end > 0xFFFF0000u){ errno = ENOMEM; return (void *)-1; }
    if (end > have && __builtin_wasm_memory_grow(0, (size_t)((end - have + 65535u) / 65536u)) == SIZE_MAX){
        errno = ENOMEM; return (void *)-1;
    }
    g_brk = (uintptr_t)end;
    return (void *)(uintptr_t)old;
}

/* the break (the top of all the heap ever took) and the memory's size, in bytes, for the status line (autotest.c) */
uint64_t heap_break(void){ return g_brk ? g_brk : (uintptr_t)&__heap_end; }
uint64_t memory_bytes(void){ return (uint64_t)__builtin_wasm_memory_size(0) * 65536u; }
