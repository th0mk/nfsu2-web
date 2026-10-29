/* Most Wanted's test-only race aids (TRACKER M4-02; UG2's are runtime/career.c).  Scripted input drives the
 * menus but not the car, so an automated race run needs help.  Nothing here is for players: every aid waits
 * for a script line (runtime/autotest.c hands this file, through debug_mw.c, the commands below), and no
 * player's page sends a script.  As in UG2 the aids let the game's own code run (its AI driver, its finish
 * path, its own car warp) and never edit race or career state (L-022).
 *
 * Where they run: a command is queued and runs at the game's per-frame anchor (game.h on_frame, game_mw.c
 * MW_FRAME_SITE: MainLoop between its update and its render), on the game's thread, with the CPU state put
 * back afterwards (com.c call_guest); a command waits (autotest's next frames) until its work has run.
 *
 *   racewin          finish the local player now, as the race's script does when a racer crosses the
 *                    finish line: Game.NotifyRaceFinished(racer), then, when Game.AllHumanPlayersDone(),
 *                    Game.EnterPostRaceFlow() (the game's own finish path; the script's calls at the line,
 *                    seen with NFSMW_HOOKLOG on the Lua bindings while the AI drove the player over it, are
 *                    NotifyCheckpointReached, NotifyLapFinished, SetRacerLapsLeft(n, 0), CalculateRanking,
 *                    NotifyRaceFinished, NotifyRacePlacement, RacerIsHuman, AllHumanPlayersDone,
 *                    EnterPostRaceFlow; the lap bookkeeping before and the placement message after are left
 *                    out)
 *   autopilot on|off the game's AI drives the local player's car (Game.ForceAIControl / ClearAIControl)
 *   teleport X Y Z DX DY DZ   move the local player's car to X Y Z (~N: N from where it is), facing DX DY DZ
 *                    (the game's simulation space, as where logs it: Y is the height, and the warp sets
 *                    the car on the surface at X, Z: ~40 ~1 ~0 moved it 40 m along X and left it at Y 163.5
 *                    for 146.5 asked, ~0 ~5 ~0 did not lift it), the
 *                    way Game.WarpPlayerToTrigger moves it (its ISimable's interface 0x4040E0, method +0x68)
 *   where            log the local player's position, heading and speed
 *   racestatus       log the race: its racers' progress and finish flags, its own flags
 *   frames           log the Presents and the main loop's passes so far (M4-01: one pass a Present)
 *
 * The game's objects, from speed.exe v1.3's code (GAME.md 13.4; the names are its Lua bindings', registered
 * with their strings from 0x61E900 on):
 *   GRaceStatus, [0x91E000]: its racers (GRacerInfo, 0x194 bytes each) from +0x18, their count at +0x1958;
 *     +0x198A (byte) actively racing, written by SetActivelyRacing (0x604050); +0x4554 (byte) has been won
 *     (SetHasBeenWon 0x604080).  GetRacerInfo(i) is 0x5DBED0 (thiscall, 0-based).
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
 * value is read inside it */
static float gcall_f(CPU *c, uint32_t fn, uint32_t self){
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

static void log_status(void){
    uint32_t rs = rd(G_RACESTATUS);
    if (!rs){ fprintf(stderr, "[mwrace] f=%ld racestatus: no GRaceStatus\n", autotest_frame()); return; }
    int n = (int)rd(rs + RS_NRACERS);
    char b[640]; int o = 0;
    for (int i = 0; i < n && i < 16 && o < (int)sizeof b - 48; i++){
        uint32_t ri = rs + RS_RACERS + (uint32_t)i * RACER_SIZE;
        o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.1f%%%s %.2fs", i, rdf(ri + RI_PCT),
                      MR8(ri + RI_FINISHED) ? " finished" : "", rdf(ri + RI_TIME));
    }
    fprintf(stderr, "[mwrace] f=%ld racestatus flow %u racers %d active %u won %u:%s\n", autotest_frame(),
            game_flow(), n, MR8(rs + RS_ACTIVE), MR8(rs + RS_WON), o ? b : " none");
}

/* ---- the queue: one piece of work at a time, run at the next frame anchor ---- */
enum { Q_NONE, Q_RACEWIN, Q_AI_ON, Q_AI_OFF, Q_TELEPORT, Q_WHERE };
static struct { int op, done, rel; float v[6]; } Q;

void mw_frame(CPU *c){
    if (Q.op == Q_NONE || Q.done) return;
    long f = autotest_frame();
    switch (Q.op){
    case Q_RACEWIN: {
        uint32_t s = player_simable(c), a = s;
        int idx = s ? (int)gcall(c, FN_RACER_INDEX, 0, 1, &a) : -1;
        if (game_flow() != g_game.flow_race || idx < 1){
            fprintf(stderr, "[mwrace] f=%ld racewin: not in a race (flow %u, racer %d)\n", f, game_flow(), idx);
            break; }
        uint32_t arg = s;
        gcall(c, FN_NOTIFY_FINISHED, 0, 1, &arg);
        int all = (int)(gcall(c, FN_ALL_HUMANS_DONE, 0, 0, 0) & 0xffu);
        if (all) gcall(c, FN_POSTRACE, 0, 0, 0);
        fprintf(stderr, "[mwrace] f=%ld racewin: racer %d finished through NotifyRaceFinished; all humans done %d%s\n",
                f, idx, all, all ? ": EnterPostRaceFlow" : "");
        log_status();
        break; }
    case Q_AI_ON: case Q_AI_OFF: {
        uint32_t zero = 0;
        gcall(c, Q.op == Q_AI_ON ? FN_FORCE_AI : FN_CLEAR_AI, 0, 1, &zero);
        fprintf(stderr, "[mwrace] f=%ld autopilot %s\n", f, Q.op == Q_AI_ON ? "on" : "off");
        break; }
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
    }
    Q.done = 1;
}

int mw_aid_is_cmd(const char *cmd){
    static const char *const k[] = { "racewin", "autopilot", "teleport", "where", "racestatus", "frames", 0 };
    for (int i = 0; k[i]; i++) if (!strcasecmp(cmd, k[i])) return 1;
    return 0;
}

/* line = "<cmd> <args>"; 1 = done, 0 = call again next frame (the queued work has not run yet) */
int mw_aid_cmd(CPU *c, const char *line, long *wait_start){
    (void)c; (void)wait_start;
    char cmd[32] = {0}, a1[32] = {0}; int used = 0;
    if (sscanf(line, "%31s %n", cmd, &used) < 1) return 1;
    const char *p = line + used;
    if (!strcasecmp(cmd, "racestatus")){ log_status(); return 1; }
    if (!strcasecmp(cmd, "frames")){
        fprintf(stderr, "[mwrace] f=%ld frames: presents %u main-loop passes %llu flow %u\n", autotest_frame(),
                com_present_no(), (unsigned long long)debug_frame_passes(), game_flow());
        return 1;
    }
    if (Q.op != Q_NONE){                /* this line's work was queued: done once the anchor ran it */
        if (!Q.done) return 0;
        Q.op = Q_NONE; Q.done = 0;
        return 1;
    }
    memset(&Q, 0, sizeof Q);
    if (!strcasecmp(cmd, "racewin")) Q.op = Q_RACEWIN;
    else if (!strcasecmp(cmd, "where")) Q.op = Q_WHERE;
    else if (!strcasecmp(cmd, "autopilot")){
        sscanf(p, "%31s", a1);
        Q.op = !strcasecmp(a1, "off") ? Q_AI_OFF : Q_AI_ON;
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
    return 0;
}
