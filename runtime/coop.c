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
#include "guest.h"
#include "game.h"
#include "splitscreen_mem.h"
#include "gamepad.h"
#include "kbd.h"
#include "coop.h"
#include "widescreen.h"           /* HOOK_SITE */
#include "setting.h"
#include "featurelist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>

long autotest_frame(void);
void travel_setup(void);          /* travel.c: a trip's track in free roam's setup */
void travel_presetup(void);       /* travel.c: before it, a trip's start or the city's place for a load the game starts */
int travel_full(void);            /* travel.c: a trip to the race track's map loaded now: load it in full, 1 once */
uint32_t travel_room(void);       /* travel.c: on a race track's map (its room's key), 0 the city */

#define G_HUMANS      0x0089e7c0u
#define G_RECORDS     0x00890160u   /* racer records (AddRacer copies into the next) */
#define REC_SZ        0x940u

#define FN_RACERINIT  0x006373e0u   /* thiscall record (index, kind, ride, name, start, ?) */
#define FN_ADDRACER   0x00601600u   /* thiscall race params (record) */
#define FN_CREATECAR  0x00606480u   /* ecx grid {vec3, +0x10 u16 heading}, edx record, (driver, start) */
#define FN_INDEXCARS  0x005f0d20u   /* thiscall World */
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
/* The city's memory: the streamer's pool is made at a track's load (0x57A580 -> 0x5E4E70), for one player's area;
 * co-op needs two (splitscreen.c's sub_005e4e70 makes it 1.5x while co-op is on).  So the first join reloads free
 * roam: player 1's place as the career's start (Career +0x8750.., the crib's and shops' exits set them), then the
 * race status's event 10 (PostEvent 0x601020 on [0x8900D8]: SwitchTrack, the free-roam setup 0x540780 again), with
 * the flow step 0x581820 told the track changes (the current track info [0x883DAC] another one's while it reads
 * it) so the track loads in full and the pools are made again. */
#define G_TRACKINFOS  0x00883da0u   /* the track infos (stride 0x128): entry 0 L1RA */
static int coop_pools_big;          /* the last track load made the pools for two (splitscreen.c tells) */
static int coop_reload;             /* 1 asked (the event went), 2 the flow step saw it */
void coop_note_pools(int big){ coop_pools_big = big; }
static int coop_ctor;               /* player 2's Player ctor runs (splitscreen.c gives it free roam's HUD) */
static long coop_settled;           /* frames free roam has been as roaming wants it */
/* after a leave, in that world: player 2's car parked, and its Player's memory (ended, out of the game's lists): the
 * car keeps pointing at it (car +0x18), and Car::Activate reads it on a rejoin, so it is not freed before the world
 * ends (then we free it: the World's teardown frees only the Players in its order) */
static uint32_t coop_parked, coop_parked_pl, coop_parked_world, coop_parked_rec;

/* a guest function with ecx, edx and stack arguments, returning to itself (guest.h) */
static uint32_t call(CPU *c, uint32_t fn, uint32_t ecx, uint32_t edx, unsigned n, const uint32_t *args){
    return guest_call_ex(c, fn, ecx, edx, (int)n, args, fn, 256u);
}
static uint32_t p1_car(void){
    uint32_t p = MR32(SPLIT_PLAYERS_INDEX);
    return p ? MR32(p + 4u) : 0;
}

int coop_making_player(void){ return coop_ctor; }
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

static int coop_new_car(CPU *c, uint32_t w, uint32_t ride, uint32_t *out);

/* ---- player 2's car: one of player 1's garage cars ------------------------------------------------------------
 * The garage: the career's 5 records (FEDatabase 0x83AD90 +0x5954, 0x7F8 each; +0x7F5 a car in it) and garage.c's
 * slots 6-10 (CAREER_SLOT_5..9, found through garage_find).  The ride is built as the game builds the career's
 * (Career::GetPlayerRide 0x528600): the ride's physics info (0x5AC6E0 on ride + 0x10), RideInfo::Init 0x610270(-1, 0,
 * 0, 0), then the record's own builder (its vtable +0xC: (player index 1, ride, 0); another class's builder on it
 * faults), then a paint slot of its own: DUMMY_SKIN53, past every one roam_game.c's cars take (5 and up, made by it,
 * roam_game_coop_skin).  The car loader loads it (Load 0x63BC70, Service; Loaded 0x610910 on its entry; WaitForLoad
 * 0x61CA50) before CreateCar; Unload 0x63AE00 when player 2 is gone or the world ended. */
#define FN_PHYSINFO   0x005ac6e0u   /* thiscall (ride + 0x10): the ride's physics info's ctor */
#define G_CARLOADER   0x008a3288u   /* entries on +0x58, handle at entry +0x123C */
#define FN_LOAD       0x0063bc70u   /* thiscall (ride, 0, 0) -> handle */
#define FN_SERVICE    0x0063bcd0u   /* thiscall (0, 0) */
#define FN_LOADED     0x00610910u   /* thiscall loader (entry) */
#define FN_WAIT       0x0061ca50u   /* thiscall (handle) */
#define FN_UNLOAD     0x0063ae00u   /* thiscall (handle) */
#define COOP_RIDE     0x00d0f000u   /* player 2's ride (0x900) */
#define COOP_SKIN     53u
#define G_CARTYPES_N  0x2eu
#define G_LANG_N      0x008383d0u   /* the game's texts: count, then sorted {hash, text} */
#define G_LANG_TAB    0x008383d8u
#define LOAD_US       15000000
uint32_t garage_find(uint32_t db, uint32_t h);
int roam_game_coop_skin(CPU *c, uint32_t n);
int64_t roam_plat_now_us(void);
static uint32_t coop_car_rec;       /* the garage car player 2 drives (0: a copy of player 1's ride) */
static uint32_t coop_ride_h, coop_ride_rec;   /* its ride's loader handle, of that record */
/* rides to unload once a world is up (the loader's handles); rides of parked cars still standing in this world (a
 * rejoin in another car leaves the parked one there, deactivated: its model stays until the world ends) */
#define COOP_RIDES_N 8
static uint32_t coop_unload_q[2 * COOP_RIDES_N], coop_kept_h[COOP_RIDES_N];
static int coop_unload_n, coop_kept_n;
static int coop_ride_ready;
static int64_t coop_load_t0;

