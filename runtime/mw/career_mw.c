/* Most Wanted's test-only race aids (TRACKER M4-02; UG2's are runtime/career.c).  Scripted input drives the
 * menus but not the car, so an automated race run needs help.  Nothing here is for players: every aid waits
 * for a script line (runtime/autotest.c hands this file, through debug_mw.c, the commands below), and no
 * player's page sends a script.  As in UG2 the aids let the game's own code run (its AI driver, its finish
 * path, its own car warp) and never edit race or career state (L-022).
 *
 * Where they run: a command is queued and runs at the game's per-frame anchor (game.h on_frame, game_mw.c
 * MW_FRAME_SITE: MainLoop between its update and its render), on the game's thread, with the CPU state put
 * back afterwards (com.c call_guest); a command waits (autotest's next frames) until its work has run, at most
 * AID_WAIT frames: MainLoop is not the only way to a Present (loading screens present from 0x6618C3, 0x663D1A
 * and 0x664952), so a command queued while the anchor does not run logs a TIMEOUT line and is dropped.
 *
 *   racewin          a shortcut finish: the local player is finished where it is, with three of the calls
 *                    the race's script makes at the finish line: Game.NotifyRaceFinished(racer), then, when
 *                    Game.AllHumanPlayersDone(), Game.EnterPostRaceFlow(); a race not yet won is set won first
 *                    (RaceStatus.SetHasBeenWon, the script's own call), as some race kinds' finish skips it.  The script's calls at the line
 *                    (seen with NFSMW_HOOKLOG on the Lua bindings while the AI drove the player over it) are
 *                    NotifyCheckpointReached(racer, 0), NotifyLapFinished(racer, 1), SetRacerLapsLeft(1, 0),
 *                    CalculateRanking(racer, 1) (the racer's place, 1 when there is none), NotifyRaceFinished
 *                    (racer), NotifyRacePlacement(p, racer, 1), RacerIsHuman(1), AllHumanPlayersDone(),
 *                    EnterPostRaceFlow() (the values one race's).  racewin leaves out the lap bookkeeping
 *                    (NotifyLapFinished hands the racer's lap timer, GRacerInfo +0x158, to 0x5DC260), the
 *                    ranking and the placement message (p is a pointer the script holds, not identified).  So
 *                    the player is the only racer finished and wins, its race time is the time so far (3 to
 *                    6 s in runs), and the game extrapolates the others' (961 s for one at 1.4 %).  For
 *                    post-race flows only (race_loop.txt); a run that looks at rewards or the save lets the
 *                    autopilot drive over the line instead (race_win.txt).
 *   autopilot on|off the game's AI drives the local player's car (Game.ForceAIControl / ClearAIControl); a car
 *                    it wedges against a wall is logged (wedge_watch below)
 *   autopilot go     the same at the GO of each race from now on (a ForceAIControl before the GO did not hold for a
 *                    LAN Lap KO joiner, run ko of 2026-10-30), with the races' edges logged; no lost-race handling
 *   autopilot races  the same at every race's GO from now on, for races that follow each other with no menu
 *                    between (the career's prologue), with the races' edges logged; a lost race is raced again
 *                    the way a player does it (watch_race below); autopilot off ends it
 *   teleport X Y Z DX DY DZ   move the local player's car to X Y Z (~N: N from where it is), facing DX DY DZ
 *                    (the game's simulation space, as where logs it: Y is the height, and the warp sets
 *                    the car on the surface at X, Z: ~40 ~1 ~0 moved it 40 m along X and left it at Y 163.5
 *                    for 146.5 asked, ~0 ~5 ~0 did not lift it), the
 *                    way Game.WarpPlayerToTrigger moves it (its ISimable's interface 0x4040E0, method +0x68)
 *   where            log the local player's position, heading and speed
 *   carreset         the player's RESET: the car put back on the road the game's way (IResetable::ResetVehicle(true),
 *                    as the RESET action does it; the wedge watch below uses it), its place logged before and after
 *   bot ...          the career bot (bot_mw.c): bot gates, bot run N, bot race I, bot pursuit, bot save, bot off;
 *                    bot wait [N] waits up to N frames (default 900000) for the bot to stop
 *   racestatus       log the race: its racers' progress and finish flags, its own flags, and which racer the
 *                    local player is ("player 1": Game.GetRacerIndex, slot #0)
 *   frames           log the Presents and the main loop's passes so far (M4-01: one pass a Present)
 *   NFSU2_CAR_TRACE=<file> (no command): every race car per frame, UG2's CSV for tools/lan_trace.py (car_trace below)
 *   pursuit [watch]  log the player's pursuit state (heat, the pursuit's heat range, the pursuit object, its length,
 *                    the bust timer), once or whenever it changes; pursuit wait on|off [N] waits up to N frames for a
 *                    pursuit to be there or gone, pursuit wait breaker [N] for a pursuit breaker to come down
 *                    (pursuit_mw.c's hook); while watching, the pursuit's end is logged as the game decides it:
 *                    busted or evaded (the state's addresses and the end's sites: log_pursuit and mw_aid_hook below)
 *   breaker [watch]  log the player's Speedbreaker (on or off, its charge, the car's speed), once or whenever it
 *                    changes (a tenth of the charge); breaker wait on|off|full|empty [N] waits up to N frames for it,
 *                    breaker wait above MPH [N] for the car's speed, breaker wait charge X [N] for a charge of X or more,
 *                    breaker wait rise X [N] for the charge X above where it is (or full)
 *   rapsheet         log the rap sheet, the impound strikes and markers (pursuit_mw.c, which says what it reads)
 *   icons [T]        log the world map's icons, those of type T one by one (12 pursuit breakers, 11 hiding spots);
 *                    icons go T K DX DY DZ moves the car to icon K of type T (as teleport does), facing DX DY DZ
 *   pools [watch]    log the game's memory pools and the frame arena, with their peaks since `pools watch`
 *
 * The game's objects, from speed.exe v1.3's code (GAME.md 13.4; the names are its Lua bindings', registered
 * with their strings from 0x61E900 on):
 *   GRaceStatus, [0x91E000]: its racers (GRacerInfo, 0x194 bytes each) from +0x18, their count at +0x1958;
 *     +0x198A (byte) actively racing, written by SetActivelyRacing (0x604050); +0x4554 (byte) has been won
 *     (SetHasBeenWon 0x604080); +0x1964 the game mode, 2 in a career (Game.IsCareerMode 0x605610).
 *     GetRacerInfo(i) is 0x5DBED0 (thiscall, 0-based).
 *   GRacerInfo: +0x00 the racer's simable handle, +0x18 (float) the share of the race done (100 at the
 *     finish, NotifyRaceFinished sets it), +0x21 (byte) finished (0x5E97B0, which also stops its timer),
 *     +0x150 (float) its race time.
 *   The players: an IPlayer* array at [0x92D87C], their count at [0x92D884]; IPlayer method +4 is its
 *     ISimable.  ISimable +0x60 its position (a const Vector3*), +0x54 its vehicle, whose +0x2C is the speed
 *     in m/s (GetSimableSpeedKmh 0x604E10); [ISimable+4] is the object its interfaces are queried from
 *     (0x5D59F0, thiscall (id): ids are addresses, a sorted array of {id, interface} at +4..+8).
 *   Game.NotifyRaceFinished (0x6119F0, cdecl (ISimable*; 0 = player 1's)), Game.ForceAIControl (0x6123B0)
 *     and Game.ClearAIControl (0x612420, cdecl (player number; 0 = the first)), Game.GetRacerIndex
 *     (0x60DAE0, cdecl (ISimable*): 1-based, -1 = none), Game.AllHumanPlayersDone (0x60E0A0, bool in al),
 *     Game.EnterPostRaceFlow (0x611F20: ends the race and shows the post-race screen, InGamePhotoMaster.fng
 *     after a win, PostRace_Results.fng after a loss, as seen in races the AI drove). */
#include "cpu.h"
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

long autotest_frame(void);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t com_present_no(void);
uint64_t debug_frame_passes(void);

#define G_RACESTATUS     0x0091e000u
#define RS_RACERS        0x18u
#define RS_NRACERS       0x1958u
#define RS_ACTIVE        0x198au
#define RS_WON           0x4554u
#define RS_GO            0x16cu          /* the first racer's timer flag (GRacerInfo +0x154): the race's GO */
#define RS_MODE          0x1964u         /* the game mode: 2 career (Game.IsCareerMode 0x605610 tests it) */
#define RACER_SIZE       0x194u
#define RI_HANDLE        0x00u
#define RI_PCT           0x18u
#define RI_FINISHED      0x21u
#define RI_TIME          0x150u
#define G_PLAYERS        0x0092d87cu
#define G_NPLAYERS       0x0092d884u
#define FN_NOTIFY_FINISHED  0x006119f0u
#define FN_FORCE_AI      0x006123b0u
#define FN_CLEAR_AI      0x00612420u
#define FN_RACER_INDEX   0x0060dae0u
#define FN_ALL_HUMANS_DONE 0x0060e0a0u
#define FN_POSTRACE      0x00611f20u
#define FN_SET_WON       0x00604080u     /* RaceStatus.SetHasBeenWon(bool) (thiscall, ecx = GRaceStatus): Lua method, the script calls it when the player wins */
#define FN_QUERY         0x005d59f0u
#define FN_IS_ROAMING    0x0056dc00u     /* 1 while GRaceStatus' activity (+0x1968) is free roam's ("1.8.1"): the pause
                                          * menu's test (0x52B5C9) between the free-roam and the race menu */
