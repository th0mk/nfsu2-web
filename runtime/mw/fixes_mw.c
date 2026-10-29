/* Most Wanted's own crash bugs, fixed defensively (TRACKER M4-28, M4-29; L-063, D-24).  Each fix logs a line when
 * it acts and has an NFSU2_TEST_* switch that brings back the game's code, so a run can show the crash it removes:
 *
 *   NFSU2_TEST_PURECALL=1     the pursuit purecall (R6025).  The roadblock spawner (the function around 0x43E005,
 *                             3/3/4/4/5 cars by heat) calls method +0x80 of every vehicle on the list it is
 *                             handed (0x43E005 `call [edx+0x80]`, ecx the vehicle).  A vehicle on that list that
 *                             is already being torn down has a base class's vtable, whose +0x80 is the CRT's
 *                             _purecall (0x7C5200): the game stops with R6025.  The fix skips that one call
 *                             when, and only when, its target is _purecall: the vehicle is passed over (as with
 *                             the community's guard at the same site, a roadblock may then lack a car).  An
 *                             instruction hook (HOOK_SITE below) points edx at a stand-in table whose +0x80 is
 *                             one of the game's own empty methods (0x410550, `ret`, in 61 of its vtables).
 *   NFSU2_TEST_COPLIST_NULL=1 the cop and roadblock unload, 0x57D0F0: it calls method +0xC of every entry of
 *                             the list at [0x92FD8C] (count [0x92FD94]) and an empty (null) entry is called
 *                             through (0x57D105 `mov edx,[ecx]`): on Windows an access violation, here a call
 *                             to address 0 (UNRESOLVED INDIRECT CALL).  The fix passes a null entry over.  The
 *                             loop is the game's own otherwise: the end is read again after every call, as the
 *                             game's code does (a call may take its own entry off the list).
 *   NFSU2_TEST_ATTRIB_NULLKEY=1  Attrib::StringToLowerCaseKey (0x4549B0, cdecl (const char *)) reads its string
 *                             at 0x4549BF with no null check: on Windows an access violation for a null name.
 *                             The fix hands it the empty string instead, which gives the empty name's key (0).
 *                             (This port reads guest address 0 as zeros, so here the game's code gives the same
 *                             key; the fix keeps it from depending on that, and says when it happens.)
 * Each fix logs its first 20 hits ("[fix] f=N ..."). */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>

long autotest_frame(void);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t com_scratch(uint32_t n);
void sub_0057d0f0_orig(CPU *c);
void sub_004549b0_orig(CPU *c);

#define HOOK_SITE(va) (va##u)
#define PURECALL_SITE  HOOK_SITE(0x0043e005)
#define CRT_PURECALL   0x007c5200u      /* _purecall: the pure slots of the game's interface vtables */
#define GAME_NULLSUB   0x00410550u      /* `ret`: an empty method of the game's own (61 vtables use it) */
#define COPLIST        0x0092fd8cu      /* the list's first entry; its count at +8 */
#define COPLIST_N      0x0092fd94u

/* 1 = the switch asks for the game's own code */
static int test_switch(const char *name, int *cache){
    if (*cache < 0){ const char *e = getenv(name); *cache = e && *e == '1'; }
    return *cache;
}
static int hit_log(int *n){ return (*n)++ < 20; }

/* M4-28: the hook at 0x43E005 (game.h hook, through debug_mw.c) */
int mw_fix_hook(CPU *c, uint32_t va){
    if (va != PURECALL_SITE) return 0;
    static int off = -1, hits;
    static uint32_t stand_in;
    if (MR32(c->edx + 0x80u) != CRT_PURECALL) return 1;
    if (test_switch("NFSU2_TEST_PURECALL", &off)){
        if (hit_log(&hits))
            fprintf(stderr, "[fix] f=%ld TEST: pursuit purecall: vehicle 0x%08x (vtable 0x%08x) at 0x43E005 calls "
                    "_purecall (the game's way)\n", autotest_frame(), c->ecx, c->edx);
        return 1;
    }
    if (!stand_in){
        stand_in = com_scratch(0x84u);
        if (!stand_in) return 1;
        MR32(stand_in + 0x80u) = GAME_NULLSUB;
    }
    if (hit_log(&hits))
        fprintf(stderr, "[fix] f=%ld pursuit purecall: vehicle 0x%08x (vtable 0x%08x) at 0x43E005 would call "
                "_purecall (R6025); passed over\n", autotest_frame(), c->ecx, c->edx);
    c->edx = stand_in;
    return 1;
}

/* M4-29: the cop and roadblock unload, 0x57D0F0 */
void sub_0057d0f0(CPU *c){
    static int off = -1, hits;
    if (test_switch("NFSU2_TEST_COPLIST_NULL", &off)){ sub_0057d0f0_orig(c); return; }
    uint32_t p = MR32(COPLIST), end = p + 4u * MR32(COPLIST_N);
    c->eax = MR32(COPLIST_N);           /* the game's eax when the list is empty */
    while (p != end){
        uint32_t obj = MR32(p);
        if (!obj){
            if (hit_log(&hits))
                fprintf(stderr, "[fix] f=%ld cop unload: entry %u of the list at 0x92FD8C is null (0x57D105); "
                        "passed over\n", autotest_frame(), (p - MR32(COPLIST)) / 4u);
        } else {
            uint32_t keep = c->ecx;
            c->ecx = obj;
            call_guest(c, MR32(MR32(obj) + 0xcu), 0, 0);
            c->ecx = keep;
        }
        p += 4u;
        end = MR32(COPLIST) + 4u * MR32(COPLIST_N);
        c->eax = MR32(COPLIST);         /* ... and after an entry */
    }
    POP32(c->eip);                      /* the game's `ret` */
}

/* M4-29: Attrib::StringToLowerCaseKey(const char *), 0x4549B0 */
void sub_004549b0(CPU *c){
    static int off = -1, hits;
    static uint32_t empty;
    if (MR32(c->esp + 4u) || test_switch("NFSU2_TEST_ATTRIB_NULLKEY", &off)){ sub_004549b0_orig(c); return; }
    if (!empty && !(empty = com_scratch(16u))){ sub_004549b0_orig(c); return; }
    MR32(empty) = 0;
    if (hit_log(&hits))
        fprintf(stderr, "[fix] f=%ld Attrib::StringToLowerCaseKey(null) from 0x%08x (0x4549BF): the empty name "
                "instead\n", autotest_frame(), MR32(c->esp));
    MR32(c->esp + 4u) = empty;          /* the caller's argument slot, put back below (cdecl: the caller pops it) */
    sub_004549b0_orig(c);
    MR32(c->esp) = 0u;                  /* after its `ret`, esp is at the argument */
}
