/* heap.c -- guest heap (v1.2 NA port, long-session track).
 *
 * The upstream runtime served HeapAlloc, HeapReAlloc, VirtualAlloc,
 * GlobalAlloc and LocalAlloc from one bump pointer and never freed anything
 * (HeapFree/VirtualFree/GlobalFree were no-ops).  That is fine for a test of a
 * few minutes but a full career (hundreds of loads, hours of free roam) would
 * eventually exhaust the 496 MB arena.  This module gives the guest a real
 * memory manager:
 *
 *   VA manager   [GH_BASE, GH_LIMIT) in 64 KiB granules (on wasm also the
 *                window above the runtime, where there is one: guest_span.h;
 *                the runtime's memory between them is never handed out),
 *                VirtualAlloc with
 *                reserve/commit/decommit/release semantics, VirtualQuery,
 *                VirtualProtect.  Heap segments and large blocks are VA
 *                regions too, so freed address space is shared by everyone;
 *                big blocks keep their holes for a while (va_find) and leave
 *                room for the heaps' segments (va_alloc).
 *   heaps        GetProcessHeap (handle 1), HeapCreate (2, 3, ...), plus two
 *                internal heaps for the COM layer (objects, Lock buffers).
 *                TLSF (two-level segregated fit): O(1) alloc/free, immediate
 *                coalescing, 16-byte aligned user pointers, in-band 16-byte
 *                headers.  Blocks >= LARGE_MIN get their own VA region and go
 *                back to the VA manager when freed; so does a segment once
 *                all of it is free (one kept per heap).
 *   stats        always on (counts, live/peak bytes, largest allocations, the
 *                longest free run in the [status] line's vrun=, the address
 *                space's map when an allocation finds no room);
 *                NFSU2_HEAPSTAT=1 adds per-call-site live bytes;
 *                NFSU2_HEAPMAP=1 (test runs: with NFSU2_INPUT_SCRIPT) the map
 *                with every [status] line, the most VA and the least longest
 *                free run since the line before, and each block of 8 MB or
 *                more as it comes and goes.
 *   checking     NFSU2_HEAPCHECK=1 (test-only): tail canaries, poisoned and
 *                quarantined frees, double/invalid free and use-after-free
 *                write detection, freed VA regions made inaccessible.
 *                NFSU2_HEAPCHECK=2 also fills new blocks with 0xCD.
 *   escape hatch NFSU2_HEAP=bump: the old bump allocator, byte for byte.
 *
 * Block header (guest memory, the 16 bytes before the user pointer):
 *   +0  user size (HeapSize; overrides.c/com.c debug scanners read +0/+4)
 *   +4  magic: GH_LIVE (= the old bump header signature), GH_FREE, GH_QUAR
 *   +8  block size incl. header (multiple of 16) | F_* flags
 *   +12 previous physical block (its header address), 0 for the first
 *   free blocks also: +16 next free, +20 previous free (same size class)
 * Every segment ends in a 16-byte sentinel block (size 0, GH_SENT), so
 * coalescing never runs off the segment.
 *
 * All guest code runs under the GIL, but host threads (Metal, audio, timers)
 * may reach the COM allocators, so every entry point takes g_hl. */
#include "cpu.h"
#include "guest_span.h"
#include <os/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>

void mem_commit(uint32_t, uint32_t);
void mem_release(uint32_t, uint32_t, int);
void guest_backtrace(CPU *, int);
long autotest_frame(void);
void win32_set_last_error(CPU *, uint32_t);

#ifdef __wasm__
#define GH_BASE   0x08000000u          /* wasm: 512 MB below GUEST_END (cpu.h) */
#define GH_LIMIT  0x28000000u
#define GH_TOP    0x80000000u          /* the index goes on to 2 GB (whole words of g_va_bits): [GH_LIMIT, the window) is
                                          the runtime's, the window too where there is none, and the window's top to
                                          2 GB nobody's (va_holes) */
void mem_window_init(void);
#else
#define GH_BASE   0x10000000u
#define GH_LIMIT  0x60000000u          /* win32_ext.c: guest thread stacks from here up */
#define GH_TOP    GH_LIMIT
static const uint32_t g_gwin_lo = 0, g_gwin_hi = 0;
static inline void mem_window_init(void){ }
#endif
#define VA_GRAN   0x10000u
#define VA_NG     ((GH_TOP - GH_BASE) / VA_GRAN)
#define GPAGE     0x1000u
#define VA_NP     ((GH_TOP - GH_BASE) / GPAGE)
#define SEG_MIN   (4u << 20)           /* heap segment size */
#define LARGE_MIN (1u << 20)           /* blocks at least this big get a VA region */

#define BUMP_BASE 0x10000000u          /* NFSU2_HEAP=bump: the old arena */
#define BUMP_MAX  0x2F000000u

#define GH_LIVE   0xA110C8EDu
#define GH_FREE   0xF4EEB10Cu
#define GH_QUAR   0xDEADB10Cu
#define GH_SENT   0x5E9E4D00u
#define BUMP_FREED 0xFEEEFEEEu         /* bump mode: +8 of a block HeapFree saw */

#define F_FREE     1u
#define F_PREVFREE 2u
#define F_LARGE    4u
#define F_TRAIL    8u
#define HDR        16u
#define TRAIL      32u                  /* check/stat trailer: 16 canary + site, frame, fsite, fframe */
#define MINBLK     32u

#define B_USIZE(b) MR32((b))
#define B_MAGIC(b) MR32((b) + 4u)
#define B_SZF(b)   MR32((b) + 8u)
#define B_PREV(b)  MR32((b) + 12u)
#define B_NEXTF(b) MR32((b) + 16u)
#define B_PREVF(b) MR32((b) + 20u)
#define B_SIZE(b)  (B_SZF(b) & ~15u)

static os_unfair_lock g_hl = OS_UNFAIR_LOCK_INIT;
#define LOCK()   os_unfair_lock_lock(&g_hl)
#define UNLOCK() os_unfair_lock_unlock(&g_hl)

static int g_bump = -1;                /* NFSU2_HEAP=bump */
static int g_check = 0;                /* NFSU2_HEAPCHECK: 1 checks, 2 + fill */
static int g_sitestat = 0;             /* NFSU2_HEAPSTAT (implied by HEAPCHECK) */
static int g_trail = 0;                /* blocks carry the 32-byte trailer */

/* ---- statistics ---------------------------------------------------------- */
typedef struct {
    uint64_t nalloc, nfree, nrealloc, nfail, nbad;
    uint64_t live, peak, nlive;         /* user bytes / blocks */
    uint64_t held;                      /* VA bytes held (segments + large blocks) */
} HStat;
typedef struct { uint32_t size, site, ra, frame; int heap; } BigAlloc;
#define NBIG 16
static BigAlloc g_big[NBIG];
static uint64_t g_api_n[48];            /* per heap_api kind */
static uint64_t g_va_reserved, g_va_peak, g_va_commit_calls, g_va_release_calls;
static uint64_t g_ev_double, g_ev_invalid, g_ev_canary, g_ev_uaf, g_ev_mismatch, g_ev_corrupt;

/* ---- TLSF heaps ---------------------------------------------------------- */
#define FL_N 24
#define SL_N 16
typedef struct {
    int used, internal;
    const char *name;
    uint32_t flmap, slmap[FL_N], head[FL_N][SL_N];
    uint32_t spare;                     /* a segment all free, kept for the next growth (its one block) */
    HStat st;
} GHeap;
#define MAXHEAP 16
static GHeap g_heap[MAXHEAP];          /* index == guest handle; 0 unused */
int g_gh_com = 0, g_gh_gbuf = 0;       /* internal heaps (com.c) */

/* ---- VA manager ---------------------------------------------------------- */
enum { VK_NONE = 0, VK_VIRTUAL, VK_SEG, VK_LARGE };
typedef struct { uint32_t ng; uint8_t kind, heap; uint16_t pad; uint32_t prot, aprot, site; } VaReg;   /* site: a large block's caller */
static uint64_t g_va_bits[VA_NG / 64];  /* 1 = granule reserved */
static uint64_t g_va_dead[VA_NG / 64];  /* HEAPCHECK: released, not reused yet */
static uint32_t g_va_start[VA_NG];      /* granule -> first granule of its region */
static VaReg    g_va[VA_NG];            /* by first granule */
static uint8_t  g_va_pc[VA_NP / 8];     /* VK_VIRTUAL: 4 KB page committed */
static uint8_t  g_va_dirty[VA_NP / 8];  /* VK_VIRTUAL: page may hold old data (not a fresh zero page) */

static inline int vbit(const uint64_t *m, uint32_t g){ return (int)((m[g >> 6] >> (g & 63)) & 1u); }
static inline void vset(uint64_t *m, uint32_t g, int v){
    if (v) m[g >> 6] |= 1ull << (g & 63); else m[g >> 6] &= ~(1ull << (g & 63)); }
static inline int pcbit(uint32_t a){ uint32_t p = (a - GH_BASE) / GPAGE; return (g_va_pc[p >> 3] >> (p & 7)) & 1; }
static inline void pcset(uint32_t a, int v){ uint32_t p = (a - GH_BASE) / GPAGE;
    if (v) g_va_pc[p >> 3] |= (uint8_t)(1u << (p & 7)); else g_va_pc[p >> 3] &= (uint8_t)~(1u << (p & 7)); }
static inline int dbit(uint32_t a){ uint32_t p = (a - GH_BASE) / GPAGE; return (g_va_dirty[p >> 3] >> (p & 7)) & 1; }
/* a in the address space the VA manager hands out: [GH_BASE, GH_LIMIT), and the window where there is one */
static inline int va_managed(uint32_t a){ return (a >= GH_BASE && a < GH_LIMIT) || (g_gwin_hi && a >= g_gwin_lo && a < g_gwin_hi); }
static uint64_t va_managed_bytes(void){ return (uint64_t)(GH_LIMIT - GH_BASE) + (g_gwin_hi - g_gwin_lo); }
/* The index's granules no one gets (heap_init): the runtime's memory between the two ranges, and the window where
 * there is none.  Reserved, but no region's (g_va_start 0, never a region's first granule past 0) and not counted in
 * g_va_reserved, so every search and run count sees them as taken and every map leaves them out.  Without a window
 * the free granules are today's, so every search finds what it found before. */
static void va_holes(void){
    for (uint32_t a = GH_LIMIT; a < GH_TOP; a += VA_GRAN)
        if (!va_managed(a)) vset(g_va_bits, (a - GH_BASE) / VA_GRAN, 1);
}
static inline void dset(uint32_t a, int v){ uint32_t p = (a - GH_BASE) / GPAGE;
    if (v) g_va_dirty[p >> 3] |= (uint8_t)(1u << (p & 7)); else g_va_dirty[p >> 3] &= (uint8_t)~(1u << (p & 7)); }

/* Big blocks' holes (g_hint, the last NHINT, newest first): a block of
 * VA_BIG or more leaves its granules here when it goes, and they are kept for
 * the next big one (va_find).  A block placed in a hole takes its part of it;
 * the rest stays kept (a race track's smaller streamer block sits in free
 * roam's hole for the race, and free roam's comes back into the whole of it).
 * A hole is kept HINT_TTL_MS at most (its rest as long as the hole): one
 * whose block never comes back would push the smaller blocks into the other
 * free space for good, cutting the runs the next big blocks need.  The list
 * is in the order the holes were made (a rest has its hole's age), and when
 * all are kept the oldest goes. */
