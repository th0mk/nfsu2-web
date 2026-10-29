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
int mw_fe_match(const char *name);              /* fe_mw.c */
uint32_t mw_flow(void); const char *mw_top_screen(void); void mw_fe_frame(void);  /* fe_mw.c */
uint32_t mw_fe_object(const char *package, uint32_t hash);
extern const ThreadName mw_thread_names[];     /* threads_mw.c */
uint32_t mw_thread_start(uint32_t entry, uint32_t arg);
void fxcensus_init(void);                      /* fxcensus_mw.c: NFSMW_FXCENSUS=1, the effect census (oracle builds) */
void mw_sound_position(void);                  /* sound_mw.c */
void mw_frame(CPU *c);                         /* career_mw.c: the test aids' work, once a pass of the main loop */
int mw_debug_hook(CPU *c, uint32_t va);        /* debug_mw.c: NFSMW_HOOKLOG, a debugging lift's extra hooks */

/* An instruction the lift hooks (gen.sh collects every HOOK_SITE of the C files in runtime/mw into HOOKS):
 * guest_hook (debug.c) runs before it. */
#define HOOK_SITE(va) (va##u)

/* The game's per-frame anchor (Q-24).  The C runtime's main (0x666590) loops on [0x9257EC] == 0 and calls
 * MainLoop (0x663D30) once a pass, from 0x66667D with the milliseconds since the last pass.  MainLoop runs the
 * game's update and then its render: World_Service (0x75AAD0, the reader of the world's first timestep
 * 0x903290) from 0x663E5A, and the render wrapper 0x64A7C0 from 0x663ECF, whose 0x6E7220 presents (a PRESENTBT
 * backtrace at the main menu).  The render is skipped while [0x925854] is set or the frame-skip count
 * [0x925850] is above 0.  0x663EC6 (mov al, [0x925854]) is where the update's branches meet before that
 * test: every pass reaches it once, after the update and before the render, so the work there is seen in the
 * same frame's picture. */
#define MW_FRAME_SITE HOOK_SITE(0x00663ec6)

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
 * oracle in an oracle build, which also reads NFSMW_FXCENSUS (fxcensus_mw.c) */