#define IID_WARP         0x004040e0u     /* the interface WarpPlayerToTrigger moves the car with */
#define WARP_METHOD      0x68u
#define SIM_POSITION     0x60u
#define SIM_VEHICLE      0x54u
#define VEH_SPEED        0x2cu

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static float rdf(uint32_t a){ uint32_t v = rd(a); float f; memcpy(&f, &v, 4); return f; }

/* a guest call with ecx = this (0 for cdecl/stdcall); the CPU state is put back (call_guest) */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args){
    uint32_t keep = c->ecx; c->ecx = self;
    uint32_t r = call_guest(c, fn, n, args);
    c->ecx = keep;
    return r;
}
/* a call that returns a float on the x87 stack: call_guest puts the whole CPU back, x87 stack too, so the
 * value is read inside it; no function (a method slot read from memory not committed) is 0, as call_guest's */
static float gcall_f(CPU *c, uint32_t fn, uint32_t self){
    if (!fn) return 0.0f;
    CPU save = *c;
    c->ecx = self;
    PUSH32(0xDEADBEEFu);
    void dispatch_indirect(CPU *c, uint32_t addr); dispatch_indirect(c, fn);
    float f = (float)ST(0);
    *c = save;
    return f;
}

/* player 1's ISimable (IPlayer method +4), 0 = none */
static uint32_t player_simable(CPU *c){
    if (!rd(G_NPLAYERS)) return 0;
    uint32_t pl = rd(rd(G_PLAYERS));
    if (!pl || !rd(pl)) return 0;
    return gcall(c, rd(rd(pl) + 4u), pl, 0, 0);
}

static void log_where(CPU *c){
    uint32_t s = player_simable(c);
    if (!s){ fprintf(stderr, "[mwrace] f=%ld where: no player\n", autotest_frame()); return; }
    uint32_t p = gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0);
    uint32_t v = gcall(c, rd(rd(s) + SIM_VEHICLE), s, 0, 0);
    float kmh = v ? gcall_f(c, rd(rd(v) + VEH_SPEED), v) * 3.6f : 0.0f;
    fprintf(stderr, "[mwrace] f=%ld where %.2f %.2f %.2f speed %.1f km/h\n", autotest_frame(),
            rdf(p), rdf(p + 4u), rdf(p + 8u), kmh);
}

/* the race, and which racer the local player is (Game.GetRacerIndex, 1-based: racer 1 is slot #0) */
static void log_status(CPU *c){
    uint32_t rs = rd(G_RACESTATUS);
    if (!rs){ fprintf(stderr, "[mwrace] f=%ld racestatus: no GRaceStatus\n", autotest_frame()); return; }
    uint32_t s = player_simable(c), a = s;
    int me = s ? (int)gcall(c, FN_RACER_INDEX, 0, 1, &a) : -1;
    int n = (int)rd(rs + RS_NRACERS);
    char b[640]; int o = 0;
    for (int i = 0; i < n && i < 16 && o < (int)sizeof b - 48; i++){
        uint32_t ri = rs + RS_RACERS + (uint32_t)i * RACER_SIZE;
        o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.1f%%%s %.2fs", i, rdf(ri + RI_PCT),
                      MR8(ri + RI_FINISHED) ? " finished" : "", rdf(ri + RI_TIME));
    }
    int roam = (int)(gcall(c, FN_IS_ROAMING, 0, 0, 0) & 0xffu);
    fprintf(stderr, "[mwrace] f=%ld racestatus flow %u racers %d active %u won %u player %d:%s; mode %u roaming %d\n",
            autotest_frame(), game_flow(), n, MR8(rs + RS_ACTIVE), MR8(rs + RS_WON), me, o ? b : " none",
            rd(rs + RS_MODE), roam);
}

/* racewin's calls: the local player finished where it is (Game.NotifyRaceFinished), then, when every human is done,
 * the post-race flow */
static void race_win(CPU *c){
    long f = autotest_frame();
    uint32_t s = player_simable(c), a = s;
    int idx = s && rd(G_RACESTATUS) ? (int)gcall(c, FN_RACER_INDEX, 0, 1, &a) : -1;   /* it reads GRaceStatus */
    if (game_flow() != g_game.flow_race || idx < 1){
        fprintf(stderr, "[mwrace] f=%ld racewin: not in a race (flow %u, racer %d)\n", f, game_flow(), idx);
        return; }
    uint32_t arg = s;
    gcall(c, FN_NOTIFY_FINISHED, 0, 1, &arg);
    /* some race kinds finish without the script's win call (NotifyRacePlacement only posts a message): the
     * player is first here, so the race is won as the script's SetHasBeenWon(true) makes it */
    uint32_t rs = rd(G_RACESTATUS), yes = 1;
    if (rs && !MR8(rs + RS_WON)) gcall(c, FN_SET_WON, rs, 1, &yes);
    int all = (int)(gcall(c, FN_ALL_HUMANS_DONE, 0, 0, 0) & 0xffu);
    if (all) gcall(c, FN_POSTRACE, 0, 0, 0);
    fprintf(stderr, "[mwrace] f=%ld racewin: racer %d finished through NotifyRaceFinished; all humans done %d%s\n",
            f, idx, all, all ? ": EnterPostRaceFlow" : "");
    log_status(c);
}

/* ---- autopilot races: the game's AI takes the player's car at each race's GO ----
 * A career runs races back to back with no menu between them (the prologue's), and the game drops a
 * ForceAIControl when a race ends, so this mode watches the race at every anchor: when the race is actively
 * racing with its timer running (GRaceStatus +0x198A and the first racer's timer flag, +0x16C) and that was not
 * so the frame before, or the player's car is another one, it calls Game.ForceAIControl(0) again.  It logs the
 * race's edges as it sees them (active, GO, slot #0 finished, won), so a log shows each race's start and end.
 * The AI that drives the player's car loses now and then (in 4 of 10 career_start.txt runs it lost one prologue
 * race, race 2 or race 3: 4 of the 30 races it had to win), and a story race has to be won to go on.  What a
 * player does then, the aid does: a lost race (slot #0 finished, the race not won) shows PostRace_Results.fng,
 * whose Enter goes back to the career menu (MainMenu_Sub.fng), where Resume Career loads the race again from the
 * autosave made before it (shown in a probe run that made the AI let go, and in 3 of the 4 career_start.txt runs
 * that lost a race; the fourth crashed in that reload); so from LOST_WAIT frames after the loss, every LOST_EVERY
 * frames, it taps Enter on either screen while it takes input, until a race's GO.
 * racewin's shortcut is no way out here: after it the prologue's own script never saw the player at the line,
 * and the winner's screen took no Enter (3 runs; one with the car stopped far from the line). */
#define LOST_WAIT  240
#define LOST_EVERY 120
#define DIK_RETURN 0x1cu
int mw_fe_match(const char *name);       /* fe_mw.c: the top package taking input, and its state */
void autotest_tap(unsigned dik, long frames);
static int auto_ai;
static struct { uint32_t rs, sim; int active, go, fin, won; long lost; } W;
static void watch_race(CPU *c){
    uint32_t rs = rd(G_RACESTATUS), s = player_simable(c);
    int active = rs ? MR8(rs + RS_ACTIVE) : 0, go = rs ? MR8(rs + RS_GO) != 0 : 0;
    int fin = rs && rd(rs + RS_NRACERS) ? MR8(rs + RS_RACERS + RI_FINISHED) : 0, won = rs ? MR8(rs + RS_WON) : 0;
    long f = autotest_frame();
    if (active != W.active || go != W.go || fin != W.fin || won != W.won)
        fprintf(stderr, "[mwrace] f=%ld race: active %d go %d #0 finished %d won %d (flow %u)\n", f, active, go, fin,
                won, game_flow());
    if (active && go && s && (!(W.active && W.go) || s != W.sim || rs != W.rs)){
        uint32_t zero = 0;
        gcall(c, FN_FORCE_AI, 0, 1, &zero);
        W.lost = 0;
        fprintf(stderr, "[mwrace] f=%ld autopilot on: the race's GO\n", f);
    }
    if (auto_ai == 2){ W.rs = rs; W.sim = s; W.active = active; W.go = go; W.fin = fin; W.won = won; return; }
    if (active && fin && !won && !W.lost && !(W.fin && !W.won)) W.lost = f;   /* finished, not won: maybe lost */
    W.rs = rs; W.sim = s; W.active = active; W.go = go; W.fin = fin; W.won = won;
    if (!W.lost || f - W.lost < LOST_WAIT || (f - W.lost) % LOST_EVERY) return;
    const char *what = mw_fe_match("PostRace_Results.fng") ? "the race was lost: Enter on its results"
                     : mw_fe_match("MainMenu_Sub.fng:e072db21@4b4e388a") ? "the career menu: Enter on Resume Career" : 0;
    if (!what) return;
    autotest_tap(DIK_RETURN, 6);
    fprintf(stderr, "[mwrace] f=%ld autopilot: %s\n", f, what);
}