#define VA_BIG ((16u << 20) / VA_GRAN)
#define NHINT 8
#define HINT_TTL_MS 600000u             /* 10 minutes, longer than a race (the game's clock, win32.c) */
uint64_t host_us(void);
static uint64_t now_ms(void){ return host_us() / 1000u; }
static struct { uint32_t g0, ng; uint64_t made; } g_hint[NHINT];
static uint64_t g_hintmask[VA_NG / 64];
static void hint_mask(void){
    memset(g_hintmask, 0, sizeof g_hintmask);
    for (int i = 0; i < NHINT; i++)
        for (uint32_t g = g_hint[i].g0; g < g_hint[i].g0 + g_hint[i].ng; g++) vset(g_hintmask, g, 1);
}
static void hint_compact(void){                           /* the kept ones first, in their order */
    int j = 0;
    for (int i = 0; i < NHINT; i++) if (g_hint[i].ng) g_hint[j++] = g_hint[i];
    for (; j < NHINT; j++){ g_hint[j].g0 = g_hint[j].ng = 0; g_hint[j].made = 0; }
}
static void hint_put(uint32_t g0, uint32_t ng, uint64_t made){   /* by age, newest first; all kept: the oldest goes */
    hint_compact();
    int n = 0;
    while (n < NHINT && g_hint[n].ng) n++;
    if (n == NHINT){
        if (made < g_hint[NHINT - 1].made) return;       /* older than every kept one */
        n--;
    }
    int i = n;
    while (i > 0 && g_hint[i - 1].made <= made){ g_hint[i] = g_hint[i - 1]; i--; }
    g_hint[i].g0 = g0; g_hint[i].ng = ng; g_hint[i].made = made;
}
static void hint_add(uint32_t g0, uint32_t ng){ hint_put(g0, ng, now_ms()); hint_mask(); }
static void hint_drop(uint32_t g0, uint32_t ng){         /* [g0, g0+ng) taken */
    struct { uint32_t g0, ng; uint64_t made; } rest[2 * NHINT]; int n = 0, cut = 0;
    for (int i = 0; i < NHINT; i++){
        uint32_t h0 = g_hint[i].g0, h1 = h0 + g_hint[i].ng;
        if (!g_hint[i].ng || h0 >= g0 + ng || g0 >= h1) continue;
        g_hint[i].ng = 0; cut = 1;
        if (h0 < g0){ rest[n].g0 = h0; rest[n].ng = g0 - h0; rest[n].made = g_hint[i].made; n++; }
        if (g0 + ng < h1){ rest[n].g0 = g0 + ng; rest[n].ng = h1 - (g0 + ng); rest[n].made = g_hint[i].made; n++; }
    }
    if (!cut) return;
    for (int i = 0; i < n; i++) hint_put(rest[i].g0, rest[i].ng, rest[i].made);
    hint_compact();
    hint_mask();
}
static void hint_expire(void){
    uint64_t now = now_ms(); int gone = 0;
    for (int i = 0; i < NHINT; i++)
        if (g_hint[i].ng && now > g_hint[i].made && now - g_hint[i].made > HINT_TTL_MS){ g_hint[i].ng = 0; gone = 1; }
    if (gone){ hint_compact(); hint_mask(); }
}
/* A run of ng free granules: bottom-up first fit for heap segments, top-down
 * for VirtualAlloc and large blocks, so the two meet in the middle and the
 * free space between them stays one run.  The big blocks come and go with
 * every trip out of the world and back (the track streamer's 60000 KB, 43000
 * or 36000 KB by track, and the car pool's extra block: the game's main pool
 * grows by exactly what it lacks), and each needs a run that long again when
 * it comes back.  First fit had the 1.4 MB textures and new segments of a trip
 * cut the hole the streamer's block left, until its 60000 KB found no run
 * (2026-11-04 reports).  So smaller requests go past the big blocks' holes
 * while anything else fits, and a big request takes the shortest run that
 * holds it (best fit: its own hole), the highest of runs as short.  Under
 * HEAPCHECK released granules are skipped until nothing else fits, but not in
 * favour of a hole: once the released marks were cleared, a hole was clean and
 * the regions released since were not, and a new segment went into the
 * streamer's hole (test runs then stopped where the game would not). */
static int64_t va_find(uint32_t ng, int top, int allow_dead){
    int dead = !allow_dead && g_check;
    if (ng >= VA_BIG){
        int64_t fit = -1; uint32_t fitlen = 0, run = 0;
        for (uint32_t g = 0; g <= VA_NG; g++){
            int full = 0;                                   /* a word all taken: the runtime's memory between the ranges */
            if (g < VA_NG && !(g & 63)){
                uint64_t w = g_va_bits[g >> 6] | (dead ? g_va_dead[g >> 6] : 0ull);
                if (!w){ run += 64; g += 63; continue; }
                full = w == ~0ull;
            }
            if (!full && g < VA_NG && !vbit(g_va_bits, g) && !(dead && vbit(g_va_dead, g))){ run++; continue; }
            if (run >= ng && (fit < 0 || run < fitlen || (run == fitlen && top))){ fit = (int64_t)g - run; fitlen = run; }
            run = 0;
            if (full) g += 63;
        }
        if (fit < 0) return -1;
        return top ? fit + (int64_t)(fitlen - ng) : fit;
    }
    for (int pass = 0; pass < 3; pass++){                   /* past the big blocks' holes (HEAPCHECK: and the released
                                                               granules, then past the holes only), then anywhere */
        if (pass == 1 && !dead) continue;
        uint32_t run = 0;
#define VA_BLOCKED_W(g) (g_va_bits[(g) >> 6] | (dead && pass == 0 ? g_va_dead[(g) >> 6] : 0ull) | (pass < 2 ? g_hintmask[(g) >> 6] : 0ull))
        if (!top){
            for (uint32_t g = 0; g < VA_NG; g++){
                uint64_t w = VA_BLOCKED_W(g);
                if (!(g & 63) && w == ~0ull){ run = 0; g += 63; continue; }
                if ((w >> (g & 63)) & 1u) run = 0;
                else if (++run == ng) return (int64_t)(g - ng + 1);
            }
        } else {
            for (int64_t g = VA_NG - 1; g >= 0; g--){
                uint64_t w = VA_BLOCKED_W((uint32_t)g);
                if ((g & 63) == 63 && w == ~0ull){ run = 0; g -= 63; continue; }
                if ((w >> (g & 63)) & 1u) run = 0;
                else if (++run == ng) return g;
            }
        }
#undef VA_BLOCKED_W
    }
    return -1;
}
static void va_free_runs(uint32_t *free_g, uint32_t *longest_g, uint32_t *runs);
static int g_heapmap = 0;                 /* NFSU2_HEAPMAP=1 (test aid): the map with every [status] line */
static uint64_t g_win_va;                 /* NFSU2_HEAPMAP: since the last [status] line, VA's most */
static uint32_t g_win_run = ~0u;          /* ... and the longest free run's least (granules) */
static uint32_t g_win_run_lo = ~0u, g_win_run_hi = ~0u;   /* ... with the window: below the runtime, in the window */
/* The longest free run in granules [lo, hi) of the index. */
static uint32_t va_longest(uint32_t lo, uint32_t hi){
    uint32_t big = 0, run = 0;
    for (uint32_t g = lo; g < hi; g++){
        if (!(g & 63) && g + 64u <= hi){
            uint64_t w = g_va_bits[g >> 6];
            if (!w){ run += 64; g += 63; continue; }
            if (w == ~0ull){ if (run > big) big = run; run = 0; g += 63; continue; }
        }
        if (vbit(g_va_bits, g)){ if (run > big) big = run; run = 0; } else run++;
    }
    return run > big ? run : big;
}
#define VA_G_LIMIT ((GH_LIMIT - GH_BASE) / VA_GRAN)
static uint32_t va_g_of(uint32_t a){ return (a - GH_BASE) / VA_GRAN; }
static uint32_t va_take(uint32_t g0, uint32_t ng, int kind, int heap, uint32_t prot){
    for (uint32_t g = g0; g < g0 + ng; g++){ vset(g_va_bits, g, 1); vset(g_va_dead, g, 0); g_va_start[g] = g0; }
    g_va[g0] = (VaReg){ ng, (uint8_t)kind, (uint8_t)heap, 0, prot, prot, 0 };
    g_va_reserved += (uint64_t)ng * VA_GRAN;
    if (g_va_reserved > g_va_peak) g_va_peak = g_va_reserved;
    if (g_heapmap){
        uint32_t run; va_free_runs(0, &run, 0);
        if (g_va_reserved > g_win_va) g_win_va = g_va_reserved;
        if (run < g_win_run) g_win_run = run;
        if (g_gwin_hi){
            uint32_t lo = va_longest(0, VA_G_LIMIT), hi = va_longest(va_g_of(g_gwin_lo), va_g_of(g_gwin_hi));
            if (lo < g_win_run_lo) g_win_run_lo = lo;
            if (hi < g_win_run_hi) g_win_run_hi = hi;
        }
    }
    return GH_BASE + g0 * VA_GRAN;
}
/* Room for the heaps to grow: large blocks and VirtualAlloc's regions leave
 * the free runs room for SEG_KEEP more heap segments.  Since empty segments go
 * back (seg_free_whole), the 1.4 MB textures of a trip back could take the
 * space the heaps had held, and the small blocks that came next found no
 * 4 MB run for a new segment: random runs under pressure had several times as
 * many small allocations failing as when the heaps kept their segments, and a
 * failed small allocation (a list node, a string) is far likelier to bring the
 * game down than a failed texture buffer.  Not the big blocks (VA_BIG and up):
 * the track streamer's and the pools' are one of a kind, come back into their
 * own holes, and the game stops at once without them (in a test run the
 * streamer's block was refused its own hole: the room was that hole).
 * The room is what lies outside the kept holes (the streamer's 60 MB hole
 * counted as room for 14 segments while the space around it filled up), and
 * it is a last resort, not a wall: a block that finds no place keeping
 * SEG_KEEP segments' room takes all but SEG_LAST's, then a kept hole, and
 * fails only after that. */
#define SEG_G    (SEG_MIN / VA_GRAN)
#define SEG_KEEP 4u                     /* kept while anything else fits */
#define SEG_LAST 1u                     /* kept */
static uint32_t va_seg_room(uint32_t x0, uint32_t xn){   /* segments the free runs outside the holes hold, [x0, x0+xn) taken */
    uint32_t room = 0, run = 0;
    for (uint32_t g = 0; g <= VA_NG; g++){
        if (g < VA_NG && !(g & 63)){
            uint64_t w = g_va_bits[g >> 6] | g_hintmask[g >> 6];
            if (!w && (g + 64u <= x0 || g >= x0 + xn)){ run += 64; g += 63; continue; }
            if (w == ~0ull){ room += run / SEG_G; run = 0; g += 63; continue; }
        }
        if (g < VA_NG && !vbit(g_va_bits, g) && !vbit(g_hintmask, g) && (g < x0 || g >= x0 + xn)){ run++; continue; }
        room += run / SEG_G; run = 0;
    }
    return room;
}
/* Where ng granules go leaving keep segments' room, or costing none of it:
 * past the kept holes and (HEAPCHECK) the released granules, then past the
 * holes, then (holes_ok) anywhere; in a pass the top of the run that costs
 * the fewest segments, of those the shortest, the highest.  -1: none. */
