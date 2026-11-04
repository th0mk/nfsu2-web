/* guest_span.h -- where the guest's memory is on wasm.
 *
 * The guest owns [0, GUEST_END) of the linear memory (cpu.h); the runtime's
 * data, heap and stacks live above it.  On a computer whose browser gave the
 * game a memory of 2 GB from the start (the page asks for it: host/process.ts,
 * NFSU2_HEAP_WINDOW=1), the guest heap also has a window above the runtime,
 * [GWIN_LO, GWIN_HI), kept below 2 GB in case the game reads a pointer as
 * signed: heap.c's VA manager spans both, sbrk.c (the runtime's heap) steps
 * over the window, and mem.c keeps its pages' commit bits as for the guest's
 * own.  Only guest data lives in the window, so cpu.h's g_guest alias rule
 * (no guest access reaches the runtime's data) still holds.  Phones, tablets
 * and Safari keep today's layout: g_gwin_hi is 0 there.  mem_window_init
 * (mem.c) decides once, at heap_init, and logs which. */
#ifndef NFSU2_GUEST_SPAN_H
#define NFSU2_GUEST_SPAN_H
#include <stdint.h>

#define GWIN_LO 0x50000000u            /* 1280 MB: the runtime's heap below it (it used up to ~1020 MB) */
#define GWIN_HI 0x7FFE0000u            /* 2 GB less 128 KB: no block ends at 2^31, where a signed compare of its end
                                          turns it negative (the game was not large-address-aware: its address space
                                          on Windows ended at 0x7FFEFFFF) */
#define GWIN_GUARD 0x20000u            /* the runtime's heap goes on this far past the window's top (2 GB): nobody's */

#ifdef __wasm__
extern uint32_t g_gwin_lo, g_gwin_hi;  /* the window, [lo, hi), or 0, 0 (mem.c) */
/* [a, a + n) inside the guest's memory: [lo, 0x28000000) (cpu.h GUEST_END) or the window */
static inline int guest_span(uint32_t lo, uint32_t a, uint32_t n){
    uint64_t e = (uint64_t)a + n;
    if (a >= lo && e <= 0x28000000u) return 1;
    return g_gwin_hi && a >= g_gwin_lo && e <= g_gwin_hi;
}
#endif
#endif
