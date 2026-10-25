/* modheap.c -- the heaps a lifted DLL creates (server.dll's CRT heap).
 *
 * Written when the runtime's guest heap was a bump allocator (HeapFree did
 * nothing): one host session allocates ~28 MB at startup (128 sessions'
 * buffers) and every lobby message more, so the 496 MB arena ran out after
 * ~16 host sessions (measured, srvcycle).  The game's heaps are now heap.c's
 * real allocator; HeapCreate called by a lifted DLL still gets its own heap
 * here (win32.c routes it, and every Heap* call on such a handle, before
 * heap.c), so a host session never mixes with the game's blocks.  Guest
 * address map: server.dll's image 0x00A00000-0x00A58000 (module.c), the
 * per-thread hostent buffers 0x00BE0000, TEBs from 0x00C00000, these heaps
 * 0x01000000-0x0F000000 (2 x 112 MB, committed on demand), the ASIPEER test
 * build's plugin 0x0F000000-0x10000000 (roam_asipeer.c), heap.c's heaps and
 * VirtualAlloc 0x10000000-0x60000000, guest thread stacks from 0x60000000:
 *
 *   - blocks have a 16-byte header (requested size, state, capacity) and are
 *     16-byte aligned; capacities up to 4 KB come in 16-byte classes with a
 *     free list each, larger ones in 4 KB steps with one free list, reused
 *     when the capacity fits within +25 %;
 *   - HeapFree really frees, HeapReAlloc grows in place when the capacity
 *     allows (HEAP_REALLOC_IN_PLACE_ONLY honoured), HEAP_ZERO_MEMORY zeroes;
 *   - HeapDestroy (the DLL's CRT on DLL_PROCESS_DETACH) empties the heap: the
 *     next host session starts from the bottom of the same pages.
 *
 * Every call runs with the guest lock held. */
#include "cpu.h"
#include "memmap.h"
#include "module.h"
#include <stdio.h>
#include <string.h>

void mem_commit(uint32_t, uint32_t);

/* The modheap region (runtime/memmap.h).  wasm: the guest span is 640 MB
 * (cpu.h) and its main stack starts at 0x05000000, so two heaps of 32 MB
 * each: 0x01000000-0x05000000; native: two of 112 MB.  A host session peaks
 * at 22.6 MB (Mac, four players, eight races in a row), and HeapDestroy
 * empties a heap per session.
 *
 * Each lifted DLL has its own part of the region (game.h modules: heap_base,
 * heap_size, heap_n): a HeapCreate the DLL calls takes a free heap of its
 * own part, never another DLL's.  UG2's server.dll has the whole region. */
#define MH_BASE   MM_MODHEAP_BASE
#define MH_SIZE   MM_MODHEAP_SIZE
#define MH_N      MM_MODHEAP_N
#define MH_MAX    8                      /* heaps of all modules together */
#define HDR       16u
#define LIVE      0x5EB7A110u
#define FREED     0x5EB7F4EEu
#define NSMALL    257                    /* classes 16..4096 bytes */
#define COMMIT    0x00100000u

typedef struct {
    int used;
    uint32_t base, size, top, committed;
    uint32_t small[NSMALL];              /* free list heads (user pointers) */
    uint32_t large;                      /* free list of blocks > 4 KB */
    uint64_t live, peak, allocs, frees, created;
    int bad;                             /* double/foreign frees seen */
} MHeap;
static MHeap g_h[MH_MAX];

static MHeap *heap_of(uint32_t h){
    for (int i = 0; i < MH_MAX; i++) if (g_h[i].used && g_h[i].base == h) return &g_h[i];
    return 0;
}
int modheap_owns(uint32_t h){ return h - MH_BASE < MH_N * MH_SIZE && heap_of(h) != 0; }

/* HeapCreate called by module mod (win32.c): a free heap of its own part.
 * Module m's heaps are g_h[] slots from the sum of the modules' heap_n
 * before it. */
uint32_t modheap_create(int mod){
    const ModLink *l = module_link(mod);
    if (!l || l->heap_n <= 0){
        fprintf(stderr, "[modheap] HeapCreate from module %d (%s): it has no heaps of its own (game.h modules)\n",
                mod, l ? l->info->name : "none");
        return 0;
    }
    int s0 = 0;
    for (int k = 1; k < mod; k++) s0 += module_link(k)->heap_n;
    for (int i = 0; i < l->heap_n && s0 + i < MH_MAX; i++){
        MHeap *m = &g_h[s0 + i];
        if (m->used) continue;
        uint32_t base = l->heap_base + (uint32_t)i * l->heap_size;
        uint32_t keep = m->committed;     /* pages of an earlier heap here stay committed */
        uint64_t created = m->created;
        memset(m, 0, sizeof *m);
        m->used = 1; m->base = base; m->size = l->heap_size;
        m->top = base + 64u;              /* the handle itself never is a block */
        m->committed = keep ? keep : base;
        m->created = created + 1;
        if (m->committed < base + COMMIT){ mem_commit(base, COMMIT); m->committed = base + COMMIT; }
        return base;
    }
    fprintf(stderr, "[modheap] HeapCreate: no free module heap (%d in use)\n", l->heap_n);
    return 0;
}

static uint32_t cap_for(uint32_t n){
    if (n == 0) n = 1;
    if (n <= 4096u) return (n + 15u) & ~15u;
    return (n + 4095u) & ~4095u;
}