static int64_t va_find_keep(uint32_t ng, uint32_t keep, int holes_ok){
    uint32_t room = va_seg_room(0, 0);
    int dead = g_check;
    for (int pass = 0; pass < 3; pass++){
        if ((pass == 1 && !dead) || (pass == 2 && !holes_ok)) continue;
        uint32_t run = 0, bcost = ~0u, blen = ~0u;
        int64_t best = -1;
        for (uint32_t g = 0; g <= VA_NG; g++){
            uint64_t w = g < VA_NG ? (g_va_bits[g >> 6] | (dead && pass == 0 ? g_va_dead[g >> 6] : 0ull)
                                      | (pass < 2 ? g_hintmask[g >> 6] : 0ull)) : ~0ull;
            int full = g < VA_NG && !(g & 63) && w == ~0ull;
            if (g < VA_NG && !(g & 63) && !w){ run += 64; g += 63; continue; }
            if (!full && g < VA_NG && !((w >> (g & 63)) & 1u)){ run++; continue; }
            if (run >= ng){
                int64_t at = (int64_t)g - ng;
                /* outside the holes the run is its own room; with holes the count is made again */
                uint32_t left = pass < 2 ? room - (run / SEG_G - (run - ng) / SEG_G) : va_seg_room((uint32_t)at, ng);
                uint32_t cost = room - left;
                if ((cost == 0 || left >= keep) && (cost < bcost || (cost == bcost && run <= blen))){ best = at; bcost = cost; blen = run; }
            }
            run = 0;
            if (full) g += 63;
        }
        if (best >= 0) return best;
    }
    return -1;
}
static uint32_t va_alloc(uint32_t bytes, int top, int kind, int heap, uint32_t prot){
    uint32_t ng = (uint32_t)(((uint64_t)bytes + VA_GRAN - 1) / VA_GRAN);
    if (!ng || ng > VA_NG) return 0;
    hint_expire();
    int64_t g0 = va_find(ng, top, 0);
    if (g0 < 0 && g_check){ memset(g_va_dead, 0, sizeof g_va_dead); g0 = va_find(ng, top, 1); }
    if (g0 >= 0 && kind != VK_SEG && ng < VA_BIG && va_seg_room((uint32_t)g0, ng) < SEG_KEEP){
        int64_t k = va_find_keep(ng, SEG_KEEP, 0);              /* elsewhere, the room kept */
        if (k < 0) k = va_find_keep(ng, SEG_LAST, 0);           /* all but the last segment's */
        if (k < 0) k = va_find_keep(ng, SEG_LAST, 1);           /* then a kept hole */
        g0 = k;
    }
    if (g0 < 0) return 0;
    hint_drop((uint32_t)g0, ng);
    return va_take((uint32_t)g0, ng, kind, heap, prot);
}
/* The free address space: granules in all, the longest run, how many runs. */
static void va_free_runs(uint32_t *free_g, uint32_t *longest_g, uint32_t *runs){
    uint32_t fr = 0, big = 0, n = 0, run = 0;
    for (uint32_t g = 0; g < VA_NG; g++){
        if (!(g & 63) && g + 64 <= VA_NG){
            uint64_t w = g_va_bits[g >> 6];
            if (!w){ run += 64; fr += 64; g += 63; continue; }
            if (w == ~0ull){ if (run){ n++; if (run > big) big = run; run = 0; } g += 63; continue; }
        }
        if (vbit(g_va_bits, g)){ if (run){ n++; if (run > big) big = run; run = 0; } }
        else { run++; fr++; }
    }
    if (run){ n++; if (run > big) big = run; }
    if (free_g) *free_g = fr;
    if (longest_g) *longest_g = big;
    if (runs) *runs = n;
}
/* Region containing a, or -1. */
static int64_t va_region(uint32_t a){
    if (!va_managed(a)) return -1;
    uint32_t g = (a - GH_BASE) / VA_GRAN;
    return vbit(g_va_bits, g) ? (int64_t)g_va_start[g] : -1;
}
static void va_free_region(uint32_t g0){
    uint32_t ng = g_va[g0].ng, a = GH_BASE + g0 * VA_GRAN, len = ng * VA_GRAN;
    if (ng >= VA_BIG) hint_add(g0, ng);                 /* a big block's hole, kept for the next big one */
    for (uint32_t g = g0; g < g0 + ng; g++){ vset(g_va_bits, g, 0); if (g_check) vset(g_va_dead, g, 1); }
    for (uint32_t p = a; p < a + len; p += GPAGE){ pcset(p, 0); dset(p, g_check != 0); }   /* HEAPCHECK: 0xDD, not zero */
    g_va[g0] = (VaReg){0};
    g_va_reserved -= (uint64_t)len;
    mem_release(a, len, !g_check);
}

/* ---- call sites ------------------------------------------------------------
 * The guest allocates through the CRT (malloc/new/calloc live in
 * [CRT_LO, CRT_HI)); the interesting site is the first return address on the
 * guest stack outside it.  Heuristic (the stack may hold stale values), but
 * good enough to attribute live bytes. */
#define TEXT_LO 0x00401000u
#define TEXT_HI 0x00782AF1u
#define CRT_LO  0x0075B000u
#define CRT_HI  0x00772000u
static uint32_t site_of(CPU *c){
    if (!c) return 0;
    uint32_t ra = c->eip;
    if (ra < CRT_LO || ra >= CRT_HI) return ra;
    for (uint32_t k = 0; k < 48; k++){
        uint32_t a = c->esp + 4u * k;
        if (!mem_is_committed(a) || !mem_is_committed(a + 3)) break;
        uint32_t v = MR32(a);
        if (v >= TEXT_LO && v < TEXT_HI && (v < CRT_LO || v >= CRT_HI)) return v;
    }
    return ra;
}
static uint32_t now_frame(void){ return (uint32_t)autotest_frame(); }

typedef struct { uint32_t site; int heap; uint64_t nalloc, nfree, live, nlive, peak, total; } SiteSt;
#define NSITE 8192
static SiteSt *g_site;
static SiteSt *site_get(uint32_t site, int heap){
    if (!g_site) g_site = calloc(NSITE, sizeof *g_site);
    uint32_t h = (site * 2654435761u ^ (uint32_t)heap * 40503u) & (NSITE - 1);
    for (uint32_t i = 0; i < NSITE; i++, h = (h + 1) & (NSITE - 1)){
        SiteSt *s = &g_site[h];
        if (!s->site && !s->nalloc){ s->site = site; s->heap = heap; return s; }
        if (s->site == site && s->heap == heap) return s;
    }
    return 0;
}
static void site_alloc(uint32_t site, int heap, uint32_t n){
    if (!g_sitestat) return;
    SiteSt *s = site_get(site, heap); if (!s) return;
    s->nalloc++; s->nlive++; s->live += n; s->total += n;
    if (s->live > s->peak) s->peak = s->live;
}
static void site_free(uint32_t site, int heap, uint32_t n){
    if (!g_sitestat) return;
    SiteSt *s = site_get(site, heap); if (!s) return;
    s->nfree++; if (s->nlive) s->nlive--; s->live = s->live >= n ? s->live - n : 0;
}
static void big_note(uint32_t n, uint32_t site, uint32_t ra, int heap){
    int lo = 0;
    for (int i = 1; i < NBIG; i++) if (g_big[i].size < g_big[lo].size) lo = i;
    if (n > g_big[lo].size) g_big[lo] = (BigAlloc){ n, site, ra, now_frame(), heap };
}
static void st_alloc(GHeap *H, uint32_t n){
    H->st.nalloc++; H->st.live += n; H->st.nlive++;
    if (H->st.live > H->st.peak) H->st.peak = H->st.live;
}
static void st_free(GHeap *H, uint32_t n){
    H->st.nfree++; H->st.live = H->st.live >= n ? H->st.live - n : 0; if (H->st.nlive) H->st.nlive--;
}

/* ---- HEAPCHECK reports ---------------------------------------------------- */
static void check_report(CPU *c, const char *what, uint32_t p, uint32_t b, const char *detail){
    static int shown = 0;
    uint32_t asite = 0, aframe = 0, fsite = 0, fframe = 0;
    if (b && (B_SZF(b) & F_TRAIL) && mem_is_committed(b + 15)){
        uint32_t t = b + HDR + ((B_USIZE(b) + 15u) & ~15u) + 16u;
        if (mem_is_committed(t + 15)){ asite = MR32(t); aframe = MR32(t + 4); fsite = MR32(t + 8); fframe = MR32(t + 12); }
    }
    fprintf(stderr, "[heapcheck] %s: ptr 0x%08x (frame %u, caller 0x%08x)%s%s"
                    " -- allocated at 0x%08x (frame %u), freed at 0x%08x (frame %u)\n",
            what, p, now_frame(), c ? c->eip : 0u, detail ? ", " : "", detail ? detail : "",
            asite, aframe, fsite, fframe);
    if (c && shown < 24){ shown++; guest_backtrace(c, 8); }
}

/* ---- TLSF core ------------------------------------------------------------ */
static inline int fls32(uint32_t x){ return 31 - __builtin_clz(x); }
static void map_ins(uint32_t s, int *fl, int *sl){
    if (s < 256u){ *fl = 0; *sl = (int)(s >> 4); }
    else { int f = fls32(s); *sl = (int)((s >> (f - 4)) ^ 16u); *fl = f - 7; }
}
static void map_search(uint32_t s, int *fl, int *sl){
    if (s >= 256u) s += (1u << (fls32(s) - 4)) - 1u;
    map_ins(s, fl, sl);
}
static void fl_insert(GHeap *H, uint32_t b, uint32_t size){
    int fl, sl; map_ins(size, &fl, &sl);
    if (fl >= FL_N){ fl = FL_N - 1; sl = SL_N - 1; }
    uint32_t nx = H->head[fl][sl];
    B_NEXTF(b) = nx; B_PREVF(b) = 0;
    if (nx) B_PREVF(nx) = b;
    H->head[fl][sl] = b;
    H->flmap |= 1u << fl; H->slmap[fl] |= 1u << sl;
}
static void fl_remove(GHeap *H, uint32_t b, uint32_t size){
    int fl, sl; map_ins(size, &fl, &sl);
    if (fl >= FL_N){ fl = FL_N - 1; sl = SL_N - 1; }
    uint32_t nx = B_NEXTF(b), pv = B_PREVF(b);
    if (nx) B_PREVF(nx) = pv;
    if (pv) B_NEXTF(pv) = nx;
    else if (H->head[fl][sl] == b){
        H->head[fl][sl] = nx;
        if (!nx){ H->slmap[fl] &= ~(1u << sl); if (!H->slmap[fl]) H->flmap &= ~(1u << fl); }
    }
}
static uint32_t fl_find(GHeap *H, uint32_t size){
    int fl, sl; map_search(size, &fl, &sl);
    if (fl >= FL_N) return 0;
    uint32_t m = H->slmap[fl] & (~0u << sl);
    if (!m){
        uint32_t f = fl + 1 < 32 ? H->flmap & (~0u << (fl + 1)) : 0;
        if (!f) return 0;
        fl = __builtin_ctz(f); m = H->slmap[fl];
    }
    sl = __builtin_ctz(m);
    return H->head[fl][sl];
}
/* New segment for heap hi with room for one block of `need` bytes. */
static int seg_grow(int hi, uint32_t need){
    GHeap *H = &g_heap[hi];
    uint32_t len = need + 2u * HDR;
    len = len < SEG_MIN ? SEG_MIN : (len + VA_GRAN - 1) & ~(VA_GRAN - 1);
    uint32_t a = va_alloc(len, 0, VK_SEG, hi, 4u);
    if (!a) return 0;
    mem_commit(a, len);
    H->st.held += len;
    uint32_t b = a, s = a + len - HDR;
    B_USIZE(b) = 0; B_MAGIC(b) = GH_FREE; B_SZF(b) = (len - HDR) | F_FREE; B_PREV(b) = 0;
    B_USIZE(s) = 0; B_MAGIC(s) = GH_SENT; B_SZF(s) = F_PREVFREE; B_PREV(s) = b;
    fl_insert(H, b, len - HDR);
    return 1;
}
/* Segment b (its first block, now free and the whole segment, size bytes up
 * to its sentinel) is all free again: one such segment stays with its heap
 * (H->spare, still in the free lists, for the next growth), any other goes
 * back to the VA manager, so a heap holds address space for what it holds now
 * rather than for its busiest moment: the COM buffers' heap kept the segments
 * of every texture of a trip back into a crowded free roam (the world's and
 * the friends' cars' textures coming in while the front end's go) for good.
 * The spare is the lowest of them: segments grow from the bottom, and one left
 * in the middle of the free space would cut its longest run in two.
 * 1 = given back (the caller does not list it). */
