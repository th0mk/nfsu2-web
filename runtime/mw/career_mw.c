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
 *   autopilot on|off the game's AI drives the local player's car (Game.ForceAIControl / ClearAIControl)
 *   autopilot races  the same at every race's GO from now on, for races that follow each other with no menu
 *                    between (the career's prologue), with the races' edges logged; a lost race is raced again
 *                    the way a player does it (watch_race below); autopilot off ends it
 *   teleport X Y Z DX DY DZ   move the local player's car to X Y Z (~N: N from where it is), facing DX DY DZ
 *                    (the game's simulation space, as where logs it: Y is the height, and the warp sets
 *                    the car on the surface at X, Z: ~40 ~1 ~0 moved it 40 m along X and left it at Y 163.5
 *                    for 146.5 asked, ~0 ~5 ~0 did not lift it), the
 *                    way Game.WarpPlayerToTrigger moves it (its ISimable's interface 0x4040E0, method +0x68)
 *   where            log the local player's position, heading and speed
 *   racestatus       log the race: its racers' progress and finish flags, its own flags, and which racer the
 *                    local player is ("player 1": Game.GetRacerIndex, slot #0)
 *   frames           log the Presents and the main loop's passes so far (M4-01: one pass a Present)
 *   pursuit [watch]  log the player's pursuit state (heat, the pursuit object, its length), once or whenever it
 *                    changes; pursuit wait on|off [N] waits up to N frames for a pursuit to be there or gone
 *                    (the state's addresses: log_pursuit below)
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
    fprintf(stderr, "[mwrace] f=%ld racestatus flow %u racers %d active %u won %u player %d:%s; mode %u\n",
            autotest_frame(), game_flow(), n, MR8(rs + RS_ACTIVE), MR8(rs + RS_WON), me, o ? b : " none",
            rd(rs + RS_MODE));
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
    if (active && fin && !won && !W.lost && !(W.fin && !W.won)) W.lost = f;   /* finished, not won: maybe lost */
    W.rs = rs; W.sim = s; W.active = active; W.go = go; W.fin = fin; W.won = won;
    if (!W.lost || f - W.lost < LOST_WAIT || (f - W.lost) % LOST_EVERY) return;
    const char *what = mw_fe_match("PostRace_Results.fng") ? "the race was lost: Enter on its results"
                     : mw_fe_match("MainMenu_Sub.fng:e072db21@4b4e388a") ? "the career menu: Enter on Resume Career" : 0;
    if (!what) return;
    autotest_tap(DIK_RETURN, 6);
    fprintf(stderr, "[mwrace] f=%ld autopilot: %s\n", f, what);
}

/* ---- the queue: one piece of work at a time, run at the next frame anchor ---- */
#define AID_WAIT 600                    /* frames a queued command waits for the anchor (once a frame in MainLoop) */
enum { Q_NONE, Q_RACEWIN, Q_AI_ON, Q_AI_OFF, Q_AI_RACES, Q_TELEPORT, Q_WHERE, Q_STATUS, Q_PURSUIT, Q_PWATCH, Q_PWAIT };

/* The player's pursuit state (exploration, wave 7: offsets read in runs, their meaning where a run showed it):
 *   IPerpetrator, interface 0x4037E0 of the player's ISimable object (vtable 0x892988): +0x1C the heat, a float
 *     (GetHeat, method +4, 0x409400; SetHeat, method +8, 0x409060, is what Game.SetWorldHeat 0x612660 calls);
 *     +0x20 the bounty (Game.GetPlayerBounty 0x612300 gave the same, 0 before a pursuit and 100 after it);
 *     +0x24 went 0 -> 100 during a pursuit and back to 0 at its end (not identified).
 *   the vehicle's AI interface, 0x403700 (vtable 0x8929F0): +0x70 the pursuit object (vtable 0x892770) from the
 *     pursuit's start (Game.ForcePursuitStart 0x60AAC0) to its end, else 0; the pursuit's +0xB4, a float its
 *     method +0x18 (0x4338C0) returns, grows with the pursuit's length (seconds, inferred).
 * `pursuit` logs these once, `pursuit watch` whenever one changes (each frame anchor), `pursuit wait on|off [N]`
 * waits (at most N frames, default 6000) for a pursuit object to be there, or gone. */
#define IID_PERP         0x004037e0u
#define IID_AIVEH        0x00403700u
#define PERP_HEAT        0x1cu
#define AIVEH_PURSUIT    0x70u
#define PURSUIT_TIME     0xb4u
static int pw_on;
static uint32_t pw_last[5];
static uint32_t pursuit_obj(CPU *c, uint32_t *perp){
    uint32_t s = player_simable(c), obj = s ? rd(s + 4u) : 0u, id = IID_PERP, id2 = IID_AIVEH;
    uint32_t p = obj ? gcall(c, FN_QUERY, obj, 1, &id) : 0u, a = obj ? gcall(c, FN_QUERY, obj, 1, &id2) : 0u;
    if (perp) *perp = p;
    return rd(a + AIVEH_PURSUIT);
}
static void log_pursuit(CPU *c, int only_changes){
    uint32_t p = 0, po = pursuit_obj(c, &p);
    uint32_t v[5] = { rd(p + PERP_HEAT), rd(p + 0x20u), rd(p + 0x24u), po, game_flow() };
    if (only_changes && !memcmp(v, pw_last, sizeof v)) return;
    memcpy(pw_last, v, sizeof v);
    float h, t; memcpy(&h, &v[0], 4); t = rdf(po + PURSUIT_TIME);
    fprintf(stderr, "[mwpursuit] f=%ld heat %.2f pursuit %08x time %.1f bounty %u perp+24 %u flow %u\n",
            autotest_frame(), h, po, po ? t : 0.0f, v[1], v[2], v[4]);
}
static struct { int op, done, rel; float v[6]; long since; } Q;

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

void mw_frame(CPU *c){
    if (auto_ai) watch_race(c);
    hfr_trace(c);
    if (pw_on) log_pursuit(c, 1);
    if (Q.op == Q_NONE || Q.done) return;
    long f = autotest_frame();
    switch (Q.op){
    case Q_RACEWIN: race_win(c); break;
    case Q_STATUS: log_status(c); break;
    case Q_AI_ON: case Q_AI_OFF: {
        uint32_t zero = 0;
        gcall(c, Q.op == Q_AI_ON ? FN_FORCE_AI : FN_CLEAR_AI, 0, 1, &zero);
        if (Q.op == Q_AI_OFF) auto_ai = 0;
        fprintf(stderr, "[mwrace] f=%ld autopilot %s\n", f, Q.op == Q_AI_ON ? "on" : "off");
        break; }
    case Q_AI_RACES:
        memset(&W, 0, sizeof W);        /* a race at its GO now is taken at the next anchor */
        auto_ai = 1;
        fprintf(stderr, "[mwrace] f=%ld autopilot races: the AI takes each race at its GO\n", f);
        break;
    case Q_TELEPORT: {
        uint32_t s = player_simable(c);
        uint32_t obj = s ? rd(s + 4u) : 0u, id = IID_WARP;
        uint32_t w = obj ? gcall(c, FN_QUERY, obj, 1, &id) : 0u;
        if (!w){ fprintf(stderr, "[mwrace] f=%ld teleport: no player car\n", f); break; }
        float pos[3] = { Q.v[0], Q.v[1], Q.v[2] }, dir[3] = { Q.v[3], Q.v[4], Q.v[5] };
        if (Q.rel){                     /* ~N: N from where the car is */
            uint32_t p = gcall(c, rd(rd(s) + SIM_POSITION), s, 0, 0);
            for (int k = 0; k < 3; k++) if (Q.rel & (1 << k)) pos[k] += rdf(p + 4u * (uint32_t)k);
        }
        c->esp -= 0x20u;              /* the two vectors below the game's stack, the call below them */
        uint32_t at = c->esp;
        memcpy(g_mem + at, pos, 12); memcpy(g_mem + at + 12u, dir, 12);
        uint32_t args[2] = { at, at + 12u };
        gcall(c, rd(rd(w) + WARP_METHOD), w, 2, args);
        c->esp += 0x20u;
        fprintf(stderr, "[mwrace] f=%ld teleport to %.2f %.2f %.2f facing %.2f %.2f %.2f\n", f,
                pos[0], pos[1], pos[2], dir[0], dir[1], dir[2]);
        log_where(c);
        break; }
    case Q_WHERE: log_where(c); break;
    case Q_PURSUIT: log_pursuit(c, 0); break;
    case Q_PWAIT: {                     /* done once the pursuit is there (v[0] 1) or gone (0), or after v[1] frames */
        int on = pursuit_obj(c, 0) != 0;
        if (on != (int)Q.v[0] && f - Q.since < (long)Q.v[1]) return;
        fprintf(stderr, "[mwpursuit] f=%ld pursuit wait %s: %s after %ld frames\n", f, Q.v[0] ? "on" : "off",
                on == (int)Q.v[0] ? "satisfied" : "TIMEOUT", f - Q.since);
        log_pursuit(c, 0);
        break; }
    case Q_PWATCH: pw_on = 1; memset(pw_last, 0xff, sizeof pw_last); break;
    }
    Q.done = 1;
}

int mw_aid_is_cmd(const char *cmd){
    static const char *const k[] = { "racewin", "autopilot", "teleport", "where", "racestatus", "frames", "pursuit", 0 };
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
        if (f - *wait_start < AID_WAIT + (Q.op == Q_PWAIT ? (long)Q.v[1] : 0L)) return 0;
        fprintf(stderr, "[mwrace] f=%ld %s TIMEOUT after %ld frames: the main loop's frame anchor never ran it\n",
                f, cmd, f - *wait_start);
        memset(&Q, 0, sizeof Q); *wait_start = -1;
        return 1;
    }
    memset(&Q, 0, sizeof Q);
    if (!strcasecmp(cmd, "racewin")) Q.op = Q_RACEWIN;
    else if (!strcasecmp(cmd, "where")) Q.op = Q_WHERE;
    else if (!strcasecmp(cmd, "racestatus")) Q.op = Q_STATUS;
    else if (!strcasecmp(cmd, "pursuit")){
        long n = 6000; char a2[32] = {0};
        sscanf(p, "%31s %31s %ld", a1, a2, &n);
        Q.op = !strcasecmp(a1, "watch") ? Q_PWATCH : !strcasecmp(a1, "wait") ? Q_PWAIT : Q_PURSUIT;
        Q.v[0] = strcasecmp(a2, "off") ? 1.0f : 0.0f; Q.v[1] = (float)n; Q.since = f;
    }
    else if (!strcasecmp(cmd, "autopilot")){
        sscanf(p, "%31s", a1);
        Q.op = !strcasecmp(a1, "off") ? Q_AI_OFF : !strcasecmp(a1, "races") ? Q_AI_RACES : Q_AI_ON;
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
