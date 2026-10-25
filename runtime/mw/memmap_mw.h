/* Most Wanted (speed.exe, image 0x00400000-0x00A79000): its part of the
 * guest layout (runtime/memmap.h).  UG2's handle values and server.dll base
 * fall inside this image, so they move out of it: the handle values into the
 * unmapped hole after the image, the DLLs above the TEBs. */
#ifndef MEMMAP_MW_H
#define MEMMAP_MW_H

#define MM_GAME         "mw"

/* handle values, in 0x00A80000-0x00B00000 (no guest memory there) */
#define MM_FH_BASE      0x00A80000u     /* fileio.c: file and find handles */
#define MM_FH_MAX       256
#define MM_H_BASE       0x00A90000u     /* win32_ext.c: kernel objects */
#define MM_H_MAX        4096
#define MM_H_GENS       112             /* handle generations (obj_reclaim): up to 0x00B00000 */

/* server.dll (0x6F000), lifted at this base (games/mw/game.conf SRV_BASE),
 * right after the TEBs */
#define MM_SRV_LO       0x00D00000u
#define MM_SRV_HI       0x00D6F000u

/* d3dx9_26.dll (0x24A000), lifted only in builds with the D3DX oracle
 * (games/mw/game.conf D3DX_BASE) */
#define MM_D3DX_LO      0x00D80000u
#define MM_D3DX_HI      0x00FCA000u

/* the DLLs' own heaps (modheap.c), half of the modheap region each:
 * server.dll's CRT heap (one per host session; UG2's server.dll peaks at
 * 22.6 MB in one heap) and the oracle's */
#define MM_SRV_HEAP_BASE  MM_MODHEAP_BASE
#define MM_SRV_HEAP_SIZE  MM_MODHEAP_SIZE
#define MM_SRV_HEAP_N     1
#define MM_D3DX_HEAP_BASE (MM_MODHEAP_BASE + MM_MODHEAP_SIZE)
#define MM_D3DX_HEAP_SIZE MM_MODHEAP_SIZE
#define MM_D3DX_HEAP_N    1

/* a handle value never lands on guest memory */
#define MM_VALUES_APART 1

#endif
