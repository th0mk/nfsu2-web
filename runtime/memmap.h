/* The guest's fixed layout: every region of guest memory the runtime places
 * itself, and every range of handle values it hands out.  What differs per
 * game (the handle values, the slots of the lifted DLLs and their parts of
 * the modheap region) is in the game's header, memmap_ug2.h by default or
 * mw/memmap_mw.h with -DNFSU2_GAME_MW; the game's file hands the DLLs' values
 * to core with its module list (game.h modules).  The rest is the same for
 * every game and is here.
 *
 * win32_init checks the layout at boot (memmap.c): no two memory regions
 * overlap, none overlaps the exe's image (its range comes from the PE), no
 * two handle-value ranges overlap, each lifted DLL lies inside its slot and
 * its heaps inside the modheap region, apart from the other DLLs' heaps.
 * It logs one [mem] line, and stops naming both ranges when the check fails.
 *
 * Include cpu.h first (GUEST_END). */
#ifndef MEMMAP_H
#define MEMMAP_H
#include <stdint.h>

#ifdef NFSU2_GAME_MW
#include "mw/memmap_mw.h"
#else
#include "memmap_ug2.h"
#endif

/* ---- values, the same for every game ---------------------------------- */
#define MM_HWND_DESKTOP 0x00B00000u     /* GetDesktopWindow */
#define MM_HWND_GAME    0x00B00001u     /* the game's window */
#define MM_HICON        0x00B10001u     /* LoadIconA, LoadCursorA */
#define MM_HDC          0x00B20001u     /* GetDC, BeginPaint */

/* ---- guest memory, the same for every game ----------------------------- */
#define MM_WSBUF_BASE   0x00BE0000u     /* winsock.c: per-thread hostent / inet_ntoa buffers */
#define MM_WSBUF_SLOT   512u
#define MM_WSBUF_N      64
#define MM_TEB_LO       0x00C00000u     /* TEBs and the PEB; TlsFree scans this range for TEBs */
#define MM_TEB_HI       0x00D00000u
#define MM_TEB_MAIN     0x00C00000u     /* the main thread's TEB (0x20000 committed, TLS array at +0x1000) */
#define MM_PEB          0x00C10000u
#define MM_TEB_FIRST    0x00C20000u     /* created threads' TEBs, MM_TEB_SIZE apart */
#define MM_TEB_SIZE     0x2000u
#define MM_TEB_TIMER    0x00C60000u     /* the WINMM timer thread's TEB */
#define MM_MODHEAP_BASE 0x01000000u     /* modheap.c: the CRT heaps of the lifted DLLs */
#define MM_MODHEAP_N    2
#ifdef __wasm__
#define MM_MODHEAP_SIZE 0x02000000u     /* 32 MB per heap: 0x01000000-0x05000000 */
#define MM_STACK_LO     0x05000000u     /* the main guest thread's stack */
#define MM_STACK_HI     0x05FF0000u
#define MM_TSTACK_LO    0x06000000u     /* created threads' stacks, MM_TSTACK_SIZE each, up to the heap */
#define MM_TSTACK_HI    0x08000000u
#define MM_HEAP_LO      0x08000000u     /* heap.c: the TLSF heaps and VirtualAlloc */
#define MM_HEAP_HI      GUEST_END
#else
/* native (the Mac): tests/boot.c puts the main stack at the top */
#define MM_MODHEAP_SIZE 0x07000000u     /* 112 MB per heap: 0x01000000-0x0F000000 */
#define MM_STACK_LO     0x7F000000u
#define MM_STACK_HI     0x7FF00000u
#define MM_TSTACK_LO    0x60000000u
#define MM_TSTACK_HI    0x7F000000u
#define MM_HEAP_LO      0x10000000u
#define MM_HEAP_HI      0x60000000u
#endif
#define MM_TSTACK_SIZE  0x00100000u     /* 1 MB */

/* memmap.c: the boot check (win32_init).  Image: the exe's range from its PE
 * header.  Returns 0 when the layout holds; otherwise it has printed both
 * ranges, and with fatal set it does not return. */
int memmap_check(uint32_t image_lo, uint32_t image_hi, int fatal);

#endif
