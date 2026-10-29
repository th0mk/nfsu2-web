/* The game a build runs, as the runtime's core sees it: one table, g_game,
 * filled by that game's own file (game_ug2.c for SPEED2.EXE).  A build links
 * exactly one game file.  Core reaches the game's features only through this
 * table and through the weak stand-ins of runtime/web/null_game.c, never by
 * naming a feature file's function or a game address, so a build without a
 * game's feature files still links (null_game.c's g_game: no game at all).
 *
 * A field left 0 is one core does not call or use for that game, but the
 * four names (id, exe_name, vroot, module_path) are always set. */
#ifndef GAME_H
#define GAME_H
#include <stdint.h>

typedef struct CPU CPU;
struct ModLink;                     /* module.h */

/* A registry value the game reads but finds nothing stored for
 * (RegQueryValueExA, win32_ext.c): text, or a DWORD when str is 0.  A table
 * ends with a 0 name.  In a key of reg_keys (below) a value can instead be
 * one the machine or the page decides: str_fn or dword_fn answers it every
 * time, whatever was stored (str_fn's 0: not there). */
typedef struct RegDefault {
    const char *name;               /* the value's name, matched without case */
    const char *str;                /* REG_SZ text, or 0 */
    uint32_t dword;                 /* REG_DWORD, when str is 0 */
    const char *(*str_fn)(void);    /* REG_SZ, decided now (reg_keys only) */
    uint32_t (*dword_fn)(void);     /* REG_DWORD, decided now (reg_keys only) */
} RegDefault;

/* A registry key of the game's machine (RegOpenKey*, win32_ext.c): which
 * keys there are, and what each answers.  A table ends with a 0 path. */
typedef struct RegKey {
    uint32_t root;                  /* the predefined key it is under: HKEY_LOCAL_MACHINE 0x80000002 ... */
    const char *path;               /* its path under root, matched without case */
    int store;                      /* 1: the values the game writes here are kept (the registry file) */
    const RegDefault *values;       /* what it answers for values nothing stored, ends with a 0 name */
} RegKey;

/* What the page's overlay and touch controls need of the game, at every
 * Present (runtime/web/status_web.c publishes each value that changed). */
typedef struct StatusOut {
    int drive;          /* 1 the player drives in a race, 2 in free roam, 0 elsewhere;
                         * | 4 with a manual gearbox, | 8 during a cutscene */
    int rate;           /* the frame rate the game paces to now, 0 = uncapped */
    int pause;          /* 1 while the game's pause menu is the top screen */
    int sms;            /* the text message of ours the game has in its inbox this game, 0 none,
                         * so the page asks for it no more */
    int roam_cars;      /* friends' cars the game holds in free roam */
    int roam_friends;   /* friends connected in free roam */
} StatusOut;

/* A resolution row's label in the game's settings menu: the text's address
 * in the exe and the bytes it has room for (com.c rewrites the labels). */
typedef struct ResLabel { uint32_t va, cap; } ResLabel;

/* One of the C runtime's tables of initializers (dispatch.c counts how many
 * of its functions ran at boot, L-051): the slots lo..hi (hi exclusive), each
 * a function or 0, which the runner calls one by one through `call [edx]`
 * (edx the slot), leaving ret on the stack; the runner itself is called at
 * from.  With stop set the runner ends at the first function that returns
 * non-zero (_initterm_e). */
typedef struct InitTable {
    const char *what, *name;        /* "C++ constructors", "__xc" */
    uint32_t lo, hi, runner, from, ret;
    int stop;
} InitTable;

/* A routine one of the game's threads runs, and the thread's name in the
 * stall watchdog's report and the [thread] lines.  A table ends with a 0
 * start. */
typedef struct ThreadName { uint32_t start; const char *name; } ThreadName;

/* A controller connected after the game enumerated its devices and found
 * none, for a game that never enumerates again (game.h pad_late): at a
 * Present its EnumDevices callback cb gets the device, with the pvRef of
 * that enumeration, as a new enumeration would offer it; then, where the
 * game set its input up for the controllers it found, that routine runs
 * again: setup, a thiscall without arguments on the object whose pointer is
 * at setup_this, when that object's vtable is setup_vtbl (0 setup: none). */