static void seg_release(GHeap *H, uint32_t size, uint32_t g0){
    H->st.held -= (uint64_t)size + HDR;
    va_free_region(g0);
}
static int seg_free_whole(GHeap *H, uint32_t b, uint32_t size){
    int64_t g0 = va_region(b);
    if (g0 < 0 || GH_BASE + (uint32_t)g0 * VA_GRAN != b || g_va[g0].kind != VK_SEG
        || (uint64_t)g_va[g0].ng * VA_GRAN != (uint64_t)size + HDR) return 0;
    if (!H->spare || H->spare == b){ H->spare = b; return 0; }
    if (b < H->spare){                                    /* the lower one stays */
        uint32_t o = H->spare, os = B_SIZE(o);
        fl_remove(H, o, os);
        H->spare = b;
        seg_release(H, os, (o - GH_BASE) / VA_GRAN);
        return 0;
    }
    seg_release(H, size, (uint32_t)g0);
    return 1;
}
static uint32_t need_of(uint32_t n){
    uint64_t need = ((uint64_t)n + 15u) & ~15ull;
    need += HDR + (g_trail ? TRAIL : 0u);
    if (need < MINBLK) need = MINBLK;
    return need > 0x7FFF0000ull ? 0 : (uint32_t)need;
}
static void trail_init(uint32_t b, uint32_t n, uint32_t site){
    uint32_t r16 = (n + 15u) & ~15u, u = b + HDR, t = u + r16;
    if (g_check) memset(g_mem + u + n, 0xFD, r16 - n + 16u);
    MR32(t + 16) = site; MR32(t + 20) = now_frame(); MR32(t + 24) = 0; MR32(t + 28) = 0;
}
static int trail_ok(uint32_t b){
    uint32_t n = B_USIZE(b), r16 = (n + 15u) & ~15u, u = b + HDR;
    const uint8_t *q = g_mem + u + n;
    for (uint32_t i = 0; i < r16 - n + 16u; i++) if (q[i] != 0xFD) return 0;
    return 1;
}
static uint32_t trail_site(uint32_t b){
    return (B_SZF(b) & F_TRAIL) ? MR32(b + HDR + ((B_USIZE(b) + 15u) & ~15u) + 16u) : 0u;
}

/* What holds the address space, one line (under the lock): each heap's
 * segments (and what of them is in use) and large blocks, VirtualAlloc's
 * regions, and the free space with its longest run; then every large block of
 * 8 MB or more with the call site that made it (and its frame, with
 * NFSU2_HEAPSTAT's trailers), and the smaller ones by call site. */
static void va_map(const char *why){
    uint32_t fr, big, nr; va_free_runs(&fr, &big, &nr);
    uint64_t seg[MAXHEAP] = {0}, lrg[MAXHEAP] = {0}, lrglive[MAXHEAP] = {0}, vb = 0, vc = 0;
    uint32_t nseg[MAXHEAP] = {0}, nlrg[MAXHEAP] = {0}, nv = 0;
    for (uint32_t g = 0; g < VA_NG; g++){
        if (!vbit(g_va_bits, g) || g_va_start[g] != g) continue;
        VaReg *R = &g_va[g]; uint64_t len = (uint64_t)R->ng * VA_GRAN; uint32_t a = GH_BASE + g * VA_GRAN;
        if (R->kind == VK_SEG){ seg[R->heap] += len; nseg[R->heap]++; }
        else if (R->kind == VK_LARGE){ lrg[R->heap] += len; nlrg[R->heap]++; lrglive[R->heap] += B_USIZE(a); }
        else { nv++; vb += len; for (uint64_t p = 0; p < len; p += GPAGE) vc += pcbit(a + (uint32_t)p) ? GPAGE : 0; }
        g += R->ng - 1;
    }
    char buf[1024];
    int k = snprintf(buf, sizeof buf, "[heap] %s: VA %.1f of %.0f MB reserved, %.1f MB free in %u runs (longest %.1f MB);",
                     why, g_va_reserved / 1048576.0, va_managed_bytes() / 1048576.0, fr * (VA_GRAN / 1048576.0), nr,
                     big * (VA_GRAN / 1048576.0));
    if (g_gwin_hi)
        k += snprintf(buf + k, sizeof buf - (size_t)k, " below the runtime longest %.1f MB, in the window %.1f MB;",
                      va_longest(0, VA_G_LIMIT) * (VA_GRAN / 1048576.0),
                      va_longest(va_g_of(g_gwin_lo), va_g_of(g_gwin_hi)) * (VA_GRAN / 1048576.0));
    for (int i = 1; i < MAXHEAP && k < (int)sizeof buf; i++){
        if (!nseg[i] && !nlrg[i]) continue;
        GHeap *H = &g_heap[i];
        uint64_t seglive = H->st.live >= lrglive[i] ? H->st.live - lrglive[i] : 0;
        k += snprintf(buf + k, sizeof buf - (size_t)k, " %s(%d): %u segments %.1f MB (%.1f MB in use), %u large %.1f MB;",
                      H->name ? H->name : "?", i, nseg[i], seg[i] / 1048576.0, seglive / 1048576.0, nlrg[i], lrg[i] / 1048576.0);
    }
    if (k < (int)sizeof buf)
        k += snprintf(buf + k, sizeof buf - (size_t)k, " VirtualAlloc: %u regions %.1f MB (%.1f MB committed);", nv, vb / 1048576.0,
                      vc / 1048576.0);
    uint32_t hn = 0, hg = 0;
    for (int i = 0; i < NHINT; i++) if (g_hint[i].ng){ hn++; hg += g_hint[i].ng; }
    if (k < (int)sizeof buf)
        k += snprintf(buf + k, sizeof buf - (size_t)k, " %u holes kept %.1f MB;", hn, hg * (VA_GRAN / 1048576.0));
    uint32_t rs[6] = {0}, rl[6] = {0}, run = 0;                  /* the widest free runs: where they are */
    for (uint32_t g = 0; g <= VA_NG; g++){
        if (g < VA_NG && !vbit(g_va_bits, g)){ run++; continue; }
        for (int i = 0; run && i < 6; i++) if (run > rl[i]){
            memmove(rl + i + 1, rl + i, (5u - (unsigned)i) * 4u); memmove(rs + i + 1, rs + i, (5u - (unsigned)i) * 4u);
            rl[i] = run; rs[i] = GH_BASE + (g - run) * VA_GRAN; break; }
        run = 0;
    }
    for (int i = 0; i < 6 && rl[i] && k < (int)sizeof buf; i++)
        k += snprintf(buf + k, sizeof buf - (size_t)k, " %.1f@%08x", rl[i] * (VA_GRAN / 1048576.0), rs[i]);
    fprintf(stderr, "%s\n", buf);
    struct { uint32_t site, n; int heap; uint64_t bytes; } grp[24]; int ng = 0;
    for (uint32_t g = 0; g < VA_NG; g++){                       /* the large blocks and who made them */
        if (!vbit(g_va_bits, g) || g_va_start[g] != g) continue;
        VaReg *R = &g_va[g]; uint32_t a = GH_BASE + g * VA_GRAN, len = R->ng * VA_GRAN;
        g += R->ng - 1;
        if (R->kind != VK_LARGE) continue;
        if (len >= (8u << 20)){
            fprintf(stderr, "[heap]   0x%08x %6.1f MB  heap %d  site 0x%08x  frame %u\n", a, len / 1048576.0, R->heap,
                    R->site, (B_SZF(a) & F_TRAIL) ? MR32(a + HDR + ((B_USIZE(a) + 15u) & ~15u) + 20u) : 0u);
            continue;
        }
        uint32_t s = R->site; int i;
        for (i = 0; i < ng && (grp[i].site != s || grp[i].heap != R->heap); i++) ;
        if (i == ng){ if (ng == 24) continue; grp[ng++] = (typeof(grp[0])){ s, 0, R->heap, 0 }; }
        grp[i].n++; grp[i].bytes += len;
    }
    for (int j = 0; j < 8; j++){                                /* the smaller ones by site, most first */
        int b = -1;
        for (int i = 0; i < ng; i++) if (grp[i].n && (b < 0 || grp[i].bytes > grp[b].bytes)) b = i;
        if (b < 0) break;
        fprintf(stderr, "[heap]   %u blocks %6.1f MB  heap %d  site 0x%08x\n", grp[b].n, grp[b].bytes / 1048576.0, grp[b].heap, grp[b].site);
        grp[b].n = 0;
    }
}
/* An allocation found no room: the map, the first times and then now and then. */
static void va_full(int hi, uint32_t need){
    static unsigned n = 0;
    if (n++ >= 6 && (n & 63)) return;
    char why[96];
    snprintf(why, sizeof why, "no room for %u bytes of the %s heap (%u times)", need, g_heap[hi].name ? g_heap[hi].name : "?", n);
    va_map(why);
}

