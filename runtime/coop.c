/* coop.c -- co-op free roam: a second local player in player 1's career free roam, on half of the screen.
 *
 * Player 2 is a guest in player 1's career.  It gets the game's own Player and car, made while free roam runs:
 * RacerInfo::Init 0x6373E0 (index 1, a human's record), AddRacer 0x601600, CreateCar 0x606480 with driver 1,
 * World::IndexCars 0x5F0D20, the Player ctor 0x5F1670 on port 1 and AttachCar 0x5F3E10.  The game mode stays the
 * career's (1): split screen's mode 4 turns the career off (Career::InFreeRoam 0x500B60).  The local-human count
 * [0x89E7C0] reads 2 only around the ctor, which then gives player 2 view 2 and a 2P HUD of its own (with one, it
 * takes view 1 and a second HUD_SingleRace, whose end frees what player 1's uses); 87 places read that count, and
 * free roam's own bundle loads only with one human, so it is 1 again right after.
 *
 * Nothing of player 2's is saved.  The career stays player 1's: player 2's Player skips the markers' and triggers'
 * step 0x5EB040 (events, shops, doors, story triggers, the Engage target), so it opens nothing; the hot-position
 * jump, the outrun challengers and the GPS are player 1's in the game already (PlayersByOrder[0]).
 *
 * A world's end (a shop, the crib, a race) frees player 2's Player and car with the rest (the World's teardown
 * frees Players from the last); while co-op is on, player 2 comes back beside player 1 once free roam has settled.
 *
 * Input: player 2's port is 1.  splitscreen.c's port override (sub_005cfd60) gives it the seat of player 2's
 * device: a controller (its local seat, START pressed on it) or the keyboard's WASD half (split screen's layout).
 * With a controller, player 1 keeps the game's own input on port 0 (keys as set in Options, player 1's controller:
 * the page lets player 2's drive nothing else, IM_LOCAL_COOP).  With the keyboard shared, player 1 has the arrows'
 * half as well, and the game's own keys but player 2's (dinput_kbd.c hides those from the game's keyboard).
 */
#include "cpu.h"
#include "splitscreen_mem.h"
#include "gamepad.h"
#include "kbd.h"
#include "coop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);

#define G_FLOW        0x008654a4u   /* GameFlowManager state: 6 in the world */
#define G_GAMEMODE    0x00850078u   /* 1 career */
#define G_WORLD       0x00890080u   /* World*: +0 car list, +0x10 cars, +0x14/+0x18 AI range */
#define G_RACEPARAMS  0x0089e7a0u   /* +0x10 race type (0 free roam), +0x20 local humans, +0x70 records, +0x74 count */
#define G_HUMANS      0x0089e7c0u
#define G_PROFILE0    0x0083a9e0u   /* user 0's profile: +0 name */
#define G_RECORDS     0x00890160u   /* racer records (AddRacer copies into the next) */
#define REC_SZ        0x940u

#define FN_RACERINIT  0x006373e0u   /* thiscall record (index, kind, ride, name, start, ?) */
#define FN_ADDRACER   0x00601600u   /* thiscall race params (record) */
#define FN_CREATECAR  0x00606480u   /* ecx grid {vec3, +0x10 u16 heading}, edx record, (driver, start) */
#define FN_INDEXCARS  0x005f0d20u   /* thiscall World */
#define FN_NEW        0x00575620u   /* cdecl (size): operator new */
#define FN_PLAYER     0x005f1670u   /* thiscall Player ctor (port, index) */
#define FN_ATTACHCAR  0x005f3e10u   /* thiscall Player (car) */
#define FN_PLAYER_END 0x005f1cc0u   /* thiscall Player: the dtor's body (the last Player in order) */
#define FN_FREE       0x00575630u   /* cdecl (p): operator delete */
#define FN_DEACTIVATE 0x005f6e20u   /* thiscall Car: park it out of the world */
#define FN_ACTIVATE   0x005f6d50u   /* thiscall Car: back from parked (driver and physics as its kind's) */
#define FN_SETPOS     0x005fbd60u   /* thiscall Car (grid, 0, 0, 0) */
#define FN_GETVIEW    0x0048b1e0u   /* cdecl (id): a view (1 player 1's, 2 player 2's); +0x44 its camera movers (bTList) */

