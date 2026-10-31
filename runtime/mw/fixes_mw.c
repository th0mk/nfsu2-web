/* Most Wanted's own crash bugs, fixed defensively (TRACKER M4-28, M4-29, M4-53; L-063, D-24).  Each fix logs a line when
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
 *                             one of the game's own empty methods (0x410550, `ret`, in 61 of its vtables), and
 *                             a second (0x43E014) keeps that vehicle out of the roadblock's cars (below).  Should
 *                             the game reach _purecall anyway, a hook at its entry logs the caller.  The list is
 *                             a pending roadblock plan's cars, and the torn-down vehicle one freed while the plan
 *                             still lists it: NFSU2_TEST_PLANCARS below takes such cars off as they are freed, so
 *                             this guard is the second line.
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
 *   NFSU2_TEST_NAME_FORMAT=1  a player name with a % in it, taken as a printf format by the game room (0x573140
 *                             from three sites; sub_00573140 below): hashed as text instead (TRACKER M8-19, L-184);
 *                             and the local LAN name as the format of a front-end text (0x515D70 from 0x520C10;
 *                             sub_00515d70 below): printed through "%s" instead.
 *   NFSU2_TEST_PFLOCK=1       the interactive music's lock (B-008, B-009; sub_00834e6d below): a main-thread try
 *                             that finds Pathfinder's lock held waits for it instead of failing at once; 1 keeps
 *                             the game's try (and logs each that fails), 2 and 3 make the music slot's start find
 *                             it held (the crash at once), 4 and 5 its stream open or the music's pause / go-on
 *                             once (NFSU2_TEST_PFLOCK_AT: at which call; the music lost, M4-26).
 *   NFSU2_TEST_PLANCARS=1     a cop car freed while a pursuit's plan (a roadblock, a head-on ram) still lists it
 *                             (M4-53, M4-28; sub_0042e570 below): the plan's next walk calls into freed memory.
 *                             The fix takes the car off the plans as the cop manager lets it go; 1 leaves it
 *                             there; 2 also makes the freed block hold something else when a walk first meets it
 *                             (the trap of run c5-s7b).  Each walk that meets such a car is logged either way.
 * Each fix logs its first 20 hits ("[fix] f=N ..."). */
#include "cpu.h"
#include "game.h"
#include "memmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

/* M4-28: the hook at 0x43E005 (game.h hook, through debug_mw.c); its runs are counted for the test aids (the
 * Repro check: how often the guarded call ran, pursuit_mw.c's [mwpursuit] line) */
static unsigned long purecall_site_runs, purecall_passed, purecall_kept_out;
unsigned long mw_fix_purecall_calls(void){ return purecall_site_runs; }
/* the guard's acts: vehicles passed over at 0x43E005 (0) and their adds at 0x43E014 sent to the stand-in (1) */
unsigned long mw_fix_purecall_acts(int add){ return add ? purecall_kept_out : purecall_passed; }
/* The vehicle passed over at 0x43E005 is then added to the new roadblock's cars (0x43E00B..0x43E014: a push_back,
 * 0x43C4E0, of ebp into the vector at esp+0x48), whose later calls on it end in _purecall all the same (run a6 of
 * 2026-10-30: R6025 in the frame of the guard's line, in a roadblock storm at heat 3).  So its add goes to a stand-in
 * vector instead (one slot, emptied before each use): the roadblock goes up without that car. */