uint32_t modheap_alloc(uint32_t h, uint32_t n, uint32_t flags){
    MHeap *m = heap_of(h);
    if (!m || n > 0x7FFF0000u) return 0;
    uint32_t cap = cap_for(n), u = 0;
    if (cap <= 4096u){
        uint32_t k = cap / 16u;
        if ((u = m->small[k])) m->small[k] = MR32(u - HDR + 12u);
    } else {
        uint32_t prev = 0;
        for (uint32_t b = m->large; b; prev = b, b = MR32(b - HDR + 12u)){
            uint32_t bc = MR32(b - HDR + 8u);
            if (bc >= cap && bc - cap <= cap / 4u){
                if (prev) MR32(prev - HDR + 12u) = MR32(b - HDR + 12u); else m->large = MR32(b - HDR + 12u);
                u = b; cap = bc; break;
            }
        }
    }
    if (!u){
        uint32_t need = HDR + cap;
        if (m->top + need > m->base + m->size || m->top + need < m->top){
            static int said = 0;
            if (said++ < 4) fprintf(stderr, "[modheap] heap 0x%08x full: %u bytes wanted, %llu live\n",
                                    h, n, (unsigned long long)m->live);
            return 0;
        }
        while (m->committed < m->top + need){ mem_commit(m->committed, COMMIT); m->committed += COMMIT; }
        u = m->top + HDR; m->top += need;
        MR32(u - HDR + 8u) = cap;
    }
    MR32(u - HDR) = n; MR32(u - HDR + 4u) = LIVE; MR32(u - HDR + 12u) = 0;
    if (flags & 0x8u) memset(g_mem + u, 0, n);                 /* HEAP_ZERO_MEMORY */
    m->live += n; m->allocs++;
    if (m->live > m->peak) m->peak = m->live;
    return u;
}

static int valid(MHeap *m, uint32_t u){
    return u >= m->base + 64u + HDR && u < m->top && !(u & 15u) && MR32(u - HDR + 4u) == LIVE;
}

int modheap_free(uint32_t h, uint32_t u){
    MHeap *m = heap_of(h);
    if (!m) return 0;
    if (!u) return 1;
    if (!valid(m, u)){
        if (m->bad++ < 4) fprintf(stderr, "[modheap] HeapFree(0x%08x, 0x%08x): not a live block (%s)\n", h, u,
                                  u >= m->base && u < m->top && MR32(u - HDR + 4u) == FREED ? "freed twice" : "foreign");
        return 0;
    }
    uint32_t cap = MR32(u - HDR + 8u);
    m->live -= MR32(u - HDR); m->frees++;
    MR32(u - HDR + 4u) = FREED;
    if (cap <= 4096u){ uint32_t k = cap / 16u; MR32(u - HDR + 12u) = m->small[k]; m->small[k] = u; }
    else { MR32(u - HDR + 12u) = m->large; m->large = u; }
    return 1;
}

uint32_t modheap_size(uint32_t h, uint32_t u){
    MHeap *m = heap_of(h);
    return m && valid(m, u) ? MR32(u - HDR) : 0xFFFFFFFFu;
}

uint32_t modheap_realloc(uint32_t h, uint32_t u, uint32_t n, uint32_t flags){
    MHeap *m = heap_of(h);
    if (!m) return 0;
    if (!u) return modheap_alloc(h, n, flags);
    if (!valid(m, u)) return 0;
    uint32_t old = MR32(u - HDR), cap = MR32(u - HDR + 8u);
    if (n <= cap && n > 0){                                    /* in place */
        if ((flags & 0x8u) && n > old) memset(g_mem + u + old, 0, n - old);
        MR32(u - HDR) = n; m->live += (uint64_t)n - old;
        if (m->live > m->peak) m->peak = m->live;
        return u;
    }
    if (flags & 0x10u) return 0;                               /* HEAP_REALLOC_IN_PLACE_ONLY */
    uint32_t v = modheap_alloc(h, n, flags);
    if (!v) return 0;
    memcpy(g_mem + v, g_mem + u, old < n ? old : n);
    modheap_free(h, u);
    return v;
}

int modheap_destroy(uint32_t h){
    MHeap *m = heap_of(h);
    if (!m) return 0;
    fprintf(stderr, "[modheap] HeapDestroy(0x%08x): %llu allocations, %llu frees, %llu bytes still live, peak %llu KB,"
                    " %u KB of the heap used\n", h, (unsigned long long)m->allocs, (unsigned long long)m->frees,
            (unsigned long long)m->live, (unsigned long long)(m->peak / 1024u), (m->top - m->base) / 1024u);
    m->used = 0;                                               /* pages stay committed for the next one */
    return 1;
}

void modheap_report(void){
    for (int i = 0; i < MH_MAX; i++){
        MHeap *m = &g_h[i];
        if (!m->created) continue;
        fprintf(stderr, "  module heap 0x%08x : created %llu times, %s; %llu KB live, peak %llu KB, %u KB committed\n",
                m->base, (unsigned long long)m->created, m->used ? "in use" : "destroyed",
                (unsigned long long)(m->used ? m->live / 1024u : 0u), (unsigned long long)(m->peak / 1024u),
                (m->committed - m->base) / 1024u);
    }
}