/* ---- a car the autopilot wedged: put back on the road by the game's own reset ----
 * The game's AI that drives the player's car (ForceAIControl) can wedge it against a wall and keep pushing, a few
 * metres forward and back: in LAN sprints of 2026-10-30 the joiner's car stayed within 30 m of one spot for nine
 * minutes (run a1), the host's after its Speedbreaker test (sb), and in a Lap KO race the host's (ko); the race then
 * never reaches the results.  The AI's own way out (AIActionGetUnstuck, its reversing) does not free it there.
 * The game's reset is the ResetCar behaviour every car has (factory 0x6B0630, constructor 0x6A41E0): its interface
 * IResetable, id 0x620250 of the car's ISimable object (vtable 0x8ABA10: +4 HasResetPosition, +8 ResetVehicle(bool
 * manual) 0x6B08C0, +0xC SetResetPosition, +0x10 ClearResetPosition).  ResetVehicle(true) first tries the reset to
 * the road network (0x6B06D0(2): the nearest road point, WRoadNetwork 0x9AA540's 0x727480, the car set there along
 * the road by 0x6A45F0), else the last good place the behaviour kept (a ring of 4 at +0x68, filled while the car is
 * upright on the ground); its own update (0x6B07C0) does the same for a car upside down or off the world.  The
 * player's RESET action does exactly this (the input handler at 0x6A8B88: ResetVehicle(true), then 0x47CC50, which
 * clears the input controllers' held state), and so does the aid: while the autopilot drives (in a race: GRaceStatus
 * active, GO, the local racer not finished; in free roam: `autopilot on`), a car that moved less than STUCK_M
 * metres in STUCK_S seconds of game time (the simulation's steps x its step, [0x9203B0] +0x28 x +0x20, so a slow or
 * paused frame rate changes nothing) is reset that way and the watch starts again from where it is put.  (R, the
 * RESET binding, tapped with the AI let go did not move the car in the LAN runs: the key is not bound to that action
 * by the PC game's defaults, inferred, not checked.)  No reset in a pursuit: the bust timer ran from 1.0 to 4.9 in the
 * second after one (bot pursuit probes of 2026-10-30, twice), and three wedge resets in pursuits were each followed by
 * the bust within 6 s. */
#define STUCK_S 8.0
#define STUCK_M 12.0f
#define IID_RESET        0x00620250u     /* IResetable, the ResetCar behaviour's interface */
#define RESET_VEHICLE    0x08u           /* IResetable::ResetVehicle(bool manual) 0x6B08C0 */
#define FN_INPUT_CLEAR   0x0047cc50u     /* the input handler's call after a reset: the controllers' held state cleared */
#define G_SIM            0x009203b0u     /* the simulation: +0x20 its step (float, s), +0x28 the steps it ran */
static int ai_on;                       /* autopilot on: the AI drives the local player's car */
static struct { double since; float p[3]; int resets, futile; uint32_t sim; } U;
double mw_game_time(void){
    uint32_t sim = rd(G_SIM);
    return sim ? (double)rd(sim + 0x28u) * (double)rdf(sim + 0x20u) : 0.0;
}
/* the player's RESET: 1 the car was put back, 0 the game found no place, -1 no car */
int mw_reset_car(CPU *c){
    uint32_t s = player_simable(c), obj = s ? rd(s + 4u) : 0u, id = IID_RESET;
    uint32_t r = obj ? gcall(c, FN_QUERY, obj, 1, &id) : 0u;
    if (!r || !rd(r)) return -1;
    uint32_t manual = 1;
    int ok = (int)(gcall(c, rd(rd(r) + RESET_VEHICLE), r, 1, &manual) & 0xffu) != 0;
    gcall(c, FN_INPUT_CLEAR, 0, 0, 0);
    return ok;
}
static uint32_t pursuit_obj(CPU *c, uint32_t *perp);  /* below: the player's pursuit, 0 = none */
static void wedge_watch(CPU *c){
    long f = autotest_frame();
    uint32_t rs = rd(G_RACESTATUS), s = player_simable(c), a = s;
    if (!rs || !s || game_flow() != g_game.flow_race){ U.since = 0; return; }
    if (ai_on && s != U.sim){           /* another car (a world loaded again: a bust, a jump): the AI takes it too */
        uint32_t zero = 0;
        gcall(c, FN_FORCE_AI, 0, 1, &zero);
        if (U.sim) fprintf(stderr, "[mwrace] f=%ld autopilot on again: another car (the world was loaded again)\n", f);
        U.sim = s; U.since = 0;
    }
    int racing = MR8(rs + RS_ACTIVE) && MR8(rs + RS_GO);
    if (!racing && !ai_on){ U.since = 0; return; }            /* a race before its GO, or its end, with `autopilot go` */
    if (racing){
        int idx = (int)gcall(c, FN_RACER_INDEX, 0, 1, &a);
        if (idx >= 1 && idx <= 16 && MR8(rs + RS_RACERS + (uint32_t)(idx - 1) * RACER_SIZE + RI_FINISHED)){ U.since = 0; return; }
    }
    uint32_t p = gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0);
    if (!p || !mem_is_committed(p + 11u)) return;
    float q[3] = { rdf(p), rdf(p + 4u), rdf(p + 8u) };
    float d = sqrtf((q[0] - U.p[0]) * (q[0] - U.p[0]) + (q[1] - U.p[1]) * (q[1] - U.p[1]) + (q[2] - U.p[2]) * (q[2] - U.p[2]));
    double t = mw_game_time();
    if (U.since <= 0.0 || d >= STUCK_M || t < U.since){
        if (d >= STUCK_M) U.futile = 0;
        U.since = t > 0.0 ? t : 1e-9; memcpy(U.p, q, sizeof q); return;
    }
    if (t - U.since < STUCK_S * (U.futile >= 3 ? 4 : 1)) return;   /* resets that put it back where it was: less often */
    if (pursuit_obj(c, 0)){             /* a reset with cops on the car ends in the bust (below): none */
        fprintf(stderr, "[mwrace] f=%ld autopilot: the car moved %.1f m in %.1f s of game time in a pursuit: no reset\n", f, d,
                t - U.since);
        U.since = t; memcpy(U.p, q, sizeof q);
        return;
    }
    int ok = mw_reset_car(c);
    U.resets++;
    uint32_t p2 = gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0);
    float n[3] = { rdf(p2), rdf(p2 + 4u), rdf(p2 + 8u) };
    float moved = sqrtf((n[0] - q[0]) * (n[0] - q[0]) + (n[2] - q[2]) * (n[2] - q[2]));
    U.futile = moved < 1.0f ? U.futile + 1 : 0;
    if (U.futile <= 3)
        fprintf(stderr, "[mwrace] f=%ld autopilot: the car moved %.1f m in %.1f s of game time (at %.1f %.1f %.1f): wedged; "
                "reset %s, now at %.1f %.1f %.1f (reset %d)%s\n", f, d, t - U.since, q[0], q[1], q[2],
                ok > 0 ? "done" : ok == 0 ? "found no place" : "no car", n[0], n[1], n[2], U.resets,
                U.futile == 3 ? "; the resets put it back where it was: from now on every 32 s, not logged" : "");
    if (racing) log_status(c);
    U.since = t; memcpy(U.p, n, sizeof n);
}

/* an icon's place (pursuit_mw.c: GManager's icon +0x10..+0x18, the world's axes, Z up) as simulation space (Y up),
 * the swizzle the game's bVector3 -> UMath::Vector3 conversions use: (-y, z, x) */
static void icon_to_sim(const float w[3], float s[3]){ s[0] = -w[1]; s[1] = w[2]; s[2] = w[0]; }

/* ---- the queue: one piece of work at a time, run at the next frame anchor ---- */
#define AID_WAIT 600                    /* frames a queued command waits for the anchor (once a frame in MainLoop) */
enum { Q_NONE, Q_RACEWIN, Q_AI_ON, Q_AI_OFF, Q_AI_RACES, Q_AI_GO, Q_TELEPORT, Q_WHERE, Q_STATUS, Q_PURSUIT, Q_PWATCH, Q_PWAIT,
       Q_BREAKER, Q_BWATCH, Q_BWAIT, Q_BOT, Q_BOTWAIT, Q_RESET, Q_RAP, Q_ICONS, Q_ICONGO, Q_POOLS };