/* guest scratch (0xD00000.. is the split screen's; 0xD0E000.. ours) */
#define COOP_REC      0x00d0e000u   /* a racer record (0x940), copied by AddRacer */
#define COOP_GRID     0x00d0e980u   /* the grid entry CreateCar places the car at */
#define COOP_NAME     0x00d0e9c0u   /* player 2's racer name */

#define SETTLE_FRAMES 90            /* free roam settled this long before player 2 (re)appears */

static int coop_want;               /* co-op is on: player 2 is in, or comes back after a world load */
static int coop_dev = -1;           /* COOP_DEV_KB or the local pad seat 0..5 */
static uint32_t coop_pl, coop_car, coop_world;   /* player 2's Player and car, and the World they are in */
static long coop_settled;           /* frames free roam has been as roaming wants it */
/* after a leave, in that world: player 2's car parked, and its Player's memory (ended, out of the game's lists): the
 * car keeps pointing at it (car +0x18), and Car::Activate reads it on a rejoin, so it is not freed before the world
 * ends (then we free it: the World's teardown frees only the Players in its order) */
static uint32_t coop_parked, coop_parked_pl, coop_parked_world;

static uint32_t call(CPU *c, uint32_t fn, uint32_t ecx, uint32_t edx, unsigned n, const uint32_t *args){
    CPU saved = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (unsigned i = n; i; --i) PUSH32(args[i - 1]);
    c->ecx = ecx; c->edx = edx;
    PUSH32(fn);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = saved;
    return r;
}
static uint32_t p1_car(void){
    uint32_t p = MR32(SPLIT_PLAYERS_INDEX);
    return p ? MR32(p + 4u) : 0;
}

int coop_in(void){ return coop_pl != 0 && MR32(G_FLOW) == 6u; }
int coop_wanted(void){ return coop_want; }
int coop_device(void){ return coop_want ? coop_dev : -1; }
uint32_t coop_player(void){ return coop_pl; }
uint32_t coop_p2_car(void){ return coop_pl ? coop_car : 0; }

/* the world's end frees player 2 with the rest: what we hold is gone with it */
static void coop_forget(const char *why){
    if (coop_pl) fprintf(stderr, "[coop] f=%ld player 2's Player and car went with the world (%s)\n", autotest_frame(), why);
    coop_pl = coop_car = coop_world = 0;
}

int roam_game_roaming_now(void);   /* roam_game.c: free roam as the online side has it (no race, one racer, a car) */