static const char *gstr_at(uint32_t p);
static uint32_t bhash(const char *s){ uint32_t h = 0xffffffffu; for (; *s; s++) h = h * 33u + (uint8_t)*s; return h; }
/* player 1's garage cars, the game's 5 then garage.c's 5 */
static int coop_garage(uint32_t out[10]){
    int n = 0;
    for (unsigned i = 0; i < 10; i++){
        uint32_t r;
        if (i < 5) r = G_FECARS + 0x5954u + i * 0x7f8u;
        else { char nm[24]; snprintf(nm, sizeof nm, "CAREER_SLOT_%u", i); r = garage_find(G_FECARS, bhash(nm)); }
        if (r && mem_is_committed(r + 0x7f7u) && MR32(r) == 0x0079b12cu && MR8(r + 0x7f5u)) out[n++] = r;
    }
    return n;
}
/* "<Make> <Model>" of a garage car, in the game's letters (its text CAR_NAME_<type>, the make from the type's id) */
static void coop_car_name(uint32_t rec, char *out, size_t osz){
    out[0] = 0;
    uint32_t tab = MR32(G_CARTYPES), hash = MR32(rec + 0x20u), t = G_CARTYPES_N;
    if (!tab) return;
    for (uint32_t i = 0; i < G_CARTYPES_N; i++) if (MR32(tab + i * 0x890u + 0xd0u) == hash){ t = i; break; }
    if (t >= G_CARTYPES_N) return;
    const char *nm = gstr_at(tab + t * 0x890u), *mf = gstr_at(tab + t * 0x890u + 0xc0u);
    if (!nm) return;
    char key[64]; snprintf(key, sizeof key, "CAR_NAME_%s", nm);
    uint32_t h = bhash(key), n = MR32(G_LANG_N), lt = MR32(G_LANG_TAB), lo = 0, hi = n < 200000u ? n : 0;
    const char *model = 0;
    while (lo < hi){
        uint32_t mid = (lo + hi) / 2, mh = MR32(lt + mid * 8u);
        if (mh == h){ model = gstr_at(MR32(lt + mid * 8u + 4u)); break; }
        if (mh < h) lo = mid + 1; else hi = mid;
    }
    char make[40] = "";
    for (size_t i = 0; mf && mf[i] && i + 1 < sizeof make; i++){       /* "PEUGEOT" -> "Peugeot" */
        char ch = mf[i], up = i == 0 || mf[i-1] == ' ' || mf[i-1] == '-' || mf[i-1] == '_';
        make[i] = ch == '_' ? ' ' : up ? ch : (ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch); make[i+1] = 0;
    }
    if (!model) snprintf(out, osz, "%s", make[0] ? make : nm);
    else if (make[0] && strncasecmp(model, make, strlen(make))) snprintf(out, osz, "%s %s", make, model);
    else snprintf(out, osz, "%s", model);
}
static uint32_t coop_loader_entry(uint32_t h){
    uint32_t head = G_CARLOADER + 0x58u, e = MR32(head);
    for (int k = 0; e && e != head && k < 640; k++, e = MR32(e)) if (MR32(e + 0x123cu) == h) return e;
    return 0;
}
static void coop_unload_later(uint32_t h){
    if (!h) return;
    if (coop_unload_n < 2 * COOP_RIDES_N) coop_unload_q[coop_unload_n++] = h;   /* (else kept loaded: never pulled away) */
}
/* `kept`: a parked car in this world still drives on it (unloaded at the world's end) */
static void coop_ride_let_go_(int kept){
    if (coop_ride_h){
        if (kept && coop_kept_n < COOP_RIDES_N) coop_kept_h[coop_kept_n++] = coop_ride_h;
        else if (!kept) coop_unload_later(coop_ride_h);
        coop_ride_h = 0;
    }
    coop_ride_ready = 0; coop_ride_rec = 0;
}
static void coop_ride_let_go(void){ coop_ride_let_go_(0); }
/* a ride (0x900) from a garage car with player 2's paint slot */
static int coop_build_ride(CPU *c, uint32_t rec, uint32_t ride){
    if (!roam_game_coop_skin(c, COOP_SKIN)){ fprintf(stderr, "[coop] no paint slot for player 2's car\n"); return 0; }
    memset(g_mem + ride, 0, 0x900);
    call(c, FN_PHYSINFO, ride + 0x10u, 0, 0, 0);
    uint32_t ia[4] = { 0xffffffffu, 0, 0, 0 };
    call(c, FN_RIDE_INIT, ride, 0, 4, ia);
    uint32_t ba[3] = { 1, ride, 0 };
    call(c, MR32(MR32(rec) + 0xcu), rec, 0, 3, ba);
    uint32_t sk = COOP_SKIN; call(c, FN_RIDE_SKINS, ride, 0, 1, &sk);
    return 1;
}
/* a record of player 1's garage now (not sold, this profile's) */
static int coop_rec_ok(uint32_t rec){
    uint32_t g[10]; int n = coop_garage(g);
    for (int i = 0; i < n; i++) if (g[i] == rec) return 1;
    return 0;
}
static uint32_t coop_default_car(void);
/* player 2's ride for its garage car, loaded: 1 ready, 0 not yet, -1 cannot (then a copy of player 1's) */
static int coop_ride(CPU *c){
    if (coop_car_rec && !coop_rec_ok(coop_car_rec)){
        coop_car_rec = coop_default_car();
        fprintf(stderr, "[coop] f=%ld player 2's car is no longer in the garage: player 1's own\n", autotest_frame());
    }
    if (!coop_car_rec) return -1;
    if (coop_ride_ready && coop_ride_rec == coop_car_rec) return 1;
    if (coop_ride_h && coop_ride_rec != coop_car_rec) coop_ride_let_go_(coop_parked && coop_parked_rec == coop_ride_rec);
    if (!coop_ride_h){
        uint32_t ride = COOP_RIDE, rec = coop_car_rec;
        if (!coop_build_ride(c, rec, ride)) return -1;
        uint32_t la[3] = { ride, 0, 0 };
        coop_ride_h = call(c, FN_LOAD, G_CARLOADER, 0, 3, la);
        uint32_t sa[2] = { 0, 0 }; call(c, FN_SERVICE, G_CARLOADER, 0, 2, sa);
        coop_ride_rec = rec; coop_ride_ready = 0; coop_load_t0 = roam_plat_now_us();
        char nm[64]; coop_car_name(rec, nm, sizeof nm);
        fprintf(stderr, "[coop] f=%ld player 2's car: %s (type %u), loading (handle %u)\n", autotest_frame(), nm, MR32(ride), coop_ride_h);
        if (!coop_ride_h) return -1;
        return 0;
    }
    uint32_t e = coop_loader_entry(coop_ride_h);
    if (e && call(c, FN_LOADED, G_CARLOADER, 0, 1, &e)){
        uint32_t h = coop_ride_h; call(c, FN_WAIT, G_CARLOADER, 0, 1, &h);
        coop_ride_ready = 1;
        fprintf(stderr, "[coop] f=%ld player 2's car loaded\n", autotest_frame());
        return 1;
    }
    if (roam_plat_now_us() - coop_load_t0 > LOAD_US){
        fprintf(stderr, "[coop] f=%ld player 2's car did not load: a copy of player 1's\n", autotest_frame());
        coop_ride_let_go(); coop_car_rec = 0;
        return -1;
    }
    return 0;
}
/* player 2 beside player 1: a human's record with player 1's ride, a car behind player 1's, its Player on port 1 */
static int coop_make(CPU *c){
    uint32_t w = MR32(G_WORLD), car1 = p1_car();
    if (!w || !car1 || MR32(SPLIT_PLAYERS_COUNT) != 1u) return -1;
    uint32_t rec1 = MR32(car1 + 0x14u);
    if (!rec1) return -1;
    int rd = coop_ride(c);
    if (rd == 0) return 0;                                 /* its car is loading */
    uint32_t ride = rd > 0 ? COOP_RIDE : rec1 + 0x10u;
    /* behind player 1's car, on its heading (forward +0x90/+0x94) */
    float fx = MRF32(car1 + 0x90u), fy = MRF32(car1 + 0x94u);
    memset(g_mem + COOP_GRID, 0, 0x20);
    MRF32(COOP_GRID) = MRF32(car1 + 0x60u) - 8.f * fx;
    MRF32(COOP_GRID + 4) = MRF32(car1 + 0x64u) - 8.f * fy;
    MRF32(COOP_GRID + 8) = MRF32(car1 + 0x68u) + 0.5f;
    MR16(COOP_GRID + 0x10u) = MR16(car1 + 0x40cu);
    uint32_t car = 0;
    uint32_t pl = 0;
    /* back in the car parked when player 2 left (the same garage car) */
    int back = coop_parked && coop_parked_world == w && coop_parked_rec == coop_car_rec;
    if (back){ car = coop_parked; pl = coop_parked_pl; }
    else if (!coop_new_car(c, w, ride, &car)) return -1;
    coop_parked = coop_parked_pl = coop_parked_world = coop_parked_rec = 0;
    if (!pl){ uint32_t sz = 0x9c0u; pl = call(c, FN_NEW, 0, 0, 1, &sz); }
    if (!pl) return 0;
    memset(g_mem + pl, 0, 0x9c0);
    MR32(G_HUMANS) = 2u;                       /* view 2 and a HUD of its own (see above) */
    uint32_t pa[2] = { 1, 1 };
    coop_ctor = 1;
    call(c, FN_PLAYER, pl, 0, 2, pa);
    coop_ctor = 0;
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
/* player 2's racer name: player 1's profile name and " 2" */
static void coop_racer_name(void){
    char name[32]; snprintf(name, sizeof name, "%.13s 2", (const char *)g_mem + G_PROFILE_NAME);
    memset(g_mem + COOP_NAME, 0, 32); memcpy(g_mem + COOP_NAME, name, strlen(name));
}
/* a car for player 2 in this world: a human's racer record with player 1's ride, CreateCar at COOP_GRID */
static int coop_new_car(CPU *c, uint32_t w, uint32_t ride, uint32_t *out){
    uint32_t n0 = MR32(G_RACEPARAMS + 0x74u);
    if (n0 >= 22u) { fprintf(stderr, "[coop] no racer record free (%u)\n", n0); return 0; }
    coop_racer_name();
    memset(g_mem + COOP_REC, 0, REC_SZ);
    uint32_t ri[6] = { 1, 1, ride, COOP_NAME, 0xfffffffeu, 0 };
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
        void split_coop_map_drop(CPU *c);
        split_coop_map_drop(c);                 /* player 2's arrow is an object of its HUD, which ends with it */
        call(c, FN_PLAYER_END, coop_pl, 0, 0, 0);
        if (coop_car){ call(c, FN_DEACTIVATE, coop_car, 0, 0, 0); coop_parked = coop_car; coop_parked_world = coop_world; coop_parked_rec = coop_car_rec; }
        if (!coop_want) coop_car_rec = 0;       /* left: the next join picks again (the parked car back if the same) */
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

/* The front end's packages: a list from the top one (FEng manager +0xE4; a package's +4 the one below it, +0xC its
 * name).  Player 2's HUD came in last, so FEng keeps it on top (it puts that order back every frame); what the game
 * and the runtime take for the screen in front (GetTopPackage 0x50C8E0: SMS, photo mode, the races' screens) is the
 * package below it then, player 1's HUD */
/* a guest string that is there (not one of memory freed at a world's end) */
static const char *gstr_at(uint32_t p){
    if (!p || !mem_is_committed(p) || !mem_is_committed(p + 63u)) return 0;
    const char *q = (const char *)g_mem + p;
    return memchr(q, 0, 64) ? q : 0;
}
static int fe_named(uint32_t node, uint32_t hud){
    if (!node || !hud || !mem_is_committed(node + 0xfu) || !mem_is_committed(hud + 0x13u)) return 0;
    const char *a = gstr_at(MR32(node + 0xcu)), *b = gstr_at(MR32(hud + 0x10u));
    return a && b && !strcasecmp(a, b);
}
/* player 2's Player while the game still has it (a world's end frees it before coop_frame sees); in a career race
 * together the game's own second Player */
int coop_racing_together(void);
static uint32_t coop_live(void){
    if (coop_pl && MR32(SPLIT_PLAYERS_INDEX + 4u) == coop_pl) return coop_pl;
    return coop_racing_together() && MR32(SPLIT_PLAYERS_COUNT) == 2u ? MR32(SPLIT_PLAYERS_INDEX + 4u) : 0;
}
uint32_t coop_fe_top(uint32_t mg){
    uint32_t top = mg ? MR32(mg + 0xe4u) : 0;
    uint32_t p2 = coop_live();
    if (top && p2 && fe_named(top, MR32(p2 + 0x9a4u)) && MR32(top + 4u)) return MR32(top + 4u);
    return top;
}
/* driving: player 2 in, and player 1's HUD the screen in front (no pause menu, dialog, map or SMS over it: those
 * take the whole keyboard, and read port 0 as well) */
int coop_driving(void){
    if (!coop_in() && !coop_racing_together()) return 0;
    uint32_t fe = MR32(G_FENG), mg = fe ? MR32(fe + 8u) : 0, p1 = MR32(SPLIT_PLAYERS_INDEX);
    return mg && p1 && fe_named(coop_fe_top(mg), MR32(p1 + 0x9a4u));
}
void sub_0050c8e0_orig(CPU *c);
void sub_0050c8e0(CPU *c){
    sub_0050c8e0_orig(c);
    uint32_t fe = MR32(G_FENG), mg = fe ? MR32(fe + 8u) : 0;
    if (mg && c->eax == MR32(mg + 0xe4u)) c->eax = coop_fe_top(mg);
}

void sub_00581820_orig(CPU *c);
void cameras_setup(void);
void sub_00581820(CPU *c){
    cameras_setup();                                       /* (a load's race setup: the views, cameras.c) */
    int trip = travel_full();                       /* travel.c: a trip to another spot of the race track's map loaded */
    if (coop_reload != 1 && !trip){ sub_00581820_orig(c); return; }
    if (coop_reload == 1) coop_reload = 2;
    uint32_t keep = MR32(G_TRACKINFO), other = MR32(G_TRACKINFOS);
    if (other && other != keep) MR32(G_TRACKINFO) = other;   /* "another track": the full load (it writes this itself) */
    if (trip) fprintf(stderr, "[coop] f=%ld free roam loads again (a trip on the race track's map)\n", autotest_frame());
    else fprintf(stderr, "[coop] f=%ld free roam loads again, with room for two\n", autotest_frame());
    sub_00581820_orig(c);
}
/* the first join in a world whose pools are one player's: the reload, from player 1's place (no menu up, one racer).
 * Not on a race track's map (travel.c): the reload is the city's; player 2 comes in once the car is back there */
static int coop_reload_now(CPU *c){
    uint32_t car1 = p1_car(), rs = MR32(G_RACESTATUS), race = MR32(G_RACE);
    if (!car1 || !rs || MR32(rs) != 4u || !race || MR32(race + 0x24u) != 1u || travel_room()) return 0;
    uint32_t fe = MR32(G_FENG), mg = fe ? MR32(fe + 8u) : 0, p1 = MR32(SPLIT_PLAYERS_INDEX);
    if (!mg || !p1 || !fe_named(coop_fe_top(mg), MR32(p1 + 0x9a4u))) return 0;
    for (unsigned i = 0; i < 3; i++) MRF32(G_START_POS + 4u * i) = MRF32(car1 + 0x60u + 4u * i);
    MR16(G_START_HEAD) = MR16(car1 + 0x40cu);
    MR8(G_START_KEEP) = 1;
    coop_reload = 1;
    uint32_t a[2] = { 10u, 0u };
    call(c, FN_POSTEVENT, rs, 0, 2, a);
    fprintf(stderr, "[coop] f=%ld player 2 joins: free roam loads again at (%.1f, %.1f)\n", autotest_frame(),
            MRF32(car1 + 0x60u), MRF32(car1 + 0x64u));
    return 1;
}

int mprace_phase(void);
static int coop_rt;
/* the front end's main menu up, no lobby race's trip under way (mprace.h: MPR_OUT .. MPR_BACK) */
static int coop_main_menu(void){
    uint32_t fe = MR32(G_FENG), mg = fe ? MR32(fe + 8u) : 0, top = mg ? MR32(mg + 0xe4u) : 0;
    const char *nm = top ? gstr_at(MR32(top + 0xcu)) : 0;
    int ph = mprace_phase();
    return nm && !strcasecmp(nm, "MC_Main.fng") && !(ph >= 1 && ph <= 4);
}
/* every frame at the main-loop hook (roam.c, 0x581475) */
void coop_frame(CPU *c){
    /* out of the world (the front end, a loading screen): the World ends with player 2's Player and car in it; a
     * Player parked out of its lists is freed once the next world is up (the old car pointed at it until its end) */
    static int away;
    void coop_outrun_world_end(void);
    if (MR32(G_FLOW) != 6u){
        if (coop_pl) coop_forget("out of the world");
        if (!away) coop_outrun_world_end();       /* an outrun between them ends with the world */
        away = 1;
        /* the main menu (Quit, or another profile next): co-op ends, unless a lobby race's trip goes through it */
        if (coop_want && coop_main_menu()){
            coop_want = 0; coop_car_rec = 0; coop_rt = 0;
            fprintf(stderr, "[coop] f=%ld the main menu: co-op ends\n", autotest_frame());
        }
    } else if (away){
        away = 0;
        coop_ride_let_go();                       /* the world's end: its ride again in the next one */
        for (int k = 0; k < coop_kept_n; k++) coop_unload_later(coop_kept_h[k]);   /* (the parked cars went with it) */
        coop_kept_n = 0;
        if (coop_parked_pl){ uint32_t p = coop_parked_pl; call(c, FN_FREE, 0, 0, 1, &p); }
        coop_parked = coop_parked_pl = coop_parked_world = 0;
    }
    if (coop_pl && MR32(SPLIT_PLAYERS_INDEX + 4u) != coop_pl) coop_forget("the game ended player 2's Player");
    int roaming = roam_game_roaming_now() && MR32(G_GAMEMODE) == 1u && MR32(G_RACEPARAMS + 0x10u) == 0u;
    coop_settled = roaming ? coop_settled + 1 : 0;
    static long asked_at;
    if (coop_reload == 1 && !asked_at) asked_at = autotest_frame();
    if (coop_reload != 1) asked_at = 0;
    if (coop_reload == 1 && autotest_frame() - asked_at > 300){                  /* the world did not go: as it is */
        fprintf(stderr, "[coop] f=%ld free roam did not load again: player 2 in this world\n", autotest_frame());
        coop_reload = 0; coop_pools_big = 1;
    }
    if (coop_reload == 2 && MR32(G_FLOW) == 6u && coop_settled > 0) coop_reload = 0;     /* back in the world */
    if (coop_want && !coop_pl && coop_settled >= SETTLE_FRAMES && !coop_reload){
        if (!coop_pools_big) coop_reload_now(c);
        else if (coop_make(c) < 0){ coop_want = 0; coop_ride_let_go(); fprintf(stderr, "[coop] player 2 could not join: co-op off\n"); }
    }
    if (!coop_want && coop_pl) coop_drop(c);
    if (!coop_want && !coop_pl && coop_ride_h && !coop_parked) coop_ride_let_go();
    if (coop_unload_n && MR32(G_FLOW) == 6u){ uint32_t h = coop_unload_q[--coop_unload_n]; call(c, FN_UNLOAD, G_CARLOADER, 0, 1, &h); }
    void coop_ask_frame_(CPU *c); coop_ask_frame_(c);
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
    void coop_outrun_icon(CPU *c); coop_outrun_icon(c);     /* (it put player 1's outrun icon out: ours back on) */
}

/* the markers' and triggers' step of Player::Update (0x602AD9): player 1's only */
void sub_005eb040_orig(CPU *c);
void sub_005eb040(CPU *c){
    if (coop_pl && c->ecx == coop_pl) { POP32(c->eip); return; }
    sub_005eb040_orig(c);
}

/* the garage car player 2 takes unless it picks (and the car dialog's first): player 1's own, else the first */
static uint32_t coop_default_car(void){
    uint32_t g[10], mine = garage_find(G_FECARS, MR32(G_CAREER_CAR));
    int n = coop_garage(g);
    for (int i = 0; i < n; i++) if (g[i] == mine) return g[i];
    return n ? g[0] : 0;
}
void coop_join(int dev){ coop_dev = dev; coop_want = 1; if (!coop_car_rec) coop_car_rec = coop_default_car(); }
void coop_join_car(int dev, uint32_t rec){ coop_car_rec = rec; coop_join(dev); }
void coop_leave(void){ coop_want = 0; }        /* (its car's record stays until coop_drop parks the car with it) */

/* script: "roam coop join kb|pad N", "roam coop leave", "roam coop state", "roam coop finish" (a race's player 2) */
int coop_cmd(CPU *c, const char *args, long *wait_start){
    (void)wait_start;
    char sub[32] = {0}, a1[32] = {0}, a2[32] = {0};
    sscanf(args, "%31s %31s %31s", sub, a1, a2);
    long f = autotest_frame();
    if (!strcasecmp(sub, "join")){
        int dev = !strcasecmp(a1, "kb") ? COOP_DEV_KB : atoi(a2);
        coop_join(dev);
        fprintf(stderr, "[coop] f=%ld script: join (%s)\n", f, dev == COOP_DEV_KB ? "the keyboard's WASD half" : a2);
    } else if (!strcasecmp(sub, "garage2")){
        /* test only (scripts): the career's slot 2 a copy of slot 1, for a garage of two cars */
        uint32_t r0 = G_FECARS + 0x5954u, r1 = r0 + 0x7f8u, h1 = MR32(r1 + 8u);
        memcpy(g_mem + r1, g_mem + r0, 0x7f8);
        MR32(r1 + 8u) = h1 ? h1 : bhash("CAREER_SLOT_1"); MR8(r1 + 0x7f4u) = 0; MR8(r1 + 0x7f5u) = 1;
        uint32_t g[10]; fprintf(stderr, "[coop] f=%ld script: garage of %d cars\n", f, coop_garage(g));
    } else if (!strcasecmp(sub, "finish")){
        /* test only: player 2's racer finishes now, through the coordinator's FinishRacer (as career.c's racewin) */
        uint32_t rc = MR32(G_RACE), p2 = MR32(SPLIT_PLAYERS_INDEX + 4u), car = p2 ? MR32(p2 + 4u) : 0, n = rc ? MR32(rc + 0x24u) : 0, i;
        for (i = 0; car && i < n && i < 8u && MR32(rc + 0xbd0u + 4u * i) && MR32(MR32(rc + 0xbd0u + 4u * i)) != car; i++) ;
        if (!car || i >= n || i >= 8u) fprintf(stderr, "[coop] f=%ld script: finish: player 2 is not racing\n", f);
        else { uint32_t a[4] = { i, 0, 0, 1u }; call(c, 0x00606cb0u, rc, 0, 4, a);
               /* a drift: its points below player 1's 0 (as racewin puts the AI's; the board 0x864630: +8 racer, +0x10 points) */
               uint32_t me = (MR32(rc + 0xbd0u + 4u * i) - (rc + 0x30u)) / 0x1f0u, e = MR32(0x00864630u);
               for (int k = 0; e && e != 0x00864630u && k < 16 && mem_is_committed(e + 0x1f); k++, e = MR32(e))
                   if (MR32(e + 8u) == me) MRF32(e + 0x10u) = -1.f;
               fprintf(stderr, "[coop] f=%ld script: player 2 (racer %u of %u) finished, position %u\n", f, i, n, MR8(MR32(rc + 0xbd0u + 4u * i) + 0xcu)); }
    } else if (!strcasecmp(sub, "leave")){
        coop_leave();
        fprintf(stderr, "[coop] f=%ld script: leave\n", f);
    } else {
        uint32_t car1 = p1_car();
        fprintf(stderr, "[coop] f=%ld state: want %d dev %d Player 0x%08x car 0x%08x players %u; P1 at (%.1f, %.1f) P2 at (%.1f, %.1f)\n",
                f, coop_want, coop_dev, coop_pl, coop_car, MR32(SPLIT_PLAYERS_COUNT),
                car1 ? MRF32(car1 + 0x60u) : 0.f, car1 ? MRF32(car1 + 0x64u) : 0.f,
                coop_car ? MRF32(coop_car + 0x60u) : 0.f, coop_car ? MRF32(coop_car + 0x64u) : 0.f);
        uint32_t rc = MR32(G_RACE), n = rc ? MR32(rc + 0x24u) : 0;   /* the race coordinator */
        fprintf(stderr, "[coop] f=%ld state: humans %u AI %u, players", f, MR32(G_HUMANS), MR32(0x0089e7c4u));
        for (uint32_t i = 0; i < 4 && i < MR32(SPLIT_PLAYERS_COUNT); i++){ uint32_t pl = MR32(SPLIT_PLAYERS_INDEX + 4u * i);
            fprintf(stderr, " 0x%08x(car 0x%08x)", pl, pl ? MR32(pl + 4u) : 0); }
        fprintf(stderr, "; entries %u [%08x %08x %08x %08x] ports [%u %u]; racers %u:", MR32(0x0089e814u), MR32(0x0089e808u), MR32(0x0089e80cu),
                MR32(0x0089e810u), MR32(0x0089e818u), MR32(0x0089e7e8u), MR32(0x0089e7ecu), n);
        for (uint32_t i = 0; rc && i < n && i < 8; i++){ uint32_t r = rc + 0x30u + 0x1f0u * i; fprintf(stderr, " 0x%08x", MR32(r)); }
        fputc('\n', stderr);
    }
    return 1;
}

/* ---- the pause menu's Add Player 2 / Remove Player 2 --------------------------------------------------------------
 * An item of Replay's kind (photo.c adds it after Replay and Photo Mode, free roam only), its action photo.c's patch
 * of Replay's (items get every message their menu gets).  Add Player 2 asks with the world's generic dialog
 * (0x558750, IG_GenericDialog_MED.fng; its answers come back to the menu, so to this item): player 2 presses START on
 * a controller (a local seat's START, input_web.c's join stamp), or picks Keyboard (the keyboard's WASD half); Cancel
 * or Back leaves it.  Remove Player 2 lets player 2 go.  Both then resume the game. */
#define FN_ADD_ITEM   0x0050dab0u   /* thiscall (item; ret 4): a menu's AddOption */
#define VT_REPLAY     0x007954f0u
#define ITEM_BYTES    0x48u
#define IT_GREY       0x20u
#define IT_LABEL      0x2cu
#define MSG_ACCEPT    0x0c407210u
#define MSG_BACK      0x911ab364u   /* PAD_BACK */
#define FN_DIALOG2    0x00558750u   /* cdecl (from, dialog package, label1, label2, msg1, msg2, back msg, first, fmt, ...) */
#define FN_FE_MSG     0x0055dda0u   /* cdecl (message, package name): a message to a package */
#define ICON_COOP     0x84d1d864u   /* (Replay's video camera until a two-player icon is drawn) */
#define MSG_KB        0xc0c0a001u   /* our dialog's answers */
#define MSG_CANCEL    0xc0c0a002u
#define MSG_DBACK     0xc0c0a003u
#define MSG_NEXT      0xc0c0a004u   /* the car dialog's */
#define MSG_DRIVE     0xc0c0a005u
#define MSG_PBACK     0xc0c0a006u
#define COOP_STR      0x00d0ea00u   /* the dialog's strings: package name, format, text */

const char *language_name(void);
uint32_t language_add_text(const char *name, const char *text);
void language_game_letters(const char *utf8, char *out, size_t n);
void sub_004c2510(CPU *c);         /* the free-roam pause menu's Resume */

static const struct { const char *lang, *add, *rem, *ask, *kb, *cancel, *next, *drive, *drives, *rt, *together, *alone; } COOP_TEXTS[] = {
    { "english", "Add Player 2", "Remove Player 2",
      "Player 2: press START on your controller, or choose Keyboard to play on the left half of this keyboard.", "Keyboard", "Cancel", "Next car", "Drive", "Player 2 drives:",
      "Race this one with player 2, or alone? (Only player 1's place counts in the career.)", "Together", "Alone" },
    { "french", "Ajouter le joueur 2", "Retirer le joueur 2",
      "Joueur 2 : appuie sur START sur ta manette, ou choisis Clavier pour jouer sur la moitié gauche de ce clavier.", "Clavier", "Annuler", "Voiture suivante", "Conduire", "Le joueur 2 conduit :",
      "Courir avec le joueur 2, ou seul ? (Seule la place du joueur 1 compte pour la carrière.)", "Ensemble", "Seul" },
    { "german", "Spieler 2 hinzufügen", "Spieler 2 entfernen",
      "Spieler 2: Drück START auf deinem Controller oder wähle Tastatur, um auf der linken Hälfte dieser Tastatur zu spielen.", "Tastatur", "Abbrechen", "Nächstes Auto", "Fahren", "Spieler 2 fährt:",
      "Mit Spieler 2 fahren oder allein? (Für die Karriere zählt nur der Platz von Spieler 1.)", "Zusammen", "Allein" },
    { "italian", "Aggiungi giocatore 2", "Rimuovi giocatore 2",
      "Giocatore 2: premi START sul tuo controller, oppure scegli Tastiera per giocare sulla metà sinistra di questa tastiera.", "Tastiera", "Annulla", "Auto successiva", "Guida", "Il giocatore 2 guida:",
      "Correre con il giocatore 2 o da solo? (Per la carriera conta solo il posto del giocatore 1.)", "Insieme", "Da solo" },
    { "spanish", "Añadir jugador 2", "Quitar jugador 2",
      "Jugador 2: pulsa START en tu mando o elige Teclado para jugar en la mitad izquierda de este teclado.", "Teclado", "Cancelar", "Siguiente coche", "Conducir", "El jugador 2 conduce:",
      "¿Correr con el jugador 2 o solo? (En la carrera solo cuenta el puesto del jugador 1.)", "Juntos", "Solo" },
    { "dutch", "Speler 2 toevoegen", "Speler 2 verwijderen",
      "Speler 2: druk op START op je controller, of kies Toetsenbord om op de linkerhelft van dit toetsenbord te spelen.", "Toetsenbord", "Annuleren", "Volgende auto", "Rijden", "Speler 2 rijdt:",
      "Samen met speler 2 racen, of alleen? (Alleen de plaats van speler 1 telt voor de carrière.)", "Samen", "Alleen" },
    { "swedish", "Lägg till spelare 2", "Ta bort spelare 2",
      "Spelare 2: tryck på START på din handkontroll, eller välj Tangentbord för att spela på vänstra halvan av det här tangentbordet.", "Tangentbord", "Avbryt", "Nästa bil", "Kör", "Spelare 2 kör:",
      "Köra med spelare 2, eller ensam? (Bara spelare 1:s placering räknas i karriären.)", "Tillsammans", "Ensam" },
    { "danish", "Tilføj spiller 2", "Fjern spiller 2",
      "Spiller 2: tryk på START på din controller, eller vælg Tastatur for at spille på venstre halvdel af dette tastatur.", "Tastatur", "Annuller", "Næste bil", "Kør", "Spiller 2 kører:",
      "Køre med spiller 2, eller alene? (Kun spiller 1's placering tæller i karrieren.)", "Sammen", "Alene" },
    { "portuguese", "Adicionar jogador 2", "Remover jogador 2",
      "Jogador 2: pressione START no seu controle ou escolha Teclado para jogar na metade esquerda deste teclado.", "Teclado", "Cancelar", "Próximo carro", "Dirigir", "O jogador 2 dirige:",
      "Correr com o jogador 2 ou sozinho? (Só a posição do jogador 1 conta na carreira.)", "Juntos", "Sozinho" },
    { "polish", "Dodaj gracza 2", "Usuń gracza 2",
      "Gracz 2: wciśnij START na swoim kontrolerze albo wybierz Klawiatura, aby grać na lewej połowie tej klawiatury.", "Klawiatura", "Anuluj", "Następny samochód", "Jedź", "Gracz 2 jedzie:",
      "Ścigać się z graczem 2 czy samemu? (W karierze liczy się tylko miejsce gracza 1.)", "Razem", "Sam" },
    { "turkish", "Oyuncu 2'yi ekle", "Oyuncu 2'yi çıkar",
      "Oyuncu 2: kumandanda START'a bas ya da bu klavyenin sol yarısında oynamak için Klavye'yi seç.", "Klavye", "İptal", "Sonraki araba", "Sür", "Oyuncu 2 sürüyor:",
      "Oyuncu 2 ile mi yarışılsın, tek başına mı? (Kariyerde yalnızca oyuncu 1'in sırası sayılır.)", "Birlikte", "Tek başına" },
    { "hungarian", "2. játékos hozzáadása", "2. játékos eltávolítása",
      "2. játékos: nyomd meg a START gombot a kontrollereden, vagy válaszd a Billentyűzetet, hogy a billentyűzet bal felén játssz.", "Billentyűzet", "Mégse", "Következő autó", "Vezetés", "A 2. játékos vezet:",
      "Versenyzel a 2. játékossal, vagy egyedül? (A karrierben csak az 1. játékos helyezése számít.)", "Együtt", "Egyedül" },
    { "czech", "Přidat hráče 2", "Odebrat hráče 2",
      "Hráč 2: stiskni START na svém ovladači, nebo zvol Klávesnice a hraj na levé polovině této klávesnice.", "Klávesnice", "Zrušit", "Další auto", "Jet", "Hráč 2 jede:",
      "Závodit s hráčem 2, nebo sám? (V kariéře se počítá jen umístění hráče 1.)", "Spolu", "Sám" },
    { "slovak", "Pridať hráča 2", "Odobrať hráča 2",
      "Hráč 2: stlač START na svojom ovládači alebo zvoľ Klávesnica a hraj na ľavej polovici tejto klávesnice.", "Klávesnica", "Zrušiť", "Ďalšie auto", "Jazdiť", "Hráč 2 jazdí:",
      "Pretekať s hráčom 2 alebo sám? (V kariére sa počíta len umiestnenie hráča 1.)", "Spolu", "Sám" },
};
static struct { const char *lang; uint32_t add, rem, kb, cancel, next, drive, together, alone; char ask[256], drives[64], rt[256]; } T;
static void coop_texts(CPU *c){
    const char *l = language_name();
    if (T.add && l == T.lang) return;
    T.lang = l;
    unsigned e = 0;
    for (unsigned i = 0; i < sizeof COOP_TEXTS / sizeof COOP_TEXTS[0]; i++) if (l && !strcmp(l, COOP_TEXTS[i].lang)) e = i;
    char s[256];
    language_game_letters(COOP_TEXTS[e].add, s, sizeof s);    T.add = language_add_text("COOP_ADD_N2", s);
    language_game_letters(COOP_TEXTS[e].rem, s, sizeof s);    T.rem = language_add_text("COOP_REMOVE_N2", s);
    language_game_letters(COOP_TEXTS[e].kb, s, sizeof s);     T.kb = language_add_text("COOP_KEYBOARD_N2", s);
    language_game_letters(COOP_TEXTS[e].cancel, s, sizeof s); T.cancel = language_add_text("COOP_CANCEL_N2", s);
    language_game_letters(COOP_TEXTS[e].ask, T.ask, sizeof T.ask);
    language_game_letters(COOP_TEXTS[e].next, s, sizeof s);   T.next = language_add_text("COOP_NEXTCAR_N2", s);
    language_game_letters(COOP_TEXTS[e].drive, s, sizeof s);  T.drive = language_add_text("COOP_DRIVE_N2", s);
    language_game_letters(COOP_TEXTS[e].drives, T.drives, sizeof T.drives);
    language_game_letters(COOP_TEXTS[e].together, s, sizeof s); T.together = language_add_text("COOP_TOGETHER_N2", s);
    language_game_letters(COOP_TEXTS[e].alone, s, sizeof s);  T.alone = language_add_text("COOP_ALONE_N2", s);
    language_game_letters(COOP_TEXTS[e].rt, T.rt, sizeof T.rt);
    language_texts_frame(c);
}

static int coop_asking;             /* the dialog is up: player 2's START on a controller joins */
static uint32_t coop_ask_stamps[6], coop_ask_pkg, coop_ask_held;   /* held: the seats' START when asked */
static int coop_close_steps;        /* after a START: Back to the dialog, then the car or the resume */
static int coop_close_dev;          /* that START's seat */
uint32_t gamepad_local_join_stamp(unsigned seat);

/* photo.c: the pause menu is being made: our item goes in (free roam's career only) */
void coop_item(CPU *c, uint32_t menu){
    if (MR32(G_GAMEMODE) != 1u || MR32(G_RACEPARAMS + 0x10u) != 0u) return;
    /* NFSU2_COOP (settings.ini coop): off on phones and tablets (the page's device defaults: one touch screen) */
    if (!setting_bool("NFSU2_COOP", 1)) return;
    coop_texts(c);
    uint32_t n = ITEM_BYTES, it = call(c, FN_NEW, 0, 0, 1, &n);
    if (!it) return;
    uint32_t arg[3] = { ICON_COOP, coop_want ? T.rem : T.add, 0 };
    call(c, FN_ITEM, it, 0, 3, arg);
    MR32(it) = VT_REPLAY;
    call(c, FN_ADD_ITEM, menu, 0, 1, &it);
    fprintf(stderr, "[coop] f=%ld the pause menu's %s\n", autotest_frame(), coop_want ? "Remove Player 2" : "Add Player 2");
}
static void coop_resume(CPU *c){ sub_004c2510(c); }
/* photo.c's patch of Replay's action: 0 not ours, 1 ours (photo.c returns from the call), 2 ours and returned (the
 * menu's Resume returns from it) */
int coop_act(CPU *c){
    uint32_t it = c->ecx, pkg = MR32(c->esp + 4u), msg = MR32(c->esp + 8u);
    if (!it || MR32(it) != VT_REPLAY || !T.add || (MR32(it + IT_LABEL) != T.add && MR32(it + IT_LABEL) != T.rem)) return 0;
    long f = autotest_frame();
    if (msg == MSG_ACCEPT && MR32(it + IT_LABEL) == T.rem){
        fprintf(stderr, "[coop] f=%ld Remove Player 2\n", f);
        coop_leave(); coop_resume(c); return 2;
    }
    if (msg == MSG_ACCEPT && !MR32(G_DLG)){
        coop_texts(c);
        char *s = (char *)g_mem + COOP_STR;
        memset(s, 0, 0x200);
        snprintf(s, 0x38, "IG_GenericDialog_MED.fng"); snprintf(s + 0x38, 8, "%%s"); snprintf(s + 0x40, 0x1c0, "%s", T.ask);
        for (char *p = s + 0x40; *p; p++) if (*p == '%') *p = '?';
        uint32_t a[10] = { pkg, COOP_STR, T.kb, T.cancel, MSG_KB, MSG_CANCEL, MSG_DBACK, 1, COOP_STR + 0x38u, COOP_STR + 0x40u };
        call(c, FN_DIALOG2, 0, 0, 10, a);
        coop_asking = 1; coop_ask_pkg = pkg;
        coop_ask_held = 0;
        for (unsigned k = 0; k < 6; k++){
            coop_ask_stamps[k] = gamepad_local_join_stamp(k);
            PadState p; if (gamepad_read_local(k, &p) && (p.buttons & 1u << PAD_START)) coop_ask_held |= 1u << k;
        }
        fprintf(stderr, "[coop] f=%ld Add Player 2: asking (dialog %08x)\n", f, MR32(G_DLG));
        return 1;
    }
    return 0;
}
/* the dialog's answer (read from the dialog: it never reaches the pause menu's screen): Keyboard joins, Cancel and
 * Back leave (the pause menu stays), Back after player 2's START resumes */
static int coop_resume_next;
/* the car dialog: "Player 2 drives: <car>" [Next car] [Drive], over the pause menu, while player 1 has two or more */
static int coop_pick_dev = -1, coop_pick_i, coop_pick_show, coop_pick_open;
static uint32_t coop_dialog(void);
static void coop_pick_begin(int dev){
    uint32_t g[10]; int n = coop_garage(g);
    long f = autotest_frame();
    if (n < 2){
        coop_join_car(dev, n ? g[0] : 0); coop_resume_next = 1;
        fprintf(stderr, "[coop] f=%ld Add Player 2: %s\n", f, n ? "the garage's one car" : "player 1's car (no garage car)");
        return;
    }
    uint32_t want = coop_default_car();
    coop_pick_i = 0;
    for (int i = 0; i < n; i++) if (g[i] == want) coop_pick_i = i;
    coop_pick_dev = dev; coop_pick_show = 1; coop_pick_open = 0;
}
static void coop_pick_frame(CPU *c){
    if (coop_pick_dev < 0) return;
    uint32_t g[10]; int n = coop_garage(g);
    if (n < 1){ coop_pick_dev = -1; return; }
    if (coop_pick_i >= n) coop_pick_i = 0;
    if (coop_pick_open){
        uint32_t d = coop_dialog(), v = d ? MR32(d + 0x50u) : 0;
        if (v == MSG_NEXT){ coop_pick_open = 0; coop_pick_i = (coop_pick_i + 1) % n; coop_pick_show = 1; }
        else if (v == MSG_DRIVE){
            char nm[64]; coop_car_name(g[coop_pick_i], nm, sizeof nm);
            fprintf(stderr, "[coop] f=%ld Add Player 2: drives the %s\n", autotest_frame(), nm);
            coop_join_car(coop_pick_dev, g[coop_pick_i]);
            coop_pick_dev = -1; coop_pick_open = 0; coop_resume_next = 1;
        } else if (v == MSG_PBACK){
            fprintf(stderr, "[coop] f=%ld Add Player 2: no car picked\n", autotest_frame());
            coop_pick_dev = -1; coop_pick_open = 0;
        } else if (!d && !MR32(G_DLG)){ coop_pick_dev = -1; coop_pick_open = 0; }   /* gone another way */
        return;
    }
    if (!coop_pick_show || MR32(G_DLG)) return;            /* the last dialog still going */
    coop_texts(c);
    char nm[64]; coop_car_name(g[coop_pick_i], nm, sizeof nm);
    char *s = (char *)g_mem + COOP_STR;
    memset(s, 0, 0x200);
    snprintf(s, 0x38, "IG_GenericDialog_MED.fng"); snprintf(s + 0x38, 8, "%%s");
    snprintf(s + 0x40, 0x1c0, "%s %s", T.drives, nm[0] ? nm : "?");
    for (char *p = s + 0x40; *p; p++) if (*p == '%') *p = '?';
    uint32_t a[10] = { coop_ask_pkg, COOP_STR, T.next, T.drive, MSG_NEXT, MSG_DRIVE, MSG_PBACK, 1, COOP_STR + 0x38u, COOP_STR + 0x40u };
    call(c, FN_DIALOG2, 0, 0, 10, a);
    coop_pick_show = 0; coop_pick_open = 1;
    fprintf(stderr, "[coop] f=%ld Add Player 2: car %d of %d, %s\n", autotest_frame(), coop_pick_i + 1, n, nm);
}
int coop_dialog_answer(CPU *c, uint32_t msg){
    (void)c;
    if (msg != MSG_KB && msg != MSG_CANCEL && msg != MSG_DBACK) return 0;
    long f = autotest_frame();
    if (!coop_asking) return 1;
    coop_asking = 0;
    if (msg == MSG_KB){
        fprintf(stderr, "[coop] f=%ld Add Player 2: the keyboard's left half\n", f);
        coop_pick_begin(COOP_DEV_KB);
    } else if (coop_close_steps){ coop_close_steps = 0; coop_pick_begin(coop_close_dev); }
    else fprintf(stderr, "[coop] f=%ld Add Player 2: cancelled\n", f);
    return 1;
}
/* the generic dialog up now (the FE manager's screens: n = [0x8637B4]; until n is the head: screen [n + 0x10]) */
#define VT_DIALOG     0x0079c88cu
static uint32_t coop_dialog(void){
    uint32_t head = G_FEMGR, n = MR32(head);
    for (int k = 0; n && n != head && k < 64; k++, n = MR32(n)){
        uint32_t scr = MR32(n - 4u + 0x14u);
        if (scr && MR32(scr) == VT_DIALOG) return scr;
    }
    return 0;
}
/* while the dialog asks: a START on a controller's seat makes it player 2's; the dialog goes with Back */
static void coop_ask_frame(CPU *c){
    if (coop_resume_next && !MR32(G_DLG)){
        coop_resume_next = 0;
        if (coop_ask_pkg){ uint32_t a[2] = { MSG_BACK, coop_ask_pkg }; call(c, FN_FE_MSG, 0, 0, 2, a); }
        fprintf(stderr, "[coop] f=%ld Add Player 2: the pause menu resumes\n", autotest_frame());
        return;
    }
    if (coop_asking){
        /* a button: the dialog keeps its message at +0x50 until it goes (0x54C1BC) */
        uint32_t d = coop_dialog(), v = d ? MR32(d + 0x50u) : 0;
        if (v == MSG_KB || v == MSG_CANCEL || v == MSG_DBACK){ coop_dialog_answer(c, v); return; }
    }
    if (!coop_asking) return;
    if (!MR32(G_DLG) && coop_close_steps){
        /* the dialog went after player 2's START: the car next */
        coop_asking = coop_close_steps = 0;
        coop_pick_begin(coop_close_dev);
        return;
    }
    if (!MR32(G_DLG)){ coop_asking = 0; return; }   /* gone another way */
    for (unsigned k = 0; k < 6 && !coop_close_steps; k++){
        /* the page's join stamp (a START claims a seat, readable once let go), or a START read from the seat */
        uint32_t st = gamepad_local_join_stamp(k);
        PadState p; int start = gamepad_read_local(k, &p) && (p.buttons & 1u << PAD_START);
        int edge = start && !(coop_ask_held & 1u << k);
        if (start) coop_ask_held |= 1u << k; else coop_ask_held &= ~(1u << k);
        if (st == coop_ask_stamps[k] && !edge) continue;
        coop_ask_stamps[k] = st;
        fprintf(stderr, "[coop] f=%ld Add Player 2: START on the controller of seat %u\n", autotest_frame(), k);
        coop_close_dev = (int)k;
        coop_close_steps = 1;
        uint32_t a[2] = { MSG_BACK, COOP_STR };
        call(c, FN_FE_MSG, 0, 0, 2, a);
    }
}
static void coop_outrun_frame(CPU *c);
static void coop_rt_frame(CPU *c);
void coop_ask_frame_(CPU *c){ coop_ask_frame(c); coop_pick_frame(c); coop_outrun_frame(c); coop_rt_frame(c); }

/* ---- outruns between player 1 and player 2 -------------------------------------------------------------------------
 * As outruns with an online friend (roam_game.c du_*): near each other (25 m) for 4 s, nothing else racing and no
 * tuner's outrun offered, both HUDs show the outrun icon (0x4DB890 with the other's car); either player's Engage
 * starts the game's own outrun (0x607180, as Engage starts one) with the World as a one-human game's for the call
 * (or_world_one: player 1's car the humans', player 2's alone the AI range); each frame the one getting away is ranked
 * first (the game ranks the opponent first, and only an AI's road path moves it); the game decides it once the other
 * is 300 m behind and ends it (0x5F8490, the World one human's again).  No career effect: the career's outrun result
 * (0x541320: the stake, the outrun's win, its SMS) is skipped for it. */
#define G_ENCOUNTERS  0x0085ef70u
#define G_OUTRUN_ST   (G_ENCOUNTERS + 0x1ae8u)   /* 2: a tuner's outrun can be taken */
#define G_ENGAGE      0x00838590u
#define G_ACTIONS     0x0086eaf0u   /* port 0's actions this frame (a bit each, 0 held) */
#define ACT_ENGAGE    15u
#define PL_OPPONENT   0x250u
#define PL_HUD        0x9a4u
#define FN_HUD_OUTRUN 0x004db890u   /* thiscall HUD (car or 0) */
#define FN_OUTRUN     0x00607180u   /* thiscall race */
#define FN_ENG_DONE   0x005010e0u   /* thiscall G_ENGAGE */
#define FN_EM_OVER    0x00500ca0u   /* thiscall G_ENCOUNTERS */
#define SPLIT_INPUT_CURRENT_COOP 0x00d01000u   /* splitscreen.c's ports' records (16 each): port 1 player 2's */
#define COOP_OUTRUNS  1
#define OR_NEAR_M     25.f
#define OR_NEAR_US    4000000
#define OR_KEEP_M     35.f
#define OR_RANK_MPS   2.f
static struct { int st; int64_t near_since, t; uint32_t icon1, icon2, eng1, eng2; float d, away, was[4]; uint32_t wins1, wins2; } OR;
enum { OR_IDLE, OR_OFFER, OR_RACE, OR_OVER };
static float or_dist(uint32_t a, uint32_t b){
    float dx = MRF32(a + 0x60u) - MRF32(b + 0x60u), dy = MRF32(a + 0x64u) - MRF32(b + 0x64u);
    return sqrtf(dx * dx + dy * dy);
}
static void or_icons(CPU *c, int on){
    uint32_t p1 = MR32(SPLIT_PLAYERS_INDEX), car1 = p1_car(), car2 = coop_p2_car();
    uint32_t h1 = p1 ? MR32(p1 + PL_HUD) : 0, h2 = coop_pl ? MR32(coop_pl + PL_HUD) : 0;
    uint32_t want1 = on ? car2 : 0, want2 = on ? car1 : 0;
    if (h1 && want1 != OR.icon1){ call(c, FN_HUD_OUTRUN, h1, 0, 1, &want1); OR.icon1 = want1; }
    if (h2 && want2 != OR.icon2){ call(c, FN_HUD_OUTRUN, h2, 0, 1, &want2); OR.icon2 = want2; }
}
int coop_outrun_on(void){ return OR.st == OR_RACE || OR.st == OR_OVER; }
static int or_starting;              /* or_start's call of the game's outrun start (sub_00532e30) */
void coop_outrun_world_end(void){
    if (OR.st != OR_IDLE) fprintf(stderr, "[coop] f=%ld the world ends: the outrun between the two players with it\n", autotest_frame());
    OR.st = OR_IDLE; OR.near_since = 0; OR.icon1 = OR.icon2 = 0;
}
/* the tuners' step puts player 1's HUD's outrun icon out each frame a tuner is not near: on again while offered */
void coop_outrun_icon(CPU *c){
    if (OR.st != OR_OFFER) return;
    uint32_t p1 = MR32(SPLIT_PLAYERS_INDEX), h1 = p1 ? MR32(p1 + PL_HUD) : 0, car2 = coop_p2_car();
    if (h1 && car2){ call(c, FN_HUD_OUTRUN, h1, 0, 1, &car2); OR.icon1 = car2; }
}
/* The World as a one-human game's around a call of the game's outrun code: player 1's car alone in the humans' range
 * [0, 1), then `n` cars as the AI range.  Player 2's car is a human's in the World's index (IndexCars puts kind 1
 * first: index 1), and the outrun's start and end take the local humans from where the AI range starts (World +0x14):
 * the start sets the local-human count [0x89E7C0] from it (0x6071D5) and ranks the AI range's racers first (0x6072D7),
 * the end keeps that many racers (0x5F8490).  With 2 there, player 1's HUD_SingleRace drew the 2P HUD's rpm lights
 * (0x4C68D0: 0x4C69C8 -> 0x4C6A33, the widget's +0x34.. which only a 2PHud fills) and trapped on the outrun's first
 * frame, the order's first was no racer, and free roam kept player 2's racer after it */
static struct { uint32_t first, end, cars[22]; } or_saved;
static int or_world_one(uint32_t w, const uint32_t *ai, uint32_t n){
    uint32_t first = MR32(w + 0x14u), end = MR32(w + 0x18u);
    if (first < 1u || first > end || end > 22u || 1u + n > 22u || MR32(w + 0x1cu) != p1_car()) return 0;
    or_saved.first = first; or_saved.end = end;
    for (uint32_t i = 0; i < 22u; i++) or_saved.cars[i] = MR32(w + 0x1cu + 4u * i);
    for (uint32_t i = 0; i < n; i++) MR32(w + 0x20u + 4u * i) = ai[i];
    MR32(w + 0x14u) = 1u; MR32(w + 0x18u) = 1u + n;
    return 1;
}
static void or_world_back(uint32_t w){
    for (uint32_t i = 0; i < 22u; i++) MR32(w + 0x1cu + 4u * i) = or_saved.cars[i];
    MR32(w + 0x14u) = or_saved.first; MR32(w + 0x18u) = or_saved.end;
}
/* player 2's car in the World's humans' range (IndexCars puts kind-1 cars first: player 1's, player 2's, a parked
 * one of player 2's), so the World is a two-human game's for the game's outrun code */
static int or_world_two(uint32_t w){
    uint32_t car2 = coop_p2_car(), first = w ? MR32(w + 0x14u) : 0;
    if (!coop_in() || !car2 || first < 2u || first > MR32(w + 0x18u) || MR32(w + 0x18u) > 22u || MR32(w + 0x1cu) != p1_car()) return 0;
    for (uint32_t i = 1; i < first; i++) if (MR32(w + 0x1cu + 4u * i) == car2) return 1;
    return 0;
}
/* the World's AI range as a one-human game's (or_world_two) */
static int or_world_ai_one(uint32_t w){
    uint32_t first = MR32(w + 0x14u), end = MR32(w + 0x18u), ai[22];
    for (uint32_t i = first; i < end; i++) ai[i - first] = MR32(w + 0x1cu + 4u * i);
    return or_world_one(w, ai, end - first);
}
/* The outrun's start (0x607180) with player 2 in, whoever's outrun it is (a tuner's from Engage, an online player's,
 * roam_game.c du_start): the World a one-human game's for it, its AI range as the caller left it */
void sub_00607180_orig(CPU *c);
void sub_00607180(CPU *c){
    uint32_t w = MR32(G_WORLD);
    int one = or_world_two(w) && or_world_ai_one(w);
    sub_00607180_orig(c);
    if (one) or_world_back(w);
}
static int or_start(CPU *c){
    uint32_t w = MR32(G_WORLD), race = MR32(G_RACE), p1 = MR32(SPLIT_PLAYERS_INDEX), car2 = coop_p2_car();
    if (!w || !race || !p1 || !car2 || MR32(race + 0x24u) != 1u || MR32(G_RACETYPE)) return 0;
    if (!or_world_one(w, &car2, 1)) return 0;                              /* the AI range: player 2's car alone */
    or_icons(c, 0);
    MR32(p1 + PL_OPPONENT) = car2; MR32(G_OUTRUN_ST) = 2u;
    or_starting = 1;
    call(c, FN_OUTRUN, race, 0, 0, 0);
    or_starting = 0;
    call(c, FN_ENG_DONE, G_ENGAGE, 0, 0, 0);
    or_world_back(w);
    if (MR32(G_RACETYPE) != 7u || MR32(race + 0x24u) != 2u){
        fprintf(stderr, "[coop] f=%ld the outrun did not start (race type %u, %u racers)\n", autotest_frame(), MR32(G_RACETYPE), MR32(race + 0x24u));
        MR32(p1 + PL_OPPONENT) = 0;
        return 0;
    }
    OR.away = 0; OR.t = 0;
    fprintf(stderr, "[coop] f=%ld outrun: player 1 against player 2, %.0f m apart\n", autotest_frame(), or_dist(p1_car(), car2));
    return 1;
}
/* the order: the one getting away from the other the faster first (smoothed over a second) */
static void or_rank(uint32_t race, int64_t now){
    uint32_t car1 = p1_car(), car2 = coop_p2_car();
    float p[4] = { MRF32(car1 + 0x60u), MRF32(car1 + 0x64u), MRF32(car2 + 0x60u), MRF32(car2 + 0x64u) };
    if (OR.t){
        float dt = (float)(now - OR.t) / 1e6f;
        if (dt > 0.f && dt < 0.5f){
            float ux = p[0] - p[2], uy = p[1] - p[3], n = sqrtf(ux * ux + uy * uy);
            if (n > 0.1f){
                ux /= n; uy /= n;
                float a1 = ((p[0] - OR.was[0]) * ux + (p[1] - OR.was[1]) * uy) / dt;     /* player 1 away from player 2 */
                float a2 = (-(p[2] - OR.was[2]) * ux - (p[3] - OR.was[3]) * uy) / dt;    /* player 2 away from player 1 */
                float k = dt > 1.f ? 1.f : dt;
                OR.away += ((a1 - a2) - OR.away) * k;
            }
        }
    }
    memcpy(OR.was, p, sizeof p); OR.t = now;
    uint32_t a = MR32(race + 0xbd0u), b = MR32(race + 0xbd4u);
    if (MR32(race + 0x24u) != 2u || !a || !b) return;
    int p1_first = MR32(a) == car1;
    int want = OR.away > OR_RANK_MPS ? 1 : OR.away < -OR_RANK_MPS ? 0 : p1_first;
    if (want != p1_first){ MR32(race + 0xbd0u) = b; MR32(race + 0xbd4u) = a; MR8(b + 0xau) = 1; MR8(a + 0xau) = 2; }
}
static int or_outcome(uint32_t race){
    uint32_t car1 = p1_car();
    for (uint32_t i = 0; car1 && i < 6; i++){
        uint32_t r = race + 0x30u + 0x1f0u * i;
        if (MR32(r) != car1) continue;
        uint32_t ct = MR16(r + 0x10u);
        return ct == 6u ? 1 : ct == 7u ? 2 : 0;
    }
    return 0;
}
static void coop_outrun_frame(CPU *c){
    uint32_t car1 = p1_car(), car2 = coop_p2_car(), race = MR32(G_RACE);
    int64_t now = roam_plat_now_us();
    if (!coop_in() || !car1 || !car2 || !race){
        if (OR.st == OR_OFFER) or_icons(c, 0);
        if (OR.st != OR_RACE && OR.st != OR_OVER) OR.st = OR_IDLE;
        return;
    }
    float d = or_dist(car1, car2);
    /* Engage: player 1's (port 0's actions) or player 2's (its seat's record), a new press */
    int e1 = !(MR32(G_ACTIONS) & 1u << ACT_ENGAGE), e2 = !(MR32(SPLIT_INPUT_CURRENT_COOP + 16u) & 1u << ACT_ENGAGE);
    int press = (e1 && !OR.eng1) || (e2 && !OR.eng2);
    OR.eng1 = e1; OR.eng2 = e2;
    if (!COOP_OUTRUNS && OR.st == OR_IDLE) return;
    switch (OR.st){
    case OR_IDLE: case OR_OFFER: {
        uint32_t p1 = MR32(SPLIT_PLAYERS_INDEX);
        int free = MR32(race + 0x24u) == 1u && !MR32(G_RACETYPE) && MR32(G_OUTRUN_ST) != 2u && coop_driving() &&
                   p1 && !MR32(p1 + 0x9acu);   /* (at a race marker Engage is the marker's: 0x602D99, Player +0x9AC) */
        if (!free || d > (OR.st == OR_OFFER ? OR_KEEP_M : OR_NEAR_M)){ OR.near_since = 0; if (OR.st == OR_OFFER){ or_icons(c, 0); OR.st = OR_IDLE; } break; }
        if (!OR.near_since) OR.near_since = now;
        if (OR.st == OR_IDLE && now - OR.near_since >= OR_NEAR_US){ OR.st = OR_OFFER; fprintf(stderr, "[coop] f=%ld outrun offered\n", autotest_frame()); }
        if (OR.st == OR_OFFER){
            or_icons(c, 1);
            if (press && or_start(c)) OR.st = OR_RACE;
        }
        break;
    }
    case OR_RACE: {
        if (MR32(G_RACETYPE) != 7u){ OR.st = OR_OVER; break; }
        or_rank(race, now);
        int o = or_outcome(race);
        if (o){
            if (o == 1) OR.wins1++; else OR.wins2++;
            fprintf(stderr, "[coop] f=%ld outrun: player %d wins, %.0f m apart (player 1 %u, player 2 %u)\n", autotest_frame(), o == 1 ? 1 : 2, d, OR.wins1, OR.wins2);
            OR.st = OR_OVER;
        }
        break;
    }
    case OR_OVER:
        if (MR32(G_RACETYPE) == 7u) break;                  /* its result still shown */
        { uint32_t p1 = MR32(SPLIT_PLAYERS_INDEX); if (p1 && MR32(p1 + PL_OPPONENT) == car2) MR32(p1 + PL_OPPONENT) = 0; }

        call(c, FN_EM_OVER, G_ENCOUNTERS, 0, 0, 0);
        OR.st = OR_IDLE; OR.near_since = 0;
        break;
    }
}
/* The outrun's end (0x5F8490: race type 0, the career's result below, the racers past the humans' let go, then the
 * AI range's cars new goals, 0x42EFB0) with player 2 in, as a one-human game's: the AI range the World's own; in one
 * between the two players player 2's racer let go as an AI opponent's is, and its car without a racer again */
void sub_005f8490_orig(CPU *c);
void sub_005f8490(CPU *c){
    uint32_t race = c->ecx, w = MR32(G_WORLD), car2 = coop_p2_car();
    int ours = race == MR32(G_RACE) && or_world_two(w) && or_world_ai_one(w);
    sub_005f8490_orig(c);
    if (!ours) return;
    or_world_back(w);
    uint32_t r = MR32(car2 + 0x1cu);
    if (r && r != race + 0x30u && MR32(r) == car2) MR32(car2 + 0x1cu) = 0;
    if (MR32(race + 0x24u) != 1u) fprintf(stderr, "[coop] f=%ld the outrun's end: %u racers\n", autotest_frame(), MR32(race + 0x24u));
}
/* The race's "everyone home?" (0x5F85B0, from the world's update: then the race's end): every local Player's +0x50
 * (its racer done) set.  Player 2 is no racer in a tuner's outrun or an online player's, which would never end: it
 * counts only in one between the two players */
void sub_005f85b0_orig(CPU *c);
void sub_005f85b0(CPU *c){
    if (!coop_in() || coop_outrun_on() || MR32(SPLIT_PLAYERS_COUNT) != 2u || MR32(SPLIT_PLAYERS_ORDER + 4u) != coop_pl){
        sub_005f85b0_orig(c); return; }
    MR32(SPLIT_PLAYERS_COUNT) = 1u;
    sub_005f85b0_orig(c);
    MR32(SPLIT_PLAYERS_COUNT) = 2u;
}
/* The career's outrun start (0x532E30, thiscall Career, from 0x607180 in the career: its kind-4 triggers queued at
 * Career +0x5D20, which the career saves, and 0x511000): not for one between player 1 and player 2, as its end
 * (0x541320 below) is not either */
void sub_00532e30_orig(CPU *c);
void sub_00532e30(CPU *c){
    if (or_starting){ POP32(c->eip); return; }
    sub_00532e30_orig(c);
}
/* the career's outrun result: not for one between player 1 and player 2 */
void sub_00541320_orig(CPU *c);
void sub_00541320(CPU *c){
    if (coop_outrun_on()){
        fprintf(stderr, "[coop] f=%ld the outrun between the two players: no career result\n", autotest_frame());
        POP32(c->eip); c->esp += 4u; return;
    }
    sub_00541320_orig(c);
}

/* ---- Race together: player 2 in player 1's career race ---------------------------------------------------------------
 * The race card's start (0x499B50, the career's StartRace) asks first while player 2 is in: Together or Alone (the
 * world's dialog over the card).  Alone: the race as in solo, player 2 waits (back beside player 1 after it).
 * Together: two humans, as the console's two-player races.  The race's setup (0x53FD30, run before the card's start)
 * forces its type's humans byte ([0x83AAC4 + type * 0x14]) to 1 and copies it to the local humans [0x89E7C0];
 * Together runs it again with the byte 2 (it only resets and refills the race's globals from the card's settings;
 * its cars, 0x525ED0, look human 1's car up in player 1's table, user 1's has an empty locked slot).  The career
 * race's build at the track load (0x5322E0) then makes player 1's racer from the career and the AI; player 2's racer
 * (its garage car, else player 1's car again; human 1, port 1) goes in right after player 1's, and the last AI
 * stays out when that makes more than the coordinator's 6 racers (URL races: 5 AI).  The game makes both
 * Players, view 2 and the 2P HUDs (2PHudTop/Bot) and ends the race once both are home; the career counts player 1's
 * place only (its win test reads car index 0).  Back in free roam the local humans are one again (the free-roam
 * setup 0x540780 never writes them). */
#define G_AI_N        0x0089e7c4u   /* the race's AI racers */
#define G_RTYPE       0x0083aab4u   /* the race's type (an index of the types' 0x14-byte records at 0x83AAC4) */
#define G_TYPE_HUMANS 0x0083aac4u
#define G_FE_PORT1    0x0083a9dcu
#define MSG_RT_TOGETHER 0xc0c0a011u
#define MSG_RT_ALONE    0xc0c0a012u
#define MSG_RT_BACK     0xc0c0a013u
static int coop_rt_go, coop_rt_ask, coop_rt_port_kept;
static uint32_t coop_rt_card, coop_rt_port;
int coop_racing_together(void){ return MR32(G_GAMEMODE) == 1u && MR32(G_HUMANS) == 2u && coop_want && coop_rt; }
/* the race's type gets two humans (the race's load copies the byte to [0x89E7C0]), human 1 port 1 */
static void coop_rt_humans(const char *when){
    if (!coop_rt || MR32(G_GAMEMODE) != 1u) return;
    uint32_t t = MR32(G_RTYPE);
    if (t < 0x20u) MR8(G_TYPE_HUMANS + t * 0x14u) = 2;
    if (!coop_rt_port_kept){ coop_rt_port = MR32(G_FE_PORT1); coop_rt_port_kept = 1; }
    MR32(G_FE_PORT1) = 1u;
    fprintf(stderr, "[coop] f=%ld the career race: two humans (type %u, %s)\n", autotest_frame(), t, when);
}
void sub_00499b50_orig(CPU *c);
void sub_00499b50(CPU *c){
    if (coop_rt_go || !coop_in() || MR32(G_DLG)){ coop_rt_go = 0; sub_00499b50_orig(c); return; }
    /* the question first; the race starts on its answer (coop_rt_frame) */
    coop_texts(c);
    char *s = (char *)g_mem + COOP_STR;
    memset(s, 0, 0x200);
    snprintf(s, 0x38, "IG_GenericDialog_MED.fng"); snprintf(s + 0x38, 8, "%%s"); snprintf(s + 0x40, 0x140, "%s", T.rt);
    for (char *p = s + 0x40; *p; p++) if (*p == '%') *p = '?';
    snprintf(s + 0x180, 0x40, "UI_EngageRaceDialog.fng");
    uint32_t a[10] = { COOP_STR + 0x180u, COOP_STR, T.together, T.alone, MSG_RT_TOGETHER, MSG_RT_ALONE, MSG_RT_BACK, 0, COOP_STR + 0x38u, COOP_STR + 0x40u };
    call(c, FN_DIALOG2, 0, 0, 10, a);
    coop_rt_card = c->ecx; coop_rt_ask = 1;
    fprintf(stderr, "[coop] f=%ld the race card: together or alone?\n", autotest_frame());
    POP32(c->eip);
}
static void coop_rt_frame(CPU *c){
    if (!coop_rt_ask) return;
    uint32_t d = coop_dialog(), v = d ? MR32(d + 0x50u) : 0;
    if (v != MSG_RT_TOGETHER && v != MSG_RT_ALONE && v != MSG_RT_BACK){
        if (!d && !MR32(G_DLG)){ coop_rt_ask = 0; MR8(coop_rt_card + 0x51u) = 0; }   /* gone another way */
        return;
    }
    if (MR32(G_DLG)) return;                                /* the dialog still going: start once it went */
    coop_rt_ask = 0;
    /* (the card marks itself started, +0x51, once its start returns (0x499C9D) and then takes no Accept or Back: the
     * card's again for another try or for Back) */
    if (v == MSG_RT_BACK){ MR8(coop_rt_card + 0x51u) = 0; fprintf(stderr, "[coop] f=%ld the race card: no answer\n", autotest_frame()); return; }
    coop_rt = v == MSG_RT_TOGETHER;
    fprintf(stderr, "[coop] f=%ld the race card: %s\n", autotest_frame(), coop_rt ? "together" : "alone");
    if (coop_rt) call(c, 0x0053fd30u, 0, 0, 0, 0);   /* the race's setup again (it ran before the card's start), two
                                                    * humans this time (the hook below) */
    coop_rt_go = 1;
    call(c, 0x00499b50u, coop_rt_card, 0, 0, 0);
}
/* the free-roam setup: one local human again, human 1's port as it was; a race's setup to cameras.c (the view after a drift event) */
void sub_00540780_orig(CPU *c);
void sub_00540780(CPU *c){
    cameras_setup();
    if (MR32(G_GAMEMODE) == 1u && MR32(G_HUMANS) == 2u){ MR32(G_HUMANS) = 1u; fprintf(stderr, "[coop] f=%ld free roam: one local human again\n", autotest_frame()); }
    if (coop_rt_port_kept){ MR32(G_FE_PORT1) = coop_rt_port; coop_rt_port_kept = 0; }
    coop_rt = 0;
    travel_presetup();                                      /* (the game's own reload on a race track's map: the city) */
    sub_00540780_orig(c);
    travel_setup();                                         /* a trip to a race track's map: its track */
}
/* player 2's racer in the career race, after player 1's (0x5322E0 makes player 1's from the career, then the AI):
 * human 1, its garage car (or a copy of player 1's ride), port 1; CreatePlayers then makes its Player */
static void coop_rt_racer(CPU *c){
    uint32_t n0 = MR32(G_RACEPARAMS + 0x74u), ride = COOP_RIDE;
    if (n0 != 1u){ fprintf(stderr, "[coop] the career race: player 1's racer not alone (%u)\n", n0); return; }
    if (!coop_car_rec || !coop_rec_ok(coop_car_rec) || !coop_build_ride(c, coop_car_rec, ride)) ride = G_RECORDS + 0x10u;   /* player 1's record's */
    coop_racer_name();
    memset(g_mem + COOP_REC, 0, REC_SZ);
    uint32_t ri[6] = { 1, 1, ride, COOP_NAME, 0xfffffffeu, 0 };
    call(c, FN_RACERINIT, COOP_REC, 0, 6, ri);
    uint32_t a = COOP_REC;
    call(c, FN_ADDRACER, G_RACEPARAMS, 0, 1, &a);
    MR32(0x0089e7ecu) = 1u;                     /* human 1's port */
    fprintf(stderr, "[coop] f=%ld the career race: player 2's racer (%s), numbers %d %d, entries %u\n", autotest_frame(),
            ride == COOP_RIDE ? "its garage car" : "player 1's car", (int)MR32(0x0089e808u), (int)MR32(0x0089e80cu), MR32(G_RACEPARAMS + 0x74u));
}
int coop_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x0053fd96):                 /* 0x53FD30 forced the type's humans to 1 */
        coop_rt_humans("the setup");
        return 1;
    case HOOK_SITE(0x005324f1):                 /* 0x5322E0, the career race's racers: player 1's just added */
        if (coop_racing_together()) coop_rt_racer(c);
        return 1;
    case HOOK_SITE(0x005324f7):                 /* 0x5322E0: the AI added (0x5260B0) after both players */
        if (coop_racing_together() && MR32(G_RACEPARAMS + 0x74u) > 6u && MR32(G_AI_N) >= MR32(G_RACEPARAMS + 0x74u) - 6u){
            /* the race coordinator holds 6 racers (+0x30, 0x1F0 each, to +0xBD0): the last AI stays out */
            uint32_t drop = MR32(G_RACEPARAMS + 0x74u) - 6u;
            MR32(G_RACEPARAMS + 0x74u) = 6u; MR32(G_AI_N) -= drop;
            fprintf(stderr, "[coop] f=%ld the career race: %u AI out for player 2 (6 racers at most)\n", autotest_frame(), drop);
        }
        return 1;
    case HOOK_SITE(0x00525fb9):                 /* 0x525ED0: human esi's car handle in edi, its user's table in edx */
        if (coop_racing_together() && c->esi == 1u){
            uint32_t want = MR32(G_CAREER_CAR), tab = G_FECARS + 0x9bd8u;
            for (uint32_t i = 0; coop_car_rec && i < 0x5bu; i++) if (MR32(tab + 4u * i) == coop_car_rec){ want = MR32(coop_car_rec + 8u); break; }
            c->edi = want; c->edx = G_FECARS; MR32(c->esp + 0x1cu) = G_FECARS;
            fprintf(stderr, "[coop] f=%ld the career race: player 2's car 0x%08x (%s)\n", autotest_frame(), want,
                    want == MR32(G_CAREER_CAR) ? "player 1's car" : "player 2's pick");
        }
        return 1;
    }
    return 0;
}