typedef struct PadLate { uint32_t cb, setup, setup_this, setup_vtbl; } PadLate;

typedef struct GameOps {
    /* ---- the game and its files ---- */
    const char *id;                 /* "ug2": the game id the module, the page and the server share */
    const char *exe_name;           /* the exe's file name in the game folder, "SPEED2.EXE" */
    const char *vroot;              /* the folder the game believes it is installed in, "C:\\NFSU2" */
    const char *module_path;        /* GetModuleFileNameA(NULL): the exe's path, as the game reads it */
    const RegDefault *reg_defaults; /* answers for registry values nothing stored */
    const RegKey *reg_keys;         /* the registry's keys and each key's answers; 0 = one flat registry
                                     * (UG2's): every key opens, and a value is the stored one or
                                     * reg_defaults', whichever key it is asked of */
    const char *reg_file;           /* where the values the game writes are kept when NFSU2_REGFILE names no
                                     * file: this file in $HOME (".nfsu2_registry", also when 0) */
    int sandbox;                    /* 1: the game sees its virtual root and nothing else: a path outside it
                                     * (another drive, the drive's own root, a UNC or device path, '..' above
                                     * it, a device name) names no file, and the game folder is never written
                                     * (fileio.c); 0 (UG2): every path maps into the game folder, as it
                                     * always has */
    int find_sorted;                /* 1: FindFirstFileA/FindNextFileA give a folder's names in NTFS order, as
                                     * Windows does (fileio.c; MW lists its aliases in that order); 0 (UG2):
                                     * in the host's order, as it always has */
    const char *const *lang_suffixes; /* a missing file "<name>_xx.<ext>" is looked for with each of
                                     * these in place of "_xx", in order (fileio.c); ends with 0 */
    void (*entry)(CPU *c);          /* the exe's entry point, lifted: the lift's GAMEINFO_ENTRY_FN (cpu.h) */
    uint32_t entry_va;              /* its address, GAMEINFO_ENTRY */
    const struct ModLink *modules;  /* the DLLs lifted next to the exe (module.h, module.c): their
                                     * imports follow the exe's in this order */
    int nmodules;

    /* ---- set-up and hooks ---- */
    void (*install)(void);          /* data patches, dispatch patches and feature set-up: after win32_init,
                                     * before the guest's entry point runs */
    int  (*hook)(CPU *c, uint32_t va);  /* every instruction hook the lift emitted (guest_hook,
                                     * debug.c); 1 = the site was one of the game's */

    /* ---- every frame ---- */
    void (*on_present)(CPU *c);     /* every Present, on the game's thread (com.c) */
    void (*on_frame)(CPU *c);       /* once per pass of the game's main loop: guest_hook (debug.c) runs it
                                     * at frame_site, on the game's thread, and counts the passes
                                     * (debug_frame_passes) */
    uint32_t frame_site;            /* the instruction of the main loop on_frame runs before (an instruction
                                     * hook the game's table marks for the lift), between the game's update
                                     * and its render; 0 = none (UG2: its features hook its GameLoop
                                     * themselves, through hook) */
    void (*on_test_frame)(void);    /* every Present, after the test frame counter moved and before the
                                     * test script's lines of that frame (autotest.c) */
    void (*status)(StatusOut *s);   /* the drive, pause, text message and free-roam state for the page */
    void (*sms_request)(int id);    /* the page asks for a text message of ours on the game's phone
                                     * (IEV_SMS, input_web.c, on the input thread), id 1.. */
    void (*photo_input)(int cmd, int a, int b); /* the page drives photo mode (IEV_PHOTO, input_web.c,
                                     * on the input thread): what (PH_*) and its two values */

    /* ---- the game's state, for core's logs, statistics and test scripts ---- */
    uint32_t (*flow)(void);         /* the game's flow state (its GameFlowManager), 0 = not known yet */
    uint32_t flow_fe, flow_race;    /* its values in the front end and while racing */
    const char *(*top_screen)(void);    /* the top front-end package's name ("UI_Main.fng"), or 0 */
    int (*fe_match)(const char *name);  /* the scripts' waitfng/tapuntil names with a qualifier after the
                                     * package's (':', '@', '\'' or '^', autotest.c fe_is) or alternatives
                                     * ('|'): 1 while the front end is in that state within the package (or
                                     * one of them); and "-PACKAGE": 1 once that package is gone; 0 = no such
                                     * names */
    uint32_t (*racer)(int human, int slot); /* test traces' $P (human 1: the local human racer) and $Rn
                                     * (racer slot n) of the current race, 0 = none */
    uint32_t (*fe_object)(const char *package, uint32_t hash); /* test expressions' $O(PACKAGE:HASH)
                                     * (autotest.c): the object of that name hash in the active front-end
                                     * package of that name, at any depth of its groups, 0 = none */
    uint32_t (*text_entry)(uint32_t *kind, uint32_t *len, uint8_t *text, uint32_t cap);
                                    /* while the game's text entry is up: the most it takes (0 = not up),
                                     * its kind, its text (cap bytes at most) and length */

    /* ---- the game's code and data, as core's helpers need them ---- */
    uint32_t crt_lo, crt_hi;        /* the exe's statically linked C runtime code: HEAPSTAT names the
                                     * first caller outside it (heap.c) */
    uint32_t crtheap_va, active_heap_va;    /* that C runtime's _crtheap and __active_heap, printed in
                                     * the exit report */
    void (*on_qpc)(CPU *c);         /* every QueryPerformanceCounter of the exe, after its return address
                                     * was taken (c->eip): a frame wait loop of the game may sleep there */
    uint32_t (*qpc_caller)(CPU *c); /* QPCWHO: the caller of the game's own clock wrapper when this QPC
                                     * returns into it, else 0 (then the caller is read at [ebp+4]) */
    uint32_t lan_obj, lan_start, lan_stop, lan_running;     /* the game's LAN server glue, for the
                                     * srvcycle test (module.c): the object (thiscall), its start(name) and
                                     * stop methods, the pointer to the server's IsServerRunning */
    const ResLabel *res_labels;     /* the settings menu's 6 resolution labels (com.c), 0 = leave them */
    const uint32_t (*d3d_modes)[2]; /* the display modes EnumAdapterModes lists, in order, up to a {0,0} row
                                     * (com.c); 0 = UG2's rows: its own 6-row table, with the 4K row's stand-in */
    int d3d9_strict;                /* 1: D3D9's documented answers where UG2's runtime keeps answers of its own
                                     * (com.c): GetLevelDesc and GetDesc give the level's size, the type, usage and
                                     * pool (a volume texture's level its D3DVOLUME_DESC), GetType names cube and
                                     * volume textures and buffers, CheckDeviceMultiSampleType and CheckDeviceFormat
                                     * offer only what the renderer draws and makes, a texture the renderer could not
                                     * make fails its CreateTexture; and the renderer's own limit: a back buffer past
                                     * 8192 a side, which it cannot resize to, fails Reset; 0 = UG2's */
    int vclock_sleep_real;          /* 1: a Sleep of the game's main thread moves the virtual clock by at least
                                     * its real length, also in a frame past the in-frame cap (win32.c), so a
                                     * speed test that sleeps between two clock reads sees its sleep; 0 = UG2's
                                     * clock, which stands at the cap there apart from its creep */
    void (*on_sound_unlock)(void);  /* every DirectSound buffer Unlock (com.c) */
    void (*on_sound_position)(void);    /* every DirectSound buffer GetCurrentPosition (com.c), on the
                                     * calling thread, before it answers */
    int sound_wall_catchup;         /* with no output to pace it (no running AudioContext, a stalled one,
                                     * NOSOUND) the mixer (runtime/web/audio_web.c) runs on the wall clock:
                                     * 1 makes up all the time its pump slept past and runs the DirectSound
                                     * cursors on between its slices, so they keep real time however coarse
                                     * its wake-ups are (a background tab's); 0 (UG2) drops a backlog over
                                     * 100 ms and moves them a slice at a time, as it always did */
    uint32_t online_host_va;        /* the Online server's host name in the exe ("*pcnfs05.ea.com") */
    const char *online_host;        /* that name without its '*' ("pcnfs05.ea.com") */

    /* ---- the game pad (dinput_pad.c: a controller as a DirectInput 8 joystick, offered once one exists) ---- */
    void (*pad_rescan)(int on);     /* the game's own search for controllers plugged in after its enumeration:
                                     * on while it should look (UG2: EnumDevices every 32 frames, pad_ug2.c);
                                     * 0 = it has none: the device stays one kind, whichever controller
                                     * drives it, and a late controller needs pad_late */
    const PadLate *pad_late;        /* how a game without a search takes a controller connected after its
                                     * enumeration found none (dinput_pad.c dipad_present); 0 = a controller
                                     * must be there when the game looks */
    const char *pad_buttons;        /* the pad buttons behind DirectInput's buttons 0..11, by name (A B X Y LB
                                     * RB BACK START LS RS, LT and RT the triggers pulled to the end), when the
                                     * game's own layout expects another controller's order; 0 = the pad's
                                     * order "A B X Y LB RB BACK START LS RS LT RT" (an Xbox pad's) */
    unsigned (*pad_trigger_axes)(uint32_t dev); /* which triggers of the IDirectInputDevice8 dev the game reads as
                                     * an axis now (bit 0 the left, bit 1 the right): their buttons press at the
                                     * end of the travel, the others' at half travel; 0 = both are axes */
    float (*pad_jolt)(void);        /* every poll of the pad with force feedback on: the game's jolt now, 0..1,
                                     * rumbled on top of its effects (UG2: the player car's hits); 0 = none */
    const char *pad_hold_screen;    /* the front-end screen (autotest_screen) that keeps an unplugged pad,
                                     * at rest, until it is left (UG2's controls page); 0 = none */
    int pad_fe_keys;                /* 1: in the front end (flow_fe) the pad's A, B and View also post Enter,
                                     * Escape and Q to the window (UG2's text fields and quit prompt take
                                     * characters only); 0 = the pad's buttons are DirectInput's alone */

    /* ---- the game's own Win32 imports ---- */
    int (*win32_misc)(CPU *c, const char *dll, const char *n, uint32_t *r);
                                    /* imports the shared layers (win32.c, win32_ext.c, fileio.c, winsock.c)
                                     * leave unimplemented: 1 and *r when it answers n.  Most Wanted's game
                                     * table sets win32_misc.c's win32_misc_call; UG2's leaves it 0, so the
                                     * ones its exe and server.dll import (CompareStringA/W, SetStdHandle,
                                     * SetEnvironmentVariableA) stay unimplemented, as they always were */

    /* ---- what the boot and the stall watchdog check (L-051, L-073) ---- */
    const InitTable *init_tables;   /* the C runtime's initializer tables, in the order they run: their
                                     * functions are counted as they run and one [init] line says how many
                                     * of each ran (dispatch.c); 0 = no line */
    int ninit_tables;
    uint32_t init_check;            /* a pointer the constructors set: the [init] line reads it, and 0 there
                                     * is a constructor that did not run its body; 0 = no check */
    const ThreadName *thread_names; /* the routines the game's threads run, named: the watchdog's report
                                     * names each thread and gives its eip as a function and offset, and
                                     * CreateThread logs which routine a new thread runs; 0 = the report
                                     * and the log as they always were */
    uint32_t (*thread_start)(uint32_t entry, uint32_t arg); /* the routine a thread CreateThread starts at
                                     * entry with arg runs, past the C runtime's and the game's own start
                                     * routines, read while CreateThread runs; 0 = entry */
} GameOps;

extern const GameOps g_game;

/* the game's flow state, 0 while not known (or no game) */
static inline uint32_t game_flow(void){ return g_game.flow ? g_game.flow() : 0u; }

/* the game's name for a thread that runs routine (thread_names), or 0 */
static inline const char *game_thread_name(uint32_t routine){
    for (const ThreadName *t = g_game.thread_names; t && t->start; t++) if (t->start == routine) return t->name;
    return 0;
}

#endif