static int coop_new_car(CPU *c, uint32_t w, uint32_t rec1, uint32_t *out);
/* player 2 beside player 1: a human's record with player 1's ride, a car behind player 1's, its Player on port 1 */
static int coop_make(CPU *c){
    uint32_t w = MR32(G_WORLD), car1 = p1_car();
    if (!w || !car1 || MR32(SPLIT_PLAYERS_COUNT) != 1u) return 0;
    uint32_t rec1 = MR32(car1 + 0x14u);
    if (!rec1) return 0;
    /* behind player 1's car, on its heading (forward +0x90/+0x94) */
    float fx = MRF32(car1 + 0x90u), fy = MRF32(car1 + 0x94u);
    memset(g_mem + COOP_GRID, 0, 0x20);
    MRF32(COOP_GRID) = MRF32(car1 + 0x60u) - 8.f * fx;
    MRF32(COOP_GRID + 4) = MRF32(car1 + 0x64u) - 8.f * fy;
    MRF32(COOP_GRID + 8) = MRF32(car1 + 0x68u) + 0.5f;
    MR16(COOP_GRID + 0x10u) = MR16(car1 + 0x40cu);
    uint32_t car = 0;
    uint32_t pl = 0;
    int back = coop_parked && coop_parked_world == w;  /* back in the car parked when player 2 left */
    if (back){ car = coop_parked; pl = coop_parked_pl; }
    else if (!coop_new_car(c, w, rec1, &car)) return 0;
    coop_parked = coop_parked_pl = coop_parked_world = 0;
    if (!pl){ uint32_t sz = 0x9c0u; pl = call(c, FN_NEW, 0, 0, 1, &sz); }
    if (!pl) return 0;
    memset(g_mem + pl, 0, 0x9c0);
    MR32(G_HUMANS) = 2u;                       /* view 2 and a 2P HUD of its own (see above) */
    uint32_t pa[2] = { 1, 1 };
    call(c, FN_PLAYER, pl, 0, 2, pa);
    MR32(G_HUMANS) = 1u;
    call(c, FN_ATTACHCAR, pl, 0, 1, &car);
    if (back){
        /* the parked car back where player 2 comes in, now that its Player is a fresh one (Activate and SetPosition
         * reach the car's Player, +0x238 among others, which the ended one's no longer holds) */
        call(c, FN_ACTIVATE, car, 0, 0, 0);
        uint32_t sp[4] = { COOP_GRID, 0, 0, 0 };
        call(c, FN_SETPOS, car, 0, 4, sp);
        call(c, FN_ATTACHCAR, pl, 0, 1, &car);
    }
    coop_pl = pl; coop_car = car; coop_world = w;
    fprintf(stderr, "[coop] f=%ld player 2 in: Player 0x%08x (view %u), car 0x%08x (index %u), cars %u, AI range [%u, %u)\n",
            autotest_frame(), pl, MR32(pl + 0x18u), car, MR32(car + 0x24u), MR32(w + 0x10u), MR32(w + 0x14u), MR32(w + 0x18u));
    return 1;
}
/* a car for player 2 in this world: a human's racer record with player 1's ride, CreateCar at COOP_GRID */
static int coop_new_car(CPU *c, uint32_t w, uint32_t rec1, uint32_t *out){
    uint32_t n0 = MR32(G_RACEPARAMS + 0x74u);
    if (n0 >= 22u) { fprintf(stderr, "[coop] no racer record free (%u)\n", n0); return 0; }
    /* the name: player 1's profile name and " 2" */
    char name[32]; snprintf(name, sizeof name, "%.13s 2", (const char *)g_mem + G_PROFILE0);
    memset(g_mem + COOP_NAME, 0, 32); memcpy(g_mem + COOP_NAME, name, strlen(name));
    memset(g_mem + COOP_REC, 0, REC_SZ);
    uint32_t ri[6] = { 1, 1, rec1 + 0x10u, COOP_NAME, 0xfffffffeu, 0 };
    call(c, FN_RACERINIT, COOP_REC, 0, 6, ri);
    uint32_t a = COOP_REC;
    call(c, FN_ADDRACER, G_RACEPARAMS, 0, 1, &a);
    uint32_t n1 = MR32(G_RACEPARAMS + 0x74u), recs = G_RECORDS;
    if (n1 != n0 + 1u || !recs) { fprintf(stderr, "[coop] AddRacer added no record (%u -> %u)\n", n0, n1); return 0; }
    uint32_t rec = recs + (n1 - 1u) * REC_SZ;
    uint32_t cc[2] = { 1, 0 };
    call(c, FN_CREATECAR, COOP_GRID, rec, 2, cc);
    call(c, FN_INDEXCARS, w, 0, 0, 0);
    uint32_t car = 0, it = MR32(w);
    for (int k = 0; it && it != w && k < 128; k++, it = MR32(it)) if (MR32(it + 0x14u) == rec) car = it;
    if (!car) { fprintf(stderr, "[coop] CreateCar made no car\n"); return 0; }
    *out = car;
    return 1;
}

/* player 2 leaves this world: its Player ended (the last in order), its car parked */
static void coop_drop(CPU *c){
    if (!coop_pl) return;
    if (MR32(SPLIT_PLAYERS_COUNT) == 2u && MR32(SPLIT_PLAYERS_ORDER + 4u) == coop_pl){
        call(c, FN_PLAYER_END, coop_pl, 0, 0, 0);
        if (coop_car){ call(c, FN_DEACTIVATE, coop_car, 0, 0, 0); coop_parked = coop_car; coop_parked_world = coop_world; }
        coop_parked_pl = coop_pl;
        /* view 2's camera movers (the chase camera the game gave player 2) follow player 2's Player and car: they go
         * too, as at a world's end (deleting one takes it off the view's list, 0x44DDA0 -> 0x48F760); the view mode
         * (0x5C2120 from the views' movers) is one player's again */
        uint32_t two = 2, view = call(c, FN_GETVIEW, 0, 0, 1, &two);
        for (int k = 0; view && k < 8; k++){
            uint32_t node = MR32(view + 0x44u);
            if (!node || node == view + 0x44u) break;
            uint32_t mover = node - 4u, del = 1;
            call(c, MR32(MR32(mover)), mover, 0, 1, &del);
        }
        fprintf(stderr, "[coop] f=%ld player 2 left\n", autotest_frame());
    }
    coop_pl = coop_car = coop_world = 0;
}

