/* Need for Speed: Most Wanted (speed.exe, PC v1.3 Black Edition): the game
 * table the runtime's core uses (runtime/game.h).  web/build.sh compiles the
 * C files of runtime/mw only for GAME=mw (objects mw_<name>.o), so this table
 * is the one MW builds link and UG2's never see; runtime/web/null_game.c's
 * weak stand-ins answer for every feature MW has no file of its own for yet.
 *
 * What the table holds now: the game and its files (its registry keys,
 * registry_mw.c; its own registry file; paths that never leave C:\NFSMW), the
 * lifted entry point, the two lifted DLLs (server.dll; d3dx9_26.dll in a D3DX
 * oracle build only, PLAN D-09), the D3DX math imports answered natively
 * (its install, d3dxmath_mw.c, D-10), the C runtime's heap globals, win32_misc.c's
 * answers to the imports only MW needs answered (GAME.md 3.3), what the test
 * harness reads (the game flow, the front end's screens: fe_mw.c), the C
 * runtime's initializer tables the boot counts, and the threads the stall
 * watchdog names (threads_mw.c).  A field left 0 has its task named below;
 * core then does for MW what it does for a game without that feature. */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include "module.h"
#include "d3dxmath_mw.h"
#include <strings.h>

void GAMEINFO_ENTRY_FN(CPU *c);                /* the entry point, 0x7C4040 (gen/mw/all/gameinfo.h, through cpu.h) */
int win32_misc_call(CPU *c, const char *dll, const char *n, uint32_t *r);   /* win32_misc.c */
int msvcrt_call(CPU *c, const char *n, uint32_t *r); uint32_t msvcrt_bind(const ImpEnt *e);    /* msvcrt_mw.c */
extern const RegKey mw_reg_keys[];             /* registry_mw.c */
uint32_t mw_flow(void); const char *mw_top_screen(void); void mw_fe_frame(void);  /* fe_mw.c */
extern const ThreadName mw_thread_names[];     /* threads_mw.c */
uint32_t mw_thread_start(uint32_t entry, uint32_t arg);

/* the lifted DLLs (module.c): EA's LAN server (gen/mw/server, prefix srv_,
 * pinned in games/mw/game.conf), and Microsoft's D3DX as a dev-only oracle
 * (gen/mw/d3dx, prefix d3dx_), linked when web/build.sh finds its lift: the
 * build then passes -DNFSU2_D3DX_ORACLE=1 and writes "oracle":1 into the
 * module's json, which release.sh refuses.  The exe imports the oracle
 * itself, so it is loaded before the entry point and the IAT slots of its two
 * effect imports are bound to its exports (module.c module_load_imports); the
 * other 11, the math, stay on mw_install's native answers.  The game folder does
 * not hold d3dx9_26.dll (a system DLL): NFSMW_D3DX names Microsoft's file on
 * the host (GAME.md 5.1).  Its C runtime is msvcrt.dll: msvcrt_mw.c, with
 * _adjust_fdiv bound as a variable. */
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
      .feature = "the D3DX oracle (dev builds)", .release = "DirectX 9.0c, June 2005 (9.07.239.0000)",
      .file_env = "NFSMW_D3DX", .bind = msvcrt_bind },
#endif
};

/* The C runtime's initializer tables, as _cinit (0x7CA2C0) runs them
 * (GAME.md 2.6): first the C initializers of __xi with _initterm_e, which
 * stops at one that returns non-zero, then the C++ constructors of __xc with
 * _initterm.  Each runner's `call [edx]` returns to ret.  The tables hold 5
 * of 6 and 870 of 1,113 slots (counted in the exe).  The constructor of the
 * global at 0x9AA9D0 (0x889DD0, slot 0x8EAED0) sets its +0x84C to the bitset
 * 0x9A2DE0: the pointer an unrun constructor leaves 0 (L-051). */
static const InitTable mw_init_tables[] = {
    { "C initializers", "__xi", 0x008eb168u, 0x008eb180u, 0x007ca570u, 0x007ca2e4u, 0x007ca596u, 1 },
    { "C++ constructors", "__xc", 0x008ea000u, 0x008eb164u, 0x007ca540u, 0x007ca311u, 0x007ca558u, 0 },
};

/* After win32_init, before the entry point (main_web.c): the D3DX math imports answered natively
 * (d3dxmath_mw.c, PLAN D-10), before module_load_imports binds what is left of d3dx9_26.dll's to the
 * oracle in an oracle build */
static void mw_install(void){
#if NFSU2_D3DX_ORACLE
    d3dxmath_install(1);
#else
    d3dxmath_install(0);
#endif
}

/* The imports the shared layers leave to the game (game.h win32_misc): msvcrt.dll's, which only the D3DX
 * oracle imports, then win32_misc.c's */
static int mw_imports(CPU *c, const char *dll, const char *n, uint32_t *r){
    if (!strcasecmp(dll, "msvcrt.dll")) return msvcrt_call(c, n, r);
    return win32_misc_call(c, dll, n, r);
}

/* Left 0, and why:
 *   reg_defaults        the registry answers per key (reg_keys), not by value name alone
 *   lang_suffixes       Q-06 (the movies' language suffix)
 *   hook                no MW hook sites yet (games/mw has no override or hook list)
 *   on_present, on_frame, status   Q-24 (MW's per-frame anchor), the page's overlay (M6)
 *   sms_request, photo_input   UG2 features, not ported
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
    .sandbox = 1,                               /* fileio.c: nothing outside C:\NFSMW, the game folder read-only */
    .entry = GAMEINFO_ENTRY_FN,
    .entry_va = GAMEINFO_ENTRY,
    .modules = mw_modules,
    .nmodules = (int)(sizeof mw_modules / sizeof mw_modules[0]),
    .install = mw_install,                      /* d3dxmath_mw.c: the D3DX math imports, native */
    .crtheap_va = 0x009c5318u,                  /* _heap_init 0x7CD4B0: HeapCreate's handle, then __heap_select's */
    .active_heap_va = 0x009c531cu,              /* answer (3 = the small-block heap) */
    .win32_misc = mw_imports,                   /* win32_misc.c: memory, locale, keyboard, voice devices; msvcrt_mw.c */
    .flow = mw_flow,                            /* fe_mw.c: GameFlowManager's state [0x925E90] */
    .flow_fe = 3,
    .flow_race = 6,
    .top_screen = mw_top_screen,                /* the active packages' tail, [[[0x91CADC]]+0xE4] */
    .on_test_frame = mw_fe_frame,               /* the screens the game brings up, for waitfng and [fng] */
    .init_tables = mw_init_tables,
    .ninit_tables = (int)(sizeof mw_init_tables / sizeof mw_init_tables[0]),
    .init_check = 0x009aa9d0u + 0x84cu,         /* the bitset pointer of 0x9AA9D0's constructor */
    .thread_names = mw_thread_names,
    .thread_start = mw_thread_start,            /* past _threadstartex and the game's trampoline */
};