void mw_aid_rapsheet(CPU *c, const char *why);  /* pursuit_mw.c: the rap sheet, the world map's icons, the pools */
void mw_aid_icons(CPU *c, int type);
int mw_aid_icon_place(int type, int k, float pos[3]);
void mw_aid_pools(CPU *c, int watch);
void mw_pursuit_frame(CPU *c);
unsigned long mw_pursuit_breakers(void);
void mw_bot_cmd(CPU *c, const char *args);     /* bot_mw.c: the career bot's commands, run at the anchor */
void mw_bot_frame(CPU *c);                     /* bot_mw.c: its work, each pass of the main loop */
int mw_bot_idle(void);                         /* bot_mw.c: 1 once the bot has stopped (done, or off) */

/* The player's pursuit state (offsets read in runs, their meaning where a run showed it or the code says it):
 *   IPerpetrator, interface 0x4037E0 of the player's ISimable object (vtable 0x892988): +0x1C the heat, a float
 *     (GetHeat, method +4, 0x409400; SetHeat, method +8, 0x409060, is what Game.SetWorldHeat 0x612660 calls);
 *     +0x20 the bounty (Game.GetPlayerBounty 0x612300 gave the same, 0 before a pursuit and 100 after it);
 *     +0x24 went 0 -> 100 during a pursuit and back to 0 at its end (not identified).
 *   the vehicle's AI interface, 0x403700 (vtable 0x8929F0): +0x70 the pursuit object (vtable 0x892770) from the
 *     pursuit's start (Game.ForcePursuitStart 0x60AAC0) to its end, else 0.
 *   the pursuit object (AIPursuit; the pointer is its base + 0x48, the base built by 0x442D00): +0xB4 its length
 *     in seconds (method +0x18, 0x4338C0); +0x9C and +0xA0 the heat range it holds the player's heat in (floats:
 *     its update, 0x443C40.., sets the heat to clamp(heat + escalation, +0x9C, +0xA0) through SetHeat every few
 *     frames, so a heat set from outside does not stay above +0xA0); +0xDC the bust timer (seconds the player has
 *     been held: the update sets the busted flag, +0xE8, when it passes 5.0, and then posts MPerpBusted).
 *   The range is set when the pursuit is made (0x442FEB..0x443171): in a career (GRaceStatus +0x1964 == 2) from
 *     the current Blacklist bin (GRaceStatus +0x196C, an Attrib instance: MinHeatLevel 0x8F186AC4 and
 *     MaxHeatLevel 0xE8C24416 of the race_bin_NN collection: 1-2 in bins 15-13, up to 3 in 12-9, 4 in 8-5, 5 in
 *     4-1; the prologue's bin 16 inherits race_bin's 0-1), 6-6 for the final pursuit (0x5FC560), and an event's
 *     own MinHeatLevel/MaxHeatLevel (0xE4211F4F int, 0xF5A03629 float) for a pursuit event (0x45F2AD6C set: the
 *     Challenge Series' pursuits); outside a career 1-5.  Game.ForcePursuitStart(N) passes N as the message's
 *     MinHeatLevel (MForcePursuitStart, +0x10), and the cop manager (0x42EBF0) raises the heat to N first.
 * `pursuit` logs these once, `pursuit watch` whenever one changes (each frame anchor; the bust timer by whole
 * seconds), `pursuit wait on|off [N]` waits (at most N frames, default 6000) for a pursuit object to be there, or
 * gone.  While watching, the end is logged from the game's own code (hooks, mw_aid_hook): "busted" when the
 * pursuit's update posts MPerpBusted for the player (0x444695; 0x444711 is an AI racer's), "evaded" when the cop
 * manager's evade method runs (vtable 0x891900 +0x10, 0x411070: it counts pursuits_in_a_row and posts
 * MPerpEscaped). */
#define IID_PERP         0x004037e0u
#define IID_AIVEH        0x00403700u
#define PERP_HEAT        0x1cu
#define AIVEH_PURSUIT    0x70u
#define PURSUIT_TIME     0xb4u
#define PURSUIT_HEAT_MIN 0x9cu
#define PURSUIT_HEAT_MAX 0xa0u
#define PURSUIT_BUST_T   0xdcu
#define PURSUIT_BUSTED   0xe8u
#define PURSUIT_COPS     0x20u      /* the number of cops in the pursuit (its list at +0x18; the update walks it) */
#define HOOK_SITE(va) (va##u)
#define SITE_BUSTED      HOOK_SITE(0x00444695)   /* the pursuit update's MPerpBusted post for the player */
#define SITE_BUSTED_AI   HOOK_SITE(0x00444711)   /* ... for an AI racer (AIRacerBusted) */
#define SITE_EVADED      HOOK_SITE(0x00411070)   /* the cop manager's evade method (the pursuit is its argument) */
static int pw_on;
static uint32_t pw_last[16];
static uint32_t pursuit_obj(CPU *c, uint32_t *perp){
    uint32_t s = player_simable(c), obj = s ? rd(s + 4u) : 0u, id = IID_PERP, id2 = IID_AIVEH;
    uint32_t p = obj ? gcall(c, FN_QUERY, obj, 1, &id) : 0u, a = obj ? gcall(c, FN_QUERY, obj, 1, &id2) : 0u;
    if (perp) *perp = p;
    return rd(a + AIVEH_PURSUIT);
}
/* pursuit_mw.c: the pursuit's state and the counts the game keeps by name, appended to the line (x: 8 values for the
 * change test; the heat by tenths, so the escalation shows) */
void mw_pursuit_extras(CPU *c, uint32_t po, uint32_t sig[8], char *out, size_t n);
void mw_pursuit_end(CPU *c, const char *how);  /* pursuit_mw.c: the cop cars asked for and the pools, at the end */
static void log_pursuit(CPU *c, int only_changes){
    uint32_t p = 0, po = pursuit_obj(c, &p);
    float bt = po ? rdf(po + PURSUIT_BUST_T) : 0.0f;
    char x[384];
    uint32_t v[16] = { (uint32_t)(int)(rdf(p + PERP_HEAT) * 10.0f), rd(p + 0x20u), rd(p + 0x24u), po, game_flow(),
                      po ? (uint32_t)(bt > 0.0f ? bt : 0.0f) : 0u, po ? MR8(po + PURSUIT_BUSTED) : 0u,
                      po ? rd(po + PURSUIT_COPS) : 0u };
    mw_pursuit_extras(c, po, v + 8, x, sizeof x);
    if (only_changes && !memcmp(v, pw_last, sizeof v)) return;
    memcpy(pw_last, v, sizeof v);
    float h = rdf(p + PERP_HEAT), t = rdf(po + PURSUIT_TIME);
    char r[96] = "";
    if (po) snprintf(r, sizeof r, " range %.2f-%.2f cops %u bust %.1f%s", rdf(po + PURSUIT_HEAT_MIN),
                     rdf(po + PURSUIT_HEAT_MAX), v[7], bt, v[6] ? " busted" : "");
    fprintf(stderr, "[mwpursuit] f=%ld heat %.2f pursuit %08x time %.1f%s bounty %u perp+24 %u flow %u%s\n",
            autotest_frame(), h, po, po ? t : 0.0f, r, v[1], v[2], v[4], x);
}

/* the pursuit's end as the game decides it (the sites above; logged while `pursuit watch` is on, and kept for the
 * career bot: mw_aid_last_end) */
static int last_end;                    /* 1 busted, 2 evaded (the player's), 0 none since the last read */
int mw_aid_last_end(void){ int e = last_end; last_end = 0; return e; }
int mw_aid_pw_on(void){ return pw_on; }
static int breaker_hook(CPU *c, uint32_t va);
int mw_aid_hook(CPU *c, uint32_t va){
    if (breaker_hook(c, va)) return 1;
    if (va != SITE_BUSTED && va != SITE_BUSTED_AI && va != SITE_EVADED) return 0;
    if (va == SITE_EVADED) last_end = 2; else if (va == SITE_BUSTED) last_end = 1;
    if (!pw_on) return 1;
    if (va == SITE_EVADED)              /* cop manager +0x10 (pursuit): ecx the cop manager, the pursuit at [esp+4] */
        fprintf(stderr, "[mwpursuit] f=%ld evaded: pursuit %08x (the cop manager's evade, 0x411070)\n",
                autotest_frame(), MR32(c->esp + 4u));
    else                                /* the pursuit's update: esi its base + 8, so the pursuit object is esi + 0x40 */
        fprintf(stderr, "[mwpursuit] f=%ld busted: pursuit %08x%s\n", autotest_frame(), c->esi + 0x40u,
                va == SITE_BUSTED ? " (MPerpBusted for the player, 0x444695)" : " (an AI racer, 0x444711)");
    if (va != SITE_BUSTED_AI) mw_pursuit_end(c, va == SITE_EVADED ? "evaded" : "busted");
    return 1;
}