static uint32_t tlsf_alloc(int hi, uint32_t n, int zero, uint32_t site, uint32_t ra){
    GHeap *H = &g_heap[hi];
    uint32_t need = need_of(n);
    if (!need){ H->st.nfail++; return 0; }
    uint32_t b;
    if (need >= LARGE_MIN){
        uint32_t a = va_alloc(need, 1, VK_LARGE, hi, 4u);
        if (!a){ H->st.nfail++; va_full(hi, need); return 0; }
        uint32_t len = g_va[(a - GH_BASE) / VA_GRAN].ng * VA_GRAN;
        g_va[(a - GH_BASE) / VA_GRAN].site = site;
        mem_commit(a, len);
        H->st.held += len;
        if (g_heapmap && len >= (8u << 20))
            fprintf(stderr, "[heap] f=%u +%.1f MB at 0x%08x (%s heap, site 0x%08x)\n", now_frame(), len / 1048576.0, a,
                    H->name ? H->name : "?", site);
        b = a;
        B_SZF(b) = len | F_LARGE; B_PREV(b) = 0;
    } else {
        b = fl_find(H, need);
        if (!b){ if (!seg_grow(hi, need) || !(b = fl_find(H, need))){ H->st.nfail++; va_full(hi, need); return 0; } }
        if (b == H->spare) H->spare = 0;        /* the spare segment is in use again */
        uint32_t size = B_SIZE(b);
        fl_remove(H, b, size);
        uint32_t nx = b + size;
        if (size - need >= MINBLK){
            uint32_t r = b + need, rs = size - need;
            B_USIZE(r) = 0; B_MAGIC(r) = GH_FREE; B_SZF(r) = rs | F_FREE; B_PREV(r) = b;
            B_PREV(nx) = r;                     /* nx keeps F_PREVFREE */
            fl_insert(H, r, rs);
            size = need;
        } else B_SZF(nx) &= ~F_PREVFREE;
        B_SZF(b) = size;                        /* a free block's predecessor is never free */
    }
    if (g_trail) B_SZF(b) |= F_TRAIL;
    B_MAGIC(b) = GH_LIVE; B_USIZE(b) = n;
    uint32_t u = b + HDR;
    if (zero) memset(g_mem + u, 0, n);
    else if (g_check >= 2) memset(g_mem + u, 0xCD, n);
    if (g_trail) trail_init(b, n, site);
    st_alloc(H, n); site_alloc(site, hi, n);
    if (n >= 256u * 1024u) big_note(n, site, ra, hi);
    return u;
}
/* Real free of a block (already validated). */
static void tlsf_release(int hi, uint32_t b){
    GHeap *H = &g_heap[hi];
    if (B_SZF(b) & F_LARGE){
        uint32_t len = B_SIZE(b);
        H->st.held -= len;
        if (g_heapmap && len >= (8u << 20))
            fprintf(stderr, "[heap] f=%u -%.1f MB at 0x%08x (%s heap)\n", now_frame(), len / 1048576.0, b, H->name ? H->name : "?");
        va_free_region((b - GH_BASE) / VA_GRAN);
        return;
    }
    uint32_t size = B_SIZE(b), nx = b + size;
    if ((B_SZF(nx) & F_FREE) && B_MAGIC(nx) == GH_FREE){
        uint32_t ns = B_SIZE(nx);
        fl_remove(H, nx, ns); size += ns; nx = b + size;
    } else if (B_MAGIC(nx) != GH_LIVE && B_MAGIC(nx) != GH_SENT && B_MAGIC(nx) != GH_QUAR){
        g_ev_corrupt++;
        if (g_ev_corrupt <= 8) fprintf(stderr, "[heap] corrupt block after 0x%08x (magic %08x): not coalescing\n", b + HDR, B_MAGIC(nx));
    }
    if (B_SZF(b) & F_PREVFREE){
        uint32_t pv = B_PREV(b);
        if (pv && B_MAGIC(pv) == GH_FREE && pv + B_SIZE(pv) == b){
            uint32_t ps = B_SIZE(pv);
            fl_remove(H, pv, ps);
            /* its header stays inside the free block before it: it must not
             * say live, or a second free of the same pointer would be taken
             * (and the block handed out twice) and HeapSize/HeapValidate
             * would call it live */
            B_MAGIC(b) = GH_FREE;
            b = pv; size += ps;
        } else { g_ev_corrupt++;
            if (g_ev_corrupt <= 8) fprintf(stderr, "[heap] corrupt prev link at 0x%08x: not coalescing\n", b + HDR); }
    }
    B_USIZE(b) = 0; B_MAGIC(b) = GH_FREE; B_SZF(b) = size | F_FREE;
    B_PREV(nx) = b; B_SZF(nx) |= F_PREVFREE;
    if (!B_PREV(b) && B_MAGIC(nx) == GH_SENT && seg_free_whole(H, b, size)) return;   /* its whole segment free */
    fl_insert(H, b, size);
}

/* ---- HEAPCHECK quarantine ------------------------------------------------- */
#define QN 65536u
static uint32_t g_q[QN], g_qheap[QN], g_qsize[QN];
static uint32_t g_qh, g_qn;
static uint64_t g_qbytes, g_qcap = 64ull << 20;
static void q_evict(void){
    uint32_t b = g_q[g_qh]; int hi = (int)g_qheap[g_qh];
    g_qbytes -= g_qsize[g_qh];
    g_qh = (g_qh + 1) % QN; g_qn--;
    if (!hi) return;                            /* its heap was destroyed (memory released) */
    uint32_t n = B_USIZE(b);
    const uint32_t *w = (const uint32_t *)(g_mem + b + HDR);
    for (uint32_t i = 0; i < n / 4u; i++)
        if (w[i] != 0xDDDDDDDDu){
            char d[96]; snprintf(d, sizeof d, "offset +%u now 0x%08x (block %u bytes)", i * 4u, w[i], n);
            g_ev_uaf++; check_report(0, "WRITE AFTER FREE", b + HDR, b, d);
            break;
        }
    if (!trail_ok(b)){ g_ev_canary++; check_report(0, "CANARY overwritten after free", b + HDR, b, 0); }
    tlsf_release(hi, b);
}
static void q_push(int hi, uint32_t b){
    while (g_qn && (g_qn >= QN || g_qbytes + B_SIZE(b) > g_qcap)) q_evict();
    g_q[(g_qh + g_qn) % QN] = b; g_qheap[(g_qh + g_qn) % QN] = (uint32_t)hi; g_qsize[(g_qh + g_qn) % QN] = B_SIZE(b);
    g_qn++; g_qbytes += B_SIZE(b);
}

/* Validate a user pointer: returns its header and owning heap, or 0. */
static uint32_t block_check(uint32_t p, int *hi){
    if ((p & 15u) || p < GH_BASE + HDR || p >= GH_TOP) return 0;
    uint32_t b = p - HDR;
    int64_t g0 = va_region(b);
    if (g0 < 0) return 0;
    VaReg *R = &g_va[g0];
    if (R->kind != VK_SEG && R->kind != VK_LARGE) return 0;
    if (R->kind == VK_LARGE && b != GH_BASE + (uint32_t)g0 * VA_GRAN) return 0;
    *hi = R->heap;
    return b;
}

static int tlsf_free(CPU *c, int hi_req, uint32_t p, uint32_t ra){
    int hi = 0;
    uint32_t b = block_check(p, &hi);
    if (!b){ g_ev_invalid++; g_heap[hi_req].st.nbad++;
        if (g_check || g_ev_invalid <= 4) check_report(c, "INVALID FREE (not a heap block)", p, 0, 0);
        return 0; }
    uint32_t mg = B_MAGIC(b);
    if (mg != GH_LIVE){                         /* refused: the first ones, then one in 256 (the guest's site) */
        char d[80];
        snprintf(d, sizeof d, "site 0x%08x, %s heap", c ? site_of(c) : ra, g_heap[hi].name ? g_heap[hi].name : "?");
        if (mg == GH_FREE || mg == GH_QUAR){ g_ev_double++;
            if (g_check || g_ev_double <= 8 || !(g_ev_double & 255)) check_report(c, "DOUBLE FREE", p, b, d); }
        else { g_ev_invalid++;
            if (g_check || g_ev_invalid <= 8 || !(g_ev_invalid & 255)) check_report(c, "INVALID FREE (bad header)", p, 0, d); }
        g_heap[hi].st.nbad++;
        return 0;
    }
    if (hi != hi_req && hi_req > 0 && !g_heap[hi_req].internal){
        g_ev_mismatch++;            /* freed through another heap handle: Windows corrupts, we cope */
        if (g_ev_mismatch <= 4) fprintf(stderr, "[heap] block 0x%08x of heap %d freed via heap %d (caller 0x%08x)\n", p, hi, hi_req, ra);
    }
    GHeap *H = &g_heap[hi];
    uint32_t n = B_USIZE(b);
    st_free(H, n);
    site_free((B_SZF(b) & F_TRAIL) ? trail_site(b) : 0u, hi, n);
    if (g_check && (B_SZF(b) & F_TRAIL)){
        if (!trail_ok(b)){ g_ev_canary++; check_report(c, "HEAP OVERRUN (tail canary)", p, b, 0); }
        uint32_t t = b + HDR + ((n + 15u) & ~15u) + 16u;
        MR32(t + 8) = c ? site_of(c) : ra; MR32(t + 12) = now_frame();
        if (!(B_SZF(b) & F_LARGE)){
            memset(g_mem + p, 0xDD, n);
            B_MAGIC(b) = GH_QUAR;
            q_push(hi, b);
            return 1;
        }
    }
    tlsf_release(hi, b);
    return 1;
}
static uint32_t tlsf_usize(uint32_t p){
    int hi; uint32_t b = block_check(p, &hi);
    return (b && B_MAGIC(b) == GH_LIVE) ? B_USIZE(b) : 0xFFFFFFFFu;
}
/* In-place resize when possible (never with a trailer: it would have to move). */
static uint32_t tlsf_realloc(CPU *c, int hi_req, uint32_t p, uint32_t n, int zero, int inplace_only, uint32_t site, uint32_t ra){
    if (!p) return inplace_only ? 0 : tlsf_alloc(hi_req, n, zero, site, ra);
    int hi = 0; uint32_t b = block_check(p, &hi);
    if (!b || B_MAGIC(b) != GH_LIVE){
        g_ev_invalid++; if (g_check || g_ev_invalid <= 4) check_report(c, "INVALID REALLOC", p, 0, 0);
        return 0; }
    GHeap *H = &g_heap[hi];
    uint32_t old = B_USIZE(b), need = need_of(n);
    if (!need) return 0;
    H->st.nrealloc++;
    if (!g_trail && !(B_SZF(b) & F_LARGE) && need < LARGE_MIN){
        uint32_t size = B_SIZE(b), nx = b + size;
        if (need > size && (B_SZF(nx) & F_FREE) && B_MAGIC(nx) == GH_FREE && size + B_SIZE(nx) >= need){
            uint32_t ns = B_SIZE(nx);
            fl_remove(H, nx, ns); size += ns; nx = b + size;
            B_SZF(nx) &= ~F_PREVFREE; B_PREV(nx) = b;
            B_SZF(b) = size | (B_SZF(b) & F_PREVFREE);
        }
        if (need <= size){
            if (size - need >= MINBLK){              /* give the tail back */
                uint32_t r = b + need;
                B_USIZE(r) = 0; B_MAGIC(r) = GH_LIVE; B_SZF(r) = size - need; B_PREV(r) = b;
                B_PREV(nx) = r;
                B_SZF(b) = need | (B_SZF(b) & F_PREVFREE);
                tlsf_release(hi, r);
            }
            B_USIZE(b) = n;
            H->st.live = H->st.live - old + n;
            if (H->st.live > H->st.peak) H->st.peak = H->st.live;
            if (zero && n > old) memset(g_mem + p + old, 0, n - old);
            return p;
        }
    } else if ((B_SZF(b) & F_LARGE) && !g_trail && need <= B_SIZE(b)){
        B_USIZE(b) = n;
        H->st.live = H->st.live - old + n;
        if (H->st.live > H->st.peak) H->st.peak = H->st.live;
        if (zero && n > old) memset(g_mem + p + old, 0, n - old);
        return p;
    }
    if (inplace_only) return 0;
    uint32_t q = tlsf_alloc(hi, n, 0, site, ra);
    if (!q) return 0;
    memcpy(g_mem + q, g_mem + p, old < n ? old : n);
    if (zero && n > old) memset(g_mem + q + old, 0, n - old);
    H->st.nalloc--; H->st.nfree--;            /* count the move as one realloc */
    tlsf_free(c, hi, p, ra);
    return q;
}

