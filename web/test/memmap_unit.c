/* runtime/memmap.c's boot check on its own (web/test/memmap.test.mjs):
 *   memmap_unit.wasm <image lo> <image hi> [fatal]
 * The game's lifted DLLs (game.h modules, as the game's file lists them):
 * server.dll at -DSRV_BASE with -DSRV_SIZE (its ModInfo in a real build) and,
 * with MW's map, d3dx9_26.dll at its slot, whose heaps -DD3DX_HEAP_BASE
 * moves.  Prints "memmap_unit: ok" or "memmap_unit: broken" and exits with
 * memmap_check's result. */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>

static const ModInfo srv = { "server.dll", "", SRV_BASE, 0x10000000u, SRV_SIZE, SRV_BASE + 0x1000u, SRV_BASE + 0x1000u, 0x1000u, 0u };
#ifdef NFSU2_GAME_MW
#ifndef D3DX_HEAP_BASE
#define D3DX_HEAP_BASE MM_D3DX_HEAP_BASE
#endif
static const ModInfo d3dx = { "d3dx9_26.dll", "", MM_D3DX_LO, 0x4fd50000u, 0x24a000u, MM_D3DX_LO + 0x1000u,
                              MM_D3DX_LO + 0x1000u, 0x1000u, 0u };
#endif
static const ModLink mods[] = {
    { .info = &srv, .slot_lo = MM_SRV_LO, .slot_hi = MM_SRV_HI,
      .heap_base = MM_SRV_HEAP_BASE, .heap_size = MM_SRV_HEAP_SIZE, .heap_n = MM_SRV_HEAP_N },
#ifdef NFSU2_GAME_MW
    { .info = &d3dx, .slot_lo = MM_D3DX_LO, .slot_hi = MM_D3DX_HI,
      .heap_base = D3DX_HEAP_BASE, .heap_size = MM_D3DX_HEAP_SIZE, .heap_n = MM_D3DX_HEAP_N },
#endif
};
const GameOps g_game = { .id = MM_GAME, .modules = mods, .nmodules = (int)(sizeof mods / sizeof mods[0]) };

int main(int argc, char **argv){
    if (argc < 3){ fprintf(stderr, "usage: memmap_unit <image lo> <image hi> [fatal]\n"); return 2; }
    uint32_t lo = (uint32_t)strtoul(argv[1], 0, 0), hi = (uint32_t)strtoul(argv[2], 0, 0);
    int r = memmap_check(lo, hi, argc > 3 && atoi(argv[3]));
    fflush(stderr);
    printf("memmap_unit: %s\n", r ? "broken" : "ok");
    return r;
}