/* The player's Speedbreaker (the game's "game breaker"; exploration, M4-19).  The IPlayer interface (the entries of
 * the player array [0x92D87C], vtable 0x8B0B38) sits at the player object's +0x4C: +0x34 (byte) on, +0x38 (float)
 * the charge, 0..1.  The input action GAMEBREAKER (Right Ctrl or G) runs IPlayer method +0x28 (0x6EDD10): off when
 * on, else on when the charge is above 0 (0x6E9AA0 switches it); it does nothing (but switch off) unless the
 * car is at 30 mph or more ([0x8B01C0]) and a few of the car's states allow it (0x6EDC90).  Each frame 0x6EDD60 (the player's update)
 * drains the charge while on (0.1 a second of its second time argument, unless the byte [0x988E1C] is set) and
 * refills it while off, by 1/25 a second ([0x901AE4] = 25 s from empty to full), but only while the car is faster
 * than [0x901AE8] = 100 mph (m/s x 2.23699, [0x8967E0]); it switches off at an empty charge.  The speed below is
 * the car's (ISimable's vehicle, +0x2C, m/s) in mph as that code converts it. */
#define PL_BREAKER_ON    0x34u
#define PL_BREAKER       0x38u
#define MPH_PER_MS       2.23699f
static int bw_on;
static int bw_last[4] = { -1, -1, -1, -1 };
static uint32_t player_iface(void){ return rd(G_NPLAYERS) ? rd(rd(G_PLAYERS)) : 0u; }
static float player_mph(CPU *c){
    uint32_t s = player_simable(c), v = s ? gcall(c, rd(rd(s) + SIM_VEHICLE), s, 0, 0) : 0u;
    return v ? gcall_f(c, rd(rd(v) + VEH_SPEED), v) * MPH_PER_MS : 0.0f;
}
/* CanUse (0x6EDC90, thiscall on the player object, IPlayer - 0x4C) and what it tests, as bits: 1 [0x988E04] set,
 * 2 [0x9885C8] set, 4 under 30 mph, 8 vehicle method +0xBC, 16 +0x20, 32 +0x5C (the car interface 0x4040E0's) */
#define FN_BREAKER_CANUSE 0x006edc90u
static int breaker_blocks(CPU *c, uint32_t pl, int *can){
    uint32_t s = player_simable(c), obj = s ? rd(s + 4u) : 0u, id = IID_WARP;
    uint32_t w = obj ? gcall(c, FN_QUERY, obj, 1, &id) : 0u;
    int b = (rd(0x00988e04u) ? 1 : 0) | (rd(0x009885c8u) ? 2 : 0) | (player_mph(c) < 30.0f ? 4 : 0);
    if (w){
        b |= (gcall(c, rd(rd(w) + 0xbcu), w, 0, 0) & 0xffu) ? 8 : 0;
        b |= (gcall(c, rd(rd(w) + 0x20u), w, 0, 0) & 0xffu) ? 16 : 0;
        b |= (gcall(c, rd(rd(w) + 0x5cu), w, 0, 0) & 0xffu) ? 32 : 0;
    }
    *can = (int)(gcall(c, FN_BREAKER_CANUSE, pl - 0x4cu, 0, 0) & 0xffu);
    return b;
}
/* The press's way in (TRACKER M4-19, M4-47 (a)): the player's InputPlayer (its FetchInput 0x6A8870, vtable slot
 * 0x8AC668, `this` kept by the hook at its entry) reads the action queue at +0x10 (a ring: +0 the count, +8 the head,
 * +0xC the capacity, the actions from +0x10, 12 bytes each; IsEmpty 0x633D80, GetAction 0x633DC0, Pop 0x633D90) once a
 * frame, unless +0x2B8 is set (then its method +0xC) or the object at +8 answers yes to its method +0x74 (then it
 * hands the car's controls on and returns, 0x6A88BA: the queue is not read).  Each action goes through the filter at
 * this - 0x74 (method +0x30) and a switch on its id - 1 (tables 0x6A8E58, 0x6A8E20); GAMEACTION_GAMEBREAKER (8, the
 * name table 0x8F6728) is the case at 0x6A8A5D, which runs the toggle (the player's interface 0x8B0BD0 method +0x50,
 * 0x6EDD10, from 0x6A8A90) only when two flags of the frame are set: [esp+0x12], no AI on the car (the object at
 * +0xC, the human AI interface 0x403770: its +0xC IsAI is false), and [esp+0x13], 0x410D40(0x919650)'s +0xC is 1
 * (or it is null) and the byte [0x911054] is 0.  The hooks count the reads, the AI path, the case and the toggle's
 * call; `breaker watch` logs them with the gates whenever one changes, and each GAMEBREAKER case. */
#define HOOK_FETCH       HOOK_SITE(0x006a8870)   /* InputPlayer's FetchInput, the entry */
#define HOOK_FETCH_AI    HOOK_SITE(0x006a88ba)   /* ... the car's AI path: the queue is not read */
#define HOOK_GB_CASE     HOOK_SITE(0x006a8a5d)   /* ... the GAMEBREAKER case */
#define HOOK_GB_TOGGLE   HOOK_SITE(0x006a8a75)   /* ... its two flags set: the toggle follows */
static uint32_t in_player;              /* the last FetchInput's this */
static unsigned long n_fetch, n_fetch_ai, n_gb_case, n_gb_toggle;
static int breaker_hook(CPU *c, uint32_t va){
    if (va == HOOK_FETCH){ in_player = c->ecx; n_fetch++; return 1; }
    if (va == HOOK_FETCH_AI){ n_fetch_ai++; return 1; }
    if (va == HOOK_GB_CASE || va == HOOK_GB_TOGGLE){
        if (va == HOOK_GB_CASE) n_gb_case++; else n_gb_toggle++;
        if (bw_on)
            fprintf(stderr, "[mwbreaker] f=%ld GAMEBREAKER action: %s (id %u; no AI %u, mode %u, 0x14 %u; queue %u)\n",
                    autotest_frame(), va == HOOK_GB_CASE ? "the case" : "the toggle is called",
                    rd(rd(c->esp + 0x1cu)), MR8(c->esp + 0x12u), MR8(c->esp + 0x13u), MR8(c->esp + 0x14u),
                    rd(c->esi + 0x10u));
        return 1;
    }
    return 0;
}
static uint32_t bi_last[14];
static void log_breaker_input(CPU *c, int only_changes){
    uint32_t ip = in_player;
    if (!ip) return;
    uint32_t a8 = rd(ip + 8u), hai = rd(ip + 0xcu), m = gcall(c, 0x00410d40u, 0x00919650u, 0, 0);
    uint32_t q = ip + 0x10u;            /* the queue (in the list [0x920E4C], count [0x920E54]): +0x268 its port, +0x274
                                         * its action map, +0x270 the device binding; 0x633D10 makes it active when +0x26C
                                         * > 0, the port >= 0, the byte +0x280 is set and it has a binding, and keeps in
                                         * +0x288 the time ([0x925B14]) it became active (0 while not) */
    uint32_t v[14] = { rd(q), MR8(ip + 0x2b8u), a8 ? gcall(c, rd(rd(a8) + 0x74u), a8, 0, 0) & 0xffu : 9u,
                       hai ? gcall(c, rd(rd(hai) + 0xcu), hai, 0, 0) & 0xffu : 9u, m ? rd(m + 0xcu) : 0xffffffffu,
                       MR8(0x00911054u), (uint32_t)n_gb_case, (uint32_t)n_gb_toggle, rd(q + 0x268u), rd(q + 0x274u),
                       rd(q + 0x270u), rd(q + 0x26cu), MR8(q + 0x280u), rd(q + 0x288u) };
    if (only_changes && !memcmp(v, bi_last, sizeof v)) return;
    memcpy(bi_last, v, sizeof v);
    fprintf(stderr, "[mwbreaker] f=%ld input: queue %u (port %d, map %08x, binding %08x, +0x26c %d, +0x280 %u, active since %u, "
            "now %u), blocked %u, ai path %u (+8 %08x), IsAI %u (+0xC %08x), mode %d, [0x911054] %u; reads %lu, on the AI "
            "path %lu; GAMEBREAKER cases %lu, toggles %lu\n", autotest_frame(), v[0], (int)v[8], v[9], v[10], (int)v[11], v[12],
            v[13], rd(0x00925b14u), v[1], v[2], a8, v[3], hai, (int)v[4], v[5], n_fetch, n_fetch_ai, n_gb_case, n_gb_toggle);
}
static void log_breaker(CPU *c, int only_changes){
    uint32_t pl = player_iface();
    log_breaker_input(c, only_changes);
    int on = pl ? MR8(pl + PL_BREAKER_ON) : -1, can = 0;
    float ch = pl ? rdf(pl + PL_BREAKER) : 0.0f;
    int tenth = (int)floorf(ch * 10.0f + 1e-4f), blk = pl ? breaker_blocks(c, pl, &can) : 0;
    if (pl && (!only_changes || (blk & ~4) != bw_last[2] || can != bw_last[3]))   /* its own line: the watch's */
        fprintf(stderr, "[mwbreaker] f=%ld can %d blocks %d\n", autotest_frame(), can, blk);  /* tenths stay apart */
    bw_last[2] = blk & ~4; bw_last[3] = can;
    if (only_changes && on == bw_last[0] && tenth == bw_last[1]) return;
    bw_last[0] = on; bw_last[1] = tenth;
    if (!pl){ fprintf(stderr, "[mwbreaker] f=%ld no player\n", autotest_frame()); return; }
    fprintf(stderr, "[mwbreaker] f=%ld %s charge %.3f speed %.1f mph\n", autotest_frame(), on ? "on" : "off", ch,
            player_mph(c));
}
/* breaker wait: v[0] 1 on, 0 off, 2 full, 3 empty, 4 the car above x mph, 5 the charge at x or more, 6 the charge
 * x above where it was when the wait began (from, at most full) */
