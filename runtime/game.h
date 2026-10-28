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
    void (*on_frame)(CPU *c);       /* once per pass of the game's main loop */
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
    uint32_t (*racer)(int human, int slot); /* test traces' $P (human 1: the local human racer) and $Rn
                                     * (racer slot n) of the current race, 0 = none */
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
    void (*on_sound_unlock)(void);  /* every DirectSound buffer Unlock (com.c) */
    uint32_t online_host_va;        /* the Online server's host name in the exe ("*pcnfs05.ea.com") */
    const char *online_host;        /* that name without its '*' ("pcnfs05.ea.com") */

    /* ---- the game's own Win32 imports ---- */
    int (*win32_misc)(CPU *c, const char *dll, const char *n, uint32_t *r);
                                    /* imports the shared layers (win32.c, win32_ext.c, fileio.c, winsock.c)
                                     * leave unimplemented: 1 and *r when it answers n.  Most Wanted's game
                                     * table sets win32_misc.c's win32_misc_call; UG2's leaves it 0, so the
                                     * ones its exe and server.dll import (CompareStringA/W, SetStdHandle,
                                     * SetEnvironmentVariableA) stay unimplemented, as they always were */
} GameOps;

extern const GameOps g_game;

/* the game's flow state, 0 while not known (or no game) */
static inline uint32_t game_flow(void){ return g_game.flow ? g_game.flow() : 0u; }

#endif