#define PURECALL_ADD_SITE HOOK_SITE(0x0043e014)
static uint32_t passed_over;            /* the vehicle the guard just passed over (0: none) */
static int purecall_add_hook(CPU *c){
    static uint32_t vec;
    if (!passed_over || c->ebp != passed_over){ passed_over = 0; return 1; }
    passed_over = 0;
    if (!vec){
        vec = com_scratch(0x20u);       /* {?, begin, end, capacity end} and the one slot at +0x10 */
        if (!vec) return 1;
        MR32(vec + 4u) = vec + 0x10u; MR32(vec + 0xcu) = vec + 0x14u;
    }
    MR32(vec + 8u) = vec + 0x10u;       /* empty */
    c->ecx = vec;
    purecall_kept_out++;
    return 1;
}
/* _purecall itself (the CRT's, 0x7C5200): the game is about to stop with R6025; say who called it */
#define CRT_PURECALL_SITE HOOK_SITE(0x007c5200)
static void purecall_caller(CPU *c){
    char b[160]; int o = 0;
    for (uint32_t k = 1; k <= 24 && o < (int)sizeof b - 12; k++){   /* return addresses in the game's code above it */
        uint32_t v = MR32(c->esp + 4u * k);
        if (v >= 0x00401000u && v < 0x007d0000u) o += snprintf(b + o, sizeof b - (size_t)o, " 0x%08x", v);
    }
    fprintf(stderr, "[fix] f=%ld _purecall (R6025) called from 0x%08x, ecx 0x%08x; above it:%s\n", autotest_frame(),
            MR32(c->esp), c->ecx, o ? b : " none");
}
static void plan_car_check(uint32_t car, uint32_t plan, const char *where);
int mw_fix_hook(CPU *c, uint32_t va){
    if (va == PURECALL_ADD_SITE) return purecall_add_hook(c);
    if (va == CRT_PURECALL_SITE){ purecall_caller(c); return 1; }
    if (va != PURECALL_SITE) return 0;
    static int off = -1, hits;
    static uint32_t stand_in;
    purecall_site_runs++;
    passed_over = 0;
    plan_car_check(c->ecx, 0, "a roadblock try's call 0x43E005");
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
                "_purecall (R6025); passed over, and kept out of the roadblock's cars\n", autotest_frame(), c->ecx, c->edx);
    c->edx = stand_in;
    passed_over = c->ecx;               /* ecx = ebp, the vehicle: its add at 0x43E014 goes to the stand-in vector */
    purecall_passed++;
    return 1;
}

/* M4-53 (B-013), M4-28: a cop car freed while a pursuit's plan still lists it.  AIPursuit (its base built by
 * 0x442D00; base+8 is `this` of its update 0x443BA0, base+0x48 its IPursuit, which the list at 0x92FD28 holds: begin
 * [0x92FD2C], count [0x92FD34]) has one plan at base+0x1DC: +0 the kind's record ([[plan]]: 1 and 2 support, 3 a
 * head-on ram, 4 a roadblock), +0x74 a timer, +0x78 the state (0 none, 1 pending, 2 made) and the plan's cars, a
 * vector of IVehicles at +8 (+0xC its begin, +0x14 its count).  The cars are the cop manager's: 0x41F130 fills a plan
 * with cars from 0x41ECD0 (a free car of its pool, else a new one; "copsuv" or "copsuvl", each given the kind's goal),
 * and a roadblock's cars need not be in the pursuit at all.  Only the plan's end clears the vector (0x42B410: from the
 * update when the timer runs out, 0x443ED5; from the pursuit's OnDetached 0x42B550 when its count of cops holding a
 * goal, base+0x1D4, reaches 0; from 0x42BA42 and 0x4438C0), and nothing takes a car off it when the car is freed: the
 * cop manager, attached to each of its cars (its IAttachable at its base+0x38, vtable 0x891940; the base is
 * [0x92C67C]-0x54), only takes a freed car off its own list (OnDetached 0x42E570: the vector at base+0xC4, begin +0xC8,
 * count +0xD0), and the pursuit's OnDetached lowers its count but leaves the car on the plan.  The plan's next walk
 * then calls into freed memory: its end queries each car's object (0x5D59F0 on [car+4]: the trap of run c5-s7b when
 * the memory held something else) unless the plan is a roadblock already made, and a pending roadblock's try (0x43DD30
 * from 0x43E7D1) calls each car's +0x80 (the R6025 of M4-28 when the car's interfaces are torn down; the guard at
 * 0x43E005 above).  So as the cop manager lets a car go, it is taken off every pursuit's plan too, before the game's
 * own OnDetached runs; a car that only leaves the pursuit and lives on stays on the plan, as in the game.
 * The let-go is the car's teardown: the cop manager's OnDetached runs under the car's Attachments destructor
 * (0x6FC560, from PVehicle's deleting destructor 0x689570: the destructor 0x687E40, then the 0x1AC bytes back to the
 * pool, whose next car of that size often gets the same address).  NFSU2_TEST_PLANCARS=1 keeps the game's way (2: see
 * plancars_level).  Either way each walk of a plan's cars that meets a car the cop manager no longer holds is logged
 * (the plan's end, the roadblock try's 0x43E005): with the fix there are none. */