static int breaker_met(CPU *c, int want, float x, float from){
    uint32_t pl = player_iface();
    if (!pl) return 0;
    int on = MR8(pl + PL_BREAKER_ON); float ch = rdf(pl + PL_BREAKER);
    return want == 1 ? on : want == 0 ? !on : want == 2 ? ch >= 1.0f : want == 3 ? ch <= 0.0f
         : want == 4 ? player_mph(c) > x : want == 5 ? ch >= x : ch >= fminf(from + x, 1.0f) - 1e-4f;
}
static struct { int op, done, rel; float v[6]; long since; char text[96]; } Q;

/* NFSMW_HFR_TRACE=1 (test only, M7-21; tests/mw/scripts/hfr_race.txt, tools/mw/hfr_ab.py): from GO (the player's
 * race timer running) an [hfrtr] line every 0.25 s of simulation time (steps x step, so runs at different
 * rates line up): the racers' share of the race (GRacerInfo +0x18), position and speed of the first four
 * simables of the list at 0x90D664 (the racers, the player first), and the speed of every vehicle of the
 * list at 0x92D0DC (racers first, then traffic).  Both lists are {vtable, base, capacity, count} with the
 * entries at base (seen in a race: simables 4 of 20, vehicles 4 of 160 before the traffic came). */
#define G_SIMLIST 0x0090d664u
#define G_VEHLIST 0x0092d0dcu
static void hfr_trace(CPU *c){
    static int on = -1; static long last = -1;
    if (on < 0){ const char *e = getenv("NFSMW_HFR_TRACE"); on = e && *e == '1'; }
    static uint32_t go_st = 0, go_sim = 0; static int go = 0;
    if (!on || game_flow() != 6u){ go = 0; return; }
    uint32_t sim = rd(0x009203b0u), rs = rd(G_RACESTATUS);
    if (!sim || !rs) return;
    uint32_t st = rd(sim + 0x28u); float P = rdf(sim + 0x20u);
    if (!go || go_sim != sim){         /* GO: the player's race timer runs (race_frames.txt) */
        if (!MR8(rs + 0x16cu)) return;
        go = 1; go_sim = sim; go_st = st; last = -1;
        fprintf(stderr, "[hfrtr] GO f=%ld st=%u step %.6f\n", autotest_frame(), st, (double)P);
    }
    double t = (double)(st - go_st) * (double)P;
    long k = (long)floor(t * 4.0 + 1e-6);
    if (k == last) return;
    last = k;
    char b[8192]; int o = 0;
    o += snprintf(b + o, sizeof b - (size_t)o, "[hfrtr] f=%ld t=%.4f st=%u", autotest_frame(), t, st);
    for (int i = 0; i < (int)rd(rs + RS_NRACERS) && i < 8; i++)
        o += snprintf(b + o, sizeof b - (size_t)o, " r%d=%.3f", i, rdf(rs + RS_RACERS + (uint32_t)i * RACER_SIZE + RI_PCT));
    uint32_t sb = rd(G_SIMLIST), sn = rd(G_SIMLIST + 8u), vb = rd(G_VEHLIST), vn = rd(G_VEHLIST + 8u);
    o += snprintf(b + o, sizeof b - (size_t)o, " ns=%u nv=%u", sn, vn);
    for (uint32_t i = 0; sb && i < sn && i < 4; i++){          /* the racers: position, speed */
        uint32_t s = rd(sb + 4u * i);
        uint32_t p = s ? gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0) : 0;
        uint32_t v = s ? gcall(c, rd(rd(s) + SIM_VEHICLE), s, 0, 0) : 0;
        float sp = v ? gcall_f(c, rd(rd(v) + VEH_SPEED), v) : -1.0f;
        o += snprintf(b + o, sizeof b - (size_t)o, " %x:%.2f,%.2f,%.2f,%.3f", s, p ? rdf(p) : 0, p ? rdf(p + 4u) : 0,
                      p ? rdf(p + 8u) : 0, sp);
    }
    o += snprintf(b + o, sizeof b - (size_t)o, " |");
    for (uint32_t i = 0; vb && i < vn && i < 160 && o < (int)sizeof b - 32; i++){   /* every vehicle: speed */
        uint32_t v = rd(vb + 4u * i);
        o += snprintf(b + o, sizeof b - (size_t)o, " %x=%.2f", v, v ? gcall_f(c, rd(rd(v) + VEH_SPEED), v) : -1.0f);
    }
    fprintf(stderr, "%s\n", b);
}

/* NFSU2_CAR_TRACE=<file> (test only; lan-pair sets it, tools/lan_trace.py reads it): UG2's CSV (debug_ug2.c car_trace)
 * for Most Wanted, one row per pass of the main loop while racing (GameFlow 6): wall-clock time (CLOCK_REALTIME),
 * frame, hud (1 while the race runs: GRaceStatus active and the first racer's timer going), cap (the sim step P,
 * [0x9203B0]+0x20), worldtimer (the sim's steps x P in 1/4000 s: the game time simulated), realtimer (RealTimer
 * [0x925B14], 1/4000 s), then per simable of the list at 0x90D664 (the racers: in a LAN race the players' cars)
 * whether it is the local player's, its position (ISimable +0x60; Y is the height) and its speed in m/s (vehicle
 * +0x2C).  With it, a [mwrace] racestatus line every RS_EVERY frames while racing and one at every change of the
 * race's flags (active, GO, the local racer finished, won), so a log shows where each race stood. */
#define RS_EVERY 600
static FILE *g_ctrace;
static void car_trace(CPU *c){
    static int init; static long last_rs = -1; static uint32_t last_flags = 0xffffffffu, last_list;
    if (!init){
        init = 1;
        const char *ct = getenv("NFSU2_CAR_TRACE");
        if (ct && *ct && (g_ctrace = fopen(ct, "w"))){
            setvbuf(g_ctrace, 0, _IOLBF, 0);
            fprintf(g_ctrace, "t,frame,hud,cap,worldtimer,realtimer,n");
            for (int i = 0; i < 8; i++) fprintf(g_ctrace, ",c%dlocal,c%dx,c%dy,c%dz,c%dv", i, i, i, i, i);
            fputc('\n', g_ctrace);
        }
    }
    if (!g_ctrace) return;
    long f = autotest_frame();
    if (game_flow() != 6u){ last_rs = -1; last_flags = 0xffffffffu; return; }
    uint32_t rs = rd(G_RACESTATUS), sim = rd(0x009203b0u), me = player_simable(c);
    int active = rs ? MR8(rs + RS_ACTIVE) : 0, go = rs ? MR8(rs + RS_GO) != 0 : 0, won = rs ? MR8(rs + RS_WON) : 0;
    int idx = -1, fin = 0;
    if (rs && me){ uint32_t a = me; idx = (int)gcall(c, FN_RACER_INDEX, 0, 1, &a); }
    if (rs && idx >= 1 && idx <= 16) fin = MR8(rs + RS_RACERS + (uint32_t)(idx - 1) * RACER_SIZE + RI_FINISHED);
    uint32_t flags = (uint32_t)active | (uint32_t)go << 1 | (uint32_t)fin << 2 | (uint32_t)won << 3;
    if (flags != last_flags || last_rs < 0 || f - last_rs >= RS_EVERY){
        if (flags != last_flags)
            fprintf(stderr, "[mwrace] f=%ld race: active %d go %d player finished %d won %d\n", f, active, go, fin, won);
        log_status(c);
        last_flags = flags; last_rs = f;
    }
    uint32_t sb = rd(G_SIMLIST), sn = rd(G_SIMLIST + 8u); if (sn > 8) sn = 8;
    if (sb != last_list){
        last_list = sb;
        fprintf(stderr, "[cartrace] f=%ld simables %u, local 0x%08x:", f, sn, me);
        for (uint32_t i = 0; sb && i < sn; i++) fprintf(stderr, " 0x%08x", rd(sb + 4u * i));
        fputc('\n', stderr);
    }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    double P = sim ? (double)rdf(sim + 0x20u) : 0.0;
    fprintf(g_ctrace, "%lld.%06ld,%ld,%d,%.7f,%lld,%u,%u", (long long)ts.tv_sec, (long)(ts.tv_nsec / 1000), f,
            active && go, P, sim ? (long long)llround((double)rd(sim + 0x28u) * P * 4000.0) : 0LL, rd(0x00925b14u),
            sb ? sn : 0u);
    for (uint32_t i = 0; sb && i < sn; i++){
        uint32_t s = rd(sb + 4u * i);
        uint32_t p = s ? gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0) : 0;
        uint32_t v = s ? gcall(c, rd(rd(s) + SIM_VEHICLE), s, 0, 0) : 0;
        if (p && mem_is_committed(p + 11u))
            fprintf(g_ctrace, ",%d,%.3f,%.3f,%.3f,%.3f", s == me, (double)rdf(p), (double)rdf(p + 4u), (double)rdf(p + 8u),
                    v ? (double)gcall_f(c, rd(rd(v) + VEH_SPEED), v) : 0.0);
        else fprintf(g_ctrace, ",,,,,");
    }
    fputc('\n', g_ctrace);
}