/* ---- bump mode (the old allocator) --------------------------------------- */
static uint32_t g_bump_ptr = BUMP_BASE;
static uint64_t g_bump_fail = 0;
static uint32_t bump_alloc(int hi, uint32_t n, int zero, uint32_t site, uint32_t ra){
    uint32_t need = (n + HDR + 15u) & ~15u;
    if (g_bump_ptr + need > BUMP_MAX || need < n){ g_bump_fail++; g_heap[hi].st.nfail++; return 0; }
    uint32_t p = g_bump_ptr; g_bump_ptr += need;
    mem_commit(p, need);
    MR32(p) = n; MR32(p + 4) = GH_LIVE; MR32(p + 8) = 0; MR32(p + 12) = site;
    uint32_t u = p + HDR;
    if (zero) memset(g_mem + u, 0, n);
    st_alloc(&g_heap[hi], n); site_alloc(site, hi, n);
    if (n >= 256u * 1024u) big_note(n, site, ra, hi);
    return u;
}
static uint32_t bump_usize(uint32_t u){
    if (!u || u < BUMP_BASE + HDR || u >= BUMP_MAX) return 0;
    if (MR32(u - HDR + 4) != GH_LIVE) return 0;
    return MR32(u - HDR);
}
/* Nothing is reused; frees are only counted (live = what a real heap would hold). */
static int bump_free(int hi, uint32_t u){
    uint32_t n = bump_usize(u);
    if (!u) return 1;
    if (!n && MR32(u - HDR + 4) != GH_LIVE){ g_heap[hi].st.nbad++; return 1; }
    if (MR32(u - HDR + 8) == BUMP_FREED){ g_ev_double++; return 1; }
    MR32(u - HDR + 8) = BUMP_FREED;
    st_free(&g_heap[hi], n); site_free(MR32(u - HDR + 12), hi, n);
    return 1;
}
static uint32_t bump_realloc(int hi, uint32_t o, uint32_t ns, int zero, uint32_t site, uint32_t ra){
    uint32_t os = bump_usize(o);
    uint32_t p = bump_alloc(hi, ns, zero, site, ra);
    if (p && o){ uint32_t cp = os && os < ns ? os : ns; memcpy(g_mem + p, g_mem + o, cp); }
    g_heap[hi].st.nrealloc++;
    if (o) bump_free(hi, o);
    return p;
}

/* ---- init ----------------------------------------------------------------- */
/* Guest heaps count up from 1 (process heap) so the CRT's HeapCreate gets 2,
 * as it always did; internal heaps take the top slots. */
static int heap_new(const char *name, int internal){
    for (int k = 1; k < MAXHEAP; k++){
        int i = internal ? MAXHEAP - k : k;
        if (g_heap[i].used) continue;
        memset(&g_heap[i], 0, sizeof g_heap[i]);
        g_heap[i].used = 1; g_heap[i].internal = internal; g_heap[i].name = name;
        return i;
    }
    return 0;
}
int gheap_bump(void){ return g_bump; }
void heap_init(void){
    if (g_bump >= 0) return;
    const char *m = getenv("NFSU2_HEAP");
    g_bump = (m && !strcmp(m, "bump")) ? 1 : 0;
    const char *hc = getenv("NFSU2_HEAPCHECK");
    g_check = (hc && *hc && *hc != '0') ? atoi(hc) : 0;
    if (g_check < 0) g_check = 1;
    const char *hs = getenv("NFSU2_HEAPSTAT");
    g_sitestat = g_check || (hs && *hs && *hs != '0');
    if (g_bump) g_check = 0;
    g_trail = !g_bump && (g_check || g_sitestat);
    const char *hm = getenv("NFSU2_HEAPMAP"), *scr = getenv("NFSU2_INPUT_SCRIPT");   /* test runs only */
    g_heapmap = !g_bump && hm && *hm && *hm != '0' && scr && *scr;
    const char *qc = getenv("NFSU2_HEAPCHECK_QUAR");
    if (qc && *qc) g_qcap = (uint64_t)strtoull(qc, 0, 0) << 20;
    if (!g_bump){ mem_window_init(); va_holes(); }
    heap_new("process", 0);                     /* handle 1 = GetProcessHeap */
    if (!g_bump){ g_gh_com = heap_new("com objects", 1); g_gh_gbuf = heap_new("com buffers", 1); }
    else mem_commit(BUMP_BASE, 0x10000);
    fprintf(stderr, "heap: %s%s%s, region 0x%08x..0x%08x",
            g_bump ? "bump allocator (NFSU2_HEAP=bump)" : "TLSF heaps + VA manager",
            g_check ? (g_check >= 2 ? ", HEAPCHECK=2 (fill)" : ", HEAPCHECK") : "",
            g_sitestat ? ", per-site stats" : "",
            g_bump ? BUMP_BASE : GH_BASE, g_bump ? BUMP_MAX : GH_LIMIT);
    if (!g_bump && g_gwin_hi) fprintf(stderr, " and 0x%08x..0x%08x", g_gwin_lo, g_gwin_hi);
    fprintf(stderr, "\n");
}

/* ---- runtime API ---------------------------------------------------------- */
uint32_t gheap_alloc_c(CPU *c, int heap, uint32_t n, int zero){
    if (g_bump < 0) heap_init();
    uint32_t site = site_of(c), ra = c ? c->eip : 0u, r;
    LOCK();
    if (heap <= 0 || heap >= MAXHEAP || !g_heap[heap].used) heap = 1;
    r = g_bump ? bump_alloc(heap, n, zero, site, ra) : tlsf_alloc(heap, n, zero, site, ra);
    UNLOCK();
    return r;
}
uint32_t gheap_alloc(int heap, uint32_t n, int zero){ return gheap_alloc_c(0, heap, n, zero); }
void gheap_free_c(CPU *c, int heap, uint32_t p){
    if (!p) return;
    LOCK();
    if (g_bump) bump_free(heap > 0 ? heap : 1, p);
    else tlsf_free(c, heap, p, c ? c->eip : 0u);
    UNLOCK();
}
void gheap_free(int heap, uint32_t p){ gheap_free_c(0, heap, p); }
uint32_t gheap_usize(uint32_t p){
    LOCK();
    uint32_t r = g_bump ? bump_usize(p) : tlsf_usize(p);
    UNLOCK();
    return r == 0xFFFFFFFFu ? 0 : r;
}
/* Scratch areas for a buffer the heap cannot give where the game writes into
 * it unchecked: 0x5CE170 converts a palettized texture into the buffer it
 * mallocs (0x5CE1E4) and copies that to the texture, and the Lock callers
 * write at the Lock's pointer at once (com.c lock_scratch).  With 0 they
 * wrote up to megabytes at guest address 0 and on: below the game's image
 * (0x400000) nothing lives in the guest's memory, but past 4 MB they reached
 * the image, and a NULL pointer's fields read texels there, not zeros.  Two
 * areas below the image, past its first 64 KB: the texture buffer's (a
 * 512x512 texture with its mips needs 1398016 bytes) and the Locks'. */
#define SCR_TEMP      0x00010000u
#define SCR_TEMP_N    0x00170000u
#define SCR_LOCK      (SCR_TEMP + SCR_TEMP_N)
#define SCR_LOCK_N    (0x00400000u - SCR_LOCK)
#define SCR_TEMP_SITE 0x005ce1e9u     /* after 0x5CE170's malloc */
static uint64_t g_scr_temp;
uint32_t gheap_scratch(int lock, uint32_t n){
    static int committed;
    if (n > (lock ? SCR_LOCK_N : SCR_TEMP_N)) return 0;
    if (!committed){ committed = 1; mem_commit(SCR_TEMP, 0x00400000u - SCR_TEMP); }
    return lock ? SCR_LOCK : SCR_TEMP;
}
/* The big blocks' holes kept now, newest first (web/test/heap_va_unit.c). */
int gheap_kept_holes(uint32_t *addr, uint32_t *len, int max){
    LOCK();
    hint_expire();
    int n = 0;
    for (int i = 0; i < NHINT && n < max; i++)
        if (g_hint[i].ng){ addr[n] = GH_BASE + g_hint[i].g0 * VA_GRAN; len[n] = g_hint[i].ng * VA_GRAN; n++; }
    UNLOCK();
    return n;
}
/* HEAPCHECK: verify one block's tail canary now (com.c, after UnlockRect). */
int gheap_check_block(CPU *c, uint32_t p, const char *what){
    if (!g_check || !p) return 1;
    LOCK();
    int hi; uint32_t b = block_check(p, &hi), ok = 1;
    if (b && B_MAGIC(b) == GH_LIVE && (B_SZF(b) & F_TRAIL) && !trail_ok(b)){
        ok = 0; g_ev_canary++;
        check_report(c, what, p, b, 0);
        memset(g_mem + p + B_USIZE(b), 0xFD, ((B_USIZE(b) + 15u) & ~15u) - B_USIZE(b) + 16u);   /* report once */
    }
    UNLOCK();
    return (int)ok;
}

/* ---- Win32 API ------------------------------------------------------------ */
enum { HK_NONE = 0, HK_GetProcessHeap, HK_HeapCreate, HK_HeapDestroy, HK_HeapAlloc, HK_HeapFree,
       HK_HeapReAlloc, HK_HeapSize, HK_HeapValidate, HK_HeapCompact, HK_HeapLock, HK_HeapUnlock,
       HK_GlobalAlloc, HK_GlobalFree, HK_GlobalReAlloc, HK_GlobalSize, HK_GlobalLock, HK_GlobalUnlock,
       HK_GlobalHandle, HK_GlobalFlags, HK_LocalAlloc, HK_LocalFree, HK_LocalReAlloc, HK_LocalSize,
       HK_LocalLock, HK_LocalUnlock, HK_LocalHandle, HK_LocalFlags, HK_VirtualAlloc, HK_VirtualFree,
       HK_VirtualQuery, HK_VirtualProtect, HK__count };
static const char *const g_hk_name[HK__count] = {
    0, "GetProcessHeap", "HeapCreate", "HeapDestroy", "HeapAlloc", "HeapFree",
    "HeapReAlloc", "HeapSize", "HeapValidate", "HeapCompact", "HeapLock", "HeapUnlock",
    "GlobalAlloc", "GlobalFree", "GlobalReAlloc", "GlobalSize", "GlobalLock", "GlobalUnlock",
    "GlobalHandle", "GlobalFlags", "LocalAlloc", "LocalFree", "LocalReAlloc", "LocalSize",
    "LocalLock", "LocalUnlock", "LocalHandle", "LocalFlags", "VirtualAlloc", "VirtualFree",
    "VirtualQuery", "VirtualProtect" };
int heap_api_kind(const char *name){
    for (int k = 1; k < HK__count; k++) if (!strcmp(name, g_hk_name[k])) return k;
    return 0;
}
static uint32_t ARG(CPU *c, int i){ return MR32(c->esp + 4u * (uint32_t)i); }

#define MEM_COMMIT   0x1000u
#define MEM_RESERVE  0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE  0x8000u
#define MEM_FREE     0x10000u
#define MEM_RESET    0x80000u
#define MEM_PRIVATE  0x20000u
#define MEM_IMAGE    0x1000000u

/* VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect) */
static uint32_t virtual_alloc(CPU *c, uint32_t addr, uint32_t size, uint32_t type, uint32_t prot){
    if (!size){ win32_set_last_error(c, 87); return 0; }
    if (type & MEM_RESET) return addr;                       /* contents may be discarded: keep them */
    uint32_t base;
    if (!addr || (type & MEM_RESERVE)){
        if (addr){                                           /* reserve at a fixed address */
            uint32_t a0 = addr & ~(VA_GRAN - 1);
            uint64_t end = (uint64_t)addr + size;
            uint32_t ng = (uint32_t)((end - a0 + VA_GRAN - 1) / VA_GRAN);
            if (a0 < GH_BASE || end > GH_TOP){ win32_set_last_error(c, 487); return 0; }   /* (the holes: reserved) */
            uint32_t g0 = (a0 - GH_BASE) / VA_GRAN;
            for (uint32_t g = g0; g < g0 + ng; g++) if (vbit(g_va_bits, g)){ win32_set_last_error(c, 487); return 0; }
            hint_drop(g0, ng);
            base = va_take(g0, ng, VK_VIRTUAL, 0, prot);
        } else base = va_alloc(size, 1, VK_VIRTUAL, 0, prot);
        if (!base){ win32_set_last_error(c, 8); return 0; }
        if (!(type & MEM_COMMIT)) return base;
        if (addr) size = (uint32_t)((uint64_t)addr + size - base);   /* commit from the base up to addr+size */
        addr = base;
    }
    /* commit [addr, addr+size) inside a VirtualAlloc region */
    int64_t g0 = va_region(addr);
    if (g0 < 0 || g_va[g0].kind != VK_VIRTUAL){ win32_set_last_error(c, 487); return 0; }
    uint32_t rbase = GH_BASE + (uint32_t)g0 * VA_GRAN, rend = rbase + g_va[g0].ng * VA_GRAN;
    uint32_t lo = addr & ~(GPAGE - 1);
    uint64_t hi = ((uint64_t)addr + size + GPAGE - 1) & ~(uint64_t)(GPAGE - 1);
    if (hi > rend){ win32_set_last_error(c, 487); return 0; }
    g_va_commit_calls++;
    mem_commit(lo, (uint32_t)(hi - lo));
    for (uint32_t p = lo; p < hi; p += GPAGE)
        if (!pcbit(p)){                                      /* newly committed pages read as zero */
            if (dbit(p)) memset(g_mem + p, 0, GPAGE);
            pcset(p, 1); dset(p, 1); }
    return lo;
}
static uint32_t virtual_free(CPU *c, uint32_t addr, uint32_t size, uint32_t type){
    int64_t g0 = va_region(addr);
    if (g0 < 0 || g_va[g0].kind != VK_VIRTUAL){ win32_set_last_error(c, 487); return 0; }
    uint32_t rbase = GH_BASE + (uint32_t)g0 * VA_GRAN, len = g_va[g0].ng * VA_GRAN;
    if (type & MEM_RELEASE){
        if (size || addr != rbase){ win32_set_last_error(c, 87); return 0; }
        g_va_release_calls++;
        va_free_region((uint32_t)g0);
        return 1;
    }
    if (type & MEM_DECOMMIT){
        uint32_t lo = addr & ~(GPAGE - 1);
        uint64_t hi = size ? (((uint64_t)addr + size + GPAGE - 1) & ~(uint64_t)(GPAGE - 1)) : (uint64_t)rbase + len;
        if (!size && addr != rbase){ win32_set_last_error(c, 87); return 0; }
        if (hi > (uint64_t)rbase + len) hi = (uint64_t)rbase + len;
        uint32_t rlo = (lo + 0x3FFFu) & ~0x3FFFu, rhi = (uint32_t)hi & ~0x3FFFu;   /* whole host pages */
        for (uint32_t p = lo; p < hi; p += GPAGE){ pcset(p, 0); if (p >= rlo && p < rhi) dset(p, g_check != 0); }
        mem_release(lo, (uint32_t)(hi - lo), !g_check);
        return 1;
    }
    win32_set_last_error(c, 87);
    return 0;
}
/* MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect,
 * RegionSize, State, Protect, Type (28 bytes). */
static uint32_t virtual_query(uint32_t addr, uint32_t mbi, uint32_t len){
    if (!mbi || len < 28u) return 0;
    uint32_t pg = addr & ~(GPAGE - 1), abase = 0, aprot = 0, rsize, state, prot = 0, type = 0;
    int64_t g0 = va_region(addr);
    if (g0 >= 0){
        VaReg *R = &g_va[g0];
        uint32_t rbase = GH_BASE + (uint32_t)g0 * VA_GRAN, rend = rbase + R->ng * VA_GRAN;
        abase = rbase; aprot = R->aprot; type = MEM_PRIVATE;
        int com = R->kind != VK_VIRTUAL || pcbit(pg);
        uint32_t e = pg;
        while (e < rend && (R->kind != VK_VIRTUAL || pcbit(e) == com)) e += GPAGE;
        rsize = e - pg; state = com ? MEM_COMMIT : MEM_RESERVE; prot = com ? R->prot : 0;
    } else if (!g_bump && va_managed(pg)){
        /* unreserved granules of the VA manager are free (released pages
         * stay host-committed, zero-filled) */
        uint32_t e = (pg + VA_GRAN) & ~(VA_GRAN - 1);
        while (va_managed(e) && va_region(e) < 0) e += VA_GRAN;
        rsize = e - pg; state = MEM_FREE; prot = 1u;
    } else if (mem_is_committed(pg)){
        uint32_t e = (pg + 0x4000u) & ~0x3FFFu;
        while (e && mem_is_committed(e) && va_region(e) < 0) e += 0x4000u;
        int img = pg >= 0x00400000u && pg < 0x00932000u;
        abase = img ? 0x00400000u : (pg & ~0x3FFFu); aprot = img ? 0x80u : 4u;
        rsize = e - pg; state = MEM_COMMIT; prot = img ? 0x40u : 4u; type = img ? MEM_IMAGE : MEM_PRIVATE;
    } else {
        uint32_t e = (pg + 0x4000u) & ~0x3FFFu;
        while (e && !mem_is_committed(e) && va_region(e) < 0 && e - pg < 0x10000000u) e += 0x4000u;
        rsize = e - pg; state = MEM_FREE; prot = 1u;
    }
    mem_commit(mbi, 28);
    MR32(mbi) = pg; MR32(mbi + 4) = abase; MR32(mbi + 8) = aprot; MR32(mbi + 12) = rsize;
    MR32(mbi + 16) = state; MR32(mbi + 20) = prot; MR32(mbi + 24) = type;
    return 28;
}

/* Bump mode keeps the old semantics of every call exactly (VirtualAlloc =
 * a fresh zeroed block whatever the flags, VirtualFree/GlobalFree no-ops). */
uint32_t heap_api(CPU *c, int k){
    if (g_bump < 0) heap_init();
    uint32_t r = 0, site = site_of(c), ra = c->eip;
    LOCK();
    if (k > 0 && k < 48) g_api_n[k]++;
    switch (k){
    case HK_GetProcessHeap: r = 1; break;
    case HK_HeapCreate:
        r = g_bump ? 2u : (uint32_t)heap_new("HeapCreate", 0);
        if (!r) win32_set_last_error(c, 8);
        break;
    case HK_HeapDestroy: {
        int h = (int)ARG(c, 0); r = 1;
        if (g_bump || h <= 1 || h >= MAXHEAP || !g_heap[h].used || g_heap[h].internal) break;
        for (uint32_t g = 0; g < VA_NG; g++)
            if (vbit(g_va_bits, g) && g_va_start[g] == g && g_va[g].heap == h
                && (g_va[g].kind == VK_SEG || g_va[g].kind == VK_LARGE)) va_free_region(g);
        if (g_check){                               /* drop its quarantined blocks */
            for (uint32_t i = 0; i < g_qn; i++) if ((int)g_qheap[(g_qh + i) % QN] == h) g_qheap[(g_qh + i) % QN] = 0;
        }
        fprintf(stderr, "[heap] HeapDestroy(%d): %llu allocs, %llu live bytes dropped\n", h,
                (unsigned long long)g_heap[h].st.nalloc, (unsigned long long)g_heap[h].st.live);
        g_heap[h].used = 0;
        break; }
    case HK_HeapAlloc: {
        int h = (int)ARG(c, 0); uint32_t fl = ARG(c, 1), n = ARG(c, 2);
        if (h <= 0 || h >= MAXHEAP || !g_heap[h].used || g_bump) h = 1;   /* bump: one arena */
        r = g_bump ? bump_alloc(h, n, fl & 8u, site, ra) : tlsf_alloc(h, n, fl & 8u, site, ra);
        if (!r){
            static int sh = 0;
            if (sh++ < 8) fprintf(stderr, "[heap] HeapAlloc(%d, 0x%x, %u) FAILED (caller 0x%08x, site 0x%08x)%s\n",
                                  h, fl, n, ra, site, (fl & 4u) ? " -- HEAP_GENERATE_EXCEPTIONS" : "");
            if (site == SCR_TEMP_SITE && (r = gheap_scratch(0, n))){       /* 0x5CE170's texture buffer */
                g_scr_temp++;
                if (g_scr_temp <= 8 || !(g_scr_temp & 255))
                    fprintf(stderr, "[heap] the texture's buffer in the scratch area instead (%llu times)\n", (unsigned long long)g_scr_temp);
            } else win32_set_last_error(c, 8);
        }
        break; }
    case HK_HeapFree: {
        int h = (int)ARG(c, 0); uint32_t p = ARG(c, 2);
        if (!p || p == SCR_TEMP){ r = 1; break; }              /* (the scratch area: 0x5CE170's free of it) */
        if (h <= 0 || h >= MAXHEAP || g_bump) h = 1;
        r = g_bump ? (uint32_t)bump_free(h, p) : (uint32_t)tlsf_free(c, h, p, ra);
        if (!r) win32_set_last_error(c, 87);
        break; }
    case HK_HeapReAlloc: {
        int h = (int)ARG(c, 0); uint32_t fl = ARG(c, 1), p = ARG(c, 2), n = ARG(c, 3);
        if (h <= 0 || h >= MAXHEAP || !g_heap[h].used || g_bump) h = 1;
        if (g_bump) r = bump_realloc(h, p, n, fl & 8u, site, ra);
        else r = tlsf_realloc(c, h, p, n, fl & 8u, fl & 0x10u, site, ra);
        if (!r) win32_set_last_error(c, 8);
        break; }
    case HK_HeapSize: {
        uint32_t p = ARG(c, 2);
        r = g_bump ? bump_usize(p) : tlsf_usize(p);
        break; }
    case HK_HeapValidate: {
        uint32_t p = ARG(c, 2); int hi;
        r = g_bump ? 1u : (!p ? 1u : (uint32_t)(block_check(p, &hi) && B_MAGIC(p - HDR) == GH_LIVE));
        break; }
    case HK_HeapCompact: r = 1u << 20; break;
    case HK_HeapLock: case HK_HeapUnlock: r = 1; break;
    case HK_GlobalAlloc: case HK_LocalAlloc: {
        uint32_t fl = ARG(c, 0), n = ARG(c, 1);
        r = g_bump ? bump_alloc(1, n, fl & 0x40u, site, ra) : tlsf_alloc(1, n, fl & 0x40u, site, ra);
        if (!r) win32_set_last_error(c, 8);
        break; }
    case HK_GlobalFree: case HK_LocalFree: {
        uint32_t p = ARG(c, 0);
        if (g_bump){ if (p) bump_free(1, p); r = 0; break; }
        r = (!p || tlsf_free(c, 1, p, ra)) ? 0u : p;           /* NULL on success */
        break; }
    case HK_GlobalReAlloc: case HK_LocalReAlloc: {
        uint32_t p = ARG(c, 0), n = ARG(c, 1), fl = ARG(c, 2);
        if (fl & 0x80u){ r = p; break; }                           /* GMEM_MODIFY: flags only */
        if (g_bump) r = bump_realloc(1, p, n, fl & 0x40u, site, ra);
        else r = tlsf_realloc(c, 1, p, n, fl & 0x40u, !(fl & 0x2u), site, ra);   /* fixed blocks move only with GMEM_MOVEABLE */
        break; }
    case HK_GlobalSize: case HK_LocalSize: {
        uint32_t p = ARG(c, 0);
        r = g_bump ? bump_usize(p) : tlsf_usize(p);
        if (r == 0xFFFFFFFFu) r = 0;
        break; }
    case HK_GlobalLock: case HK_LocalLock: case HK_GlobalHandle: case HK_LocalHandle: r = ARG(c, 0); break;
    case HK_GlobalUnlock: case HK_LocalUnlock: r = 1; break;
    case HK_GlobalFlags: case HK_LocalFlags: r = 0; break;
    case HK_VirtualAlloc: {
        uint32_t a = ARG(c, 0), n = ARG(c, 1), ty = ARG(c, 2), pr = ARG(c, 3);
        if (g_bump){ r = bump_alloc(1, n, 1, site, ra); break; }
        r = virtual_alloc(c, a, n, ty, pr);
        break; }
    case HK_VirtualFree: {
        uint32_t a = ARG(c, 0), n = ARG(c, 1), ty = ARG(c, 2);
        if (g_bump){ if (ty & MEM_RELEASE) bump_free(1, a); r = 1; break; }
        r = virtual_free(c, a, n, ty);
        break; }
    case HK_VirtualQuery:
        r = virtual_query(ARG(c, 0), ARG(c, 1), ARG(c, 2));
        break;
    case HK_VirtualProtect: {
        /* (lpAddress, dwSize, flNewProtect, lpflOldProtect): guest memory stays
         * RW (execute is metadata only); remember the value for VirtualQuery. */
        uint32_t a = ARG(c, 0), np = ARG(c, 2), po = ARG(c, 3), old = 0x40u;
        int64_t g0 = g_bump ? -1 : va_region(a);
        if (g0 >= 0){ old = g_va[g0].prot; g_va[g0].prot = np; }
        else if (!(a >= 0x00400000u && a < 0x00932000u)) old = 4u;
        if (po && mem_is_committed(po) && mem_is_committed(po + 3)) MR32(po) = old;
        r = 1;
        break; }
    default: r = 0; break;
    }
    UNLOCK();
    return r;
}