#define PURSUITS         0x0092fd2cu    /* the IPursuit list: its begin; the count at 0x92FD34 */
#define PURSUITS_N       0x0092fd34u
#define PURSUIT_IFACE    0x48u          /* IPursuit, from the pursuit's base */
#define PURSUIT_VT8      0x008928e4u    /* the vtable at base+8 */
#define PURSUIT_PLAN     0x1dcu         /* the plan, from the base */
#define PURSUIT_HELD     0x1d4u         /* the count of cops holding a goal */
#define PLAN_STATE       0x78u
#define PLAN_CARS        0x0cu          /* the plan's cars: the vector's begin; its count at +0x14 */
#define PLAN_CARS_N      0x14u
#define PLAN_MAX         64u            /* more cars than this: not a plan (left to the game) */
#define COPMGR           0x0092c67cu    /* the cop manager's interface (vtable 0x891900): its base + 0x54 */
#define COPMGR_IFACE     0x54u
#define COPMGR_ATT       0x38u          /* its IAttachable, from the base */
#define COPMGR_CARS      0xc8u          /* its cars (IVehicles): the vector's begin; the count at +0xD0 */
#define COPMGR_CARS_N    0xd0u
#define FN_QUERY         0x005d59f0u    /* an object's interface by its id (thiscall on the object, (id)) */
#define IID_IVEHICLE     0x004040e0u    /* the id of IVehicle */
static uint32_t rd32(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static int on_list(uint32_t b, uint32_t n, uint32_t max, uint32_t v){
    for (uint32_t i = 0; b && i < n && i < max; i++) if (rd32(b + 4u * i) == v) return 1;
    return 0;
}
static uint32_t copmgr_base(void){ uint32_t i = rd32(COPMGR); return i ? i - COPMGR_IFACE : 0u; }
/* return addresses in the game's code on the stack above the caller's (the first `max`) */
static void game_returns(CPU *c, char *b, size_t n, int max){
    int o = 0, k = 0;
    b[0] = 0;
    for (uint32_t i = 1; i <= 48 && k < max && o < (int)n - 12; i++){
        uint32_t v = rd32(c->esp + 4u * i);
        if (v >= 0x00401000u && v < 0x007d0000u){ o += snprintf(b + o, n - (size_t)o, " 0x%08x", v); k++; }
    }
}
/* 1: the cop manager holds the car (it is not freed); also 1 when there is no cop manager to ask */
static int copmgr_holds(uint32_t car){
    uint32_t m = copmgr_base();
    return !m || on_list(rd32(m + COPMGR_CARS), rd32(m + COPMGR_CARS_N), 4096u, car);
}
#define N_GONE 32
static struct { uint32_t car; long f; int reused; } gone[N_GONE];   /* the cars the cop manager let go last */
static unsigned n_gone;
static unsigned long plancars_taken, plancars_dead;
unsigned long mw_fix_plancars(void){ return plancars_taken; }
unsigned long mw_fix_plancars_dead(void){ return plancars_dead; }
#define VT_IVEHICLE      0x008aa828u    /* a cop car's IVehicle vtable */
#define FOREIGN_ADDR     0x7fff0000u    /* past the guest's memory (a float's bits, say) */
/* NFSU2_TEST_PLANCARS=2: as 1, and the first walk that meets a freed car finds its block holding something else (the
 * pool hands a freed car's 0x1AC bytes to the next allocation of that size): its object pointer [car+4] is set to an
 * address past the guest's memory, once per car, so the walk's own read of it shows what a reused block gives */
static int plancars_level(void){
    static int lv = -1;
    if (lv < 0){ const char *e = getenv("NFSU2_TEST_PLANCARS"); lv = e && *e >= '1' && *e <= '2' ? *e - '0' : 0; }
    return lv;
}
/* a walk of a plan's cars meets `car`: one the cop manager no longer holds is logged */
static void plan_car_check(uint32_t car, uint32_t plan, const char *where){
    static int lines;
    if (!car || copmgr_holds(car)) return;
    plancars_dead++;
    long f = -1;
    int g = -1;
    for (unsigned i = 0; i < n_gone && i < N_GONE; i++) if (gone[i].car == car){ f = gone[i].f; g = (int)i; }
    if (plancars_level() == 2 && g >= 0 && !gone[g].reused && rd32(car) == VT_IVEHICLE){
        gone[g].reused = 1;
        fprintf(stderr, "[fix] f=%ld TEST: freed car %08x (let go at f=%ld) is met by %s: its block now holds something "
                "else ([car+4] %08x -> %08x)\n", autotest_frame(), car, f, where, rd32(car + 4u), FOREIGN_ADDR);
        MR32(car + 4u) = FOREIGN_ADDR;
    }
    if (lines++ < 40)
        fprintf(stderr, "[fix] f=%ld %s meets car %08x of a plan (kind %u, state %u), which the cop manager let go at f=%ld: "
                "vtable %08x, object %08x\n", autotest_frame(), where, car, plan ? rd32(rd32(plan)) : 0u,
                plan ? rd32(plan + PLAN_STATE) : 0u, f, rd32(car), rd32(car + 4u));
}
/* the cop manager's OnDetached (its IAttachable +0x14), thiscall (the other's IAttachable): a car of its own lets go */
void sub_0042e570_orig(CPU *c);
void sub_0042e570(CPU *c){
    static int hits;
    uint32_t att = c->ecx, other = MR32(c->esp + 4u), car = 0;
    uint32_t pb = rd32(PURSUITS), pn = rd32(PURSUITS_N);
    if (att == copmgr_base() + COPMGR_ATT && other && rd32(other + 4u) && pb && pn && pn <= PLAN_MAX){
        uint32_t iid = IID_IVEHICLE;    /* what the game's OnDetached asks first (0x42E57D) */
        c->ecx = rd32(other + 4u);
        car = call_guest(c, FN_QUERY, 1, &iid);
        c->ecx = att;
    }
    if (car){
        gone[n_gone % N_GONE].car = car; gone[n_gone % N_GONE].f = autotest_frame(); gone[n_gone % N_GONE].reused = 0;
        n_gone++;
        int keep = plancars_level() > 0;
        for (uint32_t k = 0; k < pn; k++){
            uint32_t base = rd32(pb + 4u * k) - PURSUIT_IFACE, plan = base + PURSUIT_PLAN;
            uint32_t b = rd32(plan + PLAN_CARS), n = rd32(plan + PLAN_CARS_N);
            if (rd32(base + 8u) != PURSUIT_VT8 || !b || !n || n > PLAN_MAX || !on_list(b, n, PLAN_MAX, car)) continue;
            uint32_t w = 0;
            if (!keep){
                for (uint32_t i = 0; i < n; i++){
                    uint32_t v = MR32(b + 4u * i);
                    if (v != car) MR32(b + 4u * w++) = v;
                }
                MR32(plan + PLAN_CARS_N) = w;
                plancars_taken++;
            }
            if (hit_log(&hits)){
                char st[200];
                game_returns(c, st, sizeof st, 8);
                fprintf(stderr, "[fix] f=%ld %scop manager lets car %08x (vtable %08x) go while pursuit %08x's plan lists it "
                        "(kind %u, state %u, %u cars, %u cops holding a goal): %s; from 0x%08x%s\n", autotest_frame(),
                        keep ? "TEST: " : "", car, rd32(car), base, rd32(rd32(plan)), rd32(plan + PLAN_STATE), n,
                        rd32(base + PURSUIT_HELD), keep ? "it stays on the plan (the game's way)" :
                        "taken off the plan's cars (M4-53)", MR32(c->esp), st);
            }
        }
    }
    sub_0042e570_orig(c);
}
/* the plan's end, 0x42B410 (thiscall on the plan): its walk of the cars (0x42B460..0x42B4D8, not for a roadblock
 * already made) */
void sub_0042b410_orig(CPU *c);
void sub_0042b410(CPU *c){
    uint32_t plan = c->ecx, b = rd32(plan + PLAN_CARS), n = rd32(plan + PLAN_CARS_N), kind = rd32(plan);
    int walks = !(rd32(plan + PLAN_STATE) == 2u && kind && rd32(kind) == 4u);
    if (walks && b && n <= PLAN_MAX && rd32(plan - PURSUIT_PLAN + 8u) == PURSUIT_VT8)
        for (uint32_t i = 0; i < n; i++) plan_car_check(rd32(b + 4u * i), plan, "the plan's end 0x42B410");
    sub_0042b410_orig(c);
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

/* M8-19 (L-184): player names as printf formats.  0x573140 (cdecl (format, ...)) prints into a 256-byte buffer on
 * its stack (vsprintf 0x4643D0) and returns the string hash (0x460BF0) of what it printed.  Three callers in the LAN
 * and Online game room (the function around 0x555C80, its player rows) hand it a name as the format, with no
 * arguments: 0x555C9E the local player's LAN name (0x56E340), 0x555CE2 the name at +0x2A of the record at [+0x54],
 * 0x55601B the name at +0x2A of each player of the room's list (other players' names, from the network).  A name
 * with a % in it is then a format: %s and %n read and write through whatever is on the stack, %300d runs past the
 * 256 bytes.  At those three sites a name with a % is hashed as the text it is (0x460BF0 on the name, what the
 * printing gives for a name without one); a name without a % takes the game's own path.  NFSU2_TEST_NAME_FORMAT=1
 * brings back the game's way.  NFSU2_FNG_LOG=3 logs the first names each site hashes. */
#define FN_NAME_HASH   0x00460bf0u      /* the string hash, cdecl (const char *) -> eax */
#define NAMEFMT_N      3
static const uint32_t namefmt_ret[NAMEFMT_N] = { 0x00555ca3u, 0x00555ce7u, 0x00556020u };   /* after the calls */
void sub_00573140_orig(CPU *c);
void sub_00573140(CPU *c){
    uint32_t ret = MR32(c->esp), fmt = MR32(c->esp + 4u);
    int site = -1;
    for (int i = 0; i < NAMEFMT_N; i++) if (ret == namefmt_ret[i]) site = i;
    if (site < 0 || !fmt){ sub_00573140_orig(c); return; }
    char name[64]; int n = 0, pct = 0;
    for (; n < 63 && mem_is_committed(fmt + (uint32_t)n) && g_mem[fmt + (uint32_t)n]; n++){
        name[n] = (char)g_mem[fmt + (uint32_t)n];
        pct |= name[n] == '%';
    }
    name[n] = 0;
    for (int k = n; k < 256 && !pct && mem_is_committed(fmt + (uint32_t)k) && g_mem[fmt + (uint32_t)k]; k++)
        pct = g_mem[fmt + (uint32_t)k] == '%';
    static int lv = -1, seen[NAMEFMT_N], off = -1, hits;
    if (lv < 0){ const char *v = getenv("NFSU2_FNG_LOG"); lv = v ? atoi(v) : 0; }
    if (lv >= 3 && seen[site]++ < 4)
        fprintf(stderr, "[fix] f=%ld game room: name hash at 0x%08x of \"%s\"\n", autotest_frame(), ret - 5u, name);
    if (!pct || test_switch("NFSU2_TEST_NAME_FORMAT", &off)){
        if (pct && hit_log(&hits))
            fprintf(stderr, "[fix] f=%ld TEST: the name \"%s\" is a printf format at 0x%08x (the game's way)\n",
                    autotest_frame(), name, ret - 5u);
        sub_00573140_orig(c);
        return;
    }
    uint32_t h = call_guest(c, FN_NAME_HASH, 1, &fmt);
    if (hit_log(&hits))
        fprintf(stderr, "[fix] f=%ld the name \"%s\" holds a %%: hashed as text at 0x%08x, not printed as a format\n",
                autotest_frame(), name, ret - 5u);
    POP32(c->eip);                      /* the game's `ret` (cdecl: the caller pops the format) */
    c->eax = h;
}

/* B-008, B-009: Pathfinder's lock.  The interactive music library (EA Pathfinder, its globals about 0x9C2954..0x9C29C0)
 * guards its state with a semaphore object at [0x9C29B0], taken through EA's Semaphore::Wait (0x834E6D, thiscall
 * (const ThreadTime *absolute timeout)) with the timeout 0 at every entry point (0x829410 for 23 of them, 8 more
 * inline): a try.  When another thread holds it the call returns at once with -18 and does nothing.  The sound
 * thread's service (0x82E7E0 -> 0x82E710) takes it the same way and skips a turn when it cannot, but the game's music
 * code on the main thread does not look: the music manager's slot start 0x4BF6A0 makes the slot's stream object with
 * 0x82D0B0 and then marks the slot playing (0x4B6600 sets +4 whether or not an object came), so a held lock at that
 * moment leaves the slot playing with no stream object, and two later calls go through the empty pointer: the slot's
 * volume (0x4F0660 -> 0x82D560, `call [edi+0x30]` at 0x82D605; B-008) and the world's teardown (0x4B65B0: the
 * object from 0x8298D0, `call [edx+0x90]` at 0x4B65E3; B-009).  On Windows a thread is preempted inside that short
 * locked stretch only rarely.  Here guest threads take turns under one big lock and every Win32 call hands it on
 * (guest_fair_yield), WaitForSingleObject and ReleaseSemaphore too, so a thread that has just taken Pathfinder's
 * semaphore often lets the other run before it gets on, and the other's try fails (about 1 in 30 of the sound
 * thread's, and some of the main thread's, in runs of 2026-10-30).  So a try on the main thread that finds the lock
 * held waits for it, up to PF_WAIT_MS of real time, and only then gives the game its "busy"; the sound thread keeps
 * the game's try.  NFSU2_TEST_PFLOCK=1 keeps the game's try on the main thread too; NFSU2_TEST_PFLOCK=2 makes the
 * slot start's try (0x82D0B0's) find the lock held every time, which brings B-008 in the front end's music; =3 does
 * that from the second slot start on (the first world's music).  =4 makes the slot start's stream open (0x82D460)
 * find it held once, at its NFSU2_TEST_PFLOCK_AT-th call (3 when not set), and =5 the music object's pause or go-on
 * (0x829140): TRACKER M4-26's music lost after a shop visit. */
#define PF_LOCK_OBJ   0x009c29b0u       /* the Semaphore object: +0 its handle, +4 its count */
#define FN_TICKS      0x008352b3u       /* GetTickCount, the clock EA's Semaphore::Wait measures an absolute time on */
#define PF_WAIT_MS    500u
#define PF_SLOT_OBJECT 0x0082d0b9u      /* 0x829410's return in 0x82D0B0 (the stream object): NFSU2_TEST_PFLOCK=2 */
#define PF_SLOT_OPEN  0x0082d468u       /* ... in 0x82D460 (the slot start's stream open): NFSU2_TEST_PFLOCK=4 */
#define PF_PAUSE      0x0082914bu       /* ... in 0x829140 (pause or go on, the music object's states): =5 */
static unsigned long pf_waits, pf_gaveup;
void sub_00834e6d_orig(CPU *c);
void sub_00834e6d(CPU *c){
    static _Thread_local int inner;
    static int off = -1, hits;
    uint32_t sem = c->ecx, pt = MR32(c->esp + 4u);
    if (inner || c->fs_base != MM_TEB_MAIN || !sem || sem != MR32(PF_LOCK_OBJ) || !pt || MR32(pt) != 0u){
        sub_00834e6d_orig(c);
        return;
    }
    uint32_t from = MR32(c->esp);
    static int force = -1, starts, at, calls;
    if (force < 0){
        const char *e = getenv("NFSU2_TEST_PFLOCK"), *n = getenv("NFSU2_TEST_PFLOCK_AT");
        force = e && *e >= '2' && *e <= '5' ? *e - '0' : 0;
        at = n && atoi(n) > 0 ? atoi(n) : 3;
    }
    uint32_t above = MR32(c->ebp + 4u);
    if ((force == 2 || force == 3) && above == PF_SLOT_OBJECT && (force == 2 || ++starts >= 2)
        || force == 4 && above == PF_SLOT_OPEN && ++calls == at
        || force == 5 && above == PF_PAUSE && ++calls == at){   /* held, as when the sound thread has it */
        static int nf;
        if (nf++ < 20)
            fprintf(stderr, "[fix] f=%ld TEST: the main thread's try (above 0x%08x, from the game's 0x%08x) finds Pathfinder's "
                    "lock held\n", autotest_frame(), above, MR32(MR32(c->ebp) + 4u));
        POP32(c->eip);
        c->esp += 4u;                   /* ret 4 */
        c->eax = 0xfffffffeu;           /* -2: the semaphore's wait timed out */
        return;
    }
    sub_00834e6d_orig(c);               /* the game's try */
    if ((int32_t)c->eax != -2 || test_switch("NFSU2_TEST_PFLOCK", &off)){
        /* the test logs each failed try (up to 400) with the game's call into Pathfinder: through 0x829410 (its
         * return 0x829439) that is one frame further up */
        static int nfail;
        if ((int32_t)c->eax == -2 && nfail++ < 400)
            fprintf(stderr, "[fix] f=%ld TEST: Pathfinder's lock is held: the main thread's try (0x834E6D from 0x%08x, "
                    "above 0x%08x, from the game's 0x%08x) fails (the game's way)\n", autotest_frame(), from,
                    MR32(c->ebp + 4u), from == 0x00829439u ? MR32(MR32(c->ebp) + 4u) : MR32(c->ebp + 4u));
        return;
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint32_t keep_ecx = c->ecx;
    MR32(pt) = call_guest(c, FN_TICKS, 0, 0) + PF_WAIT_MS;     /* the caller's timeout, an absolute time */
    c->ecx = sem;
    inner = 1;
    uint32_t r = call_guest(c, 0x00834e6du, 1, &pt);           /* EA's Wait again, now a wait */
    inner = 0;
    c->ecx = keep_ecx;
    MR32(pt) = 0u;
    clock_gettime(CLOCK_MONOTONIC, &b);
    pf_waits++;
    if ((int32_t)r == -2) pf_gaveup++;
    if (hit_log(&hits) || ((int32_t)r == -2 && pf_gaveup <= 50u))
        fprintf(stderr, "[fix] f=%ld Pathfinder's lock was held: the main thread's try (0x834E6D from 0x%08x, above "
                "0x%08x) waited %.1f ms: %s (%lu waits, %lu still held)\n", autotest_frame(), from, MR32(c->ebp + 4u),
                (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6,
                (int32_t)r == -2 ? "still held, busy as the game's" : "taken", pf_waits, pf_gaveup);
    c->eax = r;
}

/* M8-19 (L-184), the fourth site: 0x515D70 (cdecl (FE string, format, ...), the front end's printf into a text
 * object) is called at 0x520C10 with the player's own LAN name (0x56E320: [0x91CF90]+0x134, what was typed on
 * UI_PC_LAN.fng) as the format and no arguments.  A name with a % in it is printed through "%s" instead, the text it
 * is; NFSU2_TEST_NAME_FORMAT=1 brings back the game's way. */
#define FEPRINTF        0x00515d70u
#define FEPRINTF_NAME   0x00520c15u     /* the return address of the call at 0x520C10 */
void sub_00515d70_orig(CPU *c);
void sub_00515d70(CPU *c){
    if (MR32(c->esp) != FEPRINTF_NAME){ sub_00515d70_orig(c); return; }
    uint32_t obj = MR32(c->esp + 4u), fmt = MR32(c->esp + 8u);
    int pct = 0;
    char name[64]; int n = 0;
    for (int k = 0; fmt && k < 256 && mem_is_committed(fmt + (uint32_t)k) && g_mem[fmt + (uint32_t)k]; k++){
        char ch = (char)g_mem[fmt + (uint32_t)k];
        pct |= ch == '%';
        if (n < 63) name[n++] = ch;
    }
    name[n] = 0;
    static int off = -1, hits;
    static uint32_t as_text;            /* "%s" in the guest's memory */
    if (!pct || test_switch("NFSU2_TEST_NAME_FORMAT", &off) || (!as_text && !(as_text = com_scratch(4u)))){
        if (pct && hit_log(&hits))
            fprintf(stderr, "[fix] f=%ld TEST: the LAN name \"%s\" is a front-end printf format at 0x00520c10 (the game's "
                    "way)\n", autotest_frame(), name);
        sub_00515d70_orig(c);
        return;
    }
    MR8(as_text) = '%'; MR8(as_text + 1u) = 's'; MR8(as_text + 2u) = 0;
    uint32_t args[3] = { obj, as_text, fmt };
    uint32_t r = call_guest(c, FEPRINTF, 3, args);
    if (hit_log(&hits))
        fprintf(stderr, "[fix] f=%ld the LAN name \"%s\" holds a %%: printed as text at 0x00520c10, not as a format\n",
                autotest_frame(), name);
    POP32(c->eip);                      /* the game's `ret` (cdecl: the caller pops its two arguments) */
    c->eax = r;
}
