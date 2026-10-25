/* UG2 (SPEED2.EXE, image 0x00400000-0x00932000): its part of the guest
 * layout (runtime/memmap.h). */
#ifndef MEMMAP_UG2_H
#define MEMMAP_UG2_H

#define MM_GAME         "ug2"

/* handle values */
#define MM_FH_BASE      0x00900000u     /* fileio.c: file and find handles */
#define MM_FH_MAX       256
#define MM_H_BASE       0x00A00000u     /* win32_ext.c: kernel objects (events, mutexes, threads, keys) */
#define MM_H_MAX        4096
#define MM_H_GENS       256             /* handle generations (obj_reclaim): up to 0x00B00000 */

/* server.dll, lifted at this base (games/ug2/game.conf SRV_BASE) */
#define MM_SRV_LO       0x00A00000u
#define MM_SRV_HI       0x00A58000u
/* its own heaps (modheap.c): all of the modheap region, one heap per host
 * session (its CRT's), the second one spare */
#define MM_SRV_HEAP_BASE MM_MODHEAP_BASE
#define MM_SRV_HEAP_SIZE MM_MODHEAP_SIZE
#define MM_SRV_HEAP_N    MM_MODHEAP_N

/* No lifted D3DX: UG2's is linked into the exe. */

/* The handle values above lie on guest memory: the file handles inside the
 * image, the object handles on server.dll's.  Nothing reads a handle
 * as a pointer, and moving them would change UG2, so the boot check allows
 * it here. */
#define MM_VALUES_APART 0

#endif
