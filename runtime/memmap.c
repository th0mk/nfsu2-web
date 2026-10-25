/* The guest layout's boot check (runtime/memmap.h), run by win32_init once
 * the exe's image is loaded: no two memory regions overlap, none overlaps the
 * image, no two handle-value ranges overlap (and, where the game's map asks
 * for it, no handle value lies on guest memory), every region lies inside
 * the guest, each lifted DLL lies inside its slot, and its own heaps inside
 * the modheap region, apart from the other DLLs' heaps.  The DLLs, their
 * slots and their heaps are the game's (game.h modules).  One [mem] line
 * shows the layout; a failure names both ranges and stops the run before the
 * game starts, instead of letting one region corrupt another later. */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>

_Static_assert(MM_PEB >= MM_TEB_MAIN && MM_PEB + 0x1000u <= MM_TEB_FIRST, "the PEB lies in the main TEB's pages");
_Static_assert(MM_TEB_FIRST >= MM_TEB_MAIN + 0x20000u, "created TEBs start after the main TEB's pages");
_Static_assert(MM_TEB_TIMER >= MM_TEB_FIRST && MM_TEB_TIMER + MM_TEB_SIZE <= MM_TEB_HI
               && (MM_TEB_TIMER - MM_TEB_FIRST) % MM_TEB_SIZE == 0, "the timer's TEB is one of the TEB slots");
_Static_assert((MM_TSTACK_HI - MM_TSTACK_LO) % MM_TSTACK_SIZE == 0, "whole thread stacks");

typedef struct { const char *name; uint32_t lo, hi; } Range;   /* [lo, hi) */

static int apart(const Range *a, const Range *b){ return a->hi <= b->lo || b->hi <= a->lo; }
static int clash(const char *what, const Range *a, const Range *b, int fatal){
    fprintf(stderr, "[mem] LAYOUT BROKEN: %s %s 0x%08x-0x%08x and %s 0x%08x-0x%08x overlap (runtime/memmap.h, "
            MM_GAME ")\n", what, a->name, a->lo, a->hi, b->name, b->lo, b->hi);
    if (fatal){ fflush(stderr); exit(3); }
    return 1;
}
static int outside(const char *what, const Range *a, const Range *in, int fatal){
    fprintf(stderr, "[mem] LAYOUT BROKEN: %s %s 0x%08x-0x%08x, outside %s 0x%08x-0x%08x (runtime/memmap.h, "
            MM_GAME ")\n", a->name, what, a->lo, a->hi, in->name, in->lo, in->hi);
    if (fatal){ fflush(stderr); exit(3); }
    return 1;
}

#define MM_MODS 4                        /* lifted DLLs the check looks at */

