/* roam_game.c -- free-roam multiplayer: the game side (v1.2 NA port).
 *
 * Friends on the same LAN, each in their own career free roam, see each
 * other's cars: right model and customization, moving smoothly, with their
 * names on the minimap arrow's racer record.  Off unless NFSU2_ROAM=1.
 * The wire format is in roam_core.h.
 *
 * Per frame, at the main-loop hook between World::DoTimestep and the camera
 * and render (roam.c's instruction hook at 0x581475), on the guest thread:
 *  - a sync clock: the sum of world dt (what the cars move by), slewed to
 *    the host clock (at most 5 %) and following it while the world stands
 *    (pause, SMS, streaming), stamps the local samples and is the render
 *    clock of the remote cars;
 *  - the local car's state goes to the session (roam_core), which sends a
 *    snapshot 20 times a second; its car look (type and part hashes) goes
 *    out when it changes;
 *  - remote players get "slots": up to 32 cars of our own (CreateCar), the
 *    nearest players first, out of World::IndexCars' array (22 cars), on the
 *    World's list only, which is what the renderer walks (finish_create), so
 *    the game's outrun challengers, which take the index's AI range, never
 *    meet one and come as in the game (em_update); our cars get their
 *    minimap arrows and world map markers here (arrows_update, map_update).
 *    The Windows plugin (LEND_OPP) has the game's index and paint slots only:
 *    the 3 parked free-roam opponent cars (the challengers' cars; the
 *    RandomEncounterManager is held off while we use them) and 2 cars of our
 *    own, indexed as AI racers.  The game has 6 paint slots
 *    (DUMMY_SKIN1-6: the player, the 3 opponent cars, the first 2 of ours);
 *    the others composite into textures of our own (skin_textures,
 *    roam_game_texture).  A slot's car gets the player's ride (streamed
 *    in through the CarLoader, then Car::SetRideInfo), no brain and no
 *    physics, and its pose is written every frame from the core's
 *    interpolation.  Solid cars (the default) have their collision body
 *    while they are near the player's car (SOLID_M: no other can touch it
 *    this frame; two friends' cars never need to touch each other, as each
 *    is where its player put it, but the game tested every pair of them),
 *    except while they are ghosts: from the moment they appear (or jump,
 *    or the player's car does) until they are clear of the player's car,
 *    so nobody is shoved out of a car that appeared inside theirs, and
 *    while they are passable: paused, standing for a minute, near a door,
 *    either player near one of their own open events (the friend's game
 *    says so in its snapshots: RC_BIT_PASS), or the player at the new
 *    career's start (soft_update), so nobody walls off a shop, the garage
 *    or a race's start; a friend's car at that start is not shown at all
 *    (at_spawn): every new player appears on that spot.  They are
 *    drawn where the friend is now (predicted by half the round trip), so
 *    a contact is at the same spot on both screens.  Ghost cars
 *    (NFSU2_ROAM_COLLIDE=0) have no collision body and are drawn a little in
 *    the past (NFSU2_ROAM_DELAY), smoothed through the jitter buffer; the
 *    player's own car is then passable on every friend's screen too.  The
 *    page's menu switches between the two while the game runs (solid_follow).
 *  - names over friends' cars, as the game's own LAN races show them
 *    (roam_game_hud).
 *  - horns: the player's goes out with the snapshots, and which horn it is
 *    with the car's look; friends' sound from their cars (sounds_update,
 *    horn.c).
 *  - engines: the snapshots' extension carries the player's engine as the
 *    game sounds it (eng_state); a friend's car sounds with its own engine
 *    sound's recordings at that (eng_files, eng_sound, carsfx.c), and the
 *    game's AI engine on a free-roam opponent's car a friend drives is
 *    silenced (aieng_update).
 *  - hydraulics and nitrous: the snapshots' extension carries the wheel
 *    places' heights, the nitrous left, each wheel's hydraulics and the
 *    landings; a friend's car gets its wheels where the friend's are
 *    (wheels_pose: the hydraulics lift the body, the pose has that) and a
 *    shot of nitrous of its own while theirs is on, so the game draws its
 *    streaks and flames (nos_pose), and both sound from it (sounds_update,
 *    carsfx.c: the game's own samples).
 *  - outruns: two players who stay near each other can challenge each other
 *    to the game's own outrun, raced against each other's car (du_frame),
 *    away from the doors and their open events.
 *  - keep right: a new player's car driving out of the new career's start
 *    gets a line at the top asking them to keep right (kr_frame).
 *  - leaving free roam (shop, crib, race, menus) parks the cars; their
 *    rides are unloaded once the world is gone.  A peer that leaves, times
 *    out or goes into a shop or race disappears (soft-parked), and its slot
 *    is given back after a while.
 * The network itself runs on roam_bsd.c's thread; the game thread only
 * takes the session lock for a few microseconds per frame.
 *
 * The same file is the game side of the Windows companion for the original
 * PC game (win/roam_asi, NFSU2FreeRoam.asi), built with -DROAM_HOST_WIN:
 * every guest access, guest call, option, log line and clock goes through
 * the host names of roam_host.h (the port) or roam_host_win.h (Windows), and
 * every float operation is a basic IEEE operation (no libm; both builds use
 * -ffp-contract=off), so both platforms make the same writes into the game
 * for the same inputs (tests/asi_harness.c checks that).
 */
#ifdef ROAM_HOST_WIN
#include "roam_host_win.h"
#else
#include "roam_host.h"
#endif
#include "roam_core.h"
#include "roam_plat.h"
#include "carsfx.h"
#include "names.h"

/* ---- game addresses (v1.2 NA) ------------------------------------------------ */
/* multiplayer races from free roam (mp_*, mprace.c): not in the Windows plugin */
#ifdef ROAM_HOST_WIN
#define MPRACES         0
#else
#define MPRACES         1
#endif
#define G_FLOW          0x008654a4u   /* GameFlowManager state: 3 front end, 6 in the world */
#define G_GAMEMODE      0x00850078u   /* 1 career, 0x200 LAN */
#define G_WORLD         0x00890080u   /* World*: +0x10 cars, +0x14/+0x18 AI range, +0x1C cars[] */
#define G_PLAYER1       0x008900acu   /* Player*: +4 car */
#define G_RACE          0x00890118u   /* race coordinator: +0 state (4 racing), +0x24 racers, +0xC40 encounters */
#define G_CAREER_RACE   0x00863470u   /* career race being run (0 = free roam) */
#define G_TRACK         0x0089e7a0u   /* race parameters: the track (4000 free roam; 4701.. URL, 4706 the Dyno Shop) */
#define G_RACETYPE      0x0089e7b0u   /* race parameters: 0 free roam, 1 a race, 7 an outrun, 8 the Dyno Shop */
#define RACETYPE_DYNO   8u
#define G_WORLDDT       0x00865154u   /* float: this frame's world dt (s) */
#define G_PROFILE_NAME  0x0083a9e0u   /* 16 bytes */
#define G_CARTYPES      0x008a1cccu   /* CarTypeInfo[]: 0x890 bytes, +0 name, +0xD0 name hash */
#define TYPE_TRAFFIC    31            /* CarTypeInfo 0..30 are the player cars, 31..45 traffic (TAXI .. BUS, HATCHBACK02), */
#define TYPE_STOCK      46            /* ...and Unlimiter adds car types after them */
#define G_PARTOWNERS    0x008a1cb8u   /* CarPart byte +7 -> owner (car type) hash table */
#define G_CARPARTDB     0x008a2b68u
#define G_CARLOADER     0x008a3288u   /* entries on +0x58, handle at entry +0x123C */
#define G_RIDEPOOL      0x008a1d00u   /* bSlotPool* of its entries: +0x18 in use, +0x2C entries (0x43CD10: all taken) */
#define G_CARPOOL_KB    0x007fa9d4u   /* free-roam "Cars" pool size (KB), read at the track load */
#define CARPOOL_KB_GAME 11000         /* its size in the game */
#define CARPOOL_KB      48000         /* free roam's (NFSU2_ROAM_CARPOOL_KB) */
#define G_CARS_POOL     0x00828164u   /* bMemory pool 4: +0x24 size, +0x28 used */
#define G_RECORDS       0x00890160u   /* racer records, 0x940 bytes each */
#define RC_ENCOUNTERS   0xc40u
#define FN_MALLOC       0x00575620u   /* cdecl (size) */
#define FN_CAR_ACTIVATE 0x005f6d50u
#define FN_CAR_DEACTIVATE 0x005f6e20u
#define FN_CAR_SETDRIVER 0x005eba30u  /* thiscall (type) */
#define FN_CAR_SETPHYS  0x005f46a0u   /* thiscall (mode) */
#define FN_CAR_SECTION  0x005f6cd0u   /* thiscall (): track section / collider after a move */
#define FN_ROADTRACK    0x005da330u   /* thiscall (car +0x8D8, pos*): road tracker, as Car::SetPosition does */
#define FN_CAR_BODYPOS  0x005f5010u   /* thiscall (): +0x50 = +0x60 - centre-of-mass offset (and a ground snap) */
#define FN_CAR_FX       0x0061fb00u   /* thiscall (effect, float strength, 0, 0), callee pops: a car's effect this frame */
#define FX_NOS          0xau          /* ...the nitrous flames (0x5F5730: on the throttle, from 5 mph) */
#define FX_PURGE        0xdu          /* ...the nitrous let out (standing, off the throttle; and before a race) */
#define CAR_SUSP        0x380u        /* float[4]: each wheel's suspension offset (m, up), 0 at rest */
#define CAR_STEER       0x40eu        /* u16: the front wheels' steering angle (65536 a turn, left positive) */
#define CAR_SPIN        0x4e8u        /* float[4]: each wheel's spinner angle (rad); +0x10: its speed (rad/s) */
#define CAR_NOS         0x40u         /* Car: its nitrous (+0x414 a full shot, +0x41C left, +0x420 on since; nos_left) */
#define G_TICKS         0x0086518cu   /* game time, 4000 ticks a second: the nitrous's clock (0x592480) */
#define G_HYD_LEVEL     0x007e949cu   /* int: every car's hydraulics level, -1: each its own (a debug setting) */
#define CAR_PHYS        0x34u         /* Car: its physics (+CAR_HYD + 4 k: wheel k's hydraulics, 0x5ED470, in physics mode 1) */
#define CAR_HYD         0x74u
#define CAR_BUMP        0x408u        /* u8[4]: wheel k landed this frame (the hydraulics' landing, 0x47DD40) */
#define CAR_WSPIN       0x330u        /* float[4]: each wheel's turning speed (rad/s) */
#define CAR_GROUND      0x390u        /* u8[4]: wheel k is on the ground */
#define CAR_SURF        0x394u        /* u32[4]: the surface under wheel k (the table at 0x800EE0, N_SURF of them) */
#define CAR_SLIP        0x3b4u        /* float[4][2]: wheel k's slip along it and across it (m/s): smoke from 5 on a road (0x5F5730) */
#define N_SURF          29u
#define CAR_SOUND       0x968u        /* Car: its sound state: +0x30 the car, +0x20 its children (each +0x50 the next) */
#define VT_SFX_NOS      0x0078ac58u   /* CARSFX_Nitrous (vtable): its update 0x484F20 sounds a car's nitrous */
#define VT_SFX_SKIDS    0x0078ae48u   /* CARSFX_Skids (vtable, 0x473A70): the skid sound, from the wheels' slip (0x46EA50) */
#define FN_CAR_SETRIDE  0x00601110u   /* thiscall (RideInfo*) */
#define FN_BODY_DISABLE 0x005930f0u   /* DynamicCollisionBody (car +0x3C): ++[+0x17] */
#define FN_TYPE_BY_HASH 0x00610130u   /* cdecl (hash) -> CarTypeInfo* or 0 */
#define FN_GETCARPART   0x0061b980u   /* thiscall CarPartDB (type, slot, key, prev, -1) */
#define FN_RIDE_INIT    0x00610270u   /* thiscall (type, 0, b5, b6) */
#define FN_RIDE_STOCK   0x00637040u   /* thiscall (0): stock parts */
#define FN_RIDE_PARTSON 0x0061bcd0u   /* thiscall (): UpdatePartsEnabled */
#define FN_RIDE_SETPART 0x00631d20u   /* thiscall (slot, part) -> the part it had: SetPart, then UpdatePartsEnabled */
#define G_SLOTKIND      0x00802960u   /* u32[RC_PART_SLOTS]: a ride slot's parts' kind (CarPart +4; NewGetCarPart) */
#define G_SLOTNAMES     0x00802decu   /* {char *name, u32}[]: a ride slot's name ("SPOILER" for 0xC) */
#define FN_PART_ATTR    0x0061b500u   /* thiscall CarPart (attribute hash, 0) -> its value */
#define FN_PART_NAME    0x00610000u   /* thiscall CarPart () -> its name */
#define FN_HASH         0x0043db50u   /* cdecl (string) -> the game's hash of it */
#define ATTR_WHEELTYPE  0xebb03e66u   /* a wheel's type: the hash of "SPINNER" for a spinner */
#define ATTR_RIMSIZE    0xce7d8db5u   /* a wheel's size: the car type's +0xDC for one that fits (the shop's list) */
#define S_SPINNER       0x0079bde0u   /* "SPINNER" */
#define FN_RIDE_SKINS   0x0061c280u   /* thiscall (n): DUMMY_SKIN<n> / WHEEL / SPINNER */
#define FN_PERF_SET     0x005ac4b0u   /* thiscall RideInfo +0x2F0 (perf*): installed performance parts from bits */
#define FN_PHYS_UPDATE  0x005b4fc0u   /* thiscall RideInfo +0x10 (1, 1, 1): physics from tuning and parts */
#define FN_LOADER_LOAD  0x0063bc70u   /* thiscall (ride, 0, 0) -> handle */
#define FN_LOADER_SERVICE 0x0063bcd0u /* thiscall (0, 0) */
#define FN_LOADER_LOADED 0x00610910u  /* thiscall loader (entry) */
#define FN_LOADER_WAIT  0x0061ca50u   /* thiscall (handle) */
#define FN_LOADER_UNLOAD 0x0063ae00u  /* thiscall (handle) */
#define FN_LOADER_PURGE 0x0063a1b0u   /* thiscall (phase), callee pops: phase 0 unloads one unreferenced entry -> 1, none -> 0 */
#define LOADER_BUSY     0x14u         /* CarLoader +0x14 (0x8A329C): a step of a load under way (a pack, its textures; one at a time) */
#define FN_CREATECAR    0x00606480u   /* ecx grid {vec3, +0x10 u16 heading}, edx record, (driver, start) */
#define FN_INDEXCARS    0x005f0d20u   /* thiscall World */
#define FN_MM_REBUILD   0x004f92a0u   /* cdecl (): the minimap's icons made again (0x4ECC80), as Player 0x5EA980 does */
#define FN_EM_OVER      0x00500ca0u   /* thiscall G_ENCOUNTERS: the outrun is over (the manager's state 5 calls it) */
#define G_ENCOUNTERS    0x0085ef70u   /* the career's outrun bookkeeping (the manager asks it whether one is due) */
#define EM_TABLE        0x1b10u       /* manager: 3 x {+0 time, +4 state, +8 flag, +0xC car} */
#define EM_STANDDOWN_US 20000000      /* its cars out of sight (250 m) by then, or parked anyway */
#define PARK_Z          (-123456.f)   /* Car::Deactivate's parking spot (0, 0, -123456) */
#define RIDE_SZ         0x900u
#define REC_SZ          0x940u
/* Friends' cars and the outrun challengers.  The Windows plugin has the
 * game's paint slots and car index only: friends get the 3 parked opponent
 * cars first (slots 0-2) and the challengers' manager is held off while they
 * are about (LEND_OPP).  The port makes every friend's car one of our own,
 * outside the car index, which the manager never reads: the challengers
 * come as in the game, held off only around an outrun between players and
 * the race lobbies (em_update) */
#ifdef ROAM_HOST_WIN
#define LEND_OPP        1
#define NOPP            3             /* slots 0-2: the parked free-roam opponent cars */
#else
#define LEND_OPP        0
#define NOPP            0
#endif
#define NSLOT           32            /* NOPP opponent cars + the rest of our own */
#define WORLD_INDEX     22            /* World::IndexCars' cars[] (World +0x1C; other fields follow) */
#define KIND_OUT        4             /* a racer record kind IndexCars skips (1 human, 2 AI racer, 3 traffic) */
#define SKIN_GAME       6             /* the game's paint slots, DUMMY_SKIN1-6 */
#ifdef ROAM_HOST_WIN
#define MAX_CARS        5             /* the Windows plugin: the game's own paint slots and car index only */
#else
#define MAX_CARS        NSLOT
#endif
#define CLEAR_M         6.f           /* a ghost becomes solid once its centre is this far from the player's car's */
#define SOLID_M         20.f          /* ...and has its body only this near it (cars touch from ~5 m; a frame at 20 fps and 160 m/s closing is 8) */
#define SPAWN_X         1695.3f       /* the new career's start, where every new player appears (the airport's car park) */
#define SPAWN_Y         (-883.6f)
#define SPAWN_Z         24.1f
#define SPAWN_M         5             /* a car this near it is not shown to others, and the player there is passable */
#define JUMP_M          5.f           /* a friend's car drawn this far from where it was last frame is a ghost again */
#define HIT_REPORT_US   50000         /* a hit's push (roam_game_contact) holds until the friend's snapshots from this long after it come */
#define NUDGE_REPORT_US 20000         /* ...a push of 1 m/s or less (cars leaning on each other) this long */
#define HIT_HOLD_US     400000        /* ...or this long at most; then it fades into where their game has the car */
#define HIT_FADE_US     100000        /* (time constant: the interpolation's own for a correction, rc_interp_eval) */
#define PUSH_Q          8             /* pushes held at once (more: merged into the newest) */
#define IDLE_MPS        1.f           /* a car slower than this stands */
#define IDLE_US         60000000      /* a car that has stood this long is passable */
#define BLOCK_M         6.f           /* the player's car that stays this near one spot */
#define BLOCK_US        20000000      /* ...this long */
#define BLOCK_HITS      4             /* ...and has had this many hits */
#define BLOCK_CARS      2             /* ...with this many friends' cars */
#define BLOCK_WIN_US    60000000      /* ...in this long, anywhere, is passable until it leaves the spot (block_hit) */
#define BLOCK_GAP_US    1000000       /* a friend's car touching it again after this long is another hit */
#define BLOCK_Q         64            /* hits kept: more than one car's in BLOCK_WIN_US (one each BLOCK_GAP_US) */
#define DOOR_M          20.f          /* a car this near a door's trigger is passable */
#define G_TRIGGERS      0x0088f298u   /* the track's triggers: +0x440 + 4t .. +0x444 + 4t those of type t (0x5D9BF0) */
#define TRIG_DOOR       14            /* the triggers of the shops, the garage and the car lot (0x5EB040 asks types 13-15) */
#define MAX_DOORS       64
#define TRIG_EVENT      13            /* the events' triggers, the whole career's: open or not by the player's career */
#define MAX_EVENTS      128
#define EV_PER_FRAME    8             /* event triggers asked a frame (events_read) */
#define EV_SWEEP_US     4000000       /* ...all of them again this often: a race won or an SMS opens new ones */
#define EV_SCR          0x00u         /* g_gmem scratch: the 5 dwords FN_MARKER_WHAT fills */
#define G_CAREER        0x0085ad40u   /* Career: the player's (+0x71C8 the races open now, 0x5007E0) */
#define G_STRPOOL       0x00838428u   /* the career tables' strings: a race's name at +[race +0] (u16) */
#define FN_MARKER_WHAT  0x00527800u   /* thiscall Career (out, trigger), callee pops: out = {the trigger or 0, race, shop, showcase, ...} */
#define FN_MARKER_OPEN  0x00532a50u   /* thiscall Career (out), callee pops -> al: Engage enters it now (as 0x5EAFB0 asks) */

/* ---- guest access ----------------------------------------------------------------- */
static int ok(uint32_t a, uint32_t n){ return a && rh_committed(a) && rh_committed(a + n); }
static uint32_t rd32(uint32_t a){ return ok(a, 3) ? MR32(a) : 0u; }
static uint32_t gcall0(CPU *c, uint32_t fn, uint32_t ecx){ return rh_call(c, fn, ecx, 0, 0, 0); }
static uint32_t gcall1(CPU *c, uint32_t fn, uint32_t ecx, uint32_t a){ return rh_call(c, fn, ecx, 0, 1, &a); }
static uint32_t player_car(void){ uint32_t p = rd32(G_PLAYER1); return p ? rd32(p + 4u) : 0; }
static uint32_t world(void){ uint32_t w = rd32(G_WORLD); return ok(w, 0x80) ? w : 0; }
static int du_racing(uint32_t race);
static int em_racing(uint32_t race);
/* free roam: the career's world with no race but free roam's own one-racer
 * one, or an outrun with a friend (du_racing) or with one of the game's
 * challengers (em_racing): that is still free roam.  On
 * free roam's own track: Performance Tuning / Dyno is one racer and no
 * career race too, but on tracks of its own (the Dyno Shop 4706, a circuit
 * test run on URL track 4705), whose places are not the city's -- friends
 * saw those cars stand and race in the air over them (players, 2026-10-25) */
static int roaming(void){
    uint32_t race = rd32(G_RACE);
    return rd32(G_FLOW) == 6u && rd32(G_TRACK) % 1000u == 0 && ok(race, 0xc44) && MR32(race) == 4u &&
           (MR32(race + 0x24u) == 1u || du_racing(race) || em_racing(race)) &&
           !rd32(G_CAREER_RACE) && !(rd32(G_GAMEMODE) & 0x200u) && world() && player_car();
}
static uint32_t encounter_mgr(void){ uint32_t r = rd32(G_RACE); return ok(r, 0xc44) ? r + RC_ENCOUNTERS : 0; }
/* one of the game's own opponent cars: an AI racer (kind 2) whose racer
 * record is in the game's record array -- never a car of ours, whose record
 * is a copy in our own memory (World::IndexCars sorts both kinds into the
 * same AI range, in the order of the World's list, which the game changes) */
static int game_opponent(uint32_t car){
    uint32_t rec = ok(car, 0x9e0) ? rd32(car + 0x14u) : 0;
    return rec >= G_RECORDS && rec < G_RECORDS + 22u * REC_SZ && (rec - G_RECORDS) % REC_SZ == 0 && MR32(rec + 4u) == 2u;
}
static uint32_t ai_car(uint32_t w, int i){
    int first = (int)MR32(w + 0x14u), end = (int)MR32(w + 0x18u);
    if (first < 0 || end > 22 || i < 0 || first + i >= end) return 0;
    uint32_t car = rd32(w + 0x1cu + 4u * (uint32_t)(first + i));
    return ok(car, 0x9e0) ? car : 0;
}
static int car_out(uint32_t car){ return __builtin_fabsf(MRF32(car + 0x68u) - PARK_Z) >= 0.001f; }   /* 0x5ED430 */
#define RIDE_ENTRIES    128           /* the car loader's ride entries with free-roam multiplayer on (g_pools) */
static uint32_t loader_entry(uint32_t h){
    uint32_t head = G_CARLOADER + 0x58u, e = rd32(head);
    for (int k = 0; e && e != head && k < 4 * RIDE_ENTRIES; k++, e = rd32(e)) if (rd32(e + 0x123cu) == h) return e;
    return 0;
}
/* The car loader serves its loads one after another, about half a second
 * each: a load may take 15 s, and a second more for each load ahead of it
 * (32 friends arriving at once queue 61 loads, a car of our own's and the
 * friend's ride each) */
static int loads_waiting(void);
static int64_t load_wait_us(void){ return 15000000 + 1000000 * (int64_t)loads_waiting(); }
static void cstr(uint32_t a, char *o, int n){
    int i = 0;
    if (ok(a, (uint32_t)n)) for (; i < n - 1 && MR8(a + (uint32_t)i) >= 0x20 && MR8(a + (uint32_t)i) < 0x7f; i++) o[i] = (char)MR8(a + (uint32_t)i);
    o[i] = 0;
}
static uint32_t pool_free(void){
    uint32_t p = rd32(G_CARS_POOL);
    if (!ok(p, 0x30)) return 0xffffffffu;
    uint32_t sz = MR32(p + 0x24u), used = MR32(p + 0x28u);
    return used <= sz ? sz - used : 0;
}
static void pool_log(const char *why){
    uint32_t p = rd32(G_CARS_POOL);
    if (ok(p, 0x30)) rlog("[roam] f=%ld car pool (%s): %.2f of %.2f MB used\n", rh_frame(), why,
                             MR32(p + 0x28u) / 1048576.0, MR32(p + 0x24u) / 1048576.0);
}

/* ---- options --------------------------------------------------------------------------- */
/* a player's option without a comment after it: the Mac's settings.ini only
 * knows whole-line comments, so "roam_name = Thom   ; me" would make the
 * comment part of the name; on both platforms a ';' or '#' at the start or
 * after a blank starts a comment ("Team#1" stays a name) */
static const char *opt(const char *k){
    const char *v = rh_opt(k);
    if (!v) return 0;
    static char buf[4][256]; static int nb;
    char *o = buf[nb++ & 3];
    int n = 0;
    for (int i = 0; v[i] && n < 255; i++){
        if ((v[i] == ';' || v[i] == '#') && (i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t')) break;
        o[n++] = v[i];
    }
    while (n && (o[n - 1] == ' ' || o[n - 1] == '\t')) n--;
    o[n] = 0;
    return o;
}
static int env_int(const char *k, int def, int lo, int hi){
    const char *v = opt(k);
    if (!v || !*v) return def;
    long x; char *e = 0;
    if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true")) x = 1;
    else if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false")) x = 0;
    else { x = strtol(v, &e, 10);       /* decimal, as settings.ini checks it: "047300" is 47300, not octal */
           if (e == v || *e){ rlog("[roam] %s=\"%s\" is not a number, using %d\n", k, v, def); return def; } }
    if (x < lo || x > hi){ rlog("[roam] %s=%ld out of range %d..%d, using %d\n", k, x, lo, hi, def); return def; }
    return (int)x;
}
static struct {
    int on, test, collide, max_cars, rate, delay_ms, extrap_ms, carpool_kb, labels, spawn_m;
    int test_owncars;                         /* NFSU2_ROAM_TEST_OWNCARS (test aid): friends only in cars of our own (slots NOPP and up) */
    int test_outside;                         /* NFSU2_ROAM_TEST_OUTSIDE (test aid): every car of our own outside the car index */
    int hit_share;                            /* NFSU2_ROAM_HIT_SHARE: % of a hit with a friend's car the player's car keeps (roam_game_contact);
                                                 -1: by the two cars' weights (car_mass, their square roots), 100: the game's own (the whole hit) */
    int test_mass, test_fmass;                /* NFSU2_ROAM_TEST_MASS / _FMASS (test aid, kg): car_mass of the player's car / friends' cars */
    char name[RC_NAME_MAX + 1], room[64];
    char crew[RC_CREW_MAX][100]; int ncrew;   /* NFSU2_ROAM_CREW: the crews' names, in the player's order (crews_parse; the
                                                 page's 24 characters are up to 96 bytes of UTF-8) */
    uint32_t crew_rgb[RC_CREW_MAX];           /* ...and their colours (NFSU2_ROAM_CREW_RGB, the player's picks; else CREW_RGB) */
} O;

/* ---- state ------------------------------------------------------------------------------- */
static rc_session g_s;
static int g_inited, g_net_ok;
static long g_frame0;
/* sync clock (µs): world time pulled to the host clock */
static double g_sync; static int g_sync_paused, g_in_roam;
static uint32_t g_step;                       /* frames (world steps) counted here: roam_game_contact's passes of one step */
static double g_vscale = 1.0;                 /* world seconds per stamp second (velocities on the wire) */
static int64_t g_last_host;
/* local car */
static float g_lp[3]; static int g_lp_ok;
static float g_lv[3];                         /* its velocity (m/s of world time) */
static float g_lf[2];                         /* where it points: its forward row in x and y, of length 1 (or 0) */
static int64_t g_cfg_next;
static int g_horn;                            /* the player's horn sounds this frame */
static uint8_t g_thumps; static int g_landed;  /* the player's car's landings so far; it landed last frame */
/* guest memory for rides and records (allocated once, never freed) */
static uint32_t g_gmem;
/* slots */
/* FREE: no friend, the car (if any) has its own ride.  CREATING: a car of
 * our own is being made.  LOADING: the friend's ride is streaming in.
 * ACTIVE: the friend's car is shown (or hidden while the friend is in a shop
 * or race).  PARKED: no friend any more, but the car keeps the last friend's
 * ride, hidden, so that the same look coming back costs nothing (no new
 * load, no new composited skin) */
enum { SL_FREE, SL_CREATING, SL_LOADING, SL_ACTIVE, SL_PARKED };
typedef struct {
    int      st;
    int      peer; uint32_t peer_id;          /* session table index and id */
    char     name[RC_NAME_MAX + 1];
    uint32_t car, world;                      /* the slot's car in this world */
    int      created;                         /* one of our own cars (k >= NOPP) */
    int      unindexed;                       /* ...past World::IndexCars' 22: record kind KIND_OUT */
    uint32_t ride_new, ride_orig, rec;        /* guest buffers */
    uint32_t h_cur, h_new;                    /* CarLoader handles (on the car / loading) */
    uint32_t h_orig;                          /* a car of our own: its first ride (an opponent's copy) */
    int      have_orig, activated;
    uint32_t cfg_gen;  int pending_cfg;       /* the record the car shows / one waiting */
    uint32_t look;                            /* hash of the record the car shows (0: none) */
    int64_t  load_t0, load_wait; long load_f0;    /* load_wait: how long the load may take (loads_waiting) */
    int      hidden;
    int      paused;                          /* the friend is paused (for a second now): so says the name over the car */
    int64_t  paused_since;
    int64_t  near_since;                      /* within DU_NEAR_M of the player's car since (0: not), for an outrun */
    int      ghost;                           /* solid cars: no collision body until clear of the player's car */
    int      soft;                            /* passable now (SOFT_*): no collision body, whatever the ghost rule */
    int64_t  idle_since;                      /* the friend's car stands since (0: it moves) */
    float    drawn[3];                        /* where the car was drawn last frame (+0x60) */
    float    drawn_v[3];                      /* ...and its velocity there (+0x70) */
    /* hits with the player's car push it on this screen (push_pose) */
    struct { float v[3], v0[3]; int64_t t; } pq[PUSH_Q]; int pqn;   /* the pushes the friend's game has not reported yet, oldest first
                                                                       (v0: the friend's velocity as their game had it then; t: the hit, in their time) */
    int64_t  rt;                              /* the friend's time the car was drawn at last frame (rc_interp_eval) */
    float    net_v[3];                        /* ...and its velocity there, as their game has it */
    float    push_v[3], push_u[3];            /* their sum (m/s), and how far they have moved the car since (m) */
    float    push_r[3];                       /* a leftover offset, fading (m) */
    uint32_t hit_step;                        /* g_step of the last hit */
    float    com[3];                          /* centre-of-mass offset in the car's frame (forward, left, up) */
    float    spin[4], steer;                  /* the wheels' angles about the axle and the front wheels' steering (rad) */
    float    susp[4];                         /* the wheels' suspension offsets as wheels_pose left them (CAR_SUSP) */
    int64_t  unseen_since;                    /* the peer is not visible: release after a while */
    int64_t  release_after;
    char     carname[RC_CFG_TYPE_NAME + 1];
    char     gin[2][32];                      /* its engine sound's GIN files, pulling and letting off (eng_files; "": none) */
    float    eng_idle, eng_max;               /* ...its car type's idle and most rpm */
    /* statistics */
    uint32_t loads, remode, pose_frames, wakes;   /* wakes: the game's wake-ups refused (roam_game_inert) */
    uint32_t moded, wheelfix;                 /* other physics modes the game set (roam_game_inert); frames the wheels were put back (wheels_pose) */
    uint32_t blinks;                          /* times the game blinked the car out (unblink) */
    uint32_t hits; float hit_max;             /* hits with the player's car; the most speed the game gave the player's car in one (m/s) */
    int64_t  touch_t;                         /* when the car last touched the player's car (host time; block_hit) */
    int      crew;                            /* the friend is in our crew number crew - 1 (O.crew; 0: none) */
} Slot;
static Slot g_sl[NSLOT];
static uint32_t g_world_seen;
static uint32_t g_handles_pending[3 * NSLOT]; static int g_npending;   /* unload once the world is gone */
static int g_created_n;                       /* cars of our own in this world */
/* the outrun challengers' manager: the game's (EM_GAME), held off by us
 * (EM_HELD: state 6, its cars parked), being stood down (EM_STANDDOWN:
 * state 6, its cars still out until they are out of sight), or paused
 * while the HUD's outrun icon is ours (EM_PAUSED: state 6, its cars drive
 * on; em_pause) */
enum { EM_GAME, EM_HELD, EM_STANDDOWN, EM_PAUSED };
static int g_em; static int64_t g_em_release_at, g_em_sd_t0;
static int g_em_pin;                          /* a car of ours in its window: held until the world changes */
static int g_em_racing_noted;
static uint32_t g_em_paused, g_em_paused_st;  /* the manager paused and the state it had */
static uint32_t g_em_pauses;                  /* statistics */
/* frame cost */
static uint64_t g_cost_sum, g_cost_n, g_cost_max, g_cost_win_sum, g_cost_win_n, g_cost_win_max;
static uint64_t g_cost_hud;                   /* roam_game_hud since the last frame, counted with the next one */
static int64_t g_cost_win_t0;
/* statistics */
static int g_joins, g_leaves;                 /* changed with the session lock held */
static uint32_t g_teleports, g_clock_snaps, g_acquires, g_releases, g_load_fail, g_creates, g_reuses, g_restores;
static int64_t g_ready_at;                    /* the last friend's car in (roam_plat_now_us) */
/* test aid: per-frame pose log */
static rh_file *g_poselog;
/* a script command that calls guest code waits for the main-loop hook */
#ifdef ROAM_HOST_SCRIPT
#define NJOB 4
static char g_job[NJOB][240]; static int g_njob;
static void run_job(CPU *c, const char *job);
#endif

/* ---- events from the session (network or game thread, lock held) ------------------------ */
#if MPRACES
static uint32_t g_quit_id;                    /* a player who quit (said so: not one gone out of range), for mp_frame */
#endif
void roam_game_event(int ev, int peer, const char *text){
    if (ev == RC_EV_JOIN) __atomic_fetch_add(&g_joins, 1, __ATOMIC_RELAXED);
    if (ev == RC_EV_LEAVE) __atomic_fetch_add(&g_leaves, 1, __ATOMIC_RELAXED);
#if MPRACES
    /* (the slot is cleared after the event) */
    size_t tl = strlen(text);
    if (ev == RC_EV_LEAVE && peer >= 0 && peer < RC_TABLE && ((tl > 11 && !strcmp(text + tl - 11, " left: quit")) ||
                                                              (tl > 19 && !strcmp(text + tl - 19, " left: switched off"))))
        __atomic_store_n(&g_quit_id, g_s.peer[peer].id, __ATOMIC_RELAXED);
#endif
    rlog("[roam] %s%s (peer %d)\n", ev == RC_EV_JOIN ? "JOIN: " : ev == RC_EV_LEAVE ? "LEAVE: " : "", text, peer);
}

/* ---- the local car -------------------------------------------------------------------------- */
static void rd3(uint32_t a, float *o){ o[0] = MRF32(a); o[1] = MRF32(a + 4u); o[2] = MRF32(a + 8u); }
/* The car has hydraulics: the level its HYDRAULICS part gives it (RideInfo
 * +0x581, UpdatePartsEnabled 0x61BCD0), or G_HYD_LEVEL's when that is not -1,
 * as the driver's hydraulics controls (0x4078A0) read it */
static int has_hydraulics(uint32_t car){
    uint32_t ride = car ? rd32(car + 0x10u) : 0;
    int32_t lv = ok(ride, 0x581) ? (int8_t)MR8(ride + 0x581u) : 0, ov = (int32_t)rd32(G_HYD_LEVEL);
    return (ov > -1 ? ov : lv) != 0;
}
/* The car's nitrous while it is on: what is left of a full shot, in 1/100
 * (0x5925B0: left / full; more than 1 for a car that had some over), 0 when
 * it is off.  CAR_NOS +0x420 is when it went on, +0x41C what was left then,
 * +0x414 a full shot (all in G_TICKS); 0x7FFFFFFF is no time. */
static uint8_t nos_left(uint32_t car){
    uint32_t n = car + CAR_NOS, since = MR32(n + 0x420u), full = MR32(n + 0x414u), left = MR32(n + 0x41cu);
    if (!since || since == 0x7fffffffu || !full || full == 0x7fffffffu || left == 0x7fffffffu) return 0;
    float r = ((float)(int32_t)left - (float)(int32_t)(MR32(G_TICKS) - since)) / (float)(int32_t)full;
    if (!(r > 0.f)) return 0;
    if (r >= 2.55f) return 255;
    int v = (int)(r * 100.f + 0.5f);
    return (uint8_t)(v < 1 ? 1 : v);
}
/* The player's car's engine sound (CARSFX_PlayerEngine, its update 0x474310
 * and 0x474030): an AEMS voice of the car's engine bank and two GIN voices
 * of its recordings, pulling and letting off, at the rpm of its
 * SFXCTL_Engine (+0x128, on the sound's scale: 1000 at idle, 10000 at the
 * most, with the clutch and the limiter in it), each at its volume from the
 * engine's mix (+0xC0: +0x7C the AEMS's, +0x80 the pulling's, +0x84 the
 * letting-off's) times the engine sound's own volume (its control word, at
 * [+0x58], from the game's mix maps), all 0 .. 0x7FFF */
#define VT_SFX_PENGINE  0x0078af30u   /* CARSFX_PlayerEngine (vtable) */
#define VT_SFXCTL_ENG   0x00789f70u   /* SFXCTL_Engine (vtable) */
#define VT_ENG_MIX      0x00789fe4u   /* the engine's mix (vtable) */
static uint32_t player_engine(uint32_t car){
    uint32_t st = ok(car, 0x9e0) ? rd32(car + CAR_SOUND) : 0;
    if (!ok(st, 0x100) || rd32(st + 0x30u) != car) return 0;
    for (uint32_t ch = rd32(st + 0x20u), i = 0; ok(ch, 0x110) && i < 64u; ch = rd32(ch + 0x50u), i++)
        if (rd32(ch) == VT_SFX_PENGINE) return ch;
    return 0;
}
static uint8_t eng_vol(uint32_t v, uint32_t x){                 /* v x x (an int) on 0 .. 255 */
    int32_t xi = (int32_t)x;
    uint32_t p = v * (uint32_t)(xi < 0 ? 0 : xi > 0x7fff ? 0x7fff : xi) >> 15;
    return (uint8_t)((p + 64u) >> 7 > 255u ? 255u : (p + 64u) >> 7);
}
/* for friends' games, which play it with the car's own GIN files (carsfx.c) */
static void eng_state(uint32_t car, rc_state *s){
    uint32_t pe = player_engine(car), ctl = pe ? rd32(pe + 0xb8u) : 0, mix = pe ? rd32(pe + 0xc0u) : 0, vp = pe ? rd32(pe + 0x58u) : 0;
    if (!ok(ctl, 0x130) || rd32(ctl) != VT_SFXCTL_ENG || !ok(mix, 0x88) || rd32(mix) != VT_ENG_MIX || !ok(vp, 4)) return;
    float r = MRF32(ctl + 0x128u);
    if (!__builtin_isfinite(r) || r < 1.f) return;
    uint32_t v = MR32(vp) & 0x7fffu;
    s->eng = (uint16_t)(r > 20000.f ? 20000 : (int)(r + 0.5f));
    s->eng_base = eng_vol(v, MR32(mix + 0x7cu)); s->eng_acc = eng_vol(v, MR32(mix + 0x80u)); s->eng_dcl = eng_vol(v, MR32(mix + 0x84u));
}
static int local_state(rc_state *s){
    uint32_t car = player_car();
    if (!ok(car, 0x9e0)) return 0;
    float f[3], l[3], u[3];
    memset(s, 0, sizeof *s);
    s->t_us = (uint32_t)(uint64_t)(int64_t)g_sync;
    rd3(car + 0x60u, s->p); rd3(car + 0x70u, s->v); rd3(car + 0x110u, s->w);
    rd3(car + 0x90u, f); rd3(car + 0xa0u, l); rd3(car + 0xb0u, u);
    rc_m2q(f, l, u, s->q);
    s->speed = MRF32(car + 0x42cu); s->rpm = MRF32(car + 0x440u); s->gear = (int8_t)MR32(car + 0x434u);
    uint32_t ctl = MR32(car + 0x30u);
    if (ok(ctl, 0x220)){
        s->throttle = MRF32(ctl + 0x20cu); s->brake = MRF32(ctl + 0x214u);
        s->steer = (float)(int16_t)MR16(ctl + 0x1e0u) / 32767.f;
        if (s->brake > 0.1f) s->bits |= RC_BIT_BRAKE;
        if (MRF32(ctl + 0x210u) > 0.1f) s->bits |= RC_BIT_HANDBRAKE;
    }
    if (s->speed < -0.5f) s->bits |= RC_BIT_REVERSE;
    if (g_horn) s->bits |= RC_BIT_HORN;
    /* the extension: the wheel places' heights in the car (the suspension,
     * and the hydraulics, which lift the body off its wheels) and the nitrous */
    s->ext = 1;
    for (uint32_t w = 0; w < 4; w++){
        float z = MRF32(car + 0x130u + 0x40u * w + 0x38u);
        s->wz[w] = __builtin_isfinite(z) ? z : 0.f;
    }
    s->nos = nos_left(car);
    if (s->nos) s->bits |= RC_BIT_NOS;
    /* the hydraulics, for their sound on friends' screens (carsfx.c): each
     * wheel's state (0x5ED470: the physics' own, so only in physics mode 1)
     * and the landings so far (CAR_BUMP is set for a frame as the car lands) */
    uint32_t ph = rd32(car + CAR_PHYS); int landed = 0;
    if (MR32(car + 0x28u) == 1 && ok(ph, CAR_HYD + 0x10u))
        for (uint32_t w = 0; w < 4; w++) s->hyd |= (uint8_t)((MR32(ph + CAR_HYD + 4u * w) & 3u) << (2u * w));
    for (uint32_t w = 0; w < 4; w++) landed |= MR8(car + CAR_BUMP + w) != 0;
    if (landed && !g_landed) g_thumps++;
    g_landed = landed; s->thumps = g_thumps;
    /* each wheel's slip and surface, for friends' tyre smoke, dirt and skid
     * sounds (slip_pose): the physics' own (mode 1), on the ground only */
    if (MR32(car + 0x28u) == 1)
        for (uint32_t w = 0; w < 4; w++){
            if (!MR8(car + CAR_GROUND + w)) continue;
            float a = MRF32(car + CAR_SLIP + 8u * w), b = MRF32(car + CAR_SLIP + 8u * w + 4u);
            uint32_t sf = MR32(car + CAR_SURF + 4u * w);
            s->slip[2 * w] = __builtin_isfinite(a) ? a : 0.f; s->slip[2 * w + 1] = __builtin_isfinite(b) ? b : 0.f;
            s->surf[w] = sf < N_SURF ? (uint8_t)sf : 0;
        }
    /* the engine as the game sounds it, for friends' games (eng_state) */
    eng_state(car, s);
    if (g_sync_paused > 1){ for (int i = 0; i < 3; i++){ s->v[i] = 0; s->w[i] = 0; } s->speed = 0; s->rpm = 0; for (int i = 0; i < 8; i++) s->slip[i] = 0; }
    for (int i = 0; i < 3; i++) if (!__builtin_isfinite(s->p[i]) || !__builtin_isfinite(s->v[i])) return 0;
    /* the stamp clock may run a little faster than the world (it is pulled
     * to the host clock): velocities per stamp second, so the receiver's
     * tangents and dead reckoning match the stamps */
    for (int i = 0; i < 3; i++){ s->v[i] = (float)(s->v[i] * g_vscale); s->w[i] = (float)(s->w[i] * g_vscale); }
    /* the rest comes from the car unchecked: never NaN or infinity on the wire */
    for (int i = 0; i < 3; i++) if (!__builtin_isfinite(s->w[i])) s->w[i] = 0;
    if (!__builtin_isfinite(s->speed)) s->speed = 0;
    if (!__builtin_isfinite(s->rpm)) s->rpm = 0;
    if (!__builtin_isfinite(s->throttle)) s->throttle = 0;
    if (!__builtin_isfinite(s->brake)) s->brake = 0;
    if (!__builtin_isfinite(s->steer)) s->steer = 0;
    if (__builtin_fabsf(s->p[2] - PARK_Z) < 100.f) return 0;
    return 1;
}
/* the car look: type name hash and one (slot, key, owner) per filled part
 * slot of the car's RideInfo */
static int local_cfg(uint8_t *out, int cap, char *tname){
    uint32_t car = player_car(), ride = car ? rd32(car + 0x10u) : 0, types = rd32(G_CARTYPES), owners = rd32(G_PARTOWNERS);
    if (!ok(ride, 0x8f0) || !types || !owners) return 0;
    uint32_t type = MR32(ride);
    if (type >= 256) return 0;                       /* Unlimiter adds car types after the stock ones */
    uint32_t ti = types + type * 0x890u;
    if (!ok(ti, 0x100)) return 0;
    static rc_carcfg c;
    memset(&c, 0, sizeof c);
    c.type_hash = MR32(ti + 0xd0u);
    cstr(ti, c.type_name, sizeof c.type_name);
    if (tname) snprintf(tname, RC_CFG_TYPE_NAME + 1, "%s", c.type_name);
    for (int i = 0; i < 4; i++) c.ride_b[i] = MR8(ride + 4u + (uint32_t)i);
    /* performance, as the game's LAN car description (0x5F0320) has it */
    memset(c.perf, 0, sizeof c.perf);
    for (int i = 0; i < 10; i++) if (MR32(ride + 0x318u + 4u * (uint32_t)i)) c.perf[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    for (int i = 0; i < 68; i++) if (MR8(ride + 0x340u + (uint32_t)i)) c.perf[2 + (i >> 3)] |= (uint8_t)(0x80u >> (i & 7));
    uint8_t kept[41]; int drift = rh_drift_tuning(kept);   /* drift mode's Drift setup: the car's own for friends */
    for (int i = 0; i < 41; i++) c.perf[11 + i] = drift ? kept[i] : MR8(ride + 0x398u + (uint32_t)i);
    c.has_perf = 1;
    int horn = rh_horn_mine();                       /* the horn the others hear from this car */
    if (horn >= 0){ c.has_horn = 1; c.horn = (uint8_t)horn; }
    for (uint32_t sl = 0; sl < RC_PART_SLOTS; sl++){
        uint32_t part = MR32(ride + 0x590u + 4u * sl);
        if (!part || !ok(part, 0xe)) continue;
        uint32_t key = (uint32_t)MR16(part + 2u) << 16 | MR16(part);
        uint32_t own = rd32(owners + 4u * MR8(part + 7u));
        c.part[c.n_parts].slot = (uint8_t)sl; c.part[c.n_parts].key = key; c.part[c.n_parts].owner = own; c.n_parts++;
    }
    return rc_carcfg_encode(&c, out, cap);
}

/* ---- building a remote ride ---------------------------------------------------------------- */
/* the part of car type `type` in ride slot sl with this key and owner, as
 * the CarPartDB has it (NewGetCarPart; parts of other owners may share the
 * key), or 0 */
static uint32_t find_part(CPU *c, uint32_t type, uint32_t sl, uint32_t key, uint32_t owner){
    uint32_t owners = rd32(G_PARTOWNERS), prev = 0;
    for (int tries = 0; tries < 6; tries++){
        uint32_t a[5] = { type, sl, key, prev, 0xffffffffu };
        uint32_t p = rh_call(c, FN_GETCARPART, G_CARPARTDB, 0, 5, a);
        if (!p || !ok(p, 0xe)) return 0;
        if (!owners || rd32(owners + 4u * MR8(p + 7u)) == owner) return p;
        prev = p;
    }
    return 0;
}
/* Decode a car-look record into the RideInfo at ride (0x8F0 bytes of guest
 * memory) with skin slot n: RideInfo::Init, stock parts, then each slot of
 * the record looked up by its key in the CarPartDB (NewGetCarPart, owner
 * checked) and written; slots the record does not name are left empty, as
 * on the sender; a part this game does not have keeps the stock part.
 * Returns the number of parts not found, or -1. */
static int build_ride(CPU *c, const rc_carcfg *cc, uint32_t ride, uint32_t skin, char *why, int whylen){
    uint32_t types = rd32(G_CARTYPES);
    uint32_t ti = gcall1(c, FN_TYPE_BY_HASH, 0, cc->type_hash);
    uint32_t type;
    int missing = 0;
    if (ti && types && ti >= types && (ti - types) % 0x890u == 0) type = (ti - types) / 0x890u;
    else {
        /* an add-on car this game does not have (Unlimiter): a stock car of
         * a type with the same name prefix, or the Peugeot 206 (type 0) */
        type = 0;
        for (uint32_t i = 0; i < 46 && types; i++){
            char n[20]; cstr(types + i * 0x890u, n, sizeof n);
            if (n[0] && !strncasecmp(n, cc->type_name, 4)){ type = i; break; }
        }
        snprintf(why, (size_t)whylen, "car type %s unknown here, shown as type %u", cc->type_name, type);
        missing = -1;
    }
    for (uint32_t i = 0; i < 0x8f0u; i += 4) MR32(ride + i) = 0;
    uint32_t ia[4] = { type, 0, cc->ride_b[1], cc->ride_b[2] };
    rh_call(c, FN_RIDE_INIT, ride, 0, 4, ia);
    /* performance and tuning first, the way the game's own LAN car
     * description decoder (0x5F04D0) builds a remote car: without them the
     * physics block is incomplete and CreateCar's physics pass faults */
    {
        uint8_t pf[RC_PERF_BYTES];
        if (cc->has_perf) memcpy(pf, cc->perf, sizeof pf);
        else { memset(pf, 0, sizeof pf); memset(pf + 11, 0x64, 41); }      /* stock: no parts, default tuning */
        for (int i = 0; i < 10; i++) MR32(ride + 0x318u + 4u * (uint32_t)i) = (pf[i >> 3] >> (7 - (i & 7))) & 1u;
        uint32_t perf = g_gmem + 0x40u;                                    /* scratch: {u32 0, 9 bytes of bits} */
        MR32(perf) = 0; MR8(perf + 4u) = 0;
        for (int i = 0; i < 9; i++) MR8(perf + 5u + (uint32_t)i) = pf[2 + i];
        gcall1(c, FN_PERF_SET, ride + 0x2f0u, perf);
        for (int i = 0; i < 41; i++) MR8(ride + 0x398u + (uint32_t)i) = pf[11 + i];
        /* the physics pass (0x5B4FC0) is not run: on a ride built here it
         * faults (an empty engine table, 0x5764D0); remote cars need no
         * physics, and only SetRideInfo (which does not run it) sees them */
    }
    gcall1(c, FN_RIDE_STOCK, ride, 0);
    if (missing < 0 || cc->n_parts == 0){                /* stock car */
        gcall1(c, FN_RIDE_SKINS, ride, skin);
        return missing < 0 ? 0 : 0;
    }
    uint32_t want[RC_PART_SLOTS] = {0}; uint8_t named[RC_PART_SLOTS] = {0};
    char miss[48] = {0}; int at = 0;
    for (int k = 0; k < cc->n_parts; k++){
        uint32_t sl = cc->part[k].slot, found = find_part(c, type, sl, cc->part[k].key, cc->part[k].owner);
        named[sl] = 1;
        if (found) want[sl] = found;
        else { missing++; if (at < (int)sizeof miss - 8) at += snprintf(miss + at, sizeof miss - (size_t)at, " 0x%x", sl); }
    }
    for (uint32_t sl = 0; sl < RC_PART_SLOTS; sl++){
        if (named[sl] && want[sl]) MR32(ride + 0x590u + 4u * sl) = want[sl];
        else if (!named[sl]) MR32(ride + 0x590u + 4u * sl) = 0;
        /* named but not found here: the stock part stays */
    }
    for (int i = 0; i < 4; i++) MR8(ride + 4u + (uint32_t)i) = cc->ride_b[i];
    gcall0(c, FN_RIDE_PARTSON, ride);
    gcall1(c, FN_RIDE_SKINS, ride, skin);
    if (missing > 0) snprintf(why, (size_t)whylen, "%d part(s) not in this game (slot%s), stock parts instead", missing, miss);
    return missing;
}

/* ---- paint slots past the game's six ---------------------------------------------------------- */
/* A car's paint (the composited skin: base colour, vinyls) goes into the
 * texture its ride names DUMMY_SKIN<n>, its rims into DUMMY_WHEEL<n> and its
 * spinners into DUMMY_SPINNER<n> (0x61C280).  The game's are n = 1..6
 * (GlobalMemoryFile.bin); its compositor (0x61D790) keeps a cache for those
 * six and composites into any other texture GetTextureInfo (0x4901D0)
 * finds.  The game's six are texture packs of their own in CARS/TEXTURES.BIN
 * (pixels, then the TextureInfo, then its platform record), which the car
 * loader reads in while a car uses that slot; slot 1, the player's car, is
 * always there.  Slots 7 and up get copies of a slot that is there: its
 * TextureInfo (0x7C bytes) and platform record (0x20 bytes: +0/+4 the links
 * of the game's list of D3D textures, +0x18 the texture) copied, pixels of
 * their own (zero), the D3D texture made by the game (0x5CE170), handed out
 * by the port's override of GetTextureInfo (roam.c).  Made once, when a car
 * first needs them, and kept. */
#define FN_TEXINFO      0x004901d0u   /* cdecl (hash, default, 0) -> TextureInfo* (one that is loaded) */
#define FN_TEX_CREATE   0x005ce170u   /* thiscall TextureInfo: its D3D texture from +0x70's pixels */
#define TI_SZ           0x7cu         /* TextureInfo: +0 platform record, +0x24 name hash, +0x40 bytes of mip 0, +0x4E mips,
                                       * +0x6C itself (or the texture named by +0x2C), +0x70 pixels */
#define PLAT_SZ         0x20u         /* platform record: +0x18 the D3D texture */
static struct { uint32_t hash, ti; } g_tex[3 * NSLOT]; static int g_ntex;
static uint8_t g_texbits[0x2000u / 8u];       /* hash & 0x1fff of every g_tex: the override's quick no */
static uint32_t bhash(const char *s){ uint32_t h = 0xffffffffu; for (; *s; s++) h = h * 33u + (uint8_t)*s; return h; }  /* bStringHash 0x43DB50 */
#ifndef ROAM_HOST_WIN
static int8_t g_skin_ok[NSLOT + SKIN_GAME];   /* paint slot n: 1 its textures are there, -1 they could not be made */
#endif
uint32_t roam_game_texture(uint32_t h){
    if (!(g_texbits[(h & 0x1fffu) >> 3] & (1u << (h & 7u)))) return 0;
    for (int i = 0; i < g_ntex; i++) if (g_tex[i].hash == h) return g_tex[i].ti;
    return 0;
}
static int skin_textures(CPU *c, uint32_t n){
    if (n <= SKIN_GAME) return 1;
#ifdef ROAM_HOST_WIN
    (void)c; return 0;
#else
    if (n >= sizeof g_skin_ok) return 0;
    if (g_skin_ok[n]) return g_skin_ok[n] > 0;
    static const char *const fmt[3] = { "DUMMY_SKIN%u", "DUMMY_WHEEL%u", "DUMMY_SPINNER%u" };
    int had = 0, made = 0;
    for (int i = 0; i < 3 && g_ntex < 3 * NSLOT; i++){
        char nm[24];
        uint32_t src = 0;
        for (uint32_t t = 1; t <= SKIN_GAME && !src; t++){                                /* a slot that is there now */
            snprintf(nm, sizeof nm, fmt[i], (unsigned)t);
            uint32_t a[3] = { bhash(nm), 0, 0 };
            src = rh_call(c, FN_TEXINFO, 0, 0, 3, a);
            if (!ok(src, TI_SZ) || !ok(rd32(src), PLAT_SZ)) src = 0;
        }
        if (!src) continue;                                                               /* no spinners, say */
        had++;
        /* the pixels: mip 0's bytes, each further mip a quarter of the one before */
        uint32_t px = MR32(src + 0x40u), mips = MR8(src + 0x4eu), total = 0;
        for (uint32_t m = 0, b = px; m < (mips ? mips : 1u) && b; m++, b >>= 2) total += b > 16u ? b : 16u;
        if (!px || px > (16u << 20)){ rlog("[roam] f=%ld %s: %u bytes of pixels?\n", rh_frame(), nm, px); break; }
        uint32_t sz = TI_SZ + PLAT_SZ + total + 64u, ti = rh_call(c, FN_MALLOC, 0, 0, 1, &sz), pl = ti + TI_SZ;
        if (!ti) break;
        uint32_t pix = (pl + PLAT_SZ + 63u) & ~63u;
        memcpy(GPTR(ti), GPTR(src), TI_SZ); memcpy(GPTR(pl), GPTR(rd32(src)), PLAT_SZ);
        memset(GPTR(pix), 0, total);
        MR32(pl) = 0; MR32(pl + 4u) = 0; MR32(pl + 0x18u) = 0;
        snprintf(nm, sizeof nm, fmt[i], (unsigned)n);
        MR32(ti) = pl; MR32(ti + 0x24u) = bhash(nm); MR32(ti + 0x70u) = pix;
        for (uint32_t q = 0; q < 0x18u; q++) MR8(ti + 8u + q) = q < 0x17u && q < strlen(nm) ? (uint8_t)nm[q] : 0;   /* its name */
        if (MR32(ti + 0x6cu) == src) MR32(ti + 0x6cu) = ti;
        gcall0(c, FN_TEX_CREATE, ti);
        if (!rd32(pl + 0x18u)){ rlog("[roam] f=%ld %s: the game made no texture\n", rh_frame(), nm); break; }
        uint32_t h = MR32(ti + 0x24u);
        g_tex[g_ntex].hash = h; g_tex[g_ntex].ti = ti; g_ntex++;
        g_texbits[(h & 0x1fffu) >> 3] |= (uint8_t)(1u << (h & 7u));
        made++;
    }
    g_skin_ok[n] = had && made == had ? 1 : -1;
    if (g_skin_ok[n] > 0) rlog("[roam] f=%ld paint slot %u: %d textures of our own (the paint %ux%u, format 0x%02x, %u mips)\n", rh_frame(), n,
                               made, (unsigned)MR16(g_tex[g_ntex - made].ti + 0x44u), (unsigned)MR16(g_tex[g_ntex - made].ti + 0x46u),
                               (unsigned)MR8(g_tex[g_ntex - made].ti + 0x4au), (unsigned)MR8(g_tex[g_ntex - made].ti + 0x4eu));
    else rlog("[roam] f=%ld paint slot %u: its textures could not be made (%d of %d): no car in that slot\n", rh_frame(), n, made, had);
    return g_skin_ok[n] > 0;
#endif
}

/* ---- slots ------------------------------------------------------------------------------------ */
static uint32_t skin_of(uint32_t rec){ return rec >= G_RECORDS && rec < G_RECORDS + 22u * REC_SZ ? (rec - G_RECORDS) / REC_SZ + 1u : 0; }
/* slot k's paint slot: an opponent car's own (its record's place: 2-4), ours
 * 5 and up */
static uint32_t slot_skin(int k){
    if (k >= NOPP) return 5u + (uint32_t)(k - NOPP);
    uint32_t n = skin_of(g_sl[k].car ? rd32(g_sl[k].car + 0x14u) : 0);
    return n ? n : 2u + (uint32_t)k;
}
/* g_gmem: scratch for guest-call arguments (+0x40 a performance block, +0x80
 * a grid entry, +0xA0 a file name, +0x100 the name labels' vectors and
 * strings), then the slots' rides and records */
#define GM_SCRATCH 0x200u
static void gmem_init(CPU *c){
    if (g_gmem) return;
    uint32_t sz = NSLOT * (2u * RIDE_SZ + REC_SZ) + 0x100u + GM_SCRATCH;
    g_gmem = rh_call(c, FN_MALLOC, 0, 0, 1, &sz);
    if (!g_gmem){ rlog("[roam] guest malloc failed\n"); return; }
    for (uint32_t i = 0; i < sz; i += 4) MR32(g_gmem + i) = 0;
    for (int k = 0; k < NSLOT; k++){
        uint32_t b = ((g_gmem + 0xffu) & ~0xffu) + GM_SCRATCH;
        b += (uint32_t)k * (2u * RIDE_SZ + REC_SZ);
        g_sl[k].ride_new = b; g_sl[k].ride_orig = b + RIDE_SZ; g_sl[k].rec = b + 2u * RIDE_SZ;
    }
}
static void unload_later(uint32_t h){
    if (!h) return;
    if (g_npending < 3 * NSLOT) g_handles_pending[g_npending++] = h;
    else rlog("[roam] too many rides waiting to be unloaded, one kept\n");
}
static void set_name(Slot *s){
    uint32_t rec = s->car ? rd32(s->car + 0x14u) : 0;
    if (!ok(rec, 0x920)) return;
    for (int i = 0; i < 16; i++) MR8(rec + 0x90cu + (uint32_t)i) = i < 15 ? (uint8_t)s->name[i] : 0;
    MR8(rec + 0x90cu + 15u) = 0;
}
static void make_inert(CPU *c, uint32_t car){
    if (rd32(car + 0x24u) != 0) gcall1(c, FN_CAR_SETDRIVER, car, 0);
    if (rd32(car + 0x28u) != 0) gcall1(c, FN_CAR_SETPHYS, car, 0);
}
/* the car's collision body on or off: a disable count at +0x17 (0x5930F0
 * adds one, 0 is on) */
static void body_set(CPU *c, uint32_t car, int solid){
    uint32_t body = rd32(car + 0x3cu);
    if (!ok(body, 0x20)) return;
    if (!solid && MR8(body + 0x17u) == 0) gcall0(c, FN_BODY_DISABLE, body);
    else if (solid && MR8(body + 0x17u) != 0) MR8(body + 0x17u) = 0;
}
/* a shown friend's car: solid unless it is a ghost or passable, and a ghost
 * until it is clear of the player's car.  Its body only while it is near the
 * player's car: the game's collision step (0x5FFA50 in World::DoTimestep)
 * tests every body against the others near it (0x5B5050, the box test
 * 0x595040), friends' cars against each other too, which two cars posed by
 * us never need; 32 of them crossing each other's paths took 3-4.5 ms a
 * frame (a crowd round the new career's start, roam-crowd --spread 10) */
static void body_update(CPU *c, int k){
    Slot *s = &g_sl[k];
    uint32_t me = player_car();
    int near = 0;
    if (O.collide && ok(me, 0x70)){
        float dx = MRF32(s->car + 0x60u) - MRF32(me + 0x60u), dy = MRF32(s->car + 0x64u) - MRF32(me + 0x64u),
              dz = MRF32(s->car + 0x68u) - MRF32(me + 0x68u), d2 = dx * dx + dy * dy + dz * dz;
        if (s->ghost && d2 > CLEAR_M * CLEAR_M){
            s->ghost = 0;
            if (!s->soft) rlog("[roam] f=%ld slot %d: %s's car is solid now\n", rh_frame(), k, s->name);
        }
        near = d2 < SOLID_M * SOLID_M;
    }
    body_set(c, s->car, O.collide && !s->ghost && !s->soft && near);
}
static void ghost_all(void){ for (int k = 0; k < NSLOT; k++) g_sl[k].ghost = 1; }
/* Passable cars.  A friend's car has no physics of its own here: it is
 * where the friend is, and the player's car cannot push it away.  A crowd
 * waiting at a shop's or the garage's door walled it off, and a paused
 * friend's car stood in the way for as long as its tab was in the
 * background.  So a friend's car has no collision body (as a ghost) while
 * the friend or the player is paused, or has stood for a minute, or either
 * car is near a door: a shop's, the garage's or the car lot's trigger (type
 * 14; every career has them all).  Each of those rules holds for the pair,
 * so both screens agree on whether two cars touch.  A race's start is an
 * event's trigger (type 13), which leads somewhere or not by each player's
 * career: there a player near one of their own open events lets every car
 * through theirs (events_read), and says so to the others in the snapshots
 * (RC_BIT_PASS), so the pair agrees again; the same bit makes the car of a
 * player whose friends' cars are ghosts (NFSU2_ROAM_COLLIDE=0) passable for
 * everyone.  Passable no longer, the car is a ghost until it is clear of the
 * player's car, as when it appears.  A player at the new career's start is
 * not shown to others (at_spawn), so there friends' cars are passable for
 * the player too.  A car parked across a road (an SUV at the airport's way
 * out, which the others could not shove away) was passable after a minute
 * only if its player never nudged it, so a player's car that stays near one
 * spot after hits by others (there or on its way there: hits shove it) lets
 * every car through it as well until it leaves that spot (stay_update,
 * block_hit; only its own game sees all its hits, so it says so with
 * RC_BIT_PASS).  A car of a traffic type (a bus or a fire truck across the
 * airport's way out: a profile a save editor changed makes one the player's)
 * is passable whoever sees it: its own game says so, and every other game
 * sees its type, so a game that does not say so cannot keep it solid. */
enum { SOFT_PAUSED = 1, SOFT_IDLE = 2, SOFT_DOOR = 4, SOFT_SPAWN = 8, SOFT_EVENT = 16, SOFT_SAYS = 32, SOFT_BLOCK = 64,
       SOFT_TRAFFIC = 128 };
/* an open event: its trigger's box, and what it leads to (FN_MARKER_WHAT's
 * out[kind]: 1 a race, 2 a shop, 3 a showcase (a cover shoot), 4 another
 * (the table at 0x838424: STAGE_2_SHOW_1, STAGE_3_PERF_1, ...)) */
typedef struct { float box[4]; uint32_t what; int kind; } OpenEvent;
static struct {
    int      n;
    float    box[MAX_DOORS][4];               /* min x, min y, max x, max y (the trigger's +0x20) */
    uint32_t first, end;                      /* the list was read from these */
    int64_t  paused_since, idle_since;        /* the player is paused / the player's car stands, since (0: not) */
    int      me;                              /* the player's own reasons (SOFT_*) */
    /* the spot the player's car stays near (BLOCK_M), and its last hits */
    float    stay[3];
    int64_t  stay_since;                      /* (0: none: a new one from the next frame in free roam) */
    int      blocking;                        /* in the way there (SOFT_BLOCK) */
    int      traffic;                         /* the player's car is of a traffic type (SOFT_TRAFFIC) */
    struct { uint32_t id; int64_t t; } hit[BLOCK_Q];   /* the friend (peer id) and when; a ring */
    int      hit_at, hit_n;
    /* the player's open events (events_read), and how many of the career's
     * event triggers there are */
    int      nev, evall;
    OpenEvent ev[MAX_EVENTS];
    uint32_t ev_first, ev_end, ev_at;         /* the list asked from; the next trigger to ask (0: no sweep now) */
    int      ev_nb, ev_seen;                  /* the sweep so far: open, asked */
    OpenEvent ev_new[MAX_EVENTS];
    int64_t  ev_next;                         /* the next sweep */
    uint64_t ev_ns, ev_ns_max; int ev_frames; /* this sweep's cost: in all, the dearest frame's, its frames */
    uint64_t ev_cost[3];                      /* ...the last whole sweep's (mp events) */
} g_pass;
/* the doors' boxes: the track's triggers stay while its world does; read
 * again when their place changes */
static void doors_read(void){
    uint32_t e = rd32(G_TRIGGERS + 0x440u + 4u * TRIG_DOOR), end = rd32(G_TRIGGERS + 0x444u + 4u * TRIG_DOOR);
    if (e == g_pass.first && end == g_pass.end) return;
    g_pass.first = e; g_pass.end = end; g_pass.n = 0;
    while (e < end && g_pass.n < MAX_DOORS && ok(e, 0x44)){
        int sz = (int16_t)MR16(e + 0x42u);
        float *b = g_pass.box[g_pass.n];
        for (int i = 0; i < 4; i++) b[i] = MRF32(e + 0x20u + 4u * (uint32_t)i);
        if (b[0] <= b[2] && b[1] <= b[3]) g_pass.n++;
        if (sz <= 0) break;
        e += (uint32_t)sz;
    }
    if (g_pass.n) rlog("[roam] f=%ld doors: %d shops' and garages' triggers, friends' cars near them are passable\n", rh_frame(), g_pass.n);
}
static int near_box(const float *b, float x, float y){
    return x >= b[0] - DOOR_M && x <= b[2] + DOOR_M && y >= b[1] - DOOR_M && y <= b[3] + DOOR_M;
}
static int near_door(float x, float y){ for (int i = 0; i < g_pass.n; i++) if (near_box(g_pass.box[i], x, y)) return 1; return 0; }
static int near_event(float x, float y){ for (int i = 0; i < g_pass.nev; i++) if (near_box(g_pass.ev[i].box, x, y)) return 1; return 0; }
#if MPRACES
/* in one of the player's open events' triggers: Engage there enters the game's */
static int in_event(float x, float y){
    for (int i = 0; i < g_pass.nev; i++){
        const float *b = g_pass.ev[i].box;
        if (x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]) return 1;
    }
    return 0;
}
#endif
/* The player's open events: the event triggers Engage would enter now, by
 * the game's own test (0x5EB040 finds the trigger the car is in, 0x527800
 * what it leads to, 0x5EAFB0 enters it if 0x532A50 says so: a race open in
 * the career, a cover shoot not done yet, one of the stage's other events;
 * in the prologue none).  Both are lookups in the career's tables: a few
 * triggers a frame (all 196 took 0.4 ms in 25 frames, at most 30 us a
 * frame), all of them again every EV_SWEEP_US, and from the start for a new
 * track.  Saves of stages 2 and 3 had 11 to 22 open. */
static void events_read(CPU *c, int64_t host){
    uint32_t first = rd32(G_TRIGGERS + 0x440u + 4u * TRIG_EVENT), end = rd32(G_TRIGGERS + 0x444u + 4u * TRIG_EVENT);
    if (first != g_pass.ev_first || end != g_pass.ev_end){
        g_pass.ev_first = first; g_pass.ev_end = end; g_pass.nev = g_pass.evall = 0; g_pass.ev_at = 0; g_pass.ev_next = 0;
        g_pass.ev_ns = g_pass.ev_ns_max = 0; g_pass.ev_frames = 0;
    }
    if (!g_gmem) return;
    if (!g_pass.ev_at){
        if (host < g_pass.ev_next || !first || first >= end) return;
        g_pass.ev_at = first; g_pass.ev_nb = g_pass.ev_seen = 0;
    }
    uint32_t out = g_gmem + EV_SCR;
    uint64_t t0 = rh_ns();
    for (int i = 0; i < EV_PER_FRAME && g_pass.ev_at < end && ok(g_pass.ev_at, 0x44); i++){
        uint32_t e = g_pass.ev_at, a[2] = { out, e };
        int sz = (int16_t)MR16(e + 0x42u);
        rh_call(c, FN_MARKER_WHAT, G_CAREER, 0, 2, a);
        if (MR32(out) == e && (rh_call(c, FN_MARKER_OPEN, G_CAREER, 0, 1, &out) & 0xffu) && g_pass.ev_nb < MAX_EVENTS){
            OpenEvent *v = &g_pass.ev_new[g_pass.ev_nb];
            for (int j = 0; j < 4; j++) v->box[j] = MRF32(e + 0x20u + 4u * (uint32_t)j);
            v->kind = 0; v->what = 0;
            for (int k = 1; k <= 4 && !v->kind; k++) if (MR32(out + 4u * (uint32_t)k)){ v->kind = k; v->what = MR32(out + 4u * (uint32_t)k); }
            if (v->box[0] <= v->box[2] && v->box[1] <= v->box[3]) g_pass.ev_nb++;
        }
        g_pass.ev_seen++;
        g_pass.ev_at = sz > 0 ? e + (uint32_t)sz : end;
    }
    uint64_t dt = rh_ns() - t0;
    g_pass.ev_ns += dt; g_pass.ev_frames++;
    if (dt > g_pass.ev_ns_max) g_pass.ev_ns_max = dt;
    if (g_pass.ev_at < end && ok(g_pass.ev_at, 0x44)) return;
    g_pass.ev_cost[0] = g_pass.ev_ns; g_pass.ev_cost[1] = g_pass.ev_ns_max; g_pass.ev_cost[2] = (uint64_t)g_pass.ev_frames;
    g_pass.ev_ns = g_pass.ev_ns_max = 0; g_pass.ev_frames = 0;
    if (g_pass.ev_nb != g_pass.nev || g_pass.ev_seen != g_pass.evall)
        rlog("[roam] f=%ld events: %d of the career's %d events' triggers open; with the player near one, every car is passable\n", rh_frame(),
                g_pass.ev_nb, g_pass.ev_seen);
    memcpy(g_pass.ev, g_pass.ev_new, sizeof g_pass.ev);
    g_pass.nev = g_pass.ev_nb; g_pass.evall = g_pass.ev_seen;
    g_pass.ev_at = 0; g_pass.ev_next = host + EV_SWEEP_US;
}
/* The new career's start: every new player's car appears there (all on one
 * spot, cars inside cars), stands through the prologue's first seconds, and
 * an idle tab's stays.  Players arrive and wait there all the time, so a
 * friend's car within NFSU2_ROAM_SPAWN_M (5 m; 0: none) of it is not shown,
 * as a friend's in a shop */
static int at_spawn(const float *p){
    float dx = p[0] - SPAWN_X, dy = p[1] - SPAWN_Y, dz = p[2] - SPAWN_Z, r = (float)O.spawn_m;
    return dx * dx + dy * dy + dz * dz < r * r;
}
/* the spot the player's car stays near: a new one once the car is farther
 * than BLOCK_M from it */
static void stay_update(int64_t host, const float *at){
    float dx = at[0] - g_pass.stay[0], dy = at[1] - g_pass.stay[1], dz = at[2] - g_pass.stay[2];
    if (g_pass.stay_since && dx * dx + dy * dy + dz * dz <= BLOCK_M * BLOCK_M) return;
    if (g_pass.blocking) rlog("[roam] f=%ld my car has left the spot it stood on: friends' cars touch it again\n", rh_frame());
    memcpy(g_pass.stay, at, sizeof g_pass.stay);
    g_pass.stay_since = host; g_pass.blocking = 0;
}
/* a car of a traffic type (TYPE_TRAFFIC): its type's name in name, or 0 */
static int traffic_car(uint32_t car, char *name, int len){
    uint32_t types = rd32(G_CARTYPES), ti = ok(car, 0x10) ? rd32(car + 0xcu) : 0;
    if (!types || ti < types || (ti - types) % 0x890u) return 0;
    uint32_t t = (ti - types) / 0x890u;
    if (t < TYPE_TRAFFIC || t >= TYPE_STOCK) return 0;
    if (name) cstr(ti, name, len);
    return 1;
}
/* the player's car's hits with friends' cars in the last BLOCK_WIN_US, and
 * from how many of them (*cars) */
static int block_hits(int64_t host, int *cars){
    uint32_t ids[BLOCK_Q];
    int n = 0, nc = 0;
    for (int i = 0; i < g_pass.hit_n; i++){
        if (host - g_pass.hit[i].t > BLOCK_WIN_US) continue;
        n++;
        int j = 0;
        while (j < nc && ids[j] != g_pass.hit[i].id) j++;
        if (j == nc) ids[nc++] = g_pass.hit[i].id;
    }
    *cars = nc;
    return n;
}
/* the player's own reasons, once a frame: paused (a second, as a friend's
 * pause counts), standing, near a door or one of their open events, at the
 * new career's start, standing in others' way, a traffic car; any of them,
 * or ghost cars, goes out in the snapshots (local_state: RC_BIT_PASS) */
static void soft_me(CPU *c, int64_t host, uint8_t status){
    doors_read();
    events_read(c, host);
    if (status != RC_ST_PAUSED) g_pass.paused_since = 0;
    else if (!g_pass.paused_since) g_pass.paused_since = host;
    float v = rc_sqrtf(g_lv[0] * g_lv[0] + g_lv[1] * g_lv[1] + g_lv[2] * g_lv[2]);
    if (v >= IDLE_MPS && status != RC_ST_PAUSED) g_pass.idle_since = 0;
    else if (!g_pass.idle_since) g_pass.idle_since = host;
    uint32_t me = player_car();
    float at[3] = { 0.f, 0.f, 0.f };
    if (ok(me, 0x70)){ rd3(me + 0x60u, at); stay_update(host, at); }
    else g_pass.stay_since = g_pass.blocking = 0;
    char tn[20];
    int traffic = ok(me, 0x70) && traffic_car(me, tn, sizeof tn);
    if (traffic != g_pass.traffic && ok(me, 0x70)){
        if (traffic) rlog("[roam] f=%ld my car is a traffic car (%s): every car drives through it\n", rh_frame(), tn);
        else rlog("[roam] f=%ld my car is no traffic car any more: friends' cars touch it again\n", rh_frame());
        g_pass.traffic = traffic;
    }
    if (!g_pass.blocking && g_pass.stay_since && host - g_pass.stay_since >= BLOCK_US){
        int cars = 0, n = block_hits(host, &cars);
        if (n >= BLOCK_HITS && cars >= BLOCK_CARS){
            g_pass.blocking = 1;
            rlog("[roam] f=%ld my car has stayed within %.0f m for %.0f s after %d hits from %d players' cars in %.0f s: every car drives through it\n",
                 rh_frame(), (double)BLOCK_M, (double)(host - g_pass.stay_since) / 1e6, n, cars, (double)BLOCK_WIN_US / 1e6);
        }
    }
    g_pass.me = (g_pass.paused_since && host - g_pass.paused_since >= 1000000 ? SOFT_PAUSED : 0) |
                 (g_pass.idle_since && host - g_pass.idle_since >= IDLE_US ? SOFT_IDLE : 0) |
                 (ok(me, 0x70) && near_door(at[0], at[1]) ? SOFT_DOOR : 0) |
                 (ok(me, 0x70) && near_event(at[0], at[1]) ? SOFT_EVENT : 0) |
                 (ok(me, 0x70) && at_spawn(at) ? SOFT_SPAWN : 0) |
                 (g_pass.blocking ? SOFT_BLOCK : 0) | (g_pass.traffic ? SOFT_TRAFFIC : 0);
}
/* a shown friend's car's reasons, the player's added; leaving them makes it
 * a ghost.  The friend's own (RC_BIT_PASS): near one of their open events,
 * their cars ghosts, or what we see too; and a traffic car, said or not */
static void soft_update(Slot *s, const rc_pose *pose, int64_t host){
    if (!s->paused && __builtin_fabsf(pose->speed) >= IDLE_MPS) s->idle_since = 0;
    else if (!s->idle_since) s->idle_since = host;
    int soft = g_pass.me | (s->paused ? SOFT_PAUSED : 0) | (s->idle_since && host - s->idle_since >= IDLE_US ? SOFT_IDLE : 0) |
               (near_door(pose->p[0], pose->p[1]) ? SOFT_DOOR : 0) | (pose->bits & RC_BIT_PASS ? SOFT_SAYS : 0) |
               (traffic_car(s->car, 0, 0) ? SOFT_TRAFFIC : 0);
    if (s->soft && !soft) s->ghost = 1;
    s->soft = soft;
}
/* A hidden car is out of the world: Car +0x550 clear, as Car::Deactivate
 * leaves it (its other state stays as it is; shown again, it is set).  The
 * minimap draws an AI racer's arrow (0x4CA6D0) and the world map makes its
 * marker (0x4E0440) only for a car with it set: a hidden one had its arrow at
 * the parking spot's (0, 0), or on the minimap's edge pointing there, for a
 * friend in a race, a shop or the menus, gone, or still loading. */
#define CAR_ACTIVE      0x550u
static void soft_park(CPU *c, uint32_t car){
    MR8(car + CAR_ACTIVE) = 0;
    MRF32(car + 0x60u) = 0; MRF32(car + 0x64u) = 0; MRF32(car + 0x68u) = PARK_Z;
    MRF32(car + 0x50u) = 0; MRF32(car + 0x54u) = 0; MRF32(car + 0x58u) = PARK_Z;
    for (uint32_t o = 0x70u; o < 0x7cu; o += 4) MRF32(car + o) = 0;
    for (uint32_t o = 0x110u; o < 0x11cu; o += 4) MRF32(car + o) = 0;
    MRF32(car + 0x42cu) = 0; MRF32(car + 0x430u) = 0;
    MR32(car + CAR_NOS + 0x420u) = 0;                                    /* no nitrous (nos_pose) */
    for (uint32_t w = 0; w < 4; w++) MRF32(car + CAR_SUSP + 4u * w) = 0.f;   /* the wheels at rest (wheels_pose) */
    for (uint32_t i = 0; i < 8; i++) MRF32(car + CAR_SLIP + 4u * i) = 0.f;   /* no slip: no smoke (slip_pose) */
    gcall0(c, FN_CAR_SECTION, car);
}
/* A race's end blinks its AI racers out: FinishRacer (0x606CB0) with its
 * blink, which the race's update (0x6076D0) asks for when no Player owns the
 * racer's car, sets car +0x998, the blink's start (game ticks) at +0x974 and
 * its length at +0x978.  The renderer (0x6316D0 -> 0x60D4F0) and the
 * minimap's arrow (0x4CA6D0) skip a car with +0x998 set, every other frame
 * while the blink lasts and for good after it; only Car::Activate clears it.
 * In an outrun a friend's car is the AI racer: at its end, in the winner's
 * game and the loser's alike, the friend's car went for good and only their
 * name stayed where it was (reported in play; roam_mp_outrun_view_a/b).  A
 * friend's car is shown or hidden by us alone, so the blink goes at once, as
 * the game ends one (+0x974 = 0, 0x5FBDE2). */
#define CAR_BLINKOUT    0x998u
#define CAR_BLINK_T0    0x974u
static void unblink(Slot *s){
    uint32_t car = s->car;
    if (!MR8(car + CAR_BLINKOUT)) return;
    if (s->blinks++ < 3) rlog("[roam] f=%ld slot %d: %s's car was blinked out by the game (a race's end): shown again\n",
                              rh_frame(), (int)(s - g_sl), s->name);
    MR8(car + CAR_BLINKOUT) = 0; MR32(car + CAR_BLINK_T0) = 0;
}
/* The RandomEncounterManager (race coordinator +0xC40; Update 0x42EDF0):
 * state 0 resets it (0x412F90: its three rides unloaded, World cars[first ..
 * first+2] taken as its cars), 1 asks the career whether an outrun is due,
 * 3 cruises its cars near the player and tries to engage, 4 races (the
 * outrun's start 0x607180 sets it, 0x40BE80, an outrun between players'
 * too), 5 parks its cars -- 0x40BF60, and 0x40BFB0 for every car of the AI
 * range that is out, not engaged and 250 m away -- tells the career
 * (0x500CA0) and goes to 0; at 6 and above Update does nothing.  It drove a
 * friend's car it had taken as its own into a fault (0x41C100 on a null
 * brain), so it never sees one: in the port no friend's car is in the car
 * index (LEND_OPP), in the Windows plugin ours are kept out of its window
 * (keep_out_of_window).  It is held off (why: the reason) while the
 * Windows plugin's friends have (or are about to get) a car in this world,
 * and in the port around an outrun between players (its state 4 would race
 * the friend's car) and in or by a race lobby (Engage is the lobby's):
 *  - from 0, 1 or 2 it is held in 6;
 *  - from 3 or 5 it is stood down the way its own state 5 does it, but for
 *    its three cars only: each is parked once an outrun's end blinked it
 *    out (unblink) or 250 m from the player, or after 20 s anyway; then the
 *    career is told and it goes to 0, which re-reads its cars, and then to
 *    1, where it is held;
 *  - during an outrun of its own (4) it is left alone: its 5 afterwards is
 *    taken over as above;
 *  - paused (em_pause), it is held off or stood down from the state it was
 *    paused in;
 *  - it is let go (6 -> 0) when the reason is gone (em_let_go): in the
 *    Windows plugin a minute after the last friend left. */
static void em_update(CPU *c, const char *why, int64_t host){
    uint32_t em = encounter_mgr();
    if (!em) return;
    uint32_t st = MR32(em);
    if (g_em == EM_PAUSED){
        if (em != g_em_paused) g_em = EM_GAME;      /* another race coordinator's, a new one */
        else if (!why){ if (st != 6u) MR32(em) = 6u; return; }
        else { st = g_em_paused_st; MR32(em) = st; g_em = EM_GAME; }
    }
    if (g_em == EM_HELD){ if (st != 6u) MR32(em) = 6u; return; }
    if (g_em == EM_STANDDOWN){
        MR32(em) = 6u;
        uint32_t me = player_car();
        int out = 0, parked = 0;
        for (uint32_t k = 0; k < 3; k++){
            uint32_t car = rd32(em + EM_TABLE + 0xcu + 0x10u * k);
            if (!ok(car, 0x9e0) || !car_out(car)) continue;
            float dx = me ? MRF32(car + 0x60u) - MRF32(me + 0x60u) : 1e4f, dy = me ? MRF32(car + 0x64u) - MRF32(me + 0x64u) : 0;
            if (MR8(car + CAR_BLINKOUT) || dx * dx + dy * dy > 250.f * 250.f || host - g_em_sd_t0 > EM_STANDDOWN_US){
                gcall0(c, FN_CAR_DEACTIVATE, car); MR8(em + EM_TABLE + 8u + 0x10u * k) = 0; parked++;
            } else out++;
        }
        if (parked) rlog("[roam] f=%ld outrun challengers: %d car(s) parked, %d still near you\n", rh_frame(), parked, out);
        if (!out){
            gcall0(c, FN_EM_OVER, G_ENCOUNTERS);
            MR32(em) = 0;                       /* its next Update resets it (state 0), then it is held at 1 */
            g_em = EM_GAME;
            rlog("[roam] f=%ld outrun challengers stood down after %.1f s\n", rh_frame(), (host - g_em_sd_t0) / 1e6);
        }
        return;
    }
    if (!why) return;
    if (st == 4u){                              /* an outrun is being raced: not interrupted */
        if (!g_em_racing_noted){ g_em_racing_noted = 1;
            rlog("[roam] f=%ld an outrun is on: the outrun challengers are stood down when it is over (%s)\n", rh_frame(), why); }
        return;
    }
    g_em_racing_noted = 0;
    if (st == 3u || st == 5u){
        MR32(em) = 6u; g_em = EM_STANDDOWN; g_em_sd_t0 = host;
        rlog("[roam] f=%ld outrun challengers off (%s): standing down (state %u)\n", rh_frame(), why, st);
        return;
    }
    MR32(em) = 6u; g_em = EM_HELD;              /* 0, 1, 2: its cars are parked */
    rlog("[roam] f=%ld outrun challengers off (%s, state %u)\n", rh_frame(), why, st);
}
/* the game's own outrun against a challenger, the manager not held: in the
 * port that is still free roam (roaming): friends' cars stay, as in an
 * outrun between players, and the outrun's racers are the game's cars only */
static int em_racing(uint32_t race){
    uint32_t em = race + RC_ENCOUNTERS;
    return !LEND_OPP && g_em == EM_GAME && MR32(G_RACETYPE) == 7u && MR32(race + 0x24u) >= 2u && MR32(em) == 4u && !du_racing(race);
}
/* a car of ours in the manager's window, World cars[first .. first+2] */
static int ours_in_window(uint32_t w){
    for (int i = 0; i < 3; i++){ uint32_t car = ai_car(w, i); if (car && !game_opponent(car)) return 1; }
    return 0;
}
static void em_let_go(void){
    uint32_t em = encounter_mgr(), w = world();
    if (g_em != EM_HELD || !em) return;
    if (g_em_pin || (w && ours_in_window(w))){
        static long said; if (rh_frame() - said > 7200){ said = rh_frame();
            rlog("[roam] f=%ld outrun challengers stay off: a car of ours is in their window\n", rh_frame()); }
        return;
    }
    if (MR32(em) == 6u){ MR32(em) = 0; rlog("[roam] f=%ld outrun challengers back on\n", rh_frame()); }
    g_em = EM_GAME;
}
/* a game opponent car that no slot uses (slots 0 to NOPP - 1) */
static uint32_t free_opponent_car(uint32_t w){
    int n = (int)MR32(w + 0x18u) - (int)MR32(w + 0x14u);
    for (int i = 0; i < n && i < 22; i++){
        uint32_t car = ai_car(w, i);
        if (!car || !game_opponent(car) || car_out(car)) continue;
        int used = 0;
        for (int k = 0; k < NSLOT; k++) if (g_sl[k].car == car) used = 1;
        if (!used) return car;
    }
    return 0;
}
#if LEND_OPP
static int opp_slots_used(void){ int n = 0; for (int k = 0; k < NOPP; k++) if (g_sl[k].st != SL_FREE) n++; return n; }
#endif

/* the manager's state 0 takes World cars[first .. first+2] as its three
 * cars; IndexCars puts the AI racers in the order of the World's list, and
 * the game changes that order (one of ours had become
 * cars[first] after a reversal of the list).  Our cars go to the end of the
 * list, as CreateCar appends, and the cars are indexed again: then the three
 * game opponents come first.  Only IndexCars writes cars[], and the game
 * calls it only when it builds a world. */
static void keep_out_of_window(CPU *c, uint32_t w){
    if (!ours_in_window(w)) return;
    for (int k = NOPP; k < NSLOT; k++){
        uint32_t car = g_sl[k].car;
        if (!g_sl[k].created || g_sl[k].unindexed || g_sl[k].world != w || !ok(car, 0x10)) continue;
        uint32_t nx = rd32(car), pv = rd32(car + 4u);
        if (!ok(nx, 8) || !ok(pv, 8) || rd32(pv) != car || rd32(nx + 4u) != car) continue;   /* not linked as expected */
        MR32(pv) = nx; MR32(nx + 4u) = pv;                           /* unlink */
        uint32_t tail = MR32(w + 4u);
        MR32(tail) = car; MR32(w + 4u) = car; MR32(car) = w; MR32(car + 4u) = tail;   /* append */
    }
    gcall0(c, FN_INDEXCARS, w);
    int still = ours_in_window(w);
    if (still) g_em_pin = 1;
    rlog("[roam] f=%ld car(s) of our own moved after the game's opponent cars in the AI range [%u, %u)%s\n", rh_frame(),
            MR32(w + 0x14u), MR32(w + 0x18u), still ? ": STILL in the outrun manager's window, it stays off in this world" : "");
}
/* forget every slot of a world that is gone (its cars were freed with it) */
static void world_gone(void){
    for (int k = 0; k < NSLOT; k++){
        Slot *s = &g_sl[k];
        if (s->h_cur) unload_later(s->h_cur);
        if (s->h_new) unload_later(s->h_new);
        if (s->h_orig) unload_later(s->h_orig);
        uint32_t rn = s->ride_new, ro = s->ride_orig, rec = s->rec;
        memset(s, 0, sizeof *s);
        s->ride_new = rn; s->ride_orig = ro; s->rec = rec;
    }
    g_created_n = 0; g_em = EM_GAME; g_em_pin = 0; g_em_racing_noted = 0; g_em_release_at = 0;
}
static void flush_unloads(CPU *c){
    for (int i = 0; i < g_npending; i++) gcall1(c, FN_LOADER_UNLOAD, G_CARLOADER, g_handles_pending[i]);
    if (g_npending) rlog("[roam] f=%ld %d ride(s) of friends' cars unloaded\n", rh_frame(), g_npending);
    g_npending = 0;
}
/* leaving free roam with the world still there: park the cars (rides stay
 * until the world is gone) */
static void park_all(CPU *c){
    for (int k = 0; k < NSLOT; k++){
        Slot *s = &g_sl[k];
        if (s->st == SL_ACTIVE && ok(s->car, 0x9e0) && !s->hidden){ soft_park(c, s->car); s->hidden = 1; }
    }
}
/* the friend is gone (left, timed out, or away too long): the car is
 * hidden but keeps the friend's ride for a while (SL_PARKED) */
static void release(CPU *c, int k, const char *why){
    Slot *s = &g_sl[k];
    if (s->st == SL_FREE || s->st == SL_PARKED) return;
    rlog("[roam] f=%ld slot %d: %s's car removed (%s)\n", rh_frame(), k, s->name, why);
    g_releases++;
    if (s->h_new){ unload_later(s->h_new); s->h_new = 0; s->pending_cfg = 0; }
    /* not made yet: forget it.  A car made whose first look cannot load now
     * (begin_load right after finish_create: a friend's shop parts waiting
     * for part_packs) goes on below: it stays ours, parked on its first ride
     * with h_orig kept, and the next try loads onto it */
    if (s->st == SL_CREATING && !s->car){
        if (s->h_orig) unload_later(s->h_orig);
        s->h_orig = 0; s->st = SL_FREE; s->peer = -1; s->peer_id = 0;
        return;
    }
    if (ok(s->car, 0x9e0) && s->h_cur){
        if (!s->hidden){ soft_park(c, s->car); s->hidden = 1; }
        s->st = SL_PARKED;
    } else s->st = SL_FREE;                          /* nothing of the friend's on it */
    s->peer = -1; s->peer_id = 0; s->unseen_since = 0; s->name[0] = 0;
    if (s->st == SL_FREE && k < NOPP){ s->car = 0; s->world = 0; }
}
/* the car gets its own ride back (still resident: the world, or for a car
 * of our own its first load, holds it), is deactivated, and the friend's
 * ride is unloaded */
static void restore(CPU *c, int k){
    Slot *s = &g_sl[k];
    if (s->st != SL_PARKED) return;
    if (ok(s->car, 0x9e0) && s->have_orig && s->activated){
        body_set(c, s->car, 1);
        gcall1(c, FN_CAR_SETRIDE, s->car, s->ride_orig);
        gcall0(c, FN_CAR_DEACTIVATE, s->car);
        s->activated = 0;
    }
    if (s->h_cur) gcall1(c, FN_LOADER_UNLOAD, G_CARLOADER, s->h_cur);
    Slot keep = *s;
    memset(s, 0, sizeof *s);
    s->ride_new = keep.ride_new; s->ride_orig = keep.ride_orig; s->rec = keep.rec; s->peer = -1;
    if (keep.created){                              /* the car stays ours, parked, for the next friend */
        s->car = keep.car; s->created = 1; s->unindexed = keep.unindexed; s->world = keep.world; s->h_orig = keep.h_orig;
        s->have_orig = keep.have_orig;
        memcpy(s->com, keep.com, sizeof s->com);
    }
    rlog("[roam] f=%ld slot %d: car back to its own ride\n", rh_frame(), k);
}
/* Room in the Cars pool for one more ride (3.6 MB), for a friend's new look
 * and for a car of our own alike.  An earlier look's ride, once unloaded,
 * stays in the loader with no reference: its model keeps its pool memory
 * until the loader runs short, and then the loader's own purge also evicts
 * composited skins (phase 1, the black player car of research §5.4).  So the
 * pool fills after about a dozen car types in one world and every new look
 * would be refused.  The unreferenced entries go first (phase 0, one per
 * call); entries in use (the player's, traffic's, parked friends' cars) are
 * not touched.  No room: logged once per friend every 1,200 frames.
 * The purge frees an unreferenced entry whatever it is doing, and a friend
 * who leaves while their car loads leaves one in the middle of its load: its
 * step under way (a pack, its textures) ends later in a callback that writes
 * into the freed entry or pack record, or whatever took its place, and goes
 * on with it.  So it runs only with the loader idle, as the game's own purge
 * does (0x63AE70 services the loader until +0x14 is 0); busy: the next frame. */
#define POOL_NEED (3600u * 1024u)
/* Room among the loader's ride entries (RIDE_ENTRIES) too.  An unloaded
 * ride keeps its entry until a purge, and the game purges only in Load
 * (0x63AE70), once every entry is taken -- after servicing the loader until
 * it is idle, and each step's callback goes straight on to the next load,
 * so the frame waited for the load under way and all those queued behind
 * it.  With friends coming and going all the time (the hub relinks as the
 * player drives), about 128 rides in, every load was a 30-530 ms frame (a
 * crowded 2026-10-26 session: none before the 128th load since the crib,
 * 95 of the 410 after).  So the unused entries go here, with the loader
 * idle, before they run out: one a frame (rides_tidy), two before a load.
 * A purge takes ~0.5 ms (a ride's models out): never a batch (20 at once
 * were a 6-16 ms frame).  RIDES_FREE stay free for a crowd arriving at once
 * (a jump: 32 loads queued while the old rides are still held; with only
 * 16 free the rest waited for the loader one by one, all 32 cars shown
 * after 4.1 s instead of 3.0), and two at the least: this load's and one
 * for a load of the game's own (traffic).  Busy and short of those: the
 * next frame. */
#define RIDES_FREE      40
#define RIDES_STEP      2
static struct { int n; int64_t us; } g_rides_out;
static void rides_out(CPU *c, uint32_t p, int most){
    uint32_t n = MR32(p + 0x2cu);
    if (MR32(p + 0x18u) + RIDES_FREE <= n || rd32(G_CARLOADER + LOADER_BUSY)) return;
    int k = 0;
    int64_t t0 = roam_plat_now_us();
    while (MR32(p + 0x18u) + RIDES_FREE > n && k < most && (gcall1(c, FN_LOADER_PURGE, G_CARLOADER, 0) & 0xffu)) k++;
    g_rides_out.n += k; g_rides_out.us += roam_plat_now_us() - t0;
    if (g_rides_out.n >= 50){
        rlog("[roam] f=%ld car loader: %d unused rides out (%.1f ms in all), %u of %u entries in use\n", rh_frame(), g_rides_out.n,
             g_rides_out.us / 1000.0, MR32(p + 0x18u), n);
        g_rides_out.n = 0; g_rides_out.us = 0;
    }
}
static void rides_tidy(CPU *c){
    uint32_t p = rd32(G_RIDEPOOL);
    if (ok(p, 0x30)) rides_out(c, p, 1);
}
static int ride_room(CPU *c){
    uint32_t p = rd32(G_RIDEPOOL);
    if (!ok(p, 0x30)) return 1;
    uint32_t n = MR32(p + 0x2cu);
    rides_out(c, p, RIDES_STEP);
    if (MR32(p + 0x18u) + 2 <= n) return 1;
    static long said;
    if (!said || rh_frame() - said > 1200){ said = rh_frame();
        rlog("[roam] f=%ld car loader: %u of %u entries taken, the loader %s: loads wait\n", rh_frame(), MR32(p + 0x18u), n,
             rd32(G_CARLOADER + LOADER_BUSY) ? "busy" : "holding no unused ride"); }
    return 0;
}
static struct { uint32_t id; long next; } g_memnote[RC_TABLE];
static int pool_room(CPU *c, const Slot *s, const char *car /* 0: a car of our own */){
    if (!ride_room(c)) return 0;
    if (pool_free() >= POOL_NEED) return 1;
    if (rd32(G_CARLOADER + LOADER_BUSY)) return 0;
    int n = 0;
    while (pool_free() < POOL_NEED && n < 64 && (gcall1(c, FN_LOADER_PURGE, G_CARLOADER, 0) & 0xffu)) n++;
    if (n){ rlog("[roam] f=%ld car pool: %d unused model(s) unloaded\n", rh_frame(), n); pool_log("unloaded"); }
    if (pool_free() >= POOL_NEED) return 1;
    int i = s->peer >= 0 && s->peer < RC_TABLE ? s->peer : 0;
    if (g_memnote[i].id != s->peer_id || rh_frame() >= g_memnote[i].next){
        g_memnote[i].id = s->peer_id; g_memnote[i].next = rh_frame() + 1200;
        if (car) rlog("[roam] f=%ld not enough car memory for %s's %s (%u KB free): not shown (raise NFSU2_ROAM_CARPOOL_KB)\n",
                         rh_frame(), s->name, car, pool_free() / 1024u);
        else rlog("[roam] f=%ld not enough car memory for a car of our own for %s (%u KB free): not shown (raise NFSU2_ROAM_CARPOOL_KB)\n",
                  rh_frame(), s->name, pool_free() / 1024u);
        pool_log("full");
    }
    return 0;
}
/* A racer number no car of this world has and no car of ours being made
 * (a signed byte): 103 to 127, then 99 down to 66 (the player has 0, the
 * opponent cars 100 to 102, traffic from 50).  The minimap finds a car by it
 * (0x5F0E30, the first on the World's list), so each must be its own. */
static int racer_number_used(uint32_t w, int n){
    uint32_t it = rd32(w);
    for (int m = 0; it && it != w && m < 128 && ok(it, 0x9e0); m++, it = rd32(it)) if ((int16_t)MR16(it + 0xau) == n) return 1;
    for (int k = NOPP; k < NSLOT; k++) if ((g_sl[k].st == SL_CREATING || g_sl[k].created) && (int8_t)MR8(g_sl[k].rec + 1u) == n) return 1;
    return 0;
}
static int racer_number(uint32_t w){
    for (int n = 103; n <= 127; n++) if (!racer_number_used(w, n)) return n;
    for (int n = 99; n >= 66; n--) if (!racer_number_used(w, n)) return n;
    return 127;
}
/* a car of our own (slots NOPP and up): a copy of the first opponent's racer
 * record (a racer number of its own, paint slot 5 and up), its ride loaded
 * through the CarLoader, then CreateCar (finish_create); the friend's ride
 * follows as for an opponent car */
static int part_packs(CPU *c);
static int begin_create(CPU *c, int k){
    Slot *s = &g_sl[k];
    uint32_t w = world(), opp = 0;
    for (int i = 0; w && i < 22 && !opp; i++){ uint32_t car = ai_car(w, i); if (car && game_opponent(car)) opp = car; }
    uint32_t orec = opp ? rd32(opp + 0x14u) : 0;
    if (!ok(orec, REC_SZ) || !g_gmem) return 0;
    part_packs(c);                                               /* (their loading starts; begin_load waits for them) */
    if (!skin_textures(c, slot_skin(k))) return 0;
    if (!pool_room(c, s, 0)) return 0;
    int rn = racer_number(w);
    memcpy(GPTR(s->rec), GPTR(orec), REC_SZ);
    MR8(s->rec + 1u) = (uint8_t)rn; MR8(s->rec + 0x900u) = 0;
    gcall1(c, FN_RIDE_SKINS, s->rec + 0x10u, slot_skin(k));
    uint32_t a3[3] = { s->rec + 0x10u, 0, 0 };
    s->load_wait = load_wait_us();
    s->h_orig = rh_call(c, FN_LOADER_LOAD, G_CARLOADER, 0, 3, a3);
    uint32_t a2[2] = { 0, 0 }; rh_call(c, FN_LOADER_SERVICE, G_CARLOADER, 0, 2, a2);
    s->load_t0 = roam_plat_now_us(); s->load_f0 = rh_frame();
    return 1;
}
static int finish_create(CPU *c, int k){
    Slot *s = &g_sl[k];
    uint32_t e = loader_entry(s->h_orig);
    uint32_t done = e ? gcall1(c, FN_LOADER_LOADED, G_CARLOADER, e) : 0;
    if (!done){
        if (roam_plat_now_us() - s->load_t0 > s->load_wait){ unload_later(s->h_orig); s->h_orig = 0; g_load_fail++; return -1; }
        return 0;
    }
    gcall1(c, FN_LOADER_WAIT, G_CARLOADER, s->h_orig);
    uint32_t w = world(), me = player_car();
    if (!w || !me) return -1;
    uint32_t grid = g_gmem + 0x80u;                          /* scratch: the grid entry {vec3, +0x10 heading} */
    for (uint32_t i = 0; i < 0x20u; i += 4) MR32(grid + i) = 0;
    MRF32(grid) = MRF32(me + 0x60u) + 6.f; MRF32(grid + 4) = MRF32(me + 0x64u); MRF32(grid + 8) = MRF32(me + 0x68u) + 0.5f;
    /* World::IndexCars writes cars[] without a bound, and the World's
     * fields follow its 22 entries: a car past them is of a kind IndexCars
     * skips, on the World's list only (the renderer walks the list; nothing
     * reads the debug arrays its index 0 writes, 0x89CCF8).  In the port
     * every car of ours is such a car: the outrun challengers' manager and
     * the game's outruns take the index's AI range (LEND_OPP) */
    uint32_t n0 = MR32(w + 0x10u), a2[2] = { 0, 0 };
    int out = !LEND_OPP || n0 >= WORLD_INDEX || O.test_outside;
    if (out) MR32(s->rec + 4u) = KIND_OUT;
    rh_call(c, FN_CREATECAR, grid, s->rec, 2, a2);
    if (!out) gcall0(c, FN_INDEXCARS, w);
    /* the new car is the one on the World's list with our racer record
     * (CreateCar's eax is not the car) */
    uint32_t car = 0, it = rd32(w);
    for (int n = 0; it && it != w && n < 128 && ok(it, 0x9e0); n++, it = rd32(it)) if (rd32(it + 0x14u) == s->rec) car = it;
    if (!car){ rlog("[roam] f=%ld slot %d: CreateCar made no car\n", rh_frame(), k); return -1; }
    s->car = car; s->created = 1; s->unindexed = out; s->world = w; s->activated = 1; g_created_n++; g_creates++;
    if (!out){
        keep_out_of_window(c, w);
        /* the minimap makes its icons when the HUD is built: an AI racer's
         * arrow for each car of the World's AI range then (by racer number,
         * 0x4EC950), so a car made later has none.  They are made again, now
         * with this one */
        gcall0(c, FN_MM_REBUILD, 0);
    }
    memcpy(GPTR(s->ride_orig), GPTR(s->rec + 0x10u), 0x8f0); s->have_orig = 1;
    make_inert(c, car);
    body_set(c, car, 0); s->ghost = 1;
    soft_park(c, car);
    rlog("[roam] f=%ld slot %d: car of our own created: car 0x%08x, %s, cars %u -> %u, AI range [%u, %u)\n", rh_frame(), k,
            car, out ? "outside the car index" : "indexed", n0, MR32(w + 0x10u), MR32(w + 0x14u), MR32(w + 0x18u));
    return 1;
}
/* The shops' parts that fit any car have models of their own, in packs all
 * cars share: spoilers, roof scoops, side mirrors, exhaust tips, brake
 * calipers and the trunk's audio in the game's eleven at G_PARTPACKS
 * (CARS\SPOILER\GEOMETRY.BIN, ...; a part's owner is its pack's folder,
 * bStringHash("SPOILER")), and the rim shops' rims (owner "WHEELS") in a
 * pack per brand, CARS\WHEELS\GEOMETRY_<BRAND>.BIN.  The car loader streams
 * a ride's models from those packs into the cars pool (0x63A920; the rims'
 * 0x633650), as it streams the car's own from its car's pack.  The model
 * streamer has those packs in the front end only, where the shops are:
 * going into the world, the game keeps the models the player's car uses
 * (the streamer's HIBERNATING pack, 0x48DAE0) and unloads the packs
 * (0x581A20).  So a friend's shop parts had nothing to stream from: the load
 * finished without them, the car's render info found no models for them
 * (0x634800: +0x17A8 + 0x20 x the ride slot) and the car was drawn without
 * its spoiler, roof scoop, side mirrors, exhaust tips and calipers, and with
 * shop rims without tyres and rims, its brake discs only.  The first
 * friend's car in free roam loads the packs again, for the session, as the
 * front end does (0x57F030 mode 1: each pack's header, a few KB; the pack
 * loader counts its loads, so the front end's own load and unload of them
 * stay a pair), and a ride with such parts waits until their headers are
 * in (at most PARTPACK_WAIT_US). */
#define FN_PACK_LOAD    0x004941c0u   /* cdecl (file, 0, 0, 0): a model pack for the streamer (one load more of a loaded one) */
#define FN_PACK_FIND    0x0048ce80u   /* thiscall G_STREAMER (file) -> the pack: +0x8A busy, +0xA0 its models (0: the header is not in) */
#define G_STREAMER      0x0082dbc8u
#define G_PARTPACKS     0x0086658cu   /* char *[N_PARTPACKS]: the shared part packs, CARS\<owner>\GEOMETRY.BIN (0x77D850) */
#define N_PARTPACKS     11
#define FN_PART_NEXT    0x0061b670u   /* thiscall G_CARPARTDB (car hash, slot, 0, previous part, -1) -> the next part */
#define FN_PART_NAME    0x00610000u   /* thiscall CarPart -> its name */
#define RIM_BRANDS      0xa6e1b733u   /* the rim brands: the parts of slot 0x43 of this car hash (0x57F030) */
#define OWNER_WHEELS    0x83347c07u   /* bStringHash("WHEELS"): an aftermarket rim's owner (0x633650) */
#define CRI_RIMS        0x1b48u       /* CarRenderInfo: the front rims' model (slot 0x1D's first), +0x20 the rear's (0x6174F4 draws them) */
#define CRI_MODELS      0x17a8u       /* CarRenderInfo: each ride slot's models, 0x20 bytes per slot (two of four LODs) */
#define PARTPACK_WAIT_US 10000000
static uint32_t g_pack[48]; static int g_npack = -1, g_packs_ok;   /* -1: not loaded yet */
static int64_t g_packs_t0;
static uint32_t g_packowner[N_PARTPACKS]; static int g_npackowner = -1;
static void gstr(uint32_t a, const char *s, uint32_t cap);
/* a part of one of the shared packs */
static int shared_owner(uint32_t owner){
    if (owner == OWNER_WHEELS) return 1;
    if (g_npackowner < 0){
        g_npackowner = 0;
        for (uint32_t i = 0; i < N_PARTPACKS; i++){
            char nm[64]; cstr(rd32(G_PARTPACKS + 4u * i), nm, sizeof nm);
            char *a = nm; while (*a && *a != '\\') a++;              /* CARS\<owner>\GEOMETRY.BIN */
            char *b = *a ? ++a : a; while (*b && *b != '\\') b++;
            if (!*b || b == a) continue;
            *b = 0; g_packowner[g_npackowner++] = bhash(a);
        }
    }
    for (int i = 0; i < g_npackowner; i++) if (g_packowner[i] == owner) return 1;
    return 0;
}
/* Those packs stay in from here on, and the game does not expect that: going
 * from the front end into the world (0x581A20) it moves the models the
 * player's car uses out of them into the HIBERNATING pack (0x48DAE0) and
 * unloads them, and the front end's load of them takes the models back
 * (0x48DC60, 0x57F030 mode 4).  A pack kept in by this load still lists
 * those models (not loaded) beside HIBERNATING, and the streamer loads and
 * unloads a model by name in whichever pack it finds first (0x48CED0, the
 * most recently used first): a friend's car with one of the player's parts
 * loaded a second copy into the pack, loads and unloads were counted against
 * either copy, and the front end's take-back wrote HIBERNATING's over the
 * pack's.  After a visit to the front end (a shop, the crib) the player's car
 * had no model left for its rims, spoiler or exhaust tips, in free roam and
 * in races (tests/scripts/roam_ownparts.txt: 4 of 4 drawn, then 1, then 0;
 * players' reports: a magazine cover's car without wheels).  So the models go back to their pack as the front end does,
 * whenever HIBERNATING holds some a pack of ours lists: once the pack is idle
 * (0x48D3C0's test: header in, no loads going on), all but those with a
 * copy of their own in the pack (taking them back writes over the pack's
 * entry).  Then each model is in one pack again, and its loads and unloads
 * all count there. */
#define FN_PACK_WAKE    0x0048dc60u   /* cdecl (file): a pack's models back from HIBERNATING (0x57F030 mode 4) */
#define G_HIBER         0x00830c78u   /* HIBERNATING's models: +0xC how many, +0x78 the list (0x18 bytes: +0 name, +0x14 the data) */
static uint32_t g_wake_n = 0xffffffffu; static int g_wake_dup;
static uint32_t pack_model(uint32_t pk, uint32_t name){
    uint32_t tab = MR32(pk + 0xa0u), n = MR32(pk + 0xa4u), lo = 0, hi = n;
    if (!tab || n > 100000u || !ok(tab, n * 0x18u)) return 0;
    while (lo < hi){ uint32_t m = (lo + hi) / 2u; if (MR32(tab + 0x18u * m) < name) lo = m + 1u; else hi = m; }   /* sorted (0x488BC0) */
    return lo < n && MR32(tab + 0x18u * lo) == name ? tab + 0x18u * lo : 0;
}
static void packs_wake(CPU *c){
    uint32_t n = rd32(G_HIBER + 0xcu), tab = rd32(G_HIBER + 0x78u);
    if (g_npack <= 0 || n == g_wake_n) return;      /* nothing new there */
    g_wake_n = n;
    if (!n || n > 0x100u || !ok(tab, n * 0x18u)) return;
    for (int i = 0; i < g_npack; i++){
        uint32_t pk = g_pack[i];
        if (!ok(pk, 0xc0) || !MR32(pk + 0xa0u)) continue;
        int k = 0, dup = 0;
        for (uint32_t j = 0; j < n; j++){
            uint32_t e = pack_model(pk, MR32(tab + 0x18u * j));
            if (e){ k++; dup += MR32(e + 0x14u) != 0; }
        }
        if (!k) continue;
        char nm[64]; cstr(pk + 8u, nm, sizeof nm);
        if (MR8(pk + 0x8au) || !MR32(pk + 0x90u) || (int32_t)MR32(pk + 0xa8u) > 0){ g_wake_n = 0xffffffffu; continue; }  /* busy: next frame */
        /* a model with a copy of its own in the pack stays in HIBERNATING: for the take-back (which walks
         * HIBERNATING's list, finds each name in the pack and moves the entry over the pack's) it has a name the
         * pack does not list, its own again after; the others go back (a pack left whole for one of them kept the
         * player's spoiler there, and the next shop's take-back lost it) */
        uint32_t hid[0x100], was[0x100]; int nh = 0;
        for (uint32_t j = 0; dup && j < n && nh < 0x100; j++){
            uint32_t nmj = MR32(tab + 0x18u * j), e = pack_model(pk, nmj);
            if (!e || !MR32(e + 0x14u)) continue;
            uint32_t t = nmj ^ 0x5a5a5a5au;
            while (!t || pack_model(pk, t)) t += 0x9e3779b9u;
            MR32(tab + 0x18u * j) = t; hid[nh] = t; was[nh++] = nmj;
        }
        if (dup && !g_wake_dup++) rlog("[roam] f=%ld %s: %d of its %d models in HIBERNATING are in it too, left there\n", rh_frame(), nm, dup, k);
        if (k > dup){
            uint32_t file = pk + 8u;
            rh_call(c, FN_PACK_WAKE, 0, 0, 1, &file);
            rlog("[roam] f=%ld %s: %d model(s) back from HIBERNATING (%u left there)\n", rh_frame(), nm, k - dup, rd32(G_HIBER + 0xcu));
        }
        if (nh){
            uint32_t n2 = rd32(G_HIBER + 0xcu), t2 = rd32(G_HIBER + 0x78u);
            for (uint32_t j = 0; ok(t2, n2 * 0x18u) && j < n2 && n2 <= 0x100u; j++)
                for (int h = 0; h < nh; h++) if (MR32(t2 + 0x18u * j) == hid[h]){ MR32(t2 + 0x18u * j) = was[h]; break; }
        }
        n = rd32(G_HIBER + 0xcu); tab = rd32(G_HIBER + 0x78u);
        if (!n || n > 0x100u || !ok(tab, n * 0x18u)) break;
    }
    if (g_wake_n != 0xffffffffu) g_wake_n = rd32(G_HIBER + 0xcu);
}
static void load_pack(CPU *c, uint32_t file, const char *path){
    uint32_t z[4] = { file, 0, 0, 0 };
    rh_call(c, FN_PACK_LOAD, 0, 0, 4, z);
    uint32_t pk = gcall1(c, FN_PACK_FIND, G_STREAMER, file);
    if (pk) g_pack[g_npack++] = pk;
    else rlog("[roam] f=%ld %s did not load\n", rh_frame(), path);
}
static int part_packs(CPU *c){
    if (g_packs_ok) return 1;
    if (!g_gmem) return 0;
    if (g_npack < 0){
        g_npack = 0; g_packs_t0 = roam_plat_now_us();
        const int cap = (int)(sizeof g_pack / sizeof g_pack[0]);
        char path[96];
        for (uint32_t i = 0; i < N_PARTPACKS; i++){
            uint32_t file = rd32(G_PARTPACKS + 4u * i);
            cstr(file, path, sizeof path);
            if (path[0]) load_pack(c, file, path);
        }
        int shared = g_npack;
        uint32_t fn = g_gmem + 0xa0u, part = 0;
        for (int i = 0; i < 64 && g_npack < cap; i++){
            uint32_t a[5] = { RIM_BRANDS, 0x43u, 0, part, 0xffffffffu };
            if (!(part = rh_call(c, FN_PART_NEXT, G_CARPARTDB, 0, 5, a))) break;
            char nm[40]; cstr(gcall0(c, FN_PART_NAME, part), nm, sizeof nm);
            if (!nm[0]) continue;
            snprintf(path, sizeof path, "CARS\\WHEELS\\GEOMETRY_%s.BIN", nm);
            gstr(fn, path, 0x60u);
            load_pack(c, fn, path);
        }
        rlog("[roam] f=%ld friends' shop parts: %d shared packs and %d rim brands' packs loaded\n", rh_frame(), shared, g_npack - shared);
    }
    int in = 0;
    for (int i = 0; i < g_npack; i++) in += ok(g_pack[i], 0xc0) && !MR8(g_pack[i] + 0x8au) && MR32(g_pack[i] + 0xa0u);
    if (in < g_npack && roam_plat_now_us() - g_packs_t0 < PARTPACK_WAIT_US) return 0;
    if (in < g_npack) rlog("[roam] f=%ld friends' shop parts: %d of %d packs in after %.0f s, going on without the others\n", rh_frame(), in,
                           g_npack, PARTPACK_WAIT_US / 1e6);
    else rlog("[roam] f=%ld friends' shop parts: the packs are in after %.1f s\n", rh_frame(), (roam_plat_now_us() - g_packs_t0) / 1e6);
    packs_wake(c);                                  /* the player's car's models back in them first */
    g_packs_ok = 1;
    return 1;
}
/* start loading peer's ride for slot k */
static int begin_load(CPU *c, int k, const uint8_t *cfg, int len, uint32_t gen){
    Slot *s = &g_sl[k];
    rc_carcfg cc;
    if (rc_carcfg_decode(cfg, len, &cc) != RC_OK) return 0;
    int in = part_packs(c), shop = 0;                            /* (the packs start loading with the first friend) */
    for (int i = 0; i < cc.n_parts; i++) shop |= shared_owner(cc.part[i].owner);
    if (!in && shop) return 0;
    uint32_t skin = slot_skin(k);
    if (!skin_textures(c, skin)) return 0;
    if (!pool_room(c, s, cc.type_name)) return 0;
    uint32_t ride = s->ride_new;
    char why[96] = {0};
    int miss = build_ride(c, &cc, ride, skin, why, sizeof why);
    if (miss < 0) return 0;
    if (rh_opt("NFSU2_ROAM_TEST_COPYRIDE")){
        /* test control: the player's own RideInfo byte for byte (same car
         * type only), the slot's skin number: rebuilt vs. copied looks */
        uint32_t me = player_car(), own = me ? rd32(me + 0x10u) : 0;
        if (ok(own, 0x8f0) && MR32(own) == MR32(ride)){
            memcpy(GPTR(ride), GPTR(own), 0x8f0); gcall1(c, FN_RIDE_SKINS, ride, skin);
            rlog("[roam] f=%ld slot %d: TEST: the player's own ride copied instead of the rebuilt one\n", rh_frame(), k); }
    }
    if (why[0]) rlog("[roam] f=%ld slot %d: %s's car: %s\n", rh_frame(), k, s->name, why);
    uint32_t a3[3] = { ride, 0, 0 };
    s->load_wait = load_wait_us();
    s->h_new = rh_call(c, FN_LOADER_LOAD, G_CARLOADER, 0, 3, a3);
    uint32_t a2[2] = { 0, 0 }; rh_call(c, FN_LOADER_SERVICE, G_CARLOADER, 0, 2, a2);
    s->load_t0 = roam_plat_now_us(); s->load_f0 = rh_frame(); s->pending_cfg = 1; s->cfg_gen = gen;
    s->look = rc_fnv1a_bytes(cfg, (uint32_t)len);
    snprintf(s->carname, sizeof s->carname, "%s", cc.type_name);
    s->loads++;
    rlog("[roam] f=%ld slot %d: loading %s's %s (%u parts, skin %u, handle 0x%x)\n", rh_frame(), k, s->name,
            cc.type_name, cc.n_parts, skin, s->h_new);
    return 1;
}
static void eng_files(CPU *c, Slot *s);
static int heavy_take(void);
/* the loading ride is in: put it on the slot's car */
static int finish_load(CPU *c, int k){
    Slot *s = &g_sl[k];
    uint32_t e = loader_entry(s->h_new);
    uint32_t done = e ? gcall1(c, FN_LOADER_LOADED, G_CARLOADER, e) : 0;
    if (!done){
        if (roam_plat_now_us() - s->load_t0 > s->load_wait){
            rlog("[roam] f=%ld slot %d: %s's car did not load in %.0f s\n", rh_frame(), k, s->name, s->load_wait / 1e6);
            unload_later(s->h_new); s->h_new = 0; s->pending_cfg = 0; g_load_fail++;
            return -1;
        }
        return 0;
    }
    if (!heavy_take()) return 0;                    /* in: put on its car the next frame */
    gcall1(c, FN_LOADER_WAIT, G_CARLOADER, s->h_new);
    uint32_t car = s->car;
    if (!s->have_orig){                             /* an opponent car, first time: keep its own ride */
        memcpy(GPTR(s->ride_orig), GPTR(rd32(car + 0x14u) + 0x10u), 0x8f0);
        s->have_orig = 1;
    }
    if (!s->activated){ gcall0(c, FN_CAR_ACTIVATE, car); s->activated = 1; }
    gcall1(c, FN_CAR_SETRIDE, car, s->ride_new);
    if (traffic_car(car, 0, 0)) rlog("[roam] f=%ld slot %d: %s's car is a traffic car (%s): it drives through every car\n", rh_frame(), k, s->name, s->carname);
    eng_files(c, s);
    make_inert(c, car);
    body_set(c, car, 0); s->ghost = 1;
    soft_park(c, car);
    if (s->h_cur) gcall1(c, FN_LOADER_UNLOAD, G_CARLOADER, s->h_cur);
    s->h_cur = s->h_new; s->h_new = 0;
    s->pending_cfg = 0; s->hidden = 1;
    /* the renderer draws a car at +0x50 (with the matrix copy at +0xD0),
     * the body position minus the centre-of-mass offset; the game sets it
     * in 0x5F5010 (when a car lands on loaded track, or is placed), so a
     * car moved from outside needs it every frame.  The offset is fixed in
     * the car's own frame: the player's car has +0x60 - +0x50 = R (0.30, 0,
     * 0.30) for a 206 at every heading (its physics object's world vector),
     * while for a car without physics 0x5F5010 subtracts the RidePhysicsInfo
     * vector ([+0x20]) as it is, unrotated -- so the value measured here is
     * the body-frame vector, and apply_pose rotates it with the car
     * (written unrotated, the drawn car was 0.2 to 0.6 m
     * off where the friend's own game draws it) */
    gcall0(c, FN_CAR_BODYPOS, s->car);
    for (int i = 0; i < 3; i++){ s->com[i] = MRF32(s->car + 0x60u + 4u * (uint32_t)i) - MRF32(s->car + 0x50u + 4u * (uint32_t)i);
        if (!__builtin_isfinite(s->com[i]) || __builtin_fabsf(s->com[i]) > 3.f) s->com[i] = 0; }
    soft_park(c, s->car);
    set_name(s);
    char tn[20]; cstr(rd32(s->car + 0xcu), tn, sizeof tn);
    rlog("[roam] f=%ld slot %d: %s's %s ready after %ld frames (%.0f ms); car 0x%08x is now a %s (centre of mass %.2f %.2f %.2f)\n",
            rh_frame(), k, s->name, s->carname, rh_frame() - s->load_f0, (roam_plat_now_us() - s->load_t0) / 1000.0, s->car, tn,
            (double)s->com[0], (double)s->com[1], (double)s->com[2]);
    pool_log("after load");
    g_ready_at = roam_plat_now_us();
    return 1;
}
/* sin and cos of a in [-pi, pi] (Taylor to the x^15 / x^14 term, < 1e-5),
 * from basic operations: this file has no libm */
static void sincos_pi(float a, float *sn, float *cs){
    float x2 = a * a;
    *sn = a * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f * (1.f - x2 / 72.f * (1.f - x2 / 110.f *
               (1.f - x2 / 156.f * (1.f - x2 / 210.f)))))));
    *cs = 1.f - x2 / 2.f * (1.f - x2 / 12.f * (1.f - x2 / 30.f * (1.f - x2 / 56.f * (1.f - x2 / 90.f *
               (1.f - x2 / 132.f * (1.f - x2 / 182.f))))));
}
/* The wheels.  The renderer draws each wheel with its matrix in the car,
 * +0x130 + 0x40 k for front left, front right, rear right, rear left (rows:
 * forward, left, up, then the wheel's place), and its brake with +0x230 +
 * 0x40 k (0x5F6820 hands both to the car's render, 0x620AE0).  In physics
 * mode 0 the game builds them again every frame, before our hook (the mode's
 * update 0x5EB920 -> 0x5921B0 for each wheel): no spin and no steering, and
 * the place the ride's wheel position (RidePhysicsInfo +0x10 + 0x30 k)
 * raised by the wheel's suspension offset (CAR_SUSP).  Only the physics
 * modes write that offset (0x59CB90): a friend's car that the game put into
 * physics for a moment (apply_pose puts it back to mode 0) kept whatever they
 * left there, and with a NaN every later frame's wheels were NaN, the tires
 * gone for good (reported in play, mostly after a crash; with the offsets
 * poked to NaN the wheels vanish, at 0.8 they float over the car).  So here
 * the offsets go back to ours (0, or the friend's wheel heights) and the
 * places are written every frame, and the first times that was needed are
 * logged.  Spin: the wheel's angle goes down by
 * speed / tyre radius (RidePhysicsInfo +0x20 + 0x30 k) as the car goes
 * forward.  Steering: the friend's own, the controls' +0x1e0 that goes on the
 * wire as steer.  It is the angle the game's physics turns the front wheels
 * to over a few frames, 16 bits to a full turn (steer 1 = 180 degrees, left
 * positive), speed-dependent lock included (car dumps: the caliper settles at
 * 180 degrees x steer at speed under the game's autopilot, and at 45 degrees
 * for the keys' full lock standing still); so whatever the friend steers
 * shows, counter-steer and standing still too.  Smoothed over 80 ms, at most
 * 0.8 rad.  Spinners have matrices of their own, made at the render
 * (0x5EC7F0): at the wheel's place (with CAR_SUSP), the front ones steered by
 * CAR_STEER, and each turned by its own angle (CAR_SPIN), which follows the
 * wheel with inertia (0x5ECAC0: the spinner's speed goes to the wheel's,
 * +0x330, by 0.156 of the difference a second, so it turns on for seconds
 * after the car stops).  The game runs that for a friend's car too, from the
 * wheel speeds apply_pose writes; the steering is the physics', which leaves
 * it at 0 in mode 0, so apply_pose writes it (else a front spinner pointed
 * straight on a wheel that steered). */
static void wheels_pose(Slot *s, const rc_pose *P, float dt){
    uint32_t car = s->car, ph = MR32(car + 0x20u);
    int phok = ok(ph, 0xc0);
    if (!(dt > 0.f)) dt = 0.f;
    if (dt > 0.1f) dt = 0.1f;
    float v = P->speed, want = P->steer * 3.14159265f;
    if (want > 0.8f) want = 0.8f;
    if (want < -0.8f) want = -0.8f;
    float k = dt / 0.08f;
    s->steer += (want - s->steer) * (k < 1.f ? k : 1.f);
    if (!__builtin_isfinite(s->steer)) s->steer = 0.f;
    float ss, cs; sincos_pi(s->steer, &ss, &cs);
    float susp[4]; int moved = 0;
    for (uint32_t w = 0; w < 4; w++){
        susp[w] = MRF32(car + CAR_SUSP + 4u * w);
        moved |= susp[w] != s->susp[w] && susp[w] != 0.f;            /* a NaN too; 0 is at rest (soft_park) */
        MRF32(car + CAR_SUSP + 4u * w) = s->susp[w] = 0.f;
    }
    if (moved && s->wheelfix++ < 3)
        rlog("[roam] f=%ld slot %d: %s's car had its wheels moved (suspension %g %g %g %g, put back into physics mode 0 %u times): put back\n",
                rh_frame(), (int)(s - g_sl), s->name, (double)susp[0], (double)susp[1], (double)susp[2], (double)susp[3], s->remode);
    for (uint32_t w = 0; w < 4; w++){
        float r = phok ? MRF32(ph + 0x20u + 0x30u * w) : 0.f;
        if (!(r > 0.05f && r < 2.f)) r = 0.33f;
        float d = (v + P->slip[2 * w]) / r * dt;                     /* (a spinning wheel's slip: a burnout turns it) at most 100 m/s: a few turns per frame */
        if (!__builtin_isfinite(d) || d > 32.f || d < -32.f) d = 0.f;
        float a = s->spin[w] - d;
        while (a > 3.14159265f) a -= 6.28318531f;
        while (a < -3.14159265f) a += 6.28318531f;
        s->spin[w] = a;
        float sa, ca; sincos_pi(a, &sa, &ca);
        float sy = w < 2 ? ss : 0.f, cy = w < 2 ? cs : 1.f;
        uint32_t m = car + 0x130u + 0x40u * w, b = car + 0x230u + 0x40u * w;
        MRF32(m + 0x00u) = ca * cy;  MRF32(m + 0x04u) = ca * sy;  MRF32(m + 0x08u) = sa;
        MRF32(m + 0x10u) = -sy;      MRF32(m + 0x14u) = cy;       MRF32(m + 0x18u) = 0.f;
        MRF32(m + 0x20u) = -sa * cy; MRF32(m + 0x24u) = -sa * sy; MRF32(m + 0x28u) = ca;
        if (w < 2){
            MRF32(b + 0x00u) = cy;  MRF32(b + 0x04u) = sy;
            MRF32(b + 0x10u) = -sy; MRF32(b + 0x14u) = cy;
        }
        /* the wheel's and the brake's place: at rest (0x5921B0's with offset
         * 0), or where the friend's are, when their game sends that (rc_pose
         * wz: their suspension, and their hydraulics lifting the body off the
         * wheels); that height's offset goes into CAR_SUSP too, so the game's
         * own build next frame and the render's second set of wheel places
         * (0x5EC7F0) have it as well */
        float x = phok ? MRF32(ph + 0x10u + 0x30u * w) : 0.f, y = phok ? MRF32(ph + 0x14u + 0x30u * w) : 0.f,
              z = phok ? MRF32(ph + 0x18u + 0x30u * w) : 0.f;
        if (phok && __builtin_fabsf(x) < 4.f && __builtin_fabsf(y) < 2.f && __builtin_fabsf(z) < 2.f){
            float o = P->ext && P->wz[w] > -2.f && P->wz[w] < 2.f ? P->wz[w] - z : 0.f;
            MRF32(m + 0x30u) = x; MRF32(m + 0x34u) = y; MRF32(m + 0x38u) = z + o;
            MRF32(b + 0x30u) = x; MRF32(b + 0x34u) = y; MRF32(b + 0x38u) = z + o;
            MRF32(car + CAR_SUSP + 4u * w) = s->susp[w] = o;
        }
    }
}
/* A friend's car's nitrous (a sender of core version 4 and up; an older
 * one's car has none; its wheel heights are wheels_pose's): the car gets a
 * shot of its own that stays as full as the friend's, and the game draws for
 * it what it draws for any car on nitrous: the tail-light streaks (0x61AE90)
 * and, for a car in the car index, the flames (0x5F5730, from the World's
 * update).  A car outside the index gets the flames here, as 0x5F5730 would
 * give them: FX_NOS on the throttle from 5 mph, FX_PURGE otherwise and while
 * no race runs. */
#define NOS_FULL        40000u        /* the friend's car's full shot: 10 s (a frame on, its share is the same within 0.2 %) */
static void nos_pose(CPU *c, Slot *s, const rc_pose *P){
    uint32_t car = s->car, n = car + CAR_NOS;
    if (!P->ext || !P->nos){ if (MR32(n + 0x420u)) MR32(n + 0x420u) = 0; return; }
    uint32_t now = MR32(G_TICKS);
    MR32(n + 0x414u) = NOS_FULL; MR32(n + 0x41cu) = (uint32_t)P->nos * (NOS_FULL / 100u); MR32(n + 0x420u) = now ? now : 1u;
    if (!s->unindexed) return;
    uint32_t ctl = rd32(car + 0x30u), race = rd32(G_RACE);
    float thr = ok(ctl, 0x220) ? __builtin_fabsf(MRF32(ctl + 0x20cu)) : 0.f, mph = MRF32(car + 0x430u) * 2.237f;
    uint32_t a[4] = { thr > 0.001f && mph >= 5.f ? FX_NOS : FX_PURGE, 0x3f800000u, 0, 0 };   /* strength 1.0 */
    rh_call(c, FN_CAR_FX, car, 0, 4, a);
    if (race && rd32(race) <= 3u){ a[0] = FX_PURGE; rh_call(c, FN_CAR_FX, car, 0, 4, a); }
}
/* A friend's wheels' slip and surfaces (from a sender whose wheels slide;
 * 0 otherwise, and from an older one always): the game makes the tyre smoke,
 * the dirt thrown up and the skid sound of any car from them (0x5F5730:
 * smoke from 5 m/s of slip on a road, by the surface's entry in the table at
 * 0x800EE0; the skid sound 0x46EA50), but only its physics writes them, and
 * a friend's car runs without (physics mode 0): its slip stayed 0, and it
 * slid and spun its wheels without smoke.  The surfaces only come with a
 * slip; between slides each wheel keeps the last */
static void slip_pose(Slot *s, const rc_pose *P){
    uint32_t car = s->car;
    for (uint32_t w = 0; w < 4; w++){
        MRF32(car + CAR_SLIP + 8u * w) = P->slip[2 * w]; MRF32(car + CAR_SLIP + 8u * w + 4u) = P->slip[2 * w + 1];
        if (P->slipx && P->surf[w] < N_SURF) MR32(car + CAR_SURF + 4u * w) = P->surf[w];
    }
}
static void apply_pose(CPU *c, Slot *s, const rc_pose *P, float dt){
    uint32_t car = s->car;
    float f[3], l[3], u[3];
    rc_q2m(P->q, f, l, u);
    for (int i = 0; i < 3; i++){
        uint32_t o = 4u * (uint32_t)i;
        MRF32(car + 0x60u + o) = P->p[i];
        MRF32(car + 0x50u + o) = P->p[i] - (s->com[0] * f[i] + s->com[1] * l[i] + s->com[2] * u[i]);
        MRF32(car + 0x70u + o) = P->v[i]; MRF32(car + 0x110u + o) = P->w[i];
        MRF32(car + 0x90u + o) = f[i]; MRF32(car + 0xa0u + o) = l[i]; MRF32(car + 0xb0u + o) = u[i];
        MRF32(car + 0xd0u + o) = f[i]; MRF32(car + 0xe0u + o) = l[i]; MRF32(car + 0xf0u + o) = u[i];
    }
    MR16(car + 0x40cu) = (uint16_t)(int)(rc_atan2f(f[1], f[0]) / (2.f * 3.14159265f) * 65536.f);
    MRF32(car + 0x42cu) = P->speed; MRF32(car + 0x430u) = P->speed; MRF32(car + 0x440u) = P->rpm;
    MR32(car + 0x434u) = (uint32_t)(int32_t)P->gear;
    uint32_t ph = MR32(car + 0x20u);
    if (ok(ph, 0xc0)) for (uint32_t w = 0; w < 4; w++){ float r = MRF32(ph + 0x20u + 0x30u * w); if (r > 0.05f) MRF32(car + CAR_WSPIN + 4u * w) = (P->speed + P->slip[2 * w]) / r; }
    uint32_t ctl = MR32(car + 0x30u);
    if (ok(ctl, 0x220)){
        MRF32(ctl + 0x20cu) = P->throttle; MRF32(ctl + 0x214u) = P->brake;
        MR16(ctl + 0x1e0u) = (uint16_t)(int16_t)(P->steer * 32767.f);
    }
    if (rd32(car + 0x28u) != 0){ gcall1(c, FN_CAR_SETPHYS, car, 0); s->remode++; }
    gcall1(c, FN_ROADTRACK, car + 0x8d8u, car + 0x60u);
    gcall0(c, FN_CAR_SECTION, car);
    wheels_pose(s, P, dt);
    MR16(car + CAR_STEER) = (uint16_t)(int16_t)(int)(s->steer * 10430.378f);      /* the spinners' steering (wheels_pose) */
    nos_pose(c, s, P);
    slip_pose(s, P);
    s->pose_frames++;
}
/* A hit with the player's car pushes a friend's car on this screen
 * (roam_game_contact): each push holds (the car drawn where the friend's
 * game has it, moved on by the pushes since their hits) until the friend's
 * snapshots from after its hit come, which carry the push their own game
 * gave the car, or HIT_HOLD_US at most (a friend whose snapshots stop, or
 * whose game saw no hit).  Times are the friend's: the snapshots' stamps,
 * and rt, the time the car is drawn at (rc_interp_eval: now, in the
 * friend's clock, give or take the prediction), which a hit is stamped with.
 * Then the car goes on from where it was going to be drawn, the difference
 * to where their game has it (and the pushes still held) fading as the
 * interpolation fades the jump the report makes, so the two cancel.  One
 * push at a time was not enough: a car rolling on against the other touched
 * it again and again, each touch held the whole push anew, and the friend's
 * car ran at twice their speed. */
static void push_off(Slot *s){
    s->pqn = 0;
    for (int i = 0; i < 3; i++){ s->push_v[i] = 0; s->push_u[i] = 0; s->push_r[i] = 0; }
}
/* the friend's game has reported the push: their velocity has moved by half
 * of it at least (the interpolation hands the newest snapshots' velocity on
 * unsmoothed), or a snapshot from HIT_REPORT_US after the hit has come, or
 * never will (HIT_HOLD_US).  By time alone a hit was held up to a snapshot
 * too long, the first one after it already carrying the friend's own: 0.5 m
 * off where their game had the car for 0.2 s.  A nudge, which driving could
 * hide, goes with the first snapshot after it: a car leaning on the other
 * nudges it every frame, and nudges held for HIT_REPORT_US drew the car
 * 0.5 m off for as long as the two leant on each other. */
static int push_reported(const Slot *s, int j, const rc_pose *P, int64_t newest, int64_t rt){
    float dot = 0, pp = 0;
    for (int i = 0; i < 3; i++){ dot += (P->v[i] - s->pq[j].v0[i]) * s->pq[j].v[i]; pp += s->pq[j].v[i] * s->pq[j].v[i]; }
    if (rt - s->pq[j].t >= HIT_HOLD_US) return 1;
    if (pp <= 1.f) return newest >= s->pq[j].t + NUDGE_REPORT_US;
    return dot >= 0.5f * pp || newest >= s->pq[j].t + HIT_REPORT_US;
}
static void push_pose(Slot *s, rc_pose *P, int64_t newest, int64_t rt, float dt){
    for (int i = 0; i < 3; i++) s->net_v[i] = P->v[i];
    if (!s->pqn && s->push_r[0] == 0.f && s->push_r[1] == 0.f && s->push_r[2] == 0.f) return;
    for (int i = 0; i < 3; i++) s->push_u[i] += s->push_v[i] * dt;
    int done = 0;
    while (done < s->pqn && push_reported(s, done, P, newest, rt)){
        float since = (float)(rt - s->pq[done].t) / 1e6f;
        for (int i = 0; i < 3; i++){ s->push_v[i] -= s->pq[done].v[i]; s->push_u[i] -= s->pq[done].v[i] * (since > 0.f ? since : 0.f); }
        done++;
    }
    if (done){
        for (int j = done; j < s->pqn; j++) s->pq[j - done] = s->pq[j];
        s->pqn -= done;
        if (!s->pqn) for (int i = 0; i < 3; i++){ s->push_v[i] = 0; s->push_u[i] = 0; }
        for (int i = 0; i < 3; i++) s->push_r[i] = s->drawn[i] + s->drawn_v[i] * dt - P->p[i] - s->push_u[i];
    } else {
        float k = rc_expneg(dt * 1e6f / (float)HIT_FADE_US), m = 0;
        for (int i = 0; i < 3; i++){ s->push_r[i] *= k; m += s->push_r[i] * s->push_r[i]; }
        if (m < 1e-4f) for (int i = 0; i < 3; i++) s->push_r[i] = 0;
    }
    for (int i = 0; i < 3; i++){ P->p[i] += s->push_u[i] + s->push_r[i]; P->v[i] += s->push_v[i]; }
}

/* ---- names over friends' cars --------------------------------------------------------------------- */
/* In the game's own LAN races the HUD's online part (made by 0x4F80E0 for a
 * network race only) owns the HUD package's text objects ONLINE_PLAYER_1..6,
 * and its update (0x4EBEC0, from the HUD update) puts each player's name
 * 1.7 m above their car through the player's view: hidden behind the
 * camera, off the screen and from 100 m on, scaled 25 m / distance within
 * 0.5 .. 1, and all of them while the game's player-names option is off.
 * In free roam that part does not exist and nothing uses the objects, so
 * friends' cars get them the same way, from the same point of the frame
 * (roam_game_hud, after the HUD update: this frame's camera), the nearest
 * cars first.  Past the six, copies of ONLINE_PLAYER_1 (fe_clone). */
#define G_VIEW1         0x00832e50u   /* eView 1: [+0] +0x80 view*projection, +0x40 camera (+0x40 position, +0x50 forward) */
#define G_VIEW_W        0x00870980u   /* view size in pixels (0x5BC1F0, 0x5BC200) */
#define G_VIEW_H        0x00870984u
#define G_SHOW_NAMES    0x008500c9u   /* byte: the player-names option */
#define PL_HUD          0x9a4u        /* Player: the HUD, +0x10 its package's name */
#define FN_FE_HASH      0x00505450u   /* cdecl (name) -> hash (upper case) */
#define FN_FE_FIND      0x00537a10u   /* cdecl (package name, hash) -> FE object */
#define FN_FE_PRINTF    0x00537be0u   /* cdecl (object, format, ...) */
#define FN_FE_SETSTR    0x00535fe0u   /* thiscall object (char*): its text, the string grown to fit */
#define FN_FE_COLOR     0x00517e20u   /* thiscall object (int rgba[4]*, 0) */
#define FN_FE_POS       0x00517c10u   /* thiscall object (float xyz*, 0): HUD units from the centre */
#define FN_FE_SCALE     0x00517db0u   /* thiscall object (float xyz*, 0) */
#define FN_FE_SHOW      0x0050ca50u   /* cdecl (object) */
#define FN_FE_HIDE      0x0050ca00u   /* cdecl (object) */
#define FN_VIEW_SCREEN  0x005bc4a0u   /* thiscall view (out*, in*): world -> view pixels, z > 0 in front */
#define TAG_SCR         0x100u        /* g_gmem scratch: +0 in, +0x10 out, +0x20 vectors, +0x40 "%s", +0x48 text */
#define TAG_TEXT_MAX    (RC_NAME_MAX + 24)  /* "<name> (paused)", "<name> (paused, 12.3 km)" */
#define TAG_NEAR_M      100.f         /* names from this far only for the crews, with the distance */
#define TAG_GAME        6             /* the HUD's ONLINE_PLAYER_1..6 */
#define TAG_N           NSLOT
#define TAG_DUEL        TAG_N         /* one more: the outrun's line of text (du_hud) */
#define TAG_MP0         (TAG_N + 1)   /* and four: labels over race spots and lobbies (mp_labels) */
#define TAG_MPN         4
#define TAG_ALL         (TAG_N + 1 + TAG_MPN)
#define TAG_DUEL_MAX    120
static struct { uint32_t hud, obj[TAG_ALL]; uint8_t looked[TAG_ALL], shown[TAG_ALL]; char text[TAG_ALL][TAG_DUEL_MAX]; uint32_t rgb[TAG_ALL]; } g_tag;
static void gstr(uint32_t a, const char *s, uint32_t cap){
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) MR8(a + i) = (uint8_t)s[i];
    MR8(a + i) = 0;
}
static uint32_t gvec(uint32_t a, float x, float y, float z){ MRF32(a) = x; MRF32(a + 4u) = y; MRF32(a + 8u) = z; return a; }
static void tag_hide(CPU *c, int k){
    if (g_tag.shown[k] && g_tag.obj[k]) gcall1(c, FN_FE_HIDE, 0, g_tag.obj[k]);
    g_tag.shown[k] = 0;
}
/* A copy of FE object o of package pkg, named `name`: its class's Clone
 * (vtable slot 1, thiscall (1): its scripts copied too; its render object,
 * +0x58, which that leaves unset, made by the renderer as for a new object),
 * the hash the package's lookups compare (+0x10), and linked after o in the
 * list that holds it: the package's own (FEPackage +0x48 count, +0x4C first,
 * +0x50 last) or a group's (type 5: +0x60, +0x64, +0x68), each object's +4
 * next, +8 previous.  The FE renderer draws what those lists hold and the
 * package frees it with the rest.  0 when the list is not as expected. */
#define FN_FE_PACKAGE   0x0052cef0u   /* cdecl (package name) -> FEPackage* */
#define FN_FE_FINDOBJ   0x005379c0u   /* cdecl (package name, hash) -> FE object of any type */
static uint32_t fe_list_of(uint32_t first, uint32_t o, int depth){   /* the address of the list's "first" that holds o */
    int n = 0;
    for (uint32_t it = rd32(first); it && n < 4096; it = rd32(it + 4u), n++){
        if (it == o) return first;
        if (depth < 8 && rd32(it + 0x18u) == 5u){ uint32_t l = fe_list_of(it + 0x64u, o, depth + 1); if (l) return l; }
    }
    return 0;
}
static uint32_t fe_clone(CPU *c, uint32_t pkg, uint32_t o, const char *name){
    uint32_t pk = gcall1(c, FN_FE_PACKAGE, 0, pkg), l = ok(pk, 0x54) ? fe_list_of(pk + 0x4cu, o, 0) : 0;
    if (!l || !ok(rd32(o), 8)) return 0;
    uint32_t n = 0, last = 0;
    for (uint32_t it = rd32(l); it && n < 4096; it = rd32(it + 4u)){ last = it; n++; }
    if (n != rd32(l - 4u) || last != rd32(l + 4u)) return 0;          /* not the count and last we take it for */
    uint32_t one = 1, cl = rh_call(c, rd32(rd32(o) + 4u), o, 0, 1, &one);
    if (!ok(cl, 0x5c)) return 0;
    MR32(cl + 0x58u) = 0;
    /* an image's or a string's drawing data (+0x2C, +0x30 its size: colour,
     * place, turn, size, an image's texture corners; numbers only) the way
     * the original has it: Clone leaves the corners, the depth and (on the
     * world map) the colour, the turn and the size 0, and such a copy was
     * drawn as nothing -- the minimap's arrows past the HUD's eight too, and
     * the world map's lobby labels */
    uint32_t dn = rd32(o + 0x30u), d0 = rd32(o + 0x2cu), d1 = rd32(cl + 0x2cu), ty = MR32(o + 0x18u);
    if ((ty == 1u || ty == 2u) && rd32(cl + 0x30u) == dn && dn && dn <= 0x100u && ok(d0, dn) && ok(d1, dn) && d0 != d1)
        memcpy(GPTR(d1), GPTR(d0), dn);
    uint32_t sa = g_gmem + TAG_SCR + 0x48u;
    gstr(sa, name, 24);
    MR32(cl + 0x10u) = gcall1(c, FN_FE_HASH, 0, sa);
    uint32_t nx = rd32(o + 4u);
    MR32(cl + 4u) = nx; MR32(cl + 8u) = o; MR32(o + 4u) = cl;
    if (nx) MR32(nx + 8u) = cl; else MR32(l + 4u) = cl;
    MR32(l - 4u) += 1;
    return cl;
}
/* an image's or a string's colour: its drawing data's first four ints
 * (blue, green, red, alpha, as FN_FE_COLOR takes them) */
static int fe_colour_get(uint32_t o, int32_t col[4]){
    uint32_t d = ok(o, 0x34) ? rd32(o + 0x2cu) : 0;
    if (!ok(d, 16) || rd32(o + 0x30u) < 16u) return 0;
    for (uint32_t i = 0; i < 4; i++) col[i] = (int32_t)MR32(d + 4u * i);
    return 1;
}
static void fe_colour_set(CPU *c, uint32_t o, int32_t b, int32_t g, int32_t r, int32_t a){
    uint32_t col = g_gmem + TAG_SCR + 0x20u;
    MR32(col) = (uint32_t)b; MR32(col + 4u) = (uint32_t)g; MR32(col + 8u) = (uint32_t)r; MR32(col + 12u) = (uint32_t)a;
    uint32_t ac[2] = { col, 0 }; rh_call(c, FN_FE_COLOR, o, 0, 2, ac);
}
/* o's colour as rgb (b, g, r) has it, its alpha kept; nothing when it has it already */
static void fe_colour_to(CPU *c, uint32_t o, const int32_t rgb[3]){
    int32_t col[4];
    if (fe_colour_get(o, col) && (col[0] != rgb[0] || col[1] != rgb[1] || col[2] != rgb[2])) fe_colour_set(c, o, rgb[0], rgb[1], rgb[2], col[3]);
}
/* the label's FE object, looked up (or made) once per HUD */
static uint32_t tag_obj(CPU *c, int k, uint32_t pkg){
    if (g_tag.looked[k]) return g_tag.obj[k];
    g_tag.looked[k] = 1;
    if (k >= TAG_GAME){
        uint32_t o1 = tag_obj(c, 0, pkg);
        char nm[24]; snprintf(nm, sizeof nm, k == TAG_DUEL ? "ROAM_OUTRUN" : k >= TAG_MP0 ? "ROAM_RACE_%d" : "ROAM_PLAYER_%d",
                              k >= TAG_MP0 ? k - TAG_MP0 + 1 : k + 1);   /* past the HUD's six: copies of the first */
        g_tag.obj[k] = o1 ? fe_clone(c, pkg, o1, nm) : 0;
        if (!g_tag.obj[k] && k == TAG_DUEL) rlog("[roam] f=%ld no copy of ONLINE_PLAYER_1 for the outrun's line of text\n", rh_frame());
        else if (!g_tag.obj[k]) rlog("[roam] f=%ld no copy of ONLINE_PLAYER_1 for name %d: one name fewer over friends' cars\n", rh_frame(), k + 1);
        return g_tag.obj[k];
    }
    char nm[24]; snprintf(nm, sizeof nm, "ONLINE_PLAYER_%d", k + 1);
    uint32_t sa = g_gmem + TAG_SCR + 0x48u;
    gstr(sa, nm, 24);
    uint32_t h = gcall1(c, FN_FE_HASH, 0, sa), a[2] = { pkg, h };
    g_tag.obj[k] = rh_call(c, FN_FE_FIND, 0, 0, 2, a);
    if (!ok(g_tag.obj[k], 0x70)) g_tag.obj[k] = 0;
    if (!g_tag.obj[k]) rlog("[roam] f=%ld no %s in the HUD: one name fewer over friends' cars\n", rh_frame(), nm);
    return g_tag.obj[k];
}
static void tag_text_rgb(CPU *c, int k, uint32_t obj, const char *name, uint32_t rgb){
    if (!strcmp(g_tag.text[k], name) && g_tag.rgb[k] == rgb) return;
    snprintf(g_tag.text[k], sizeof g_tag.text[k], "%s", name);
    g_tag.rgb[k] = rgb;
    uint32_t col = g_gmem + TAG_SCR + 0x20u, fmt = g_gmem + TAG_SCR + 0x40u, txt = g_gmem + TAG_SCR + 0x48u;
    for (uint32_t i = 0; i < 4; i++) MR32(col + 4u * i) = 0xffu;               /* white, as 0x4F80E0 */
    if (rgb){ MR32(col) = rgb & 0xffu; MR32(col + 4u) = rgb >> 8 & 0xffu; MR32(col + 8u) = rgb >> 16 & 0xffu; }   /* (blue, green, red, alpha) */
    uint32_t ac[2] = { col, 0 }; rh_call(c, FN_FE_COLOR, obj, 0, 2, ac);
    MR32(obj + 0x6cu) = (MR32(obj + 0x6cu) & ~0xfu) | 5u;                     /* centred */
    MR32(obj + 0x1cu) |= 0x100u;
    gstr(fmt, "%s", 4); gstr(txt, name, TAG_DUEL_MAX);
    if (k >= TAG_DUEL){
        /* FEPrintf keeps a text within the object's string as it is (20 for
         * ONLINE_PLAYER_1's); the string's own setter, which it calls, grows it */
        gcall1(c, FN_FE_SETSTR, obj, txt);
        MR32(obj + 0x1cu) |= 2u;
        return;
    }
    uint32_t ap[3] = { obj, fmt, txt }; rh_call(c, FN_FE_PRINTF, 0, 0, 3, ap);
}
static void tag_text(CPU *c, int k, uint32_t obj, const char *name){ tag_text_rgb(c, k, obj, name, 0); }
/* the HUD's package is the top one: no screen over it (the world map, the
 * pause menu, a message), which hides the HUD's own parts but not ours */
#define G_FENG         0x008384c4u   /* +8 the package manager: +0xE4 the top package (GetTopPackage 0x50C8E0), +0xC its name */
static int hud_on_top(uint32_t pkg){
    uint32_t fe = rd32(G_FENG), mg = fe ? rd32(fe + 8u) : 0, top = mg ? rd32(mg + 0xe4u) : 0;
    char a[40], b[40];
    cstr(top ? rd32(top + 0xcu) : 0, a, sizeof a); cstr(pkg, b, sizeof b);
    return a[0] && !strcasecmp(a, b);
}
/* Quick chat (the rest after the race lobbies, say_*): players say one of the page's set phrases
 * (web/src/app/phrases.ts), a number on the wire, so each player reads it in their own language and nobody types
 * anything.  Any number goes through: the page knows which it has (a newer page's it does not show).  The page
 * shows a line over the car of each player who said something lately, where the name over it is: tags_update tells
 * it where, with every frame (roam_plat_say_marks).  Not in the Windows plugin (no page). */
#ifdef ROAM_HOST_WIN
#define SAYS            0
#else
#define SAYS            1
#endif
#define SAY_NEAR_M      250.f         /* heard from as far as a horn is (horn.c FAR_M), and from anyone in our race lobby */
#define SAY_SHOW_US     5000000       /* the line over the car: as long as the page shows it */
#define SAY_BURST       3             /* at most this many from one player in SAY_WINDOW_US (the page keeps the player's
                                         own to that): more from a friend are not shown */
#define SAY_WINDOW_US   6000000
#define SAY_AGAIN_US    2000000       /* the same phrase from one friend again within this: not shown */
#define SAY_MARKS       32
/* a friend's: the last phrases shown (when), the last shown (when, until when over the car), the last heard */
typedef struct { uint32_t id; int64_t t[SAY_BURST], at, until; uint8_t seq, phrase, heard; } SayWho;
static struct {
    SayWho   who[RC_TABLE];                               /* by session peer */
    int64_t  mine[SAY_BURST], mine_until; uint8_t seq;
    int      test_pick;                                   /* a script's "mp say N" (-1: none) */
    uint32_t said, heard, dropped;                        /* statistics */
} g_say = { .test_pick = -1 };
static int say_live(int peer, uint32_t id, int64_t now){
    return SAYS && peer >= 0 && peer < RC_TABLE && g_say.who[peer].id == id && g_say.who[peer].until > now;
}
/* a crew's name over a car past TAG_NEAR_M: the distance with it, in steps of 10 m, past 1 km in km */
static void tag_far(char *tx, int cap, const char *name, int paused, float d){
    char ds[16];
    if (d < 995.f) snprintf(ds, sizeof ds, "%d m", (int)(d / 10.f + 0.5f) * 10);
    else snprintf(ds, sizeof ds, "%.1f km", (double)d / 1000.0);
    snprintf(tx, (size_t)cap, paused ? "%s (paused, %s)" : "%s (%s)", name, ds);
}
static long g_tag_self = -9;       /* photo mode's frame of the names (roam_game_names): the player's own too */
static void tags_update(CPU *c){
    uint32_t pl = rd32(G_PLAYER1), hud = pl && ok(pl, PL_HUD + 4u) ? rd32(pl + PL_HUD) : 0;
    uint32_t pkg = hud && ok(hud, 0x14) ? rd32(hud + 0x10u) : 0;
    if (hud != g_tag.hud){                          /* another HUD: its own objects (the old ones went with it) */
        memset(&g_tag, 0, sizeof g_tag);
        g_tag.hud = hud;
    }
    if (!hud || !ok(pkg, 0x20) || !g_gmem) return;
    uint32_t cam = rd32(G_VIEW1 + 0x40u);
    int on = O.labels && g_in_roam && MR8(G_SHOW_NAMES) && ok(cam, 0x60) && hud_on_top(pkg);
    /* the cars a name shows on: in front of the camera, within 100 m (a
     * crew's at any distance) and on the screen; the nearest TAG_N of them
     * get one */
    struct { int k; float d2, hx, hy, z; } t[NSLOT + 1];
    int n = 0;
    Slot self = { 0 };                              /* (k == NSLOT) */
    int with_self = rh_frame() - g_tag_self <= 1 && O.name[0] && ok(player_car(), 0x9e0);
    if (with_self){ snprintf(self.name, sizeof self.name, "%s", O.name); self.car = player_car(); }
    for (int k = 0; on && k < NSLOT + with_self; k++){
        Slot *s = k < NSLOT ? &g_sl[k] : &self;
        if (k < NSLOT && (s->st != SL_ACTIVE || s->hidden || !s->name[0] || !ok(s->car, 0x9e0))) continue;
        uint32_t car = s->car;
        float p[3], u[3], d[3];
        rd3(car + 0x60u, p); rd3(car + 0xb0u, u);
        for (int i = 0; i < 3; i++) d[i] = p[i] - MRF32(cam + 0x40u + 4u * (uint32_t)i);
        float ahead = d[0] * MRF32(cam + 0x50u) + d[1] * MRF32(cam + 0x54u) + d[2] * MRF32(cam + 0x58u);
        float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (ahead < 0.f || !(d2 > 0.f) || !(s->crew || d2 < TAG_NEAR_M * TAG_NEAR_M)) continue;
        uint32_t in = gvec(g_gmem + TAG_SCR, u[0] * 1.7f + p[0], u[1] * 1.7f + p[1], u[2] * 1.7f + p[2]);
        uint32_t out = g_gmem + TAG_SCR + 0x10u, av[2] = { out, in };
        rh_call(c, FN_VIEW_SCREEN, G_VIEW1, 0, 2, av);
        float x = MRF32(out), y = MRF32(out + 4u), z = MRF32(out + 8u);
        float w = (float)(int32_t)MR32(G_VIEW_W), h = (float)(int32_t)MR32(G_VIEW_H);
        if (!(z > 0.f) || !(x >= 0.f) || !(x <= w) || !(y >= 0.f) || !(y <= h)) continue;
        int j = n++;
        for (; j > 0 && t[j - 1].d2 > d2; j--) t[j] = t[j - 1];      /* nearest first */
        t[j].k = k; t[j].d2 = d2; t[j].hx = x / w * 640.f; t[j].hy = y / h * 480.f; t[j].z = z;
    }
    for (int i = 0; i < TAG_N; i++){
        if (i >= n){ tag_hide(c, i); continue; }
        uint32_t obj = tag_obj(c, i, pkg);
        if (!obj) continue;
        const Slot *s = t[i].k < NSLOT ? &g_sl[t[i].k] : &self;
        float hx = t[i].hx, hy = t[i].hy;
        rh_hud_marker(&hx, &hy);
        char tx[TAG_TEXT_MAX];
        float d = rc_sqrtf(t[i].d2);
        if (s->crew && d >= TAG_NEAR_M) tag_far(tx, sizeof tx, s->name, s->paused, d);
        else snprintf(tx, sizeof tx, s->paused ? "%s (paused)" : "%s", s->name);
        tag_text_rgb(c, i, obj, tx, s->crew ? O.crew_rgb[s->crew - 1] : 0);
        /* (a crew's in front of the other names: a crew-mate far behind a crowd still reads) */
        uint32_t ap[2] = { gvec(g_gmem + TAG_SCR + 0x20u, hx - 320.f, hy - 240.f, s->crew ? t[i].z * 0.95f : t[i].z), 0 };
        rh_call(c, FN_FE_POS, obj, 0, 2, ap);
        float sc = 25.f / rc_sqrtf(t[i].d2);
        if (sc < 0.5f) sc = 0.5f;
        if (sc > 1.f) sc = 1.f;
        uint32_t as[2] = { gvec(g_gmem + TAG_SCR + 0x30u, sc, sc, 1.f), 0 };
        rh_call(c, FN_FE_SCALE, obj, 0, 2, as);
        gcall1(c, FN_FE_SHOW, 0, obj);
        g_tag.shown[i] = 1;
    }
#if SAYS
    /* quick chat: where the names of those who said something lately are (the page's lines go over them), and the
     * player's own line over their car, which has no name: just over its roof (1.2 m up; a friend's name is at 1.7),
     * unless the camera is in or right behind the car */
    int64_t now = roam_plat_now_us();
    uint32_t mid[SAY_MARKS]; float mxy[3 * SAY_MARKS]; int nm = 0;
    for (int i = 0; i < n && i < TAG_N && nm < SAY_MARKS; i++){
        if (t[i].k == NSLOT) continue;                     /* (the player's own: below) */
        const Slot *s = &g_sl[t[i].k];
        if (!say_live(s->peer, s->peer_id, now)) continue;
        float sc = 25.f / rc_sqrtf(t[i].d2);
        mid[nm] = s->peer_id; mxy[3 * nm] = t[i].hx / 640.f; mxy[3 * nm + 1] = t[i].hy / 480.f;
        mxy[3 * nm + 2] = sc < 0.5f ? 0.5f : sc > 1.f ? 1.f : sc; nm++;
    }
    uint32_t me = player_car();
    if (on && g_say.mine_until > now && nm < SAY_MARKS && ok(me, 0xc0)){
        float p[3], u[3], d[3];
        rd3(me + 0x60u, p); rd3(me + 0xb0u, u);
        for (int i = 0; i < 3; i++) d[i] = p[i] - MRF32(cam + 0x40u + 4u * (uint32_t)i);
        float ahead = d[0] * MRF32(cam + 0x50u) + d[1] * MRF32(cam + 0x54u) + d[2] * MRF32(cam + 0x58u);
        uint32_t in = gvec(g_gmem + TAG_SCR, u[0] * 1.2f + p[0], u[1] * 1.2f + p[1], u[2] * 1.2f + p[2]);
        uint32_t out = g_gmem + TAG_SCR + 0x10u, av[2] = { out, in };
        if (ahead > 2.f) rh_call(c, FN_VIEW_SCREEN, G_VIEW1, 0, 2, av);
        float x = MRF32(out), y = MRF32(out + 4u), z = MRF32(out + 8u);
        float w = (float)(int32_t)MR32(G_VIEW_W), h = (float)(int32_t)MR32(G_VIEW_H);
        if (ahead > 2.f && z > 0.f && x >= 0.f && x <= w && y >= 0.f && y <= h){
            mid[nm] = g_s.conf.self_id; mxy[3 * nm] = x / w; mxy[3 * nm + 1] = y / h; mxy[3 * nm + 2] = 1.f; nm++; }
    }
    if (nm) roam_plat_say_marks(mid, mxy, nm);
#endif
}

/* ---- minimap arrows --------------------------------------------------------------------------------- */
/* The minimap's icons (G_MM_ICONS, made by 0x4EC950 when the HUD is built
 * and again by 0x4F92A0) give an AI racer's arrow (type 3) to each car of the
 * World's AI range.  An icon keeps the car's racer number and finds the car
 * by it every frame on the World's list (0x4CA6D0 -> 0x5F0E30), and draws
 * one of the free "visuals" of its type, which 0x4DB900 makes from the HUD's
 * AIRACERINDICATOR_0..7 at each rebuild (and frees, 0x4EC6F0); it draws it
 * only while the car is in the world (CAR_ACTIVE).  A shown friend's car
 * without an arrow gets one here: an icon by racer number (0x4EC790); when no
 * visual is free, a visual of our own from the game's pools (it frees them
 * with its own) on a copy of AIRACERINDICATOR_0 (fe_clone) that no visual
 * holds now.  Which cars have one is looked up in the icons every frame: the
 * game makes them again (0x4F92A0, from several places of the career's), and
 * the new icons object is a block of the size just freed, at the old one's
 * address as often as not, so the cars outside the car index lost their
 * arrows for good when their icons were remembered by that address. */
#define G_MM_ICONS      0x008364d4u   /* the minimap's icons: +0/+4 their list (icon +8 racer number, +0x1C its visual), +0x18 the package's name */
#define G_MM_VISUALS    0x00836560u   /* the free visuals: nodes {+0 next, +4 previous, +8 visual {+0 FE object, +4 type}} */
#define G_MM_VISPOOL    0x008364b4u   /* the visuals' pool */
#define FN_MM_ADDICON   0x004ec790u   /* thiscall icons (type, racer number), callee pops: an icon with a free visual of that type */
#define FN_POOL_ALLOC   0x0043f8f0u   /* cdecl (pool) */
#define FN_NODE_ALLOC   0x00440ec0u   /* cdecl (): a list node, +8 its data */
#define MM_AIRACER      3u
#define MM_POOL         256u          /* the minimap's visual and icon pools with free roam on (g_pools) */
static struct { uint32_t hud, copy[NSLOT], hash[NSLOT]; int ncopy; int said, stop;
                int32_t col[NSLOT][3];                                  /* each copy's own colour (b, g, r) */
                uint32_t orig; int vis; } g_mm;                         /* AIRACERINDICATOR_0; its copies are 0 unknown, 1 hidden, 2 shown */
/* the game's own AI racers (the outrun challengers) on the minimap and the
 * world map: grey, so they are not taken for players, whose arrows keep the
 * game's colour (or their crew's) */
#define AI_RGB          0xc8c8c8u     /* a light grey (picked from three, 2026-10-31) */
static uint32_t g_ai_rgb = AI_RGB;            /* (the test aid mp airgb: another grey, for pictures) */
/* the arrows arrow_colours has in a crew's colour or grey, and the colour each had before */
#define MMC_MAX         (NSLOT + 8)
static struct { uint32_t hud; int n; uint32_t obj[MMC_MAX]; int32_t orig[MMC_MAX][3]; } g_mmc;
/* an arrow's own colour (b, g, r): the one it had before a crew's or grey, else the one it has */
static int mm_own_colour(uint32_t o, int32_t rgb[3]){
    int32_t col[4];
    if (!fe_colour_get(o, col)) return 0;
    for (int i = 0; i < 3; i++) rgb[i] = col[i];
    for (int j = 0; g_mmc.hud == g_mm.hud && j < g_mmc.n; j++) if (g_mmc.obj[j] == o) for (int i = 0; i < 3; i++) rgb[i] = g_mmc.orig[j][i];
    return 1;
}
/* a visual (a free one, or one for a copy of AIRACERINDICATOR_0) in the free list */
static int mm_visual_held(uint32_t icons, uint32_t fo){
    for (uint32_t n = rd32(G_MM_VISUALS), m = 0; n && n != G_MM_VISUALS && m < 256; n = rd32(n), m++) if (rd32(rd32(n + 8u)) == fo) return 1;
    for (uint32_t n = rd32(icons), m = 0; n && n != icons && m < 256; n = rd32(n), m++) if (rd32(rd32(n + 0x1cu)) == fo) return 1;
    return 0;
}
/* a copy is the HUD package's object: a HUD made again (after a shop the
 * new one can have the old one's address, and g_mm.hud is the same) has
 * none of ours, they were freed with the old package.  So a copy is taken
 * again only while the package still holds it under its name (a freed one
 * drawn by the minimap faulted in the arrow's rotation, 0x517C80) */
static int mm_copy_live(CPU *c, uint32_t icons, int i){
    uint32_t pk = gcall1(c, FN_FE_PACKAGE, 0, rd32(icons + 0x18u));
    return ok(pk, 0x54) && fe_list_of(pk + 0x4cu, g_mm.copy[i], 0) && rd32(g_mm.copy[i] + 0x10u) == g_mm.hash[i];
}
static int mm_visual(CPU *c, uint32_t icons){
    for (uint32_t n = rd32(G_MM_VISUALS), m = 0; n && n != G_MM_VISUALS && m < 256; n = rd32(n), m++){
        uint32_t v = rd32(n + 8u);
        if (ok(v, 8) && MR32(v + 4u) == MM_AIRACER) return 1;
    }
    int i = 0;
    while (i < g_mm.ncopy && mm_visual_held(icons, g_mm.copy[i])) i++;
    uint32_t fo = i < g_mm.ncopy ? g_mm.copy[i] : 0;
    if (fo && !mm_copy_live(c, icons, i)){
        rlog("[roam] f=%ld minimap: a new HUD at the old one's address: its %d copies of AIRACERINDICATOR_0 forgotten\n", rh_frame(), g_mm.ncopy);
        g_mm.ncopy = 0; g_mm.orig = 0; fo = 0;
    }
    if (!fo){
        if (g_mm.ncopy >= NSLOT) return 0;
        uint32_t pkg = rd32(icons + 0x18u), sa = g_gmem + TAG_SCR + 0x48u;
        gstr(sa, "AIRACERINDICATOR_0", 24);
        uint32_t a[2] = { pkg, gcall1(c, FN_FE_HASH, 0, sa) }, o0 = ok(pkg, 0x10) ? rh_call(c, FN_FE_FINDOBJ, 0, 0, 2, a) : 0;
        char nm[24]; snprintf(nm, sizeof nm, "ROAM_AIRACER_%d", g_mm.ncopy);
        fo = ok(o0, 0x5c) ? fe_clone(c, pkg, o0, nm) : 0;
        if (!fo) return 0;
        i = g_mm.ncopy;
        if (!mm_own_colour(o0, g_mm.col[i])) mm_own_colour(fo, g_mm.col[i]);
        g_mm.hash[i] = MR32(fo + 0x10u); g_mm.copy[g_mm.ncopy++] = fo;
        g_mm.orig = o0; g_mm.vis = 0;                                     /* shown or hidden as the original, next frame */
    }
    /* a copy in its own colour: fe_clone gave it the original's, a crew's
     * while the original is on a crew-mate's arrow (and every car the copy
     * went to was marked as a crew-mate's), grey on an AI racer's; and
     * arrow_colours forgets a copy no visual holds, in a crew's colour or not */
    fe_colour_to(c, fo, g_mm.col[i]);
    uint32_t pool = rd32(G_MM_VISPOOL), v = pool ? rh_call(c, FN_POOL_ALLOC, 0, 0, 1, &pool) : 0, n = v ? rh_call(c, FN_NODE_ALLOC, 0, 0, 0, 0) : 0;
    if (!ok(v, 8) || !ok(n, 12)) return 0;
    MR32(v) = fo; MR32(v + 4u) = MM_AIRACER;
    uint32_t tail = rd32(G_MM_VISUALS + 4u);                              /* at the end, as 0x4DB900 adds them */
    MR32(n + 8u) = v; MR32(tail) = n; MR32(G_MM_VISUALS + 4u) = n; MR32(n + 4u) = tail; MR32(n) = G_MM_VISUALS;
    return 1;
}
static void arrows_update(CPU *c, uint32_t w){
    uint32_t pl = rd32(G_PLAYER1), hud = pl && ok(pl, PL_HUD + 4u) ? rd32(pl + PL_HUD) : 0, icons = rd32(G_MM_ICONS);
    if (hud != g_mm.hud){ memset(&g_mm, 0, sizeof g_mm); g_mm.hud = hud; }      /* our copies went with the old HUD */
    if (!hud || !ok(icons, 0x1c) || !g_gmem) return;
    /* the copies are shown and hidden with the original: the HUD hides its
     * own arrows (the prologue's start, under the world map or the pause
     * menu) and shows them again, but not ours; a copy made while the
     * original was hidden stayed hidden for good.  Only copies the HUD's
     * package still holds are touched (mm_copy_live) */
    if (g_mm.ncopy && ok(g_mm.orig, 0x20)){
        int vis = (MR32(g_mm.orig + 0x1cu) & 1u) ? 1 : 2;
        if (vis != g_mm.vis){
            if (!mm_copy_live(c, icons, 0)){
                rlog("[roam] f=%ld minimap: a new HUD at the old one's address: its %d copies of AIRACERINDICATOR_0 forgotten\n", rh_frame(), g_mm.ncopy);
                g_mm.ncopy = 0; g_mm.orig = 0; g_mm.vis = 0;
            } else {
                for (int i = 0; i < g_mm.ncopy; i++) gcall1(c, vis == 2 ? FN_FE_SHOW : FN_FE_HIDE, 0, g_mm.copy[i]);
                g_mm.vis = vis;
            }
        }
    }
    if (g_mm.stop) return;
    uint8_t has[256] = {0};                                     /* by racer number (a signed byte): an AI racer's arrow */
    uint32_t m = 0;
    for (uint32_t n = rd32(icons); n && n != icons && m < 256 && ok(n, 0x20); n = rd32(n), m++){
        uint32_t v = rd32(n + 0x1cu);
        if (ok(v, 8) && MR32(v + 4u) == MM_AIRACER) has[(uint8_t)MR32(n + 8u)] = 1;
    }
    for (int k = 0; k < NSLOT; k++){
        Slot *s = &g_sl[k];
        if (s->st != SL_ACTIVE || s->hidden || s->world != w || !ok(s->car, 0x9e0)) continue;
        int16_t rn = (int16_t)MR16(s->car + 0xau);
        if (has[(uint8_t)rn]) continue;
        /* far below the icons' pool (MM_POOL; an icon it cannot give is
         * linked in as a null pointer) */
        if (m >= MM_POOL - 16u || !mm_visual(c, icons)){
            if (!g_mm.said++) rlog("[roam] f=%ld no minimap arrow for a friend's car (no AIRACERINDICATOR to copy, or the minimap's pools are full)\n", rh_frame());
            return;
        }
        uint32_t a[2] = { MM_AIRACER, (uint32_t)(int32_t)rn };
        rh_call(c, FN_MM_ADDICON, icons, 0, 2, a);
        m++; has[(uint8_t)rn] = 1;
        /* the icon is the list's last: with no visual of its type it would
         * be added again every frame */
        uint32_t last = rd32(icons + 4u), v = ok(last, 0x20) ? rd32(last + 0x1cu) : 0;
        if (!ok(v, 8) || MR32(v + 4u) != MM_AIRACER || (int16_t)MR32(last + 8u) != rn){
            g_mm.stop = 1;
            rlog("[roam] f=%ld minimap: an arrow made for a friend's car has no visual: no more until the next HUD\n", rh_frame());
            return;
        }
    }
}

/* the crew (1..) of the friend's car shown with this racer number, 0: none */
static int racer_crew(uint32_t w, int16_t rn){
    for (int k = 0; k < NSLOT; k++){
        const Slot *sl = &g_sl[k];
        if (sl->st == SL_ACTIVE && sl->crew && !sl->hidden && sl->world == w && ok(sl->car, 0x9e0) && (int16_t)MR16(sl->car + 0xau) == rn) return sl->crew;
    }
    return 0;
}
/* a friend's car has this racer number, shown or not (a parked opponent car
 * of the Windows plugin's is the game's again) */
static int racer_friend(uint32_t w, int16_t rn){
    for (int k = 0; k < NSLOT; k++){
        const Slot *sl = &g_sl[k];
        if (sl->st != SL_FREE && sl->st != SL_PARKED && sl->world == w && ok(sl->car, 0x9e0) && (int16_t)MR16(sl->car + 0xau) == rn) return 1;
    }
    return 0;
}
/* A crew's arrows in the crew's colour, the AI racers' grey (an arrow of a
 * car no friend has: the outrun challengers'): the arrow's object (its
 * visual's) gets the colour each frame its icon belongs to such a car, and
 * its own colour back once it no longer does (the visuals go from car to
 * car, the friends' and the AI racers'); the alpha is left as it is.  Only
 * objects the HUD still holds are touched: a visual on an icon or in the
 * free list (G_MM_VISUALS); the others went with an old HUD and are
 * forgotten (a copy of ours that no visual holds for a while too: mm_visual
 * gives it its own colour back when it takes it again) */
static void arrow_colours(CPU *c, uint32_t w){
    uint32_t pl = rd32(G_PLAYER1), hud = pl && ok(pl, PL_HUD + 4u) ? rd32(pl + PL_HUD) : 0, icons = rd32(G_MM_ICONS);
    if (hud != g_mmc.hud){ memset(&g_mmc, 0, sizeof g_mmc); g_mmc.hud = hud; }
    if (!hud || !ok(icons, 0x1c) || !g_gmem) return;
    uint8_t used[MMC_MAX] = {0}, live[MMC_MAX] = {0};
    for (uint32_t n = rd32(icons), m = 0; n && n != icons && m < 256 && ok(n, 0x20); n = rd32(n), m++){
        uint32_t v = rd32(n + 0x1cu), o = ok(v, 8) && MR32(v + 4u) == MM_AIRACER ? MR32(v) : 0;
        if (!ok(o, 0x34)) continue;
        int16_t rn = (int16_t)MR32(n + 8u);
        int crew = racer_crew(w, rn), ai = !crew && !racer_friend(w, rn), j = -1;
        for (int i = 0; i < g_mmc.n; i++) if (g_mmc.obj[i] == o) j = i;
        if (j >= 0) live[j] = 1;
        if (!crew && !ai) continue;
        int32_t col[4];
        if (!fe_colour_get(o, col)) continue;
        if (j < 0){
            if (g_mmc.n >= MMC_MAX) continue;
            j = g_mmc.n++; g_mmc.obj[j] = o; live[j] = 1;
            for (int i = 0; i < 3; i++) g_mmc.orig[j][i] = col[i];
        }
        used[j] = 1;
        uint32_t rgb = crew ? O.crew_rgb[crew - 1] : g_ai_rgb;
        int32_t b = (int32_t)(rgb & 0xffu), g = (int32_t)(rgb >> 8 & 0xffu), r = (int32_t)(rgb >> 16 & 0xffu);
        if (col[0] != b || col[1] != g || col[2] != r) fe_colour_set(c, o, b, g, r, col[3]);
    }
    for (uint32_t n = rd32(G_MM_VISUALS), m = 0; n && n != G_MM_VISUALS && m < 256; n = rd32(n), m++){
        uint32_t v = rd32(n + 8u), o = ok(v, 8) ? MR32(v) : 0;
        for (int i = 0; o && i < g_mmc.n; i++) if (g_mmc.obj[i] == o) live[i] = 1;
    }
    for (int j = g_mmc.n - 1; j >= 0; j--){
        if (used[j]) continue;
        int32_t col[4];
        if (live[j] && fe_colour_get(g_mmc.obj[j], col)) fe_colour_set(c, g_mmc.obj[j], g_mmc.orig[j][0], g_mmc.orig[j][1], g_mmc.orig[j][2], col[3]);
        g_mmc.n--;                                                     /* (the last one in its place) */
        g_mmc.obj[j] = g_mmc.obj[g_mmc.n]; memcpy(g_mmc.orig[j], g_mmc.orig[g_mmc.n], sizeof g_mmc.orig[j]);
        used[j] = used[g_mmc.n]; live[j] = live[g_mmc.n];
    }
}

/* ---- the world map ---------------------------------------------------------------------------------- */
/* The world map (M, or the pause menu's Map: InGameWorldMapScreen, 0x4EE320,
 * for UI_InGame_WorldMap.fng) is made when it opens.  Its WorldMap (screen
 * +0x4C, 0x4E0440) makes a marker for each car of the World's AI range that
 * is in the world (0x4B0740: type 0xE, the package's AIRACERINDICATOR_<n> by
 * a running count, and the car's position and heading then) and places every
 * marker on the map (0x4B09E0, again at each zoom).  Friends' cars in the AI
 * range get theirs that way; the ones outside it get theirs here, the first
 * frame the map is open: the same marker, in the same list.  The package has
 * AIRACERINDICATOR_0..2 only (the outrun challengers' three cars), so every
 * car marker past them, the game's own for a friend's car in the AI range or
 * one of ours, has no object; each gets a copy of the first (fe_clone), and
 * the map is placed again.  The game's own marker stays where the car was
 * when the map opened; while the map is open, friends' cars keep driving,
 * and their markers are moved to where the cars are WM_STEP_US (wm_live).
 *
 * The players in free roam we have no car for (the hub links us to the 32
 * nearest at most) get the same marker too: the map asks the hub for them
 * as it opens (rc_ask_map: the nearest past our peers, RC_MAP_MAX or as many
 * as the map has room for, where they were in the last half second, how
 * they move, each by the hub's key for it), and each one's marker is made
 * from a stand-in for a car when the answer comes (0x4B0740 reads only the
 * car's +0x60/+0x64 and its heading +0x40C), a frame or a few later.  A
 * quarter second after each answer it asks again, and the hub answers with
 * what changed (the players come, gone, or off where we moved them): a
 * player keeps its marker, one come takes a marker left over or a new one,
 * and WM_STEP_US each marker is moved on at its player's velocity from where
 * the hub last placed it (wm_step); a player placed again goes from where
 * its marker was to there over WM_BLEND_US, not at once.
 *
 * Each marker is one more object of the package, and FEng draws a package's
 * objects through two arrays of a fixed size: the renderer's list of the
 * objects to draw (0x51A970 adds each one whose colour has any alpha, hidden
 * or not: renderer +0x110 the count, +0x114 1024 entries; the radix sort
 * 0x5114F0 takes the 0x2000 bytes after them as its other half, so more
 * would garble both) and the level sorter's nodes (0x51B850, one for each
 * object drawn: 512 in the game, RH_FE_NODES here; an object past them is
 * not drawn).  The map has 235 objects of its own (173 drawn at the new
 * career's start), so 512 markers more took the nodes from its shop and
 * event icons and the player's own arrow.  On the port both take
 * FE_LIST_MAX (overrides.c moves the list past its 1024 to one of its own).
 * The markers stay within both (wm_room): the package's objects are counted
 * first, all of them (the zoom shows and hides markers), and the farthest
 * players are left out. */
#define WM_PACKAGE      "UI_InGame_WorldMap.fng"
#define VT_WM_SCREEN    0x00797f20u   /* InGameWorldMapScreen's vtable; +4 its package's name */
#define WM_MAP          0x4cu         /* the screen's WorldMap: +4/+8 its markers' list (+0 next, +4 previous) */
#define WM_MARK_SZ      0x2cu         /* a marker: +8 type, +0xC its FE object, +0x10/+0x14 where, +0x18 heading */
#define WM_CAR          0xeu          /* a car's marker (the player's is 0x10) */
#define FN_WM_CARMARK   0x004b0740u   /* thiscall marker (package name, car), callee pops */
#define FN_WM_PLACE     0x004b09e0u   /* thiscall WorldMap () */
#define G_WM_COUNT      0x00835f70u   /* the car markers' running count: AIRACERINDICATOR_<n> */
#define PKG_SCREEN      0x40u         /* FEPackage: +0x40 an object whose +0x14 is the package's screen */
#define WM_ASK_US       1000000       /* no answer from the hub in this time: asked again, WM_ASKS times at most */
#define WM_ASKS         3             /* ...until its first answer (a hub before the map never answers) */
#define WM_LIVE_US      250000        /* asked again this long after each answer (the hub answers a browser every quarter second) */
#define WM_STEP_US      33333         /* the open map's markers moved this often */
#define WM_STALE_US     2000000       /* the hub's players' markers moved on this long past its last answer at most */
#define WM_BLEND_US     400000        /* a player placed again: its marker comes to its new place in this time */
#define G_WM_HEADING_K  0x00784240u   /* float: a car's heading (+0x40C, 65536 a turn) to a marker's (0x4B0740) */
#ifdef ROAM_HOST_WIN
#define FE_LIST_MAX     1024          /* the FE renderer's list of a package's objects to draw */
#else
#define FE_LIST_MAX     4096          /* ...past its 1024 in a list of overrides.c's */
#endif
#define WM_SPARE        16            /* objects left to the package itself */
static struct { int done, said, copies, asks, others, lobbies, nf, steps; uint32_t map, gen, fake; int64_t asked, got, moved, step_us;
                int col_ok; int32_t col[3]; } g_wm;                   /* AIRACERINDICATOR_0's colour as the map opened (b, g, r) */
static rc_mapent g_wm_e[RC_MAP_MAX], g_wm_in[RC_MAP_MAX];           /* the hub's players (wm_n of them), by key; its answer */
static int16_t g_wm_k[RC_MAP_MAX], g_wm_kin[RC_MAP_MAX];             /* ...each one's marker, -1 none */
static struct { float x, y; int64_t at; } g_wm_o[RC_MAP_MAX], g_wm_oin[RC_MAP_MAX];   /* ...where its marker was off its new place, when */
static int g_wm_n;
static struct { uint32_t m; int w; uint8_t used; } g_wm_m[RC_MAP_MAX];   /* the hub's players' markers (others of them), their filter entries */
static struct { uint32_t m, id; int k, w; } g_wm_f[NSLOT];           /* the friends' cars' markers (nf): the slot and its friend, the filter entry */
/* the open map's screen, 0 while it is not made (or not this screen) */
static uint32_t wm_screen(uint32_t pk){
    uint32_t h = rd32(pk + PKG_SCREEN), s = ok(h, 0x18) ? rd32(h + 0x14u) : 0;
    return ok(s, WM_MAP + 4u) && MR32(s) == VT_WM_SCREEN ? s : 0;
}
static uint32_t wm_find(CPU *c, uint32_t pkg, const char *name){
    uint32_t sa = g_gmem + TAG_SCR + 0x48u;
    gstr(sa, name, 24);
    uint32_t a[2] = { pkg, gcall1(c, FN_FE_HASH, 0, sa) }, o = rh_call(c, FN_FE_FINDOBJ, 0, 0, 2, a);
    return ok(o, 0x5c) ? o : 0;
}
/* a car marker at the end of the map's list (as 0x4E0440 adds them) for this car; the marker, 0 when not made */
static uint32_t wm_mark(CPU *c, uint32_t map, uint32_t pkg, uint32_t car){
    uint32_t sz = WM_MARK_SZ, m = rh_call(c, FN_MALLOC, 0, 0, 1, &sz);
    if (!ok(m, WM_MARK_SZ)) return 0;
    uint32_t a[2] = { pkg, car };
    rh_call(c, FN_WM_CARMARK, m, 0, 2, a);
    uint32_t head = map + 4u, tail = rd32(head + 4u);
    MR32(tail) = m; MR32(head + 4u) = m; MR32(m + 4u) = tail; MR32(m) = head;
    return m;
}
/* the objects of a package FEng could draw: every one in its list and its
 * groups' (type 5: +0x64 the first), groups themselves aside */
static int fe_objects(uint32_t first, int depth){
    int n = 0, k = 0;
    for (uint32_t o = rd32(first); ok(o, 0x5c) && k < 4096; o = rd32(o + 4u), k++)
        n += rd32(o + 0x18u) != 5u ? 1 : depth < 8 ? fe_objects(o + 0x64u, depth + 1) : 0;
    return n;
}
/* a package's string objects in the log (mp map strings): their hashes and texts */
static void fe_strings(uint32_t first, int depth){
    int k = 0;
    for (uint32_t o = rd32(first); ok(o, 0x5c) && k < 4096; o = rd32(o + 4u), k++){
        uint32_t t = rd32(o + 0x18u);
        if (t == 5u && depth < 8){ fe_strings(o + 0x64u, depth + 1); continue; }
        if (t != 2u) continue;
        char tx[48]; uint32_t w = rd32(o + 0x64u), n = 0;
        for (; ok(w, 2) && n + 1 < sizeof tx && MR16(w + 2u * n); n++) tx[n] = (char)MR16(w + 2u * n);
        tx[n] = 0;
        uint32_t d = rd32(o + 0x2cu);
        rlog("[roam]   string 0x%08x hash %08x flags %08x depth %d at %.1f %.1f %.1f colour %08x %08x %08x %08x: %s\n", o, rd32(o + 0x10u), rd32(o + 0x1cu), depth,
             ok(d, 0x40) ? (double)MRF32(d + 0x1cu) : 0., ok(d, 0x40) ? (double)MRF32(d + 0x20u) : 0., ok(d, 0x40) ? (double)MRF32(d + 0x24u) : 0.,
             ok(d, 0x40) ? MR32(d) : 0u, ok(d, 0x40) ? MR32(d + 4u) : 0u, ok(d, 0x40) ? MR32(d + 8u) : 0u, ok(d, 0x40) ? MR32(d + 12u) : 0u, tx);
    }
}
/* a package's objects of any kind left of x0 in the log (mp map parts): kind, hash, place, size, flags */
static void fe_parts(uint32_t first, int depth, float x0){
    int k = 0;
    for (uint32_t o = rd32(first); ok(o, 0x5c) && k < 4096; o = rd32(o + 4u), k++){
        uint32_t t = rd32(o + 0x18u), d = rd32(o + 0x2cu);
        if (t == 5u && depth < 8) fe_parts(o + 0x64u, depth + 1, x0);
        if (!ok(d, 0x40) || MRF32(d + 0x1cu) > x0) continue;
        rlog("[roam]   part 0x%08x kind %u hash %08x flags %08x depth %d at %.1f %.1f %.1f size %.1f %.1f colour %08x\n", o, t, rd32(o + 0x10u), rd32(o + 0x1cu), depth,
             (double)MRF32(d + 0x1cu), (double)MRF32(d + 0x20u), (double)MRF32(d + 0x24u), (double)MRF32(d + 0x38u), (double)MRF32(d + 0x3cu), MR32(d + 12u));
    }
}
/* how many objects more the map's package can take */
static int wm_room(uint32_t pk){
    int room = (RH_FE_NODES < FE_LIST_MAX ? RH_FE_NODES : FE_LIST_MAX) - WM_SPARE - fe_objects(pk + 0x4cu, 0);
    return room > 0 ? room : 0;
}
/* a copy of AIRACERINDICATOR_0 for each car marker without an object, at most max */
static int wm_dress(CPU *c, uint32_t map, uint32_t pkg, int max){
    uint32_t o0 = 0; int copies = 0;
    for (uint32_t e = rd32(map + 4u), i = 0; e && e != map + 4u && i < 4096 && copies < max && ok(e, WM_MARK_SZ); e = rd32(e), i++){
        if (MR32(e + 8u) != WM_CAR || MR32(e + 0xcu)) continue;
        char nm[24]; snprintf(nm, sizeof nm, "ROAM_MAPCAR_%d", g_wm.copies);
        if (!o0) o0 = wm_find(c, pkg, "AIRACERINDICATOR_0");
        uint32_t cl = o0 ? fe_clone(c, pkg, o0, nm) : 0;
        if (!cl){
            if (!g_wm.said++) rlog("[roam] f=%ld world map: no copy of AIRACERINDICATOR_0: some players not marked\n", rh_frame());
            break;
        }
        /* in the marker's own colour: the hub's players are marked after
         * crew_map_open, and AIRACERINDICATOR_0 can be a crew-mate's marker
         * by then, in the crew's colour (every one of them was), or an AI
         * racer's, grey (ai_map_open) */
        if (g_wm.col_ok) fe_colour_to(c, cl, g_wm.col);
        MR32(e + 0xcu) = cl; copies++; g_wm.copies++;
    }
    return copies;
}
#if MPRACES
static int mp_map_icons(CPU *c, uint32_t map, uint32_t pkg, uint32_t room, int hub);   /* lobbies on the map */
static void mp_map_labels(CPU *c, uint32_t map);
#endif
/* A crew's car markers on the world map in the crew's colour, the name
 * beside each (a copy of the legend's string, as the race lobbies' labels).
 * A marker is the car's by its place: made from the car as the map opened
 * (0x4B0740: +0x10/+0x14 the car's +0x60/+0x64), and the world stands while
 * the map is open.  Each frame the names follow their markers, hidden with
 * them (crew_map_labels) */
#define WMC_MAX         8
#define WMC_TEXT        "OPPONENT_CAR"   /* the legend's string ("Opponent Car") */
static struct { uint32_t map; int n; uint32_t m[WMC_MAX], lbl[WMC_MAX]; } g_wmc;
static void crew_map_open(CPU *c, uint32_t map, uint32_t pkg, uint32_t pk, uint32_t w){
    memset(&g_wmc, 0, sizeof g_wmc); g_wmc.map = map;
    int any = 0;
    for (int k = 0; k < NSLOT; k++) any |= g_sl[k].st == SL_ACTIVE && g_sl[k].crew;
    if (!any || !g_gmem) return;
    uint32_t base = wm_find(c, pkg, WMC_TEXT);
    for (uint32_t e = rd32(map + 4u), i = 0; e && e != map + 4u && i < 4096 && ok(e, WM_MARK_SZ); e = rd32(e), i++){
        uint32_t o = MR32(e + 0xcu);
        if (MR32(e + 8u) != WM_CAR || !ok(o, 0x34)) continue;
        float x = MRF32(e + 0x10u), y = MRF32(e + 0x14u);
        const Slot *sl = 0;
        for (int k = 0; k < NSLOT && !sl; k++){
            const Slot *q = &g_sl[k];
            if (q->st != SL_ACTIVE || !q->crew || q->hidden || q->world != w || !ok(q->car, 0x9e0)) continue;
            float dx = MRF32(q->car + 0x60u) - x, dy = MRF32(q->car + 0x64u) - y;
            if (dx * dx + dy * dy < 1.f) sl = q;
        }
        if (!sl) continue;
        uint32_t rgb = O.crew_rgb[sl->crew - 1];
        int32_t col[4];
        if (fe_colour_get(o, col)) fe_colour_set(c, o, (int32_t)(rgb & 0xffu), (int32_t)(rgb >> 8 & 0xffu), (int32_t)(rgb >> 16 & 0xffu), col[3]);
        if (g_wmc.n >= WMC_MAX) continue;
        char ln[24]; snprintf(ln, sizeof ln, "ROAM_CREWTXT_%d", g_wmc.n);
        uint32_t lbl = base && wm_room(pk) > 0 ? fe_clone(c, pkg, base, ln) : 0;
        if (lbl){
            uint32_t txt = g_gmem + TAG_SCR + 0x48u;
            gstr(txt, sl->name, TAG_DUEL_MAX);
            gcall1(c, FN_FE_SETSTR, lbl, txt);
            MR32(lbl + 0x1cu) |= 2u;
            fe_colour_set(c, lbl, (int32_t)(rgb & 0xffu), (int32_t)(rgb >> 8 & 0xffu), (int32_t)(rgb >> 16 & 0xffu), 0xff);
            gcall1(c, FN_FE_HIDE, 0, lbl);
        } else rlog("[roam] f=%ld world map: no name for %s's marker (%s 0x%08x)\n", rh_frame(), sl->name, WMC_TEXT, base);
        g_wmc.m[g_wmc.n] = e; g_wmc.lbl[g_wmc.n] = lbl; g_wmc.n++;
    }
    if (g_wmc.n) rlog("[roam] f=%ld world map: %d crew marker(s) in their colours\n", rh_frame(), g_wmc.n);
}
static void crew_map_labels(CPU *c, uint32_t map){
    if (g_wmc.map != map) return;
    float px[WMC_MAX], py[WMC_MAX]; int placed = 0;
    for (int i = 0; i < g_wmc.n; i++){
        uint32_t e = g_wmc.m[i], lbl = g_wmc.lbl[i], o = ok(e, WM_MARK_SZ) ? rd32(e + 0xcu) : 0;
        if (!lbl || !ok(o, 0x20)) continue;
        if (MR32(o + 0x1cu) & 1u){ gcall1(c, FN_FE_HIDE, 0, lbl); continue; }
        /* crew-mates near each other: their names one under another, not over each other */
        float x = MRF32(e + 0x1cu) + 10.f, y = MRF32(e + 0x20u);
        for (int again = 1, k = 0; again && k < WMC_MAX; k++){
            again = 0;
            for (int j = 0; j < placed; j++) if (__builtin_fabsf(px[j] - x) < 70.f && __builtin_fabsf(py[j] - y) < 13.f){ y = py[j] + 13.f; again = 1; }
        }
        px[placed] = x; py[placed] = y; placed++;
        uint32_t ap[2] = { gvec(g_gmem + TAG_SCR + 0x20u, x, y, 7.f), 0 };
        rh_call(c, FN_FE_POS, lbl, 0, 2, ap);
        gcall1(c, FN_FE_SHOW, 0, lbl);
    }
}
/* The AI racers' car markers grey, as their minimap arrows (arrow_colours):
 * the game's markers (0x4E0440, as the map opens) of the World's AI range's
 * cars no friend has, each found by its place; the legend's Opponent Car
 * arrow with them */
#define WM_OPP_ARROW    0x7e34c30fu   /* the legend's Opponent Car line's arrow */
static int slot_car(uint32_t car){
    for (int k = 0; k < NSLOT; k++) if (g_sl[k].st != SL_FREE && g_sl[k].st != SL_PARKED && g_sl[k].car == car) return 1;
    return 0;
}
static void ai_map_open(CPU *c, uint32_t map, uint32_t pkg, uint32_t w){
    int32_t b = (int32_t)(g_ai_rgb & 0xffu), g = (int32_t)(g_ai_rgb >> 8 & 0xffu), r = (int32_t)(g_ai_rgb >> 16 & 0xffu), col[4];
    int n = 0;
    for (uint32_t e = rd32(map + 4u), i = 0; e && e != map + 4u && i < 4096 && ok(e, WM_MARK_SZ); e = rd32(e), i++){
        uint32_t o = MR32(e + 0xcu);
        if (MR32(e + 8u) != WM_CAR || !ok(o, 0x34)) continue;
        float x = MRF32(e + 0x10u), y = MRF32(e + 0x14u);
        for (int k = 0; k < WORLD_INDEX; k++){
            uint32_t car = ai_car(w, k);
            if (!car || !MR8(car + CAR_ACTIVE) || slot_car(car)) continue;
            float dx = MRF32(car + 0x60u) - x, dy = MRF32(car + 0x64u) - y;
            if (dx * dx + dy * dy >= 1.f) continue;
            if (fe_colour_get(o, col)){ fe_colour_set(c, o, b, g, r, col[3]); n++; }
            break;
        }
    }
    uint32_t a[2] = { pkg, WM_OPP_ARROW }, ar = rh_call(c, FN_FE_FINDOBJ, 0, 0, 2, a);
    if (ok(ar, 0x5c) && fe_colour_get(ar, col)) fe_colour_set(c, ar, b, g, r, col[3]);
    if (n) rlog("[roam] f=%ld world map: %d AI racer marker(s) grey\n", rh_frame(), n);
}
/* The map's filter: two rows of our own in its legend (the list on its left,
 * a row for each kind of place on the map: Up and Down pick one, Enter
 * crosses its tick and the map hides that kind), "Players" for the other
 * players' cars (a crew-mate's stays) and "Players' races" for the race
 * lobbies' icons and labels (the one the player is in stays).  The legend
 * (the screen's +0x54, made with the map by 0x4EE320) is a list of rows of
 * 0x2C bytes, the last made first (Down walks back along it): +8 the tick,
 * +0xC the icon, +0x10 a second icon, +0x14 the text (the package's
 * DATA_A_<n>, DATA_B_<n>, DATA_D_<n>, DATA_C_<n>, n the row from 1), +0x18
 * and +0x1C the tick's colours, +0x20 the kind of place, +0x24 1 crossed,
 * +0x28 the package's name.  The package has 14 rows; a career takes one a
 * kind it has (12 at stage 3, all 14 once it has every kind), ours the next
 * two, or with no room for them the two lines above the rows (wmf_top_parts).
 * Their kinds are past the game's 14, which its show flags take no notice of
 * (0x4964D0 counts them hidden, 0x496390 sets nothing): Enter only crosses
 * the tick (0x4B0EF0) and a pick never jumps to a marker of the kind
 * (0x4B0E70).  The ticks are read every frame.  A marker hidden is moved off the map: the map's placing
 * (0x4B09E0) hides a marker out of its view, and it shows a car's whatever
 * its filter says.  The choices are kept in the game's registry. */
#define WMF_ROWS        14            /* the legend's rows in the package */
#define WMF_ROW_SZ      0x2cu
#define WMF_SCR_ROWS    0x54u         /* the screen: its legend (+0 the first row, +4 the last) */
#define WMF_SCR_BUTTONS 0x50u         /* the screen: its buttons' list (+4 first, +8 last; 0x14 bytes: +8 1 a row's, +0xC ICON_BACKING_<n>, +0x10 the row) */
#define WMF_KIND0       0x20u         /* our rows' kinds: Players, Players' races (the game's are 0 .. 13) */
#define FN_WM_ROWLOOK   0x00496920u   /* thiscall row (crossed): the tick or the cross, the icon and the text dimmed or not */
#define FN_FE_TEX       0x0050c980u   /* cdecl (image) -> its texture's hash */
#define FN_FE_SETTEX    0x0050c990u   /* cdecl (image, texture hash) */
#define WMF_RACES_RGB   0xffc832u     /* Players' races' icon: the lobbies' labels' gold (MP_RGB_LOBBY) */
#define WMF_AWAY        1.0e5f        /* a hidden marker's place: off every map */
#define WMF_MAX         (NSLOT + RC_MAP_MAX + 32)
#define WMF_PLAYERS     0
#define WMF_RACES       1
#if MPRACES
#define WMF_N           2
#else
#define WMF_N           1
#endif
#ifndef ROAM_HOST_WIN
int  win32_reg_get_dword(const char *nm, uint32_t *out);
void win32_reg_put_dword(const char *nm, uint32_t v);
static const char *const wmf_reg[2] = { "MapPlayers", "MapRaces" };   /* (the Windows plugin keeps them a session) */
#endif
static const char *const wmf_text[2] = { "Players", "Players' races" };
static int g_wmf_on[2] = { 1, 1 }, g_wmf_loaded;
static struct { uint32_t map, row[2]; int n; uint32_t m[WMF_MAX]; float x[WMF_MAX], y[WMF_MAX]; uint8_t what[WMF_MAX]; } g_wmf;
static void wmf_load(void){
    if (g_wmf_loaded) return;
    g_wmf_loaded = 1;
#ifndef ROAM_HOST_WIN
    for (int i = 0; i < 2; i++){ uint32_t v; if (win32_reg_get_dword(wmf_reg[i], &v)) g_wmf_on[i] = v != 0; }
#endif
}
/* marker m of the open map is a player's (what WMF_PLAYERS) or a lobby's
 * (WMF_RACES): off the map while its row is crossed (placed again by the
 * caller); its entry, -1 when it has none */
static int wmf_track(uint32_t map, uint32_t m, int what){
    if (g_wmf.map != map || g_wmf.n >= WMF_MAX || !ok(m, WM_MARK_SZ)) return -1;
    int i = g_wmf.n++;
    g_wmf.m[i] = m; g_wmf.x[i] = MRF32(m + 0x10u); g_wmf.y[i] = MRF32(m + 0x14u); g_wmf.what[i] = (uint8_t)what;
    if (!g_wmf_on[what]){ MRF32(m + 0x10u) = WMF_AWAY; MRF32(m + 0x14u) = WMF_AWAY; }
    return i;
}
/* marker m (its entry i, -1: none) to x, y (WMF_AWAY: off the map, whatever
 * the row), heading h (a car's, 65536 a turn); placed again by the caller */
static void wmf_move(uint32_t m, int i, float x, float y, uint16_t h){
    if (i >= 0 && i < g_wmf.n && g_wmf.m[i] == m){
        g_wmf.x[i] = x; g_wmf.y[i] = y;
        if (!g_wmf_on[g_wmf.what[i]]) x = y = WMF_AWAY;
    }
    MRF32(m + 0x10u) = x; MRF32(m + 0x14u) = y; MRF32(m + 0x18u) = (float)h * MRF32(G_WM_HEADING_K);
}
/* our row of the legend (as 0x4B0D50 makes the game's, from its row like):
 * its parts o (tick, icon, second icon, text), its backing bk; in front of
 * the list (after the game's rows: Down reaches it last) or at its end (top:
 * above them, Up from the first row reaches it) */
static uint32_t wmf_row(CPU *c, uint32_t scr, uint32_t pkg, const uint32_t o[4], uint32_t bk, int top, int what, uint32_t like, uint32_t tex, uint32_t rgb){
    uint32_t sz = WMF_ROW_SZ, r = rh_call(c, FN_MALLOC, 0, 0, 1, &sz);
    if (!ok(r, WMF_ROW_SZ)) return 0;
    for (uint32_t i = 0; i < WMF_ROW_SZ; i += 4) MR32(r + i) = 0;
    for (int i = 0; i < 4; i++) MR32(r + 8u + 4u * (uint32_t)i) = o[i];
    MR32(r + 0x18u) = MR32(like + 0x18u); MR32(r + 0x1cu) = MR32(like + 0x1cu);
    MR32(r + 0x20u) = WMF_KIND0 + (uint32_t)what; MR32(r + 0x28u) = pkg;
    uint32_t a[2] = { o[1], tex };
    if (tex) rh_call(c, FN_FE_SETTEX, 0, 0, 2, a);
    fe_colour_set(c, o[1], (int32_t)(rgb & 0xffu), (int32_t)(rgb >> 8 & 0xffu), (int32_t)(rgb >> 16 & 0xffu), 0xff);
    uint32_t txt = g_gmem + TAG_SCR + 0x48u;
    gstr(txt, wmf_text[what], 24);
    gcall1(c, FN_FE_SETSTR, o[3], txt);
    MR32(o[3] + 0x1cu) |= 2u;
    gcall1(c, FN_FE_SHOW, 0, o[0]); gcall1(c, FN_FE_SHOW, 0, o[1]); gcall1(c, FN_FE_SHOW, 0, o[3]); gcall1(c, FN_FE_HIDE, 0, o[2]);
    uint32_t head = scr + WMF_SCR_ROWS;
    if (!top){ uint32_t first = rd32(head); MR32(r) = first; MR32(r + 4u) = head; MR32(head) = r; MR32(first + 4u) = r; }
    else { uint32_t last = rd32(head + 4u); MR32(r) = head; MR32(r + 4u) = last; MR32(last) = r; MR32(head + 4u) = r; }
    uint32_t crossed = !g_wmf_on[what];
    rh_call(c, FN_WM_ROWLOOK, r, 0, 1, &crossed);
    /* its button (the backing a pointer picks the row by), at the end of the screen's */
    uint32_t l = rd32(scr + WMF_SCR_BUTTONS), bsz = 0x14u, b = bk && ok(l, 0xc) ? rh_call(c, FN_MALLOC, 0, 0, 1, &bsz) : 0;
    if (ok(b, 0x14)){
        uint32_t tail = rd32(l + 8u);
        MR32(b) = l + 4u; MR32(b + 4u) = tail; MR32(b + 8u) = 1; MR32(b + 0xcu) = bk; MR32(b + 0x10u) = r;
        MR32(tail) = b; MR32(l + 8u) = b;
    }
    return r;
}
/* the parts of the package's row n (DATA_A_<n> ...), its backing in *bk; 0 when one is missing */
static int wmf_parts(CPU *c, uint32_t pkg, int n, uint32_t o[4], uint32_t *bk){
    static const char *const part[5] = { "DATA_A_%d", "DATA_B_%d", "DATA_D_%d", "DATA_C_%d", "ICON_BACKING_%d" };
    for (int i = 0; i < 5; i++){
        char nm[24]; snprintf(nm, sizeof nm, part[i], n);
        uint32_t x = wm_find(c, pkg, nm);
        if (!x && i < 4) return 0;
        if (i < 4) o[i] = x; else *bk = x;
    }
    return 1;
}
/* A career with too many kinds of places for two rows more: ours take the
 * places of the legend's two lines above its rows, Opponent Car's (Players)
 * and Your Car's (Players' races): copies of row 1's parts moved there, the
 * lines' arrows and words hidden, their backings our buttons */
static int wmf_top_parts(CPU *c, uint32_t pkg, int slot, uint32_t o[4], uint32_t *bk){
    static const uint32_t arrow[2] = { WM_OPP_ARROW, 0x83f0594bu },  /* the lines' arrows (y -137, -159) */
                          text[2]  = { 0x26008b07u, 0xb98d6801u },  /* their words: OPPONENT_CAR, "Your Car" */
                          back[2]  = { 0x4a502afdu, 0xb3e8fa39u };  /* their backings */
    uint32_t r1[4], bk1 = 0;
    if (!wmf_parts(c, pkg, 1, r1, &bk1)) return 0;
    uint32_t a[2] = { pkg, arrow[slot] }, ar = rh_call(c, FN_FE_FINDOBJ, 0, 0, 2, a);
    a[1] = text[slot]; uint32_t tx = rh_call(c, FN_FE_FINDOBJ, 0, 0, 2, a);
    a[1] = back[slot]; *bk = rh_call(c, FN_FE_FINDOBJ, 0, 0, 2, a);
    uint32_t da = ok(ar, 0x5c) ? rd32(ar + 0x2cu) : 0, d0 = rd32(r1[0] + 0x2cu);
    if (!ok(da, 0x40) || !ok(d0, 0x40) || !ok(tx, 0x5c)) return 0;
    float dy = MRF32(da + 0x20u) - MRF32(d0 + 0x20u);                    /* row 1's tick to the line's arrow */
    for (int i = 0; i < 4; i++){
        char nm[24]; snprintf(nm, sizeof nm, "ROAM_LEGEND_%d_%d", slot, i);
        uint32_t d1 = rd32(r1[i] + 0x2cu);
        if (!ok(d1, 0x40) || !(o[i] = fe_clone(c, pkg, r1[i], nm))) return 0;
        uint32_t ap[2] = { gvec(g_gmem + TAG_SCR + 0x20u, MRF32(d1 + 0x1cu), MRF32(d1 + 0x20u) + dy, MRF32(d1 + 0x24u)), 0 };
        rh_call(c, FN_FE_POS, o[i], 0, 2, ap);
    }
    gcall1(c, FN_FE_HIDE, 0, ar); gcall1(c, FN_FE_HIDE, 0, tx);
    return 1;
}
/* as the map opens: our rows after the game's, and the players' markers it has */
static void wmf_begin(uint32_t map){
    wmf_load();
    memset(&g_wmf, 0, sizeof g_wmf); g_wmf.map = map;
}
static void wmf_open(CPU *c, uint32_t scr, uint32_t map, uint32_t pkg, uint32_t w){
    int rows = 0; uint32_t like = 0;
    for (uint32_t r = rd32(scr + WMF_SCR_ROWS); r && r != scr + WMF_SCR_ROWS && rows < 64 && ok(r, WMF_ROW_SZ); r = rd32(r), rows++) like = r;
    int top = rows + WMF_N > WMF_ROWS || env_int("NFSU2_ROAM_TEST_LEGEND_TOP", 0, 0, 1);   /* (test aid: as a full legend) */
    if (like){
        uint32_t car = wm_find(c, pkg, "AIRACERINDICATOR_0"), ring = wm_find(c, pkg, "SPRINTEVENTINDICATOR_0");
        uint32_t rgb = g_wm.col_ok ? (uint32_t)(g_wm.col[2] & 0xff) << 16 | (uint32_t)(g_wm.col[1] & 0xff) << 8 | (uint32_t)(g_wm.col[0] & 0xff) : 0xffffffu;
        uint32_t tex[2] = { car ? gcall1(c, FN_FE_TEX, 0, car) : 0, ring ? gcall1(c, FN_FE_TEX, 0, ring) : 0 }, col[2] = { rgb, WMF_RACES_RGB };
        for (int i = 0; i < WMF_N; i++){
            uint32_t o[4], bk = 0;
            if (top ? wmf_top_parts(c, pkg, i, o, &bk) : wmf_parts(c, pkg, rows + 1 + i, o, &bk))
                g_wmf.row[i] = wmf_row(c, scr, pkg, o, bk, top, i, like, tex[i], col[i]);
        }
    }
    /* the friends' cars (a crew-mate's aside): the game's markers for those in
     * the AI range and ours, each where its car was as the map opened; all
     * of them moved with their cars from then on (wm_live) */
    g_wm.nf = 0;
    for (uint32_t e = rd32(map + 4u), i = 0; e && e != map + 4u && i < 4096 && ok(e, WM_MARK_SZ); e = rd32(e), i++){
        if (MR32(e + 8u) != WM_CAR) continue;
        float x = MRF32(e + 0x10u), y = MRF32(e + 0x14u);
        for (int k = 0; k < NSLOT; k++){
            const Slot *q = &g_sl[k];
            if (q->st != SL_ACTIVE || q->hidden || q->world != w || !ok(q->car, 0x9e0)) continue;
            float dx = MRF32(q->car + 0x60u) - x, dy = MRF32(q->car + 0x64u) - y;
            if (dx * dx + dy * dy >= 1.f) continue;
            int f = q->crew ? -1 : wmf_track(map, e, WMF_PLAYERS);
            if (g_wm.nf < NSLOT){ g_wm_f[g_wm.nf].m = e; g_wm_f[g_wm.nf].id = q->peer_id; g_wm_f[g_wm.nf].k = k; g_wm_f[g_wm.nf].w = f; g_wm.nf++; }
            break;
        }
    }
    rlog("[roam] f=%ld world map: legend rows %d of the game's%s%s%s, players %s, races %s\n", rh_frame(), rows, top ? ", ours above them" : "",
         g_wmf.row[0] ? " and Players" : "", g_wmf.row[1] ? " and Players' races" : "", g_wmf_on[0] ? "shown" : "hidden", g_wmf_on[1] ? "shown" : "hidden");
}
/* every frame the map is open: a row ticked or crossed since, its markers on or off the map */
static void wmf_frame(CPU *c, uint32_t map){
    if (g_wmf.map != map) return;
    int moved = 0;
    for (int i = 0; i < 2; i++){
        uint32_t r = g_wmf.row[i];
        if (!r) continue;
        int on = !MR8(r + 0x24u);
        if (on == g_wmf_on[i]) continue;
        g_wmf_on[i] = on; moved = 1;
#ifndef ROAM_HOST_WIN
        win32_reg_put_dword(wmf_reg[i], (uint32_t)on);
#endif
        rlog("[roam] f=%ld world map: %s %s\n", rh_frame(), wmf_text[i], on ? "shown" : "hidden");
    }
    if (!moved) return;
    for (int i = 0; i < g_wmf.n; i++){
        uint32_t m = g_wmf.m[i];
        int on = g_wmf_on[g_wmf.what[i]];
        MRF32(m + 0x10u) = on ? g_wmf.x[i] : WMF_AWAY; MRF32(m + 0x14u) = on ? g_wmf.y[i] : WMF_AWAY;
    }
    gcall0(c, FN_WM_PLACE, map);
}
/* the friends' cars' markers where their cars are now: a friend gone (or
 * hidden) off the map, its marker taken by a friend come since (a crew-mate's,
 * in the crew's colour, is not), or a marker made for one; 1 when the map is
 * to be placed again */
static int wm_shown(const Slot *q, uint32_t w){
    return q->st == SL_ACTIVE && !q->hidden && q->world == w && ok(q->car, 0x9e0) && MR8(q->car + CAR_ACTIVE);
}
static int wm_live(CPU *c, uint32_t map, uint32_t pkg, uint32_t pk, uint32_t w){
    int had[NSLOT] = {0}, made = 0;
    for (int i = 0; i < g_wm.nf; i++){
        const Slot *q = &g_sl[g_wm_f[i].k];
        if (g_wm_f[i].id && q->peer_id == g_wm_f[i].id && wm_shown(q, w)) had[g_wm_f[i].k] = 1;
        else if (g_wm_f[i].w >= 0) g_wm_f[i].id = 0;                  /* (free for another) */
    }
    for (int k = 0; k < NSLOT; k++){
        const Slot *q = &g_sl[k];
        if (had[k] || !wm_shown(q, w)) continue;
        int i = 0;
        while (i < g_wm.nf && (g_wm_f[i].id || g_wm_f[i].w < 0)) i++;
        if (i == g_wm.nf){
            uint32_t m = i < NSLOT && wm_room(pk) > 0 ? wm_mark(c, map, pkg, q->car) : 0;
            if (!m) continue;
            g_wm_f[i].m = m; g_wm_f[i].w = wmf_track(map, m, WMF_PLAYERS); g_wm.nf++; made++;
        }
        g_wm_f[i].id = q->peer_id; g_wm_f[i].k = k;
    }
    if (made) wm_dress(c, map, pkg, made);
    for (int i = 0; i < g_wm.nf; i++){
        const Slot *q = &g_sl[g_wm_f[i].k];
        if (g_wm_f[i].id && q->peer_id == g_wm_f[i].id && wm_shown(q, w))
            wmf_move(g_wm_f[i].m, g_wm_f[i].w, MRF32(q->car + 0x60u), MRF32(q->car + 0x64u), MR16(q->car + 0x40cu));
        else wmf_move(g_wm_f[i].m, g_wm_f[i].w, WMF_AWAY, WMF_AWAY, 0);
    }
    return g_wm.nf > 0;
}
/* the hub's answer, its n players past our friends (in g_wm_in, by key):
 * each one keeps its marker, one come takes a marker left over or a new one
 * while there is room, the markers left over go off the map; 1 when the map
 * is to be placed again */
static void wm_where(int i, int64_t host, float *x, float *y);
static int wm_others(CPU *c, uint32_t map, uint32_t pkg, uint32_t pk, int n, int total, int64_t host){
    int first = !g_wm.got;
    /* (a player placed again: where its marker is now, off its new place) */
    for (int i = 0, j = 0; j < n; j++){
        while (i < g_wm_n && g_wm_e[i].id < g_wm_in[j].id) i++;
        g_wm_oin[j].x = g_wm_oin[j].y = 0.f; g_wm_oin[j].at = 0;
        if (i >= g_wm_n || g_wm_e[i].id != g_wm_in[j].id) continue;
        if (g_wm_e[i].at_us == g_wm_in[j].at_us){ g_wm_oin[j] = g_wm_o[i]; continue; }
        float x, y; wm_where(i, host, &x, &y);
        float dt = host > g_wm_in[j].at_us ? (float)(host - g_wm_in[j].at_us) * 1e-6f : 0.f;
        g_wm_oin[j].x = x - ((float)g_wm_in[j].x + (float)g_wm_in[j].vx * dt);
        g_wm_oin[j].y = y - ((float)g_wm_in[j].y + (float)g_wm_in[j].vy * dt);
        g_wm_oin[j].at = host;
    }
    g_wm.got = host; g_wm.asks = 0;
#if MPRACES
    if (!g_wm.lobbies){ g_wm.lobbies = 1; mp_map_icons(c, map, pkg, (uint32_t)wm_room(pk), 1); }
#endif
    for (int k = 0; k < g_wm.others; k++) g_wm_m[k].used = 0;
    int kept = 0;
    for (int i = 0, j = 0; j < n; j++){
        while (i < g_wm_n && g_wm_e[i].id < g_wm_in[j].id) i++;
        g_wm_kin[j] = i < g_wm_n && g_wm_e[i].id == g_wm_in[j].id ? g_wm_k[i] : -1;
        if (g_wm_kin[j] >= 0){ g_wm_m[g_wm_kin[j]].used = 1; kept++; }
    }
    if (!g_wm.fake){ uint32_t sz = 0x410u; g_wm.fake = rh_call(c, FN_MALLOC, 0, 0, 1, &sz); if (!ok(g_wm.fake, 0x410u)) g_wm.fake = 0; }
    int room = -1, made = 0;
    for (int j = 0, f = 0; j < n; j++){
        if (g_wm_kin[j] >= 0) continue;
        while (f < g_wm.others && g_wm_m[f].used) f++;
        if (f < g_wm.others){ g_wm_kin[j] = (int16_t)f; g_wm_m[f].used = 1; continue; }
        if (room < 0) room = wm_room(pk);
        int i = g_wm.others + made;
        if (made >= room || !g_wm.fake || i >= RC_MAP_MAX) continue;
        MRF32(g_wm.fake + 0x60u) = (float)g_wm_in[j].x; MRF32(g_wm.fake + 0x64u) = (float)g_wm_in[j].y;
        MR16(g_wm.fake + 0x40cu) = (uint16_t)(g_wm_in[j].heading << 8);
        uint32_t m = wm_mark(c, map, pkg, g_wm.fake);
        if (!m){ room = 0; continue; }
        g_wm_m[i].m = m; g_wm_m[i].w = wmf_track(map, m, WMF_PLAYERS); g_wm_m[i].used = 1;
        g_wm_kin[j] = (int16_t)i; made++;
    }
    int copies = made ? wm_dress(c, map, pkg, room) : 0;
    g_wm.others += made;
    for (int k = 0; k < g_wm.others; k++) if (!g_wm_m[k].used) wmf_move(g_wm_m[k].m, g_wm_m[k].w, WMF_AWAY, WMF_AWAY, 0);
    memcpy(g_wm_e, g_wm_in, (size_t)n * sizeof *g_wm_e); memcpy(g_wm_k, g_wm_kin, (size_t)n * sizeof *g_wm_k);
    memcpy(g_wm_o, g_wm_oin, (size_t)n * sizeof *g_wm_o); g_wm_n = n;
    if (first || made)
        rlog("[roam] f=%ld world map: %d other players in free roam marked, %d of them now (the hub knows %d past your friends; room for %d), %d copies more\n",
             rh_frame(), g_wm.others, kept + made, total, room, copies);
    return g_wm.others > 0;
}
/* where the hub's player i's marker is now: moved on from where the hub
 * placed it at its velocity (not past WM_STALE_US from its last answer),
 * and the rest of the way it was off when placed again (WM_BLEND_US) */
static void wm_where(int i, int64_t host, float *x, float *y){
    const rc_mapent *e = &g_wm_e[i];
    int64_t until = g_wm.got + WM_STALE_US < host ? g_wm.got + WM_STALE_US : host;
    float dt = until > e->at_us ? (float)(until - e->at_us) * 1e-6f : 0.f;
    float k = g_wm_o[i].at && host - g_wm_o[i].at < WM_BLEND_US ? 1.f - (float)(host - g_wm_o[i].at) / (float)WM_BLEND_US : 0.f;
    *x = (float)e->x + (float)e->vx * dt + g_wm_o[i].x * k;
    *y = (float)e->y + (float)e->vy * dt + g_wm_o[i].y * k;
}
/* the hub's players' markers where they are now (wm_where); 1 when the map
 * is to be placed again */
static int wm_step(int64_t host){
    for (int i = 0; i < g_wm_n; i++){
        if (g_wm_k[i] < 0) continue;
        float x, y; wm_where(i, host, &x, &y);
        wmf_move(g_wm_m[g_wm_k[i]].m, g_wm_m[g_wm_k[i]].w, x, y, (uint16_t)(g_wm_e[i].heading << 8));
    }
    return g_wm_n > 0;
}
static void map_update(CPU *c, uint32_t w, int64_t host){
    if (!g_sync_paused || !g_gmem){ g_wm.done = 0; return; }        /* the world stands while the map is open */
    uint32_t sa = g_gmem + TAG_SCR + 0x48u;
    gstr(sa, WM_PACKAGE, 24);
    uint32_t pk = gcall1(c, FN_FE_PACKAGE, 0, sa);
    if (!ok(pk, 0x54)){ g_wm.done = 0; return; }
    uint32_t scr = wm_screen(pk), map = scr ? rd32(scr + WM_MAP) : 0, pkg = scr ? rd32(scr + 4u) : 0;
    if (!ok(map, 0x2c) || !ok(pkg, 24)){ g_wm.done = 0; return; }
    if (g_wm.done && map != g_wm.map) g_wm.done = 0;                    /* another map (opened again) */
    if (!g_wm.done){
        g_wm.done = 1; g_wm.map = map; g_wm.copies = 0; g_wm.asks = 0; g_wm.others = 0; g_wm.lobbies = 0;
        g_wm.got = 0; g_wm.moved = host; g_wm_n = 0; g_wm.steps = 0; g_wm.step_us = 0;
        int32_t col[4] = {0}; uint32_t o0 = wm_find(c, pkg, "AIRACERINDICATOR_0");      /* (before a crew's colour) */
        g_wm.col_ok = o0 && fe_colour_get(o0, col);
        for (int i = 0; i < 3; i++) g_wm.col[i] = col[i];
        int n = 0;
        wmf_begin(map);
#if MPRACES
        mp_map_icons(c, map, pkg, (uint32_t)wm_room(pk), 0);                   /* (before the cars take the room) */
#endif
        for (int k = NOPP; k < NSLOT; k++){
            Slot *s = &g_sl[k];
            if (s->st != SL_ACTIVE || s->hidden || !s->unindexed || s->world != w || !ok(s->car, 0x9e0) || !MR8(s->car + CAR_ACTIVE)) continue;
            if (!wm_mark(c, map, pkg, s->car)) break;
            n++;
        }
        int copies = wm_dress(c, map, pkg, wm_room(pk));
        if (n || copies) gcall0(c, FN_WM_PLACE, map);
        crew_map_open(c, map, pkg, pk, w);
        ai_map_open(c, map, pkg, w);
        wmf_open(c, scr, map, pkg, w);
        gcall0(c, FN_WM_PLACE, map);
        rlog("[roam] f=%ld world map open: %u car markers, %d of them friends' cars outside the car index; %d copies of AIRACERINDICATOR_0\n",
                rh_frame(), MR32(G_WM_COUNT), n, copies);
        roam_plat_lock(); g_wm.gen = g_s.map_gen; roam_plat_unlock();
    }
    wmf_frame(c, map);
#if MPRACES
    mp_map_labels(c, map);
#endif
    crew_map_labels(c, map);
    /* the friends' cars: their markers where the cars are; the hub's players'
     * moved on; WM_STEP_US */
    int place = 0, step = host - g_wm.moved >= WM_STEP_US;
    int64_t t0 = step ? roam_plat_now_us() : 0;
    if (step){ g_wm.moved = host; place = wm_live(c, map, pkg, pk, w) | wm_step(host); }
    /* the other players: asked for as the map opens and WM_LIVE_US after each
     * answer, marked (or moved) when the answer is in */
    int n = -1, total = 0, ask = host - g_wm.got >= WM_LIVE_US && (g_wm.got || g_wm.asks < WM_ASKS) && (!g_wm.asks || host - g_wm.asked > WM_ASK_US);
    int most = ask ? g_wm.others + wm_room(pk) : 0;                   /* (the markers made, and room for more) */
    roam_plat_lock();
    if (g_s.map_gen != g_wm.gen){
        g_wm.gen = g_s.map_gen;
        n = g_s.map_n < RC_MAP_MAX ? g_s.map_n : RC_MAP_MAX; total = g_s.map_total;
        memcpy(g_wm_in, g_s.map_e, (size_t)n * sizeof *g_wm_in);
    } else if (ask && rc_ask_map(&g_s, host, most < RC_MAP_MAX ? most : RC_MAP_MAX)){
        g_wm.asks++; g_wm.asked = host;
    }
    roam_plat_unlock();
    if (n >= 0) place |= wm_others(c, map, pkg, pk, n, total, host) | wm_step(host);
    if (place) gcall0(c, FN_WM_PLACE, map);
    if (step && n < 0){ g_wm.steps++; g_wm.step_us += roam_plat_now_us() - t0; }
}

/* ---- per-frame --------------------------------------------------------------------------------- */
typedef struct { int idx; uint32_t id; char name[RC_NAME_MAX + 1]; int visible, paused; float dist; uint32_t cfg_gen;
                 float rank;           /* the slots' order (assign): the nearest of the distances SOON_S and LOOK_S
                                          on, both cars at their speeds, and from our view point VIEW_M ahead (as
                                          the hub links, nfsu2-roam) */
                 int listed;           /* the hub's last list has it (or there is no hub): rc_peer_listed */
                 int duels;            /* takes outrun challenges (its HELLO) */
                 int has_cfg;          /* a complete car record is held */
                 int cfg_len;          /* > 0: the record is copied here (a slot needs it) */
                 uint8_t cfg[RC_CFG_MAX]; int have_pose; rc_pose pose; float err; uint32_t look;
                 int64_t newest, rt;   /* the sender's time of its newest snapshot, and the one the car is drawn at (push_pose) */
                 int horn;             /* their horn (horn.h's kinds) */
                 int crew;             /* in our crew number crew - 1 (rc_peer_crew + 1; 0: none) */ } PeerView;
static PeerView g_pv[RC_TABLE]; static int g_npv;
static struct { uint32_t id, gen; int horn; } g_phorn[RC_TABLE];   /* each peer's horn, as its record (cfg_gen) has it */

/* ---- horns, friends' engines, nitrous and hydraulics ----------------------------------------------- */
/* N or the controller's left stick click sounds the player's horn in free
 * roam while the world runs, and the snapshots carry it (RC_BIT_HORN); a
 * friend's horn sounds from their car, as the camera hears it (horn.c: the
 * distance, the side, Doppler).  So do a friend's engine, nitrous and
 * hydraulics, which the game sounds for the player's car (and, for a
 * friend's car that has an AI racer's sound state, an AI engine and its
 * nitrous going on and off): carsfx.c plays them with the game's own sounds
 * from the friend's state: the engine with the car's GIN files at the rpm
 * and volumes the friend's game sounds it (eng_sound), the nitrous with the
 * friend's throttle and speed as nos_pose draws it (let out unless on the
 * throttle from 5 mph), each wheel's hydraulics and the landings
 * (NFSU2_ROAM_CARSFX=0: not those, and the game's AI engines as they were). */
static int g_horns_on, g_carsfx_on;           /* horns, friends' car sounds were driven last frame */
static int g_carsfx_off = -1;
/* A friend's car's engine sound: its GIN files, as the game picks the
 * player's (0x4593B0) by the car's sound type (0x459240, from its car type)
 * and its engine upgrades (the ride's performance at +0x2F0: the package
 * 0x18, 0x13 or 0xF installed, 0x599610: level 3, 2, 1, else 0): the
 * engine of G_ENG_PICK's entry for both (3 dwords each, the engine first),
 * the pulling recording its +0xC and the letting-off one G_ENG_DCL's entry
 * at its +0x14.  From finish_load, with the friend's ride on the car. */
#define FN_SND_TYPE     0x00459240u   /* cdecl (car type) -> its sound type: 0 .. 30 */
#define FN_PERF_HAS     0x00599610u   /* thiscall (ride + 0x2F0, package) -> al: installed */
#define G_ENG_PICK      0x007f2230u   /* [sound type][level]: 3 dwords, the engine first */
#define G_ENGINES       0x007f0de0u   /* the engine sounds, 0x4C each: +0 the AEMS bank, +0xC the pulling GIN, +0x14 the letting-off one */
#define N_ENGINES       43u
#define G_ENG_DCL       0x007f1aa8u   /* the letting-off GIN files, 0x34 each: +0 the name */
#define N_ENG_DCL       37u
#define ENG_OLD_ON      0.25f         /* a friend whose game sends no engine: the pulling recording on the throttle, */
#define ENG_OLD_UNDER   0.08f         /* ...the letting-off one under it, */
#define ENG_OLD_OFF     0.22f         /* ...and off the throttle (what the game's volumes mostly are then) */
static void eng_files(CPU *c, Slot *s){
    uint32_t ride = s->ride_new, type = MR32(ride), types = rd32(G_CARTYPES);
    s->gin[0][0] = s->gin[1][0] = 0; s->eng_idle = s->eng_max = 0.f;
    uint32_t st = gcall1(c, FN_SND_TYPE, 0, type), lvl = 0;
    if (st > 30u) return;
    if (gcall1(c, FN_PERF_HAS, ride + 0x2f0u, 0x18u) & 0xffu) lvl = 3;
    else if (gcall1(c, FN_PERF_HAS, ride + 0x2f0u, 0x13u) & 0xffu) lvl = 2;
    else if (gcall1(c, FN_PERF_HAS, ride + 0x2f0u, 0xfu) & 0xffu) lvl = 1;
    uint32_t e = MR32(G_ENG_PICK + 12u * (st * 4u + lvl)), ent = G_ENGINES + 0x4cu * e, d = e < N_ENGINES ? MR32(ent + 0x14u) : N_ENG_DCL;
    if (e >= N_ENGINES || d >= N_ENG_DCL) return;
    cstr(MR32(ent + 0xcu), s->gin[0], sizeof s->gin[0]);
    cstr(MR32(G_ENG_DCL + 0x34u * d), s->gin[1], sizeof s->gin[1]);
    /* for a friend whose game sends no engine: the car type's idle and most rpm (CarTypeInfo +0x300, +0x308) */
    if (types && type < 64u && ok(types + 0x890u * type + 0x300u, 0x10)){
        s->eng_idle = MRF32(types + 0x890u * type + 0x300u); s->eng_max = MRF32(types + 0x890u * type + 0x308u);
    }
}
/* the engine for carsfx.c: the car's GIN files at the rpm and volumes the
 * friend's own game plays them (their snapshots' engine; the engine model
 * under the recordings, the game's AEMS voice, is not played here: its
 * volume goes to the letting-off recording, which at idle is the engine
 * settling there).  From a friend whose game sends no engine (an older one),
 * the rpm from the car's as the game scales it (0x484C50: 1000 + 9000 x
 * (rpm - idle) / (the most - idle)) and the pulling recording on the
 * throttle, the letting-off one off it */
static int g_eng_off = -1;                    /* NFSU2_ROAM_ENGINES=0: no friends' engines (the game's AI ones still silenced) */
static void eng_sound(const Slot *s, const rc_pose *P, carsfx_state *st){
    if (g_eng_off < 0){ const char *e = rh_opt("NFSU2_ROAM_ENGINES"); g_eng_off = e && *e == '0'; }
    if (g_eng_off || !s->gin[0][0] || !s->gin[1][0]) return;
    st->gin[0] = s->gin[0]; st->gin[1] = s->gin[1];
    if (P->eng > 0.f){
        st->eng = P->eng;
        st->eng_gain[0] = P->eng_acc;
        st->eng_gain[1] = rc_sqrtf(P->eng_dcl * P->eng_dcl + P->eng_base * P->eng_base);
        return;
    }
    if (!(s->eng_max > s->eng_idle + 100.f)) return;
    float n = (P->rpm - s->eng_idle) / (s->eng_max - s->eng_idle);
    st->eng = 1000.f + 9000.f * (n < 0.f ? 0.f : n > 1.f ? 1.f : n);
    int on = P->throttle > 0.05f;
    st->eng_gain[0] = on ? ENG_OLD_ON : 0.f; st->eng_gain[1] = on ? ENG_OLD_UNDER : ENG_OLD_OFF;
}
/* The game's own engine on a friend's car: the free-roam opponents' cars
 * have an AI racer's sound state, whose CARSFX_AIEngine plays an AEMS voice
 * of one of the few AI engine banks the game has loaded (not the friend's
 * car's, nor at the friend's rpm); carsfx.c plays the friend's own, so on a
 * friend's car with its GIN files the game's is silenced: its update
 * (0x4610F0, called only through its vtable) reads its volume through its
 * control word ([+0x58], 0x45E390), pointed at a zero for the call */
#ifndef ROAM_HOST_WIN
#define FN_AIENG_UPDATE 0x004610f0u
#define ZERO_SCR        0x70u         /* g_gmem scratch: a dword that stays 0 */
void dispatch_patch(uint32_t a, void (*f)(CPU *));
void sub_004610f0(CPU *c);
static int g_aieng_patched;
static uint32_t g_aieng_calls, g_aieng_muted;   /* (mp sounds) */
static void aieng_update(CPU *c){
    uint32_t self = c->ecx, st = rd32(self + 0x9cu), car = ok(st, 0x40) ? rd32(st + 0x30u) : 0, keep = 0;
    int mute = 0;
    for (int k = 0; car && g_gmem && !mute && k < NSLOT; k++) mute = g_sl[k].st != SL_FREE && g_sl[k].car == car && g_sl[k].gin[0][0];
    g_aieng_calls++; g_aieng_muted += (uint32_t)mute;
    if (mute){ keep = MR32(self + 0x58u); MR32(self + 0x58u) = g_gmem + ZERO_SCR; }
    sub_004610f0(c);
    if (mute) MR32(self + 0x58u) = keep;
}
#endif
/* the game sounds this car's nitrous itself: its sound state (an AI
 * racer's) has a nitrous child */
static int game_sounds_nos(uint32_t car){
    uint32_t st = ok(car, 0x9e0) ? rd32(car + CAR_SOUND) : 0;
    if (!ok(st, 0x100) || rd32(st + 0x30u) != car) return 0;
    for (uint32_t ch = rd32(st + 0x20u), i = 0; ok(ch, 0x100) && i < 64u; ch = rd32(ch + 0x50u), i++)
        if (rd32(ch) == VT_SFX_NOS) return 1;
    return 0;
}
static void sounds_update(const rc_state *ls, int heard){
    float L[3] = {0}, f[3] = {1.f, 0.f, 0.f};
    uint32_t cam = rd32(G_VIEW1 + 0x40u), me = player_car();
    if (!ls) heard = 0;
    if (heard && ok(cam, 0x60)){ rd3(cam + 0x40u, L); rd3(cam + 0x50u, f); }       /* the listener: the camera, */
    else if (heard && ok(me, 0x9e0)){ rd3(me + 0x60u, L); rd3(me + 0x90u, f); }   /* else the player's car */
    else heard = 0;
    if (g_carsfx_off < 0){ const char *e = rh_opt("NFSU2_ROAM_CARSFX"); g_carsfx_off = e && *e == '0'; }
#ifndef ROAM_HOST_WIN
    if (!g_carsfx_off && !g_aieng_patched){ dispatch_patch(FN_AIENG_UPDATE, aieng_update); g_aieng_patched = 1; }
#endif
    if ((heard || g_carsfx_on) && !g_carsfx_off){
        rh_carsfx_begin(heard ? L : 0, f, ls ? ls->v : 0);
        for (int i = 0; heard && i < g_npv; i++){
            const PeerView *v = &g_pv[i];
            if (!v->visible || v->paused || !v->have_pose || !v->pose.ext) continue;
            const rc_pose *P = &v->pose;
            int ks = -1;
            for (int k = 0; k < NSLOT && ks < 0; k++)
                if (g_sl[k].st == SL_ACTIVE && g_sl[k].peer_id == v->id && !g_sl[k].hidden) ks = k;
            uint32_t car = ks >= 0 ? g_sl[ks].car : 0;
            carsfx_state st = { (uint8_t)(P->nos != 0), (uint8_t)!(__builtin_fabsf(P->throttle) > 0.001f && P->speed * 2.237f >= 5.f),
                                P->hyd, P->thumps, (uint8_t)game_sounds_nos(car), { 0, 0 }, 0.f, { 0.f, 0.f } };
            if (car) eng_sound(&g_sl[ks], P, &st);
            rh_carsfx_car(v->id, v->name, P->p, P->v, &st);
        }
        rh_carsfx_end();
        g_carsfx_on = heard;
    }
    if (!heard && !g_horn && !g_horns_on) return;     /* racehorn.c may have the voices */
    rh_horn_begin(heard ? L : 0, f, ls ? ls->v : 0);
    if (g_horn) rh_horn_sound(g_s.conf.self_id, O.name, rh_horn_mine(), 0, 0);
    for (int i = 0; heard && i < g_npv; i++){
        const PeerView *v = &g_pv[i];
        if (v->visible && !v->paused && v->have_pose && (v->pose.bits & RC_BIT_HORN))
            rh_horn_sound(v->id, v->name, v->horn, v->pose.p, v->pose.v);
    }
    rh_horn_end();
    g_horns_on = heard || g_horn;
}

/* How far behind its sender's clock a friend's car is drawn (the render
 * delay of rc_interp_eval).  Ghost cars: the display delay
 * (NFSU2_ROAM_DELAY), smooth through the jitter buffer.  Solid cars: where
 * the friend is now, the newest state carried on by the one-way network
 * delay (half the smallest of the last 8 round trips, at most 150 ms): a
 * friend at 30 m/s drawn 150 ms in the past is hit 4.5 m behind the car's
 * real place, and at another spot on each screen.  A change is slewed at
 * 5 % of the frame's time, so the car never jumps. */
#define RD_PINGS 8
static struct { uint32_t id; int32_t rtt[RD_PINGS], last; int n, h, init; int64_t d; } g_rd[RC_TABLE];
static int64_t render_delay(int i, const rc_peer *p, int64_t frame_dt){
    if (g_rd[i].id != p->id){ memset(&g_rd[i], 0, sizeof g_rd[i]); g_rd[i].id = p->id; }
    if (p->rtt_us > 0 && p->rtt_us != g_rd[i].last){
        g_rd[i].last = p->rtt_us; g_rd[i].rtt[g_rd[i].h] = p->rtt_us; g_rd[i].h = (g_rd[i].h + 1) % RD_PINGS;
        if (g_rd[i].n < RD_PINGS) g_rd[i].n++;
    }
    int64_t want = (int64_t)O.delay_ms * 1000;
    if (O.collide){
        int32_t m = 0;
        for (int k = 0; k < g_rd[i].n; k++) if (k == 0 || g_rd[i].rtt[k] < m) m = g_rd[i].rtt[k];
        want = -(int64_t)(m / 2 < 150000 ? m / 2 : 150000);
    }
    if (!g_rd[i].init){ g_rd[i].d = want; g_rd[i].init = 1; }
    else {
        int64_t lim = (frame_dt > 1000000 ? 1000000 : frame_dt < 0 ? 0 : frame_dt) / 20 + 1, d = want - g_rd[i].d;
        g_rd[i].d += d > lim ? lim : d < -lim ? -lim : d;
    }
    return g_rd[i].d;
}

/* ---- outruns between players --------------------------------------------------------------------- */
/* Two players who stay near each other in free roam (within DU_NEAR_M for
 * DU_NEAR_US) get the game's own outrun challenge: the HUD's outrun icon
 * (0x4DB890: the "!" and two cars, the D-pad's Up lit) and a line of text
 * naming the friend (du_hud).  Engage (Enter, or the pad's D-pad Up: the
 * game's action 0xF, however the player set it) challenges; the friend gets
 * the icon and "<name> challenges you" and takes it with Engage, or lets it
 * go.  Then both games run the game's own outrun -- the "1,000 Feet" meter,
 * the result message, the bank reward or loss -- each against the other's
 * car as shown there:
 *  - started as Engage starts one (0x602E09), from the main-loop hook, with
 *    the World's AI range narrowed to the friend's car for the call, so the
 *    race (0x607180) has the two cars as its racers whichever slot the car
 *    has (one past World::IndexCars' 22 too);
 *  - the game decides it as its own: the racer ranked first wins once the
 *    other is 300 m behind it (1,000 feet with imperial units; 0x607C9C,
 *    the distances from 0x5FC510).  It ranks the opponent first at the
 *    start ("Catch Them!"), and only the leader's AI road path puts the
 *    player past it; a friend's car has none.  So each frame the one
 *    getting away is ranked first (du_rank): the one moving away from the
 *    other the faster, from where both cars are drawn frame to frame (the
 *    friend's velocity as sent is too rough), over DU_AWAY_US.  Only
 *    someone driving away takes the two 300 m apart, so a car standing
 *    still never wins, however the road bends (where the cars head would
 *    not do: on a loop the one getting away heads back).  Each game works
 *    it out from its own screen, and the two agree;
 *  - each game tells the other its result; one that has none yet takes it
 *    (race +0xBEC/+0xBF0, as the game's result message 0x4A7B30 does), so
 *    a few metres of network delay never make two winners;
 *  - a player who leaves (back to the menus, quits, loses the connection)
 *    loses: the other wins.
 * While it runs both still count as in free roam: friends' cars stay, and
 * friends still see the two.  Messages: RC_MSG_DUEL (roam_core.h), each
 * repeated until answered (a datagram lost on a LAN).  Not in the Windows
 * plugin (DUELS): its players are not offered one. */
#ifdef ROAM_HOST_WIN
#define DUELS           0
#else
#define DUELS           1
#endif
#define G_ACTIONS       0x0086eaf0u   /* the joystick actions held this frame, a bit per action, 0 = held (0x5CFD60) */
#define ACT_ENGAGE      15u           /* Engage */
#define G_OUTRUN_STATE  (G_ENCOUNTERS + 0x1ae8u)  /* 2: an outrun can be taken (0x500D20) */
#define G_ENGAGE        0x00838590u
#define PL_OPPONENT     0x250u        /* Player: the car an outrun is against */
#define PL_MARKER       0x9acu        /* Player: the event or shop marker Engage would enter */
#define RACE_RESULT     0xbecu        /* race: the result message's position (+0xBF0 its player number) */
#define FN_HUD_OUTRUN   0x004db890u   /* thiscall HUD (car or 0): the outrun icon on or off */
#define FN_OUTRUN_START 0x00607180u   /* thiscall race: an outrun against player +0x250, the AI range its racers */
#define FN_ENGAGE_DONE  0x005010e0u   /* thiscall G_ENGAGE: what Engage does after starting one */
#define DU_NEAR_M       25.f          /* the game's own reach for a challenge is 30 m (0x40C030): less in a crowd */
#define DU_NEAR_US      4000000
#define DU_PROMPT_M     35.f          /* the offer stays while the two are this near */
#define DU_KEEP_M       60.f          /* a challenge stands while the two are this near */
#define DU_RANK_MPS     2.f           /* the order changes when the other gets away this much faster (m/s) */
#define DU_AWAY_US      1000000
#define DU_OFFER_US     15000000      /* a challenge nobody takes goes */
#define DU_GONE_US      2000000       /* a friend whose car is not shown for this long has left the outrun */
#define DU_NOTE_US      4000000
enum { DU_IDLE, DU_SENT, DU_GOT, DU_ACCEPTED, DU_RACE, DU_OVER };
static const char *const du_st[] = { "idle", "challenge sent", "challenged", "accepted", "racing", "over" };
static struct {
    int      st;
    uint32_t id, duel;                        /* the friend; the outrun's id (the challenger's) */
    char     name[RC_NAME_MAX + 1];
    int      mine;                            /* we challenged */
    int64_t  since, tx_at, heard;             /* this state's start, our last message, the friend's last */
    uint8_t  op, arg;                         /* what we say until answered */
    uint32_t car;                             /* the friend's car: the outrun's opponent */
    int      won;                             /* our game's result: 1 won, 2 lost, 0 none (yet) */
    int      their;                           /* the friend's game's, for us: 1 we won, 2 we lost */
    int      posted, nres;                    /* we gave the game a result; results still to send */
    int64_t  gone_at;
    float    gap, away;                       /* metres between the cars; how much faster we get away (m/s, smoothed) */
    float    was[4]; int64_t away_at;         /* where the two cars were (ours x y, the friend's x y), and when */
    uint32_t ranks;                           /* order changes */
    uint32_t icon, hud;                       /* the car the HUD's outrun icon is on (0 off), in this HUD */
    int      engage;                          /* Engage held last frame */
    int      busy;                            /* we take no outrun now (du_free): the snapshots say so (RC_BIT_BUSY) */
    char     note[TAG_DUEL_MAX]; int64_t note_until;
    uint32_t offers, races, wins, losses;     /* statistics */
} g_du;
static int du_racing(uint32_t race){
    return (g_du.st == DU_RACE || g_du.st == DU_OVER) && g_du.car && MR32(G_RACETYPE) == 7u && MR32(race + 0x24u) >= 2u;
}
/* the outrun's friend keeps the slot (assign) */
static int du_keeps(int k){ return g_du.st != DU_IDLE && g_sl[k].peer_id == g_du.id; }
static void du_note(int64_t host, const char *fmt, const char *name){
    snprintf(g_du.note, sizeof g_du.note, fmt, name);
    g_du.note_until = host + DU_NOTE_US;
    rlog("[roam] f=%ld outrun: %s\n", rh_frame(), g_du.note);
}
static void du_send(int64_t host, uint8_t op, uint8_t arg){
    roam_plat_lock(); rc_send_duel(&g_s, g_du.id, op, arg, g_du.duel); roam_plat_unlock();
    g_du.op = op; g_du.arg = arg; g_du.tx_at = host;
}
static void du_to(int st, int64_t host){
    if (st != g_du.st) rlog("[roam] f=%ld outrun with %s: %s -> %s\n", rh_frame(), g_du.name[0] ? g_du.name : "?", du_st[g_du.st], du_st[st]);
    g_du.st = st; g_du.since = host;
    if (st == DU_IDLE){ g_du.id = 0; g_du.duel = 0; g_du.car = 0; g_du.won = g_du.their = 0; g_du.posted = 0; g_du.nres = 0; g_du.op = 0; }
}
/* the friend's slot with a car shown, or -1 */
static int du_slot(uint32_t id){
    for (int k = 0; k < NSLOT; k++)
        if (g_sl[k].st == SL_ACTIVE && g_sl[k].peer_id == id && !g_sl[k].hidden && ok(g_sl[k].car, 0x9e0)) return k;
    return -1;
}
static const PeerView *du_pv(uint32_t id){ for (int i = 0; i < g_npv; i++) if (g_pv[i].id == id) return &g_pv[i]; return 0; }
/* the friend can race now: shown, in free roam and not paused, takes challenges */
static int du_able(int k){
    const PeerView *v = k >= 0 ? du_pv(g_sl[k].peer_id) : 0;
    return v && v->visible && !v->paused && v->duels && !g_sl[k].paused;
}
/* the friend's game says it takes no outrun now (near a door or one of their
 * events, in a marker, racing): none offered (the challenge goes on once made) */
static int du_busy(int k){
    const PeerView *v = du_pv(g_sl[k].peer_id);
    return v && v->have_pose && (v->pose.bits & RC_BIT_BUSY);
}
static float du_dist(uint32_t car){
    uint32_t me = player_car();
    float dx = MRF32(car + 0x60u) - MRF32(me + 0x60u), dy = MRF32(car + 0x64u) - MRF32(me + 0x64u);
    return rc_sqrtf(dx * dx + dy * dy);
}
static void du_icon(CPU *c, uint32_t car){
    uint32_t pl = world() ? rd32(G_PLAYER1) : 0, hud = pl ? rd32(pl + PL_HUD) : 0;
    if (hud != g_du.hud){ g_du.hud = hud; g_du.icon = 0; }                 /* a new HUD: nothing on it yet */
    if (!hud || g_du.icon == car) return;
    gcall1(c, FN_HUD_OUTRUN, hud, car);
    g_du.icon = car;
}
/* The game offers a challenger's outrun (0x413030, every frame of the
 * manager's state 3) when one of its cars is within 30 m of the player's
 * (0x40C030; 40 m once offered), ahead, about as fast and heading the same
 * way: that car is then the player's +0x250 (PL_OPPONENT), and Engage
 * takes it.  The challenger comes first: no outrun between players is
 * offered, or taken, while one of its cars is that near or its offer
 * stands */
#define EM_REACH_M      30.f
static int em_near(void){
    uint32_t em = encounter_mgr(), pl = rd32(G_PLAYER1);
    if (!em || !pl || !player_car() || (g_em != EM_GAME && g_em != EM_PAUSED)) return 0;
    for (uint32_t k = 0; k < 3; k++){
        uint32_t car = rd32(em + EM_TABLE + 0xcu + 0x10u * k);
        if (ok(car, 0x9e0) && car_out(car) && (rd32(pl + PL_OPPONENT) == car || du_dist(car) < EM_REACH_M)) return 1;
    }
    return 0;
}
/* That pass also turns the outrun icon off every frame no challenger is in
 * reach (0x500D20 with no car), ours too: while ours is up (an outrun or a
 * friend's challenge offered) the manager is paused, 6 from where it was (0
 * to 3), its cars driving on, and it goes on from there once ours is gone
 * (em_resume) -- or is held off or stood down from there (em_update) */
static void em_pause(const char *why){
    uint32_t em = encounter_mgr();
    if (!em || g_em != EM_GAME || MR32(em) > 3u) return;
    g_em_paused = em; g_em_paused_st = MR32(em); MR32(em) = 6u; g_em = EM_PAUSED;
    if (g_em_pauses++ < 20) rlog("[roam] f=%ld outrun challengers paused (state %u): %s\n", rh_frame(), g_em_paused_st, why);
}
/* At a shop's or garage's door, or an event's (DOOR_M), Enter is for the
 * door: no outrun between players is offered there (du_free), and the
 * challengers' manager is paused too, an outrun it offered taken back (the
 * icon off, its state 3 as with no car in reach: Engage takes an outrun
 * only at 2) */
static int em_at_door(void){
    uint32_t pl = rd32(G_PLAYER1);
    return (pl && rd32(pl + PL_MARKER)) || (g_pass.me & (SOFT_DOOR | SOFT_EVENT));
}
static void em_door(CPU *c){
    em_pause("Enter is for the door");
    if (g_em != EM_PAUSED || MR32(G_OUTRUN_STATE) != 2u) return;
    uint32_t pl = rd32(G_PLAYER1), hud = pl ? rd32(pl + PL_HUD) : 0;
    if (hud) gcall1(c, FN_HUD_OUTRUN, hud, 0);
    MR32(G_OUTRUN_STATE) = 3u;
    rlog("[roam] f=%ld outrun challengers: their outrun taken back, Enter is for the door\n", rh_frame());
}
static void em_resume(void){
    if (g_em != EM_PAUSED) return;
    uint32_t em = encounter_mgr();
    if (em && em == g_em_paused && MR32(em) == 6u) MR32(em) = g_em_paused_st;
    g_em = EM_GAME;
    if (g_em_pauses <= 20) rlog("[roam] f=%ld outrun challengers go on (state %u)\n", rh_frame(), g_em_paused_st);
}
/* our game can race now: free roam, the world running, no race, no marker
 * Engage would enter, and not near one (DOOR_M: Enter there is for the door
 * or the event) */
#if MPRACES
static int mp_busy(void);                     /* in a race lobby, at a multiplayer-only spot or by a lobby (mp_frame): no outrun */
#else
static int mp_busy(void){ return 0; }
#endif
static int du_free(int in_roam, int world_runs){
    uint32_t race = rd32(G_RACE), pl = rd32(G_PLAYER1);
    return in_roam && world_runs && pl && ok(race, 0xc44) && MR32(race + 0x24u) == 1u && !MR32(G_RACETYPE) && !rd32(pl + PL_MARKER) &&
           !(g_pass.me & (SOFT_DOOR | SOFT_EVENT)) && !mp_busy();
}
/* the game's outrun against the friend's car, as Engage starts one */
static int du_start(CPU *c, int64_t host){
    int k = du_slot(g_du.id);
    uint32_t w = world(), race = rd32(G_RACE), pl = rd32(G_PLAYER1);
    if (k < 0 || !w || !pl || !ok(race, 0xc44) || MR32(race + 0x24u) != 1u || MR32(G_RACETYPE)) return 0;
    uint32_t first = MR32(w + 0x14u), end = MR32(w + 0x18u);
    if (first >= WORLD_INDEX) return 0;
    uint32_t car = g_sl[k].car, keep = MR32(w + 0x1cu + 4u * first);
    du_icon(c, 0);
    MR32(w + 0x1cu + 4u * first) = car; MR32(w + 0x18u) = first + 1u;     /* the AI range: the friend's car alone */
    MR32(pl + PL_OPPONENT) = car; MR32(G_OUTRUN_STATE) = 2u;
    gcall0(c, FN_OUTRUN_START, race);
    gcall0(c, FN_ENGAGE_DONE, G_ENGAGE);
    MR32(w + 0x1cu + 4u * first) = keep; MR32(w + 0x18u) = end;
    g_du.car = car; g_du.won = g_du.their = 0; g_du.posted = 0; g_du.nres = 0; g_du.gone_at = 0; g_du.ranks = 0; g_du.away = 0; g_du.away_at = 0;
    if (MR32(G_RACETYPE) != 7u || MR32(race + 0x24u) != 2u){
        rlog("[roam] f=%ld outrun with %s: the game did not start it (race type %u, %u racers)\n", rh_frame(), g_du.name,
                MR32(G_RACETYPE), MR32(race + 0x24u));
        MR32(pl + PL_OPPONENT) = 0; g_du.car = 0;
        return 0;
    }
    g_du.races++;
    rlog("[roam] f=%ld outrun with %s: on, against car 0x%08x, %.0f m apart\n", rh_frame(), g_du.name, car, (double)du_dist(car));
    return 1;
}
/* a result into the game, as its result message does (0x4A7B30): the
 * player's position, 1 won or 2 lost; the race finishes on its next update */
static void du_post(int won, const char *why){
    uint32_t race = rd32(G_RACE), me = player_car(), owner = me ? rd32(me + 0x18u) : 0;
    if (g_du.posted || !ok(race, 0xc44) || !du_racing(race)) return;
    MR32(race + RACE_RESULT + 4u) = owner ? rd32(owner + 0x10u) : 0;
    MR32(race + RACE_RESULT) = won ? 1u : 2u;
    g_du.posted = 1;
    rlog("[roam] f=%ld outrun with %s: %s for us (%s), %.0f m apart\n", rh_frame(), g_du.name, won ? "a win" : "a loss", why, (double)g_du.gap);
}
/* our game's outrun is over: the player's racer says how (FinishRacer's
 * completion, 6 won, 7 lost; it stays a few seconds after the race), or it
 * ended without one */
static int du_outcome(uint32_t race){
    uint32_t me = player_car();
    for (uint32_t i = 0; me && i < 6; i++){
        uint32_t r = race + 0x30u + 0x1f0u * i;
        if (rd32(r) != me) continue;
        uint32_t ct = MR16(r + 0x10u);
        return ct == 6u ? 1 : ct == 7u ? 2 : 0;
    }
    return 0;
}
/* the race's order: the one ahead first (position 1), as the game's own
 * outrun has it once the player gets past the AI */
static void du_rank(uint32_t race, int me_first){
    uint32_t a = rd32(race + 0xbd0u), b = rd32(race + 0xbd4u);
    if (MR32(race + 0x24u) != 2u || !ok(a, 0x1f0) || !ok(b, 0x1f0) || (rd32(a) == player_car()) == me_first) return;
    MR32(race + 0xbd0u) = b; MR32(race + 0xbd4u) = a;
    MR8(b + 0xau) = 1; MR8(a + 0xau) = 2;
    g_du.ranks++;
}
/* the outrun is over in the game (its result shown) or never was: what it
 * leaves behind goes, and the career's outrun bookkeeping starts again */
static void du_clear(CPU *c){
    uint32_t race = rd32(G_RACE), pl = rd32(G_PLAYER1);
    if (ok(race, 0xc44) && !MR32(G_RACETYPE)){ MR32(race + RACE_RESULT) = 0; MR32(race + RACE_RESULT + 4u) = 0; }
    if (pl && rd32(pl + PL_OPPONENT) == g_du.car) MR32(pl + PL_OPPONENT) = 0;
    gcall0(c, FN_EM_OVER, G_ENCOUNTERS);
}
/* one DUEL message from the friend `from` */
static void du_recv(CPU *c, int64_t host, uint32_t from, uint8_t op, uint8_t arg, uint32_t duel, int in_roam, int world_runs){
    uint32_t self = g_s.conf.self_id;
    int ours = g_du.st != DU_IDLE && from == g_du.id && duel == g_du.duel;
    if (ours) g_du.heard = host;
    switch (op){
    case RC_DUEL_OFFER: {
        if (ours) return;                                              /* repeated */
        int k = du_slot(from);
        if (g_du.st == DU_SENT && from == g_du.id){
            /* we challenged each other: the lower id's challenge it is */
            g_du.heard = host;
            if (self < from){ rlog("[roam] f=%ld outrun with %s: challenged each other, ours stands\n", rh_frame(), g_du.name);
                              goto accepted; }
            rlog("[roam] f=%ld outrun with %s: challenged each other, theirs stands\n", rh_frame(), g_du.name);
            g_du.duel = duel; g_du.mine = 0;
            du_send(host, RC_DUEL_ACCEPT, 0); du_to(DU_ACCEPTED, host);
            return;
        }
        if (g_du.st != DU_IDLE || k < 0 || !du_able(k) || !du_free(in_roam, world_runs) || em_near() || du_dist(g_sl[k].car) > DU_KEEP_M){
            roam_plat_lock(); rc_send_duel(&g_s, from, RC_DUEL_CANCEL, RC_DC_BUSY, duel); roam_plat_unlock();
            return;
        }
        g_du.id = from; g_du.duel = duel; g_du.mine = 0; g_du.heard = host; g_du.op = 0;
        snprintf(g_du.name, sizeof g_du.name, "%s", g_sl[k].name);
        du_to(DU_GOT, host);
        return; }
    case RC_DUEL_ACCEPT:
        if (!ours) return;
        if (g_du.st == DU_RACE && g_du.mine){ du_send(host, RC_DUEL_START, 0); return; }   /* our START was lost */
        if (g_du.st != DU_SENT) return;
    accepted:
        if (!du_start(c, host)){
            du_send(host, RC_DUEL_CANCEL, RC_DC_BUSY); du_note(host, "The outrun with %s could not start", g_du.name); du_to(DU_IDLE, host);
            return;
        }
        du_send(host, RC_DUEL_START, 0); du_to(DU_RACE, host);
        return;
    case RC_DUEL_START:
        if (!ours || g_du.st != DU_ACCEPTED) return;
        if (!du_start(c, host)){
            du_send(host, RC_DUEL_CANCEL, RC_DC_LEFT); du_note(host, "The outrun with %s could not start", g_du.name); du_to(DU_IDLE, host);
            return;
        }
        g_du.op = 0;
        du_to(DU_RACE, host);
        return;
    case RC_DUEL_RESULT:                                               /* the friend won (1) or lost (2) */
        if (ours && (g_du.st == DU_RACE || g_du.st == DU_OVER) && (arg == 1 || arg == 2)) g_du.their = arg == 1 ? 2 : 1;
        return;
    case RC_DUEL_CANCEL:
        if (!ours) return;
        if (g_du.st == DU_RACE){ g_du.their = 1; du_note(host, "%s left the outrun", g_du.name); du_post(1, "the friend left"); return; }
        if (g_du.st == DU_OVER) return;
        if (g_du.st == DU_SENT) du_note(host, arg == RC_DC_BUSY ? "%s cannot race now" : "%s did not take the outrun", g_du.name);
        else if (g_du.st == DU_GOT) du_note(host, "%s called off the outrun", g_du.name);
        else du_note(host, "The outrun with %s is off", g_du.name);
        du_to(DU_IDLE, host);
        return;
    }
}
/* per frame, after the slots (the friends' cars are where they are drawn) */
static void du_frame(CPU *c, int64_t host, int in_roam, int world_runs, const uint32_t *in, int nin){
    if (!DUELS) return;
    for (int i = 0; i < nin; i++) du_recv(c, host, in[4 * i], (uint8_t)in[4 * i + 1], (uint8_t)in[4 * i + 2], in[4 * i + 3], in_roam, world_runs);
    uint32_t race = rd32(G_RACE), me = player_car();
    int eng = in_roam && world_runs && !(rd32(G_ACTIONS) & (1u << ACT_ENGAGE));
    int press = eng && !g_du.engage;
    g_du.engage = eng;
    int free = du_free(in_roam, world_runs);
    int offer = free && !em_near();                 /* (a challenger of the game's in reach: Engage is its outrun's) */
    g_du.busy = !offer;
    /* who has been near for the time it takes */
    for (int k = 0; k < NSLOT; k++){
        Slot *s = &g_sl[k];
        int near = offer && me && s->st == SL_ACTIVE && !s->hidden && ok(s->car, 0x9e0) && du_able(k) && !(s->soft & SOFT_DOOR) &&
                   !du_busy(k) && du_dist(s->car) < (g_du.st == DU_IDLE && g_du.icon == s->car ? DU_PROMPT_M : DU_NEAR_M);
        if (!near) s->near_since = 0;
        else if (!s->near_since) s->near_since = host;
    }
    int k = g_du.st != DU_IDLE ? du_slot(g_du.id) : -1;
    switch (g_du.st){
    case DU_IDLE: {
        int best = -1; float bd = 0;
        for (int j = 0; j < NSLOT; j++){
            if (!g_sl[j].near_since || host - g_sl[j].near_since < DU_NEAR_US) continue;
            float d = du_dist(g_sl[j].car);
            if (best < 0 || d < bd){ best = j; bd = d; }
        }
        int door = in_roam && world_runs && em_at_door();
        if (best >= 0) em_pause("the outrun icon is a friend's");   /* before the icon (em_pause) */
        else if (door) em_door(c);
        du_icon(c, best >= 0 ? g_sl[best].car : 0);
        if (best < 0 && !door) em_resume();
        if (best < 0 || !press) break;
        roam_plat_lock(); do g_du.duel = roam_plat_random(); while (!g_du.duel); roam_plat_unlock();
        g_du.id = g_sl[best].peer_id; g_du.mine = 1; g_du.heard = host; g_du.offers++;
        snprintf(g_du.name, sizeof g_du.name, "%s", g_sl[best].name);
        du_send(host, RC_DUEL_OFFER, 0);
        du_to(DU_SENT, host);
        break; }
    case DU_SENT:
    case DU_GOT:
    case DU_ACCEPTED:
        if (k >= 0 && g_du.st == DU_GOT) em_pause("the outrun icon is a friend's");
        du_icon(c, k >= 0 && g_du.st == DU_GOT ? g_sl[k].car : 0);
        if (k < 0 || !du_able(k) || !free || du_dist(g_sl[k].car) > DU_KEEP_M){
            if (g_du.st != DU_GOT) du_send(host, RC_DUEL_CANCEL, RC_DC_WITHDRAWN);
            du_note(host, k >= 0 && du_able(k) && free ? "The outrun with %s is off: too far apart" : "The outrun with %s is off", g_du.name);
            du_to(DU_IDLE, host);
            break;
        }
        if (g_du.st == DU_SENT){
            if (host - g_du.since > DU_OFFER_US){
                du_send(host, RC_DUEL_CANCEL, RC_DC_WITHDRAWN); du_note(host, "%s did not take the outrun", g_du.name); du_to(DU_IDLE, host);
            } else if (host - g_du.tx_at >= 500000) du_send(host, RC_DUEL_OFFER, 0);
        } else if (g_du.st == DU_GOT){
            if (host - g_du.heard > 2000000 || host - g_du.since > DU_OFFER_US){ du_to(DU_IDLE, host); break; }   /* the challenge stopped */
            if (press){ du_send(host, RC_DUEL_ACCEPT, 0); du_to(DU_ACCEPTED, host); }
        } else {
            if (host - g_du.since > 4000000){ du_note(host, "The outrun with %s did not start", g_du.name); du_to(DU_IDLE, host); }
            else if (host - g_du.tx_at >= 300000) du_send(host, RC_DUEL_ACCEPT, 0);
        }
        break;
    case DU_RACE: {
        if (!ok(race, 0xc44) || !me || !du_racing(race)){
            /* our game's outrun is over: its result to the friend, or we left it */
            g_du.won = ok(race, 0xc44) ? du_outcome(race) : 0;
            if (g_du.won == 1) g_du.wins++;
            if (g_du.won == 2) g_du.losses++;
            rlog("[roam] f=%ld outrun with %s: over, %s, %.0f m apart, the order changed %u time(s)%s\n", rh_frame(), g_du.name,
                    g_du.won == 1 ? "WON" : g_du.won == 2 ? "LOST" : "we left it", (double)g_du.gap, g_du.ranks,
                    g_du.their && g_du.won && g_du.their != g_du.won ? "; the friend's game said otherwise" : "");
            if (!g_du.won){
                for (int n = 0; n < 3; n++) du_send(host, RC_DUEL_CANCEL, RC_DC_LEFT);
                if (ok(race, 0xc44)) du_clear(c);
                du_to(DU_IDLE, host);
                break;
            }
            g_du.nres = 5; g_du.tx_at = 0;
            du_to(DU_OVER, host);
            break;
        }
        if (g_du.mine && g_du.op == RC_DUEL_START && host - g_du.since < 1000000 && host - g_du.tx_at >= 300000) du_send(host, RC_DUEL_START, 0);
        /* the friend is gone (not shown, not in free roam, no packets): its
         * car stays where it was (the slots leave it), and we win */
        const PeerView *v = du_pv(g_du.id);
        if (!car_out(g_du.car) || (k >= 0 && g_sl[k].car != g_du.car)){
            /* its car was parked (the friend left the session): at once, before the game's next update counts it */
            if (!g_du.posted){ du_note(host, "%s left the outrun", g_du.name); du_post(1, "the friend's car is gone"); }
            break;
        }
        if (k < 0 || !v || !v->visible){
            if (!g_du.gone_at) g_du.gone_at = host;
            if (host - g_du.gone_at > DU_GONE_US && !g_du.posted){ du_note(host, "%s left the outrun", g_du.name); du_post(1, "the friend is gone"); }
            break;
        }
        g_du.gone_at = 0;
        if (g_du.their){ du_post(g_du.their == 1, "the friend's game has its result"); break; }
        uint32_t car = g_du.car;
        float p[4] = { MRF32(me + 0x60u), MRF32(me + 0x64u), MRF32(car + 0x60u), MRF32(car + 0x64u) };
        float dx = p[0] - p[2], dy = p[1] - p[3];
        g_du.gap = rc_sqrtf(dx * dx + dy * dy);
        float dt = (float)(host - g_du.away_at) / 1e6f;
        if (g_du.away_at && dt > 0.f && g_du.gap > 1.f){
            /* our speed away from the friend less the friend's away from us,
             * from how each car moved since the last frame: the friend's game
             * gets minus this.  A jump (a respawn) counts for nothing */
            float ux = dx / g_du.gap, uy = dy / g_du.gap;
            float a = ((p[0] - g_du.was[0]) * ux + (p[1] - g_du.was[1]) * uy) / dt, b = -((p[2] - g_du.was[2]) * ux + (p[3] - g_du.was[3]) * uy) / dt;
            if (!__builtin_isfinite(a - b) || __builtin_fabsf(a) > 150.f || __builtin_fabsf(b) > 150.f) a = b = 0;
            float t = dt * 1e6f / (float)DU_AWAY_US;
            g_du.away += (a - b - g_du.away) * (t < 1.f ? t : 1.f);
        }
        for (int i = 0; i < 4; i++) g_du.was[i] = p[i];
        g_du.away_at = host;
        if (g_du.away > DU_RANK_MPS) du_rank(race, 1);
        else if (g_du.away < -DU_RANK_MPS) du_rank(race, 0);
        break; }
    case DU_OVER:
        if (g_du.nres > 0 && host - g_du.tx_at >= 300000){ du_send(host, RC_DUEL_RESULT, (uint8_t)g_du.won); g_du.nres--; }
        if (!g_du.nres){
            if (ok(race, 0xc44)) du_clear(c);
            du_to(DU_IDLE, host);
        }
        break;
    }
}
/* ---- keep right: new players leaving the airport ------------------------------------------------ */
/* Every new player's car appears at the new career's start, the airport's
 * car park, and drives out from there onto roads full of other players.  A
 * player whose car stood on that spot gets a line at the top as it drives
 * off, while friends are in free roam (du_hud: an outrun's or a race lobby's
 * line first): KR_SHOW_US of it, counted while it shows. */
#define KR_TEXT         "Please keep right to help avoid crashes"
#define KR_GO_M         15.f          /* the car is this far from the spot: it drives off */
#define KR_SHOW_US      15000000
static struct { int armed; int64_t left, at; } g_kr;   /* at: when it showed last (0: not last frame) */
static void kr_frame(void){
    if (!g_lp_ok) return;
    float dx = g_lp[0] - SPAWN_X, dy = g_lp[1] - SPAWN_Y, dz = g_lp[2] - SPAWN_Z, d2 = dx * dx + dy * dy + dz * dz;
    float v = rc_sqrtf(g_lv[0] * g_lv[0] + g_lv[1] * g_lv[1] + g_lv[2] * g_lv[2]);
    if (d2 < (float)(SPAWN_M * SPAWN_M) && v < IDLE_MPS) g_kr.armed = 1;
    else if (g_kr.armed && d2 > KR_GO_M * KR_GO_M){
        g_kr.armed = 0;
        int about = 0;
        for (int i = 0; i < g_npv; i++) if (g_pv[i].visible) about++;
        if (about){ g_kr.left = KR_SHOW_US; g_kr.at = 0; }
        rlog("[roam] f=%ld keep right: the car drives off the new career's start, %d friend(s) in free roam%s\n", rh_frame(), about,
             about ? ": the line shows" : ": no line");
    }
}
/* the line this frame, while some of its time is left; last: when it showed last frame (0: it did not) */
static int kr_line(int64_t host, int64_t last){
    if (g_kr.left <= 0) return 0;
    int64_t dt = last ? host - last : 0;
    g_kr.left -= dt < 0 ? 0 : dt > 100000 ? 100000 : dt;
    g_kr.at = host;
    return 1;
}

/* the line of text, top centre, after the HUD's update (roam_game_hud) */
#if MPRACES
static int mp_line(int64_t host, char *tx, int n);
static int64_t g_mp_hud_at;                   /* free roam's HUD was up, no dialog over it (the lobby's panel shows) */
#endif
static void du_hud(CPU *c, int64_t host){
    uint32_t pl = rd32(G_PLAYER1), hud = pl && ok(pl, PL_HUD + 4u) ? rd32(pl + PL_HUD) : 0;
    uint32_t pkg = hud && ok(hud, 0x14) ? rd32(hud + 0x10u) : 0;
    int64_t kr_last = g_kr.at;
    g_kr.at = 0;
    if (!hud || hud != g_tag.hud || !ok(pkg, 0x20) || !g_gmem) return;
    char tx[TAG_DUEL_MAX] = {0};
#if MPRACES
    if (g_in_roam && hud_on_top(pkg) && !rd32(0x00838974u /* a dialog up */)) g_mp_hud_at = host;
    if (g_in_roam && hud_on_top(pkg) && g_du.st == DU_IDLE && !g_du.icon && !rd32(0x00838974u /* a dialog up */)) mp_line(host, tx, sizeof tx);   /* the race lobby's, else the outrun's */
    if (!tx[0] && g_in_roam && hud_on_top(pkg)){
#else
    if (g_in_roam && hud_on_top(pkg)){
#endif
        if (g_du.st == DU_IDLE && g_du.icon){
            for (int k = 0; k < NSLOT; k++) if (g_sl[k].car == g_du.icon && g_sl[k].st == SL_ACTIVE)
                snprintf(tx, sizeof tx, "Press Enter to challenge %s to an outrun", g_sl[k].name);
        }
        else if (g_du.st == DU_SENT) snprintf(tx, sizeof tx, "Waiting for %s to take the outrun", g_du.name);
        else if (g_du.st == DU_GOT) snprintf(tx, sizeof tx, "%s challenges you to an outrun: press Enter to take it", g_du.name);
        else if (g_du.st == DU_ACCEPTED) snprintf(tx, sizeof tx, "Outrun with %s", g_du.name);
        if (!tx[0] && g_du.note[0] && host < g_du.note_until && g_du.st != DU_RACE && g_du.st != DU_OVER) snprintf(tx, sizeof tx, "%s", g_du.note);
        if (!tx[0] && g_du.st == DU_IDLE && !rd32(0x00838974u /* a dialog up */) && kr_line(host, kr_last)) snprintf(tx, sizeof tx, "%s", KR_TEXT);
    }
    if (!tx[0]){ tag_hide(c, TAG_DUEL); return; }
    uint32_t obj = tag_obj(c, TAG_DUEL, pkg);
    if (!obj) return;
    tag_text(c, TAG_DUEL, obj, tx);
    /* above the top of the HUD's middle (the prologue's arrow), in front
     * (the names over cars come with their depth, about 0.99; 1 is not drawn) */
    uint32_t ap[2] = { gvec(g_gmem + TAG_SCR + 0x20u, 0.f, -165.f, 0.5f), 0 };
    rh_call(c, FN_FE_POS, obj, 0, 2, ap);
    /* a long line (a race lobby's) smaller, so that it fits the screen */
    int len = (int)strlen(tx);
    float sc = len > 66 ? 66.f / (float)len : 1.f;
    uint32_t as[2] = { gvec(g_gmem + TAG_SCR + 0x30u, sc, sc, 1.f), 0 };
    rh_call(c, FN_FE_SCALE, obj, 0, 2, as);
    gcall1(c, FN_FE_SHOW, 0, obj);
    g_tag.shown[TAG_DUEL] = 1;
}

/* ---- multiplayer races from free roam: the race spots ------------------------------------------- */
/* A race of the career is raced with other players as the game's own LAN
 * race on the same track (mprace.c); the career never sees it.  Where one can
 * start: every event trigger (type 13, the whole career's) holds at most one
 * race of each stage (the race table's +0x28 marker hash is the trigger's
 * +0x30; EV_04 in stage 4 holds two, an SUV race and another).  The race a
 * spot offers is the one the game's Engage enters there, by its own test
 * (as events_read; Solo too) -- else the one of the player's current stage
 * there (multiplayer only, with the reason) or, when this stage has none
 * there, the latest one the player has won there (multiplayer only), or
 * none: never a race of a stage not reached yet.  A spot is multiplayer
 * only where Engage enters nothing of the game's (a race of another stage
 * still open on the trigger, a cover shoot, another trigger's open event at
 * the same place): elsewhere our lobby would take Enter and close the
 * game's race card.  None in the prologue
 * (stage 0), none while a guided event (a story step) runs, and no story
 * races (type 3; the DDAY events' track 4000 has no LAN race).  Lookups in
 * the career's tables only, a few triggers a frame as events_read. */
#if MPRACES
#define G_RACES         0x008383e0u   /* the career's race table ([0x8383E4] races of 0x88 bytes) */
#define G_NRACES        0x008383e4u
#define RACE_SZ         0x88u
#define C_STAGE         0x7af8u       /* Career: the current stage */
#define C_NEVENTS       0x7afcu       /* Career: the event entries, 8 bytes each: +0 hash, +6 1 won */
#define C_EVENTS        0x7b00u
#define C_OPENRACES     0x71c8u       /* Career: the races open now (64 race records) */
#define C_GUIDED        0xc08u        /* Career: the guided event running (a story step), 0 none */
#define C_STAGEREC      0x72d8u       /* Career: 8 stage records of 0x104 bytes: +0xE8 the sponsor signed */
#define MP_SPOTS        160
enum { MPS_NONE = 0, MPS_OPEN = 1, MPS_ONLY = 2 };                 /* Solo or multiplayer; multiplayer only */
enum { MPW_OPEN = 0, MPW_WON, MPW_LATER, MPW_SPONSOR, MPW_CONTRACT, MPW_URL, MPW_SUV };
static const char *const mpw_text[] = { "", "you won it in your career", "not open in your career yet", "sign with a sponsor first",
    "your sponsor's contract has no place for it", "it opens after more wins", "it takes an SUV" };
typedef struct { uint32_t trig, race; float box[4]; uint8_t kind, why; } MpSpot;
static struct {
    MpSpot   s[MP_SPOTS], nw[MP_SPOTS];
    int      n, nn, seen;
    uint32_t first, end, at, stage;          /* the trigger list swept; the next trigger (0: no sweep now) */
    int64_t  next;
} g_spot;
static uint32_t race_at(uint32_t i){ uint32_t t = rd32(G_RACES); return t && i < rd32(G_NRACES) ? t + i * RACE_SZ : 0; }
/* the race's name as the race table has it ("S2_CIRCUIT_5") */
static void race_name(uint32_t r, char *o, int n){
    uint32_t pool = rd32(G_STRPOOL);
    cstr(r && pool ? pool + MR16(r) : 0, o, n);
}
static uint32_t race_by_hash(uint32_t h){
    uint32_t n = rd32(G_NRACES);
    for (uint32_t i = 0; i < n && i < 1024; i++){ uint32_t r = race_at(i); if (ok(r, RACE_SZ) && MR32(r + 8u) == h) return r; }
    return 0;
}
static int race_won(uint32_t r){
    uint32_t n = rd32(G_CAREER + C_NEVENTS), h = MR32(r + 8u);
    for (uint32_t i = 0; i < n && i < 384; i++)
        if (MR32(G_CAREER + C_EVENTS + 8u * i) == h) return MR8(G_CAREER + C_EVENTS + 8u * i + 6u) == 1;
    return 0;
}
static int race_open(uint32_t r){
    for (uint32_t i = 0; i < 64; i++) if (rd32(G_CAREER + C_OPENRACES + 4u * i) == r) return 1;
    return 0;
}
/* a race a LAN race can be: not a story race (type 3), its first heat's track a network one */
static int race_lan(uint32_t r){ return MR8(r + 0xfu) != 3u && MR8(r + 0xfu) <= 5u && MR16(r + 0x18u) != 4000u && MR8(r + 0x7eu) >= 1u; }
static int car_suv(void){
    uint32_t car = player_car(), ride = car ? rd32(car + 0x10u) : 0, types = rd32(G_CARTYPES);
    uint32_t type = ok(ride, 4) ? MR32(ride) : 0xffffffffu;
    return types && type < 256 && ok(types + type * 0x890u, 0x890) && MR8(types + type * 0x890u + 0x88au);
}
/* why a race of this stage is not open (the career's own gates, 0x532A50's
 * list: the per-type caps and the random pick of the normal races give no
 * reason a player can act on) */
static int race_why(uint32_t r, uint32_t stage){
    if (race_won(r)) return MPW_WON;
    if (MR8(r + 0xdu) && !car_suv()) return MPW_SUV;
    uint8_t kind = MR8(r + 0x34u);
    if (kind == 1) return stage < 8 && !rd32(G_CAREER + C_STAGEREC + stage * 0x104u + 0xe8u) ? MPW_SPONSOR : MPW_CONTRACT;
    if (kind == 2) return MPW_URL;
    return MPW_LATER;
}
/* the spot of trigger e (a type-13 trigger), or MPS_NONE */
static void spot_of(CPU *c, uint32_t e, uint32_t stage, MpSpot *sp){
    memset(sp, 0, sizeof *sp);
    uint32_t mk = MR32(e + 0x30u), n = rd32(G_NRACES), cur = 0, won = 0;
    for (uint32_t i = 0; i < n && i < 1024; i++){
        uint32_t r = race_at(i);
        if (!ok(r, RACE_SZ) || MR32(r + 0x28u) != mk || !race_lan(r)) continue;
        uint32_t st = MR8(r + 0x37u);
        if (st == stage){
            /* EV_04: the open one, else the one the career car fits (SUV or not) */
            if (!cur || (race_open(r) && !race_open(cur)) || (!race_open(cur) && !!MR8(r + 0xdu) == car_suv() && !!MR8(cur + 0xdu) != car_suv())) cur = r;
        } else if (st < stage && race_won(r) && (!won || st > MR8(won + 0x37u))) won = r;
    }
    if (!cur && !won) return;
    /* what Engage enters here, by the game's own test (the guided events and
     * the list, as events_read): the first race of the career's open list at
     * this trigger, of any stage (0x527780), else a cover shoot or another
     * event.  Where it enters one, Enter is the game's: Solo or Multiplayer
     * for its race, or no spot (not a LAN race, not a race) */
    uint32_t out = g_gmem + EV_SCR, a[2] = { out, e };
    rh_call(c, FN_MARKER_WHAT, G_CAREER, 0, 2, a);
    uint32_t game = MR32(out + 4u);
    if (MR32(out) == e && (rh_call(c, FN_MARKER_OPEN, G_CAREER, 0, 1, &out) & 0xffu)){
        if (!game || !race_lan(game)) return;
        sp->race = game; sp->kind = MPS_OPEN; sp->why = MPW_OPEN;
    } else if (cur){ sp->race = cur; sp->kind = MPS_ONLY; sp->why = (uint8_t)race_why(cur, stage); }
    else { sp->race = won; sp->kind = MPS_ONLY; sp->why = MPW_WON; }
    sp->trig = e;
    for (int j = 0; j < 4; j++) sp->box[j] = MRF32(e + 0x20u + 4u * (uint32_t)j);
    if (!(sp->box[0] <= sp->box[2] && sp->box[1] <= sp->box[3])) sp->kind = MPS_NONE;
}
/* the sweep: EV_PER_FRAME triggers a frame, all again every EV_SWEEP_US and
 * when the stage changes; none in the prologue or a guided event */
static void spots_read(CPU *c, int64_t host){
    uint32_t first = rd32(G_TRIGGERS + 0x440u + 4u * TRIG_EVENT), end = rd32(G_TRIGGERS + 0x444u + 4u * TRIG_EVENT);
    uint32_t stage = rd32(G_CAREER + C_STAGE);
    if (first != g_spot.first || end != g_spot.end || stage != g_spot.stage){
        g_spot.first = first; g_spot.end = end; g_spot.stage = stage; g_spot.n = 0; g_spot.at = 0; g_spot.next = 0;
    }
    if (!g_gmem || !(rd32(G_GAMEMODE) & 1u)) return;
    if (!stage || rd32(G_CAREER + C_GUIDED)){ g_spot.n = 0; g_spot.at = 0; return; }
    if (!g_spot.at){
        if (host < g_spot.next || !first || first >= end) return;
        g_spot.at = first; g_spot.nn = 0; g_spot.seen = 0;
    }
    for (int i = 0; i < EV_PER_FRAME && g_spot.at < end && ok(g_spot.at, 0x44); i++){
        uint32_t e = g_spot.at;
        int sz = (int16_t)MR16(e + 0x42u);
        MpSpot sp; spot_of(c, e, stage, &sp);
        if (sp.kind && g_spot.nn < MP_SPOTS) g_spot.nw[g_spot.nn++] = sp;
        g_spot.seen++;
        g_spot.at = sz > 0 ? e + (uint32_t)sz : end;
    }
    if (g_spot.at < end && ok(g_spot.at, 0x44)) return;
    /* a multiplayer-only spot whose marker is in another trigger's open event
     * (two triggers at one place: STAGE_1_SPRINT_1's at Bayview Plaza, won
     * in every career, and the one of the later stages' races there): Engage
     * there enters the game's (0x5EB040 takes the first trigger the car is in
     * that has one), so it is no spot of ours (mp_spot_here: the edges) */
    int open = 0;
    char gone[160] = "";
    for (int i = 0; i < g_spot.nn; i++){
        const float *b = g_spot.nw[i].box;
        if (g_spot.nw[i].kind == MPS_ONLY && in_event((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)){
            size_t u = strlen(gone);
            char nm[32]; race_name(g_spot.nw[i].race, nm, sizeof nm);
            snprintf(gone + u, sizeof gone - u, "%s%s", u ? ", " : "; at an open event of the game's: ", nm);
            g_spot.nw[i--] = g_spot.nw[--g_spot.nn];
            continue;
        }
        open += g_spot.nw[i].kind == MPS_OPEN;
    }
    if (g_spot.nn != g_spot.n) rlog("[roam] f=%ld race spots: %d of the career's %d event triggers (%d open in the career, %d multiplayer only%s)\n",
                                    rh_frame(), g_spot.nn, g_spot.seen, open, g_spot.nn - open, gone);
    memcpy(g_spot.s, g_spot.nw, sizeof g_spot.s);
    g_spot.n = g_spot.nn; g_spot.at = 0; g_spot.next = host + EV_SWEEP_US;
}
#endif

/* ---- multiplayer races from free roam: the lobby ------------------------------------------------ */
/* A player at a race spot opens a lobby there (Multiplayer); the others near
 * it see it (a label over the spot, the world map) and join it with Enter at
 * the spot.  The host starts it with Enter once another player is in: a 5 s
 * countdown, then every game in it drives itself to the game's LAN race on
 * the race's first heat (mprace.c) and back to this spot.  A player leaves a
 * lobby by driving away (MP_STAY_M); the host's leaving closes it.
 * Messages: RC_MSG_LOBBY (roam_core.h), only between games that take them
 * (RC_HF_LOBBY): the host's ADVERT every second to each peer and to the hub
 * (the world map), JOIN and LEAVE to the host, START and CLOSE from it.
 * Lobbies are within a room, as everything in free roam. */
#if MPRACES
#include "mprace.h"
#define MP_JOIN_M       15.f          /* a player within this of a lobby's start joins it with Enter */
#define MP_STAY_M       40.f          /* ...and stays in it while within this (the host too) */
#define MP_ADVERT_US    1000000
#define MP_GONE_US      3500000       /* a lobby, or a player in one, not heard of for this long is gone */
#define MP_COUNT_US     5000000       /* the countdown */
#define MP_ASK_US       5000000       /* a JOIN the host does not take in this long is off */
#define MP_LATE_US      30000000      /* ...but taken this long after it was sent, still in (a connection seconds behind) */
#define MP_AWAY_US      3000000       /* a player of ours gone from free roam or the start this long is out (a hitch is not) */
#define MP_ALONE_US     (300ll * 1000000)   /* a lobby nobody joins closes after this (NFSU2_ROAM_LOBBY_ALONE_S: tests) */
/* a lobby with others in it whose host made no input this long closes (NFSU2_ROAM_LOBBY_IDLE_S: tests): a host gone
 * from the keys kept a full lobby, and the spot, for good (a player's report, 2026-10-26); its last MP_IDLE_WARN_US
 * (at most half of it) every panel of the lobby counts down */
#define MP_IDLE_US      (120ll * 1000000)
#define MP_IDLE_WARN_US 30000000
#define MP_STALE_US     3000000       /* in free roam this long with the trip not over: it is over (mprace_end_in_roam) */
#define MP_NOTE_US      5000000
/* a career with no car of its own yet (a new one's prologue, mprace_car_ok) neither joins nor opens a lobby: the
 * LAN race would build its car from garbage */
#define MP_NOCAR        "Multiplayer races take a car of your own: buy one in your career first"
#define MP_SEEN         16
enum { MP_IDLE, MP_HOST, MP_ASKED, MP_IN, MP_COUNT, MP_TRIP };
static const char *const mp_st[] = { "idle", "hosting", "asked to join", "in a lobby", "counting down", "on the trip" };
/* away_s: its host seems away (rc_lobby): the lobby closes in this many s (0: not) */
typedef struct { uint32_t id, host, race; float x, y; uint8_t n, max, state, away_s; uint32_t ids[RC_LOBBY_MAX]; int64_t heard; } MpLobby;
static struct {
    int      st;
    MpLobby  l;                                  /* ours (hosting) or the one we are in */
    int64_t  since, tx_at, start_at;
    int      engage;                             /* Engage held last frame */
    MpLobby  seen[MP_SEEN]; int nseen;           /* the others' lobbies heard of */
    char     note[TAG_DUEL_MAX]; int64_t note_until;
    uint32_t opened, joined, trips;              /* statistics */
    int64_t  away[RC_LOBBY_MAX];                 /* hosting: since when each player of ours looks gone (0: here) */
    int64_t  alone_at;                           /* hosting: since when nobody else is in it (0: someone is) */
    int64_t  roam_at;                            /* on the trip: since when in free roam with it not over (0: not) */
    int64_t  alone_us;                           /* MP_ALONE_US, or the test's */
    int64_t  input_at;                           /* hosting: our last input (mp_idle; alone: now) */
    uint32_t acts;                               /* ...the game's actions held last frame */
    int64_t  said_at;                            /* our last quick chat pick (say_frame) */
    int64_t  idle_us;                            /* MP_IDLE_US, or the test's */
    uint32_t asked_host, asked_id; int64_t asked_at;   /* the lobby we last sent a JOIN to (its host, its id), and when */
    char     names[RC_LOBBY_MAX][RC_NAME_MAX + 1];   /* on the trip: the lobby's players' names (their links end in the menus) */
    char     lan[RC_LOBBY_MAX][13];              /* ...and their LAN names (lan_names), the race's results' */
} g_mp;
static const char *race_kind(uint32_t r){
    if (!r) return "race";
    uint8_t t = MR8(r + 0xfu);
    return MR8(r + 0x34u) == 2u ? "URL race" : t == 0 ? "circuit" : t == 1 ? "sprint" : t == 2 ? "Street X" : t == 4 ? "drag" :
           t == 5 ? "drift" : "race";
}
/* the network race's type (mprace N_MODE) for a career race: URL 4, circuit 0, sprint 1, Street X 10, drag 2, drift 3 */
static int race_net_mode(uint32_t r){
    uint8_t ty = MR8(r + 0xfu);
    return MR8(r + 0x34u) == 2u ? 4 : ty == 0 ? 0 : ty == 1 ? 1 : ty == 2 ? 0xa : ty == 4 ? 2 : ty == 5 ? 3 : 0;
}
/* the most players a race takes: the game's network race takes 6, drag one a
 * lane (4; 3 on track 4402) */
static int race_max(uint32_t r){ return MR8(r + 0xfu) == 4u ? (MR16(r + 0x18u) == 4402u ? 3 : 4) : RC_LOBBY_MAX; }
/* where a race starts: its event trigger's centre (the spot's, else the
 * first trigger with its marker) */
static int race_place(uint32_t r, float *x, float *y){
    for (int i = 0; i < g_spot.n; i++)
        if (g_spot.s[i].race == r){ *x = (g_spot.s[i].box[0] + g_spot.s[i].box[2]) / 2; *y = (g_spot.s[i].box[1] + g_spot.s[i].box[3]) / 2; return 1; }
    uint32_t e = rd32(G_TRIGGERS + 0x440u + 4u * TRIG_EVENT), end = rd32(G_TRIGGERS + 0x444u + 4u * TRIG_EVENT), mk = MR32(r + 0x28u);
    for (int n = 0; e < end && n < 1024 && ok(e, 0x44); n++){
        if (MR32(e + 0x30u) == mk){ *x = (MRF32(e + 0x20u) + MRF32(e + 0x28u)) / 2; *y = (MRF32(e + 0x24u) + MRF32(e + 0x2cu)) / 2; return 1; }
        int sz = (int16_t)MR16(e + 0x42u);
        if (sz <= 0) break;
        e += (uint32_t)sz;
    }
    return 0;
}
static float mp_dist(float x, float y){
    uint32_t me = player_car();
    if (!me) return 1e9f;
    float dx = MRF32(me + 0x60u) - x, dy = MRF32(me + 0x64u) - y;
    return rc_sqrtf(dx * dx + dy * dy);
}
static void mp_note(int64_t host, const char *t){
    snprintf(g_mp.note, sizeof g_mp.note, "%s", t);
    g_mp.note_until = host + MP_NOTE_US;
    rlog("[roam] f=%ld race lobby: %s\n", rh_frame(), t);
}
static void mp_to(int st, int64_t host){
    if (st != g_mp.st) rlog("[roam] f=%ld race lobby %08x: %s -> %s\n", rh_frame(), g_mp.l.id, mp_st[g_mp.st], mp_st[st]);
    if (st != g_mp.st){ g_mp.alone_at = g_mp.roam_at = 0; g_mp.input_at = host; }
    if (st != g_mp.st && g_mp.st == MP_HOST) g_mp.l.away_s = 0;      /* (the countdown's adverts: not away) */
    g_mp.st = st; g_mp.since = host;
    if (st == MP_IDLE) memset(&g_mp.l, 0, sizeof g_mp.l);
}
static int mp_in(const MpLobby *l, uint32_t id){ for (int i = 0; i < l->n; i++) if (l->ids[i] == id) return 1; return 0; }
/* hosting: player i out of our lobby (its id, since when it looked gone, its name on the trip) */
static void mp_drop(int i){
    for (int k = i; k + 1 < g_mp.l.n; k++){
        g_mp.l.ids[k] = g_mp.l.ids[k + 1]; g_mp.away[k] = g_mp.away[k + 1];
        memcpy(g_mp.names[k], g_mp.names[k + 1], sizeof g_mp.names[k]);
    }
    g_mp.l.n--;
}
static const rc_peer *mp_peer(uint32_t id){
    for (int i = 0; i < RC_TABLE; i++) if (g_s.peer[i].state == RC_PS_EST && g_s.peer[i].id == id) return &g_s.peer[i];
    return 0;
}
static const char *mp_name(uint32_t id){
    if (id == g_s.conf.self_id) return O.name[0] ? O.name : "you";
    const rc_peer *p = mp_peer(id);
    return p && p->name[0] ? p->name : "a player";
}
/* one message about lobby l to `to` (0: the hub); the session lock is ours */
static void mp_send_of(const MpLobby *l, uint8_t op, uint8_t arg, uint32_t to){
    rc_lobby m; memset(&m, 0, sizeof m);
    m.op = op; m.arg = arg; m.to = to; m.lobby = l->id; m.race = MR32(l->race + 8u);
    m.max = l->max; m.state = l->state; m.n = l->n; m.away_s = op == RC_LOBBY_ADVERT ? l->away_s : 0;
    m.x = (int16_t)(l->x < -32767.f ? -32767.f : l->x > 32767.f ? 32767.f : l->x);
    m.y = (int16_t)(l->y < -32767.f ? -32767.f : l->y > 32767.f ? 32767.f : l->y);
    if (op == RC_LOBBY_START){ int64_t t = (g_mp.start_at - roam_plat_now_us()) / 1000; m.t_ms = (uint16_t)(t < 0 ? 0 : t > 60000 ? 60000 : t); }
    memcpy(m.ids, l->ids, sizeof m.ids);
    rc_send_lobby(&g_s, &m);
}
/* one about our lobby (g_mp.l) */
static void mp_send(uint8_t op, uint8_t arg, uint32_t to){ mp_send_of(&g_mp.l, op, arg, to); }
/* the host's lobby to every peer that takes lobbies, and to the hub */
static void mp_send_all(uint8_t op, uint8_t arg){
    roam_plat_lock();
    for (int i = 0; i < RC_TABLE; i++) if (g_s.peer[i].state == RC_PS_EST && g_s.peer[i].lobbies) mp_send(op, arg, g_s.peer[i].id);
    mp_send(op, arg, 0);
    roam_plat_unlock();
}
static void mp_send_one(uint8_t op, uint8_t arg, uint32_t to){ roam_plat_lock(); mp_send(op, arg, to); roam_plat_unlock(); }
/* the trip's race reported to the server's hub, back in free roam (RC_MSG_RACEREP): the race, our place, our car as
 * the game rated it in the race; the server logs it ([race] lines), for evening out the cars */
static float rr_in(float v, float lo, float hi){ return !(v >= lo) ? lo : v > hi ? hi : v; }
static void mp_report(void){
    MpReport r;
    if (!g_mp.l.id || !g_mp.l.race || !mprace_report(&r)) return;
    rc_racerep m; memset(&m, 0, sizeof m);
    m.lobby = g_mp.l.id; m.race = MR32(g_mp.l.race + 8u);
    m.mode = (uint8_t)r.mode; m.laps = (uint8_t)r.laps; m.track = (uint16_t)r.track;
    m.players = (uint8_t)(r.cars < 1 ? 1 : r.cars > RC_LOBBY_MAX ? RC_LOBBY_MAX : r.cars);
    m.place = (uint8_t)(r.place < 0 || r.place > m.players ? 0 : r.place);
    m.flags = (uint8_t)((r.host ? RC_RR_HOST : 0) | (r.quit ? RC_RR_QUIT : 0));
    m.type = (uint8_t)r.car.type; m.cls = (int8_t)(r.car.cls < -1 || r.car.cls > 3 ? -1 : r.car.cls);
    m.rating = rr_in(r.car.rating, 0.f, 5000.f); m.field = rr_in(r.field, 0.f, 5000.f); m.factor = rr_in(r.factor, 0.01f, 100.f);
    m.kw = rr_in(r.car.power / 1000.f, 0.f, 10000.f); m.kg = rr_in(r.car.mass, 0.f, 50000.f); m.kmh = rr_in(r.car.top * 3.6f, 0.f, 2000.f);
    m.time = rr_in(r.time, 0.f, 100000.f);
    for (int i = 0; i < RC_RACEREP_NAME && r.car.name[i]; i++){ char ch = r.car.name[i]; m.name[i] = ch >= 0x20 && ch < 0x7f ? ch : '?'; }
    /* and the race's results as our game showed them, each row by its player's id (the LAN names the trip gave the
     * lobby's players): the server compares every player's results for a racer far ahead of the others */
    MpResult res[MPRACE_RESULTS_MAX]; int nr = mprace_results(res, MPRACE_RESULTS_MAX);
    rc_raceres q; memset(&q, 0, sizeof q);
    q.lobby = m.lobby; q.race = m.race; q.mode = m.mode; q.laps = m.laps;
    for (int k = 0; k < nr && q.n < RC_LOBBY_MAX; k++){
        uint32_t id = 0;
        for (int i = 0; i < g_mp.l.n && i < RC_LOBBY_MAX; i++)
            if (res[k].name[0] && !strcasecmp(g_mp.lan[i], res[k].name)) id = g_mp.l.ids[i];
        if (res[k].ours) id = g_s.conf.self_id;
        q.row[q.n].id = id;
        q.row[q.n].flags = (uint8_t)((res[k].finished ? RC_RS_FINISHED : 0) | (res[k].ours ? RC_RS_OURS : 0));
        q.row[q.n].time = rr_in(res[k].clock, 0.f, 100000.f);
        q.row[q.n].done = rr_in(res[k].done, 0.f, 1000.f);
        q.row[q.n].dist = rr_in(res[k].dist, 0.f, 1e6f);
        q.n++;
    }
    roam_plat_lock(); int sent = rc_send_racerep(&g_s, &m), sent_res = q.n && rc_send_raceres(&g_s, &q); roam_plat_unlock();
    rlog("[roam] f=%ld race lobby %08x: its race reported to the server%s: place %d of %d, %s rated %.1f (the cars' average %.1f); "
         "its results, %d rows%s\n", rh_frame(), m.lobby, sent ? "" : " (not sent: no hub)", m.place, m.players, m.name, (double)m.rating,
         (double)m.field, q.n, sent_res ? "" : " (not sent)");
}
/* we open a lobby for race r */
static int mp_open(int64_t host, uint32_t r){
    if (g_mp.st != MP_IDLE || !r) return 0;
    memset(&g_mp.l, 0, sizeof g_mp.l);
    roam_plat_lock(); do g_mp.l.id = roam_plat_random(); while (!g_mp.l.id); roam_plat_unlock();
    g_mp.l.host = g_s.conf.self_id; g_mp.l.race = r; g_mp.l.max = (uint8_t)race_max(r); g_mp.l.state = RC_LS_OPEN;
    g_mp.l.n = 1; g_mp.l.ids[0] = g_s.conf.self_id;
    memset(g_mp.away, 0, sizeof g_mp.away);
    if (!race_place(r, &g_mp.l.x, &g_mp.l.y)) return 0;
    g_mp.opened++;
    char nm[40]; race_name(r, nm, sizeof nm);
    rlog("[roam] f=%ld race lobby %08x: opened for %s (%s, at most %d players) at %.0f %.0f\n", rh_frame(), g_mp.l.id, nm, race_kind(r),
         g_mp.l.max, (double)g_mp.l.x, (double)g_mp.l.y);
    mp_to(MP_HOST, host);
    mp_send_all(RC_LOBBY_ADVERT, 0); g_mp.tx_at = host;
    return 1;
}
static void mp_close(int64_t host, uint8_t why, const char *note){
    if (g_mp.st == MP_HOST || (g_mp.st == MP_COUNT && g_mp.l.host == g_s.conf.self_id)){
        g_mp.l.n = 0;
        for (int k = 0; k < 3; k++) mp_send_all(RC_LOBBY_CLOSE, why);
    } else if (g_mp.st == MP_ASKED || g_mp.st == MP_IN || g_mp.st == MP_COUNT){
        for (int k = 0; k < 3; k++) mp_send_one(RC_LOBBY_LEAVE, why, g_mp.l.host);
    }
    if (note) mp_note(host, note);
    mp_to(MP_IDLE, host);
}
/* a lobby heard of from its host */
static MpLobby *mp_seen(uint32_t host_id, int make){
    for (int i = 0; i < g_mp.nseen; i++) if (g_mp.seen[i].host == host_id) return &g_mp.seen[i];
    if (!make) return 0;
    int k = g_mp.nseen < MP_SEEN ? g_mp.nseen++ : 0;
    if (k == 0 && g_mp.nseen == MP_SEEN) for (int i = 1; i < MP_SEEN; i++) if (g_mp.seen[i].heard < g_mp.seen[k].heard) k = i;   /* the stalest goes */
    memset(&g_mp.seen[k], 0, sizeof g_mp.seen[k]);
    return &g_mp.seen[k];
}
static void mp_forget(uint32_t host_id){
    for (int i = 0; i < g_mp.nseen; i++) if (g_mp.seen[i].host == host_id){ g_mp.seen[i] = g_mp.seen[--g_mp.nseen]; return; }
}
/* the race spot the player's car is in (its trigger's box), or 0; a
 * multiplayer-only one not where the car is in an open event of the game's
 * too (Enter is the game's there: spots_read) */
static const MpSpot *mp_spot_here(void){
    uint32_t me = player_car();
    if (!me) return 0;
    float x = MRF32(me + 0x60u), y = MRF32(me + 0x64u);
    const MpSpot *only = 0;
    for (int i = 0; i < g_spot.n; i++){
        const float *b = g_spot.s[i].box;
        if (!(x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3])) continue;
        if (g_spot.s[i].kind == MPS_OPEN) return &g_spot.s[i];
        if (!only) only = &g_spot.s[i];
    }
    return only && !in_event(x, y) ? only : 0;
}
/* the lobby nearest the player that it may join now: open, room in it, its start within MP_JOIN_M, its host linked
 * with us (the hub links a lobby's host with the players near its start first: a JOIN goes over that link) */
static MpLobby *mp_joinable(void){
    MpLobby *best = 0; float bd = MP_JOIN_M;
    for (int i = 0; i < g_mp.nseen; i++){
        MpLobby *l = &g_mp.seen[i];
        if (l->state != RC_LS_OPEN || l->n >= l->max || !l->race || !mp_peer(l->host)) continue;
        float d = mp_dist(l->x, l->y);
        if (d < bd){ bd = d; best = l; }
    }
    return best;
}
/* the JOIN to the host, and its copy to the hub: the hub links us with the host first (were it not linking us, the
 * JOIN would go nowhere) */
static void mp_ask_in(void){ roam_plat_lock(); mp_send(RC_LOBBY_JOIN, 0, g_mp.l.host); mp_send(RC_LOBBY_JOIN, 0, 0); roam_plat_unlock(); }
static int mp_join(int64_t host, MpLobby *l){
    if (g_mp.st != MP_IDLE || !l) return 0;
    if (!mprace_car_ok()){ mp_note(host, MP_NOCAR); return 0; }
    g_mp.l = *l;
    g_mp.asked_host = l->host; g_mp.asked_id = l->id; g_mp.asked_at = host;
    mp_to(MP_ASKED, host);
    mp_ask_in(); g_mp.tx_at = host;
    return 1;
}
/* one LOBBY message for us */
static void mp_recv(int64_t host, uint32_t from, const rc_lobby *m){
    uint32_t self = g_s.conf.self_id;
    uint32_t r = race_by_hash(m->race);
    switch (m->op){
    case RC_LOBBY_ADVERT:
    case RC_LOBBY_START: {
        if (!r){ rlog("[roam] f=%ld race lobby: %s's race %08x is not in this game's table\n", rh_frame(), mp_name(from), m->race); return; }
        MpLobby *l = mp_seen(from, 0);
        if (!l && m->state == RC_LS_OPEN && m->n < m->max){
            /* a lobby we hear of first: a message over the game */
            char t[64]; snprintf(t, sizeof t, " is starting a multiplayer %s nearby", race_kind(r));
            roam_plat_lock(); roam_plat_notice(mp_name(from), t); roam_plat_unlock();
        }
        if (!l) l = mp_seen(from, 1);
        l->id = m->lobby; l->host = from; l->race = r; l->x = m->x; l->y = m->y; l->n = m->n; l->max = m->max; l->state = m->state;
        l->away_s = m->op != RC_LOBBY_ADVERT ? 0 : m->away_s > MP_IDLE_WARN_US / 1000000 ? MP_IDLE_WARN_US / 1000000 : m->away_s;
        memcpy(l->ids, m->ids, sizeof l->ids); l->heard = host;
        if ((g_mp.st == MP_ASKED || g_mp.st == MP_IN || g_mp.st == MP_COUNT) && g_mp.l.host == from && g_mp.l.id == m->lobby){
            if (l->away_s && !g_mp.l.away_s) rlog("[roam] f=%ld race lobby %08x: %s seems away, it closes in %d s\n", rh_frame(), l->id, mp_name(from), l->away_s);
            if (!l->away_s && g_mp.l.away_s) rlog("[roam] f=%ld race lobby %08x: %s is back\n", rh_frame(), l->id, mp_name(from));
            g_mp.l = *l;
            int in = mp_in(l, self);
            if (g_mp.st == MP_ASKED && in){ g_mp.joined++; mp_to(MP_IN, host); }
            else if (g_mp.st != MP_ASKED && !in){ mp_note(host, "You are no longer in the race"); mp_to(MP_IDLE, host); }
            if (m->op == RC_LOBBY_START && in && g_mp.st != MP_COUNT){
                g_mp.start_at = host + (int64_t)m->t_ms * 1000; mp_to(MP_COUNT, host);
            }
        } else if (g_mp.st == MP_IDLE && mp_in(l, self)){
            /* our JOIN taken after we gave up asking (a connection seconds behind: on the live server hundreds
             * were): in, while still at its start and the lobby open; else out again, or the host would keep a
             * player who is not, and wait for it in the LAN race's room (one starting already: its countdown
             * and adverts list us).  Only the lobby we asked to join, lately: any other that lists us is a
             * host's mistake, or a player pulling others into its race, and then off on its trip (the third
             * security audit) */
            int asked = from == g_mp.asked_host && m->lobby == g_mp.asked_id && host - g_mp.asked_at <= MP_LATE_US;
            int open = m->op == RC_LOBBY_ADVERT && m->state == RC_LS_OPEN;
            if (open && asked && mp_dist(l->x, l->y) <= MP_STAY_M){ g_mp.asked_host = 0; g_mp.l = *l; g_mp.joined++; mp_to(MP_IN, host); }
            else { roam_plat_lock(); mp_send_of(l, RC_LOBBY_LEAVE, RC_LL_LEFT, from); roam_plat_unlock(); }   /* about its lobby: ours is none */
        }
        return; }
    case RC_LOBBY_JOIN:
        if (g_mp.st != MP_HOST || m->lobby != g_mp.l.id) return;
        if (mp_in(&g_mp.l, from)){ mp_send_one(RC_LOBBY_ADVERT, 0, from); return; }            /* our answer was lost */
        if (g_mp.l.n >= g_mp.l.max || g_mp.l.state != RC_LS_OPEN){ mp_send_one(RC_LOBBY_LEAVE, RC_LL_FULL, from); return; }
        g_mp.away[g_mp.l.n] = 0; g_mp.l.ids[g_mp.l.n++] = from;
        rlog("[roam] f=%ld race lobby %08x: %s joined, %d of %d\n", rh_frame(), g_mp.l.id, mp_name(from), g_mp.l.n, g_mp.l.max);
        mp_send_all(RC_LOBBY_ADVERT, 0); g_mp.tx_at = host;
        return;
    case RC_LOBBY_LEAVE:
        /* hosting, also counting down and on the trip: a player that left is not waited for in the LAN race's room */
        if ((g_mp.st == MP_HOST || g_mp.st == MP_COUNT || g_mp.st == MP_TRIP) && g_mp.l.host == self && m->lobby == g_mp.l.id &&
            from != self && mp_in(&g_mp.l, from)){
            for (int i = 1; i < g_mp.l.n; i++) if (g_mp.l.ids[i] == from){ mp_drop(i); break; }
            rlog("[roam] f=%ld race lobby %08x: %s left, %d of %d\n", rh_frame(), g_mp.l.id, mp_name(from), g_mp.l.n, g_mp.l.max);
            if (g_mp.st == MP_TRIP) mprace_player_gone();
            else { mp_send_all(RC_LOBBY_ADVERT, 0); g_mp.tx_at = host; }
        } else if ((g_mp.st == MP_ASKED || g_mp.st == MP_IN || g_mp.st == MP_COUNT) && from == g_mp.l.host && m->lobby == g_mp.l.id){
            mp_note(host, m->arg == RC_LL_FULL ? "The race is full" : "You are no longer in the race");
            mp_to(MP_IDLE, host);
        }
        return;
    case RC_LOBBY_CLOSE:
        mp_forget(from);
        if ((g_mp.st == MP_ASKED || g_mp.st == MP_IN || g_mp.st == MP_COUNT) && from == g_mp.l.host && m->lobby == g_mp.l.id){
            char t[TAG_DUEL_MAX];
            snprintf(t, sizeof t, g_mp.l.away_s ? "%s seemed away: the race is called off" : "%s called off the race", mp_name(from));
            mp_note(host, t); mp_to(MP_IDLE, host);
        }
        return;
    }
}
/* a player's LAN name: the letters and digits of its name in free roam, 12
 * at most (the game's name entry takes no more), "Player" for none */
static void lan_name(const char *src, char *o){
    int n = 0;
    for (; *src && n < 12; src++) if ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') || (*src >= '0' && *src <= '9')) o[n++] = *src;
    o[n] = 0;
    if (!n) snprintf(o, 13, "Player");
}
/* the lobby's players' LAN names, the host first: one already taken by an
 * earlier player gets its place's digit at its end, again while taken ("Bob",
 * "Bob2", "Bob22"; a LAN server takes each name once).  At 12 characters that
 * changes nothing (two players named "AAAAAAAAAAA2" hung every game of their
 * lobby here: the third security audit), so then its last character is the
 * first of the other digits and letters that is free (there are more than
 * players).  Every game of the lobby works them out alike. */
static int lan_taken(char nm[RC_LOBBY_MAX][13], int i){
    for (int j = 0; j < i; j++) if (!strcasecmp(nm[i], nm[j])) return 1;
    return 0;
}
static void lan_names(char nm[RC_LOBBY_MAX][13]){
    static const char other[] = "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int i = 0; i < g_mp.l.n && i < RC_LOBBY_MAX; i++){
        lan_name(mp_name(g_mp.l.ids[i]), nm[i]);
        for (int k = 0; k < 12 && lan_taken(nm, i); k++){
            int len = (int)strlen(nm[i]); if (len > 11) len = 11;
            nm[i][len] = (char)('1' + i); nm[i][len + 1] = 0;
        }
        for (int k = 0; other[k] && lan_taken(nm, i); k++) nm[i][11] = other[k];   /* 12 characters by now */
    }
}
/* the trip, with the lobby's race */
static void mp_trip(int64_t host){
    MpRace t; memset(&t, 0, sizeof t);
    uint32_t r = g_mp.l.race;
    t.host = g_mp.l.host == g_s.conf.self_id;
    const rc_peer *hp = t.host ? 0 : mp_peer(g_mp.l.host);
    if (!t.host && !hp){ mp_note(host, "The race is off: its host is gone"); mp_to(MP_IDLE, host); return; }
    t.host_ip = hp ? hp->ip : 0;
    char nm12[RC_LOBBY_MAX][13]; lan_names(nm12);
    snprintf(t.host_name, sizeof t.host_name, "%s", nm12[0]);
    for (int i = 0; i < g_mp.l.n; i++) if (g_mp.l.ids[i] == g_s.conf.self_id) snprintf(t.name, sizeof t.name, "%s", nm12[i]);
    if (!t.name[0]){ mp_note(host, "The race is off: you are not in it"); mp_to(MP_IDLE, host); return; }
    snprintf(t.password, sizeof t.password, "%08x", g_mp.l.id);
    t.race = MR32(r + 8u);
    t.mode = race_net_mode(r);
    t.track = MR16(r + 0x18u); t.reverse = MR8(r + 0x1au) & 1u; t.laps = MR8(r + 0x1bu);
    t.players = g_mp.l.n;
    for (int i = 0; i < g_mp.l.n && i < RC_LOBBY_MAX; i++){
        snprintf(g_mp.names[i], sizeof g_mp.names[i], "%s", mp_name(g_mp.l.ids[i]));
        memcpy(g_mp.lan[i], nm12[i], sizeof g_mp.lan[i]);
    }
    char nm[40]; race_name(r, nm, sizeof nm);
    rlog("[roam] f=%ld race lobby %08x: the trip to the LAN race: %s (mode %d, track %d%s, %d laps), %d players, %s\n", rh_frame(), g_mp.l.id, nm,
         t.mode, t.track, t.reverse ? " reverse" : "", t.laps, t.players, t.host ? "we host" : "we join");
    if (!mprace_start(&t)){ mp_note(host, "The race could not start"); mp_close(host, RC_LL_GONE, 0); return; }
    g_mp.trips++;
    mp_to(MP_TRIP, host);
}
/* the player pressed Engage (or chose Multiplayer) at a spot of race r: our
 * lobby there, or the one waiting there */
static int mp_engage_spot(int64_t host, uint32_t r){
    if (!mprace_car_ok()){ mp_note(host, MP_NOCAR); return 0; }
    MpLobby *l = mp_joinable();
    if (l) return mp_join(host, l);
    return mp_open(host, r);
}
/* hosting: players gone from free roam, or away from the start, for MP_AWAY_US (a player who just joined, whose car
 * we may not see yet, or a hitch in its snapshots, is not) are out */
static void mp_away(int64_t host){
    for (int i = 1; i < g_mp.l.n; i++){
        const PeerView *v = du_pv(g_mp.l.ids[i]);
        float far = v && v->have_pose ? rc_sqrtf((v->pose.p[0] - g_mp.l.x) * (v->pose.p[0] - g_mp.l.x) +
                                                 (v->pose.p[1] - g_mp.l.y) * (v->pose.p[1] - g_mp.l.y)) : 0.f;
        int gone = !v || !v->visible || far > MP_STAY_M + 10.f;
        if (!gone){ g_mp.away[i] = 0; continue; }
        if (!g_mp.away[i]) g_mp.away[i] = host;
        if (host - g_mp.away[i] < MP_AWAY_US) continue;
        rlog("[roam] f=%ld race lobby %08x: %s is gone from it (%s)\n", rh_frame(), g_mp.l.id, mp_name(g_mp.l.ids[i]),
             !v ? "not linked" : !v->visible ? "not in free roam" : "away from the start");
        mp_send_one(RC_LOBBY_LEAVE, RC_LL_LEFT, g_mp.l.ids[i]);
        mp_drop(i--);
        g_mp.tx_at = 0;
    }
}
/* hosting: 1 when we made no input for g_mp.idle_us with others in the lobby (the clock waits while we are alone:
 * MP_ALONE_US is that lobby's end).  Input: the game's actions changing (keys, a pad's buttons and stick), the car
 * moving (while the world runs: paused its velocity is the last one), a quick chat pick.  Its last MP_IDLE_WARN_US
 * goes out in the adverts (away_s), for every panel of the lobby */
static int mp_idle(int64_t host, int world_runs){
    uint32_t acts = rd32(G_ACTIONS);
    float v = rc_sqrtf(g_lv[0] * g_lv[0] + g_lv[1] * g_lv[1] + g_lv[2] * g_lv[2]);
    int input = acts != g_mp.acts || (world_runs && v >= IDLE_MPS) || g_mp.said_at > g_mp.input_at;
    g_mp.acts = acts;
    if (g_mp.l.away_s && input) rlog("[roam] f=%ld race lobby %08x: input from us again: it stays\n", rh_frame(), g_mp.l.id);
    if (input || g_mp.l.n < 2) g_mp.input_at = host;
    int64_t idle = host - g_mp.input_at, warn = g_mp.idle_us / 2 < MP_IDLE_WARN_US ? g_mp.idle_us / 2 : MP_IDLE_WARN_US;
    if (idle >= g_mp.idle_us) return 1;
    int s = idle < g_mp.idle_us - warn ? 0 : (int)((g_mp.idle_us - idle + 999999) / 1000000);
    if (s && !g_mp.l.away_s) rlog("[roam] f=%ld race lobby %08x: no input from us for %.0f s: it closes in %d s\n", rh_frame(), g_mp.l.id,
                                  (double)idle / 1e6, s);
    if (s != g_mp.l.away_s) g_mp.tx_at = 0;                          /* the panels at once (and each second) */
    g_mp.l.away_s = (uint8_t)s;
    return 0;
}
static void mp_ui_frame(CPU *c, int64_t host, int in_roam, int press, const MpSpot *sp);
/* per frame */
static void mp_frame(CPU *c, int64_t host, int in_roam, int world_runs, const uint32_t *from, const rc_lobby *in, int nin){
    for (int i = 0; i < nin; i++) mp_recv(host, from[i], &in[i]);
    for (int i = 0; i < g_mp.nseen; i++) if (host - g_mp.seen[i].heard > MP_GONE_US) mp_forget(g_mp.seen[i--].host);
    int eng = in_roam && world_runs && !(rd32(G_ACTIONS) & (1u << ACT_ENGAGE));
    int press = eng && !g_mp.engage;
    g_mp.engage = eng;
    uint32_t quit = __atomic_exchange_n(&g_quit_id, 0, __ATOMIC_RELAXED);
    /* hosting, counting down or on the trip: a player of ours who quit is out at once (not waited for in the room) */
    if (quit && quit != g_s.conf.self_id && g_mp.l.host == g_s.conf.self_id && (g_mp.st == MP_HOST || g_mp.st == MP_COUNT || g_mp.st == MP_TRIP))
        for (int i = 1; i < g_mp.l.n; i++) if (g_mp.l.ids[i] == quit){
            rlog("[roam] f=%ld race lobby %08x: %s quit, %d of %d\n", rh_frame(), g_mp.l.id, mp_name(quit), g_mp.l.n - 1, g_mp.l.max);
            mp_drop(i);
            if (g_mp.st == MP_TRIP) mprace_player_gone(); else g_mp.tx_at = 0;
            break;
        }
    if (g_mp.st == MP_TRIP){
        int ph = mprace_phase();
        if (quit && quit == g_mp.l.host && quit != g_s.conf.self_id) mprace_host_gone();   /* on the way to the race: its server is gone */
        /* free roam again with the trip not over (its own end comes within frames of the HUD): a trip that
         * lost its way kept the lobby, and every race card closed itself until a restart */
        if (in_roam && ph != MPR_OUT && ph != MPR_DONE && ph != MPR_FAILED){
            if (!g_mp.roam_at) g_mp.roam_at = host;
            if (host - g_mp.roam_at > MP_STALE_US){ rlog("[roam] f=%ld race lobby %08x: in free roam, the trip not over: over\n", rh_frame(), g_mp.l.id); mprace_end_in_roam(); ph = mprace_phase(); }
        } else g_mp.roam_at = 0;
        if (ph != MPR_DONE && ph != MPR_FAILED) return;
        if (!in_roam) return;                                     /* back once free roam is */
        char t[TAG_DUEL_MAX];
        int place = mprace_place(), prize = mprace_prize();
        if (ph == MPR_FAILED) snprintf(t, sizeof t, "The multiplayer race did not happen: %s", mprace_why());
        else if (place && prize) snprintf(t, sizeof t, "Multiplayer race: %d%s place, $%d", place, place == 1 ? "st" : place == 2 ? "nd" : place == 3 ? "rd" : "th", prize);
        else if (place) snprintf(t, sizeof t, "Multiplayer race: %d%s place", place, place == 1 ? "st" : place == 2 ? "nd" : place == 3 ? "rd" : "th");
        else if (mprace_quit() && mprace_fee()) snprintf(t, sizeof t, "Multiplayer race: you quit it, $%d fee", mprace_fee());
        else if (mprace_quit()) snprintf(t, sizeof t, "Multiplayer race: you quit it");
        else snprintf(t, sizeof t, "Multiplayer race: not finished");
        float ef = mprace_factor();
        if (ph != MPR_FAILED && (ef > 1.005f || ef < 0.995f)){
            size_t n = strlen(t);
            snprintf(t + n, sizeof t - n, " (your engine %+.0f%% to even out the cars)", (double)((ef - 1.f) * 100.f));
        }
        mp_report();
        mprace_ack();
        mp_note(host, t); mp_to(MP_IDLE, host);
        return;
    }
    if (!in_roam){
        if (g_mp.st != MP_IDLE) mp_close(host, RC_LL_LEFT, "You left the race");      /* the menus, a shop, a race of the game's */
        return;
    }
    switch (g_mp.st){
    case MP_IDLE: {
        /* Engage at a multiplayer-only spot: its question (mp_ui_frame); at
         * an open race's marker the game's own card comes, and its Accept
         * asks Solo or Multiplayer (mp_card); by a lobby elsewhere: join */
        const MpSpot *sp = mp_spot_here();
        mp_ui_frame(c, host, in_roam, press, sp);
        if (press && !sp && g_mp.st == MP_IDLE){ MpLobby *l = mp_joinable(); if (l) mp_join(host, l); }
        break; }
    case MP_HOST:
        if (mp_dist(g_mp.l.x, g_mp.l.y) > MP_STAY_M){ mp_close(host, RC_LL_GONE, "You drove away: the race is called off"); break; }
        if (g_mp.l.n < 2){
            if (!g_mp.alone_at) g_mp.alone_at = host;
            if (host - g_mp.alone_at > g_mp.alone_us){
                char t[TAG_DUEL_MAX]; int s = (int)(g_mp.alone_us / 1000000);
                if (s % 60) snprintf(t, sizeof t, "Nobody joined in %d s: the lobby is closed", s);
                else snprintf(t, sizeof t, "Nobody joined in %d minutes: the lobby is closed", s / 60);
                mp_close(host, RC_LL_GONE, t);
                break;
            }
        } else g_mp.alone_at = 0;
        mp_away(host);
        if (mp_idle(host, world_runs)){ mp_close(host, RC_LL_GONE, "You seemed away: the lobby is closed"); break; }
        if (press && g_mp.l.n >= 2){
            g_mp.l.state = RC_LS_STARTING; g_mp.start_at = host + MP_COUNT_US;
            mp_to(MP_COUNT, host);
            for (int k = 0; k < 3; k++) for (int i = 1; i < g_mp.l.n; i++) mp_send_one(RC_LOBBY_START, 0, g_mp.l.ids[i]);
            g_mp.tx_at = 0;
        }
        if (host - g_mp.tx_at >= MP_ADVERT_US){ mp_send_all(RC_LOBBY_ADVERT, 0); g_mp.tx_at = host; }
        break;
    case MP_ASKED:
        if (host - g_mp.since > MP_ASK_US){ mp_note(host, "The race did not take you"); mp_to(MP_IDLE, host); break; }
        if (host - g_mp.tx_at >= 500000){ mp_ask_in(); g_mp.tx_at = host; }
        break;
    case MP_IN:
        if (host - g_mp.l.heard > MP_GONE_US){ mp_note(host, "The race is off: its host is gone"); mp_to(MP_IDLE, host); break; }
        if (mp_dist(g_mp.l.x, g_mp.l.y) > MP_STAY_M){ mp_close(host, RC_LL_LEFT, "You drove away: you left the race"); break; }
        break;
    case MP_COUNT:
        if (g_mp.l.host == g_s.conf.self_id){
            /* the others' leaving and going counts until the trip: those left are the ones the LAN race waits for */
            mp_away(host);
            if (g_mp.l.n < 2){
                g_mp.l.state = RC_LS_OPEN; mp_to(MP_HOST, host); mp_note(host, "Everyone else left: the race waits for players again");
                mp_send_all(RC_LOBBY_ADVERT, 0); g_mp.tx_at = host;
                break;
            }
            if (host - g_mp.tx_at >= MP_ADVERT_US){ mp_send_all(RC_LOBBY_ADVERT, 0); g_mp.tx_at = host; }
        }
        if (host >= g_mp.start_at) mp_trip(host);
        break;
    }
}
/* the lobby's line of text, top centre (du_hud shows it before the outrun's),
 * where the platform shows no panel (mp_panel) */
static int g_mp_panel = -1;                   /* the platform's panel: 1 the page shows it, 0 none, -1 not known yet */
static int mp_line(int64_t host, char *tx, int n){
    char nm[24];
    if (g_mp_panel == 1) return 0;
    switch (g_mp.st){
    case MP_HOST:
        if (g_mp.l.n < 2) snprintf(tx, (size_t)n, "Multiplayer %s: waiting for players (drive away to call it off)", race_kind(g_mp.l.race));
        else if (g_mp.l.away_s) snprintf(tx, (size_t)n, "You seem away: the lobby closes in %d s unless you press a key", g_mp.l.away_s);
        else snprintf(tx, (size_t)n, "Multiplayer %s: %d of %d players. Press Enter to start", race_kind(g_mp.l.race), g_mp.l.n, g_mp.l.max);
        return 1;
    case MP_ASKED:
        snprintf(tx, (size_t)n, "Joining %s's race...", mp_name(g_mp.l.host));
        return 1;
    case MP_IN:
        snprintf(nm, sizeof nm, "%s", mp_name(g_mp.l.host));
        if (g_mp.l.away_s) snprintf(tx, (size_t)n, "%s seems away: the lobby closes in %d s", nm, g_mp.l.away_s);
        else snprintf(tx, (size_t)n, "%s's %s: %d of %d players. Waiting for %s to start", nm, race_kind(g_mp.l.race), g_mp.l.n, g_mp.l.max, nm);
        return 1;
    case MP_COUNT: {
        int64_t s = (g_mp.start_at - host + 999999) / 1000000;
        snprintf(tx, (size_t)n, "The race starts in %d", (int)(s < 0 ? 0 : s));
        return 1; }
    case MP_IDLE: {
        const MpSpot *sp = mp_spot_here();
        MpLobby *l = mp_joinable();
        if (l && (!sp || sp->kind == MPS_ONLY)){
            if (!mprace_car_ok()) snprintf(tx, (size_t)n, "%s's %s: %s", mp_name(l->host), race_kind(l->race), MP_NOCAR);
            else snprintf(tx, (size_t)n, "Press Enter to join %s's %s (%d of %d). Quitting the race costs $%d%s", mp_name(l->host), race_kind(l->race),
                          l->n, l->max, MPRACE_QUIT_FEE, l->race && mprace_evened(race_net_mode(l->race)) ? ". The cars are evened out" : "");
            return 1;
        }
        if (sp && sp->kind == MPS_ONLY){
            /* a lobby here with no room left: said, as Enter starts another */
            for (int i = 0; i < g_mp.nseen; i++){
                const MpLobby *f = &g_mp.seen[i];
                if (f->state != RC_LS_OPEN || !f->race || f->n < f->max || mp_dist(f->x, f->y) >= MP_JOIN_M) continue;
                snprintf(tx, (size_t)n, "%s's %s is full (%d of %d). Press Enter to start another", mp_name(f->host), race_kind(f->race), f->n, f->max);
                return 1;
            }
            snprintf(tx, (size_t)n, "Multiplayer %s here (%s): press Enter to open a lobby", race_kind(sp->race), mpw_text[sp->why]);
            return 1;
        }
        break; }
    }
    if (g_mp.note[0] && host < g_mp.note_until){ snprintf(tx, (size_t)n, "%s", g_mp.note); return 1; }
    return 0;
}
/* no outrun in a lobby, at a multiplayer-only spot or by a lobby to join:
 * Enter there is for the race (as at the game's own markers, du_free) */
static int mp_busy(void){
    if (g_mp.st != MP_IDLE) return 1;
    const MpSpot *sp = mp_spot_here();
    return (sp && sp->kind == MPS_ONLY) || mp_joinable();
}

/* The lobby's panel over the game (the page's, roam_plat_lobby): what the
 * top line says where there is none (mp_line), and the lobby's players in
 * its six places, and during the trip a cover over the game's menus (the
 * race and its results stay the game's own).  Its state as a JSON object,
 * sent when it changes ("{}": none):
 *   v      "spot" a multiplayer-only spot: Enter opens a lobby (why: the reason it is not open);
 *          "join" a lobby here to join; "full" the lobby here is full, Enter opens another;
 *          "lobby" ours, or the one we are in (st: host, asked, in, count; t: the countdown's seconds; away: its host
 *          seems away, the seconds until it closes);
 *          "trip" the way to the race and back (step: where it is); "note" a message (text)
 *   kind, track, laps, stage   the race; names: the lobby's players, the host first; me: our place (-1: none); max */
/* a track's name in the game's language, as UTF-8 (discord.c's track_name): TRACK_NAME_<n>, or
 * REVERSE_TRACK_NAME_<n>, in the game's text table ({hash, text} sorted by hash), for a track of its list */
#define G_TRACKS        0x00883da0u   /* TrackInfo[], 0x128 bytes: +0x8a the int16 number */
#define G_TRACKS_N      0x00883da4u
#define G_TEXTS         0x008383d8u
#define G_TEXTS_N       0x008383d0u
static int game_text(const char *key, char *o, int n){
    uint32_t tab = rd32(G_TEXTS), cnt = rd32(G_TEXTS_N), h = bhash(key), lo = 0, hi = cnt;
    o[0] = 0;
    if (!tab || !cnt || cnt > 200000u || !ok(tab, cnt * 8u)) return 0;
    while (lo < hi){
        uint32_t mid = lo + (hi - lo) / 2, mh = MR32(tab + 8u * mid);
        if (mh < h){ lo = mid + 1; continue; }
        if (mh > h){ hi = mid; continue; }
        uint32_t s = MR32(tab + 8u * mid + 4u); int k = 0;
        for (uint32_t i = 0; i < 64 && ok(s + i, 1) && MR8(s + i) && k + 3 < n; i++){
            uint8_t ch = MR8(s + i);
            if (ch < 0x80) o[k++] = (char)(ch < 0x20 ? ' ' : ch);
            else if (ch >= 0xa0){ o[k++] = (char)(0xc0 | ch >> 6); o[k++] = (char)(0x80 | (ch & 0x3f)); }   /* Latin-1 */
        }
        o[k] = 0;
        return k > 0;
    }
    return 0;
}
static void track_name(uint32_t num, int reverse, char *o, int n){
    uint32_t tab = rd32(G_TRACKS), cnt = rd32(G_TRACKS_N);
    o[0] = 0;
    if (!tab || !cnt || cnt > 2000u || !ok(tab, cnt * 0x128u)) return;
    for (uint32_t i = 0; i < cnt; i++){
        int16_t id = (int16_t)MR16(tab + i * 0x128u + 0x8au);
        if ((uint32_t)(int32_t)id != num) continue;
        char key[48];
        snprintf(key, sizeof key, "REVERSE_TRACK_NAME_%d", id);
        if (reverse && game_text(key, o, n)) return;
        snprintf(key, sizeof key, "TRACK_NAME_%d", id);
        game_text(key, o, n);
        return;
    }
}
typedef struct { char *b; int n, cap; } MpJs;
static void js_raw(MpJs *j, const char *s){ while (*s && j->n + 1 < j->cap) j->b[j->n++] = *s++; j->b[j->n] = 0; }
static void js_key(MpJs *j, const char *k){ if (j->n > 1) js_raw(j, ","); js_raw(j, "\""); js_raw(j, k); js_raw(j, "\":"); }
static void js_val(MpJs *j, const char *v){
    js_raw(j, "\"");
    for (; *v && j->n + 3 < j->cap; v++){
        char ch = (uint8_t)*v < 0x20 ? ' ' : *v;                   /* (no control characters in names) */
        if (ch == '"' || ch == '\\') j->b[j->n++] = '\\';
        j->b[j->n++] = ch;
    }
    j->b[j->n] = 0; js_raw(j, "\"");
}
static void js_str(MpJs *j, const char *k, const char *v){ js_key(j, k); js_val(j, v); }
static void js_int(MpJs *j, const char *k, int v){ char t[16]; snprintf(t, sizeof t, "%d", v); js_key(j, k); js_raw(j, t); }
/* the race: its kind, track and laps (a LAN race takes at least 2, Street X 4, as mprace.c sets them; a downhill
 * drift, 1 lap in the career, is one run and shows none, as a sprint), its career stage (a lobby races its host's
 * stage's race at the spot, or the last won there: spot_of) */
static void js_race(MpJs *j, uint32_t r){
    static uint32_t cr; static char tn[64];
    if (!r) return;
    if (r != cr){ cr = r; track_name(MR16(r + 0x18u), MR8(r + 0x1au) & 1, tn, sizeof tn); }
    js_str(j, "kind", race_kind(r));
    if (tn[0]) js_str(j, "track", tn);
    if (MR8(r + 0x37u) >= 1u && MR8(r + 0x37u) <= 9u) js_int(j, "stage", MR8(r + 0x37u));
    uint8_t ty = MR8(r + 0xfu); int laps = MR8(r + 0x1bu), lo = ty == 2 ? 4 : 2;
    if (MR8(r + 0x34u) != 2u && (ty == 0 || ty == 2 || ty == 5) && laps > 1) js_int(j, "laps", laps < lo ? lo : laps > 10 ? 10 : laps);
}
static void js_players(MpJs *j, const MpLobby *l){
    js_key(j, "names"); js_raw(j, "[");
    int me = -1;
    for (int i = 0; i < l->n && i < RC_LOBBY_MAX; i++){
        if (i) js_raw(j, ",");
        js_val(j, l == &g_mp.l && g_mp.st == MP_TRIP ? g_mp.names[i] : mp_name(l->ids[i]));
        if (l->ids[i] == g_s.conf.self_id) me = i;
    }
    js_raw(j, "]");
    js_int(j, "me", me); js_int(j, "max", l->max);
}
/* the race's results, once the game showed them (mprace_results, by the LAN names the trip gave the lobby's players):
 * res, the rows in their order, each {n: the player's name, f: 1 finished, t: its time (s) or p: a drift's points,
 * me: 1 ours}; prize: ours */
static void js_results(MpJs *j){
    MpResult r[MPRACE_RESULTS_MAX]; int n = mprace_results(r, MPRACE_RESULTS_MAX);
    if (n <= 0) return;
    js_key(j, "res"); js_raw(j, "[");
    for (int k = 0; k < n; k++){
        const char *nm = r[k].name[0] ? r[k].name : "?";
        for (int i = 0; i < g_mp.l.n && i < RC_LOBBY_MAX; i++) if (r[k].name[0] && !strcasecmp(g_mp.lan[i], r[k].name)){ nm = g_mp.names[i]; break; }
        char t[48];
        js_raw(j, k ? ",{\"n\":" : "{\"n\":"); js_val(j, nm);
        snprintf(t, sizeof t, ",\"f\":%d", r[k].finished); js_raw(j, t);
        if (r[k].finished && r[k].points > 0){ snprintf(t, sizeof t, ",\"p\":%d", r[k].points); js_raw(j, t); }
        else if (r[k].finished && r[k].time > 0.f){ snprintf(t, sizeof t, ",\"t\":%.2f", (double)r[k].time); js_raw(j, t); }
        if (r[k].ours) js_raw(j, ",\"me\":1");
        js_raw(j, "}");
    }
    js_raw(j, "]");
    if (mprace_prize() > 0) js_int(j, "prize", mprace_prize());
}
static void mp_panel(int64_t host, int in_roam){
    static char last[1200];
    if (!g_mp_panel) return;
    char buf[1200]; MpJs j = { buf, 0, sizeof buf };
    js_raw(&j, "{");
    int hud = in_roam && host - g_mp_hud_at < 250000;
    if (g_mp.st == MP_TRIP){
        if (mprace_covered()){ js_str(&j, "v", "trip"); js_str(&j, "step", mprace_step()); js_race(&j, g_mp.l.race); js_players(&j, &g_mp.l); js_results(&j); }
    } else if (hud && g_mp.st != MP_IDLE){
        static const char *const st[] = { "", "host", "asked", "in", "count" };
        js_str(&j, "v", "lobby"); js_str(&j, "st", st[g_mp.st]);
        js_race(&j, g_mp.l.race); js_players(&j, &g_mp.l);
        if (g_mp.st == MP_COUNT){ int64_t s = (g_mp.start_at - host + 999999) / 1000000; js_int(&j, "t", (int)(s < 0 ? 0 : s)); }
        if (g_mp.st == MP_HOST && g_mp.alone_at){
            int64_t s = (g_mp.alone_at + g_mp.alone_us - host + 999999) / 1000000;
            if (s <= 60) js_int(&j, "closes", (int)(s < 0 ? 0 : s));
        }
        if ((g_mp.st == MP_HOST || g_mp.st == MP_IN) && g_mp.l.away_s) js_int(&j, "away", g_mp.l.away_s);
    } else if (hud){
        const MpSpot *sp = mp_spot_here();
        MpLobby *l = mp_joinable();
        const MpLobby *full = 0;
        for (int i = 0; !l && sp && sp->kind == MPS_ONLY && i < g_mp.nseen; i++){
            const MpLobby *f = &g_mp.seen[i];
            if (f->state == RC_LS_OPEN && f->race && f->n >= f->max && mp_dist(f->x, f->y) < MP_JOIN_M) full = f;
        }
        /* a message first, for its few seconds (back from the race at the spot: the place and the prize) */
        if (g_mp.note[0] && host < g_mp.note_until){ js_str(&j, "v", "note"); js_str(&j, "text", g_mp.note); }
        else if (l && (!sp || sp->kind == MPS_ONLY) && !mprace_car_ok()){ js_str(&j, "v", "note"); js_str(&j, "text", MP_NOCAR); }
        else if (l && (!sp || sp->kind == MPS_ONLY)){ js_str(&j, "v", "join"); js_race(&j, l->race); js_players(&j, l); js_int(&j, "fee", MPRACE_QUIT_FEE);
            if (l->race && mprace_evened(race_net_mode(l->race))) js_int(&j, "even", 1); }
        else if (full){ js_str(&j, "v", "full"); js_race(&j, full->race); js_players(&j, full); }
        else if (sp && sp->kind == MPS_ONLY){ js_str(&j, "v", "spot"); js_race(&j, sp->race); js_str(&j, "why", mpw_text[sp->why]); }
    }
    js_raw(&j, "}");
    if (!strcmp(buf, last)) return;
    roam_plat_lock(); int r = roam_plat_lobby(buf); roam_plat_unlock();
    if (r == 0){ g_mp_panel = 0; return; }                         /* no panel here: the top line says it */
    if (r < 0) return;                                                /* again next frame */
    g_mp_panel = 1;
    snprintf(last, sizeof last, "%s", buf);
}
#endif

/* ---- multiplayer races from free roam: the pop-ups -------------------------------------------- */
/* An open race's own card (UI_EngageRaceDialog: Accept / Decline) asks, on
 * Accept, a second question over it: Solo (the card's own Accept: the
 * career race) or Multiplayer (a lobby here, or the one waiting here: the
 * card closes as Decline does but for its "declined" mark in the career).
 * A multiplayer-only spot asks its own: the reason it is not open, and
 * Multiplayer (Join) or Cancel.  Both are the game's generic in-game dialog
 * (DialogInterface 0x558750: two buttons, a printf text) over the screen
 * they come from; the buttons' labels are strings of our own, added to the
 * game's language table (language.c language_add_text), so the dialog looks
 * them up as its own.
 * The answer: a press writes the button's message to the dialog's +0x50
 * (0x54C1BC); on its next update the dialog goes and sends it to the
 * screen it was over (0x5511E0 -> 0x51CDF0): to the card, whose handler is
 * ours (dispatch_patch 0x499C00); over the HUD it is read from the dialog
 * while it is up. */
#if MPRACES
void dispatch_patch(uint32_t a, void (*f)(CPU *));
int career_fe_active(const char *name);       /* career.c */
static int career_card_up(void){ return career_fe_active("UI_EngageRaceDialog.fng"); }
#define FN_DIALOG2      0x00558750u   /* cdecl (from package, dialog package, label1, label2, msg1, msg2, back msg, first, fmt, ...) */
#define G_DLG_ID        0x00838974u   /* the dialog up now (0 none) */
#define G_FEMGR_LIST    0x008637b4u   /* the FE manager's screens: n = [it]; until n == it: screen [n - 4 + 0x14], n = [n] */
#define VT_DIALOG       0x0079c88cu   /* the generic dialog's class */
#define FN_CARD         0x00499c00u   /* EngageRaceDialog's MessageHandler (thiscall msg, obj, a3, a4; ret 0x10) */
#define FN_CARD_ACCEPT  0x00499b50u   /* thiscall card: the race starts (the career's StartRace) */
#define FN_SMS_DONE     0x00511280u   /* thiscall 0x8635BC: what Decline does to the messages after the card */
#define FN_ENGAGE_EXIT  0x005f4240u   /* thiscall [0x8900D8] (1): leave the engage state */
#define G_ENGAGE_MGR    0x008900d8u
#define G_ENGAGED_RACE  0x00863448u   /* the race the card is for */
#define MSG_BUTTON      0x0c407210u   /* a button pressed (obj: the button; its hash +0x10) */
#define CARD_ACCEPT     0xd72f002au
#define CARD_MAP        0x0f365871u   /* the card's TRACK_MAP */
#define MP_LANG_N       4
static const char *const mp_lang_name[MP_LANG_N] = { "MPRACE_SOLO", "MPRACE_MULTI", "MPRACE_JOIN", "MPRACE_CANCEL" };
static const char *const mp_lang_text[MP_LANG_N] = { "Solo", "Multiplayer", "Join", "Cancel" };
static uint32_t mp_lang_hash[MP_LANG_N];
enum { MPD_NONE, MPD_CARD };
static struct {
    int      dlg;                             /* MPD_*: the question we asked, up now */
    uint32_t card, race;                      /* the card it is over (its handler's this); the race asked about */
    uint32_t msg;                             /* the answer read from the dialog (over the HUD) */
    uint32_t str;                             /* guest scratch for the dialog's strings */
    int      patched;
    uint32_t asked, solo, multi;
} g_mpd;
#define MSG_MP_SOLO     0x4d505301u   /* our answers (not the game's messages: its screens ignore them) */
#define MSG_MP_MULTI    0x4d505302u
#define MSG_MP_BACK     0x4d505303u
/* our labels in the game's language table (language.c: again whenever it loads another) */
uint32_t language_add_text(const char *name, const char *text);
uint32_t language_text(uint32_t hash);
void language_texts_frame(CPU *c);
static void mp_lang(CPU *c){
    if (mp_lang_hash[0]) return;
    for (int i = 0; i < MP_LANG_N; i++) mp_lang_hash[i] = language_add_text(mp_lang_name[i], mp_lang_text[i]);
    language_texts_frame(c);                  /* in the table at once: a question may come this frame */
}
/* a two-button question over package `from` (a name in guest memory).  The
 * dialog's text is a format the game's vsprintf (0x43FBC0) fills in, and ours
 * has another player's name in it (a name with "%n" or "%999d" in it wrote
 * over the game's stack: the third security audit).  So it goes in as the
 * argument of a "%s", and with no % left in it: the game takes the text it
 * made as a format again later (a joiner whose question named
 * "Ann%n%s%999d" froze when its trip quit to the main menu) */
static int mp_ask(CPU *c, uint32_t from, const char *text, int l1, int l2, uint32_t m1, uint32_t m2, uint32_t back){
    if (!g_mpd.str){ uint32_t sz = 0x200u; g_mpd.str = rh_call(c, FN_MALLOC, 0, 0, 1, &sz); }
    if (!g_mpd.str || rd32(G_DLG_ID) || !mp_lang_hash[0]) return 0;
    uint32_t dl = g_mpd.str, fmt = g_mpd.str + 0x38u, txt = g_mpd.str + 0x40u;
    char plain[0x1c0]; snprintf(plain, sizeof plain, "%s", text);
    for (char *p = plain; *p; p++) if (*p == '%') *p = '?';
    gstr(dl, "IG_GenericDialog_MED.fng", 0x38);
    gstr(fmt, "%s", 8);
    gstr(txt, plain, 0x1c0);
    uint32_t a[10] = { from, dl, mp_lang_hash[l1], mp_lang_hash[l2], m1, m2, back, 0, fmt, txt };
    rh_call(c, FN_DIALOG2, 0, 0, 10, a);
    g_mpd.asked++;
    return rd32(G_DLG_ID) != 0;
}
/* the card's handler: on Accept our question first */
void sub_00499c00(CPU *c);
static void mp_card_ret(CPU *c){ POP32(c->eip); c->esp += 0x10u; }
/* the card closed as Decline (0x499BB0) closes it, but for its "declined"
 * mark in the career (Career 0x5138F0) */
static void mp_card_close(CPU *c, uint32_t self){
    uint32_t a[2] = { rd32(self + 4u), CARD_MAP };
    uint32_t map = rh_call(c, FN_FE_FINDOBJ, 0, 0, 2, a);
    if (map) gcall1(c, FN_FE_HIDE, 0, map);
    gcall0(c, FN_SMS_DONE, 0x008635bcu);
    gcall1(c, FN_ENGAGE_EXIT, rd32(G_ENGAGE_MGR), 1);
    MR8(self + 0x51u) = 1;
}
static void mp_card(CPU *c){
    uint32_t self = c->ecx, msg = MR32(c->esp + 4u), obj = MR32(c->esp + 8u);
    int64_t host = roam_plat_now_us();
    if (!O.on || !g_net_ok){ sub_00499c00(c); return; }
    if (g_mp.st != MP_IDLE && g_mp.st != MP_TRIP && g_mpd.dlg != MPD_CARD && !MR8(self + 0x51u)){
        /* in a race lobby, Enter (start, or nothing) at an open race's marker
         * engaged the race too: its card goes at once, as Multiplayer closes it */
        rlog("[roam] f=%ld race card: in a race lobby, closed\n", rh_frame());
        mp_card_close(c, self);
        mp_card_ret(c);
        return;
    }
    if (msg == MSG_BUTTON && ok(obj, 0x14) && MR32(obj + 0x10u) == CARD_ACCEPT && !MR8(self + 0x51u) && g_mp.st == MP_IDLE){
        uint32_t r = rd32(G_ENGAGED_RACE);
        if (!r || !race_lan(r)){ sub_00499c00(c); return; }                        /* a race no LAN race can be: as before */
        MpLobby *l = mp_joinable();
        char t[160];
        if (l) snprintf(t, sizeof t, "%s is starting a multiplayer %s here. Race it on your own, or join?", mp_name(l->host), race_kind(l->race));
        else snprintf(t, sizeof t, "Race it on your own, or with other players? With others, only the prize money goes to your career.");
        uint32_t from = rd32(self + 4u);
        if (!mp_ask(c, from, t, 0, l ? 2 : 1, MSG_MP_SOLO, MSG_MP_MULTI, MSG_MP_BACK)){ sub_00499c00(c); return; }
        g_mpd.dlg = MPD_CARD; g_mpd.card = self; g_mpd.race = r;
        rlog("[roam] f=%ld race card: Solo or Multiplayer?\n", rh_frame());
        mp_card_ret(c);
        return;
    }
    if ((msg == MSG_MP_SOLO || msg == MSG_MP_MULTI || msg == MSG_MP_BACK) && g_mpd.dlg == MPD_CARD && self == g_mpd.card){
        g_mpd.dlg = MPD_NONE;
        if (msg == MSG_MP_SOLO && !MR8(self + 0x51u)){
            rlog("[roam] f=%ld race card: Solo\n", rh_frame()); g_mpd.solo++;
            gcall0(c, FN_CARD_ACCEPT, self); MR8(self + 0x51u) = 1;              /* the card's own Accept */
        } else if (msg == MSG_MP_MULTI && !MR8(self + 0x51u)){
            rlog("[roam] f=%ld race card: Multiplayer\n", rh_frame()); g_mpd.multi++;
            mp_card_close(c, self);
            mp_engage_spot(host, g_mpd.race);
        }
        mp_card_ret(c);
        return;
    }
    sub_00499c00(c);
}
/* per frame: the patch once, our labels, the answer over the HUD */
static void mp_ui_frame(CPU *c, int64_t host, int in_roam, int press, const MpSpot *sp){
    if (!g_mpd.patched){ dispatch_patch(FN_CARD, mp_card); g_mpd.patched = 1; }
    mp_lang(c);
    if (g_mpd.dlg == MPD_CARD && !rd32(G_DLG_ID) && !career_card_up()) g_mpd.dlg = MPD_NONE;       /* the card went another way */
    if (!press || !in_roam || g_mp.st != MP_IDLE || g_mpd.dlg) return;
    /* Engage where the game answers nothing, at a multiplayer-only spot: the
     * line at the top said why it is not open and that Enter races it with
     * others (mp_line), as an outrun's challenge: a dialog over free roam
     * gets no input (the world keeps it: the game's own dialogs there come in
     * its engage state or its pause) */
    if (!sp || sp->kind != MPS_ONLY) return;
    mp_engage_spot(host, sp->race);
}
#endif

/* ---- multiplayer races from free roam: labels over the spots ---------------------------------- */
/* A lobby shows its host, race and players over its start (gold, from
 * MP_SEE_LOBBY_M), a multiplayer-only spot near the player what it is
 * (light blue, from MP_SEE_ONLY_M); the game draws its own markers for the
 * open races.  The nearest TAG_MPN on the screen get one, as the names over
 * friends' cars (tags_update), after the HUD's update. */
#if MPRACES
#define MP_SEE_LOBBY_M  250.f
#define MP_SEE_ONLY_M   50.f
#define MP_RGB_LOBBY    0xffc832u
#define MP_RGB_ONLY     0xa0d2ffu
/* the height of a race's start (its trigger's marker +0x14), else the player's car's */
static float race_z(uint32_t r){
    uint32_t e = rd32(G_TRIGGERS + 0x440u + 4u * TRIG_EVENT), end = rd32(G_TRIGGERS + 0x444u + 4u * TRIG_EVENT), mk = r ? MR32(r + 0x28u) : 0;
    for (int n = 0; mk && e < end && n < 1024 && ok(e, 0x44); n++){
        if (MR32(e + 0x30u) == mk){ float z = MRF32(e + 0x14u); if (__builtin_isfinite(z) && z > -500.f && z < 2000.f) return z; break; }
        int sz = (int16_t)MR16(e + 0x42u);
        if (sz <= 0) break;
        e += (uint32_t)sz;
    }
    uint32_t me = player_car();
    return me ? MRF32(me + 0x68u) : 0.f;
}
static void mp_labels(CPU *c){
    uint32_t pl = rd32(G_PLAYER1), hud = pl && ok(pl, PL_HUD + 4u) ? rd32(pl + PL_HUD) : 0;
    uint32_t pkg = hud && ok(hud, 0x14) ? rd32(hud + 0x10u) : 0;
    if (!hud || hud != g_tag.hud || !ok(pkg, 0x20) || !g_gmem) return;
    uint32_t cam = rd32(G_VIEW1 + 0x40u), me = player_car();
    int on = g_in_roam && ok(cam, 0x60) && me && hud_on_top(pkg) && !rd32(0x00838974u /* a dialog up */);
    struct { float x, y, z, d2, sx, sy, sz; char t[48]; uint32_t rgb; } L[MP_SEEN + 8]; int n = 0;
    float mx = on ? MRF32(me + 0x60u) : 0, my = on ? MRF32(me + 0x64u) : 0;
    /* the lobbies: ours (or the one we are in) and the others heard of */
    for (int i = -1; on && i < g_mp.nseen && n < MP_SEEN + 8; i++){
        const MpLobby *l = i < 0 ? &g_mp.l : &g_mp.seen[i];
        if (!l->race || (i < 0 && g_mp.st == MP_IDLE) || (i >= 0 && g_mp.st != MP_IDLE && l->id == g_mp.l.id)) continue;
        float dx = l->x - mx, dy = l->y - my, d2 = dx * dx + dy * dy;
        if (d2 > MP_SEE_LOBBY_M * MP_SEE_LOBBY_M) continue;
        L[n].x = l->x; L[n].y = l->y; L[n].z = race_z(l->race); L[n].d2 = d2; L[n].rgb = MP_RGB_LOBBY;
        if (l->host == g_s.conf.self_id) snprintf(L[n].t, sizeof L[n].t, "Your %s (%d/%d)", race_kind(l->race), l->n, l->max);
        else snprintf(L[n].t, sizeof L[n].t, "%s's %s (%d/%d)", mp_name(l->host), race_kind(l->race), l->n, l->max);
        n++;
    }
    /* the multiplayer-only spots near, but those with a lobby */
    for (int i = 0; on && i < g_spot.n && n < MP_SEEN + 8; i++){
        const MpSpot *sp = &g_spot.s[i];
        if (sp->kind != MPS_ONLY) continue;
        float x = (sp->box[0] + sp->box[2]) / 2, y = (sp->box[1] + sp->box[3]) / 2, dx = x - mx, dy = y - my, d2 = dx * dx + dy * dy;
        if (d2 > MP_SEE_ONLY_M * MP_SEE_ONLY_M) continue;
        int taken = 0;
        for (int j = 0; j < n; j++) if ((L[j].x - x) * (L[j].x - x) + (L[j].y - y) * (L[j].y - y) < 20.f * 20.f) taken = 1;
        if (taken) continue;
        L[n].x = x; L[n].y = y; L[n].z = race_z(sp->race); L[n].d2 = d2; L[n].rgb = MP_RGB_ONLY;
        char k[16]; snprintf(k, sizeof k, "%s", race_kind(sp->race)); k[0] = (char)(k[0] >= 'a' && k[0] <= 'z' ? k[0] - 32 : k[0]);
        snprintf(L[n].t, sizeof L[n].t, "%s (multiplayer)", k);
        n++;
    }
    /* on the screen, nearest first */
    int m = 0;
    for (int i = 0; i < n; i++){
        float d[3] = { L[i].x - MRF32(cam + 0x40u), L[i].y - MRF32(cam + 0x44u), L[i].z + 3.f - MRF32(cam + 0x48u) };
        float ahead = d[0] * MRF32(cam + 0x50u) + d[1] * MRF32(cam + 0x54u) + d[2] * MRF32(cam + 0x58u);
        if (ahead < 1.f) continue;
        uint32_t in = gvec(g_gmem + TAG_SCR, L[i].x, L[i].y, L[i].z + 3.f), out = g_gmem + TAG_SCR + 0x10u, av[2] = { out, in };
        rh_call(c, FN_VIEW_SCREEN, G_VIEW1, 0, 2, av);
        float x = MRF32(out), y = MRF32(out + 4u), z = MRF32(out + 8u);
        float w = (float)(int32_t)MR32(G_VIEW_W), h = (float)(int32_t)MR32(G_VIEW_H);
        if (!(z > 0.f) || !(x >= 0.f) || !(x <= w) || !(y >= 0.f) || !(y <= h)) continue;
        L[i].sx = x / w * 640.f; L[i].sy = y / h * 480.f; L[i].sz = z;
        __typeof__(L[0]) e = L[i];                  /* (m <= i: the ones before m are on the screen, sorted) */
        int j = m++;
        for (; j > 0 && L[j - 1].d2 > e.d2; j--) L[j] = L[j - 1];
        L[j] = e;
    }
    for (int k = 0; k < TAG_MPN; k++){
        int tk = TAG_MP0 + k;
        if (k >= m){ tag_hide(c, tk); continue; }
        uint32_t obj = tag_obj(c, tk, pkg);
        if (!obj) continue;
        float hx = L[k].sx, hy = L[k].sy;
        rh_hud_marker(&hx, &hy);
        tag_text_rgb(c, tk, obj, L[k].t, L[k].rgb);
        uint32_t ap[2] = { gvec(g_gmem + TAG_SCR + 0x20u, hx - 320.f, hy - 240.f, L[k].sz), 0 };
        rh_call(c, FN_FE_POS, obj, 0, 2, ap);
        float sc = 60.f / rc_sqrtf(L[k].d2 + 1.f);
        if (sc < 0.55f) sc = 0.55f;
        if (sc > 1.f) sc = 1.f;
        uint32_t as[2] = { gvec(g_gmem + TAG_SCR + 0x30u, sc, sc, 1.f), 0 };
        rh_call(c, FN_FE_SCALE, obj, 0, 2, as);
        gcall1(c, FN_FE_SHOW, 0, obj);
        g_tag.shown[tk] = 1;
    }
}
#endif

/* ---- multiplayer races from free roam: lobbies on the world map ------------------------------- */
/* A lobby gets the game's own icon of its race's type on the world map (M):
 * a marker on the map's list (WorldMap +4, as wm_mark adds the cars') of the
 * map type of the race (0x5002E0's: 0 circuit, 1 drag, 2 Street X, 3 drift,
 * 4 sprint, 5 URL, 6 SUV) whose FE object is that type's icon
 * "<TYPE>EVENTINDICATOR_0" (the prefixes at 0x7F4348; the game numbers its
 * own from 1, so _0 is never used), a copy of it for a second lobby of a
 * type.  The map places, sizes and hides it as its own every frame
 * (0x4B09E0); nothing but that and the map's destructor (which frees it)
 * walks the marker list, so it needs no career entry.  The lobbies: ours,
 * the ones heard of near, and with the map's answer the hub's of our room. */
#if MPRACES
#define G_WM_PREFIX     0x007f4348u   /* char *[type]: the map's icon names ("CIRCUITEVENTINDICATOR_") */
#define WM_ICON_SZ      0x2cu         /* a marker: +8 type, +0xC FE object, +0x10/+0x14 x y, +0x18 heading, +0x28 1 (an event) */
#define WM_LOBBIES      24
#define WM_LABELS       4             /* lobbies with their words beside the icon: the nearest, none over another */
#define WM_CHAR_W       6.4f          /* a character of the legend's text in the map's units, at its own size (measured: 6.2, and a margin) */
#define WM_TEXT_H       15.f          /* its line's height */
#define WM_ICON_R       7.5f          /* half an icon (the map sizes them 15 or 11, 0x4B0D37) */
#define WM_OBSTACLES    160           /* the icons a label keeps off (every marker but the cars') */
#define FN_FE_TOPLEFT   0x0051d070u   /* cdecl (object, float *x, float *y): its box's top left corner */
#define FN_FE_BOTRIGHT  0x0051d170u   /* cdecl (object, float *x, float *y): its bottom right one */
#define WM_VIEW         0x20u         /* WorldMap: the map's own image (0x496760 hides a marker outside it) */
static struct { uint32_t map; int n; float x[WM_LOBBIES], y[WM_LOBBIES], d[WM_LOBBIES]; uint8_t used[8], len[WM_LOBBIES]; uint32_t m[WM_LOBBIES], lbl[WM_LOBBIES]; } g_wml;
#define WM_LABEL_K      0.7f          /* the labels' size, of the legend's text's (at its own, 0.6, they were too big for a crowded map) */
static float g_wml_s0;               /* the legend's text's size (OPPONENT_CAR, data +0x38) */
#define WM_TEXT_BASE    "OPPONENT_CAR"   /* the legend's string ("Opponent Car"): our labels are copies of it */
static int race_icon(uint32_t r){
    if (MR8(r + 0xdu)) return 6;
    if (MR8(r + 0x34u) == 2u) return 5;
    switch (MR8(r + 0xfu)){ case 1: return 4; case 2: return 2; case 4: return 1; case 5: return 3; default: return 0; }
}
static int mp_map_icon(CPU *c, uint32_t map, uint32_t pkg, uint32_t room, uint32_t r, float x, float y, const char *text){
    if (g_wml.map != map){ memset(&g_wml, 0, sizeof g_wml); g_wml.map = map; }
    for (int i = 0; i < g_wml.n; i++) if ((g_wml.x[i] - x) * (g_wml.x[i] - x) + (g_wml.y[i] - y) * (g_wml.y[i] - y) < 15.f * 15.f) return 0;
    if (g_wml.n >= WM_LOBBIES || !r) return 0;
    int type = race_icon(r);
    char nm[32]; cstr(rd32(G_WM_PREFIX + 4u * (uint32_t)type), nm, 24);
    if (!nm[0] || strlen(nm) > 22) return 0;
    strcat(nm, "0");
    uint32_t obj = wm_find(c, pkg, nm);
    if (!obj){ rlog("[roam] f=%ld world map: no %s for a race lobby\n", rh_frame(), nm); return 0; }
    if (g_wml.used[type]){
        if (!room) return 0;
        char cn[24]; snprintf(cn, sizeof cn, "ROAM_LOBBY_%d", g_wml.n);
        obj = fe_clone(c, pkg, obj, cn);
        if (!obj) return 0;
    }
    uint32_t sz = WM_ICON_SZ, m = rh_call(c, FN_MALLOC, 0, 0, 1, &sz);
    if (!ok(m, WM_ICON_SZ)) return 0;
    for (uint32_t i = 0; i < WM_ICON_SZ; i += 4) MR32(m + i) = 0;
    MR32(m + 8u) = (uint32_t)type; MR32(m + 0xcu) = obj; MRF32(m + 0x10u) = x; MRF32(m + 0x14u) = y; MR8(m + 0x28u) = 1;
    uint32_t head = map + 4u, tail = rd32(head + 4u);
    MR32(tail) = m; MR32(head + 4u) = m; MR32(m + 4u) = tail; MR32(m) = head;
    g_wml.used[type] = 1; g_wml.x[g_wml.n] = x; g_wml.y[g_wml.n] = y; g_wml.m[g_wml.n] = m; g_wml.lbl[g_wml.n] = 0;
    g_wml.d[g_wml.n] = g_mp.st != MP_IDLE && x == g_mp.l.x && y == g_mp.l.y ? -1.f : mp_dist(x, y);     /* (ours first) */
    if (g_wml.d[g_wml.n] >= 0.f) wmf_track(map, m, WMF_RACES);          /* (the legend's Players' races: not ours) */
    /* its label: a copy of the legend's string, in front of the map (a smaller z), gold */
    uint32_t base = room > 1 ? wm_find(c, pkg, WM_TEXT_BASE) : 0;
    char ln[24]; snprintf(ln, sizeof ln, "ROAM_LOBBYTXT_%d", g_wml.n);
    uint32_t lbl = base ? fe_clone(c, pkg, base, ln) : 0;
    if (lbl){
        uint32_t txt = g_gmem + TAG_SCR + 0x48u, col = g_gmem + TAG_SCR + 0x20u;
        gstr(txt, text, TAG_DUEL_MAX);
        gcall1(c, FN_FE_SETSTR, lbl, txt);
        MR32(lbl + 0x1cu) |= 2u;
        g_wml.len[g_wml.n] = (uint8_t)(strlen(text) < 255 ? strlen(text) : 255);   /* (the size: each frame, mp_map_labels) */
        uint32_t d0 = rd32(base + 0x2cu);
        if (ok(d0, 0x40) && MRF32(d0 + 0x38u) > 0.f && MRF32(d0 + 0x38u) < 4.f) g_wml_s0 = MRF32(d0 + 0x38u);
        MR32(col) = MP_RGB_LOBBY & 0xffu; MR32(col + 4u) = MP_RGB_LOBBY >> 8 & 0xffu; MR32(col + 8u) = MP_RGB_LOBBY >> 16 & 0xffu; MR32(col + 12u) = 0xffu;
        uint32_t ac[2] = { col, 0 }; rh_call(c, FN_FE_COLOR, lbl, 0, 2, ac);
        gcall1(c, FN_FE_HIDE, 0, lbl);
        g_wml.lbl[g_wml.n] = lbl;
    } else rlog("[roam] f=%ld world map: no label for a race lobby (%s 0x%08x, room %u)\n", rh_frame(), WM_TEXT_BASE, base, room);
    g_wml.n++;
    return 1;
}
/* every frame the map is open: the nearest WM_LABELS lobbies' labels beside
 * their icons, hidden with the icon (the map places the icons every frame,
 * 0x4B09E0: +0x1C/+0x20 the icon's centre in the package, its object +0x1C
 * bit 0 hidden).  A label goes right of its icon, else left, above or below,
 * wherever it covers no icon (the shops', the events', the lobbies', the
 * player's; the cars' are too many to keep off) and no other label, else it
 * stays hidden: a map with every open lobby of a crowded server labelled was
 * a pile of words, and labels the legend's size over the icons were hard to
 * read (a player's report) */
static int wm_box_free(const float (*b)[4], int n, const float r[4]){
    for (int i = 0; i < n; i++) if (r[0] < b[i][2] && b[i][0] < r[2] && r[1] < b[i][3] && b[i][1] < r[3]) return 0;
    return 1;
}
static void mp_map_labels(CPU *c, uint32_t map){
    if (g_wml.map != map) return;
    static float box[WM_OBSTACLES + WM_LABELS][4];
    int ord[WM_LOBBIES], n = g_wml.n, shown = 0, nb = 0;
    /* the map's own frame: a label stays inside it */
    float view[4] = { -1e9f, -1e9f, 1e9f, 1e9f };
    uint32_t vo = rd32(map + WM_VIEW), sc = g_gmem + TAG_SCR + 0x30u;
    if (ok(vo, 0x5c)){
        uint32_t a[3] = { vo, sc, sc + 4u };
        rh_call(c, FN_FE_TOPLEFT, 0, 0, 3, a); view[0] = MRF32(sc); view[1] = MRF32(sc + 4u);
        rh_call(c, FN_FE_BOTRIGHT, 0, 0, 3, a); view[2] = MRF32(sc); view[3] = MRF32(sc + 4u);
    }
    for (int i = 0; i < n; i++){ int j = i; ord[i] = i; for (; j > 0 && g_wml.d[ord[j - 1]] > g_wml.d[i]; j--) ord[j] = ord[j - 1]; ord[j] = i; }
    for (uint32_t e = rd32(map + 4u), i = 0; e && e != map + 4u && i < 4096 && nb < WM_OBSTACLES && ok(e, WM_MARK_SZ); e = rd32(e), i++){
        uint32_t o = MR32(e + 0xcu);
        if (MR32(e + 8u) == WM_CAR || !ok(o, 0x20) || (MR32(o + 0x1cu) & 1u)) continue;
        float x = MRF32(e + 0x1cu), y = MRF32(e + 0x20u);
        box[nb][0] = x - WM_ICON_R; box[nb][1] = y - WM_ICON_R; box[nb][2] = x + WM_ICON_R; box[nb][3] = y + WM_ICON_R; nb++;
    }
    for (int k = 0; k < n; k++){
        int i = ord[k];
        uint32_t m = g_wml.m[i], lbl = g_wml.lbl[i], icon = ok(m, WM_ICON_SZ) ? rd32(m + 0xcu) : 0;
        if (!lbl || !ok(icon, 0x20)) continue;
        float ix = MRF32(m + 0x1cu), iy = MRF32(m + 0x20u), w = (float)g_wml.len[i] * WM_CHAR_W * WM_LABEL_K, h = WM_TEXT_H * WM_LABEL_K, gap = 3.f;
        /* where the text starts (it is left-aligned) and its middle's height */
        float at[4][2] = { { ix + WM_ICON_R + gap, iy }, { ix - WM_ICON_R - gap - w, iy },
                           { ix - w / 2, iy - WM_ICON_R - gap - h / 2 }, { ix - w / 2, iy + WM_ICON_R + gap + h / 2 } };
        int pick = -1;
        for (int t = 0; t < 4 && pick < 0 && shown < WM_LABELS && !(MR32(icon + 0x1cu) & 1u); t++){
            float r[4] = { at[t][0], at[t][1] - h / 2, at[t][0] + w, at[t][1] + h / 2 };
            /* (its own icon is among the boxes: no place touches it) */
            if (r[0] >= view[0] && r[1] >= view[1] && r[2] <= view[2] && r[3] <= view[3] && wm_box_free((const float (*)[4])box, nb, r)) pick = t;
        }
        if (pick < 0){ gcall1(c, FN_FE_HIDE, 0, lbl); continue; }
        float *r = box[nb++];
        r[0] = at[pick][0]; r[1] = at[pick][1] - h / 2; r[2] = at[pick][0] + w; r[3] = at[pick][1] + h / 2;
        shown++;
        float sz = (g_wml_s0 > 0.f ? g_wml_s0 : 0.6f) * WM_LABEL_K;
        uint32_t as[2] = { gvec(g_gmem + TAG_SCR + 0x30u, sz, sz, 1.f), 0 }, ap[2] = { gvec(g_gmem + TAG_SCR + 0x20u, at[pick][0], at[pick][1], 7.f), 0 };
        rh_call(c, FN_FE_SCALE, lbl, 0, 2, as);                       /* (sizes are absolute: the legend's text's times ours) */
        rh_call(c, FN_FE_POS, lbl, 0, 2, ap);
        gcall1(c, FN_FE_SHOW, 0, lbl);
    }
}
/* hub 0: ours and the ones heard of; hub 1: the hub's with the map's answer */
static int mp_map_icons(CPU *c, uint32_t map, uint32_t pkg, uint32_t room, int hub){
    int n = 0;
    if (!hub){
        char t[64];
        if (g_mp.st != MP_IDLE && g_mp.l.race){
            if (g_mp.l.host == g_s.conf.self_id) snprintf(t, sizeof t, "Your %s %d/%d", race_kind(g_mp.l.race), g_mp.l.n, g_mp.l.max);
            else snprintf(t, sizeof t, "%s's %s %d/%d", mp_name(g_mp.l.host), race_kind(g_mp.l.race), g_mp.l.n, g_mp.l.max);
            n += mp_map_icon(c, map, pkg, room, g_mp.l.race, g_mp.l.x, g_mp.l.y, t);
        }
        for (int i = 0; i < g_mp.nseen; i++){
            const MpLobby *l = &g_mp.seen[i];
            snprintf(t, sizeof t, "%s's %s %d/%d", mp_name(l->host), race_kind(l->race), l->n, l->max);
            n += mp_map_icon(c, map, pkg, room, l->race, l->x, l->y, t);
        }
    } else {
        struct { int16_t x, y; uint32_t race; uint8_t n, max; } e[RC_LOBBIES_MAX]; int k = 0;
        roam_plat_lock();
        for (int i = 0; i < g_s.lobbies_n && i < RC_LOBBIES_MAX; i++){
            e[k].x = g_s.lobbies_e[i].x; e[k].y = g_s.lobbies_e[i].y; e[k].race = g_s.lobbies_e[i].race;
            e[k].n = g_s.lobbies_e[i].n; e[k].max = g_s.lobbies_e[i].max; k++;
        }
        roam_plat_unlock();
        for (int i = 0; i < k; i++){
            uint32_t r = race_by_hash(e[i].race);
            char t[64]; snprintf(t, sizeof t, "%s %d/%d", race_kind(r), e[i].n, e[i].max);
            if (t[0] >= 'a' && t[0] <= 'z') t[0] = (char)(t[0] - 32);
            n += mp_map_icon(c, map, pkg, room, r, e[i].x, e[i].y, t);
        }
    }
    rlog("[roam] f=%ld world map: %d race lobbies marked%s (%d heard of, the hub's %d)\n", rh_frame(), n, hub ? " (the hub's)" : "",
         g_mp.nseen + (g_mp.st != MP_IDLE), g_s.lobbies_n);
    return n;
}
#endif

/* the app has parked the game in the background (macOS: lifecycle.c; the
 * game thread then waits in GetMessageA and this file runs no frame) */
static int g_parked;
/* the player races: a race of the game's, or an outrun with a friend (set
 * each frame with the lock held; roam_web.c shows no notices then) */
static int g_racing;
int roam_game_racing(void){ return g_racing; }     /* with the lock held */
#ifndef ROAM_HOST_WIN
/* In a room (the Room setting) with nobody else in free roam there: the page says so once the player has been
 * ROOM_ALONE_US in free roam without a friend while the server's hub answers (it links the players of a room at any
 * distance, so no friend means nobody of the room is in free roam), and again only ROOM_AGAIN_US later.  A friend,
 * or leaving free roam, starts the wait over.  With the lock held. */
#define ROOM_ALONE_US 20000000
#define ROOM_AGAIN_US 300000000
static int64_t g_alone_since, g_alone_told;
static void room_alone(int64_t host, uint8_t st){
    if (!g_s.conf.room || !g_s.conf.hub_ip || g_s.hub_was != 2 || (st != RC_ST_ROAM && st != RC_ST_PAUSED) || rc_established(&g_s)){
        g_alone_since = 0; return; }
    if (!g_alone_since) g_alone_since = host;
    if (st != RC_ST_ROAM || host - g_alone_since < ROOM_ALONE_US || (g_alone_told && host - g_alone_told < ROOM_AGAIN_US)) return;
    g_alone_told = host;
    char t[160];
    snprintf(t, sizeof t, "Nobody else is in free roam in room \"%s\". Clear Room under Online on the start page to see everyone.", O.room);
    roam_plat_notice(0, t);
    rlog("[roam] f=%ld alone in room \"%s\" for %.0f s: told\n", rh_frame(), O.room, (double)(host - g_alone_since) / 1e6);
}
#endif
#if SAYS
/* ---- quick chat: what the player says, what friends say (the state: before tags_update) -------------------------
 * The player picks a phrase on the page (keys 1-4 twice, the right stick, the touch button: quickchat.ts); while
 * driving in free roam it goes to every friend (rc_send_say) and back to the page as said by the player.  A friend's
 * is shown when their car was within SAY_NEAR_M of ours when last seen, or they are in our race lobby, while we are
 * in free roam and not racing; not past SAY_BURST in SAY_WINDOW_US from one friend, nor the same phrase again within
 * SAY_AGAIN_US.  To the page as {"id","name","p"} (+ "me" for the player's own, "lobby" for a lobby's).  With the
 * lock held. */
static int say_burst(const int64_t *t, int64_t host){
    int k = 0;
    for (int i = 0; i < SAY_BURST; i++) k += t[i] && host - t[i] < SAY_WINDOW_US;
    return k >= SAY_BURST;
}
static void say_stamp(int64_t *t, int64_t host){
    int o = 0;
    for (int i = 1; i < SAY_BURST; i++) if (t[i] < t[o]) o = i;   /* the oldest goes */
    t[o] = host;
}
static void say_page(uint32_t id, const char *name, int phrase, int me, int lobby){
    char buf[128], ids[16]; MpJs j = { buf, 0, sizeof buf };
    snprintf(ids, sizeof ids, "%u", id);
    js_raw(&j, "{"); js_key(&j, "id"); js_raw(&j, ids); js_str(&j, "name", name); js_int(&j, "p", phrase);
    if (me) js_int(&j, "me", 1);
    if (lobby) js_int(&j, "lobby", 1);
    js_raw(&j, "}");
    if (roam_plat_said(buf) < 0) rlog("[roam] f=%ld quick chat: the page's ring is full, a line lost\n", rh_frame());
}
static int say_in_lobby(uint32_t id){
#if MPRACES
    return (g_mp.st == MP_HOST || g_mp.st == MP_IN || g_mp.st == MP_COUNT) && mp_in(&g_mp.l, id) && mp_in(&g_mp.l, g_s.conf.self_id);
#else
    (void)id; return 0;
#endif
}
static void say_send(int phrase, int64_t host, int can){
    if (!can){ rlog("[roam] f=%ld quick chat: phrase %d not said (not driving in free roam)\n", rh_frame(), phrase); return; }
    if (say_burst(g_say.mine, host)){ rlog("[roam] f=%ld quick chat: phrase %d not said (too many at once)\n", rh_frame(), phrase); return; }
    say_stamp(g_say.mine, host);
    int to = rc_send_say(&g_s, (uint8_t)phrase, ++g_say.seq);
    g_say.said++; g_say.mine_until = host + SAY_SHOW_US;
    say_page(g_s.conf.self_id, g_s.conf.name, phrase, 1, 0);
    rlog("[roam] f=%ld quick chat: said phrase %d to %d player(s)\n", rh_frame(), phrase, to);
}
static void say_recv(uint32_t from, int phrase, uint8_t seq, int64_t host, int can){
    int i = -1;
    for (int k = 0; k < RC_TABLE; k++) if (g_s.peer[k].state == RC_PS_EST && g_s.peer[k].id == from){ i = k; break; }
    if (i < 0) return;
    const rc_peer *p = &g_s.peer[i];
    SayWho *w = &g_say.who[i];
    if (w->id != from){ memset(w, 0, sizeof *w); w->id = from; }
    const char *why = 0;
    int lobby = say_in_lobby(from);
    float d = 1e9f;
    if (g_lp_ok && p->mo.n){ const float *b = p->mo.s[p->mo.n - 1].p; float dx = g_lp[0] - b[0], dy = g_lp[1] - b[1]; d = rc_sqrtf(dx * dx + dy * dy); }
    if (w->heard && seq == w->seq) why = "again";                                   /* the same one twice */
    else if (!can) why = "not in free roam";
    else if (!lobby && !(d <= SAY_NEAR_M)) why = "too far";
    else if (say_burst(w->t, host)) why = "too many at once";
    else if (w->at && phrase == w->phrase && host - w->at < SAY_AGAIN_US) why = "the same again";
    w->seq = seq; w->heard = 1;
    if (why){ g_say.dropped++; rlog("[roam] f=%ld quick chat: %s's phrase %d not shown (%s, %.0f m)\n", rh_frame(), p->name, phrase, why, (double)d); return; }
    say_stamp(w->t, host);
    w->phrase = (uint8_t)phrase; w->at = host; w->until = host + SAY_SHOW_US;
    g_say.heard++;
    say_page(from, p->name, phrase, 0, lobby);
    rlog("[roam] f=%ld quick chat: %s said phrase %d (%.0f m%s)\n", rh_frame(), p->name, phrase, (double)d, lobby ? ", our lobby" : "");
}
static void say_frame(int64_t host, int in_roam, uint8_t st){
    if (!g_s.conf.say) return;
    int pick = roam_plat_say_pick();
    if (g_say.test_pick >= 0){ pick = g_say.test_pick; g_say.test_pick = -1; }
#if MPRACES
    if (pick >= 0) g_mp.said_at = host;                 /* a host who talks is there (mp_idle) */
#endif
    if (pick >= 0) say_send(pick, host, in_roam && st == RC_ST_ROAM && !g_racing);
    uint32_t from; uint8_t ph, sq;
    while (rc_take_say(&g_s, &from, &ph, &sq)) say_recv(from, ph, sq, host, in_roam && !g_racing);
}

/* Front bumpers (free roam's friends: the page's "Add friend", web/src/app/friends.ts): the player's car and a
 * friend's touching nose to nose for BUMP_US (3 s), whether cars are solid or not (passable cars drive into each
 * other, so this reads the cars' places, not the game's collisions).  The page is told then, and again once they
 * parted; it offers to add that player (the server checks that the two are linked, and both names claimed).  A
 * car's nose: its place (+0x60, the centre of mass: minus the car's own offset to it, +0x60 - +0x50 forward) plus
 * its front wheels' place forward (RidePhysicsInfo +0x10, as wheels_pose reads it) plus BUMP_NOSE_M (the
 * overhangs, 0.7-1.0 m).  Touching: the two point at each other (within BUMP_DOT) and each is ahead of the other,
 * side by side within BUMP_SIDE_M, the noses at most BUMP_GAP_M apart or up to BUMP_DEEP_M into each other (a
 * passable car), heights within 1.5 m.  A part of BUMP_LAPSE_US or less is no parting (each game draws the other
 * car a little off); parted for BUMP_OFF_US after the 3 s, it is over.  Only in free roam proper (driving, no
 * race, no outrun, no lobby).  Not in the Windows plugin (no page). */
#define BUMP_US         3000000
#define BUMP_LAPSE_US   300000
#define BUMP_OFF_US     1500000
#define BUMP_NOSE_M     0.85f
#define BUMP_DOT        (-0.8f)
#define BUMP_SIDE_M     1.2f
#define BUMP_GAP_M      0.45f
#define BUMP_DEEP_M     1.2f
static struct { uint32_t id; int64_t since, last; int on; char name[RC_NAME_MAX + 1]; } g_bump[NSLOT];
static int g_bump_can;                       /* free roam proper at the last frame (for `mp bump`) */
/* how far a car's nose is ahead of its place +0x60, along fx, fy (its forward, flat and of length 1) */
static float bump_nose(uint32_t car, float fx, float fy){
    uint32_t ph = MR32(car + 0x20u);
    float x = ok(ph, 0xc0) ? MRF32(ph + 0x10u) : 0.f;
    if (!(x > 0.6f && x < 2.6f)) x = 1.3f;
    float com = (MRF32(car + 0x60u) - MRF32(car + 0x50u)) * fx + (MRF32(car + 0x64u) - MRF32(car + 0x54u)) * fy;
    if (!(com > -1.f && com < 1.f)) com = 0.f;
    return x - com + BUMP_NOSE_M;
}
/* a car's forward, flat and of length 1 (0: it points up or down) */
static int bump_fwd(uint32_t car, float *fx, float *fy){
    float x = MRF32(car + 0x90u), y = MRF32(car + 0x94u), l = rc_sqrtf(x * x + y * y);
    if (!(l > 0.3f && l < 2.f)) return 0;
    *fx = x / l; *fy = y / l;
    return 1;
}
/* the player's car `me` and `car` touch nose to nose now (gap: how far apart the noses are, negative: into each other) */
static int bump_touch(uint32_t me, uint32_t car, float *gap){
    float f0x, f0y, f1x, f1y;
    if (!bump_fwd(me, &f0x, &f0y) || !bump_fwd(car, &f1x, &f1y) || f0x * f1x + f0y * f1y > BUMP_DOT) return 0;
    float dx = MRF32(car + 0x60u) - MRF32(me + 0x60u), dy = MRF32(car + 0x64u) - MRF32(me + 0x64u), dz = MRF32(car + 0x68u) - MRF32(me + 0x68u);
    float along = dx * f0x + dy * f0y, side = dx * f0y - dy * f0x;
    if (!(along > 0.f) || -(dx * f1x + dy * f1y) <= 0.f || side > BUMP_SIDE_M || side < -BUMP_SIDE_M || dz > 1.5f || dz < -1.5f) return 0;
    *gap = along - bump_nose(me, f0x, f0y) - bump_nose(car, f1x, f1y);
    return *gap < BUMP_GAP_M && *gap > -BUMP_DEEP_M;
}
static void bump_page(uint32_t id, const char *name, int on){
    char buf[96], ids[16]; MpJs j = { buf, 0, sizeof buf };
    snprintf(ids, sizeof ids, "%u", id);
    js_raw(&j, "{"); js_key(&j, "id"); js_raw(&j, ids); js_str(&j, "name", name); js_int(&j, "on", on); js_raw(&j, "}");
    roam_plat_lock();
    int r = roam_plat_friend(buf);
    roam_plat_unlock();
    if (r < 0) rlog("[roam] f=%ld front bumpers: the page's ring is full, a line lost\n", rh_frame());
}
/* every frame: `can` = free roam proper (see above) */
static void bump_frame(int64_t host, int can){
    uint32_t me = can ? player_car() : 0;
    if (me && !ok(me, 0x9e0)) me = 0;
    for (int k = 0; k < NSLOT; k++){
        Slot *s = &g_sl[k];
        int live = me && s->st == SL_ACTIVE && !s->hidden && !s->paused && ok(s->car, 0x9e0);
        if (live && g_bump[k].id != s->peer_id){
            /* another friend in this slot: the one before parted */
            if (g_bump[k].on) bump_page(g_bump[k].id, g_bump[k].name, 0);
            g_bump[k].id = s->peer_id; g_bump[k].since = g_bump[k].last = 0; g_bump[k].on = 0;
            snprintf(g_bump[k].name, sizeof g_bump[k].name, "%s", s->name);
        }
        float gap = 0.f;
        g_bump_can = can;
        if (live && bump_touch(me, s->car, &gap)){
            if (!g_bump[k].since || host - g_bump[k].last > BUMP_LAPSE_US) g_bump[k].since = host;
            g_bump[k].last = host;
            if (!g_bump[k].on && host - g_bump[k].since >= BUMP_US){
                g_bump[k].on = 1;
                bump_page(s->peer_id, s->name, 1);
                rlog("[roam] f=%ld front bumpers: touching %s's for 3 s (the noses %.2f m apart)\n", rh_frame(), s->name, (double)gap);
            }
        } else if (g_bump[k].on && (!live || host - g_bump[k].last > BUMP_OFF_US)){
            g_bump[k].on = 0; g_bump[k].since = 0;
            bump_page(g_bump[k].id, g_bump[k].name, 0);
            rlog("[roam] f=%ld front bumpers: apart from %s's\n", rh_frame(), g_bump[k].name);
        } else if (!g_bump[k].on && g_bump[k].since && host - g_bump[k].last > BUMP_LAPSE_US) g_bump[k].since = 0;
    }
}
#endif
static uint8_t local_status(int in_roam){
    uint32_t flow = rd32(G_FLOW);
    if (in_roam) return g_sync_paused > 1 || g_parked ? RC_ST_PAUSED : RC_ST_ROAM;
    if (flow == 6u){
        /* a race (career event, or more than one racer); the Dyno Shop (its
         * test runs, one-racer races, stay "loading"); otherwise the world
         * is still coming up */
        uint32_t race = rd32(G_RACE);
        if (rd32(G_CAREER_RACE) || (ok(race, 0x28) && MR32(race + 0x24u) > 1u)) return RC_ST_EVENT;
        if (rd32(G_RACETYPE) == RACETYPE_DYNO) return RC_ST_SHOP;
        return RC_ST_LOADING;
    }
    if (flow == 3u) return (rd32(G_GAMEMODE) & 1u) ? RC_ST_SHOP : RC_ST_MENUS;
    return RC_ST_LOADING;
}
/* each crew's colour unless the player picked another (the Crews setting's
 * colour dots: NFSU2_ROAM_CREW_RGB), by its place: cyan, magenta, violet,
 * red (Thom's pick, 2026-10-24); the page's CREW_PALETTE starts with them.
 * No green or orange: the game's own arrows for the player and the others */
static const uint32_t CREW_RGB[RC_CREW_MAX] = { 0x3cd2ff, 0xff50c8, 0xa078ff, 0xff4646 };
static void crew_rgb_parse(const char *rgb);
/* The Crews setting: names joined by commas (the start page's list), each
 * trimmed; empty ones, one that hashes as an earlier one (rc_room_hash:
 * spaces and A-Z case do not count) and one that reads as a slur (names.h:
 * the page's box refuses it, a hand-written .ini may have it) are left out,
 * at most RC_CREW_MAX */
static void crews_parse(const char *v, rc_config *cf){
    O.ncrew = 0; cf->n_crews = 0;
    while (v && *v && O.ncrew < RC_CREW_MAX){
        const char *e = v;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - v);
        while (n && (*v == ' ' || *v == '\t')){ v++; n--; }
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
        char nm[sizeof O.crew[0]]; snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm - 1 ? n : sizeof nm - 1), v);
        uint32_t h = rc_room_hash(nm);
        int dup = 0;
        for (int i = 0; i < cf->n_crews; i++) dup |= cf->crews[i] == h;
        if (h && !dup && !name_blocked(nm)){ snprintf(O.crew[O.ncrew++], sizeof O.crew[0], "%s", nm); cf->crews[cf->n_crews++] = h; }
        v = *e ? e + 1 : 0;
    }
    crew_rgb_parse(opt("NFSU2_ROAM_CREW_RGB"));
}
/* the crews' colours (NFSU2_ROAM_CREW_RGB, the test aid mp crewrgb): rrggbb
 * values joined by commas in the crews' order, an empty or unreadable one
 * the place's CREW_RGB */
static void crew_rgb_parse(const char *rgb){
    for (int i = 0; i < RC_CREW_MAX; i++){
        O.crew_rgb[i] = CREW_RGB[i];
        if (!rgb || !*rgb) continue;
        while (*rgb == ' ' || *rgb == '\t') rgb++;
        uint32_t x = 0; int nd = 0;
        for (; nd < 6 && ((*rgb >= '0' && *rgb <= '9') || ((*rgb | 0x20) >= 'a' && (*rgb | 0x20) <= 'f')); rgb++, nd++)
            x = x << 4 | (uint32_t)(*rgb <= '9' ? *rgb - '0' : (*rgb | 0x20) - 'a' + 10);
        if (nd == 6) O.crew_rgb[i] = x;
        while (*rgb && *rgb != ',') rgb++;
        if (*rgb) rgb++;
    }
}
static void init_once(CPU *c){
    (void)c;
    g_inited = 1;
    O.on = env_int("NFSU2_ROAM", 0, 0, 1);
    if (!O.on) return;
    O.test = roam_plat_test_mode();
#ifdef ROAM_HOST_WIN
    O.collide = env_int("NFSU2_ROAM_COLLIDE", 0, 0, 1);     /* the Windows plugin: ghosts unless Collide = 1 */
#else
    O.collide = env_int("NFSU2_ROAM_COLLIDE", 1, 0, 1);
#endif
    O.labels = env_int("NFSU2_ROAM_LABELS", 1, 0, 1);
    O.spawn_m = env_int("NFSU2_ROAM_SPAWN_M", SPAWN_M, 0, 1000);
    O.hit_share = env_int("NFSU2_ROAM_HIT_SHARE", -1, -1, 100);
    O.test_mass = env_int("NFSU2_ROAM_TEST_MASS", 0, 0, 20000);
    O.test_fmass = env_int("NFSU2_ROAM_TEST_FMASS", 0, 0, 20000);
    O.test_owncars = env_int("NFSU2_ROAM_TEST_OWNCARS", 0, 0, 1);
    O.test_outside = env_int("NFSU2_ROAM_TEST_OUTSIDE", 0, 0, 1);
    O.max_cars = env_int("NFSU2_ROAM_MAX_CARS", MAX_CARS, 0, MAX_CARS);
#if MPRACES
    g_mp.alone_us = (int64_t)env_int("NFSU2_ROAM_LOBBY_ALONE_S", (int)(MP_ALONE_US / 1000000), 5, 3600) * 1000000;
    g_mp.idle_us = (int64_t)env_int("NFSU2_ROAM_LOBBY_IDLE_S", (int)(MP_IDLE_US / 1000000), 10, 3600) * 1000000;
#endif
    /* at most 30: each snapshot goes to up to 32 friends, and 60 tripled what each of them got from that player
     * (the server's port the limit, nfsu2.xyz 2026-10-22) for little they see: they draw a car 100 ms in the past,
     * between its snapshots */
    O.rate = env_int("NFSU2_ROAM_RATE", 20, 10, 30);
    O.delay_ms = env_int("NFSU2_ROAM_DELAY", 100, 0, 1000);
    O.extrap_ms = env_int("NFSU2_ROAM_EXTRAP", 250, 0, 1000);
    O.carpool_kb = env_int("NFSU2_ROAM_CARPOOL_KB", CARPOOL_KB, 0, 128000);
    /* at or below the game's own size the player's car and traffic fill the pool and no friend's car is shown:
     * the value the Settings panel's old "Car memory pool" row left in players' files (its help said "never
     * below the game's own 11000"), so it counts as unset */
    if (O.carpool_kb <= CARPOOL_KB_GAME){
        rlog("[roam] NFSU2_ROAM_CARPOOL_KB=%d is not above the game's own %d KB (no room for friends' cars), using %d\n",
             O.carpool_kb, CARPOOL_KB_GAME, CARPOOL_KB);
        O.carpool_kb = CARPOOL_KB;
    }
    int port = env_int("NFSU2_ROAM_PORT", RC_DEFAULT_PORT, 1024, 65535);
    int maxp = env_int("NFSU2_ROAM_MAX_PLAYERS", RC_MAX_PLAYERS, 2, RC_MAX_PLAYERS);
    int disc = env_int("NFSU2_ROAM_DISCOVERY", 1, 0, 1);
    const char *nm = opt("NFSU2_ROAM_NAME");
    if (nm && *nm) rc_name_clean(O.name, nm, 64);
    else O.name[0] = 0;                                    /* the profile name, once there is one */
    const char *rm = opt("NFSU2_ROAM_ROOM");
    /* a room that reads as a slur is none (the settings refuse it; the Windows plugin's .ini does not) */
    if (rm && name_blocked(rm)){ rlog("[roam] the room setting reads as a slur: no room\n"); rm = 0; }
    snprintf(O.room, sizeof O.room, "%s", rm ? rm : "");
    rc_config cf; memset(&cf, 0, sizeof cf);
    cf.self_id = roam_plat_random(); cf.port = (uint16_t)port; cf.room = rc_room_hash(O.room);
    crews_parse(opt("NFSU2_ROAM_CREW"), &cf);
    snprintf(cf.name, sizeof cf.name, "%s", O.name[0] ? O.name : "Player");
#ifdef ROAM_HOST_WIN
    cf.platform = RC_PLAT_WIN;                             /* HELLO byte 18: the Windows plugin */
#else
    cf.platform = RC_PLAT_MAC;
#endif
    cf.test_mode = O.test; cf.max_players = maxp; cf.rate_hz = O.rate; cf.duels = DUELS; cf.lobbies = MPRACES;
    /* quick chat: where there is a page to pick and show phrases (settings.ini quick_chat = off: neither, and the
     * HELLO tells the others to send none) */
    cf.say = SAYS && roam_plat_quickchat() && env_int("NFSU2_ROAM_SAY", 1, 0, 1);
    cf.discovery = disc && !O.test;
    rc_io io = { 0, 0, 0, 0 };
    rc_session_init(&g_s, &cf, &io);
    roam_plat_cfg pc = { (uint16_t)port, O.test, disc, rh_opt("NFSU2_ROAM_IMPAIR") };
    if (roam_plat_start(&pc, &g_s) != 0){
        rlog("[roam] free-roam multiplayer could not start (see above): off\n"); O.on = 0; return; }
    g_net_ok = 1;
    uint32_t pip[RC_MAX_PEERS * 2]; uint16_t ppt[RC_MAX_PEERS * 2];
    int np = roam_plat_parse_peers(opt("NFSU2_ROAM_PEERS"), O.test, (uint16_t)port, pip, ppt, RC_MAX_PEERS * 2);
    roam_plat_lock();
    for (int i = 0; i < np; i++) rc_add_peer(&g_s, pip[i], ppt[i], 1, roam_plat_now_us());
    roam_plat_unlock();
    /* the free-roam car pool: room for friends' cars (read at the next track load) */
    if (O.carpool_kb && rd32(G_CARPOOL_KB) && rd32(G_CARPOOL_KB) < (uint32_t)O.carpool_kb){
        rlog("[roam] free-roam car memory pool %u -> %d KB\n", rd32(G_CARPOOL_KB), O.carpool_kb);
        MR32(G_CARPOOL_KB) = (uint32_t)O.carpool_kb;
    }
    const char *pl = rh_opt("NFSU2_ROAM_POSELOG");
    if (pl && *pl){ g_poselog = rh_fopen(pl); if (g_poselog) rh_fprintf(g_poselog, "kind,host_us,sync_us,id,x,y,z,speed,mode,err,fx,fy,fz,uz,rx,ry,rz\n"); }
    g_frame0 = rh_frame();
    char ids[16];
    rlog("[roam] free-roam multiplayer ON: id %08x, port %d, %d configured peer(s), room \"%s\" (%08x), up to %d players, "
                    "%d remote cars, %d Hz, %s, none shown within %d m of the new career's start, quick chat %s%s\n", cf.self_id, port, np,
            O.room, cf.room, maxp, O.max_cars, O.rate, O.collide ? "solid cars" : "ghost cars", O.spawn_m, cf.say ? "on" : "off",
            O.test ? ", test harness (loopback only)" : "");
    (void)ids;
    for (int i = 0; i < O.ncrew; i++) rlog("[roam] crew %d: \"%s\" (%08x), colour %06x\n", i + 1, O.crew[i], cf.crews[i], O.crew_rgb[i]);
}

/* Friends' cars wait while the city's sections load.  The track streamer's
 * step (0x5E6A70, each frame from its service 0x5E86E0) does nothing while
 * the car loader is busy (+0x14, 0x8A329C): no section read starts, no
 * phase of its work ends.  The loader serves its loads one after another,
 * each step's callback going straight on to the next, so with friends
 * coming and going it was busy nearly all the time and the streamer moved
 * only when its queue ran dry: the player drove into sections that were not
 * in, the world stopped on its Loading screen (HUD_World_Loading), and that
 * lasted until the last car queued was in, with more queued meanwhile
 * (players' logs of 2026-10-29: screens of 1.7 to 3.3 s, each over 3 frames
 * after the last friend's car, 7 to 15 cars loading during it; and one for
 * good, the loader stuck on a car).  So no friend's car is asked for while
 * the streamer has work (its phase +0x168 not 0), and the loader holds at
 * most LOADS_AHEAD of them at once: the streamer waits for those at the
 * most.  A car held STREAM_HOLD_US goes anyway, should the streamer never
 * get done.  NFSU2_ROAM_STREAM_FIRST=0 (test aid): the cars as before. */
#define G_TRACKSTREAMER 0x0088eb00u   /* +0x168 its phase: 0 nothing to do, 1-6 room made and sections read */
#define LOADS_AHEAD     2
#define STREAM_HOLD_US  5000000
static struct { int64_t t0, us, max; int n, gone; } g_shold;    /* cars held for the streamer: since, in all, the longest, times, let go */
static int loads_in_loader(CPU *c){
    int n = 0;
    for (int k = 0; k < NSLOT; k++){
        uint32_t h = g_sl[k].st == SL_CREATING ? g_sl[k].h_orig : g_sl[k].h_new, e = h ? loader_entry(h) : 0;
        if (e && !(gcall1(c, FN_LOADER_LOADED, G_CARLOADER, e) & 0xffu)) n++;
    }
    return n;
}
/* a hold ends when the streamer is done (each frame), or when a car goes anyway */
static void stream_hold_end(int64_t now){
    if (!g_shold.t0) return;
    int64_t d = now - g_shold.t0;
    g_shold.us += d; if (d > g_shold.max) g_shold.max = d;
    g_shold.t0 = 0;
}
static int load_may_start(CPU *c){
    static int off = -1;
    if (off < 0){ const char *e = rh_opt("NFSU2_ROAM_STREAM_FIRST"); off = e && *e == '0'; }
    if (off) return 1;
    int64_t now = roam_plat_now_us();
    if (rd32(G_TRACKSTREAMER + 0x168u)){
        if (!g_shold.t0){ g_shold.t0 = now; g_shold.n++; }
        if (now - g_shold.t0 < STREAM_HOLD_US) return 0;
        g_shold.gone++;
    }
    stream_hold_end(now);
    return loads_in_loader(c) < LOADS_AHEAD;
}
/* The loader busy with no friend's car in for 20 s: which friends' cars it
 * holds, and how far each got (the model pack +0x954 byte +0xC, the record
 * +0x958 byte +8: 2 in), for the log of a world that waits for good */
static void loader_watch(CPU *c, int64_t host){
    static int64_t since, said_at;
    if (!rd32(G_CARLOADER + LOADER_BUSY)){ since = 0; return; }
    if (!since) since = host;
    int64_t from = since > g_ready_at ? since : g_ready_at;
    if (host - from < 20000000 || said_at >= from) return;
    said_at = host;
    rlog("[roam] f=%ld the car loader has been busy for %.0f s, no friend's car in for 20 s\n", rh_frame(), (host - since) / 1e6);
    for (int k = 0; k < NSLOT; k++){
        Slot *s = &g_sl[k];
        uint32_t h = s->st == SL_CREATING ? s->h_orig : s->h_new, e = h ? loader_entry(h) : 0;
        if (!e) continue;
        uint32_t m = rd32(e + 0x954u), r = rd32(e + 0x958u), t = rd32(e + 0x95cu);
        rlog("[roam]   slot %d: %s's %s, handle 0x%x, entry state %u, model %d, record %d, textures 0x%x, %s, asked for %.1f s ago\n", k,
             s->name, s->carname, h, rd32(e + 0xcu), m ? (int)MR8(m + 0xcu) : -1, r ? (int)MR8(r + 8u) : -1, t,
             gcall1(c, FN_LOADER_LOADED, G_CARLOADER, e) & 0xffu ? "in" : "not in", (host - s->load_t0) / 1e6);
    }
}
static void cost_add(uint64_t t0){
    uint64_t ns = rh_ns() - t0 + g_cost_hud;
    g_cost_hud = 0;
    g_cost_sum += ns; g_cost_n++; if (ns > g_cost_max) g_cost_max = ns;
    g_cost_win_sum += ns; g_cost_win_n++; if (ns > g_cost_win_max) g_cost_win_max = ns;
}
static void periodic_log(int64_t host){
    if (!g_cost_win_t0) g_cost_win_t0 = host;
    if (host - g_cost_win_t0 < 30000000) return;
    roam_plat_stats ps; roam_plat_get_stats(&ps);
    roam_plat_lock();
    int est = rc_established(&g_s);
    rlog("[roam] f=%ld stats: %d friend(s), tx %u (%.1f KB), rx %u, bad %u, other version %u, other room %u, policy %u, ratelimit %u, "
                    "stray %u, second address %u, blocked %llu; frame cost mean %.1f us max %.1f us (%llu frames); heap %u MB\n",
            rh_frame(), est, g_s.tx, g_s.tx_bytes / 1024.0, g_s.rx, g_s.rx_bad, g_s.rx_version, g_s.rx_room, g_s.rx_policy, g_s.rx_ratelimit,
            g_s.rx_stray, g_s.rx_dup_id, (unsigned long long)ps.blocked, g_cost_win_n ? g_cost_win_sum / 1000.0 / (double)g_cost_win_n : 0.0,
            g_cost_win_max / 1000.0, (unsigned long long)g_cost_win_n, rh_heap_mb());
    if (g_s.conf.hub_ip)
        rlog("[roam]   the server's hub: %s, %u lists, %u HELLOs of players it did not list ignored\n",
                g_s.hub_was == 2 ? "answering (it picks the players near us)" : g_s.hub_was == 1 ? "no answer yet" : "silent (broadcast discovery)",
                g_s.hub_lists, g_s.rx_unlisted);
    if (g_shold.n){
        rlog("[roam]   friends' cars waited for the city's sections %d time(s), %.1f s in all, the longest %.0f ms%s\n", g_shold.n,
             g_shold.us / 1e6, g_shold.max / 1e3, g_shold.gone ? " (some went after 5 s)" : "");
        g_shold.n = g_shold.gone = 0; g_shold.us = g_shold.max = 0;
    }
    for (int i = 0; i < RC_TABLE; i++){
        const rc_peer *p = &g_s.peer[i];
        if (p->state != RC_PS_EST) continue;
        const rc_interp *m = &p->mo;
        rlog("[roam]   %s (%s, %s, rtt %.1f ms, render delay %.0f ms): snaps %u, snaps lost %u, interp %llu extrap %llu hold %llu, dup %llu, late %llu, "
                        "corrections > 5 m %llu, largest %.2f m, re-syncs %llu\n",
                p->name, p->platform == RC_PLAT_WIN ? "Windows" : p->platform == RC_PLAT_MAC ? "Mac" : "tool", rc_status_name(p->status),
                p->rtt_us / 1000.0, g_rd[i].id == p->id ? g_rd[i].d / 1000.0 : (double)O.delay_ms, p->rx_snap, p->seq_lost,
                (unsigned long long)m->n_interp, (unsigned long long)m->n_extrap, (unsigned long long)m->n_hold,
                (unsigned long long)m->n_dup, (unsigned long long)m->n_stale, (unsigned long long)m->n_snap, (double)m->err_max, (unsigned long long)m->n_epoch);
    }
    roam_plat_unlock();
    g_cost_win_t0 = host; g_cost_win_sum = g_cost_win_n = g_cost_win_max = 0;
}

static int loads_waiting(void){
    int n = 0;
    for (int k = 0; k < NSLOT; k++) n += (g_sl[k].st == SL_CREATING) + (g_sl[k].h_new != 0);
    return n;
}
/* What costs a frame time: a car of our own made (its paint textures, its
 * first ride asked for, or CreateCar), a ride asked for, a loaded ride put
 * on its car (SetRideInfo makes its render info: ~1 ms).  At most two a
 * frame: 32 friends arriving at once made 29 cars in one frame (112 ms), and
 * in a crowd coming and going (roam_crowd_busy.txt) 116 frames of 3,600
 * took 3 to 6 of them */
#define HEAVY_PER_FRAME 2
static int g_heavy;
static int heavy_take(void){
    if (g_heavy >= HEAVY_PER_FRAME) return 0;
    g_heavy++;
    return 1;
}
/* "nearest" for the slots, as the server's hub weighs its links (nfsu2-roam
 * RoamOptions soon_ms, look_ms, view_m): the nearest of the distances SOON_S
 * and LOOK_S on, both cars at their speeds, and from our view point, VIEW_M
 * ahead of where our car points (SOON_S on), to the friend's car SOON_S on
 * (PeerView.rank).  So the car loader serves a car we head for, or one in
 * front of us, before one we leave behind: a car takes it a second or two
 * to show */
#define SOON_S          1.f
#define LOOK_S          4.f
#define VIEW_M          150.f
/* choose which friends get the slots: visible ones the hub lists, our
 * crews first (wherever they are), then the nearest; a slot keeps its friend
 * while the friend stays visible, unless a
 * friend the hub lists has none and the slot's friend is out of sight (step
 * 4), left out of the hub's last list (step 5: roam_core keeps such a peer
 * T_HUB_GRACE, 2.5 s, and the car waiting for its slot appeared that much
 * later, near), or much farther (step 6: with the server's hub there can be
 * more friends near us than slots).  A friend whose car
 * cannot be made now (no car memory, a record not understood) is skipped
 * for this frame and the next one is served: a farther friend whose look a
 * parked car already wears, or one with a car that fits, is not kept
 * waiting behind him (int3a review: a nearest friend without memory hid
 * every other friend until he left) */
static void assign(CPU *c, int64_t host){
    int nslots = O.max_cars;
    uint8_t tried[RC_TABLE] = {0};             /* g_pv indices refused this frame */
    for (int k = 0; k < nslots; k++){
        Slot *s = &g_sl[k];
        if (s->st == SL_FREE || s->st == SL_PARKED) continue;
        int found = -1;
        for (int i = 0; i < g_npv; i++) if (g_pv[i].idx == s->peer && g_pv[i].id == s->peer_id) found = i;
        if (found < 0){ release(c, k, "left"); continue; }
        if (!g_pv[found].visible){
            if (!s->unseen_since) s->unseen_since = host;
            if (host - s->unseen_since > 60000000) release(c, k, "away for a minute");
        } else s->unseen_since = 0;
    }
    for (;;){
        /* the nearest visible friend without a slot */
        int best = -1;
        for (int i = 0; i < g_npv; i++){
            if (tried[i] || !g_pv[i].visible || !g_pv[i].listed || !g_pv[i].has_cfg || !g_pv[i].cfg_len || !g_pv[i].have_pose) continue;
            int has = 0;
            for (int k = 0; k < nslots; k++)
                if (g_sl[k].st != SL_FREE && g_sl[k].st != SL_PARKED && g_sl[k].peer == g_pv[i].idx && g_sl[k].peer_id == g_pv[i].id) has = 1;
            if (has) continue;
            if (best < 0 || (g_pv[i].crew != 0) > (g_pv[best].crew != 0) ||
                ((g_pv[i].crew != 0) == (g_pv[best].crew != 0) && g_pv[i].rank < g_pv[best].rank)) best = i;
        }
        if (best < 0) return;
        PeerView *v = &g_pv[best];
        int k = -1, reuse = 0;
        /* 1. a parked car already wearing this look: no load at all */
        for (int j = 0; j < nslots && k < 0; j++)
            if (g_sl[j].st == SL_PARKED && g_sl[j].look == v->look && ok(g_sl[j].car, 0x9e0)){ k = j; reuse = 1; }
        /* 2. a free slot: an opponent car while the outrun manager is idle,
         *    else one of our own */
        for (int j = 0; j < nslots && k < 0; j++){
            if (g_sl[j].st != SL_FREE) continue;
            if (j < NOPP){
                if (O.test_owncars) continue;
                uint32_t w = world(), car = w && g_em == EM_HELD ? free_opponent_car(w) : 0;
                if (!car) continue;
                g_sl[j].car = car; g_sl[j].world = w; g_sl[j].created = 0;
            }
            k = j;
        }
        /* 3. a parked car with another look: the new ride replaces it */
        for (int j = 0; j < nslots && k < 0; j++) if (g_sl[j].st == SL_PARKED && ok(g_sl[j].car, 0x9e0)) k = j;
        if (k < 0){
            /* 4. a friend out of sight: the one unseen the longest gives way */
            int far = -1;
            for (int j = 0; j < nslots; j++)
                if (g_sl[j].st == SL_ACTIVE && g_sl[j].unseen_since && !du_keeps(j) && (far < 0 || g_sl[j].unseen_since < g_sl[far].unseen_since)) far = j;
            /* 5. a friend the hub left out of its last list (going within
             *    T_HUB_GRACE): the farthest of them */
            if (far < 0){
                float fd = -1.f;
                for (int j = 0; j < nslots; j++){
                    if (g_sl[j].st != SL_ACTIVE || du_keeps(j)) continue;
                    for (int i = 0; i < g_npv; i++)
                        if (g_pv[i].idx == g_sl[j].peer && g_pv[i].id == g_sl[j].peer_id && !g_pv[i].listed && g_pv[i].rank > fd){ fd = g_pv[i].rank; far = j; }
                }
            }
            /* 6. else the farthest shown one, for a friend much nearer (well
             *    within 60 % of its distance and 50 m nearer: no swapping
             *    back and forth at the edge) -- with more players than
             *    slots, the cars shown are the nearest ones.  A crew's car
             *    first takes the farthest car of no crew, wherever it is
             *    (pass 0), and gives way only to a crew's */
            for (int pass = v->crew ? 0 : 1; pass < 2 && far < 0; pass++){
                float fd = 0.f; int fj = -1;
                for (int j = 0; j < nslots; j++){
                    if (g_sl[j].st != SL_ACTIVE || du_keeps(j) || (g_sl[j].crew && (pass == 0 || !v->crew))) continue;
                    for (int i = 0; i < g_npv; i++)
                        if (g_pv[i].idx == g_sl[j].peer && g_pv[i].id == g_sl[j].peer_id && g_pv[i].rank > fd){ fd = g_pv[i].rank; fj = j; }
                }
                if (fj >= 0 && (pass == 0 || (v->rank < fd * 0.6f && fd - v->rank > 50.f))) far = fj;
            }
            if (far < 0) return;
            int left_out = 0;
            for (int i = 0; i < g_npv; i++) if (g_pv[i].idx == g_sl[far].peer && g_pv[i].id == g_sl[far].peer_id) left_out = !g_pv[i].listed;
            release(c, far, g_sl[far].unseen_since ? "slot needed for a nearer friend" :
                            left_out ? "the server's hub links us no more" : v->crew ? "a friend of our crew takes the slot" :
                            "a much nearer friend takes the slot");
            continue;
        }
        Slot *s = &g_sl[k];
        int was = s->st;
        s->peer = v->idx; s->peer_id = v->id; s->unseen_since = 0; s->near_since = 0; s->idle_since = 0; s->soft = 0; s->crew = v->crew;
        snprintf(s->name, sizeof s->name, "%s", v->name);
        if (reuse){
            s->st = SL_ACTIVE; s->cfg_gen = v->cfg_gen; s->hidden = 1; g_acquires++; g_reuses++;
            set_name(s);
            rlog("[roam] f=%ld slot %d for %s, %.0f m away: the parked %s already wears this look\n", rh_frame(), k,
                    s->name, (double)v->dist, s->carname);
            continue;
        }
        if (g_heavy >= HEAVY_PER_FRAME || !load_may_start(c)){                 /* the next frame */
            s->peer = -1; s->peer_id = 0; s->name[0] = 0;
            if (was == SL_FREE && k < NOPP){ s->car = 0; s->world = 0; }
            return;
        }
        g_heavy++;
        if (k >= NOPP && !s->car){
            if (!begin_create(c, k)){ s->peer = -1; s->peer_id = 0; s->name[0] = 0; tried[best] = 1; continue; }
            s->st = SL_CREATING; g_acquires++;
            rlog("[roam] f=%ld slot %d (car of our own, being created) for %s, %.0f m away\n", rh_frame(), k, s->name,
                    (double)v->dist);
            continue;
        }
        if (!begin_load(c, k, v->cfg, v->cfg_len, v->cfg_gen)){
            /* not now (memory, record): try again later */
            s->peer = -1; s->peer_id = 0; s->name[0] = 0;
            if (was == SL_FREE && k < NOPP){ s->car = 0; s->world = 0; }
            tried[best] = 1;
            continue;
        }
        s->st = SL_LOADING; g_acquires++;
        rlog("[roam] f=%ld slot %d (%s) for %s, %.0f m away\n", rh_frame(), k,
                k < NOPP ? "free-roam opponent car" : "car of our own", s->name, (double)v->dist);
    }
}

/* a different world: every slot of the old one is gone */
static void world_changed(CPU *c, uint32_t w){
    if (g_world_seen){
        int any = 0; for (int k = 0; k < NSLOT; k++) if (g_sl[k].st != SL_FREE || g_sl[k].car) any = 1;
        if (any || g_npending) rlog("[roam] f=%ld world 0x%08x -> 0x%08x: friends' cars gone with it\n", rh_frame(), g_world_seen, w);
        world_gone();
        flush_unloads(c);                           /* before the next world loads its own cars */
        memset(&g_tag, 0, sizeof g_tag);            /* never touch the old HUD's objects */
        memset(&g_mm, 0, sizeof g_mm);              /* (nor its minimap's: the next HUD may be at its address) */
        g_pass.first = g_pass.end = 0;            /* its triggers too */
    }
    g_world_seen = w;
}

#ifndef ROAM_HOST_WIN
/* Solid cars switched while the game runs (the page's menu: roam_plat_solid).
 * Off: friends' cars lose their bodies at once, and the player's car is
 * passable on their screens (RC_BIT_PASS).  On: friends' cars are ghosts
 * until they are clear of the player's car, as when they appear, so nobody
 * is shoved out of a car that was inside theirs.  The cars slew to the other
 * rule's drawing delay (render_delay) */
static void solid_follow(void){
    int v = roam_plat_solid();
    if (v < 0 || v == O.collide) return;
    O.collide = v;
    if (v) ghost_all();
    rlog("[roam] f=%ld solid cars %s (the page's menu)\n", rh_frame(), v ? "on" : "off");
}
/* Crews or their colours changed while the game runs (the page's menu:
 * roam_plat_crews): the session takes the crews (its HELLOs tell the hub and
 * the peers at once), and the cars' colours follow from the next frame
 * (rc_peer_crew), by the new order and colours.  Only the hashes come: the
 * names in the log are the start's */
static uint32_t g_crews_gen;
static void crews_follow(void){
    uint32_t h[RC_CREW_MAX] = {0}, rgb[RC_CREW_MAX] = {0}, gen = roam_plat_crews(h, rgb);
    if (!gen || gen == g_crews_gen) return;
    g_crews_gen = gen;
    /* the colours go with their crews: a crew left out (0, a repeat) takes its colour along */
    uint32_t keep[RC_CREW_MAX] = {0}; int m = 0;
    for (int i = 0; i < RC_CREW_MAX; i++){
        int dup = !h[i];
        for (int k = 0; k < i; k++) dup |= h[k] == h[i];
        if (!dup && m < RC_CREW_MAX) keep[m++] = rgb[i];
    }
    for (int i = 0; i < RC_CREW_MAX; i++) O.crew_rgb[i] = i < m && keep[i] ? keep[i] : CREW_RGB[i];
    roam_plat_lock();
    rc_set_crews(&g_s, h, RC_CREW_MAX);
    int n = g_s.conf.n_crews; uint32_t c[RC_CREW_MAX] = {0};
    for (int i = 0; i < n; i++) c[i] = g_s.conf.crews[i];
    roam_plat_unlock();
    O.ncrew = n;
    for (int i = 0; i < RC_CREW_MAX; i++) O.crew[i][0] = 0;
    rlog("[roam] f=%ld crews changed (the page's menu): %d: %08x %08x %08x %08x, colours %06x %06x %06x %06x\n", rh_frame(), n,
         c[0], c[1], c[2], c[3], O.crew_rgb[0], O.crew_rgb[1], O.crew_rgb[2], O.crew_rgb[3]);
}
/* The room changed while the game runs (an invite link on the page: roam_plat_room): the session leaves the old one
 * (its friends' cars go as when they leave) and the hub links it in the new one at its next HELLO.  Not during an
 * outrun, a race lobby or a race: the change waits until the player is back to driving alone in free roam, where
 * leaving everyone breaks nothing.  A name that reads as a slur is no room, as at the start. */
static uint32_t g_room_gen;
static void room_follow(void){
    char name[64];
    uint32_t gen = roam_plat_room(name);
    if (!gen || gen == g_room_gen || g_racing) return;
#if DUELS
    if (g_du.st != DU_IDLE) return;
#endif
#if MPRACES
    if (g_mp.st != MP_IDLE) return;
#endif
    g_room_gen = gen;
    if (name_blocked(name)){ rlog("[roam] f=%ld the room from the page reads as a slur: no room\n", rh_frame()); name[0] = 0; }
    uint32_t h = rc_room_hash(name);
    roam_plat_lock();
    uint32_t was = g_s.conf.room;
    int left = rc_set_room(&g_s, h);
    g_alone_since = 0; g_alone_told = 0;
    roam_plat_unlock();
    snprintf(O.room, sizeof O.room, "%s", name);
    rlog("[roam] f=%ld room changed (the page): %08x -> %08x, %d friend(s) left with the old one\n", rh_frame(), was, h, left);
}
#endif
/* Engage does something of ours now (roam_game_engage: the touch controls' Start, beside the game's own markers and
 * outruns): a friend's outrun on the HUD to challenge or to take, a multiplayer-only race spot (an open race's
 * marker is the game's), a lobby to join, our lobby to start */
static int g_engage;
static int engage_ours(void){
    if (DUELS && g_du.icon) return 1;
#if MPRACES
    if (g_mp.st == MP_HOST) return g_mp.l.n >= 2;
    if (g_mp.st != MP_IDLE || g_mpd.dlg) return 0;
    const MpSpot *sp = mp_spot_here();
    if (sp) return sp->kind == MPS_ONLY;
    return mp_joinable() != 0;
#else
    return 0;
#endif
}
void roam_game_frame(CPU *c){
    if (!g_inited) init_once(c);
    if (!O.on) return;
#ifndef ROAM_HOST_WIN
    solid_follow();
    crews_follow();
    room_follow();
#endif
    g_step++;
    uint64_t t0 = rh_ns();
    int64_t host = roam_plat_now_us();
    int in_roam = roaming();
    uint32_t w = world();
    if (w != g_world_seen) world_changed(c, w);
    if (g_npending && (!w || in_roam)) flush_unloads(c);      /* the old world's cars are gone */
    if (g_packs_ok) packs_wake(c);                  /* after the game's hibernation (into the world) */
    /* sync clock */
    float wdt = rd32(G_FLOW) == 6u ? MRF32(G_WORLDDT) : 0.f;
    if (!__builtin_isfinite(wdt) || wdt < 0 || wdt > 1.f) wdt = 0;
    int64_t frame_dt = 0;                       /* the sync clock's step this frame (remote cars move by it) */
    int64_t hdt = host - g_last_host;
    if (hdt < 0 || hdt > 1000000) hdt = 0;
    if (!in_roam){
        g_sync = (double)host; g_sync_paused = 0;
        if (g_in_roam){ roam_plat_lock(); rc_bump_clock(&g_s); roam_plat_unlock(); }
    } else if (!g_in_roam){
        g_sync = (double)host; g_sync_paused = 0;
        roam_plat_lock(); rc_bump_epoch(&g_s); rc_bump_clock(&g_s); roam_plat_unlock();
        g_lp_ok = 0; g_cfg_next = 0;
        if (!g_gmem) gmem_init(c);
        memset(&g_tag, 0, sizeof g_tag);                /* the labels of this stay in free roam's HUD */
    } else if (wdt <= 0.f){
        /* paused, SMS, map, streaming: the local world stands, but friends
         * keep driving, so the clock follows the host clock */
        g_sync_paused++;
        if (hdt > 0){ g_sync += (double)hdt; frame_dt = hdt; }
    } else {
        /* the world moved: the clock advances by world dt (what the cars
         * moved by, so stamps and motion agree) and is pulled to the host
         * clock with a 250 ms time constant.  A world that runs slower than
         * real time -- the game's 50 ms dt clamp below 20 fps, a frame
         * limiter's smaller clamp, slow motion, hitches -- leaves it a
         * steady (1 - rate) x 250 ms behind instead of drifting off: a
         * friend's car still moves in real time on every screen, and a
         * re-anchor (a re-sync at every friend) is left for stalls of over
         * a second */
        double before = g_sync;
        g_sync += wdt * 1e6;
        double err = (double)host - g_sync;
        if (__builtin_fabs(err) > 1000000.0){
            g_sync = (double)host; g_clock_snaps++;
            roam_plat_lock(); rc_bump_clock(&g_s); roam_plat_unlock();
        } else {
            double k = (double)hdt / 250000.0;
            g_sync += err * (k < 1.0 ? k : 1.0);
            if (g_sync < before) g_sync = before;               /* never backwards */
        }
        double step = g_sync - before;
        frame_dt = (int64_t)step;
        /* world time per stamp time, smoothed alike (1 while the world runs
         * in real time) */
        double r = step > 1.0 ? (double)wdt * 1e6 / step : 1.0;
        if (r > 1.0) r = 1.0;
        if (r < 0.05) r = 0.05;
        double kk = (double)hdt / 250000.0;
        g_vscale += (r - g_vscale) * (kk < 1.0 ? kk : 1.0);
        g_sync_paused = 0;
    }
    g_in_roam = in_roam;
    g_last_host = host;
    /* the player's name: the profile's, unless NFSU2_ROAM_NAME */
    if (!O.name[0] && ok(G_PROFILE_NAME, 16) && MR8(G_PROFILE_NAME) >= 0x20){
        char pn[17]; cstr(G_PROFILE_NAME, pn, sizeof pn);
        if (pn[0] && !rh_no_profile(pn)){
            rc_name_clean(O.name, pn, 16);
            roam_plat_lock(); snprintf(g_s.conf.name, sizeof g_s.conf.name, "%s", O.name); roam_plat_unlock();
            rlog("[roam] my name: %s (the profile's; NFSU2_ROAM_NAME changes it)\n", O.name);
        }
    }
    /* local state and look; the horn only while the world runs (not in the
     * pause menu, the map or the messages), the stick click not for a car
     * with hydraulics (horn.h) */
    int world_runs = in_roam && g_sync_paused <= 1 && !g_parked;
    g_horn = world_runs && rh_horn_held(!has_hydraulics(player_car()));
    rc_state ls; int have_ls = in_roam && local_state(&ls);
    /* what friends can't see for themselves (last frame's): our car lets
     * every car through, we take no outrun now */
    if (have_ls && (!O.collide || g_pass.me)) ls.bits |= RC_BIT_PASS;
    if (have_ls && g_du.busy) ls.bits |= RC_BIT_BUSY;
    uint8_t cfgb[RC_CFG_MAX]; int cfgn = 0; char tname[RC_CFG_TYPE_NAME + 1] = {0};
    if (in_roam && host >= g_cfg_next){ cfgn = local_cfg(cfgb, sizeof cfgb, tname); g_cfg_next = host + 5000000; }
    if (have_ls){
        if (g_lp_ok){
            float dx = ls.p[0] - g_lp[0], dy = ls.p[1] - g_lp[1], dz = ls.p[2] - g_lp[2];
            float v = rc_sqrtf(ls.v[0]*ls.v[0] + ls.v[1]*ls.v[1] + ls.v[2]*ls.v[2]) / (float)g_vscale;
            if (rc_sqrtf(dx*dx + dy*dy + dz*dz) > v * wdt + 10.f){
                g_teleports++; ghost_all(); roam_plat_lock(); rc_bump_epoch(&g_s); roam_plat_unlock(); }
        }
        memcpy(g_lp, ls.p, sizeof g_lp); g_lp_ok = 1;
        for (int i = 0; i < 3; i++) g_lv[i] = ls.v[i] / (float)g_vscale;
        float f[3], l[3], u[3]; rc_q2m(ls.q, f, l, u);
        float fl = rc_sqrtf(f[0] * f[0] + f[1] * f[1]);
        g_lf[0] = fl > 1e-3f ? f[0] / fl : 0.f; g_lf[1] = fl > 1e-3f ? f[1] / fl : 0.f;
    }
    roam_plat_lock();
    uint8_t st = local_status(in_roam);
    rc_set_status(&g_s, st);
    g_racing = st == RC_ST_EVENT || (in_roam && (du_racing(rd32(G_RACE)) || em_racing(rd32(G_RACE))));
#ifndef ROAM_HOST_WIN
    room_alone(host, st);
#endif
    if (cfgn > 0){
        uint16_t before = g_s.cfg_rev;
        rc_set_cfg(&g_s, cfgb, cfgn, host);
        if (g_s.cfg_rev != before) rlog("[roam] f=%ld my car: %s, %d-byte record, revision %u\n", rh_frame(), tname, cfgn, g_s.cfg_rev);
    }
    if (have_ls) rc_push_state(&g_s, &ls, host);
    /* outrun messages for us (du_frame) */
    uint32_t duin[4 * RC_DUEL_INBOX]; int nduin = 0;
    while (nduin < RC_DUEL_INBOX){
        uint8_t op, arg;
        if (!rc_take_duel(&g_s, &duin[4 * nduin], &op, &arg, &duin[4 * nduin + 3])) break;
        duin[4 * nduin + 1] = op; duin[4 * nduin + 2] = arg; nduin++;
    }
#if MPRACES
    /* race lobby messages for us (mp_frame) */
    uint32_t mpfrom[RC_LOBBY_INBOX]; rc_lobby mpin[RC_LOBBY_INBOX]; int nmpin = 0;
    while (nmpin < RC_LOBBY_INBOX && rc_take_lobby(&g_s, &mpfrom[nmpin], &mpin[nmpin])) nmpin++;
#endif
#if SAYS
    say_frame(host, in_roam, st);                   /* quick chat: the player's pick out, friends' phrases to the page */
#endif
    /* what the slots need from the session */
    g_npv = 0;
    if (in_roam){
        for (int i = 0; i < RC_TABLE; i++){
            rc_peer *p = &g_s.peer[i];
            if (p->state != RC_PS_EST) continue;
            PeerView *v = &g_pv[g_npv++];
            v->idx = i; v->id = p->id; snprintf(v->name, sizeof v->name, "%s", p->name);
            /* shown while it is in free roam and its snapshots keep coming,
             * or while it is paused there: a paused game (pause menu, or a
             * window in the background: lifecycle.c parks the game, no
             * frames, no snapshots) still pings and announces, and its car
             * holds still where it stopped (roam_core.c).  With the snapshot
             * age rule alone a friend's car vanished 2 s after their window
             * lost focus, and came back when it got it again. */
            v->visible = !p->lost && p->mo.n > 0 &&
                         (p->status == RC_ST_PAUSED ||
                          (p->status == RC_ST_ROAM && host - p->last_snap_rx < 2000000));
            v->paused = p->status == RC_ST_PAUSED; v->duels = p->duels; v->listed = rc_peer_listed(&g_s, i);
            v->crew = rc_peer_crew(&g_s, i) + 1;
            v->cfg_gen = p->cfg_gen; v->cfg_len = 0; v->has_cfg = p->cfg_len > 0; v->err = 0;
            int slotted = -1;
            for (int k = 0; k < NSLOT; k++) if (g_sl[k].st != SL_FREE && g_sl[k].st != SL_PARKED && g_sl[k].peer == i && g_sl[k].peer_id == p->id) slotted = k;
            if (slotted >= 0) g_sl[slotted].crew = v->crew;
            /* the record only when a slot will need it */
            if (p->cfg_len && (slotted < 0 || g_sl[slotted].cfg_gen != p->cfg_gen)){
                memcpy(v->cfg, p->cfg, (size_t)p->cfg_len); v->cfg_len = p->cfg_len; v->look = rc_fnv1a_bytes(p->cfg, (uint32_t)p->cfg_len); }
            /* their horn: the record's (read once a record), the dual one without */
            if (g_phorn[i].id != p->id || g_phorn[i].gen != p->cfg_gen){
                static rc_carcfg hc;
                g_phorn[i].id = p->id; g_phorn[i].gen = p->cfg_gen;
                g_phorn[i].horn = p->cfg_len && rc_carcfg_decode(p->cfg, p->cfg_len, &hc) == RC_OK && hc.has_horn ? hc.horn : 0;
                if (p->cfg_len && g_phorn[i].horn) rlog("[roam] f=%ld %s's horn: %s\n", rh_frame(), p->name, rh_horn_name(g_phorn[i].horn));
            }
            v->horn = g_phorn[i].horn;
            v->have_pose = 0;
            v->newest = p->mo.n > 0 ? p->mo.t[p->mo.n - 1] : 0;
            if (v->visible){
                if (slotted >= 0 && g_sl[slotted].st == SL_ACTIVE){
                    /* the car was not drawn last frame (just loaded, the
                     * friend back from a park, a stall, a shop, or we are
                     * back in free roam): it appears where the friend is,
                     * not blended from where it was last drawn seconds ago
                     * (int3a review: up to 5 m of slide after a clock
                     * re-anchor) */
                    if (g_sl[slotted].hidden) p->mo.snap_pending = 1;
                    v->have_pose = rc_interp_eval(&p->mo, (int64_t)g_sync, frame_dt, render_delay(i, p, frame_dt),
                                                  (int64_t)O.extrap_ms * 1000, &v->pose);
                    v->rt = p->mo.prev_rt;
                }
                else v->have_pose = rc_interp_peek(&p->mo, p->mo.t[p->mo.n - 1], 0, &v->pose);
                /* at the new career's start: not shown, as in a shop (its slot
                 * waits a minute, or goes to a friend who needs one) */
                if (v->have_pose && at_spawn(v->pose.p)) v->visible = 0;
            }
            float dx = v->pose.p[0] - g_lp[0], dy = v->pose.p[1] - g_lp[1];
            v->dist = v->have_pose ? rc_sqrtf(dx*dx + dy*dy) : 1e9f;
            float ux = v->pose.v[0] - g_lv[0], uy = v->pose.v[1] - g_lv[1];
            float sx = dx + ux * SOON_S, sy = dy + uy * SOON_S, ax = dx + ux * LOOK_S, ay = dy + uy * LOOK_S;
            float wx = sx - g_lf[0] * VIEW_M, wy = sy - g_lf[1] * VIEW_M;
            float sd = rc_sqrtf(sx*sx + sy*sy), ad = rc_sqrtf(ax*ax + ay*ay), wd = rc_sqrtf(wx*wx + wy*wy);
            float rk = sd < ad ? sd : ad;
            v->rank = !v->have_pose ? v->dist : wd < rk ? wd : rk;
            v->err = rc_sqrtf(p->mo.err_p[0] * p->mo.err_p[0] + p->mo.err_p[1] * p->mo.err_p[1] + p->mo.err_p[2] * p->mo.err_p[2]);
        }
    }
    roam_plat_unlock();
    /* slots (guest calls, outside the lock) */
    if (!in_roam){
        if (w) park_all(c);
        g_pass.stay_since = g_pass.blocking = 0;
    } else {
        /* the outrun challengers' manager first: whether opponent cars are
         * free this frame depends on it (LEND_OPP) */
        const char *em_why = 0;
#if LEND_OPP
        for (int k = 0; k < NSLOT; k++) if (g_sl[k].st != SL_FREE && g_sl[k].st != SL_PARKED) em_why = "friends are here";
        for (int i = 0; i < g_npv; i++) if (g_pv[i].visible && g_pv[i].has_cfg && g_pv[i].have_pose) em_why = "friends are here";
#else
        if (g_du.st != DU_IDLE) em_why = "an outrun between players";
        else if (mp_busy()) em_why = "a race lobby";
#endif
        em_update(c, em_why, host);
        soft_me(c, host, st);
        if (DUELS) kr_frame();
#if MPRACES
        spots_read(c, host);
#endif
        g_heavy = 0;
        rides_tidy(c);
        loader_watch(c, host);
        if (!rd32(G_TRACKSTREAMER + 0x168u)) stream_hold_end(host);
        assign(c, host);
        for (int k = 0; k < NSLOT; k++){
            Slot *s = &g_sl[k];
            if (s->st == SL_FREE || s->st == SL_PARKED) continue;
            PeerView *v = 0;
            for (int i = 0; i < g_npv; i++) if (g_pv[i].idx == s->peer && g_pv[i].id == s->peer_id) v = &g_pv[i];
            if (!v) continue;
            /* a second first: a hitch in the friend's game (streaming) is a pause too */
            if (!v->paused) s->paused_since = 0;
            else if (!s->paused_since) s->paused_since = host;
            s->paused = s->paused_since && host - s->paused_since >= 1000000;
            if (s->st == SL_CREATING){
                if (g_heavy >= HEAVY_PER_FRAME || !load_may_start(c)) continue;   /* CreateCar (and the ride after): the next frame */
                int r = finish_create(c, k);
                if (r > 0) g_heavy++;
                if (r < 0){ release(c, k, "could not create a car"); continue; }
                if (r > 0){
                    if (v->cfg_len > 0 && begin_load(c, k, v->cfg, v->cfg_len, v->cfg_gen)) s->st = SL_LOADING;
                    else release(c, k, "the car could not be loaded now");
                }
                continue;
            }
            if (s->st == SL_LOADING){
                int r = finish_load(c, k);
                if (r > 0) s->st = SL_ACTIVE;
                else if (r < 0) release(c, k, "load failed");
                continue;
            }
            if (!ok(s->car, 0x9e0)) continue;
            /* a new look: load it while the old one stays on */
            if (!s->h_new && v->cfg_len > 0 && v->cfg_gen != s->cfg_gen && g_heavy < HEAVY_PER_FRAME && load_may_start(c)
                && begin_load(c, k, v->cfg, v->cfg_len, v->cfg_gen))
                g_heavy++;
            if (s->h_new){
                int r = finish_load(c, k);
                if (r < 0) s->cfg_gen = v->cfg_gen;                 /* do not retry this one */
            }
            if (v->visible && v->have_pose){
                make_inert(c, s->car);
                if (s->hidden) push_off(s);
                push_pose(s, &v->pose, v->newest, v->rt, (float)frame_dt / 1e6f);
                s->rt = v->rt;
                /* appearing, or a jump (the friend's teleport, a snap): a
                 * ghost until clear, never shoving the player's car out */
                float jx = v->pose.p[0] - s->drawn[0], jy = v->pose.p[1] - s->drawn[1], jz = v->pose.p[2] - s->drawn[2];
                if (s->hidden || jx * jx + jy * jy + jz * jz > JUMP_M * JUMP_M){
                    s->ghost = 1;
                    for (int i = 0; i < 3; i++){ v->pose.p[i] -= s->push_u[i] + s->push_r[i]; v->pose.v[i] -= s->push_v[i]; }
                    push_off(s);
                }
                apply_pose(c, s, &v->pose, (float)frame_dt / 1e6f);
                soft_update(s, &v->pose, host);
                body_update(c, k);
                for (int i = 0; i < 3; i++){ s->drawn[i] = v->pose.p[i]; s->drawn_v[i] = v->pose.v[i]; }
                s->hidden = 0; MR8(s->car + CAR_ACTIVE) = 1;
                unblink(s);
                if (g_poselog)
                    rh_fprintf(g_poselog, "R,%lld,%.0f,%08x,%.4f,%.4f,%.4f,%.3f,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", (long long)host, g_sync, s->peer_id,
                            (double)v->pose.p[0], (double)v->pose.p[1], (double)v->pose.p[2], (double)v->pose.speed, v->pose.mode, (double)v->err,
                            (double)MRF32(s->car + 0x90u), (double)MRF32(s->car + 0x94u), (double)MRF32(s->car + 0x98u), (double)MRF32(s->car + 0xb8u),
                            (double)MRF32(s->car + 0x50u), (double)MRF32(s->car + 0x54u), (double)MRF32(s->car + 0x58u));
            } else if (!s->hidden && !(g_du.st == DU_RACE && s->car == g_du.car)){ soft_park(c, s->car); s->hidden = 1; }
            /* (an outrun's opponent stays where it was: parked, the game would count it 120 km behind) */
        }
        arrows_update(c, w);
        arrow_colours(c, w);
        map_update(c, w, host);
        /* a screen over the HUD: the HUD's update, which places the names
         * (roam_game_hud), does not run under the world map, and they stayed
         * on it where the cars had been on the screen */
        uint32_t pl = rd32(G_PLAYER1), hud = pl && ok(pl, PL_HUD + 4u) ? rd32(pl + PL_HUD) : 0;
        if (hud && hud == g_tag.hud && ok(hud, 0x14) && !hud_on_top(rd32(hud + 0x10u))) for (int i = 0; i <= TAG_N; i++) tag_hide(c, i);
#if LEND_OPP
        int busy = 0;
        for (int k = 0; k < NSLOT; k++) if (g_sl[k].st != SL_FREE && g_sl[k].st != SL_PARKED) busy = 1;
        if (busy) g_em_release_at = 0;
        else if (g_em == EM_HELD){
            /* no friend's car in this world for a minute: the opponent cars
             * get their own rides back and the outrun challengers return */
            if (!g_em_release_at) g_em_release_at = host + 60000000;
            if (host >= g_em_release_at){
                for (int k = 0; k < NOPP; k++) if (g_sl[k].st == SL_PARKED){ restore(c, k); g_restores++; }
                if (!opp_slots_used()) em_let_go();
                g_em_release_at = 0;
            }
        }
#else
        if (!em_why && g_em == EM_HELD) em_let_go();   /* the outrun between players or the lobby is over */
#endif
    }
#if SAYS
    bump_frame(host, in_roam && world_runs && st == RC_ST_ROAM && !g_racing && g_du.st == DU_IDLE && !mp_busy());
#endif
    du_frame(c, host, in_roam, world_runs, duin, nduin);
#if MPRACES
    mp_frame(c, host, in_roam, world_runs, mpfrom, mpin, nmpin);
    mp_panel(host, in_roam);
#endif
    g_engage = in_roam && world_runs && engage_ours();
    sounds_update(have_ls ? &ls : 0, world_runs);
    if (g_poselog && have_ls){
        uint32_t me = player_car();
        rh_fprintf(g_poselog, "L,%lld,%.0f,%08x,%.4f,%.4f,%.4f,%.3f,0,0,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", (long long)host, g_sync, g_s.conf.self_id,
                (double)ls.p[0], (double)ls.p[1], (double)ls.p[2], (double)ls.speed,
                (double)MRF32(me + 0x90u), (double)MRF32(me + 0x94u), (double)MRF32(me + 0x98u), (double)MRF32(me + 0xb8u),
                (double)MRF32(me + 0x50u), (double)MRF32(me + 0x54u), (double)MRF32(me + 0x58u));
        static int nfl; if (++nfl % 240 == 0) rh_fflush(g_poselog);
    }
    cost_add(t0);
    periodic_log(host);
#ifdef ROAM_HOST_SCRIPT
    for (int j = 0; j < g_njob; j++) run_job(c, g_job[j]);
    g_njob = 0;
#endif
}

/* the port's hook after the HUD update (roam.c, in 0x600590 behind its call
 * of 0x4EC5A0, on the guest thread): the names over friends' cars, placed
 * with this frame's camera, where the game places its own */
void roam_game_hud(CPU *c){
    if (!O.on || !g_net_ok) return;
    uint64_t t0 = rh_ns();
    tags_update(c);
    if (DUELS) du_hud(c, roam_plat_now_us());
#if MPRACES
    mp_labels(c);
#endif
    g_cost_hud += rh_ns() - t0;
}

/* photo mode (photo.c): whether friends' names can show here, and the names over their cars, and the player's own over
 * theirs, placed for photo mode's camera (its frame of the front end, which draws them alone): the FE objects shown */
int roam_game_names_here(void){ return O.on && g_net_ok && O.labels && g_in_roam && MR8(G_SHOW_NAMES); }
int roam_game_names(CPU *c, uint32_t *obj, int max){
    if (!roam_game_names_here()) return 0;
    g_tag_self = rh_frame();
    tags_update(c);
    int n = 0;
    for (int i = 0; i < TAG_N && n < max; i++) if (g_tag.shown[i] && g_tag.obj[i]) obj[n++] = g_tag.obj[i];
    return n;
}

/* ---- report at exit ----------------------------------------------------------------------------- */
void roam_game_report(void){
    if (!O.on || !g_net_ok) return;
    roam_plat_stats ps; roam_plat_get_stats(&ps);
    rlog("\n  [roam] free-roam multiplayer: joins %d, leaves %d, slots acquired %u / released %u, cars created %u, "
                    "load failures %u, looks reused %u, rides given back %u, local teleports %u, clock re-anchors %u\n", __atomic_load_n(&g_joins, __ATOMIC_RELAXED),
            __atomic_load_n(&g_leaves, __ATOMIC_RELAXED), g_acquires, g_releases, g_creates, g_load_fail, g_reuses, g_restores, g_teleports, g_clock_snaps);
    rlog("  [roam] datagrams sent %llu (%llu bytes), received %llu (%llu bytes), blocked %llu, send errors %llu, "
                    "impairment dropped %llu duplicated %llu\n", (unsigned long long)ps.sent, (unsigned long long)ps.sent_bytes,
            (unsigned long long)ps.recv, (unsigned long long)ps.recv_bytes, (unsigned long long)ps.blocked,
            (unsigned long long)ps.send_err, (unsigned long long)ps.impair_drop, (unsigned long long)ps.impair_dup);
    if (g_du.offers || g_du.races) rlog("  [roam] outruns with friends: %u challenge(s) made, %u raced, %u won, %u lost\n",
                                        g_du.offers, g_du.races, g_du.wins, g_du.losses);
    if (g_say.said || g_say.heard || g_say.dropped) rlog("  [roam] quick chat: %u said, %u heard, %u not shown, %u lost (inbox full)\n",
                                                         g_say.said, g_say.heard, g_say.dropped, g_s.say_dropped);
    rlog("  [roam] game-thread cost per frame: mean %.1f us, max %.1f us over %llu frames\n",
            g_cost_n ? g_cost_sum / 1000.0 / (double)g_cost_n : 0.0, g_cost_max / 1000.0, (unsigned long long)g_cost_n);
    if (g_poselog){ rh_fflush(g_poselog); }
    roam_plat_stop(0);
}

/* ---- script commands ("roam mp ...", roam.c; the port's test harness only) ------------------- */
#ifdef ROAM_HOST_SCRIPT
#define G_HOTPOS        0x00890140u
#define G_HOTPOS_ANGLE  0x008900d0u
#define G_HOTPOS_JUMP   0x008900cdu
#define G_HOTPOS_VALID  0x008900ceu
/* the crew (1..) whose colour an FE object has, 0: none (mp status, mp map) */
static int colour_crew(uint32_t o){
    int32_t col[4];
    if (!fe_colour_get(o, col)) return 0;
    uint32_t rgb = (uint32_t)(col[2] & 0xff) << 16 | (uint32_t)(col[1] & 0xff) << 8 | (uint32_t)(col[0] & 0xff);
    for (int i = 0; i < O.ncrew; i++) if (O.crew_rgb[i] == rgb) return i + 1;
    return 0;
}
/* an FE object has the AI racers' grey (mp status, mp map) */
static int colour_ai(uint32_t o){
    int32_t col[4];
    return fe_colour_get(o, col) && ((uint32_t)(col[2] & 0xff) << 16 | (uint32_t)(col[1] & 0xff) << 8 | (uint32_t)(col[0] & 0xff)) == g_ai_rgb;
}
/* the player jumps (hot-position jump) back metres behind a car, left
 * metres to its left, turned by turn degrees from its heading */
static void jump_behind(uint32_t car, float back, float left, float turn){
    float fx = MRF32(car + 0x90u), fy = MRF32(car + 0x94u), lx = MRF32(car + 0xa0u), ly = MRF32(car + 0xa4u);
    MRF32(G_HOTPOS) = MRF32(car + 0x60u) - back * fx + left * lx; MRF32(G_HOTPOS + 4) = MRF32(car + 0x64u) - back * fy + left * ly;
    MRF32(G_HOTPOS + 8) = MRF32(car + 0x68u) + 0.5f;
    MR16(G_HOTPOS_ANGLE) = (uint16_t)(MR16(car + 0x40cu) + (int)(turn * 65536.0f / 360.0f));
    MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
}
/* the HUD's outrun icon is up (mp outrun): FN_HUD_OUTRUN's icon 9 in the
 * HUD's icon list (+0x58: 8 entries of 0x14 from +0x60, +0 its state, 0
 * free, +1 the icon) */
static int du_icon_up(void){
    uint32_t pl = world() ? rd32(G_PLAYER1) : 0, hud = pl ? rd32(pl + PL_HUD) : 0, il = hud ? rd32(hud + 0x58u) : 0;
    if (!ok(il, 0x60u + 8u * 0x14u)) return 0;
    for (uint32_t i = 0; i < 8; i++) if (MR8(il + 0x60u + 0x14u * i) && MR8(il + 0x61u + 0x14u * i) == 9u) return 1;
    return 0;
}
int roam_game_cmd(CPU *c, const char *args, long *wait_start){
    (void)c;
    char sub[32] = {0}, a1[200] = {0}, a2[64] = {0}; int used = 0;
    sscanf(args, "%31s %n", sub, &used);
    sscanf(args + used, "%199s %63s", a1, a2);
    long f = rh_frame();
    if (!O.on){ rlog("[roam] f=%ld mp %s: multiplayer is off (NFSU2_ROAM=1)\n", f, sub); return 1; }
    /* "mp crewrgb rrggbb,rrggbb,..": the crews' colours from now on (the names and arrows at once, the map at its
     * next opening) */
    if (!strcasecmp(sub, "crewrgb")){
        crew_rgb_parse(a1);
        rlog("[roam] f=%ld mp crewrgb: %06x %06x %06x %06x\n", f, O.crew_rgb[0], O.crew_rgb[1], O.crew_rgb[2], O.crew_rgb[3]);
        return 1;
    }
    /* "mp airgb rrggbb": the AI racers' grey from now on (the arrows at once, the map at its next opening) */
    if (!strcasecmp(sub, "airgb")){
        char *e = 0; uint32_t v = (uint32_t)strtol(a1, &e, 16);
        g_ai_rgb = e == a1 + 6 && !*e ? v : AI_RGB;
        rlog("[roam] f=%ld mp airgb: %06x\n", f, g_ai_rgb);
        return 1;
    }
#if SAYS
    if (!strcasecmp(sub, "bump")){
        /* front bumpers (bump_*): for each friend's car, how the two cars stand to each other now */
        uint32_t me = player_car();
        rlog("[roam] f=%ld mp bump: free roam proper %d, my car %08x, nose %.2f m\n", f, g_bump_can, me,
             me && ok(me, 0x9e0) ? (double)bump_nose(me, MRF32(me + 0x90u), MRF32(me + 0x94u)) : 0.);
        for (int k = 0; k < NSLOT; k++){
            const Slot *s = &g_sl[k];
            if (s->st != SL_ACTIVE || !ok(s->car, 0x9e0) || !me || !ok(me, 0x9e0)) continue;
            float f0x = 0.f, f0y = 0.f, f1x = 0.f, f1y = 0.f, gap = 0.f;
            int fw = bump_fwd(me, &f0x, &f0y) && bump_fwd(s->car, &f1x, &f1y);
            int t = bump_touch(me, s->car, &gap);
            float dx = MRF32(s->car + 0x60u) - MRF32(me + 0x60u), dy = MRF32(s->car + 0x64u) - MRF32(me + 0x64u);
            rlog("[roam]   %s: %.2f m apart, facing %.2f, its nose %.2f m, %s (gap %.2f m), %s%s\n", s->name, (double)rc_sqrtf(dx * dx + dy * dy),
                 fw ? (double)(f0x * f1x + f0y * f1y) : 0., fw ? (double)bump_nose(s->car, f1x, f1y) : 0., t ? "nose to nose" : "not nose to nose",
                 (double)gap, g_bump[k].on ? "told the page" : g_bump[k].since ? "touching" : "apart", s->hidden ? ", hidden" : s->paused ? ", paused" : "");
        }
        return 1;
    }
#endif
    if (!strcasecmp(sub, "status")){
        roam_plat_lock();
        rlog("[roam] f=%ld mp status: %s, %d friend(s), my cfg rev %u\n", f, rc_status_name(g_s.status), rc_established(&g_s), g_s.cfg_rev);
        for (int i = 0; i < RC_TABLE; i++){
            const rc_peer *p = &g_s.peer[i]; char b[16];
            if (p->state == RC_PS_FREE) continue;
            rlog("[roam]   peer %d %s:%u id %08x state %d %s (%s) cfg rev %u%s, %d states, crew %d (%d crews, reads them %d; %d of ours by the hub)\n", i,
                    roam_ip_str(p->ip, b), p->port, p->id, p->state, p->name, rc_status_name(p->status), p->cfg_rev, p->cfg_len ? "" : " (none yet)",
                    p->mo.n, rc_peer_crew(&g_s, i) + 1, p->n_crews, p->reads_crews, p->n_hub_crews);
        }
        roam_plat_unlock();
        uint32_t icons = rd32(G_MM_ICONS); int arrows = 0, freev = 0, tinted = 0, wrong = 0, ais = 0, grey = 0, greyf = 0; uint8_t has[256] = {0};
        for (uint32_t n = ok(icons, 0x1c) ? rd32(icons) : 0, m = 0; n && n != icons && m < 256; n = rd32(n), m++)
            if (ok(rd32(n + 0x1cu), 8) && MR32(rd32(n + 0x1cu) + 4u) == MM_AIRACER){
                arrows++; has[(uint8_t)MR32(n + 8u)] = 1;
                /* (arrows in a crew's colour, and of those the ones whose car is not a crew-mate's; the AI racers'
                 * arrows, those of them grey, and grey ones of a friend's car) */
                int ct = colour_crew(MR32(rd32(n + 0x1cu))), ai = !racer_friend(world(), (int16_t)MR32(n + 8u)), gr = colour_ai(MR32(rd32(n + 0x1cu)));
                tinted += ct != 0; wrong += ct && !racer_crew(world(), (int16_t)MR32(n + 8u));
                ais += ai; grey += ai && gr; greyf += !ai && gr;
            }
        for (uint32_t n = rd32(G_MM_VISUALS), m = 0; n && n != G_MM_VISUALS && m < 256; n = rd32(n), m++)
            if (ok(rd32(n + 8u), 8) && MR32(rd32(n + 8u) + 4u) == MM_AIRACER) freev++;
        /* friends' cars shown, and of those with an arrow; hidden ones still in the world (an arrow at the parking spot) */
        int shown = 0, with = 0, stray = 0;
        for (int k = 0; k < NSLOT; k++){
            uint32_t car = g_sl[k].car;
            if (g_sl[k].st == SL_FREE || !ok(car, 0x9e0)) continue;
            if (g_sl[k].st == SL_ACTIVE && !g_sl[k].hidden){ shown++; with += has[(uint8_t)MR16(car + 0xau)]; }
            else if (MR8(car + CAR_ACTIVE)) stray++;
        }
        int cdrawn = 0;                             /* copies the renderer has drawn (their render object made) */
        for (int i = 0; i < g_mm.ncopy; i++) cdrawn += ok(g_mm.copy[i], 0x5c) && MR32(g_mm.copy[i] + 0x58u);
        rlog("[roam]   minimap: %d cars' arrows, %d free, %d copies of AIRACERINDICATOR_0 (%d drawn); %d of %d friends' cars shown have one, "
                        "%d hidden one(s) in the world; %d in a crew's colour (%d of them not a crew-mate's car); %d AI racers', %d of them grey "
                        "(%d grey friends')\n", arrows, freev, g_mm.ncopy, cdrawn, with, shown, stray, tinted, wrong, ais, grey, greyf);
        for (int k = 0; k < TAG_N; k++)
            if (g_tag.shown[k] && g_tag.obj[k])
                rlog("[roam]   name %d: \"%s\" colour %06x at %.1f %.1f\n", k, g_tag.text[k], g_tag.rgb[k],
                     ok(rd32(g_tag.obj[k] + 0x2cu), 0x40) ? (double)MRF32(rd32(g_tag.obj[k] + 0x2cu) + 0x1cu) : 0.,
                     ok(rd32(g_tag.obj[k] + 0x2cu), 0x40) ? (double)MRF32(rd32(g_tag.obj[k] + 0x2cu) + 0x20u) : 0.);
        uint32_t me = player_car();
        if (me) rlog("[roam]   me: car 0x%08x at %.1f %.1f %.1f heading 0x%04x speed %.1f\n", me, (double)MRF32(me + 0x60u),
                        (double)MRF32(me + 0x64u), (double)MRF32(me + 0x68u), MR16(me + 0x40cu), (double)MRF32(me + 0x42cu));
        for (int k = 0; k < NSLOT; k++) if (g_sl[k].st != SL_FREE){
            uint32_t car = g_sl[k].car;
            float dx = car ? MRF32(car + 0x60u) - (me ? MRF32(me + 0x60u) : 0) : 0, dy = car ? MRF32(car + 0x64u) - (me ? MRF32(me + 0x64u) : 0) : 0;
            rlog("[roam]   slot %d: %s %s car 0x%08x %s, %u frames posed; at %.1f %.1f %.1f (%.1f m away) heading 0x%04x, "
                            "on track %u, active %u, driver %u, physics %u, render 0x%08x, index %d, drawn at %.1f %.1f %.1f\n", k, g_sl[k].name, g_sl[k].carname, car,
                    g_sl[k].st == SL_ACTIVE ? (g_sl[k].hidden ? "hidden" : "shown") : g_sl[k].st == SL_PARKED ? "parked" : "loading", g_sl[k].pose_frames,
                    car ? (double)MRF32(car + 0x60u) : 0, car ? (double)MRF32(car + 0x64u) : 0, car ? (double)MRF32(car + 0x68u) : 0,
                    sqrt((double)(dx * dx + dy * dy)), car ? MR16(car + 0x40cu) : 0, car ? MR8(car + 0x5c0u) : 0, car ? MR8(car + 0x550u) : 0,
                    car ? MR32(car + 0x24u) : 0, car ? MR32(car + 0x28u) : 0, car ? MR32(car + 0x554u) : 0, car ? (int16_t)MR16(car + 8u) : -1,
                    car ? (double)MRF32(car + 0x50u) : 0, car ? (double)MRF32(car + 0x54u) : 0, car ? (double)MRF32(car + 0x58u) : 0);
            uint32_t body = car ? rd32(car + 0x3cu) : 0;
            if (car) rlog("[roam]     %s%s%s%s%s%s%s%s%s%s; collision body %s%s; %u hit(s) with my car (at most %.1f m/s)\n", !O.collide ? "solid cars off" : g_sl[k].soft ? "passable:" : g_sl[k].ghost ? "a ghost" : "solid",
                          g_sl[k].soft & SOFT_PAUSED ? " paused" : "", g_sl[k].soft & SOFT_IDLE ? " standing" : "",
                          g_sl[k].soft & SOFT_DOOR ? " near a door" : "", g_sl[k].soft & SOFT_SPAWN ? " at the start" : "",
                          g_sl[k].soft & SOFT_EVENT ? " near my event" : "", g_sl[k].soft & SOFT_BLOCK ? " my car in the way" : "",
                          g_sl[k].soft & SOFT_SAYS ? " says so" : "", g_sl[k].soft & SOFT_TRAFFIC ? " a traffic car" : "",
                          g_sl[k].soft && g_sl[k].ghost ? " (then a ghost)" : "",
                          !ok(body, 0x20) ? "none" : MR8(body + 0x17u) ? "off" : "on", DUELS && du_busy(k) ? "; takes no outrun now" : "",
                          g_sl[k].hits, (double)g_sl[k].hit_max);
            if (car) rlog("[roam]     nitrous %u%% (the game sounds it: %s), wheels at %.3f %.3f %.3f %.3f m in the car, front spinners at %.2f rad turning %.2f rad/s, "
                          "blinked out %u (blink from %u)\n",
                          nos_left(car), game_sounds_nos(car) ? "yes" : "no", (double)MRF32(car + 0x168u), (double)MRF32(car + 0x1a8u), (double)MRF32(car + 0x1e8u),
                          (double)MRF32(car + 0x228u), (double)MRF32(car + CAR_SPIN), (double)MRF32(car + CAR_SPIN + 0x10u), MR8(car + CAR_BLINKOUT), MR32(car + CAR_BLINK_T0));
        }
        return 1;
    }
    if (!strcasecmp(sub, "doors") || !strcasecmp(sub, "events")){
        /* the doors' or the player's open events' boxes, and the player's own
         * reasons (sent as RC_BIT_PASS with ghost cars too) */
        uint32_t me = player_car();
        int ev = !strcasecmp(sub, "events"), n = ev ? g_pass.nev : g_pass.n;
        float mx = me ? MRF32(me + 0x60u) : 0, my = me ? MRF32(me + 0x64u) : 0;
        int64_t now = roam_plat_now_us();
        int cars = 0, hits = block_hits(now, &cars);
        rlog("[roam] f=%ld mp %s: %d doors, %d open events of %d; the player at %.1f %.1f is%s%s%s%s%s%s%s%s; sends%s%s; "
                "on its spot for %.0f s, %d hit(s) from %d players' cars in %.0f s\n", f, sub, g_pass.n,
                g_pass.nev, g_pass.evall, (double)mx, (double)my,
                g_pass.me ? "" : " not passable", g_pass.me & SOFT_PAUSED ? " paused" : "", g_pass.me & SOFT_IDLE ? " standing" : "",
                g_pass.me & SOFT_DOOR ? " near a door" : "", g_pass.me & SOFT_EVENT ? " near an event" : "",
                g_pass.me & SOFT_SPAWN ? " at the start" : "", g_pass.me & SOFT_BLOCK ? " in the way" : "",
                g_pass.me & SOFT_TRAFFIC ? " a traffic car" : "", !O.collide || g_pass.me ? " passable" : "", g_du.busy ? " no outruns" : (!O.collide || g_pass.me ? "" : " nothing"),
                g_pass.stay_since ? (double)(now - g_pass.stay_since) / 1e6 : 0.0, hits, cars, (double)BLOCK_WIN_US / 1e6);
        if (ev) rlog("[roam]   the last sweep of the event triggers: %.0f us in %d frames, at most %.0f us a frame\n",
                     (double)g_pass.ev_cost[0] / 1e3, (int)g_pass.ev_cost[2], (double)g_pass.ev_cost[1] / 1e3);
        for (int i = 0; i < n; i++){
            const float *b = ev ? g_pass.ev[i].box : g_pass.box[i];
            float cx = (b[0] + b[2]) / 2 - mx, cy = (b[1] + b[3]) / 2 - my;
            /* what an event leads to: a race or another (their names in the string pool), a shop or a showcase (theirs) */
            char what[48] = ""; const OpenEvent *v = ev ? &g_pass.ev[i] : 0;
            if (v && (v->kind == 1 || v->kind == 4) && ok(v->what, 2)) cstr(rd32(G_STRPOOL) + MR16(v->what), what, 32);
            else if (v && (v->kind == 2 || v->kind == 3)) cstr(v->what, what, 32);
            rlog("[roam]   %s %d%s%s: %.1f %.1f to %.1f %.1f (%.1f x %.1f m), its centre %.0f m away\n", ev ? "event" : "door", i,
                    v ? (v->kind == 1 ? " race " : v->kind == 3 ? " showcase " : v->kind == 2 ? " shop " : " other ") : "", what, (double)b[0],
                    (double)b[1], (double)b[2], (double)b[3], (double)(b[2] - b[0]), (double)(b[3] - b[1]), sqrt((double)(cx * cx + cy * cy)));
        }
        return 1;
    }
#if MPRACES
    if (!strcasecmp(sub, "spots")){
        /* the race spots (spots_read): the nearest first, each with its race,
         * Solo or multiplayer only, and why */
        uint32_t me = player_car();
        float mx = me ? MRF32(me + 0x60u) : 0, my = me ? MRF32(me + 0x64u) : 0;
        int open = 0;
        for (int i = 0; i < g_spot.n; i++) open += g_spot.s[i].kind == MPS_OPEN;
        rlog("[roam] f=%ld mp spots: stage %u, %d race spots (%d open in the career), the player at %.1f %.1f\n", f,
             rd32(G_CAREER + C_STAGE), g_spot.n, open, (double)mx, (double)my);
        int order[MP_SPOTS], n = g_spot.n;
        float dist[MP_SPOTS];
        for (int i = 0; i < n; i++){
            const float *b = g_spot.s[i].box;
            float cx = (b[0] + b[2]) / 2 - mx, cy = (b[1] + b[3]) / 2 - my, d = cx * cx + cy * cy;
            int j = i;
            for (; j > 0 && dist[j - 1] > d; j--){ dist[j] = dist[j - 1]; order[j] = order[j - 1]; }
            dist[j] = d; order[j] = i;
        }
        for (int i = 0; i < n && i < (a1[0] ? atoi(a1) : 12); i++){
            const MpSpot *sp = &g_spot.s[order[i]];
            char nm[40]; race_name(sp->race, nm, sizeof nm);
            rlog("[roam]   %s (stage %u, type %u, track %u%s, %u laps): %s%s%s, %.0f m away\n", nm, MR8(sp->race + 0x37u), MR8(sp->race + 0xfu),
                 MR16(sp->race + 0x18u), MR8(sp->race + 0x1au) & 1u ? " reverse" : "", MR8(sp->race + 0x1bu),
                 sp->kind == MPS_OPEN ? "solo or multiplayer" : "multiplayer only", sp->why ? ": " : "", mpw_text[sp->why], sqrt((double)dist[i]));
        }
        return 1;
    }
#endif
    if (!strcasecmp(sub, "car")){
        /* the player's car: where, how fast, rpm and gear (a race's progress in a test) */
        uint32_t me = player_car();
        if (!ok(me, 0x450)){ rlog("[roam] f=%ld mp car: none\n", f); return 1; }
        rc_state es; memset(&es, 0, sizeof es); eng_state(me, &es);
        rlog("[roam] f=%ld mp car: at %.1f %.1f, speed %.1f m/s, rpm %.0f, gear %d; its engine sounds at %u (pulling %u, letting off %u, "
             "under them %u of 255)\n", f, (double)MRF32(me + 0x60u), (double)MRF32(me + 0x64u), (double)MRF32(me + 0x42cu),
             (double)MRF32(me + 0x440u), (int)(int8_t)MR32(me + 0x434u), es.eng, es.eng_acc, es.eng_dcl, es.eng_base);
        return 1;
    }
#if MPRACES
    if (!strcasecmp(sub, "perf")){
        /* "mp perf [survey | scale F | limit MPH]": the player's car's performance as the game rates it (mprace.c);
         * survey: as every player car type, stock and with every performance part, then the car as it was; scale: its
         * engine's torque and nitrous times F; limit: its governor at MPH (0 none) (test aids) */
        if (!strcasecmp(a1, "survey")) mprace_perf_survey(c, player_car());
        else if (!strcasecmp(a1, "scale")){ mprace_torque_scale(player_car(), (float)atof(a2)); mprace_perf_log("mp perf scale", player_car()); }
        else if (!strcasecmp(a1, "limit")){ mprace_speed_limit(player_car(), (float)atof(a2)); mprace_perf_log("mp perf limit", player_car()); }
        else mprace_perf_log("mp perf", player_car());
        return 1;
    }
#endif
    if (!strcasecmp(sub, "map")){
        /* the open world map's car markers, its race lobbies' icons and labels
         * (and with "strings" every string object of its package) */
        uint32_t sa = g_gmem + TAG_SCR + 0x48u, pk = 0;
        if (g_gmem){ gstr(sa, WM_PACKAGE, 24); pk = gcall1(c, FN_FE_PACKAGE, 0, sa); }
        uint32_t scr = ok(pk, 0x54) ? wm_screen(pk) : 0, map = scr ? rd32(scr + WM_MAP) : 0;
        if (!ok(map, 0x2c)){ rlog("[roam] f=%ld mp map: the world map is not open\n", f); return 1; }
        int n = 0, cars = 0, drawn = 0, tinted = 0, grey = 0;
        for (uint32_t e = rd32(map + 4u); e && e != map + 4u && n < 4096 && ok(e, WM_MARK_SZ); e = rd32(e), n++){
            if (MR32(e + 8u) != WM_CAR) continue;
            cars++;
            uint32_t o = MR32(e + 0xcu);
            int32_t col[4] = {0};
            drawn += ok(o, 0x5c) && MR32(o + 0x58u);
            tinted += colour_crew(o) != 0;
            grey += colour_ai(o);
            fe_colour_get(o, col);
            if (cars <= 64)
                rlog("[roam]   car marker at %.1f %.1f, object 0x%08x colour %02x%02x%02x%s%s\n", (double)MRF32(e + 0x10u), (double)MRF32(e + 0x14u), o,
                        col[2] & 0xff, col[1] & 0xff, col[0] & 0xff,
                        ok(o, 0x5c) && (MR32(o + 0x1cu) & 1u) ? ", hidden" : "", ok(o, 0x5c) && MR32(o + 0x58u) ? ", drawn" : "");
        }
        rlog("[roam] f=%ld mp map: %d markers, %d of them cars', %d of those drawn, %d in a crew's colour, %d grey (AI racers'); %d other players' "
                "(from the hub), moved %d times (%.0f us each)\n", f, n, cars, drawn, tinted, grey, g_wm.others, g_wm.steps,
                g_wm.steps ? (double)g_wm.step_us / g_wm.steps : 0.0);
        /* the level sorter's most nodes in one package so far (0x50B430): RH_FE_NODES when objects went undrawn */
        rlog("[roam] f=%ld mp map: the package has %d objects, room for %d more; FEng drew %u objects of one package at most (%d nodes)\n",
                f, fe_objects(pk + 0x4cu, 0), wm_room(pk), rd32(0x00838490u), RH_FE_NODES);
        if (!strcasecmp(a1, "strings")) fe_strings(pk + 0x4cu, 0);
        if (!strcasecmp(a1, "parts")) fe_parts(pk + 0x4cu, 0, -60.f);
        /* the legend's rows (ours with their ticks) and the markers they hide */
        if (g_wmf.map == map){
            int rows = 0, tracked[2] = { 0, 0 }, away = 0;
            for (uint32_t r = rd32(scr + WMF_SCR_ROWS); r && r != scr + WMF_SCR_ROWS && rows < 64 && ok(r, WMF_ROW_SZ); r = rd32(r)) rows++;
            for (int i = 0; i < g_wmf.n; i++){ tracked[g_wmf.what[i]]++; away += MRF32(g_wmf.m[i] + 0x10u) == WMF_AWAY; }
            rlog("[roam] f=%ld mp map: legend %d rows, the picked one kind %d; Players %s, Players' races %s; %d players' markers, %d lobbies', %d of them off the map\n",
                 f, rows, ok(rd32(scr + 0x5cu), WMF_ROW_SZ) ? (int)MR32(rd32(scr + 0x5cu) + 0x20u) : -1,
                 g_wmf.row[0] ? (MR8(g_wmf.row[0] + 0x24u) ? "crossed" : "ticked") : "-", g_wmf.row[1] ? (MR8(g_wmf.row[1] + 0x24u) ? "crossed" : "ticked") : "-",
                 tracked[0], tracked[1], away);
        }
#if MPRACES
        for (int i = 0; i < g_wml.n && g_wml.map == map; i++){
            uint32_t m = g_wml.m[i], o = ok(m, WM_ICON_SZ) ? rd32(m + 0xcu) : 0, l = g_wml.lbl[i];
            rlog("[roam]   lobby icon %d: object 0x%08x at %.1f %.1f (flags %08x), label 0x%08x (flags %08x, drawn %d)\n", i, o,
                 (double)MRF32(m + 0x1cu), (double)MRF32(m + 0x20u), ok(o, 0x20) ? MR32(o + 0x1cu) : 0u, l, ok(l, 0x5c) ? MR32(l + 0x1cu) : 0u,
                 ok(l, 0x5c) && MR32(l + 0x58u));
        }
#endif
        return 1;
    }
#if MPRACES
    if (!strcasecmp(sub, "lobby")){
        /* the race lobby we host or are in, and the others heard of */
        char nm[40]; race_name(g_mp.l.race, nm, sizeof nm);
        rlog("[roam] f=%ld mp lobby: %s, lobby %08x of %s, %s, %d of %d players, state %u, away %u; trip phase %d; %d other lobbies heard of; "
             "opened %u, joined %u, trips %u\n", f, mp_st[g_mp.st], g_mp.l.id, g_mp.l.host ? mp_name(g_mp.l.host) : "-", nm[0] ? nm : "-",
             g_mp.l.n, g_mp.l.max, g_mp.l.state, g_mp.l.away_s, mprace_phase(), g_mp.nseen, g_mp.opened, g_mp.joined, g_mp.trips);
        for (int i = 0; i < g_mp.nseen; i++){
            const MpLobby *l = &g_mp.seen[i];
            race_name(l->race, nm, sizeof nm);
            rlog("[roam]   %s's lobby %08x: %s (%s) at %.0f %.0f, %d of %d, state %u, %.0f m away, heard %.1f s ago\n", mp_name(l->host), l->id, nm,
                 race_kind(l->race), (double)l->x, (double)l->y, l->n, l->max, l->state, (double)mp_dist(l->x, l->y),
                 (double)(roam_plat_now_us() - l->heard) / 1e6);
        }
        char tx[TAG_DUEL_MAX];
        if (mp_line(roam_plat_now_us(), tx, sizeof tx)) rlog("[roam]   the line at the top: %s\n", tx);   /* (when the HUD is up) */
        return 1;
    }
    if (!strcasecmp(sub, "pull")){
        /* "mp pull": hosting, our advert lists every linked peer that takes lobbies, as though each had asked to join:
         * what a player's game does with an advert that lists it unasked (it leaves: mp_recv) */
        int n0 = g_mp.l.n;
        if (g_mp.st == MP_HOST){
            roam_plat_lock();
            for (int i = 0; i < RC_TABLE && g_mp.l.n < g_mp.l.max; i++){
                const rc_peer *p = &g_s.peer[i];
                if (p->state == RC_PS_EST && p->lobbies && !mp_in(&g_mp.l, p->id)){ g_mp.away[g_mp.l.n] = 0; g_mp.l.ids[g_mp.l.n++] = p->id; }
            }
            roam_plat_unlock();
            mp_send_all(RC_LOBBY_ADVERT, 0); g_mp.tx_at = roam_plat_now_us();
        }
        rlog("[roam] f=%ld mp pull: %s, %d players listed (were %d)\n", f, mp_st[g_mp.st], g_mp.l.n, n0);
        return 1;
    }
    if (!strcasecmp(sub, "ui")){
        /* the pop-ups: the dialog up, the FE manager's screens and their classes */
        rlog("[roam] f=%ld mp ui: dialog %08x, ours %d (race %08x, answer %08x), asked %u, solo %u, multiplayer %u; labels %s\n", f, rd32(G_DLG_ID),
             g_mpd.dlg, g_mpd.race, g_mpd.msg, g_mpd.asked, g_mpd.solo, g_mpd.multi, mp_lang_hash[0] && language_text(mp_lang_hash[0]) ? "in" : "not in");
        uint32_t n = rd32(G_FEMGR_LIST);
        for (int k = 0; n && n != G_FEMGR_LIST && k < 32; k++, n = rd32(n)){
            uint32_t sc = rd32(n - 4u + 0x14u);
            rlog("[roam]   screen %d: node %08x screen %08x class %08x +0x50 %08x +0x54 %08x\n", k, n, sc, ok(sc, 4) ? MR32(sc) : 0,
                 ok(sc, 0x58) ? MR32(sc + 0x50u) : 0, ok(sc, 0x58) ? MR32(sc + 0x54u) : 0);
        }
        return 1;
    }
    if (!strcasecmp(sub, "waittrip")){
        /* "mp waittrip [tmo]": a trip went and we are back in free roam, the lobby idle */
        long tmo = a1[0] ? atol(a1) : 0;
        if (*wait_start < 0) *wait_start = f;
        if (g_mp.trips && g_mp.st == MP_IDLE){ rlog("[roam] f=%ld mp waittrip satisfied after %ld frames: %s\n", f, f - *wait_start, g_mp.note);
                                              *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp waittrip TIMEOUT (%s, trip phase %d)\n", f, mp_st[g_mp.st], mprace_phase()); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(sub, "waitlobby")){
        /* "mp waitlobby N [tmo]": we host a lobby, or are in one, of N players at least; N 0: we heard of another's;
         * in none (idle): we heard of one of N players */
        int want = atoi(a1); long tmo = a2[0] ? atol(a2) : 0;
        if (*wait_start < 0) *wait_start = f;
        int have = want ? ((g_mp.st == MP_HOST || g_mp.st == MP_IN || g_mp.st == MP_COUNT) ? g_mp.l.n : 0) : g_mp.nseen;
        if (want && g_mp.st == MP_IDLE) for (int i = 0; i < g_mp.nseen; i++) if (g_mp.seen[i].n > have) have = g_mp.seen[i].n;
        if (have >= (want ? want : 1)){ rlog("[roam] f=%ld mp waitlobby %d satisfied after %ld frames (%s)\n", f, want, f - *wait_start, mp_st[g_mp.st]);
                                        *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp waitlobby %d TIMEOUT (%s, %d)\n", f, want, mp_st[g_mp.st], have); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(sub, "waitaway") || !strcasecmp(sub, "waitidle")){
        /* "mp waitaway [tmo]": the lobby we host or are in counts down for its host's being away (mp_idle);
         * "mp waitidle [tmo]": we are in no lobby */
        int away = !strcasecmp(sub, "waitaway"); long tmo = a1[0] ? atol(a1) : 0;
        if (*wait_start < 0) *wait_start = f;
        if (away ? (g_mp.st == MP_HOST || g_mp.st == MP_IN) && g_mp.l.away_s : g_mp.st == MP_IDLE){
            rlog("[roam] f=%ld mp %s satisfied after %ld frames (%s, away %u): %s\n", f, sub, f - *wait_start, mp_st[g_mp.st], g_mp.l.away_s, g_mp.note);
            *wait_start = -1; return 1;
        }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp %s TIMEOUT (%s)\n", f, sub, mp_st[g_mp.st]); *wait_start = -1; return 1; }
        return 0;
    }
#endif
#if SAYS
    if (!strcasecmp(sub, "say")){
        /* "mp say N": quick chat, the player says phrase N, as if picked on the page */
        g_say.test_pick = atoi(a1);
        rlog("[roam] f=%ld mp say %d (quick chat %s)\n", f, g_say.test_pick, g_s.conf.say ? "on" : "off");
        return 1;
    }
    if (!strcasecmp(sub, "waitsaid")){
        /* "mp waitsaid N [tmo]": N friends' phrases shown (quick chat) */
        int want = atoi(a1); long tmo = a2[0] ? atol(a2) : 0;
        if (*wait_start < 0) *wait_start = f;
        if ((int)g_say.heard >= want){ rlog("[roam] f=%ld mp waitsaid %d satisfied after %ld frames (%u not shown)\n", f, want, f - *wait_start,
                                            g_say.dropped); *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp waitsaid %d TIMEOUT (have %u, %u not shown)\n", f, want, g_say.heard, g_say.dropped);
                                            *wait_start = -1; return 1; }
        return 0;
    }
#endif
    if (!strcasecmp(sub, "waitfar")){
        /* "mp waitfar NAME M [T]": until the friend of that name is shown at least M m away (T frames at most) */
        char a3[32] = {0}; sscanf(args + used, "%*s %*s %31s", a3);
        float m = (float)atof(a2); long tmo = a3[0] ? atol(a3) : 0, d = -1;
        if (*wait_start < 0) *wait_start = f;
        for (int i = 0; i < g_npv; i++) if (!strcasecmp(g_pv[i].name, a1) && g_pv[i].visible && g_pv[i].have_pose) d = (long)g_pv[i].dist;
        if (d >= 0 && (float)d >= m){ rlog("[roam] f=%ld mp waitfar %s %.0f satisfied after %ld frames (%ld m)\n", f, a1, (double)m, f - *wait_start, d); *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp waitfar %s %.0f TIMEOUT (%ld m)\n", f, a1, (double)m, d); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(sub, "waitpeers") || !strcasecmp(sub, "waitcars") || !strcasecmp(sub, "waitsolid") || !strcasecmp(sub, "waithidden")){
        /* friends established / friends' cars shown / of those, solid (no longer ghosts) / friends' cars hidden or parked */
        int want = atoi(a1); long tmo = a2[0] ? atol(a2) : 0;
        if (*wait_start < 0) *wait_start = f;
        int have = 0;
        if (!strcasecmp(sub, "waitpeers")){ roam_plat_lock(); have = rc_established(&g_s); roam_plat_unlock(); }
        else if (!strcasecmp(sub, "waithidden")){ for (int k = 0; k < NSLOT; k++) have += g_sl[k].st == SL_PARKED || (g_sl[k].st == SL_ACTIVE && g_sl[k].hidden); }
        else for (int k = 0; k < NSLOT; k++)
            if (g_sl[k].st == SL_ACTIVE && !g_sl[k].hidden && (strcasecmp(sub, "waitsolid") || (O.collide && !g_sl[k].ghost))) have++;
        if (have >= want){ rlog("[roam] f=%ld mp %s %d satisfied after %ld frames\n", f, sub, want, f - *wait_start); *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp %s %d TIMEOUT (have %d)\n", f, sub, want, have); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(sub, "follow")){
        /* "mp follow K [back] [left] [turn]": the player jumps behind slot K's
         * car (hot-position jump), for frame dumps; left 0 puts it right
         * behind (the collision tests drive into the friend's car from
         * there); turn (degrees, left positive) turns it from the friend's
         * heading: -90 from its left looks at its side.  K "tuner": the
         * nearest of the game's outrun challengers' cars out */
        char a3[32] = {0}, a4[32] = {0}; sscanf(args + used, "%*s %*s %31s %31s", a3, a4);
        int k = atoi(a1); float back = a2[0] ? (float)atof(a2) : 12.f;
        uint32_t car = 0; const char *who = "the tuner";
        if (!strcasecmp(a1, "tuner")){
            uint32_t em = encounter_mgr(); float nd = 1e9f;
            for (uint32_t j = 0; em && player_car() && j < 3; j++){
                uint32_t tc = rd32(em + EM_TABLE + 0xcu + 0x10u * j);
                if (ok(tc, 0x9e0) && car_out(tc) && du_dist(tc) < nd){ nd = du_dist(tc); car = tc; }
            }
            if (!car){ rlog("[roam] f=%ld mp follow tuner: none out\n", f); return 1; }
        } else {
            if (a1[0] < '0' || a1[0] > '9'){ k = -1; for (int j = 0; j < NSLOT; j++) if (g_sl[j].st != SL_FREE && !strcasecmp(g_sl[j].name, a1)) k = j; }
            if (k < 0 || k >= NSLOT || g_sl[k].st != SL_ACTIVE || g_sl[k].hidden){ rlog("[roam] f=%ld mp follow %d: no car shown there\n", f, k); return 1; }
            car = g_sl[k].car; who = g_sl[k].name;
        }
        /* by default behind it and 3.5 m to the left, so that the player's
         * own car (the chase camera looks over it) does not hide the friend's */
        jump_behind(car, back, a3[0] ? (float)atof(a3) : 3.5f, (float)atof(a4));
        rlog("[roam] f=%ld mp follow: behind %s's car at %.1f %.1f %.1f\n", f, who,
                (double)MRF32(car + 0x60u), (double)MRF32(car + 0x64u), (double)MRF32(car + 0x68u));
        return 1;
    }
    if (!strcasecmp(sub, "keepnear")){
        /* "mp keepnear NAME [tmo]": wait to be challenged to an outrun
         * ("got"), jumping 12 m behind NAME's car whenever it is more than
         * 22 m away (at most every 90 frames): NAME's script can move about */
        static long jumped;
        long tmo = a2[0] ? atol(a2) : 0;
        if (*wait_start < 0){ *wait_start = f; jumped = f - 90; }
        if (g_du.st == DU_GOT){ rlog("[roam] f=%ld mp keepnear %s: challenged after %ld frames\n", f, a1, f - *wait_start); *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp keepnear %s TIMEOUT\n", f, a1); *wait_start = -1; return 1; }
        int k = -1; for (int j = 0; j < NSLOT; j++) if (g_sl[j].st == SL_ACTIVE && !g_sl[j].hidden && !strcasecmp(g_sl[j].name, a1)) k = j;
        if (k >= 0 && player_car() && f - jumped >= 90 && du_dist(g_sl[k].car) > 22.f){
            jump_behind(g_sl[k].car, 12.f, 0.f, 0.f); jumped = f;
            rlog("[roam] f=%ld mp keepnear: behind %s's car\n", f, a1);
        }
        return 0;
    }
    if (!strcasecmp(sub, "net")){
        /* "mp net off|on": stop the network (as quitting would: BYE), or
         * restart it with a new id (a rejoin) */
        if (!strcasecmp(a1, "off") && g_net_ok){
            roam_plat_stop(1); g_net_ok = 0;
            rc_config cf = g_s.conf; rc_io io = { 0, 0, 0, 0 };
            roam_plat_lock(); rc_session_init(&g_s, &cf, &io); roam_plat_unlock();   /* no friends any more */
            rlog("[roam] f=%ld mp net off\n", f); }
        else if (!strcasecmp(a1, "on") && !g_net_ok){
            rc_config cf = g_s.conf; cf.self_id = roam_plat_random();
            rc_io io = { 0, 0, 0, 0 };
            rc_session_init(&g_s, &cf, &io);
            roam_plat_cfg pc = { cf.port, O.test, cf.discovery, rh_opt("NFSU2_ROAM_IMPAIR") };
            if (roam_plat_start(&pc, &g_s) == 0){
                g_net_ok = 1;
                uint32_t pip[RC_MAX_PEERS * 2]; uint16_t ppt[RC_MAX_PEERS * 2];
                int np = roam_plat_parse_peers(opt("NFSU2_ROAM_PEERS"), O.test, cf.port, pip, ppt, RC_MAX_PEERS * 2);
                roam_plat_lock(); for (int i = 0; i < np; i++) rc_add_peer(&g_s, pip[i], ppt[i], 1, roam_plat_now_us()); roam_plat_unlock();
                g_cfg_next = 0;
                rlog("[roam] f=%ld mp net on: id %08x\n", f, cf.self_id);
            }
        }
        return 1;
    }
    if (!strcasecmp(sub, "outrun") || !strcasecmp(sub, "waitoutrun")){
        /* "mp outrun": the outrun with a friend, and the game's challengers;
         * "mp waitoutrun STATE [tmo]": wait for prompt (the challenge is
         * offered), idle, sent, got, accepted, race or over, or tuner (a
         * challenger's car out) */
        static const char *const st[] = { "idle", "sent", "got", "accepted", "race", "over" };
        uint32_t em = encounter_mgr(), pl = rd32(G_PLAYER1), opp = pl ? rd32(pl + PL_OPPONENT) : 0;
        int nout = 0, offered = 0; float nd = 1e9f;
        for (uint32_t k = 0; em && player_car() && k < 3; k++){
            uint32_t car = rd32(em + EM_TABLE + 0xcu + 0x10u * k);
            if (!ok(car, 0x9e0) || !car_out(car)) continue;
            nout++; if (car == opp) offered = 1;
            float d = du_dist(car); if (d < nd) nd = d;
        }
        if (!strcasecmp(sub, "outrun")){
            rlog("[roam] f=%ld mp outrun: %s%s%s%s, %.0f m apart, getting away %.1f m/s faster; %u challenge(s), %u raced, %u won, %u lost\n", f, st[g_du.st],
                    g_du.id ? " with " : "", g_du.id ? g_du.name : "", g_du.icon ? ", challenge offered" : "", (double)g_du.gap, (double)g_du.away,
                    g_du.offers, g_du.races, g_du.wins, g_du.losses);
            static const char *const emm[] = { "the game's", "held", "standing down", "paused" };
            char near[32] = "";
            if (nout) snprintf(near, sizeof near, ", nearest %.0f m", (double)nd);
            rlog("[roam] f=%ld mp outrun: HUD outrun icon %s; challengers %s, state %d, %d out%s%s%s\n", f, du_icon_up() ? "UP" : "off",
                    emm[g_em], em ? (int)MR32(em) : -1, nout, near, offered ? ", its outrun offered" : "",
                    em_near() ? ", in reach: no outrun between players" : "");
            return 1;
        }
        long tmo = a2[0] ? atol(a2) : 0;
        if (*wait_start < 0) *wait_start = f;
        int have = !strcasecmp(a1, "prompt") ? g_du.st == DU_IDLE && g_du.icon : !strcasecmp(a1, "tuner") ? nout > 0 : !strcasecmp(a1, st[g_du.st]);
        if (have){ rlog("[roam] f=%ld mp waitoutrun %s satisfied after %ld frames\n", f, a1, f - *wait_start); *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){ rlog("[roam] f=%ld mp waitoutrun %s TIMEOUT (%s)\n", f, a1, st[g_du.st]); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(sub, "rims")){
        /* "mp rims": each shown friend's car's rims, the shops' or its own,
         * and whether its render info has their models (part_packs) */
        int n = 0, have = 0;
        for (int k = 0; k < NSLOT; k++){
            Slot *s = &g_sl[k];
            if (s->st != SL_ACTIVE || s->hidden || !ok(s->car, 0x9e0)) continue;
            uint32_t ride = rd32(s->car + 0x10u), ri = rd32(s->car + 0x554u), rim = ok(ride, 0x8f0) ? rd32(ride + 0x604u) : 0;
            int shop = ok(rim, 0xe) && rd32(rd32(G_PARTOWNERS) + 4u * MR8(rim + 7u)) == OWNER_WHEELS;
            int fr = ok(ri, CRI_RIMS + 0x24u) && MR32(ri + CRI_RIMS), re = ok(ri, CRI_RIMS + 0x24u) && MR32(ri + CRI_RIMS + 0x20u);
            n += shop; have += shop && fr && re;
            rlog("[roam] f=%ld mp rims: slot %d %s (paint %u): %s rims, models front %s rear %s\n", f, k, s->name, slot_skin(k),
                    shop ? "the shops'" : "its own", fr ? "yes" : "NO", re ? "yes" : "NO");
        }
        rlog("[roam] f=%ld mp rims: %d of %d friends' cars with the shops' rims have their models\n", f, have, n);
        return 1;
    }
    if (!strcasecmp(sub, "models")){
        /* "mp models": each shown friend's car's parts from the shared packs
         * (part_packs) that have a model, and whether its render info has
         * their first (CRI_MODELS; the trunk's audio, 0x23-0x2E, is not
         * there), by the ride slot's name (G_SLOTNAMES) */
        int n = 0, have = 0;
        for (int k = 0; k < NSLOT; k++){
            Slot *s = &g_sl[k];
            if (s->st != SL_ACTIVE || s->hidden || !ok(s->car, 0x9e0)) continue;
            uint32_t ride = rd32(s->car + 0x10u), ri = rd32(s->car + 0x554u), owners = rd32(G_PARTOWNERS);
            if (!ok(ride, 0x8f0) || !ok(ri, CRI_MODELS + 0x20u * 0x3fu) || !owners) continue;
            char list[400] = {0}; int cn = 0, ch = 0, at = 0;
            for (uint32_t sl = 0; sl < 0x3fu; sl++){
                uint32_t p = rd32(ride + 0x590u + 4u * sl);
                if ((sl >= 0x23u && sl <= 0x2eu) || !ok(p, 0xe) || MR16(p + 0xcu) == 0xffffu) continue;   /* (+0xC: its model's name, 0x634800) */
                if (!shared_owner(rd32(owners + 4u * MR8(p + 7u)))) continue;
                int m = MR32(ri + CRI_MODELS + 0x20u * sl) != 0;
                char nm[24]; cstr(rd32(G_SLOTNAMES + 8u * sl), nm, sizeof nm);
                cn++; ch += m;
                if (at < (int)sizeof list - 40) at += snprintf(list + at, sizeof list - (size_t)at, " %s %s", nm, m ? "yes" : "NO");
            }
            n += cn; have += ch;
            rlog("[roam] f=%ld mp models: slot %d %s: %d of %d shop parts have their models:%s\n", f, k, s->name, ch, cn, list);
        }
        rlog("[roam] f=%ld mp models: %d of %d shop parts of friends' cars have their models\n", f, have, n);
        return 1;
    }
    if (!strcasecmp(sub, "solids")){
        /* "mp solids [K]": the player's car's (or slot K's) parts from the
         * shared packs: each one's model in the render info, that model's
         * solid (eModel +0xC: 0x5C5930 draws nothing without one), and each
         * of the streamer's packs that lists the model (G_STREAMER +0x18, in
         * the order 0x48CED0 searches them to load or unload it): its loads
         * (+0x12) and whether its data is in (+0x14) */
        int k = a1[0] ? atoi(a1) : -1;
        uint32_t car = k < 0 ? player_car() : k < NSLOT ? g_sl[k].car : 0, owners = rd32(G_PARTOWNERS);
        uint32_t ride = ok(car, 0x9e0) ? rd32(car + 0x10u) : 0, ri = ok(car, 0x9e0) ? rd32(car + 0x554u) : 0;
        if (!ok(ride, 0x8f0) || !ok(ri, CRI_MODELS + 0x20u * 0x3fu) || !owners){ rlog("[roam] f=%ld mp solids: no car\n", f); return 1; }
        int n = 0, drawn = 0;
        for (uint32_t sl = 0; sl < 0x3fu; sl++){
            uint32_t p = rd32(ride + 0x590u + 4u * sl);
            if ((sl >= 0x23u && sl <= 0x2eu) || !ok(p, 0xe) || MR16(p + 0xcu) == 0xffffu) continue;
            if (!shared_owner(rd32(owners + 4u * MR8(p + 7u)))) continue;
            uint32_t m = rd32(ri + CRI_MODELS + 0x20u * sl), name = ok(m, 0x10) ? MR32(m + 8u) : 0, solid = ok(m, 0x10) ? MR32(m + 0xcu) : 0;
            char nm[24]; cstr(rd32(G_SLOTNAMES + 8u * sl), nm, sizeof nm);
            char packs[400] = {0}; int at = 0, np = 0;
            uint32_t head = G_STREAMER + 0x18u;
            for (uint32_t pk = rd32(head); name && pk && pk != head && ok(pk, 0xc0) && np < 256; pk = rd32(pk), np++){
                uint32_t tab = MR32(pk + 0xa0u), cnt = MR32(pk + 0xa4u);
                if (!tab || cnt > 100000u || !ok(tab, cnt * 0x18u)) continue;
                for (uint32_t i = 0; i < cnt; i++){
                    uint32_t e = tab + 0x18u * i;
                    if (MR32(e) != name) continue;
                    char pn[48]; cstr(pk + 8u, pn, sizeof pn);
                    if (at < (int)sizeof packs - 80)
                        at += snprintf(packs + at, sizeof packs - (size_t)at, " [%s: loads %u, %s]", pn, MR16(e + 0x12u), MR32(e + 0x14u) ? "in" : "out");
                    break;
                }
            }
            n++; drawn += solid != 0;
            rlog("[roam] f=%ld mp solids: %s model %08x %s, solid %s;%s\n", f, nm, name, m ? "yes" : "NO", solid ? "yes" : "NO", packs);
        }
        rlog("[roam] f=%ld mp solids: %d of %d shop parts of %s drawn\n", f, drawn, n, k < 0 ? "the player's car" : "the slot's car");
        return 1;
    }
    if (!strcasecmp(sub, "sounds")){
        /* "mp sounds K": slot K's car's sound state (CAR_SOUND; an AI
         * racer's, or none) and its children's vtables, the nitrous's and
         * the skids' named */
        int k = atoi(a1); uint32_t car = k >= 0 && k < NSLOT ? g_sl[k].car : 0, st = ok(car, 0x9e0) ? rd32(car + CAR_SOUND) : 0;
        char list[300] = {0}; int at = 0, n = 0;
        if (ok(st, 0x100) && rd32(st + 0x30u) == car)
            for (uint32_t ch = rd32(st + 0x20u); ok(ch, 0x100) && n < 64; ch = rd32(ch + 0x50u), n++){
                uint32_t vt = rd32(ch);
                if (at < (int)sizeof list - 24) at += snprintf(list + at, sizeof list - (size_t)at, " %08x%s", vt,
                                                               vt == VT_SFX_NOS ? "(nitrous)" : vt == VT_SFX_SKIDS ? "(skids)" : "");
            }
        rlog("[roam] f=%ld mp sounds: slot %d car 0x%08x, sound state 0x%08x%s, %d children:%s\n", f, k, car, st,
             ok(st, 0x100) && rd32(st + 0x30u) == car ? "" : " (none of its own)", n, list);
        if (k >= 0 && k < NSLOT) rlog("[roam] f=%ld mp sounds: slot %d engine %s + %s (idle %.0f, most %.0f rpm)\n", f, k,
                                      g_sl[k].gin[0][0] ? g_sl[k].gin[0] : "-", g_sl[k].gin[1][0] ? g_sl[k].gin[1] : "-",
                                      (double)g_sl[k].eng_idle, (double)g_sl[k].eng_max);
#ifndef ROAM_HOST_WIN
        rlog("[roam] f=%ld mp sounds: the game's AI engines updated %u times, %u of them on friends' cars (silenced)\n", f,
             g_aieng_calls, g_aieng_muted);
#endif
        return 1;
    }
    if (!strcasecmp(sub, "carpoke") || !strcasecmp(sub, "carpeek")){
        /* "mp carpoke K OFF VALUE [N]": N floats (VALUE may be nan) into slot
         * K's car at +OFF (hex), the way the game's physics could leave them;
         * "mp carpeek K OFF [N]" logs them */
        char a3[32] = {0}, a4[32] = {0}; sscanf(args + used, "%*s %*s %31s %31s", a3, a4);
        int k = atoi(a1); uint32_t off = (uint32_t)strtoul(a2, 0, 16);
        if (k < 0 || k >= NSLOT || !ok(g_sl[k].car, 0x9e0) || off >= 0x9e0u){ rlog("[roam] f=%ld mp %s: no car in slot %d\n", f, sub, k); return 1; }
        int poke = !strcasecmp(sub, "carpoke"), n = atoi(poke ? a4 : a3); if (n < 1) n = 1;
        float v = poke ? (float)atof(a3) : 0.f;
        for (int i = 0; i < n && off + 4u * (uint32_t)i < 0x9e0u; i++){
            uint32_t a = g_sl[k].car + off + 4u * (uint32_t)i;
            if (poke) MRF32(a) = v;
            rlog("[roam] f=%ld mp %s slot %d +0x%x = %g (0x%08x)\n", f, sub, k, off + 4u * (uint32_t)i, (double)MRF32(a), MR32(a));
        }
        return 1;
    }
    if (!strcasecmp(sub, "cfgdump") || !strcasecmp(sub, "cfgtest") || !strcasecmp(sub, "parts") || !strcasecmp(sub, "setpart")){
        if (g_njob == NJOB){ rlog("[roam] f=%ld mp %s: busy\n", f, sub); return 1; }
        snprintf(g_job[g_njob++], sizeof g_job[0], "%s", args);
        return 1;
    }
    rlog("[roam] f=%ld mp: unknown '%s'\n", f, sub);
    return 1;
}
static void run_job(CPU *c, const char *job){
    char sub[32] = {0}, a1[200] = {0}, a2[64] = {0}, a3[32] = {0}; int used = 0;
    sscanf(job, "%31s %n", sub, &used);
    sscanf(job + used, "%199s %63s %31s", a1, a2, a3);
    long f = rh_frame();
    if (!strcasecmp(sub, "cfgdump")){
        /* write a car-look record: the player's car, or a stock car of type N */
        uint8_t b[RC_CFG_MAX]; int n = 0; char tn[RC_CFG_TYPE_NAME + 1] = {0};
        if (a2[0]){
            uint32_t t = (uint32_t)atoi(a2), types = rd32(G_CARTYPES);
            rc_carcfg cc; memset(&cc, 0, sizeof cc);
            if (types && t < 46){ cc.type_hash = MR32(types + t * 0x890u + 0xd0u); cstr(types + t * 0x890u, cc.type_name, sizeof cc.type_name);
                cc.ride_b[3] = 1; n = rc_carcfg_encode(&cc, b, sizeof b); snprintf(tn, sizeof tn, "%s", cc.type_name); }
        } else n = local_cfg(b, sizeof b, tn);
        FILE *o = n > 0 ? fopen(a1, "wb") : 0;
        if (o){ fwrite(b, 1, (size_t)n, o); fclose(o); }
        rlog("[roam] f=%ld mp cfgdump %s: %s, %d bytes\n", f, a1, tn, n);
        return;
    }
    if (!strcasecmp(sub, "cfgtest")){
        /* the player's look -> record -> a RideInfo built from it: every part
         * slot and the derived bytes must come out the same */
        uint8_t b[RC_CFG_MAX]; char tn[RC_CFG_TYPE_NAME + 1];
        int n = local_cfg(b, sizeof b, tn);
        rc_carcfg cc;
        if (n <= 0 || rc_carcfg_decode(b, n, &cc) != RC_OK || !g_gmem){ rlog("[roam] f=%ld mp cfgtest: no record\n", f); return; }
        uint32_t car = player_car(), src = rd32(car + 0x10u), dst = g_sl[NSLOT - 1].ride_new;
        char why[96] = {0};
        int miss = build_ride(c, &cc, dst, 1u, why, sizeof why);      /* the player's skin slot */
        int diff_slots = 0, diff_bytes = 0, first = -1;
        for (uint32_t sl = 0; sl < RC_PART_SLOTS; sl++) if (MR32(src + 0x590u + 4u * sl) != MR32(dst + 0x590u + 4u * sl)) diff_slots++;
        for (uint32_t o = 0; o < 0x8f0u; o++){
            if (o >= 0x10u && o < 0x580u) continue;                  /* physics/tuning block: not visual */
            if (MR8(src + o) != MR8(dst + o)){ diff_bytes++; if (first < 0) first = (int)o; }
        }
        int diff_perf = 0, diff_phys = 0;
        for (uint32_t o = 0x318u; o < 0x3c1u; o++) if ((o < 0x384u || o >= 0x398u) && MR8(src + o) != MR8(dst + o)) diff_perf++;
        for (uint32_t o = 0x10u; o < 0x580u; o++) if (MR8(src + o) != MR8(dst + o)) diff_phys++;
        rlog("[roam] f=%ld mp cfgtest: %s, %d parts in a %d-byte record, %d not found%s%s; rebuilt ride: %d of 170 part slots differ, "
                        "%d other bytes differ (first at +0x%x); performance/tuning bytes differing %d, physics block bytes differing %d\n",
                f, tn, cc.n_parts, n, miss, why[0] ? ": " : "", why, diff_slots, diff_bytes, first, diff_perf, diff_phys);
        return;
    }
    if (!strcasecmp(sub, "parts") || !strcasecmp(sub, "setpart")){
        /* "mp parts SLOT [spinner]": the CarPartDB's parts for ride slot SLOT
         * (hex; its packs on the list at G_CARPARTDB, +0x34 the parts of 14
         * bytes, +0x38 how many; a part's kind, +4, is G_SLOTKIND's for the
         * slot), with "spinner" only the spinners that fit the car (type
         * SPINNER and the car type's size: the spinner shop's list,
         * 0x539960); "here": a car of the player's car type can have it
         * (find_part, as a friend's look is built); "mp setpart SLOT N [spinner]": the
         * N-th of them on the player's ride (RideInfo::SetPart), as a shop
         * would but without loading the car again: its look has it, so
         * friends' games show it */
        uint32_t slot = (uint32_t)strtoul(a1, 0, 16), want = (uint32_t)atoi(a2), n = 0, all = 0, owners = rd32(G_PARTOWNERS);
        uint32_t kind = slot < RC_PART_SLOTS ? rd32(G_SLOTKIND + 4u * slot) : 0xffu, str = S_SPINNER;
        uint32_t spinner = rh_call(c, FN_HASH, 0, 0, 1, &str), ride0 = rd32(player_car() + 0x10u), types = rd32(G_CARTYPES);
        int size = ride0 && types ? (int8_t)MR8(types + MR32(ride0) * 0x890u + 0xdcu) : -1;   /* the car type's rim size */
        int set = !strcasecmp(sub, "setpart"), only = !strcasecmp(set ? a3 : a2, "spinner");
        for (uint32_t node = rd32(G_CARPARTDB), i = 0; node && node != G_CARPARTDB && i < 64; node = rd32(node), i++){
            uint32_t base = rd32(node + 0x34u), cnt = rd32(node + 0x38u);
            for (uint32_t p = base; p && cnt < 0x10000u && p < base + cnt * 0xeu && ok(p, 0xe); p += 0xeu){
                all++;
                if (MR8(p + 4u) != kind) continue;
                uint32_t key = (uint32_t)MR16(p + 2u) << 16 | MR16(p), at[2] = { ATTR_WHEELTYPE, 0 }, as[2] = { ATTR_RIMSIZE, 0 };
                int spin = rh_call(c, FN_PART_ATTR, p, 0, 2, at) == spinner;
                int fits = (int8_t)(rh_call(c, FN_PART_ATTR, p, 0, 2, as) & 0xffu) == size;
                if (only && !(spin && fits)) continue;
                char nm[48] = {0}; uint32_t np = rh_call(c, FN_PART_NAME, p, 0, 0, 0);
                if (np) cstr(np, nm, sizeof nm);
                uint32_t own = owners ? rd32(owners + 4u * MR8(p + 7u)) : 0u;
                int here = !set && ride0 && find_part(c, MR32(ride0), slot, key, own) == p;
                if (!set) rlog("[roam] f=%ld mp parts %x: #%u 0x%08x %s key %08x owner %08x level %u%s%s%s\n", f, slot, n, p, nm, key,
                               own, (uint32_t)MR8(p + 5u) >> 5, spin ? " spinner" : "", fits ? " fits" : "", here ? " here" : "");
                else if (n == want){
                    uint32_t ride = rd32(player_car() + 0x10u), a[2] = { slot, p };
                    uint32_t old = ok(ride, 0x8f0) ? rh_call(c, FN_RIDE_SETPART, ride, 0, 2, a) : 0u;
                    g_cfg_next = 0;                                        /* the look goes out now */
                    rlog("[roam] f=%ld mp setpart %x: part 0x%08x (key %08x) on ride 0x%08x, was 0x%08x\n", f, slot, p, key, ride, old);
                    return;
                }
                n++;
            }
        }
        rlog("[roam] f=%ld mp %s %x: %u part(s) of %u%s\n", f, sub, slot, n, all, set ? ", none set" : "");
        return;
    }
}
#endif /* ROAM_HOST_SCRIPT */

#ifndef ROAM_HOST_WIN
/* roam.c, Car::SetPhysicsMode (0x5F46A0): refuse the null brain's wake-up
 * (0x404890, returning to 0x4048B6 for mode 3 or 0x4048C0 for mode 1) of a
 * friend's car.  The game wakes a car in mode 0 into its AI physics on every
 * frame another car is near it (within some 10 m in the tests, touching or
 * not); for a friend's car that physics then ran on
 * a ride built without its physics pass (build_ride), drove the car for the
 * rest of the timestep and left its wheel and suspension state in the car
 * (+0x340..+0x38C; wheels_pose puts the suspension offsets back).  Kept in
 * mode 0 the car is still solid: a car that hits it is pushed in its owner's
 * game as before.  Other callers (Car::Activate, SetRideInfo) are not
 * refused: make_inert puts their mode back to 0, as it always did; the first
 * one to do it to a friend's car that is shown is logged (what moves the
 * wheels, wheels_pose).  1: refused. */
int roam_game_inert(uint32_t car, uint32_t mode, uint32_t from){
    if (!mode || !car) return 0;
    int wake = from == 0x004048b6u || from == 0x004048c0u;
    for (int k = 0; k < NSLOT; k++) if (g_sl[k].car == car && (g_sl[k].st == SL_ACTIVE || g_sl[k].st == SL_LOADING)){
        if (!wake){
            if (g_sl[k].st == SL_ACTIVE && !g_sl[k].hidden && !g_sl[k].moded++)
                rlog("[roam] f=%ld slot %d: %s's car put into physics mode %u from 0x%08x\n", rh_frame(), k, g_sl[k].name, mode, from);
            return 0;
        }
        if (!g_sl[k].wakes++) rlog("[roam] f=%ld slot %d: %s's car stays without physics when a car comes near\n", rh_frame(), k, g_sl[k].name);
        return 1;
    }
    return 0;
}
/* The player's car's motion that a pair changes, in its body's rigid body
 * ([body +0x68]): the momentum (+0x70, the velocity times the mass in
 * tonnes: 1.365 for the prologue's 350Z) and the angular momentum (+0x80),
 * from which each step works the velocity and the spin out again, and
 * those (+0xA0, +0xC0: 0x593B70, 0x593BD0), which the car has too (+0x70,
 * +0x110).  Measured by what the pair changed; a velocity set without the
 * momentum was gone the next step (the car went on at 0.1 m/s).  The pair
 * sets the car's speeds too (+0x42C, +0x430: the speedometer's and the
 * engine sound's), which the next step works out again: left alone they
 * showed the game's whole hit for a frame (32 -> -3.5 -> 14 m/s). */
enum { CS_V = 6, CS_W = 9, CS_S = 12, CS_N = 14 };   /* st[]: momentum, angular momentum, velocity, spin, the 2 speeds */
static const uint32_t g_cs_off[4] = { 0x70u, 0x80u, 0xa0u, 0xc0u };
static void contact_get(uint32_t car, uint32_t rb, float *st){
    for (int j = 0; j < 4; j++) rd3(rb + g_cs_off[j], st + 3 * j);
    st[CS_S] = MRF32(car + 0x42cu); st[CS_S + 1] = MRF32(car + 0x430u);
}
static void contact_set(uint32_t car, uint32_t rb, const float *st){
    for (int j = 0; j < 4; j++) for (int i = 0; i < 3; i++) MRF32(rb + g_cs_off[j] + 4u * (uint32_t)i) = st[3 * j + i];
    for (int i = 0; i < 3; i++){ MRF32(car + 0x70u + 4u * (uint32_t)i) = st[CS_V + i]; MRF32(car + 0x110u + 4u * (uint32_t)i) = st[CS_W + i]; }
    MRF32(car + 0x42cu) = st[CS_S]; MRF32(car + 0x430u) = st[CS_S + 1];
}
/* A car's weight for sharing a hit (tonnes): its model's (CarTypeInfo,
 * car +0xC, at +0x220: the physics block the ride's physics pass 0x5AC4F0
 * copies from +0x110, the mass first in its rigid body's part at +0x110,
 * 0x59B940), from 0.96 t (Peugeot 106) to 2.9 t (Hummer).  Not the mass
 * the car's physics has: the weight upgrade takes up to 20 % off that
 * (0x5A6B50), a friend's car has none (its ride skips the physics pass),
 * and both games must weigh both cars alike for their shares to add up to
 * the whole hit.  NFSU2_ROAM_TEST_MASS / _FMASS (kg, test aid): the
 * player's car / friends' cars weigh that instead. */
static float car_mass(uint32_t car, int mine){
    int kg = mine ? O.test_mass : O.test_fmass;
    if (kg > 0) return (float)kg / 1000.f;
    uint32_t types = rd32(G_CARTYPES), ti = ok(car, 0x10) ? rd32(car + 0xcu) : 0;
    float m = types && ti >= types && (ti - types) % 0x890u == 0 && (ti - types) / 0x890u < 64u && ok(ti, 0x224) ? MRF32(ti + 0x220u) : 0.f;
    return m > 0.3f && m < 20.f ? m : 1.4f;
}
/* a push on a friend's car, held until their game reports it (push_pose) */
static void push_add(Slot *s, const float *v, int64_t t){
    if (s->pqn == PUSH_Q) s->pqn--;                                     /* merged into the newest */
    else for (int i = 0; i < 3; i++){ s->pq[s->pqn].v[i] = 0; s->pq[s->pqn].v0[i] = s->net_v[i]; }
    for (int i = 0; i < 3; i++){ s->pq[s->pqn].v[i] += v[i]; s->push_v[i] += v[i]; }
    s->pq[s->pqn++].t = t;
}
/* a friend's car touched the player's car this step: a hit (block_hits)
 * when it had not touched it for BLOCK_GAP_US, as a car leaning on it
 * touches it every step */
static void block_hit(Slot *s, int64_t host){
    int64_t last = s->touch_t;
    s->touch_t = host;
    if (last && host - last < BLOCK_GAP_US) return;
    g_pass.hit[g_pass.hit_at].id = s->peer_id; g_pass.hit[g_pass.hit_at].t = host;
    g_pass.hit_at = (g_pass.hit_at + 1) % BLOCK_Q;
    if (g_pass.hit_n < BLOCK_Q) g_pass.hit_n++;
}
/* roam.c, a touching pair (0x5B2610, thiscall: the pair, its bodies at +0xC
 * and +0x10, from the collision step 0x5FFA50): the two are rewound to the
 * moment they touch, the impulse (0x5AF0E0) goes into the bodies' own state,
 * their physics takes it ([body]+0x38) and they move on through the rest of
 * the step.  A friend's car has no physics here (mode 0: its body's
 * velocity is the car's own +0x70, 0x593B70), so the game hits it as an
 * immovable thing, and the friend's game hits the player's car the same
 * way: both cars took the whole hit.  Measured with two players and no
 * network delay: a car driven at 10.6 m/s into a standing one stopped dead
 * in one step (0.1 m/s) on its screen, and the standing one left at 10 m/s
 * within three on its own, where two equal cars both go on at about 5.  So
 * the player's car keeps the friend's car's part of the two cars' weight
 * (car_mass, by their square roots; O.hit_share % instead when set) of the
 * change the game gave its motion (contact_set), and the friend's car takes
 * the rest of the momentum as a push on the car as drawn here, from the
 * next frame until the friend's own report of the hit comes (push_pose):
 * against a car of the same weight half each, a 106 against a Hummer 63 and
 * 37 %.  By the weights themselves that was 75 and 25 %, and a braked Hummer
 * hardly moved for anyone (2026-10-24).  Each game does the same with its
 * own car, so the hit is shared on both screens: the same ram now left the
 * two at 5.6 and 4.4 m/s; at 32 m/s into a standing car (the game: the
 * driver back at -3.5, the other off at 35.5) 14.3 and 18.2; head on, 32
 * against 18.4 m/s, both on at 4.0 and 9.3 (the game: thrown back at 23 and
 * 37 m/s).
 * The game's crash view (slow motion and the jump camera: Player mode 6,
 * 0x6022E0) starts inside the pair's impulse (0x5AF0E0 -> 0x5A9620 ->
 * 0x59FB70) when the hit on the player's car (its impulse, capped at 40
 * first) reaches 30.  It weighed the game's whole hit, against an immovable
 * car: twice what the player's car keeps of one of the same weight, so a
 * friend's car started it at half the closing speed a car of the game's own
 * needs.  For the pair the threshold is 1.5 x the game's (CRASH_VIEW_X)
 * over the part the player's car keeps (the cap raised to it), and a next
 * pass of the same step (none kept) never starts it.  Measured with two
 * 240SX (1.22 t): the hit is about 1.85 x the velocity change it makes, 67
 * for 32 m/s into a standing car (the rammer turned back at -3.5 m/s), so
 * the view came from about 53 km/h of closing speed, with the game's own
 * over the part kept (60) from about 105, still too often, and now (90)
 * from about 155; that ram started it on both screens at 60, and on
 * neither now.  The crashed mark the same hit sets (car +0x38, 0x5F6BE0,
 * read by the physics) and the game's other thresholds of the capped hit
 * (30) keep the game's own values. */
#define G_CRASH_VIEW    0x007a1864u   /* float 30: the hit (t m/s) that starts the crash view */
#define G_CRASH_CAP     0x007a1858u   /* float 40: what 0x59FB70 caps the hit at first */
#define CRASH_VIEW_X    1.5f          /* a friend's car: the player's car's part of the hit starts it at this x the game's */
void roam_game_contact(CPU *c, void (*orig)(CPU *)){
    uint32_t pair = c->ecx, me = O.on && O.collide && O.hit_share != 100 ? player_car() : 0;
    uint32_t mb = ok(me, 0x40) ? rd32(me + 0x3cu) : 0;
    uint32_t a = mb && ok(pair, 0x14) ? MR32(pair + 0xcu) : 0, b = a ? MR32(pair + 0x10u) : 0;
    int k = -1;
    if (a == mb || b == mb){
        uint32_t other = a == mb ? b : a;
        for (int j = 0; j < NSLOT && other; j++)
            if (g_sl[j].st == SL_ACTIVE && !g_sl[j].hidden && ok(g_sl[j].car, 0x9e0) && MR32(g_sl[j].car + 0x3cu) == other) k = j;
    }
    uint32_t rb = k >= 0 && ok(mb, 0x6c) && MR32(mb + 0x5cu) ? rd32(mb + 0x68u) : 0;
    if (!ok(rb, 0xcc)){ orig(c); return; }
    Slot *s = &g_sl[k];
    float st0[CS_N], st[CS_N];
    float mm = car_mass(me, 1), mf = car_mass(s->car, 0);
    float keep = O.hit_share >= 0 ? (float)O.hit_share / 100.f : rc_sqrtf(mf) / (rc_sqrtf(mm) + rc_sqrtf(mf)), dv[3], push[3], d2 = 0;
    int again = s->hits && s->hit_step == g_step;
    uint32_t pl = rd32(G_PLAYER1), mode = ok(pl, 0x5c) ? MR32(pl + 0x58u) : 0;
    float view = MRF32(G_CRASH_VIEW), cap = MRF32(G_CRASH_CAP), need = again || keep <= 0.f ? 1e30f : CRASH_VIEW_X * view / keep;
    contact_get(me, rb, st0);
    MRF32(G_CRASH_VIEW) = need;
    if (need > cap) MRF32(G_CRASH_CAP) = need;
    orig(c);
    MRF32(G_CRASH_VIEW) = view; MRF32(G_CRASH_CAP) = cap;
    if (mode != 6u && ok(pl, 0x5c) && MR32(pl + 0x58u) == 6u)
        rlog("[roam] f=%ld hit with %s's car: the crash view (my car's part of the hit over %.0f)\n", rh_frame(), s->name, (double)(CRASH_VIEW_X * view));
    if (again){
        /* the pair again in the same step's next pass: the friend's car
         * stood where it was for the step (a body without physics moves
         * only with the pose, next frame), so the game met it again and
         * took the rest of the player's speed (10.6 -> 5.5 -> 3.0 m/s in
         * the tests); the pushed car would have moved on, so the player's
         * car keeps the velocity the first pass left it */
        contact_set(me, rb, st0);
        return;
    }
    s->hit_step = g_step;
    block_hit(s, roam_plat_now_us());
    contact_get(me, rb, st);
    for (int i = 0; i < CS_N; i++) if (!__builtin_isfinite(st[i])) return;
    for (int i = 0; i < 3; i++){ dv[i] = st[CS_V + i] - st0[CS_V + i]; d2 += dv[i] * dv[i]; push[i] = -(1.f - keep) * dv[i]; }
    for (int i = 0; i < CS_N; i++) st[i] = st0[i] + keep * (st[i] - st0[i]);
    contact_set(me, rb, st);
    float wdt = MRF32(G_WORLDDT);                                   /* the hit is a step after the car was drawn at s->rt */
    push_add(s, push, s->rt + (__builtin_isfinite(wdt) && wdt > 0.f && wdt < 1.f ? (int64_t)(wdt * 1e6f) : 0));
    float d = rc_sqrtf(d2);
    if (d > s->hit_max) s->hit_max = d;
    if (++s->hits <= 8 || d > 4.f)
        rlog("[roam] f=%ld hit with %s's car: the game changed my car's velocity by %.2f m/s (%.2f %.2f %.2f), kept %.0f %% "
             "(%.2f t against %.2f t); my velocity now %.2f %.2f %.2f, the push on theirs %.2f %.2f %.2f\n", rh_frame(), s->name, (double)d,
             (double)dv[0], (double)dv[1], (double)dv[2], (double)keep * 100.0, (double)mm, (double)mf,
             (double)st[CS_V], (double)st[CS_V + 1], (double)st[CS_V + 2], (double)s->push_v[0], (double)s->push_v[1], (double)s->push_v[2]);
}
/* roam.c, 0x42CD60, from the race's end (0x5F8490 -> 0x42EFB0): every car of
 * the World's AI range that is out gets a new goal from its brain (car +0x2C):
 * the one at the brain's +0x6FC is looked at and let go through its vtable
 * (0x42AA20 -> 0x402070), and the new one kept there.  During an outrun
 * between players friends' cars are out in that range (the three opponent
 * cars and the first 2 of ours), and their brains (vtable 0x7A5CA8, driver 0)
 * hold no goal there: one held 0xE490B3FC in the tests, and a player's game
 * trapped at an outrun's end reading past the memory (2026-10-20).  The AI
 * range goes without friends' cars for the call, as du_start narrows it. */
void roam_game_goals(CPU *c, void (*orig)(CPU *)){
    uint32_t w = world(), first = w ? MR32(w + 0x14u) : 0, end = w ? MR32(w + 0x18u) : 0, was[WORLD_INDEX], n = first;
    if (!w || first > end || end > WORLD_INDEX){ orig(c); return; }
    for (uint32_t i = first; i < end; i++){
        uint32_t car = was[i] = MR32(w + 0x1cu + 4u * i);
        int ours = 0;
        for (int k = 0; k < NSLOT && !ours; k++) ours = car && g_sl[k].car == car;
        if (!ours) MR32(w + 0x1cu + 4u * n++) = car;
    }
    if (n == end){ orig(c); return; }
    rlog("[roam] f=%ld the race's end: new goals for the AI range's cars, not for %u friends' car(s)\n", rh_frame(), end - n);
    MR32(w + 0x18u) = n;
    orig(c);
    for (uint32_t i = first; i < end; i++) MR32(w + 0x1cu + 4u * i) = was[i];
    MR32(w + 0x18u) = end;
}
/* roam.c, bSlotPool's constructor (0x440DF0), at the game's start.  With
 * free-roam multiplayer on, three pools get more entries:
 *  - the car loader's rides (CarLoadedRideInfoSlotPool, 22 x 0x1240 bytes):
 *    its Load (0x63AE70) waits until one is free, purging, for good when every
 *    entry is in use (the player's, the opponent cars', traffic's (up to 7), a
 *    car of our own's first ride, each friend's): 128 (600 KB);
 *  - the minimap's visuals and icons (MinimapEntity 102 x 8, MinimapElements
 *    102 x 0x20 bytes), taken with 0x43F8F0, which never grows a pool: a
 *    friend's arrow is one of each (minimap arrows), and an icon the pool
 *    could not give is linked in as a null pointer (0x4EC790): 256 each. */
static struct { uint32_t name, count; const char *what; int said; } g_pools[] = {
    { 0x007a8694u, RIDE_ENTRIES, "rides the car loader holds" },   /* "CarLoadedRideInfoSlotPool" */
    { 0x00790b18u, 256, "minimap visuals" },                       /* "MinimapEntity" */
    { 0x00790b08u, 256, "minimap icons" },                         /* "MinimapElements" */
};
/* The Cars pool's extra memory.  A track load adds the difference between
 * its size (0x7FA9D4 table, free roam's raised to O.carpool_kb) and the
 * startup size (+0x68) to the pool as one block (0x610750: car loader +0x6C,
 * +0x70), and the track's unload (0x580EF0, when the next track is another
 * one: a URL or drag race, a cover shoot, the front end) gives it back
 * (0x63A6F0): it unloads the loader's unused rides, takes the block out of
 * the pool (0x43F9E0) and frees it.  That removal only works when the block
 * is one free block again; when it is not, it does nothing and the block is
 * freed anyway, with the pool's free list still running through it, and the
 * next track load walked into it (0x43FA50: "memory access out of bounds",
 * or a hang; the 2026-10-21 crash reports, races entered from free roam
 * with every friend's car slot in use).  After free roam with friends the
 * block is not free again: friends' rides (still held then: the World is
 * gone, 0x5815C0 runs first, but roam_game_frame unloads them only on the
 * next frame) and about 23 MB more that stay in the pool after every ride
 * is unloaded.  So: friends' rides go first, the loader's unused rides go
 * as the game's own loop would, and the block is given back only when it
 * has become one free block; otherwise it stays in the pool (still recorded
 * at +0x6C), and the next track load adds nothing while base and block
 * cover its size (roam_game_pool_grow).  The pool then keeps free roam's
 * size through races and the front end.  roam.c, on the guest thread:
 * 1 = run the game's own 0x63A6F0. */
#define FN_LOADER_FREE  0x0063a000u   /* thiscall CarLoader (entry, 1): an unused ride's models out (0x63A6F0's loop) */
static int extra_is_free(uint32_t p, uint32_t x, uint32_t xn){
    uint32_t head = p + 4u, b = MR32(head);
    for (int n = 0; b != head && ok(b, 0x10) && n < 1000000; n++, b = MR32(b))
        if (b == x) return MR32(b + 8u) == xn;
    return 0;
}
int roam_game_loader_release(CPU *c){
    if (!g_inited || !O.on) return 1;
    uint32_t p = rd32(G_CARS_POOL), x = rd32(G_CARLOADER + 0x6cu), xn = rd32(G_CARLOADER + 0x70u);
    if (!x || !ok(p, 0x40)) return 1;
    uint32_t w = world();
    if (!w && (g_world_seen || g_npending)){
        rlog("[roam] f=%ld the car loader gives back its extra memory: friends' rides go first\n", rh_frame());
        if (g_world_seen) world_changed(c, 0);
        else flush_unloads(c);
    }
    uint32_t head = G_CARLOADER + 0x58u, e = rd32(head);
    for (int n = 0; e && e != head && n < 4 * RIDE_ENTRIES; n++){
        uint32_t nx = rd32(e), a2[2] = { e, 1 };
        rh_call(c, FN_LOADER_FREE, G_CARLOADER, 0, 2, a2);
        e = nx;
    }
    if (extra_is_free(p, x, xn)) return 1;
    rlog("[roam] f=%ld the car pool's extra %u KB is still in use (%.2f of %.2f MB used): it stays in the pool\n", rh_frame(),
         xn / 1024u, MR32(p + 0x28u) / 1048576.0, MR32(p + 0x24u) / 1048576.0);
    return 0;
}
/* roam.c: a track load's 0x610750 (thiscall CarLoader, size): 1 = let it add
 * the difference to the startup size; 0 = the pool has that size already
 * (with an extra block it kept, roam_game_loader_release) */
int roam_game_pool_grow(uint32_t size){
    uint32_t base = rd32(G_CARLOADER + 0x68u), x = rd32(G_CARLOADER + 0x6cu), xn = x ? rd32(G_CARLOADER + 0x70u) : 0;
    if (x && size <= base + xn) return 0;
    return 1;
}
/* No room in the Cars pool for a load.  A step of a load sizes what it reads
 * (a ride's textures 0x63AD8A, its models 0x63A2A2, ...: 0x48D420 counts
 * what does not fit into the largest free block) and calls the purge (0x63A1B0
 * with phase 1 allowed) until it fits or nothing more can go.  The purge
 * frees a ride nobody uses, and when there is none (phase 1) it takes the
 * textures off the rides it holds, the ones on cars too, and has their car
 * type's render infos made again: a car keeps a texture only another ride
 * holds (a car type shown twice), which goes with that ride, and draws with
 * it freed -- "memory access out of bounds" in its shadow or a part
 * (0x620AE0; overrides.c sub_00639f20 for the purge's lost references).  With
 * friends' cars that is no rare case: a few dozen in use, many of them loading
 * at once (the server's hub relinks a dozen as the player drives) where
 * pool_room checked room for one, and an hour of rides of all sizes coming and
 * going breaks the pool's free memory into pieces.  So in free roam, at a
 * load's memory check, a ride nobody uses goes first, as the game does, and
 * then the pool gets a block more (POOL_MORE, from the main pool, as a track
 * load adds its part: 0x610750), up to POOL_MORE_N of them; past that the
 * game's own purge.  The blocks stay in the pool.  roam.c: 1 = done here.
 * NFSU2_TEST_CARPOOL_FIXED=1 (test aid): the game's way. */
#define FN_BMALLOC      0x00440820u   /* cdecl (size, flags) -> memory */
#define FN_POOL_ADD     0x00440090u   /* cdecl (pool, memory, size) */
#define POOL_MORE       (16u << 20)
#define POOL_MORE_N     6
static int g_pool_more;
int roam_game_purge(CPU *c, uint32_t ret){
    static int fixed = -1;
    if (fixed < 0) fixed = rh_opt("NFSU2_TEST_CARPOOL_FIXED") ? 1 : 0;
    if (!g_inited || !O.on || fixed) return 0;
    switch (ret){                                  /* the load steps' memory checks (not Load's wait for a ride's place, 0x63AEBD) */
    case 0x0063a2bfu: case 0x0063a2ebu: case 0x0063a8d9u: case 0x0063a9cau: case 0x0063ab18u: case 0x0063ac8cu: case 0x0063ad9cu: break;
    default: return 0;
    }
    if (gcall1(c, FN_LOADER_PURGE, G_CARLOADER, 0) & 0xffu) return 1;
    if (g_pool_more >= POOL_MORE_N){
        static long said;
        if (!said || rh_frame() - said > 3600){ said = rh_frame();
            rlog("[roam] f=%ld car pool: no room for a load after %d blocks more: the game's purge\n", rh_frame(), g_pool_more); }
        return 0;
    }
    uint32_t a[2] = { POOL_MORE, 0x2000u }, mem = rh_call(c, FN_BMALLOC, 0, 0, 2, a);
    if (!mem) return 0;
    uint32_t b[3] = { 4u, mem, POOL_MORE };
    rh_call(c, FN_POOL_ADD, 0, 0, 3, b);
    g_pool_more++;
    rlog("[roam] f=%ld car pool: no room for a load (from 0x%08x): %u MB more (%d of %d)\n", rh_frame(), ret, POOL_MORE >> 20,
         g_pool_more, POOL_MORE_N);
    pool_log("more");
    return 1;
}
uint32_t roam_game_pool(uint32_t name, uint32_t count){
    for (unsigned i = 0; i < sizeof g_pools / sizeof g_pools[0]; i++){
        if (name != g_pools[i].name || count >= g_pools[i].count || !env_int("NFSU2_ROAM", 0, 0, 1)) continue;
        if (!g_pools[i].said++) rlog("[roam] %u %s, not %u (friends' cars)\n", g_pools[i].count, g_pools[i].what, count);
        return g_pools[i].count;
    }
    return count;
}
/* lifecycle.c, on the guest thread, when the app parks the game in the
 * background (Cmd-Tab, hidden: WM_ACTIVATEAPP(FALSE), then the game waits in
 * GetMessageA) and when it lets it run again (lifecycle_game_parked(): not
 * while a LAN session keeps the game running in the background).  No frame
 * runs while parked, but the network thread keeps answering friends with
 * the last status, so a player in free roam is announced as paused now
 * (friends see "paused" and the car standing where it stopped, for as long
 * as the pings keep coming); the next frame after the park sends the real status again.
 * int3a review: friends saw a frozen car marked "free roam". */
void roam_game_parked(int on){
    g_parked = on;
    if (!O.on || !g_net_ok) return;
    roam_plat_lock();
    int was_roam = g_s.status == RC_ST_ROAM;
    if (on && was_roam) rc_set_status(&g_s, RC_ST_PAUSED);
    roam_plat_unlock();
    if (on && was_roam) rlog("[roam] f=%ld the app is in the background: friends see you paused\n", rh_frame());
}
/* discord.c (the port's Discord presence): friends connected now, i.e.
 * established and not lost; 0 while free-roam multiplayer is off.  Takes the
 * session lock, so any thread may ask. */
int roam_game_friends(void){
    if (!O.on || !g_net_ok) return 0;
    int n = 0;
    roam_plat_lock();
    for (int i = 0; i < RC_TABLE; i++) if (g_s.peer[i].state == RC_PS_EST && !g_s.peer[i].lost) n++;
    roam_plat_unlock();
    return n;
}
/* status_web.c, on the guest thread: friends' cars the game holds now, being
 * made, loading, shown or parked (a parked one keeps its ride) */
int roam_game_cars(void){
    if (!O.on) return 0;
    int n = 0;
    for (int k = 0; k < NSLOT; k++) n += g_sl[k].st != SL_FREE;
    return n;
}
/* status_web.c, on the guest thread: Engage does something of ours now (engage_ours, this frame's) */
int roam_game_engage(void){ return O.on && g_engage; }
/* status_web.c, on the guest thread: friends' cars shown within a horn's reach (SAY_NEAR_M), so the touch controls'
 * Horn and Quick chat show; once there, until none is within 50 m more (no flicker at the edge) */
int roam_game_near(void){
    static int was;
    uint32_t me = O.on ? player_car() : 0u;
    if (!me || !ok(me, 0x70)) return was = 0;
    float x = MRF32(me + 0x60u), y = MRF32(me + 0x64u), r = SAY_NEAR_M, out = SAY_NEAR_M + 50.f;
    int n = 0, keep = 0;
    for (int k = 0; k < NSLOT; k++){
        uint32_t car = g_sl[k].car;
        if (g_sl[k].st != SL_ACTIVE || !ok(car, 0x70)) continue;
        float dx = MRF32(car + 0x60u) - x, dy = MRF32(car + 0x64u) - y, d2 = dx * dx + dy * dy;
        n += d2 <= r * r; keep += d2 <= out * out;
    }
    return was = n ? n : was ? keep : 0;
}
/* headlights.c, on the guest thread: is this a friend's car being shown? */
int roam_game_friend_car(uint32_t car){
    if (!O.on || !car) return 0;
    for (int k = 0; k < NSLOT; k++)
        if (g_sl[k].car == car && g_sl[k].st == SL_ACTIVE && !g_sl[k].hidden) return 1;
    return 0;
}
#endif