/* every frame at the main-loop hook (roam.c, 0x581475) */
void coop_frame(CPU *c){
    /* out of the world (the front end, a loading screen): the World ends with player 2's Player and car in it; a
     * Player parked out of its lists is freed once the next world is up (the old car pointed at it until its end) */
    static int away;
    if (MR32(G_FLOW) != 6u){
        if (coop_pl) coop_forget("out of the world");
        away = 1;
    } else if (away){
        away = 0;
        if (coop_parked_pl){ uint32_t p = coop_parked_pl; call(c, FN_FREE, 0, 0, 1, &p); }
        coop_parked = coop_parked_pl = coop_parked_world = 0;
    }
    if (coop_pl && MR32(SPLIT_PLAYERS_INDEX + 4u) != coop_pl) coop_forget("the game ended player 2's Player");
    int roaming = roam_game_roaming_now() && MR32(G_GAMEMODE) == 1u && MR32(G_RACEPARAMS + 0x10u) == 0u;
    coop_settled = roaming ? coop_settled + 1 : 0;
    if (coop_want && !coop_pl && coop_settled >= SETTLE_FRAMES) {
        if (!coop_make(c)) { coop_want = 0; fprintf(stderr, "[coop] player 2 could not join: co-op off\n"); }
    }
    if (!coop_want && coop_pl) coop_drop(c);
#ifdef __wasm__
    /* the page lets player 2's controller drive nothing but its seat */
    static int told = -1;
    int seat = coop_want && coop_dev >= 0 && coop_dev < COOP_DEV_KB ? coop_dev : -1;
    if (seat != told){ gamepad_local_coop(seat); told = seat; }
#endif
}

/* The outrun challengers' step (0x413030): for each Player, a game tuner near it (0x40C030) lights player 1's HUD's
 * outrun icon (0x500D20) or puts it out, and becomes the Player's +0x250; player 2's pass (no tuner near) would put
 * player 1's offer out every frame.  In co-op the step is player 1's alone: player 2 is offered no outrun */
void sub_00413030_orig(CPU *c);
void sub_00413030(CPU *c){
    if (!coop_pl || MR32(SPLIT_PLAYERS_COUNT) != 2u){ sub_00413030_orig(c); return; }
    MR32(SPLIT_PLAYERS_COUNT) = 1u;
    sub_00413030_orig(c);
    MR32(SPLIT_PLAYERS_COUNT) = 2u;
    MR32(coop_pl + 0x250u) = 0;
}

/* the markers' and triggers' step of Player::Update (0x602AD9): player 1's only */
void sub_005eb040_orig(CPU *c);
void sub_005eb040(CPU *c){
    if (coop_pl && c->ecx == coop_pl) { POP32(c->eip); return; }
    sub_005eb040_orig(c);
}

void coop_join(int dev){ coop_dev = dev; coop_want = 1; }
void coop_leave(void){ coop_want = 0; }

/* script: "roam coop join kb|pad N", "roam coop leave", "roam coop state" */
int coop_cmd(CPU *c, const char *args, long *wait_start){
    (void)c; (void)wait_start;
    char sub[32] = {0}, a1[32] = {0}, a2[32] = {0};
    sscanf(args, "%31s %31s %31s", sub, a1, a2);
    long f = autotest_frame();
    if (!strcasecmp(sub, "join")){
        int dev = !strcasecmp(a1, "kb") ? COOP_DEV_KB : atoi(a2);
        coop_join(dev);
        fprintf(stderr, "[coop] f=%ld script: join (%s)\n", f, dev == COOP_DEV_KB ? "the keyboard's WASD half" : a2);
    } else if (!strcasecmp(sub, "leave")){
        coop_leave();
        fprintf(stderr, "[coop] f=%ld script: leave\n", f);
    } else {
        uint32_t car1 = p1_car();
        fprintf(stderr, "[coop] f=%ld state: want %d dev %d Player 0x%08x car 0x%08x players %u; P1 at (%.1f, %.1f) P2 at (%.1f, %.1f)\n",
                f, coop_want, coop_dev, coop_pl, coop_car, MR32(SPLIT_PLAYERS_COUNT),
                car1 ? MRF32(car1 + 0x60u) : 0.f, car1 ? MRF32(car1 + 0x64u) : 0.f,
                coop_car ? MRF32(coop_car + 0x60u) : 0.f, coop_car ? MRF32(coop_car + 0x64u) : 0.f);
    }
    return 1;
}
