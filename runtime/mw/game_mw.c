/* Need for Speed: Most Wanted (speed.exe, PC v1.3 Black Edition): the game
 * table the runtime's core uses (runtime/game.h).  web/build.sh compiles the
 * C files of runtime/mw only for GAME=mw (objects mw_<name>.o), so this table
 * is the one MW builds link and UG2's never see; runtime/web/null_game.c's
 * weak stand-ins answer for every feature MW has no file of its own for yet.
 *
 * What the table holds now: the game and its files (its registry keys,
 * registry_mw.c, and its own registry file), the lifted entry point, the two
 * lifted DLLs (server.dll; d3dx9_26.dll in a D3DX oracle build only, PLAN
 * D-09), the C runtime's heap globals, and win32_misc.c's answers to the
 * imports only MW needs answered (GAME.md 3.3).
 * A field left 0 has its task named below; core then does for MW what it does
 * for a game without that feature. */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include "module.h"

void GAMEINFO_ENTRY_FN(CPU *c);                /* the entry point, 0x7C4040 (gen/mw/all/gameinfo.h, through cpu.h) */
int win32_misc_call(CPU *c, const char *dll, const char *n, uint32_t *r);   /* win32_misc.c */
extern const RegKey mw_reg_keys[];             /* registry_mw.c */

/* the lifted DLLs (module.c): EA's LAN server (gen/mw/server, prefix srv_,
 * pinned in games/mw/game.conf), and Microsoft's D3DX as a dev-only oracle
 * (gen/mw/d3dx, prefix d3dx_), linked when web/build.sh finds its lift: the
 * build then passes -DNFSU2_D3DX_ORACLE=1 and writes "oracle":1 into the
 * module's json, which release.sh refuses */
MODLINK_DECLARE(srv_);
#if NFSU2_D3DX_ORACLE
MODLINK_DECLARE(d3dx_);
#endif
static const ModLink mw_modules[] = {
    { MODLINK_TABLES(srv_), .slot_lo = MM_SRV_LO, .slot_hi = MM_SRV_HI,
      .heap_base = MM_SRV_HEAP_BASE, .heap_size = MM_SRV_HEAP_SIZE, .heap_n = MM_SRV_HEAP_N,
      .feature = "LAN hosting", .release = "NFS Most Wanted v1.3" },
#if NFSU2_D3DX_ORACLE
    { MODLINK_TABLES(d3dx_), .slot_lo = MM_D3DX_LO, .slot_hi = MM_D3DX_HI,
      .heap_base = MM_D3DX_HEAP_BASE, .heap_size = MM_D3DX_HEAP_SIZE, .heap_n = MM_D3DX_HEAP_N,
      .feature = "the D3DX oracle (dev builds)", .release = "DirectX 9.0c, June 2005 (9.07.239.0000)" },
#endif
};

/* Left 0, and why:
 *   reg_defaults        the registry answers per key (reg_keys), not by value name alone
 *   lang_suffixes       Q-06 (the movies' language suffix)
 *   install, hook       no MW patches or hook sites yet (games/mw has no override or hook list)
 *   on_present, on_frame, on_test_frame, status   Q-24 (MW's per-frame anchor), the page's overlay (M6)
 *   sms_request, photo_input   UG2 features, not ported
 *   flow, flow_fe, flow_race, top_screen   M2-18 (GameFlow 0x925E90 and the FEng root, unverified)
 *   racer, text_entry   test traces (M4), DirectInput text entry (M3-11)
 *   crt_lo, crt_hi      HEAPSTAT's call sites: the C runtime's code range is not measured (its heap's
 *                       HeapAlloc/HeapReAlloc/HeapFree calls are at 0x7D10EF-0x7D5E6C; GAME.md 2.11 has an
 *                       estimate only); with 0 it names the runtime's own caller
 *   on_qpc, qpc_caller  MW's frame wait loop is not studied (M4)
 *   lan_*               MW's LAN server glue is not studied (M8)
 *   res_labels, on_sound_unlock   M3-03 (the resolution rows), UG2 debugging only
 *   online_host_va, online_host   M9: the lobby is "*pcnfs06.ea.com" at 0x8F42C8 on port 30920
 *                       (Q-14), which the server's Online stand-in does not serve; Online stays off */
const GameOps g_game = {
    .id = "mw",
    .exe_name = "speed.exe",
    .vroot = "C:\\NFSMW",
    .module_path = "C:\\NFSMW\\speed.exe",
    .reg_keys = mw_reg_keys,                    /* registry_mw.c: the keys speed.exe opens, all HKLM */
    .reg_file = ".nfsmw_registry",              /* never UG2's ~/.nfsu2_registry */
    .entry = GAMEINFO_ENTRY_FN,
    .entry_va = GAMEINFO_ENTRY,
    .modules = mw_modules,
    .nmodules = (int)(sizeof mw_modules / sizeof mw_modules[0]),
    .crtheap_va = 0x009c5318u,                  /* _heap_init 0x7CD4B0: HeapCreate's handle, then __heap_select's */
    .active_heap_va = 0x009c531cu,              /* answer (3 = the small-block heap) */
    .win32_misc = win32_misc_call,              /* win32_misc.c: memory, locale, keyboard, voice devices */
};