static void mw_install(void){
#if NFSU2_D3DX_ORACLE
    d3dxmath_install(1);
    fxcensus_init();
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

/* The display modes the adapter lists (com.c EnumAdapterModes): the six 4:3
 * modes of the game's own table first (0x8F9414 widths, 0x8F942C heights).
 * The game marks each of them the list holds (0x6C15B0, [0x93DBEC+4i]) and
 * the options' resolution arrows step over unmarked ones without a bound
 * (0x50F998), so all six are listed; the wide modes after them match no row. */
static const uint32_t mw_modes[][2] = {
    {640,480},{800,600},{1024,768},{1280,960},{1280,1024},{1600,1200},
    {1280,720},{1920,1080},{2560,1440},{3840,2160},{0,0},
};

/* The game pad (dinput_pad.c): speed.exe enumerates its input devices once,
 * in its DirectInput manager's constructor (0x7F1B3F, made by 0x7F1889 from
 * 0x62847A and kept in [0x9BB410] for the game's life): EnumDevices
 * (DI8DEVCLASS_ALL, callback 0x7F1A6B, pvRef the manager, ATTACHEDONLY) from
 * 0x7F1B26, and nothing calls it again.  The callback makes a joystick for
 * device types 0x14..0x18 (0x4A4 bytes, 0x7F2735: c_dfDIJoystick2 0x8C152C,
 * GetCapabilities with 0x2C, autocentre off, Acquire), which joins the
 * manager's poll list (0x7F1DAA) and goes to the game's handler at manager
 * +0xC (0x6283D0: a joystick is appended to [0x91F150], count [0x9203B8]; the
 * game reads the first, [0x91F150], where it wants one).  So the callback,
 * a static function, and its pvRef, the manager, stay valid, and a
 * controller connected later is offered through them (pad_late_cb).  A second
 * enumeration, DI8DEVCLASS_GAMECTRL with callback 0x6D81D0 and no pvRef from
 * 0x6DC5AD on another IDirectInput8 ([0x982D14]), keeps force-feedback
 * devices in [0x93DF00 + 4n]; it gets a controller there at the start only.
 * Every frame the pad's poll (0x7F28A4) does Poll; when that fails it calls
 * Acquire, and while Acquire answers DIERR_INPUTLOST it calls it again with
 * SleepEx(0, TRUE) between (0x7F28EC..0x7F2904, no other way out), then
 * returns without a state, so dinput_pad.c's Poll and Acquire never fail.
 * Otherwise it does GetDeviceState with 0x110 bytes (0x7F2918) and keeps
 * the state at device +0x234 (axes, then one dword per
 * button from +0xC, the POVs at +0x220).
 * The actions read it through the binding table [0x91F418] (0x3DC dwords,
 * the defaults at 0x8F6D80, 0x628410): per action a name and four bindings
 * of (type, device or key, code), the keyboard's in the first two, a
 * joystick's in the last two (0x628560); type 2 a button, 3 a POV 0
 * direction (0 up, 1 right, 2 down, 3 left), 4..9 axes (0x628940, jump table
 * 0x628BB8).  Its front-end defaults are a DualShock's under DirectInput:
 * FE_ACTION_ACCEPT button 1, CANCEL 2, START 9, LTRIG 4, RTRIG 5, B5 0, the
 * D-pad moving; none for driving.  Hence pad_buttons: Cross (the pad's A) is
 * button 1, Circle 2, Square 0, Triangle 3, the triggers 6 and 7, Share 8,
 * Options (Start) 9, the sticks 10 and 11.
 * The PC input device ([0x920DE8], vtable 0x8A7B9C; 0x634370 gives its
 * holder) sets itself up once at the start (method 3, 0x63F850): with a
 * joystick it polls it, keeps its state as the rest state (0x91F1E8) and
 * picks the joystick's bindings by its product name (0x63CD80, also run by
 * the profile load when the saved joystick is another, 0x56E27D).  After a
 * start with none, FE_ACTION_RIGHT's first joystick binding read (5, 0, 0),
 * an axis, where a start with the pad had (3, 0, 1), the POV, and a pad that
 * came later had its buttons but no D-pad (tests/mw/scripts/pad_menu.txt
 * runs, the table peeked).  So that set-up runs again after a late pad. */
static const PadLate mw_pad_late = {
    .cb = 0x007f1a6bu, .setup = 0x0063f850u, .setup_this = 0x00920de8u, .setup_vtbl = 0x008a7b9cu,
};

/* Left 0, and why:
 *   reg_defaults        the registry answers per key (reg_keys), not by value name alone
 *   lang_suffixes       Q-06 (the movies' language suffix)
 *   on_present, status  the page's overlay (M6)
 *   sms_request, photo_input   UG2 features, not ported
 *   racer, text_entry   test traces (M4), the page's phone text field (M6-21: the alias name field takes
 *                       WM_CHAR through the window procedure 0x6DB6C0, tests/mw/scripts/kb_alias.txt)
 *   crt_lo, crt_hi      HEAPSTAT's call sites: the C runtime's code range is not measured (its heap's
 *                       HeapAlloc/HeapReAlloc/HeapFree calls are at 0x7D10EF-0x7D5E6C; GAME.md 2.11 has an
 *                       estimate only); with 0 it names the runtime's own caller
 *   on_qpc, qpc_caller  MW has no QPC wait loop: its frame limiter sleeps in the Sim timer's step (0x642E60),
 *                       which framerate_mw.c replaces with the host's slot pacer
 *   lan_*               MW's LAN server glue is not studied (M8)
 *   res_labels          the options show the table's own labels ("800x600"); the rows' real device sizes
 *                       are com.c's res_pick, as for UG2
 *   on_sound_unlock     UG2 debugging only
 *   online_host_va, online_host   M9: the lobby is "*pcnfs06.ea.com" at 0x8F42C8 on port 30920
 *                       (Q-14), which the server's Online stand-in does not serve; Online stays off
 *   pad_rescan          no search after the start (above): the pad stays one device, whichever
 *                       controller drives it (a wheel after a pad included)
 *   pad_trigger_axes, pad_jolt, pad_hold_screen, pad_fe_keys   UG2's pad features: MW's driving
 *                       layout, its rumble and its controls screen are M6-37's */
const GameOps g_game = {
    .id = "mw",
    .exe_name = "speed.exe",
    .vroot = "C:\\NFSMW",
    .module_path = "C:\\NFSMW\\speed.exe",
    .reg_keys = mw_reg_keys,                    /* registry_mw.c: the keys speed.exe opens, all HKLM */
    .reg_file = ".nfsmw_registry",              /* never UG2's ~/.nfsu2_registry */
    .sandbox = 1,                               /* fileio.c: nothing outside C:\NFSMW, the game folder read-only */
    .find_sorted = 1,                           /* fileio.c: folder scans in NTFS order (the alias lists) */
    .entry = GAMEINFO_ENTRY_FN,
    .entry_va = GAMEINFO_ENTRY,
    .modules = mw_modules,
    .nmodules = (int)(sizeof mw_modules / sizeof mw_modules[0]),
    .install = mw_install,                      /* d3dxmath_mw.c: the D3DX math imports, native */
    .crtheap_va = 0x009c5318u,                  /* _heap_init 0x7CD4B0: HeapCreate's handle, then __heap_select's */
    .active_heap_va = 0x009c531cu,              /* answer (3 = the small-block heap) */
    .win32_misc = mw_imports,                   /* win32_misc.c: memory, locale, keyboard, voice devices; msvcrt_mw.c */
    .d3d_modes = mw_modes,                      /* the 4:3 modes first (GAME.md 5.7) */
    .d3d9_strict = 1,                           /* D3D9's answers: level sizes, failed creators, no multisampling */
    .gpu_stages = 8,                            /* t4..t7 and s4..s7 at SM2 already, depth textures (GAME.md 5.9) */
    .d3d_sm3 = 1,                               /* vs/ps 3.0: the ps_2_x and SM3 techniques (GAME.md 5.3) */
    .d3d_host_backing = 1,                      /* textures' and buffers' copies outside the guest's 512 MB:
                                                 * a race's world (~170 MB of them) and the game's own heap
                                                 * (~200 MB) no longer share it */
    .vclock_sleep_real = 1,                     /* the boot's CPU speed test (0x6CAF90) sees its Sleep(50) */
    .flow = mw_flow,                            /* fe_mw.c: GameFlowManager's state [0x925E90] */
    .flow_fe = 3,
    .flow_race = 6,
    .fe_match = mw_fe_match,                    /* the scripts' names within a package: dialogs, highlights */
    .top_screen = mw_top_screen,               /* the active packages' tail, [[[0x91CADC]]+0xE4] */
    .on_test_frame = mw_fe_frame,               /* the screens the game brings up, for waitfng and [fng] */
    .fe_object = mw_fe_object,                  /* the scripts' $O: an active package's object by name hash */
    .init_tables = mw_init_tables,
    .ninit_tables = (int)(sizeof mw_init_tables / sizeof mw_init_tables[0]),
    .init_check = 0x009aa9d0u + 0x84cu,         /* the bitset pointer of 0x9AA9D0's constructor */
    .thread_names = mw_thread_names,
    .thread_start = mw_thread_start,            /* past _threadstartex and the game's trampoline */
    .on_sound_position = mw_sound_position,     /* sound_mw.c: the sound thread's stall count, watched */
    .sound_wall_catchup = 1,                    /* its cursors keep real time with no output (GAME.md 7.2) */
    .on_frame = mw_frame,                       /* career_mw.c: the queued test aids, between update and render */
    .frame_site = MW_FRAME_SITE,                /* MainLoop's 0x663EC6, above */
    .hook = mw_debug_hook,                      /* debug_mw.c: the frame anchor is core's; the crash fixes'
                                                 * sites (fixes_mw.c) and a debugging lift's HOOKS=... reach it */
    .pad_late = &mw_pad_late,                   /* a late pad: the input manager's callback, then the input set-up */
    .pad_buttons = "X A B Y LB RB LT RT BACK START LS RS",  /* a DualShock's order, which its layout expects */
};