/* ---- reports ---------------------------------------------------------------- */
static uint64_t host_footprint(void){
    task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS) return 0;
    return vi.phys_footprint;
}
void heap_totals(uint64_t *live, uint64_t *peak, uint64_t *va, uint64_t *held){
    uint64_t l = 0, p = 0, h = 0;
    for (int i = 1; i < MAXHEAP; i++) if (g_heap[i].used && !g_heap[i].internal){ l += g_heap[i].st.live; p += g_heap[i].st.peak; h += g_heap[i].st.held; }
    if (live) *live = l;
    if (peak) *peak = p;
    if (held) *held = h;
    if (va) *va = g_bump ? (uint64_t)(g_bump_ptr - BUMP_BASE) : g_va_reserved;
}
uint64_t heap_internal_live(int i){ return (i > 0 && i < MAXHEAP) ? g_heap[i].st.live : 0; }
uint64_t heap_internal_nlive(int i){ return (i > 0 && i < MAXHEAP) ? g_heap[i].st.nlive : 0; }

/* One status-line fragment for autotest.c ([status] ... mem: ...). */
void com_mem_stats(uint64_t *com_bytes, uint64_t *buf_bytes, uint64_t *objs);
void metal_mem_stats(uint64_t *alloc_bytes, unsigned *live_tex, unsigned *tex_slots, uint64_t *tex_bytes);
int  win32_obj_live(void);
int  fileio_open_handles(void);
int  win32_threads_live(void);
int  memstat_line(char *buf, size_t n){
    uint64_t live, peak, va, held, cb = 0, bb = 0, objs = 0, ma = 0, mt = 0; unsigned lt = 0, ts = 0;
    uint32_t run = 0;
    LOCK(); heap_totals(&live, &peak, &va, &held);
    if (!g_bump) va_free_runs(0, &run, 0);
    if (g_heapmap){
        va_map("status");
        if (g_win_va){                              /* between two [status] lines (a burst the lines miss) */
            fprintf(stderr, "[heap] since the last status: VA at most %.1f MB, the longest free run at least %.1f MB",
                    g_win_va / 1048576.0, g_win_run * (VA_GRAN / 1048576.0));
            if (g_gwin_hi && g_win_run_hi != ~0u)
                fprintf(stderr, " (below the runtime %.1f MB, in the window %.1f MB)",
                        g_win_run_lo * (VA_GRAN / 1048576.0), g_win_run_hi * (VA_GRAN / 1048576.0));
            fprintf(stderr, "\n");
        }
        g_win_va = g_va_reserved; g_win_run = run;
        if (g_gwin_hi){ g_win_run_lo = va_longest(0, VA_G_LIMIT); g_win_run_hi = va_longest(va_g_of(g_gwin_lo), va_g_of(g_gwin_hi)); }
    }
    UNLOCK();
    com_mem_stats(&cb, &bb, &objs);
    metal_mem_stats(&ma, &lt, &ts, &mt);
    /* vrun: the longest free run of the address space (what one large block can have) */
    return snprintf(buf, n, " foot=%lluMB heap=%.1f/%.1fMB va=%lluMB vrun=%uMB com=%lluKB/%llu gbuf=%.1fMB"
                            " mtl=%lluMB mtx=%lluMB tex=%u/%u obj=%d fh=%d thr=%d",
                    (unsigned long long)(host_footprint() >> 20), live / 1048576.0, peak / 1048576.0,
                    (unsigned long long)(va >> 20), (unsigned)(((uint64_t)run * VA_GRAN) >> 20), (unsigned long long)(cb >> 10),
                    (unsigned long long)objs,
                    bb / 1048576.0, (unsigned long long)(ma >> 20), (unsigned long long)(mt >> 20), lt, ts,
                    win32_obj_live(), fileio_open_handles(), win32_threads_live());
}

void heap_report(void){
    LOCK();
    fprintf(stderr, "\n=== GUEST HEAP (%s) ===\n", g_bump ? "bump" : "TLSF");
    for (int i = 1; i < MAXHEAP; i++){
        GHeap *H = &g_heap[i];
        if (!H->used && !H->st.nalloc) continue;
        fprintf(stderr, "  heap %-2d %-12s: %llu alloc, %llu free, %llu realloc, %llu failed, %llu bad;"
                        " live %.2f MB in %llu blocks, peak %.2f MB, VA held %.1f MB\n",
                i, H->name ? H->name : "?", (unsigned long long)H->st.nalloc, (unsigned long long)H->st.nfree,
                (unsigned long long)H->st.nrealloc, (unsigned long long)H->st.nfail, (unsigned long long)H->st.nbad,
                H->st.live / 1048576.0, (unsigned long long)H->st.nlive, H->st.peak / 1048576.0, H->st.held / 1048576.0);
    }
    if (g_bump) fprintf(stderr, "  bump arena: %.1f MB of %.1f MB used, %llu failed\n",
                        (g_bump_ptr - BUMP_BASE) / 1048576.0, (BUMP_MAX - BUMP_BASE) / 1048576.0,
                        (unsigned long long)g_bump_fail);
    else {
        uint32_t nv = 0, ns = 0, nl = 0; uint64_t bv = 0, bvc = 0;
        for (uint32_t g = 0; g < VA_NG; g++) if (vbit(g_va_bits, g) && g_va_start[g] == g){
            if (g_va[g].kind == VK_VIRTUAL){ nv++; bv += (uint64_t)g_va[g].ng * VA_GRAN;
                for (uint32_t p = 0; p < g_va[g].ng * VA_GRAN; p += GPAGE) bvc += pcbit(GH_BASE + g * VA_GRAN + p) ? GPAGE : 0; }
            else if (g_va[g].kind == VK_SEG) ns++; else nl++; }
        fprintf(stderr, "  VA: %.1f MB reserved now (peak %.1f MB of %.1f MB): %u heap segments, %u large blocks,"
                        " %u VirtualAlloc regions (%.1f MB, %.1f MB committed); %llu commit / %llu release calls\n",
                g_va_reserved / 1048576.0, g_va_peak / 1048576.0, va_managed_bytes() / 1048576.0, ns, nl, nv,
                bv / 1048576.0, bvc / 1048576.0, (unsigned long long)g_va_commit_calls, (unsigned long long)g_va_release_calls);
    }
    fprintf(stderr, "  API calls:");
    for (int k = 1; k < HK__count; k++) if (g_api_n[k]) fprintf(stderr, " %s=%llu", g_hk_name[k], (unsigned long long)g_api_n[k]);
    fprintf(stderr, "\n  events: %llu double free, %llu invalid free, %llu heap mismatch, %llu canary, %llu write-after-free, %llu corrupt links\n",
            (unsigned long long)g_ev_double, (unsigned long long)g_ev_invalid, (unsigned long long)g_ev_mismatch,
            (unsigned long long)g_ev_canary, (unsigned long long)g_ev_uaf, (unsigned long long)g_ev_corrupt);
    if (g_check){
        /* blocks still in quarantine were never checked: check them now */
        uint64_t uaf0 = g_ev_uaf, can0 = g_ev_canary;
        for (uint32_t i = 0; i < g_qn; i++){
            uint32_t b = g_q[(g_qh + i) % QN], n = B_USIZE(b);
            if (!g_qheap[(g_qh + i) % QN] || !mem_is_committed(b) || B_MAGIC(b) != GH_QUAR) continue;
            const uint32_t *w = (const uint32_t *)(g_mem + b + HDR);
            for (uint32_t k = 0; k < n / 4u; k++)
                if (w[k] != 0xDDDDDDDDu){
                    char d[96]; snprintf(d, sizeof d, "offset +%u now 0x%08x (block %u bytes)", k * 4u, w[k], n);
                    g_ev_uaf++; check_report(0, "WRITE AFTER FREE (exit sweep)", b + HDR, b, d); break; }
            if (!trail_ok(b)){ g_ev_canary++; check_report(0, "CANARY overwritten after free (exit sweep)", b + HDR, b, 0); }
        }
        fprintf(stderr, "  HEAPCHECK quarantine: %u blocks, %.1f MB (cap %.0f MB); exit sweep: %llu write-after-free, %llu canary\n",
                g_qn, g_qbytes / 1048576.0, g_qcap / 1048576.0,
                (unsigned long long)(g_ev_uaf - uaf0), (unsigned long long)(g_ev_canary - can0));
    }
    fprintf(stderr, "  largest allocations:\n");
    for (int k = 0; k < NBIG; k++){
        int b = -1;
        for (int i = 0; i < NBIG; i++) if (g_big[i].size && (b < 0 || g_big[i].size > g_big[b].size)) b = i;
        if (b < 0) break;
        fprintf(stderr, "    %10u B  heap %d  site 0x%08x (caller 0x%08x)  frame %u\n",
                g_big[b].size, g_big[b].heap, g_big[b].site, g_big[b].ra, g_big[b].frame);
        g_big[b].size = 0;
    }
    if (g_site){
        fprintf(stderr, "  live bytes by call site (top 30):\n");
        for (int k = 0; k < 30; k++){
            int b = -1;
            for (int i = 0; i < NSITE; i++) if (g_site[i].nalloc && g_site[i].site != 0xFFFFFFFFu
                                                 && (b < 0 || g_site[i].live > g_site[b].live)) b = i;
            if (b < 0 || !g_site[b].live) break;
            SiteSt *s = &g_site[b];
            fprintf(stderr, "    site 0x%08x heap %d: live %10llu B in %7llu blocks (peak %llu B), %llu alloc / %llu free, %llu B total\n",
                    s->site, s->heap, (unsigned long long)s->live, (unsigned long long)s->nlive,
                    (unsigned long long)s->peak, (unsigned long long)s->nalloc, (unsigned long long)s->nfree,
                    (unsigned long long)s->total);
            s->site = 0xFFFFFFFFu;
        }
    }
    UNLOCK();
}