int memmap_check(uint32_t image_lo, uint32_t image_hi, int fatal){
    int nmod = g_game.nmodules < MM_MODS ? g_game.nmodules : MM_MODS;
    const Range modheap = { "modheap", MM_MODHEAP_BASE, MM_MODHEAP_BASE + MM_MODHEAP_SIZE * MM_MODHEAP_N };
    Range mem[8 + MM_MODS]; int nm = 0;
    mem[nm++] = (Range){ "image", image_lo, image_hi };
    for (int i = 0; i < nmod; i++){                     /* the DLLs' slots */
        const ModLink *l = &g_game.modules[i];
        mem[nm++] = (Range){ l->info->name[0] ? l->info->name : "module", l->slot_lo, l->slot_hi };
    }
    mem[nm++] = (Range){ "winsock buffers", MM_WSBUF_BASE, MM_WSBUF_BASE + MM_WSBUF_SLOT * MM_WSBUF_N };
    mem[nm++] = (Range){ "TEBs", MM_TEB_LO, MM_TEB_HI };
    mem[nm++] = modheap;
    mem[nm++] = (Range){ "main stack", MM_STACK_LO, MM_STACK_HI };
    mem[nm++] = (Range){ "thread stacks", MM_TSTACK_LO, MM_TSTACK_HI };
    mem[nm++] = (Range){ "heap", MM_HEAP_LO, MM_HEAP_HI };
    const Range val[3] = {
        { "files", MM_FH_BASE, MM_FH_BASE + MM_FH_MAX },
        { "objects", MM_H_BASE, MM_H_BASE + MM_H_GENS * MM_H_MAX },   /* every generation's (win32_ext.c) */
        { "windows", MM_HWND_DESKTOP, MM_HDC + 1u },
    };
    const int nv = 3;
    int bad = 0;
    for (int i = 0; i < nm; i++){
        if (mem[i].hi <= mem[i].lo){
            fprintf(stderr, "[mem] LAYOUT BROKEN: %s 0x%08x-0x%08x is empty (runtime/memmap.h, " MM_GAME ")\n",
                    mem[i].name, mem[i].lo, mem[i].hi);
            if (fatal){ fflush(stderr); exit(3); }
            bad = 1;
        }
#ifdef __wasm__
        if (mem[i].hi > GUEST_END){
            fprintf(stderr, "[mem] LAYOUT BROKEN: %s 0x%08x-0x%08x ends beyond the guest (0x%08x)\n",
                    mem[i].name, mem[i].lo, mem[i].hi, GUEST_END);
            if (fatal){ fflush(stderr); exit(3); }
            bad = 1;
        }
#endif
        for (int j = i + 1; j < nm; j++) if (!apart(&mem[i], &mem[j])) bad |= clash("memory", &mem[i], &mem[j], fatal);
    }
    for (int i = 0; i < nv; i++){
        for (int j = i + 1; j < nv; j++) if (!apart(&val[i], &val[j])) bad |= clash("handle values", &val[i], &val[j], fatal);
        if (MM_VALUES_APART)
            for (int j = 0; j < nm; j++) if (!apart(&val[i], &mem[j])) bad |= clash("handle values on memory:", &val[i], &mem[j], fatal);
    }
    Range heaps[MM_MODS];
    for (int i = 0; i < nmod; i++){
        const ModLink *l = &g_game.modules[i];
        const ModInfo *mi = l->info;
        if (mi->name[0] && (mi->base < l->slot_lo || mi->base + mi->size > l->slot_hi)){
            fprintf(stderr, "[mem] LAYOUT BROKEN: %s was lifted at 0x%08x-0x%08x, outside its slot 0x%08x-0x%08x"
                    " (runtime/memmap.h, " MM_GAME ")\n", mi->name, mi->base, mi->base + mi->size, l->slot_lo, l->slot_hi);
            if (fatal){ fflush(stderr); exit(3); }
            bad = 1;
        }
        /* its own heaps: inside the modheap region, apart from the others' */
        heaps[i] = (Range){ mem[1 + i].name, l->heap_base, l->heap_base + l->heap_size * (uint32_t)(l->heap_n > 0 ? l->heap_n : 0) };
        if (heaps[i].hi > heaps[i].lo && (heaps[i].lo < modheap.lo || heaps[i].hi > modheap.hi))
            bad |= outside("heaps", &heaps[i], &modheap, fatal);
        for (int j = 0; j < i; j++)
            if (heaps[i].hi > heaps[i].lo && heaps[j].hi > heaps[j].lo && !apart(&heaps[i], &heaps[j]))
                bad |= clash("module heaps", &heaps[j], &heaps[i], fatal);
    }
    if (bad) return 1;
    char line[1024]; int n = snprintf(line, sizeof line, "[mem] " MM_GAME ":");
    for (int i = 0; i < nm && n < (int)sizeof line; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, "%s %s 0x%08x-0x%08x", i ? "," : "", mem[i].name, mem[i].lo, mem[i].hi);
    for (int i = 0; i < nv && n < (int)sizeof line; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, "%s %s 0x%08x-0x%08x", i ? "," : "; handle values:", val[i].name, val[i].lo, val[i].hi);
    if (n < (int)sizeof line)
        snprintf(line + n, sizeof line - (size_t)n, "; no overlap%s", MM_VALUES_APART ? "" : " (handle values may lie on memory)");
    fprintf(stderr, "%s\n", line);
    return 0;
}