/* ---- what the career bot (bot_mw.c) shares ---- */
uint32_t mw_aid_call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args){ return gcall(c, fn, self, n, args); }
uint32_t mw_aid_simable(CPU *c){ return player_simable(c); }
uint32_t mw_aid_pursuit(CPU *c, uint32_t *perp){ return pursuit_obj(c, perp); }
/* the local player's car: its position (simulation space) and speed; 0 = no car */
int mw_aid_where(CPU *c, float pos[3], float *kmh){
    uint32_t s = player_simable(c);
    uint32_t p = s ? gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0) : 0u;
    if (!p || !mem_is_committed(p + 11u)) return 0;
    for (int k = 0; k < 3; k++) pos[k] = rdf(p + 4u * (uint32_t)k);
    if (kmh){
        uint32_t v = gcall(c, rd(rd(s) + SIM_VEHICLE), s, 0, 0);
        *kmh = v ? gcall_f(c, rd(rd(v) + VEH_SPEED), v) * 3.6f : 0.0f;
    }
    return 1;
}
/* the car moved to pos facing dir the way Game.WarpPlayerToTrigger moves it (interface 0x4040E0, +0x68); 0 = no car */
int mw_aid_teleport(CPU *c, const float pos[3], const float dir[3]){
    uint32_t s = player_simable(c);
    uint32_t obj = s ? rd(s + 4u) : 0u, id = IID_WARP;
    uint32_t w = obj ? gcall(c, FN_QUERY, obj, 1, &id) : 0u;
    if (!w) return 0;
    c->esp -= 0x20u;                    /* the two vectors below the game's stack, the call below them */
    uint32_t at = c->esp;
    memcpy(g_mem + at, pos, 12); memcpy(g_mem + at + 12u, dir, 12);
    uint32_t args[2] = { at, at + 12u };
    gcall(c, rd(rd(w) + WARP_METHOD), w, 2, args);
    c->esp += 0x20u;
    U.since = 0;                        /* the wedge watch starts again from here */
    return 1;
}
/* the autopilot: 0 off (Game.ClearAIControl, and no more races taken at their GO), 1 on (Game.ForceAIControl now),
 * 2 go (the AI takes each race at its GO), 3 races (as 2, with a lost race raced again: watch_race), 4 the AI lets the
 * car go now (Game.ClearAIControl) and a race already taken at its GO stays so (the career bot between races) */
void mw_aid_autopilot(CPU *c, int mode){
    long f = autotest_frame();
    uint32_t zero = 0;
    if (mode <= 1 || mode == 4){
        gcall(c, mode == 1 ? FN_FORCE_AI : FN_CLEAR_AI, 0, 1, &zero);
        if (!mode) auto_ai = 0;
        ai_on = mode == 1; U.since = 0; U.futile = 0;
        U.sim = ai_on ? player_simable(c) : 0u;
        fprintf(stderr, "[mwrace] f=%ld autopilot %s\n", f, mode == 1 ? "on" : mode == 4 && auto_ai ? "off until the next GO" : "off");
        return;
    }
    memset(&W, 0, sizeof W);            /* a race at its GO now is taken at the next anchor */
    auto_ai = mode == 2 ? 2 : 1;
    fprintf(stderr, "[mwrace] f=%ld autopilot %s: the AI takes each race at its GO\n", f, auto_ai == 2 ? "go" : "races");
}

void mw_frame(CPU *c){
    if (auto_ai) watch_race(c);
    if (auto_ai || ai_on) wedge_watch(c);
    mw_bot_frame(c);
    mw_pursuit_frame(c);
    hfr_trace(c);
    car_trace(c);
    if (pw_on) log_pursuit(c, 1);
    if (bw_on) log_breaker(c, 1);
    if (Q.op == Q_NONE || Q.done) return;
    long f = autotest_frame();
    switch (Q.op){
    case Q_RACEWIN: race_win(c); break;
    case Q_STATUS: log_status(c); break;
    case Q_AI_ON: mw_aid_autopilot(c, 1); break;
    case Q_AI_OFF: mw_aid_autopilot(c, 0); break;
    case Q_AI_RACES: mw_aid_autopilot(c, 3); break;
    case Q_AI_GO: mw_aid_autopilot(c, 2); break;
    case Q_BOT: mw_bot_cmd(c, Q.text); break;
    case Q_RESET: {                     /* the player's RESET, as the wedge watch does it */
        log_where(c);
        int ok = mw_reset_car(c);
        fprintf(stderr, "[mwrace] f=%ld carreset: %s\n", f, ok > 0 ? "done" : ok == 0 ? "the game found no place" : "no car");
        log_where(c);
        break; }
    case Q_BOTWAIT: {                   /* done once the bot has stopped, or after v[1] frames */
        int idle = mw_bot_idle();
        if (!idle && f - Q.since < (long)Q.v[1]) return;
        fprintf(stderr, "[mwbot] f=%ld bot wait: %s after %ld frames\n", f, idle ? "the bot stopped" : "TIMEOUT", f - Q.since);
        break; }
    case Q_TELEPORT: {
        uint32_t s = player_simable(c);
        float pos[3] = { Q.v[0], Q.v[1], Q.v[2] }, dir[3] = { Q.v[3], Q.v[4], Q.v[5] };
        if (s && Q.rel){                /* ~N: N from where the car is */
            uint32_t p = gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0);
            for (int k = 0; k < 3; k++) if (Q.rel & (1 << k)) pos[k] += rdf(p + 4u * (uint32_t)k);
        }
        if (!mw_aid_teleport(c, pos, dir)){ fprintf(stderr, "[mwrace] f=%ld teleport: no player car\n", f); break; }
        fprintf(stderr, "[mwrace] f=%ld teleport to %.2f %.2f %.2f facing %.2f %.2f %.2f\n", f,
                pos[0], pos[1], pos[2], dir[0], dir[1], dir[2]);
        log_where(c);
        break; }
    case Q_WHERE: log_where(c); break;
    case Q_RAP: mw_aid_rapsheet(c, "asked"); break;
    case Q_ICONS: mw_aid_icons(c, (int)Q.v[0]); break;
    case Q_ICONGO: {                    /* the car to icon v[1] of type v[0], at its place, facing v[2..4] */
        float pos[3], dir[3] = { Q.v[2], Q.v[3], Q.v[4] };
        if (!mw_aid_icon_place((int)Q.v[0], (int)Q.v[1], pos)){
            fprintf(stderr, "[mwicon] f=%ld icon go: no icon %d of type %d\n", f, (int)Q.v[1], (int)Q.v[0]); break; }
        float sim[3];
        icon_to_sim(pos, sim);
        fprintf(stderr, "[mwicon] f=%ld icon go: icon %d of type %d at %.1f %.1f %.1f\n", f, (int)Q.v[1], (int)Q.v[0],
                sim[0], sim[1], sim[2]);
        if (!mw_aid_teleport(c, sim, dir)) fprintf(stderr, "[mwicon] f=%ld icon go: no player car\n", f);
        log_where(c);
        break; }
    case Q_POOLS: mw_aid_pools(c, (int)Q.v[0]); break;
    case Q_PURSUIT: log_pursuit(c, 0); break;
    case Q_PWAIT: {                     /* done once the pursuit is there (v[0] 1) or gone (0), or a pursuit breaker came
                                         * down since the wait began (2: more than v[2] of them), or after v[1] frames */
        if (Q.v[0] == 2.0f && Q.v[2] < 0.0f) Q.v[2] = (float)mw_pursuit_breakers();
        int on = Q.v[0] == 2.0f ? ((float)mw_pursuit_breakers() > Q.v[2] ? 2 : 0) : pursuit_obj(c, 0) != 0;
        if (on != (int)Q.v[0] && f - Q.since < (long)Q.v[1]) return;
        fprintf(stderr, "[mwpursuit] f=%ld pursuit wait %s: %s after %ld frames\n", f,
                Q.v[0] == 2.0f ? "breaker" : Q.v[0] ? "on" : "off", on == (int)Q.v[0] ? "satisfied" : "TIMEOUT",
                f - Q.since);
        log_pursuit(c, 0);
        break; }
    case Q_PWATCH: pw_on = 1; memset(pw_last, 0xff, sizeof pw_last); break;
    case Q_BREAKER: log_breaker(c, 0); break;
    case Q_BWATCH: bw_on = 1; bw_last[0] = bw_last[1] = bw_last[2] = bw_last[3] = -2; log_breaker(c, 1); break;
    case Q_BWAIT: {                     /* done once v[0] holds (breaker_met), or after v[1] frames */
        static const char *const nm[] = { "off", "on", "full", "empty", "above", "charge", "rise" };
        int met = breaker_met(c, (int)Q.v[0], Q.v[2], Q.v[3]);
        if (!met && f - Q.since < (long)Q.v[1]) return;
        fprintf(stderr, "[mwbreaker] f=%ld breaker wait %s: %s after %ld frames\n", f, nm[(int)Q.v[0] % 7],
                met ? "satisfied" : "TIMEOUT", f - Q.since);
        log_breaker(c, 0);
        break; }
    }
    Q.done = 1;
}

int mw_aid_is_cmd(const char *cmd){
    static const char *const k[] = { "racewin", "autopilot", "teleport", "where", "racestatus", "frames", "pursuit",
                                     "breaker", "bot", "carreset", "rapsheet", "icons", "pools", 0 };
    for (int i = 0; k[i]; i++) if (!strcasecmp(cmd, k[i])) return 1;
    return 0;
}

/* line = "<cmd> <args>"; 1 = done, 0 = call again next frame (the queued work has not run yet) */
int mw_aid_cmd(CPU *c, const char *line, long *wait_start){
    (void)c;
    char cmd[32] = {0}, a1[32] = {0}; int used = 0;
    if (sscanf(line, "%31s %n", cmd, &used) < 1) return 1;
    const char *p = line + used;
    long f = autotest_frame();
    if (!strcasecmp(cmd, "frames")){
        fprintf(stderr, "[mwrace] f=%ld frames: presents %u main-loop passes %llu flow %u\n", f,
                com_present_no(), (unsigned long long)debug_frame_passes(), game_flow());
        return 1;
    }
    if (Q.op != Q_NONE){                /* this line's work was queued: done once the anchor ran it */
        if (Q.done){ Q.op = Q_NONE; Q.done = 0; *wait_start = -1; return 1; }
        if (*wait_start < 0) *wait_start = f;
        if (f - *wait_start < AID_WAIT + (Q.op == Q_PWAIT || Q.op == Q_BWAIT || Q.op == Q_BOTWAIT ? (long)Q.v[1] : 0L)) return 0;
        fprintf(stderr, "[mwrace] f=%ld %s TIMEOUT after %ld frames: the main loop's frame anchor never ran it\n",
                f, cmd, f - *wait_start);
        memset(&Q, 0, sizeof Q); *wait_start = -1;
        return 1;
    }
    memset(&Q, 0, sizeof Q);
    if (!strcasecmp(cmd, "racewin")) Q.op = Q_RACEWIN;
    else if (!strcasecmp(cmd, "where")) Q.op = Q_WHERE;
    else if (!strcasecmp(cmd, "racestatus")) Q.op = Q_STATUS;
    else if (!strcasecmp(cmd, "carreset")) Q.op = Q_RESET;
    else if (!strcasecmp(cmd, "rapsheet")) Q.op = Q_RAP;
    else if (!strcasecmp(cmd, "pools")){ sscanf(p, "%31s", a1); Q.op = Q_POOLS; Q.v[0] = !strcasecmp(a1, "watch"); }
    else if (!strcasecmp(cmd, "icons")){
        /* icons [T] | icons go T K DX DY DZ: the car to icon K of type T, facing DX DY DZ */
        int t = -1, k = 0; float d[3] = { 1.0f, 0.0f, 0.0f };
        if (sscanf(p, "%31s", a1) == 1 && !strcasecmp(a1, "go")){
            sscanf(p, "%*s %d %d %f %f %f", &t, &k, &d[0], &d[1], &d[2]);
            Q.op = Q_ICONGO; Q.v[0] = (float)t; Q.v[1] = (float)k; Q.v[2] = d[0]; Q.v[3] = d[1]; Q.v[4] = d[2];
        } else { sscanf(p, "%d", &t); Q.op = Q_ICONS; Q.v[0] = (float)t; }
    }
    else if (!strcasecmp(cmd, "pursuit")){
        long n = 6000; char a2[32] = {0};
        sscanf(p, "%31s %31s %ld", a1, a2, &n);
        Q.op = !strcasecmp(a1, "watch") ? Q_PWATCH : !strcasecmp(a1, "wait") ? Q_PWAIT : Q_PURSUIT;
        Q.v[0] = !strcasecmp(a2, "breaker") ? 2.0f : strcasecmp(a2, "off") ? 1.0f : 0.0f; Q.v[1] = (float)n; Q.since = f;
        Q.v[2] = -1.0f;                 /* breaker: the count when the wait begins, read at the anchor */
    }
    else if (!strcasecmp(cmd, "breaker")){
        /* breaker [watch] | breaker wait on|off|full|empty [N] | breaker wait above MPH [N] | breaker wait charge X [N]
         * | breaker wait rise X [N] (the charge X above where it is now, or full) */
        long n = 6000; char a2[32] = {0}; float x = 0.0f; int used2 = 0, used3 = 0;
        if (sscanf(p, "%31s %31s %n", a1, a2, &used2) == 2){
            const char *q = p + used2;
            if (!strcasecmp(a2, "above") || !strcasecmp(a2, "charge") || !strcasecmp(a2, "rise")){
                sscanf(q, "%f %n", &x, &used3); q += used3; }
            sscanf(q, "%ld", &n);
        }                               /* (one word: a1 holds it, "watch" or nothing) */
        Q.op = !strcasecmp(a1, "watch") ? Q_BWATCH : !strcasecmp(a1, "wait") ? Q_BWAIT : Q_BREAKER;
        Q.v[0] = !strcasecmp(a2, "off") ? 0.0f : !strcasecmp(a2, "full") ? 2.0f : !strcasecmp(a2, "empty") ? 3.0f
               : !strcasecmp(a2, "above") ? 4.0f : !strcasecmp(a2, "charge") ? 5.0f : !strcasecmp(a2, "rise") ? 6.0f : 1.0f;
        Q.v[1] = (float)n; Q.v[2] = x; Q.since = f;
        { uint32_t pl = player_iface(); Q.v[3] = pl ? rdf(pl + PL_BREAKER) : 0.0f; }   /* rise: from here */
    }
    else if (!strcasecmp(cmd, "bot")){
        long n = 900000;
        if (sscanf(p, "%31s %ld", a1, &n) >= 1 && !strcasecmp(a1, "wait")){ Q.op = Q_BOTWAIT; Q.v[1] = (float)n; Q.since = f; }
        else { Q.op = Q_BOT; snprintf(Q.text, sizeof Q.text, "%s", p); }
    }
    else if (!strcasecmp(cmd, "autopilot")){
        sscanf(p, "%31s", a1);
        Q.op = !strcasecmp(a1, "off") ? Q_AI_OFF : !strcasecmp(a1, "races") ? Q_AI_RACES : !strcasecmp(a1, "go") ? Q_AI_GO
             : Q_AI_ON;
    } else if (!strcasecmp(cmd, "teleport")){
        char t[6][24]; int used2 = 0, k = 0;
        while (k < 6 && sscanf(p, "%23s %n", t[k], &used2) == 1){ p += used2; k++; }
        if (k != 6){ fprintf(stderr, "[mwrace] teleport: X Y Z DX DY DZ (X, Y, Z may be ~N)\n"); return 1; }
        for (int i = 0; i < 6; i++){
            const char *v = t[i];
            if (i < 3 && *v == '~'){ Q.rel |= 1 << i; v++; }
            Q.v[i] = *v ? strtof(v, 0) : 0.0f;
        }
        Q.op = Q_TELEPORT;
    }
    *wait_start = f;                    /* queued: the wait for the anchor starts now */
    return 0;
}
