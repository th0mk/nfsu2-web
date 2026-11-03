/* career.c -- TEST-ONLY career progression aids (v1.2 NA port).
 *
 * Scripted input can drive the car but cannot win races, so an automated
 * career run needs help.  Nothing here is for players: every aid is off
 * unless an input-script line (autotest.c) or an NFSU2_* variable asks for
 * it.  The aids prefer to let the game's own logic run (its autopilot, its
 * hot-position teleport, its race-finish path) over editing state.
 *
 * Guest calls run inside the game's main loop: GameLoop (0x5811C0) calls the
 * empty function 0x4022C0 once per frame from 0x581470 (return address
 * 0x581475).  The override below runs the queued work there, on the guest
 * thread between two frames, and restores the CPU state afterwards.
 *
 * Script commands (lines of NFSU2_INPUT_SCRIPT, see autotest.c):
 *   call FN [ecx=X] [ARG...]   call guest function FN (thiscall/stdcall/cdecl;
 *                              esp is restored), log eax
 *   memdump X LEN FILE         write LEN bytes of guest memory at X to FILE
 *   waitx X VAL [tmo]          like waitmem, X may dereference
 *   peekx X [N]                log N dwords at X
 *   teleport X Y Z [ANGLE]     move the player's car (the game's own
 *                              hot-position jump, 0x600E10; career free roam)
 *   autopilot on|off [tmo]     the game's Player::AutoPilotOn/Off; off also
 *                              gives the car back to the player's input
 *                              (SetInputMode 2).  "on tmo" first waits up to
 *                              tmo frames until player 1's car has a human
 *                              driver: a network race makes the local player
 *                              one only at its start, and that would take the
 *                              car back from the AI (LAN race tests).  The
 *                              only implementation: the heap track's soaks
 *                              and the LAN race scripts use it too
 *   racewin                    finish the player now (1st if nobody finished)
 *   careerlist                 log stage, bank and the available races; the
 *                              stage's outrun wins, the "other" events open
 *                              (unique parts' stars, cover shoots), the map's
 *                              markers and the other event being run
 *   fnglist [x]                log the active front-end packages (overlays too;
 *                              x: FNGInfo flags and control mask)
 *   waitfe NAME [tmo]          wait until package NAME is active (overlays too)
 *   tapfe KEY NAME [every] [tmo]  tap KEY (RETURN ESCAPE LEFT RIGHT UP DOWN)
 *                              every `every` frames until NAME is active
 *   careergo NAME|next[:TYPE]  teleport onto a career race's start marker
 *                              (NAME as in the race table, e.g. S2_DRAG_2;
 *                              next = the available race the bot would pick)
 *   shopgo NAME                teleport onto a shop's marker (CC_CAR_LOT_1 ...)
 *   showgo NAME                teleport onto a cover shoot's marker (STAGE_2_DVD_1)
 *                              or an other event's (STAGE_3_PERF_1)
 *   waitroam [tmo]             wait until the car is in free roam (2 s settled)
 *   cargo K [BEHIND]           jump behind the K-th car of the car manager
 *   outrun [K] [tmo]           challenge the free-roam tuner in racer slot K
 *                              (1..3, default: the nearest one on the map),
 *                              waiting up to tmo frames for one; when it is
 *                              more than 150 m away, jump behind it first
 *   outrunwin                  win the outrun in progress (RACEWIN does too)
 *   visual                     log the car's visual rating (0x528410: with the
 *                              NFSU2_CAREER_BOT_VISUAL tweak, and its own)
 *   uniques [won]              log the shown car's unique (level-4) visual parts
 *                              per slot and how many the shops' check (0x52A010,
 *                              filter 6) and the garage's (2) let through, and
 *                              whether Caleb is beaten (uniques.c); "won" first
 *                              adds a won record of stage 5's final event
 *   watchw X                   debug: log the guest functions that write X
 *   bot on|off                 the career bot below (also NFSU2_CAREER_BOT=1)
 *   mpcredit NAME[,NAME] [MARGIN]  the wins of those races at the next crib, as
 *                              multiplayer race's (mpcredit.c; MARGIN as the
 *                              game measures it, 100 by default, -1 a friend's)
 * X/ARG/FN are expressions: numbers, "*X" (guest dword at X), "(X)", "+", "-",
 * and "*" between two operands (multiply), e.g. "*(*0x890118+0xbd0)".
 */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/resource.h>

void dispatch_indirect(CPU *c, uint32_t addr);
long autotest_frame(void);
void autotest_tap(unsigned dik, long frames);

/* ---- guest globals (v1.2 NA, verified in our exe) ------------------------ */
#define G_FLOW          0x008654a4u   /* TheGameFlowManager state (6 = in game) */
#define G_PLAYER1       0x008900acu   /* PlayersByNumber[0] (Player*) */
#define G_HOTPOS        0x00890140u   /* hot-position vec3 used by the jump */
#define G_HOTPOS_ANGLE  0x008900d0u   /* u16 */
#define G_HOTPOS_JUMP   0x008900cdu   /* u8: jump to the hot position */
#define G_HOTPOS_VALID  0x008900ceu   /* u8: a hot position is stored */
#define FN_AUTOPILOT_ON  0x005fae20u  /* Player::AutoPilotOn(this) */
#define FN_AUTOPILOT_OFF 0x005faf10u  /* Player::AutoPilotOff(this) */
#define FN_SETINPUTMODE  0x00605c70u  /* Player::SetInputMode(this, mode) */
#define MAINLOOP_RET     0x00581475u
/* Race coordinator ([0x890118]): +0x00 RCSTATE (4 = race in progress, 7 =
 * post-race), +0x24 racer count, +0x30 racers (0x1F0 bytes each: +0x00 car,
 * +0x0A current position, +0x0C final position, +0x10 u16 completion type,
 * +0x14 fraction of the route done), +0xBD0 racer pointers sorted by
 * position.  FinishRacer(sortedIndex, position (0 = next free), flag,
 * completionType) at 0x606CB0 (thiscall, ret 0x10) is what the coordinator
 * itself calls when a racer crosses the line (0x608C48: (i, 0, 0, 1)). */
#define G_RACE           0x00890118u
#define FN_FINISHRACER   0x00606cb0u
#define RCSTATE_RACING   4u
#define RACING_CAR_NORMAL_COMPLETION 1u
/* Career (0x85AD40 = profileData 0x83A9D0 + 0x20370; layout from yugecin's
 * nfsu2-re notes, checked against our exe): +0x7134 bank, +0x71C8 64
 * available races (CareerRace*), +0x7AF8 current stage, +0x7AFC count and
 * +0x7B00 per-race state {hash, flags; byte +6: 1 = won}, +0x8708 engaged
 * race, +0x8730 race being run.
 * Tables loaded from GLOBAL\GLOBALB.BUN: races [0x8383E0] x [0x8383E4], 0x88
 * bytes each (+0 name offset into the string pool [0x838428], +8 hash, +0xF
 * type, +0x28 marker hash, +0x30 bank reward, +0x37 stage); shops [0x8383F0]
 * x [0x8383F4], 0xA0 bytes (+0 name, +0x38 hash, +0x3C marker hash).
 * Markers: PathsData (0x88F298) +0x434 count, +0x438 size, +0x43C blob of
 * variable-size records (+0 type, +4 x, +8 y, +0xC/+0x10 heading, +0x14 z
 * for event markers, +0x30 hash, +0x42 record size). */
#define G_CAREER         0x0085ad40u
#define G_RACES          0x008383e0u
#define G_NRACES         0x008383e4u
#define G_SHOPS          0x008383f0u
#define G_NSHOPS         0x008383f4u
#define G_STRPOOL        0x00838428u
#define G_PATHS          0x0088f298u
#define RACE_SIZE        0x88u
#define SHOP_SIZE        0xa0u
/* Showcases (magazine/DVD cover shoots): [0x83840C] x [0x838408], 0x40 bytes
 * (+0 name e.g. STAGE_2_DVD_1, +0x20 hash, +0x25 stage, +0x2C marker hash,
 * +0x3C visual rating a DVD needs).  CalculateVisualRep (0x5234C0) returns
 * the tweak at 0x7FA290 instead of the car's rating when it is not -1. */
#define G_SHOWS          0x0083840cu
#define G_NSHOWS         0x00838408u
#define SHOW_SIZE        0x40u
#define G_VISUAL_TWEAK   0x007fa290u
/* "Other" events: [0x838424] x [0x838420], 0x18 bytes (+0 name, +2 its
 * trigger's name, +4 stage, +6 what opens it: 2 the stage's outrun wins
 * (stage record +0xDC) reaching +7; +0xC the shop or cover it leads to):
 * the unique parts' stars (STAGE_5_VISUAL_5, the wide body kit) and the
 * cover shoots' (STAGE_2_SHOW_1).  Career +0x7148: those open now (32),
 * Career +0: the map's markers (64 of 0x18 bytes: +0 trigger, +4 race,
 * +8 shop, +0xC cover, +0x10 other event), Career +0xC08: the one being
 * run. */
#define G_OTHERS         0x00838424u
#define G_NOTHERS        0x00838420u

/* ---- expressions ---------------------------------------------------------- */
static const char *xp;
static uint32_t x_expr(void);
static void x_ws(void){ while (*xp == ' ' || *xp == '\t') xp++; }
static uint32_t x_unary(void){
    x_ws();
    if (*xp == '*'){ xp++; uint32_t a = x_unary();
        return mem_is_committed(a) && mem_is_committed(a + 3) ? MR32(a) : 0u; }
    if (*xp == '('){ xp++; uint32_t v = x_expr(); x_ws(); if (*xp == ')') xp++; return v; }
    char *end; uint32_t v = (uint32_t)strtoul(xp, &end, 0); xp = end; return v;
}
static uint32_t x_term(void){        /* '*' after an operand multiplies */
    uint32_t v = x_unary();
    for (;;){ x_ws(); if (*xp == '*'){ xp++; v *= x_unary(); } else return v; }
}
static uint32_t x_expr(void){
    uint32_t v = x_term();
    for (;;){ x_ws();
        if (*xp == '+'){ xp++; v += x_term(); }
        else if (*xp == '-'){ xp++; v -= x_term(); }
        else return v; }
}
static uint32_t eval(const char *s){ xp = s; return x_expr(); }

/* ---- guest calls ---------------------------------------------------------- */
/* Call guest function fn with ecx and stack arguments.  Must run on the guest
 * thread at a point where the game can take a call (the main-loop hook).
 * The whole CPU state is restored, so it works for any calling convention. */
static uint32_t guest_call(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(MAINLOOP_RET);           /* the callee's ret pops it into eip */
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}

#define QMAX 16
typedef struct { char line[160]; } Job;
static Job g_q[QMAX]; static int g_qh, g_qt;
static int g_hooked;                 /* the main-loop hook has run at least once */

static int player1_ok(void){
    uint32_t p = MR32(G_PLAYER1);
    return p && mem_is_committed(p) && mem_is_committed(p + 0x40);
}
/* Player 1's vehicle (Player+4), 0 if none; its +0x24 is the driver kind
 * (1 human, 3 AI).  autotest.c's car trace marks it as the local car. */
uint32_t autopilot_vehicle(void){
    if (!player1_ok()) return 0;
    uint32_t v = MR32(MR32(G_PLAYER1) + 4u);
    return v && mem_is_committed(v) && mem_is_committed(v + 0x27u) ? v : 0u;
}
static int player1_human(void){ uint32_t v = autopilot_vehicle(); return v && MR32(v + 0x24u) == 1u; }

/* The player's racer: the one driving the car of PlayersByNumber[0]. */
static int player_racer(uint32_t race, uint32_t *idx, uint32_t *rr){
    if (!player1_ok() || !race || !mem_is_committed(race + 0xc00)) return 0;
    uint32_t car = MR32(MR32(G_PLAYER1) + 4u), n = MR32(race + 0x24u);
    for (uint32_t i = 0; i < n && i < 8; i++){
        uint32_t r = MR32(race + 0xbd0u + 4u * i);
        if (r && mem_is_committed(r + 0x20) && MR32(r) == car){ *idx = i; *rr = r; return 1; }
    }
    return 0;
}

/* Finish the player now, through the coordinator's own FinishRacer: the
 * position is the next free one, i.e. 1st when nobody has finished yet.
 * Drift events are decided on points instead, so RACEWIN finishes those
 * after 0.25 s, before anybody has drifted (a tie at 0 goes to the first one
 * home).  On the downhill tracks (0x104E..0x1053) the other drivers' points
 * come from a table (6 tracks x 0x34 bytes at 0x7FA5C0: +0 track, +4 12
 * floats) into the live leaderboard (DriftManager 0x56D850, list 0x864630)
 * when the race starts, and the results read that list (the player's entry
 * is refreshed from the real score at the finish); a career race's drift win
 * zeroes the table and puts every other entry at -1 (a tie at 0 sorts the
 * player last on these tracks).  (The score at racer +0x140
 * and +0x194 is only a copy: setting it does not change the result.) */
#define G_DOWNHILL_SCORES 0x007fa5c0u
#define G_DRIFT_BOARD     0x00864630u   /* list head; entries +8 id (>= 0x64: set score), +0x10 points */
static void drift_lead(uint32_t me){   /* me = the player's racer index */
    for (uint32_t t = 0; t < 6; t++)
        for (uint32_t k = 0; k < 12; k++) MRF32(G_DOWNHILL_SCORES + t * 0x34u + 4u + 4u * k) = 0.f;
    uint32_t e = MR32(G_DRIFT_BOARD);
    for (int k = 0; e && e != G_DRIFT_BOARD && k < 16 && mem_is_committed(e + 0x1f); k++, e = MR32(e))
        if (MR32(e + 8u) != me) MRF32(e + 0x10u) = -1.f;   /* below the player's 0 */
}
static void race_win(CPU *c){
    uint32_t race = MR32(G_RACE), i, r;
    if (!race || MR32(race) != RCSTATE_RACING || !player_racer(race, &i, &r)){
        fprintf(stderr, "[career] f=%ld racewin: no race in progress\n", autotest_frame()); return; }
    if (MR16(r + 0x10u)){ fprintf(stderr, "[career] racewin: player already finished\n"); return; }
    { uint32_t cr = MR32(G_CAREER + 0x8730u);
      if (cr && mem_is_committed(cr + 0x10) && MR8(cr + 0xfu) == 5){
          drift_lead((r - (race + 0x30u)) / 0x1f0u);
          fprintf(stderr, "[career] f=%ld racewin: drift, the other entries on the board put below 0\n", autotest_frame()); } }
    uint32_t a[4] = { i, 0, 0, RACING_CAR_NORMAL_COMPLETION };
    guest_call(c, FN_FINISHRACER, race, 4, a);
    fprintf(stderr, "[career] f=%ld racewin: player (racer %u of %u, was %u.) finished, position %u\n",
            autotest_frame(), i, MR32(race + 0x24u), MR8(r + 0xau), MR8(r + 0xcu));
}

/* NFSU2_CAREER_RACEWIN=<s>: in a career race, finish the player once the race
 * clock (race +0x14, 1/4000 s, what the HUD shows) passes <s> seconds
 * (test-only; unset = off).  Free roam is a one-racer "race" in progress too,
 * so it needs a running career race (Career +0x8730).  In an outrun (race
 * type 7) it posts the player 1st once the outrun has run for <s> seconds of
 * host time (the outrun's own clock fields are not the race clock's). */
static double g_racewin_s = -1;
static void outrun_win(void);
static int outrun_running(uint32_t race);
static void race_tick(CPU *c){
    if (g_racewin_s <= 0) return;
    uint32_t race = MR32(G_RACE), i, r;
    if (!race || !mem_is_committed(race + 0xc00) || MR32(race) != RCSTATE_RACING) return;
    static int or_on, or_posted, or_logged; static double or_t0;
    if (!outrun_running(race)) or_on = 0;
    else {
        /* seconds of host time since the outrun was first seen; once per outrun */
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (!or_on){ or_on = 1; or_t0 = now; or_posted = or_logged = 0; }
        if (!or_posted && now - or_t0 >= g_racewin_s){ or_posted = 1; outrun_win(); }
        if (!or_logged && player_racer(race, &i, &r) && MR16(r + 0x10u)){
            or_logged = 1;
            fprintf(stderr, "[career] f=%ld outrun finished: player completion %u (6 won, 7 lost)\n",
                    autotest_frame(), MR16(r + 0x10u)); }
        return;
    }
    /* a career race; one racer is fine there (downhill drift tracks, where
     * the game sets the other scores) */
    if (!MR32(G_CAREER + 0x8730u)) return;
    /* drift events are decided on points: finish before the others have
     * drifted at all (a tie at 0 goes to the first one home) */
    uint32_t cr = MR32(G_CAREER + 0x8730u);
    double after = mem_is_committed(cr + 0x10) && MR8(cr + 0xfu) == 5 ? 0.25 : g_racewin_s;
    if ((double)MR32(race + 0x14u) >= after * 4000.0 && player_racer(race, &i, &r) && !MR16(r + 0x10u))
        race_win(c);
}

/* Outrun (test-only).  In career free roam the tuners are the car manager's
 * racer slots 1..3 ([0x890080] +0x14 first, +0x18 end, +0x1C car pointers).
 * The game offers a challenge when one drives within 30 m ahead of the
 * player at >= 30 mph, within 10 mph of the player's speed and 10 degrees of
 * the heading (0x40C030, per frame from 0x413030): Career4230 (0x85EF70)
 * +0x1AE8 = 2 and player +0x250 = that car.  Engage (0x602E09) then sets race
 * coordinator +0xC30, and the coordinator's update starts the outrun itself
 * (0x607180: race type [0x89E7B0] = 7, racers from those slots, opponent
 * from player +0x250).  Matching a tuner's speed needs a driver, so `outrun`
 * puts that state in place and lets the game start the race.  The outrun
 * ends when the game posts the result (0x4A7B30: coordinator +0xBEC = the
 * position of the player whose number is in +0xBF0); the coordinator then
 * calls FinishRacer with completion 6 (won) or 7 (lost) and
 * Career::OnOutrunFinish pays out.  `outrunwin` posts "player 1st". */
#define G_RACETYPE       0x0089e7b0u   /* 7 = outrun */
#define G_CARMGR         0x00890080u
#define G_OUTRUN_STATE   (0x0085ef70u + 0x1ae8u)
#define RACETYPE_OUTRUN  7u
static uint32_t outrun_car(uint32_t k){   /* k = 0: the nearest one on the map */
    uint32_t cm = MR32(G_CARMGR), best = 0, me = MR32(MR32(G_PLAYER1) + 4u);
    if (!cm || !mem_is_committed(cm + 0x40) || !me) return 0;
    uint32_t lo = MR32(cm + 0x14u), hi = MR32(cm + 0x18u);
    float bd = 1e30f;
    for (uint32_t i = lo; i < hi && i < 8; i++){
        uint32_t car = MR32(cm + 0x1cu + 4u * i);
        if (!car || !mem_is_committed(car + 0x440)) continue;
        if (k) { if (i == k) return car; continue; }
        if (MRF32(car + 0x68u) < -1000.f) continue;   /* parked slots sit at z = -123456 */
        float dx = MRF32(car + 0x60u) - MRF32(me + 0x60u), dy = MRF32(car + 0x64u) - MRF32(me + 0x64u);
        if (dx * dx + dy * dy < bd){ bd = dx * dx + dy * dy; best = car; }
    }
    return best;
}
/* 1: challenge placed; 0: no tuner within reach yet (jumped behind the
 * nearest one when there is one); -1: not in free roam */
static int outrun_start(uint32_t k, int quiet){
    uint32_t race = MR32(G_RACE), car = outrun_car(k), p = MR32(G_PLAYER1);
    long f = autotest_frame();
    if (!race || !player1_ok() || MR32(race) != RCSTATE_RACING || MR32(race + 0x24u) != 1 || MR32(G_RACETYPE)){
        fprintf(stderr, "[career] f=%ld outrun: not in free roam\n", f); return -1; }
    if (!car){ if (!quiet) fprintf(stderr, "[career] f=%ld outrun: no tuner on the map (slot %u)\n", f, k); return 0; }
    float dx = MRF32(car + 0x60u) - MRF32(MR32(p + 4u) + 0x60u), dy = MRF32(car + 0x64u) - MRF32(MR32(p + 4u) + 0x64u);
    if (dx * dx + dy * dy > 150.f * 150.f){
        /* first get within reach: 12 m behind it, same heading */
        uint16_t ang = MR16(car + 0x40cu); double a = ang / 65536.0 * 2.0 * 3.14159265358979;
        MRF32(G_HOTPOS) = MRF32(car + 0x60u) - 12.f * (float)cos(a);
        MRF32(G_HOTPOS + 4) = MRF32(car + 0x64u) - 12.f * (float)sin(a);
        MRF32(G_HOTPOS + 8) = MRF32(car + 0x68u) + 0.5f;
        MR16(G_HOTPOS_ANGLE) = ang; MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
        fprintf(stderr, "[career] f=%ld outrun: tuner car 0x%08x is %.0f m away, jump behind it\n", f, car,
                sqrt(dx * dx + dy * dy));
        return 0;
    }
    MR32(p + 0x250u) = car;
    MR32(G_OUTRUN_STATE) = 2;
    MR8(race + 0xc30u) = 1;
    fprintf(stderr, "[career] f=%ld outrun: challenge tuner car 0x%08x at %.1f %.1f (player at %.1f %.1f)\n", f, car,
            MRF32(car + 0x60u), MRF32(car + 0x64u), MRF32(MR32(p + 4u) + 0x60u), MRF32(MR32(p + 4u) + 0x64u));
    return 1;
}
static int outrun_running(uint32_t race){
    return race && mem_is_committed(race + 0xc00) && MR32(race) == RCSTATE_RACING &&
           MR32(G_RACETYPE) == RACETYPE_OUTRUN && MR32(race + 0x24u) >= 2;
}
static void outrun_win(void){
    uint32_t race = MR32(G_RACE), i, r;
    if (!outrun_running(race) || !player_racer(race, &i, &r)){
        fprintf(stderr, "[career] f=%ld outrunwin: no outrun in progress\n", autotest_frame()); return; }
    uint32_t owner = MR32(MR32(r) + 0x18u);
    MR32(race + 0xbf0u) = owner && mem_is_committed(owner + 0x14) ? MR32(owner + 0x10u) : 0;
    MR32(race + 0xbecu) = 1;
    fprintf(stderr, "[career] f=%ld outrunwin: player %u posted 1st (racer %u of %u)\n", autotest_frame(),
            MR32(race + 0xbf0u), i, MR32(race + 0x24u));
}

/* ---- front end -------------------------------------------------------------- */
/* Active FEng packages: cFEng [0x8384C4] +8 -> manager, whose lists at +0xE4
 * and +0xF4 (heads; GetTopPackage 0x50C8E0 returns the first) chain FNGInfo
 * records through +4; the package name is at +0xC.  Overlays such as
 * GenericDialog_SMALL.fng are on these lists while GetTopPackage still names
 * the screen underneath. */
static uint32_t fe_mgr(void){
    if (!mem_is_committed(0x8384c4u)) return 0;
    uint32_t fe = MR32(0x8384c4u);
    if (!fe || !mem_is_committed(fe + 11)) return 0;
    uint32_t mg = MR32(fe + 8u);
    return mg && mem_is_committed(mg + 0xf7u) ? mg : 0;
}
static const char *fe_name(uint32_t p){
    uint32_t n = MR32(p + 0xcu);
    if (!n || !mem_is_committed(n) || !mem_is_committed(n + 47)) return 0;
    const char *q = (const char *)(g_mem + n); int k = 0;
    while (k < 47 && q[k] >= 0x20 && q[k] < 0x7f) k++;
    return (k > 4 && !q[k]) ? q : 0;
}
/* calls fn(name) for every active package; stops when fn returns nonzero */
static uint32_t g_fe_cur;            /* FNGInfo of the package fn is looking at */
static int fe_each(int (*fn)(const char *, void *), void *u){
    uint32_t mg = fe_mgr();
    if (!mg) return 0;
    for (uint32_t head = 0xe4u; head <= 0xf4u; head += 0x10u){
        uint32_t p = MR32(mg + head);
        for (int k = 0; p && k < 64 && mem_is_committed(p) && mem_is_committed(p + 0x10); k++){
            const char *nm = fe_name(p);
            g_fe_cur = p;
            if (nm && fn(nm, u)) return 1;
            p = MR32(p + 4u);
        }
    }
    return 0;
}
static int fe_eq(const char *nm, void *u){ return !strcasecmp(nm, (const char *)u); }
int career_fe_active(const char *name){ return fe_each(fe_eq, (void *)name); }
static int fe_print(const char *nm, void *u){
    if (u) fprintf(stderr, " %s[%02x%02x%02x%02x mask %08x]", nm, MR8(g_fe_cur + 0x14u), MR8(g_fe_cur + 0x15u),
                   MR8(g_fe_cur + 0x16u), MR8(g_fe_cur + 0x17u), MR32(g_fe_cur + 0x1cu));
    else fprintf(stderr, " %s", nm);
    return 0;
}

/* ---- career data ---------------------------------------------------------- */
static const char *gstr(uint32_t a){
    return a && mem_is_committed(a) && mem_is_committed(a + 63) ? (const char *)(g_mem + a) : "?";
}
static const char *race_name(uint32_t r){ return gstr(MR32(G_STRPOOL) + MR16(r)); }
static const char *const k_race_type[] = { "circuit", "sprint", "streetx", "url", "drag", "drift" };
static const char *race_type(uint32_t r){ uint8_t t = MR8(r + 0xfu); return t < 6 ? k_race_type[t] : "?"; }
static int event_won(uint32_t h){   /* h = race (+8) or showcase (+0x20) hash */
    uint32_t n = MR32(G_CAREER + 0x7afcu);
    for (uint32_t i = 0; i < n && i < 384; i++)
        if (MR32(G_CAREER + 0x7b00u + 8u * i) == h) return MR8(G_CAREER + 0x7b06u + 8u * i) == 1;
    return 0;
}
static int race_won(uint32_t r){ return event_won(MR32(r + 8u)); }
static uint32_t race_by_name(const char *nm){
    uint32_t t = MR32(G_RACES), n = MR32(G_NRACES);
    for (uint32_t i = 0; t && i < n; i++)
        if (!strcasecmp(race_name(t + i * RACE_SIZE), nm)) return t + i * RACE_SIZE;
    return 0;
}
static uint32_t shop_by_name(const char *nm){
    uint32_t t = MR32(G_SHOPS), n = MR32(G_NSHOPS);
    for (uint32_t i = 0; t && i < n; i++)
        if (!strcasecmp(gstr(t + i * SHOP_SIZE), nm)) return t + i * SHOP_SIZE;
    return 0;
}
static uint32_t marker_by_hash(uint32_t h){
    uint32_t m = MR32(G_PATHS + 0x43cu), end = m + MR32(G_PATHS + 0x438u);
    for (uint32_t k = 0; m && m < end && k < 4096; k++){
        if (MR32(m + 0x30u) == h) return m;
        int16_t sz = (int16_t)MR16(m + 0x42u);
        if (sz <= 0) break;
        m += (uint32_t)sz;
    }
    return 0;
}
/* Jump onto marker m, through the game's hot-position jump. */
static int goto_marker(uint32_t m, const char *what){
    if (!m){ fprintf(stderr, "[career] f=%ld %s: marker not found\n", autotest_frame(), what); return 0; }
    float x = MRF32(m + 4u), y = MRF32(m + 8u), z = MRF32(m + 0x14u);
    if (z == 0 && player1_ok()){ uint32_t car = MR32(MR32(G_PLAYER1) + 4u); if (car) z = MRF32(car + 0x68u); }
    double a = atan2((double)MRF32(m + 0x10u), (double)MRF32(m + 0xcu));
    uint16_t ang = (uint16_t)(int)(a / (2.0 * 3.14159265358979) * 65536.0);
    MRF32(G_HOTPOS) = x; MRF32(G_HOTPOS + 4) = y; MRF32(G_HOTPOS + 8) = z + 0.5f;
    MR16(G_HOTPOS_ANGLE) = ang; MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
    fprintf(stderr, "[career] f=%ld %s: teleport to marker type %u at %.1f %.1f %.1f angle 0x%04x\n",
            autotest_frame(), what, MR32(m), x, y, z, ang);
    return 1;
}
static void career_list(void){
    fprintf(stderr, "[career] f=%ld stage %u bank $%u, available races:", autotest_frame(),
            MR32(G_CAREER + 0x7af8u), MR32(G_CAREER + 0x7134u));
    for (int i = 0; i < 64; i++){
        uint32_t r = MR32(G_CAREER + 0x71c8u + 4u * (uint32_t)i);
        if (r) fprintf(stderr, " %s(%s%s)", race_name(r), race_type(r), race_won(r) ? ",won" : "");
    }
    fputc('\n', stderr);
    /* the other events open now, the map's markers by kind (the other events
     * and shops by name), the other event being run */
    uint32_t st = MR32(G_CAREER + 0x7af8u), run = MR32(G_CAREER + 0xc08u), n[5] = { 0 };
    fprintf(stderr, "[career] f=%ld stage %u: %u outrun wins, other events open:", autotest_frame(), st,
            st < 8 ? MR32(G_CAREER + 0x72d8u + st * 0x104u + 0xdcu) : 0u);
    for (uint32_t i = 0; i < 32u; i++){
        uint32_t e = MR32(G_CAREER + 0x7148u + 4u * i);
        if (e) fprintf(stderr, " %s", race_name(e));
    }
    fprintf(stderr, "; markers:");
    for (uint32_t i = 0; i < 64u; i++){
        uint32_t m = G_CAREER + 0x18u * i;
        if (!MR32(m)) continue;
        for (uint32_t k = 0; k < 5u; k++) if (k == 4u || MR32(m + 4u + 4u * k)){ n[k]++; break; }
        if (MR32(m + 0x10u)) fprintf(stderr, " %s", race_name(MR32(m + 0x10u)));
        else if (MR32(m + 8u) && run) fprintf(stderr, " shop %s", gstr(MR32(m + 8u)));
    }
    fprintf(stderr, " (%u races, %u shops, %u cover shoots, %u other events, %u more)", n[0], n[1], n[2], n[3], n[4]);
    if (run) fprintf(stderr, "; running %s", race_name(run));
    fputc('\n', stderr);
}
/* the available race that is not won yet (of type ty, if given) that moves the
 * career on fastest, skipping the n races in skip[]: this stage's final race
 * (stage settings +0x28), then sponsor races (+0x34 == 1), URL races (2),
 * the rest of this stage, older stages last; within each class drift events
 * (decided on points, often lost) come last */
static uint32_t next_race(const char *ty, const uint32_t *skip, int n){
    uint32_t st = MR32(G_CAREER + 0x7af8u), set = st < 8 ? MR32(G_CAREER + 0x72d8u + st * 0x104u + 0x100u) : 0;
    uint32_t fin = set ? MR32(set + 0x28u) : 0, best = 0; int bs = 1000;
    for (int i = 0; i < 64; i++){
        uint32_t q = MR32(G_CAREER + 0x71c8u + 4u * (uint32_t)i), k;
        if (!q || race_won(q) || (ty && strncasecmp(race_type(q), ty, strlen(ty)))) continue;
        for (k = 0; k < (uint32_t)n && skip[k] != q; k++) ;
        if (k < (uint32_t)n) continue;
        int sc = (MR8(q + 0x37u) == st ? 0 : 100) + 2 * (MR32(q + 8u) == fin ? 0 : MR8(q + 0x34u) == 1 ? 1 :
                 MR8(q + 0x34u) == 2 ? 2 : 3) + (MR8(q + 0xfu) == 5);   /* drift (on points) after the rest */
        if (sc < bs){ bs = sc; best = q; }
    }
    return best;
}
static int race_go(uint32_t r){
    char what[96]; snprintf(what, sizeof what, "careergo %s (%s, stage %u)", race_name(r), race_type(r), MR8(r + 0x37u));
    return goto_marker(marker_by_hash(MR32(r + 0x28u)), what);
}
static void career_go(const char *arg){
    uint32_t r = !strncasecmp(arg, "next", 4) ? next_race(arg[4] == ':' ? arg + 5 : 0, 0, 0) : race_by_name(arg);
    if (!r){ fprintf(stderr, "[career] f=%ld careergo %s: no such race\n", autotest_frame(), arg); return; }
    race_go(r);
}

/* ---- career bot (NFSU2_CAREER_BOT=1, test-only) ----------------------------
 * Plays the career unattended, from the title screen on.  It loads the profile
 * and resumes the career; in free roam it jumps onto the next available race
 * that is not won yet and presses RETURN until the engage dialog opens; the
 * race itself is finished by NFSU2_CAREER_RACEWIN (3 s unless set); then it
 * walks the post-race screens and saves.  Dialogs are accepted with LEFT,
 * RETURN (the confirm button is on the left, the cursor starts on the right;
 * the bot never opens a dialog it would refuse).  With no race to run it
 * tries the stage's cover shoots (showcases), then drives into the crib
 * (that is what moves the prologue on).  A screen it
 * does not know gets RETURN after NFSU2_CAREER_BOT_WAIT frames (default 600)
 * and is logged.
 *   NFSU2_CAREER_BOT_SAVE=0     answer the post-race "save profile?" with No
 *   NFSU2_CAREER_BOT_RACES=<n>  quit (clean exit) in free roam after n events
 *                               (races and cover shoots) the bot has won
 *   NFSU2_CAREER_BOT_HEAP=<MB>  ... or once the guest address space the heap
 *                               has reserved (VA) passes MB
 *   NFSU2_CAREER_BOT_VISUAL=<n> force the car's visual rating to n stars (the
 *                               game's own tweak 0x7FA290), so the DVD and
 *                               magazine cover shoots open up.  Unset or 0:
 *                               the car's own rating.  A DVD cover shoot is
 *                               only offered (and its marker only engages)
 *                               when the rating reaches the shoot's need
 *                               (showcase +0x3C, checked when free roam
 *                               loads, 0x5331C0); the bot never buys visual
 *                               parts, so without this the stage-5 shoots
 *                               (7-10 stars; KONIG's contract asks for all
 *                               four, S5_URL_9 and the final come after them)
 *                               are never offered and the career stops in
 *                               stage 5.
 *                               tools/career_segments.sh sets 10 unless told
 *                               otherwise
 *   NFSU2_CAREER_BOT_SHOWFIRST=1  try the stage's cover shoots before races
 *   NFSU2_CAREER_BOT_DRIVE=1    drive onto a cover shoot's marker with the
 *                               arrow keys, the way a player gets there,
 *                               instead of jumping onto it (from 40 m behind
 *                               it along its heading, else 60 m off in one of
 *                               four directions)
 * so a driver script can run the career in segments, one launch each. */
void autotest_tap(unsigned dik, long frames);
void autotest_quit(const char *why);
const char *autotest_top_fng(void);
uint32_t win32_heap_used(void);          /* guest VA the heap has reserved */
uint32_t win32_heap_live(void);          /* bytes in live heap blocks */
void kbd_set_key(int src, unsigned dik, int down);   /* kbd.h; src 1 = script */
#define DIK_RETURN 0x1cu
#define DIK_LEFT   0xcbu
#define DIK_RIGHT  0xcdu
#define DIK_UP     0xc8u
#define DIK_DOWN   0xd0u
#define FN_VISUAL_RATING 0x00528410u  /* Career::the player car's visual rating (thiscall, st0) */
#define G_VISUAL_OFF     0x007842a0u  /* the tweak's "off" value (-1.0) */
enum { BOT_IDLE, BOT_GOTO, BOT_SHOP, BOT_SHOW, BOT_SHOW_NEAR, BOT_SHOW_DRIVE };
static struct {
    int on, save, phase, tries, nskip, stalled, jumps, quit_races;
    long next, t0, wait, top_since, last_poke, roam_since, quit_heap;
    uint32_t race, skip[64], show, show_skip[32]; int nshow_skip, show_first;
    int not_offered;                   /* this stage's DVD shoots the rating does not reach */
    int drive;                         /* NFSU2_CAREER_BOT_DRIVE */
    struct { long t0, stuck, back; int keys, flip, cal, nstuck; float h0; } dv;   /* driving onto a marker */
    float visual;
    float tx, ty;
    char keys[8]; int nkeys;           /* pending key sequence */
    char top[48];
    long dump_at; int ndumps;          /* a frame dump shortly after each new screen */
    int movies;                        /* NOMOVIES unset or 0: movies play */
    int raced;                         /* B.race has been run since the last free roam */
    int shot;                          /* B.show has been engaged since the last free roam */
    int won;                           /* races and cover shoots the bot won this launch (the
                                        * event table also marks entries the bot did not run
                                        * as won, e.g. the prologue's DDAY_EVENT_A/B) */
    uint32_t show_stage;               /* stage the cover-shoot skip list belongs to */
} B;

void metal_request_dump(long guest_frame, const char *label);   /* metal.m */
static void bot_dump(const char *what){
    if (B.ndumps >= 200) return;
    char lab[64]; int k = 0;
    for (const char *q = what; *q && *q != '.' && k < 40; q++) lab[k++] = isalnum((unsigned char)*q) ? *q : '_';
    lab[k] = 0;
    if (B.race && strncasecmp(what, "UI_Engage", 9) && (strstr(what, "HUD") || strstr(what, "PostRace") || strstr(what, "postrace")))
        snprintf(lab + k, sizeof lab - (size_t)k, "_%s", race_name(B.race));
    B.ndumps++; metal_request_dump(autotest_frame(), lab);
}

static void bot_keys(const char *k){    /* L = LEFT, R = RETURN, G = RIGHT */
    snprintf(B.keys, sizeof B.keys, "%s", k); B.nkeys = (int)strlen(B.keys);
}
static int fe_dialog(const char *nm, void *u){    /* GenericDialog_*, IG_GenericDialog_* */
    (void)u; return strstr(nm, "GenericDialog") != 0;
}
static int roaming(void){
    uint32_t race = MR32(G_RACE);
    return MR32(G_FLOW) == 6u && race && mem_is_committed(race + 0xc00) &&
           MR32(race + 0x24u) == 1u && !MR32(G_CAREER + 0x8730u);
}
static int near_xy(float x, float y, float d){
    if (!player1_ok()) return 0;
    uint32_t car = MR32(MR32(G_PLAYER1) + 4u);
    if (!car) return 0;
    float dx = MRF32(car + 0x60u) - x, dy = MRF32(car + 0x64u) - y;
    return dx * dx + dy * dy < d * d;
}
static int near_target(void){ return near_xy(B.tx, B.ty, 40.f); }
static int bot_jump(uint32_t m, const char *what){
    if (!goto_marker(m, what)) return 0;
    B.tx = MRF32(m + 4u); B.ty = MRF32(m + 8u); return 1;
}
/* NFSU2_CAREER_TRACEFE=1 (debug): log the messages the engage dialogs get
 * (msg hash, and the focused object's hash for button presses 0x0C407210) */
void sub_00498f00(CPU *c);   /* EngageEventDialogFNGObject::MessageHandler */
void sub_00499c00(CPU *c);   /* EngageRaceDialogFNGObject::MessageHandler */
void dispatch_patch(uint32_t a, void (*f)(CPU *));
static void trace_msg(CPU *c, const char *who){
    uint32_t m = MR32(c->esp + 4u), o = MR32(c->esp + 8u);
    uint32_t oh = o && mem_is_committed(o) && mem_is_committed(o + 0x13) ? MR32(o + 0x10u) : 0;
    fprintf(stderr, "[fe] f=%ld %s msg 0x%08x obj 0x%08x (hash 0x%08x) a3 0x%08x a4 0x%08x\n", autotest_frame(),
            who, m, o, oh, MR32(c->esp + 12u), MR32(c->esp + 16u));
}
static void trace_event_dialog(CPU *c){ trace_msg(c, "EngageEventDialog"); sub_00498f00(c); }
static void trace_race_dialog(CPU *c){ trace_msg(c, "EngageRaceDialog"); sub_00499c00(c); }

static void bot_init(void){
    { const char *t = getenv("NFSU2_CAREER_TRACEFE");
      if (t && atoi(t)){ dispatch_patch(0x498f00u, trace_event_dialog); dispatch_patch(0x499c00u, trace_race_dialog); } }
    const char *e = getenv("NFSU2_CAREER_BOT"); B.on = e && atoi(e);
    e = getenv("NFSU2_CAREER_BOT_SAVE"); B.save = !e || atoi(e);
    e = getenv("NFSU2_CAREER_BOT_WAIT"); B.wait = e ? atol(e) : 600;
    e = getenv("NFSU2_CAREER_BOT_RACES"); B.quit_races = e ? atoi(e) : 0;
    e = getenv("NFSU2_CAREER_BOT_HEAP"); B.quit_heap = e ? atol(e) : 0;
    e = getenv("NFSU2_CAREER_BOT_VISUAL"); B.visual = e ? (float)atof(e) : 0;
    e = getenv("NOMOVIES"); B.movies = !e || !*e || *e == '0';
    e = getenv("NFSU2_CAREER_BOT_SHOWFIRST"); B.show_first = e && atoi(e);
    e = getenv("NFSU2_CAREER_BOT_DRIVE"); B.drive = e && atoi(e);
    e = getenv("NFSU2_CAREER_RACEWIN"); g_racewin_s = e ? atof(e) : 0;
    if (B.on && g_racewin_s <= 0) g_racewin_s = 3.0;
    B.show_stage = 0xffffffffu;
    /* before the first free-roam load: entering free roam is when the career
     * checks the visual rating against the DVD covers (0x5412D0 -> 0x5331C0) */
    if (B.visual > 0) MRF32(G_VISUAL_TWEAK) = B.visual;
    if (B.on){
        char vis[32] = "the car's own";
        if (B.visual > 0) snprintf(vis, sizeof vis, "forced to %.1f", (double)B.visual);
        fprintf(stderr, "[bot] career bot on (racewin %.1f s, save %d, quit after %d races / %ld MB VA, "
                "visual rating %s%s)\n", g_racewin_s, B.save, B.quit_races, B.quit_heap, vis,
                B.drive ? ", drives onto cover-shoot markers" : "");
    }
}
/* The player car's visual rating as the career sees it (0x528410, what the
 * DVD offer 0x5331C0 compares against), and its own one with the tweak off */
static CPU *g_cpu;                   /* the guest CPU while the main-loop hook runs */
static double guest_call_st0(CPU *c, uint32_t fn, uint32_t ecx){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    c->ecx = ecx;
    PUSH32(MAINLOOP_RET);
    dispatch_indirect(c, fn);
    double r = c->st[c->top & 7];
    *c = save;
    return r;
}
static double visual_rating(int own){
    if (!g_cpu || !player1_ok()) return -1;
    float tw = MRF32(G_VISUAL_TWEAK);
    if (own) MRF32(G_VISUAL_TWEAK) = MRF32(G_VISUAL_OFF);
    double r = guest_call_st0(g_cpu, FN_VISUAL_RATING, G_CAREER);
    MRF32(G_VISUAL_TWEAK) = tw;
    return r;
}
/* event-state entry of hash h (career +0x7B00: {hash, flags}), 0 if none */
static uint32_t event_entry(uint32_t h){
    uint32_t n = MR32(G_CAREER + 0x7afcu);
    for (uint32_t i = 0; i < n && i < 384; i++)
        if (MR32(G_CAREER + 0x7b00u + 8u * i) == h) return G_CAREER + 0x7b00u + 8u * i;
    return 0;
}
/* a DVD cover shoot is offered when its entry's byte +4 is 2: 0x5331C0 sets
 * that (and puts the shoot on the active list, career +0x887C) when free roam
 * loads and the visual rating reaches the shoot's need (+0x3C), and clears it
 * when the rating is below */
static int show_offered(uint32_t q){
    uint32_t e = event_entry(MR32(q + 0x20u));
    return e && MR8(e + 4u) == 2;
}
/* next DVD cover shoot of the current stage the bot has not tried yet (done
 * ones simply do not engage and get skipped).  Sponsor contracts ask for DVD
 * covers; the magazine covers (STAGE_n_SHOW_k_PICTURE, +0x34 == 3) only open
 * for a while after their SMS, so the bot leaves those alone. */
static uint32_t next_showcase(void){
    uint32_t t = MR32(G_SHOWS), n = MR32(G_NSHOWS), st = MR32(G_CAREER + 0x7af8u);
    for (uint32_t i = 0; t && i < n && i < 64; i++){
        uint32_t q = t + i * SHOW_SIZE; int k;
        if (MR8(q + 0x25u) != st || MR8(q + 0x34u) != 4) continue;
        for (k = 0; k < B.nshow_skip && B.show_skip[k] != q; k++) ;
        if (k == B.nshow_skip) return q;
    }
    return 0;
}
/* jump to a point 60 m off marker m (k-th of four directions: the first one
 * that is a road wins, the game drops the car elsewhere otherwise) */
static void bot_approach(uint32_t m, int k){
    static const float dx[4] = { 0, 0, 60.f, -60.f }, dy[4] = { 60.f, -60.f, 0, 0 };
    float hx = MRF32(m + 0xcu), hy = MRF32(m + 0x10u), hl = sqrtf(hx * hx + hy * hy);
    if (B.drive && k == 0 && hl > 0.01f){
        /* driving: first from 40 m behind the marker along its heading (the
         * way the road runs there), facing it; then the four directions */
        MRF32(G_HOTPOS) = MRF32(m + 4u) - 40.f * hx / hl; MRF32(G_HOTPOS + 4) = MRF32(m + 8u) - 40.f * hy / hl;
        MR16(G_HOTPOS_ANGLE) = (uint16_t)(int)(atan2(hy, hx) / (2.0 * 3.14159265358979) * 65536.0);
    } else {
        if (B.drive) k--;
        MRF32(G_HOTPOS) = MRF32(m + 4u) + dx[k & 3]; MRF32(G_HOTPOS + 4) = MRF32(m + 8u) + dy[k & 3];
        MR16(G_HOTPOS_ANGLE) = 0;
    }
    MRF32(G_HOTPOS + 8) = MRF32(m + 0x14u) + 0.5f;
    MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
    B.tx = MRF32(G_HOTPOS); B.ty = MRF32(G_HOTPOS + 4);
}
/* A cover shoot engages as soon as the car is on its marker.  Jumping straight
 * onto it opens the dialog while the world is still streaming in
 * (HUD_World_Loading), and that dialog then never gets FE input; so the bot
 * first jumps 60 m off the marker, lets the world settle, then moves on. */
static int bot_try_show(long f){
    uint32_t q, m;
    while (B.nshow_skip < 32 && (q = next_showcase())){
        B.show_skip[B.nshow_skip++] = q;
        if (event_won(MR32(q + 0x20u))) continue;           /* done */
        if (!show_offered(q)){
            /* its marker does not engage: say why instead of jumping there */
            B.not_offered++;
            fprintf(stderr, "[bot] f=%ld %s not offered: it needs visual rating %.2f, the car has %.2f "
                    "(its own %.2f)%s, skipped\n", f, gstr(q), (double)MRF32(q + 0x3cu), visual_rating(0),
                    visual_rating(1), B.visual > 0 ? "" : "; NFSU2_CAREER_BOT_VISUAL=<stars> forces it");
            continue; }
        if (!(m = marker_by_hash(MR32(q + 0x2cu)))) continue;
        B.show = q; B.tries = 0; B.jumps = 0; B.phase = BOT_SHOW_NEAR;
        bot_approach(m, 0);
        fprintf(stderr, "[career] f=%ld showcase %s: approach (needs visual rating %.2f, the car has %.2f)\n",
                f, gstr(q), (double)MRF32(q + 0x3cu), visual_rating(0));
        B.next = f + 720; return 1;
    }
    return 0;
}
/* NFSU2_CAREER_BOT_DRIVE=1: from the approach point, drive onto the marker
 * with the arrow keys (accelerate, steer toward it, brake on it), as a player
 * would; the cover shoot engages when the car is on the marker.  60 frames of
 * rolling, then 60 with LEFT held, find which way LEFT turns the heading.
 * Stuck three times (a wall between): the next approach direction. */
static void drive_keys(int up, int down, int left, int right){
    int k = up | down << 1 | left << 2 | right << 3;
    if (k == B.dv.keys) return;
    kbd_set_key(1, DIK_UP, up); kbd_set_key(1, DIK_DOWN, down);
    kbd_set_key(1, DIK_LEFT, left); kbd_set_key(1, DIK_RIGHT, right);
    B.dv.keys = k;
}
static void drive_stop(void){ B.dv.keys = -1; drive_keys(0, 0, 0, 0); }
static void bot_drive(long f){
    uint32_t m = marker_by_hash(MR32(B.show + 0x2cu)), car = player1_ok() ? MR32(MR32(G_PLAYER1) + 4u) : 0;
    if (!m || !car || !mem_is_committed(car + 0x440)){ drive_stop(); B.phase = BOT_IDLE; return; }
    float x = MRF32(car + 0x60u), y = MRF32(car + 0x64u), dx = MRF32(m + 4u) - x, dy = MRF32(m + 8u) - y;
    float d = sqrtf(dx * dx + dy * dy), sp = MRF32(car + 0x430u);
    float h = MR16(car + 0x40cu) / 65536.f * 6.2831853f;
    if (f - B.dv.t0 > 4800){
        fprintf(stderr, "[bot] f=%ld %s: drove for %ld frames, %.1f m short of the marker, no engage dialog, skipped\n",
                f, gstr(B.show), f - B.dv.t0, d);
        drive_stop(); B.phase = BOT_IDLE; return; }
    if ((f - B.dv.t0) % 240 < 2)
        fprintf(stderr, "[bot] f=%ld drive to %s: %.1f m, speed %.1f, heading %.0f deg\n", f, gstr(B.show), d, sp,
                h * 57.29578f);
    if (B.dv.cal < 3){              /* calibration: roll, then which way does LEFT turn? */
        if (B.dv.cal == 0){ B.dv.cal = 1; drive_keys(1, 0, 0, 0); B.next = f + 60; return; }
        if (B.dv.cal == 1){ B.dv.cal = 2; B.dv.h0 = h; drive_keys(1, 0, 1, 0); B.next = f + 60; return; }
        float dh = remainderf(h - B.dv.h0, 6.2831853f);
        B.dv.flip = dh < 0; B.dv.cal = 3;
        fprintf(stderr, "[bot] f=%ld drive: LEFT turned the heading by %.1f deg in 60 frames (speed %.1f)\n", f,
                dh * 57.29578f, sp);
    }
    float e = remainderf(atan2f(dy, dx) - h, 6.2831853f);   /* > 0: the marker is on LEFT's side */
    if (B.dv.flip) e = -e;
    int left = e > 0.08f, right = e < -0.08f;
    if (B.dv.back > f){ drive_keys(0, 1, right, left); B.next = f + 2; return; }   /* backing out */
    if (fabsf(sp) < 1.f && d > 8.f && f - B.dv.t0 > 240){
        if (!B.dv.stuck) B.dv.stuck = f;
        else if (f - B.dv.stuck > 120){
            B.dv.stuck = 0;
            if (++B.dv.nstuck >= 3 && B.jumps + 1 < 5){   /* in the way: from the next direction */
                fprintf(stderr, "[bot] f=%ld drive: stuck %.1f m off three times, another approach\n", f, d);
                drive_stop(); bot_approach(m, ++B.jumps); B.phase = BOT_SHOW_NEAR; B.next = f + 720; return; }
            B.dv.back = f + 90;
            fprintf(stderr, "[bot] f=%ld drive: stuck %.1f m off, backing up\n", f, d); }
    } else B.dv.stuck = 0;
    if (d < 6.f) drive_keys(0, fabsf(sp) > 2.f, 0, 0);       /* on the marker: stop */
    else drive_keys(fabsf(e) < 1.2f && sp < (d < 30.f ? 8.f : 15.f), 0, left, right);
    B.next = f + 2;
}
static void bot_roam(long f){
    if (B.visual > 0 && MRF32(G_VISUAL_TWEAK) != B.visual) MRF32(G_VISUAL_TWEAK) = B.visual;
    if (B.raced && B.race){          /* back from a race: lost ones are not tried again */
        B.raced = 0;
        if (race_won(B.race)) B.won++;
        else {
            fprintf(stderr, "[bot] f=%ld %s was lost (scores set by the game), skipped from now on\n", f, race_name(B.race));
            if (B.nskip < 64) B.skip[B.nskip++] = B.race; } }
    if (B.shot && B.show){           /* back from a cover shoot */
        B.shot = 0;
        if (event_won(MR32(B.show + 0x20u))) B.won++; }
    uint32_t heap = win32_heap_used() >> 20;
    if (B.phase == BOT_IDLE && ((B.quit_races && B.won >= B.quit_races) || (B.quit_heap && (long)heap >= B.quit_heap))){
        struct rusage ru; getrusage(RUSAGE_SELF, &ru);
        fprintf(stderr, "[bot] f=%ld segment done: %d events won this launch, heap %u MB live, VA %u MB, max rss %ld MB\n",
                f, B.won, win32_heap_live() >> 20, heap,
#ifdef __wasm__
                0L);   /* no ru_maxrss in WASI */
#else
                (long)(ru.ru_maxrss >> 20));
#endif
        /* in a window the quit only asks the event loop to stop and returns:
         * the bot must not act (or report again) in the frames before the exit */
        B.on = 0; career_list(); autotest_quit("career bot segment done"); return; }
    if (f - B.roam_since < 480){ B.next = B.roam_since + 480; return; }   /* let the world settle */
    if (B.phase == BOT_SHOW_DRIVE){ bot_drive(f); return; }
    if (B.phase == BOT_IDLE){
        uint32_t st = MR32(G_CAREER + 0x7af8u);
        if (st != B.show_stage){ B.show_stage = st; B.nshow_skip = 0; B.not_offered = 0; }   /* a new stage: its shoots */
        if (B.show_first && bot_try_show(f)) return;
        uint32_t r = next_race(0, B.skip, B.nskip);
        /* only drift events left (often lost on points): the cover shoots first,
         * they are what a stage's contract may still be waiting for */
        if (r && MR8(r + 0xfu) == 5 && bot_try_show(f)) return;
        if (r){
            B.stalled = 0; B.race = r; B.tries = 0; B.jumps = 0; B.phase = BOT_GOTO;
            if (!bot_jump(marker_by_hash(MR32(r + 0x28u)), race_name(r))){
                if (B.nskip < 64) B.skip[B.nskip++] = r;
                B.phase = BOT_IDLE; }
            B.next = f + 360; return; }
        if (bot_try_show(f)) return;
        if (st >= 6){                /* past stage 5's final: the career is complete */
            fprintf(stderr, "[bot] f=%ld career complete: stage %u, nothing left to race (%d events won this launch)\n",
                    f, st, B.won);
            B.on = 0; career_list(); autotest_quit("career bot: career complete"); return; }
        if (B.stalled >= 2){
            fprintf(stderr, "[bot] f=%ld career stalled: nothing to race, no cover shoot engages, the crib did not help\n", f);
            if (B.not_offered)
                fprintf(stderr, "[bot] f=%ld %d of this stage's DVD cover shoots were not offered: the visual rating is "
                        "below their need (%s)\n", f, B.not_offered, B.visual > 0 ? "NFSU2_CAREER_BOT_VISUAL is too low"
                        : "NFSU2_CAREER_BOT_VISUAL unset; the bot buys no visual parts");
            B.on = 0; career_list(); autotest_quit("career bot stalled"); return; }
        B.stalled++; fprintf(stderr, "[bot] f=%ld no race or showcase to run: into the crib\n", f); career_list();
        uint32_t sh = shop_by_name("CRIB_1");
        if (sh && bot_jump(marker_by_hash(MR32(sh + 0x3cu)), "crib")){ B.phase = BOT_SHOP; B.tries = 0; B.jumps = 0; }
        B.next = f + 360; return;
    }
    if (B.phase == BOT_SHOW_NEAR){
        uint32_t m = marker_by_hash(MR32(B.show + 0x2cu));
        if (near_target() || (m && near_xy(MRF32(m + 4u), MRF32(m + 8u), 200.f))){   /* the world is in */
            char what[64]; snprintf(what, sizeof what, "showcase %s", gstr(B.show));
            if (B.drive && m){
                fprintf(stderr, "[career] f=%ld %s: drive onto the marker at %.1f %.1f\n", f, what,
                        MRF32(m + 4u), MRF32(m + 8u));
                memset(&B.dv, 0, sizeof B.dv); B.dv.t0 = f; B.dv.keys = -1;
                B.phase = BOT_SHOW_DRIVE; B.next = f + 1; return; }
            B.phase = bot_jump(m, what) ? BOT_SHOW : BOT_IDLE;
            B.next = f + 360; return; }
        if (++B.jumps >= (B.drive ? 5 : 4) || !m){ fprintf(stderr, "[bot] f=%ld %s: no road near it, skipped\n", f, gstr(B.show));
            B.phase = BOT_IDLE; return; }
        bot_approach(m, B.jumps); B.next = f + 720; return; }
    /* BOT_GOTO / BOT_SHOP / BOT_SHOW: check that the jump happened, then
     * RETURN asks the game to engage the marker the car stands on */
    if (!near_target()){
        if (++B.jumps > 3){ fprintf(stderr, "[bot] f=%ld jump did not happen, giving up on it\n", f);
            if (B.phase == BOT_GOTO && B.nskip < 64) B.skip[B.nskip++] = B.race;
            B.phase = BOT_IDLE; return; }
        MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;    /* the stored target again */
        B.next = f + 360; return; }
    if (B.tries++ >= (B.phase == BOT_SHOW ? 4 : 8)){
        fprintf(stderr, "[bot] f=%ld %s: no engage dialog, skipped\n", f,
                B.phase == BOT_GOTO ? race_name(B.race) : B.phase == BOT_SHOW ? gstr(B.show) : "crib");
        if (B.phase == BOT_GOTO && B.nskip < 64) B.skip[B.nskip++] = B.race;
        B.phase = BOT_IDLE; return; }
    bot_keys("R"); B.next = f + 150;
}
static void bot_tick(void){
    if (!B.on) return;
    long f = autotest_frame();
    const char *top = autotest_top_fng(); if (!top) top = "";
    if (strcmp(top, B.top)){ snprintf(B.top, sizeof B.top, "%s", top); B.top_since = f; B.last_poke = f;
        if (!strcasecmp(top, "HUD_SingleRace.fng")) B.roam_since = f;
        fprintf(stderr, "[bot] f=%ld screen %s (flow %u, heap %u MB live, VA %u MB)\n", f, top, MR32(G_FLOW),
                win32_heap_live() >> 20, win32_heap_used() >> 20);
        B.dump_at = f + 45; }
    if (B.dump_at && f >= B.dump_at){ B.dump_at = 0; bot_dump(top); }
    if (B.phase == BOT_SHOW_DRIVE && strcasecmp(top, "HUD_SingleRace.fng")){   /* a screen came up: hands off */
        drive_stop();
        fprintf(stderr, "[bot] f=%ld drive: %s opened %ld frames after the start\n", f, top, f - B.dv.t0);
        if (!strcasecmp(top, "UI_EngageEventDialog.fng")){ B.phase = BOT_SHOW; B.next = f + 30; }
        else B.phase = BOT_IDLE;
    }
    if (f < B.next) return;
    if (B.nkeys){                      /* one key of the pending sequence every 20 frames */
        char k = B.keys[0]; memmove(B.keys, B.keys + 1, sizeof B.keys - 1); B.nkeys--;
        autotest_tap(k == 'L' ? DIK_LEFT : k == 'G' ? DIK_RIGHT : DIK_RETURN, 6);
        B.next = f + (B.nkeys ? 20 : 90); return;
    }
    /* overlays first: dialogs, and MC_Main.fng = the memory-card prompts
     * ("save the profile?", "Save complete.", "Load complete.") */
    if (fe_each(fe_dialog, 0)){ bot_keys("LR"); B.next = f + 30; return; }
    if (career_fe_active("MC_Main.fng")){ bot_keys(B.save ? "LR" : "R"); B.next = f + 60; return; }
    if (!strcasecmp(top, "UI_EngageRaceDialog.fng") || !strcasecmp(top, "UI_EngageEventDialog.fng")){
        if (!strcasecmp(top, "UI_EngageRaceDialog.fng") && B.phase == BOT_GOTO) B.raced = 1;
        if (!strcasecmp(top, "UI_EngageEventDialog.fng") && B.phase == BOT_SHOW) B.shot = 1;
        bot_keys("LR"); B.phase = BOT_IDLE; return; }
    if (!strcasecmp(top, "UI_EngageShopDialog.fng")){
        bot_keys(B.phase == BOT_SHOP ? "LR" : "R"); B.phase = BOT_IDLE; return; }
    if (!strcasecmp(top, "UI_ShowCase_Preview.fng")){ bot_keys("R"); B.next = f + 120; return; }  /* Take Picture */
    if (!strcasecmp(top, "UI_EngageMessageDialog.fng") || !strcasecmp(top, "UI_PostRaceResults.fng") ||
        !strcasecmp(top, "UI_PostRaceReward.fng") || !strcasecmp(top, "UI_postrace.fng") ||
        !strcasecmp(top, "UI_PostRace_TournResults.fng") || !strcasecmp(top, "UI_PostRace_TournStandings.fng") ||
        !strcasecmp(top, "UI_SMS_Mailbox.fng") ||            /* new messages: read the first one */
        !strcasecmp(top, "UI_SponsorPopup.fng") || !strcasecmp(top, "UI_Sponsorship_new.fng") ||
        !strcasecmp(top, "UI_MagazineBack.fng") || !strcasecmp(top, "UI_MagazineReward.fng") ||
        !strcasecmp(top, "MC_Background.fng")){                /* title, "Load complete." */
        bot_keys("R"); B.next = f + 150; return; }
    if (MR32(G_FLOW) == 3u){                                  /* front end */
        if (career_fe_active("UI_CareerCrib.fng")){ bot_keys("R"); B.next = f + 60; return; } /* Return to Explore Mode */
        if (career_fe_active("UI_Main.fng")){ bot_keys("LR"); B.next = f + 240; return; }      /* main menu: Career */
        if (career_fe_active("UI_StartCareer.fng")){ bot_keys("R"); B.next = f + 240; return; } /* Resume Career */
    }
    if (!strcasecmp(top, "HUD_SingleRace.fng") && MR32(G_FLOW) == 6u){
        if (!roaming()){ if (B.phase == BOT_SHOW_DRIVE) drive_stop();
            B.phase = BOT_IDLE; return; }                     /* racing: NFSU2_CAREER_RACEWIN */
        if (career_fe_active("UI_EngageMessageDialog.fng")){ bot_keys("R"); return; }
        bot_roam(f); return;
    }
    /* race HUDs, loading screens: wait.  A playing movie gets 3 minutes (with
     * NOMOVIES=1 the movie file does not open and the screen just waits).
     * Unknown screens: nudge with RETURN */
    if (!strcasecmp(top, "HUD_Drag.fng") || !strcasecmp(top, "HUD_Drift.fng") || !strcasecmp(top, "HUD_Short_Track.fng") ||
        !strcasecmp(top, "PC_Loading.fng") || !strcasecmp(top, "HUD_World_Loading.fng")) return;
    if (!strcasecmp(top, "IG_PlayMovie.fng") && B.movies && f - B.top_since < 180 * 120) return;
    if (!strcasecmp(top, "Credits.fng") && f - B.top_since < 600 * 120) return;   /* let them roll */
    if (f - B.last_poke >= B.wait){
        B.last_poke = f;
        fprintf(stderr, "[bot] f=%ld screen %s for %ld frames: RETURN\n", f, top, f - B.top_since);
        bot_keys("R");
    }
}

/* "uniques [won]": the shown car's unique (level-4) visual parts, slot by slot
 * (CARSLOTID names from the table at 0x802DE8, {id, name} pairs), and how many
 * of them 0x52A010 lets through for the shops (filter 6) and the garage (2).
 * Parts as a shop lists them (0x539960): the first by 0x61BA30(car type, slot,
 * 0, -1), the next by 0x61BA50(part, car type, slot, 0, -1), on the parts
 * database 0x8A2B68; the car type from the shown car ([0x8389AC], its method
 * +4). */
int uniques_caleb_beaten(void);
static void uniques_list(CPU *c, int won){
    if (won){
        uint32_t si = MR32(0x0086262cu), n = MR32(G_CAREER + 0x7afcu);
        if (si && mem_is_committed(si + 0x2bu) && n < 0x181u){
            uint32_t r = G_CAREER + 0x7b00u + 8u * n;
            MR32(r) = MR32(si + 0x28u); MR32(r + 4u) = 0; MR8(r + 6u) = 1;
            MR32(G_CAREER + 0x7afcu) = n + 1u;
            fprintf(stderr, "[career] uniques: stage 5's final event 0x%08x marked won (record %u)\n", MR32(si + 0x28u), n);
        }
    }
    uint32_t car = MR32(0x008389acu), ct = 0;      /* set in the front end (garage, shops), 0 in free roam */
    if (car && mem_is_committed(car) && mem_is_committed(MR32(car) + 7u)) ct = guest_call(c, MR32(MR32(car) + 4u), car, 0, 0);
    fprintf(stderr, "[career] f=%ld uniques: Caleb beaten %d, shown car 0x%08x, car type %u, game mode %u, garage flag %u\n",
            autotest_frame(), uniques_caleb_beaten(), car, ct, MR32(0x00850078u), MR32(0x0083898cu));
    if (!car || ct > 64u) return;
    unsigned tot = 0, tshop = 0, tgar = 0, parts = 0;
    for (uint32_t slot = 0; slot < 0x4cu; slot++){
        uint32_t e = 0x00802de8u + 8u * slot, nm = MR32(e + 4u);
        const char *name = MR32(e) == slot && nm >= 0x783000u && nm < 0x7e8000u ? (const char *)&MR8(nm) : "?";
        unsigned all = 0, uniq = 0, shop = 0, gar = 0, lv[8] = {0};
        uint32_t prev = 0, price = 0;
        for (int k = 0; k < 4096; k++){
            uint32_t a[4] = { ct, slot, 0, 0xffffffffu }, b[5] = { prev, ct, slot, 0, 0xffffffffu };
            uint32_t p = prev ? guest_call(c, 0x0061ba50u, 0x008a2b68u, 5, b) : guest_call(c, 0x0061ba30u, 0x008a2b68u, 4, a);
            if (!p || p == prev || !mem_is_committed(p + 13u)) break;
            prev = p; all++; lv[MR8(p + 5u) >> 5]++;
            if ((MR8(p + 5u) >> 5) != 4u) continue;
            if (!uniq++){ uint32_t pa[2] = { slot, p }; price = guest_call(c, 0x00514280u, G_CAREER, 2, pa); }  /* a buy's price (0x5289F0) */
            uint32_t s6[4] = { 6u, slot, p, MR32(0x008389b0u) }, s2[4] = { 2u, slot, p, MR32(0x008389b0u) };
            if (guest_call(c, 0x0052a010u, 0, 4, s6) & 0xffu) shop++;
            if (guest_call(c, 0x0052a010u, 0, 4, s2) & 0xffu) gar++;
        }
        if (uniq) fprintf(stderr, "[career] uniques: slot %u %s: %u parts (levels 0-7: %u %u %u %u %u %u %u %u), %u unique (the first $%u), %u in the shops, %u in the garage\n",
                          slot, name, all, lv[0], lv[1], lv[2], lv[3], lv[4], lv[5], lv[6], lv[7], uniq, price, shop, gar);
        tot += uniq; tshop += shop; tgar += gar; parts += all;
    }
    fprintf(stderr, "[career] uniques: %u parts, %u of them unique, %u in the shops, %u in the garage\n", parts, tot, tshop, tgar);
}

static void run_job(CPU *c, const char *line){
    char cmd[32] = {0}; int used = 0;
    sscanf(line, "%31s %n", cmd, &used);
    const char *rest = line + used;
    if (!strcasecmp(cmd, "call")){
        char tok[16][64]; int nt = 0; const char *p = rest;
        while (nt < 16 && *p){
            while (*p == ' ') p++;
            if (!*p) break;
            int k = 0; while (*p && *p != ' ' && k < 63) tok[nt][k++] = *p++;
            tok[nt++][k] = 0;
        }
        if (nt < 1) return;
        uint32_t fn = eval(tok[0]), ecx = 0, arg[15]; int na = 0;
        for (int i = 1; i < nt; i++){
            if (!strncasecmp(tok[i], "ecx=", 4)) ecx = eval(tok[i] + 4);
            else arg[na++] = eval(tok[i]);
        }
        uint32_t r = guest_call(c, fn, ecx, na, arg);
        fprintf(stderr, "[career] f=%ld call 0x%08x(ecx=0x%08x, %d args) -> eax=0x%08x (%d)\n",
                autotest_frame(), fn, ecx, na, r, (int)r);
    } else if (!strcasecmp(cmd, "autopilot")){
        if (!player1_ok()){ fprintf(stderr, "[career] autopilot: no player\n"); return; }
        uint32_t p = MR32(G_PLAYER1);
        if (!strncasecmp(rest, "on", 2)) guest_call(c, FN_AUTOPILOT_ON, p, 0, 0);
        else {
            guest_call(c, FN_AUTOPILOT_OFF, p, 0, 0);
            uint32_t mode = 2; guest_call(c, FN_SETINPUTMODE, p, 1, &mode);
        }
        fprintf(stderr, "[career] f=%ld autopilot %s\n", autotest_frame(), rest);
    } else if (!strcasecmp(cmd, "visual")){      /* the car's visual rating (with the tweak, its own) */
        g_cpu = c;
        fprintf(stderr, "[career] f=%ld visual rating %.2f (its own %.2f, tweak %.2f)\n", autotest_frame(),
                visual_rating(0), visual_rating(1), (double)MRF32(G_VISUAL_TWEAK));
        g_cpu = 0;
    } else if (!strcasecmp(cmd, "uniques")) uniques_list(c, !strncasecmp(rest, "won", 3));
    else if (!strcasecmp(cmd, "racewin")) race_win(c);
    else if (!strcasecmp(cmd, "outrunwin")) outrun_win();
    else if (!strcasecmp(cmd, "bot")){
        B.on = !strncasecmp(rest, "on", 2); B.phase = BOT_IDLE; B.nkeys = 0;
        { const char *e = getenv("NFSU2_CAREER_RACEWIN"); g_racewin_s = e ? atof(e) : 0; }
        if (B.on && g_racewin_s <= 0) g_racewin_s = 3.0;
        fprintf(stderr, "[bot] f=%ld career bot %s (racewin %.1f s)\n", autotest_frame(), B.on ? "on" : "off", g_racewin_s);
    }
}

/* Main-loop hook.  sub_004022c0 is an empty function with ~300 callers; only
 * GameLoop's do work: the call returning to 0x581475 (before the camera's
 * and the sound's update), and the first after the frame's Present
 * (0x5D2850), which gives back the camera the right stick's look turned for
 * the frame drawn (look.c). */
#define LOOK_END_RET     0x005814e0u
void look_frame_end(void);
void sub_004022c0_orig(CPU *c);
void sub_004022c0(CPU *c){
    if (MR32(c->esp) == LOOK_END_RET) look_frame_end();
    else if (MR32(c->esp) == MAINLOOP_RET){
        if (!g_hooked) bot_init();
        g_hooked = 1;
        while (g_qh != g_qt){ Job *j = &g_q[g_qh]; g_qh = (g_qh + 1) % QMAX; run_job(c, j->line); }
        race_tick(c);
        g_cpu = c; bot_tick(); g_cpu = 0;
    }
    sub_004022c0_orig(c);
}

static void queue(const char *line){
    if ((g_qt + 1) % QMAX == g_qh){ fprintf(stderr, "[career] job queue full, dropped: %s\n", line); return; }
    snprintf(g_q[g_qt].line, sizeof g_q[g_qt].line, "%s", line);
    g_qt = (g_qt + 1) % QMAX;
}

/* ---- script commands (called from autotest.c) ----------------------------- */
int career_is_cmd(const char *cmd){
    static const char *const k[] = { "call", "memdump", "waitx", "teleport", "autopilot", "racewin",
                                     "careerlist", "careergo", "shopgo", "peekx", "fnglist", "bot", "waitfe", "tapfe", "waitroam", "showgo", "cargo", "watchw",
                                     "outrun", "outrunwin", "visual", "uniques", "mpcredit", 0 };
    for (int i = 0; k[i]; i++) if (!strcasecmp(cmd, k[i])) return 1;
    return 0;
}

/* line = "<cmd> <args>".  Returns 1 when done, 0 to be called again next frame. */
int career_cmd(CPU *c, const char *line, long *wait_start){
    (void)c;
    char cmd[32] = {0}, a1[64] = {0}, a2[64] = {0}, a3[200] = {0}; int used = 0;
    sscanf(line, "%31s %n", cmd, &used);
    const char *rest = line + used;
    sscanf(rest, "%63s %63s %199s", a1, a2, a3);
    long f = autotest_frame();
    if (!strcasecmp(cmd, "autopilot") && !strncasecmp(a1, "on", 2) && a2[0]){
        /* "autopilot on TMO": wait (up to TMO frames) for a human driver in
         * player 1's car; the AI is handed the car in the main loop after */
        long tmo = atol(a2);
        if (*wait_start < 0) *wait_start = f;
        if (!player1_human() && f - *wait_start < tmo) return 0;
        if (player1_human())
            fprintf(stderr, "[career] f=%ld autopilot on: player 1 drives as a human after %ld frames\n", f, f - *wait_start);
        else fprintf(stderr, "[career] f=%ld autopilot on: no human driver in player 1's car after %ld frames\n", f, tmo);
        *wait_start = -1;
        queue(line); return 1;
    }
    if (!strcasecmp(cmd, "call") || !strcasecmp(cmd, "autopilot") || !strcasecmp(cmd, "racewin") ||
        !strcasecmp(cmd, "bot") || !strcasecmp(cmd, "outrunwin") || !strcasecmp(cmd, "visual") ||
        !strcasecmp(cmd, "uniques")){
        queue(line); return 1; }
    if (!strcasecmp(cmd, "memdump")){
        uint32_t a = eval(a1), n = eval(a2);
        FILE *o = fopen(a3, "wb");
        if (!o){ fprintf(stderr, "[career] memdump: cannot write %s\n", a3); return 1; }
        uint32_t w = 0;
        for (; w < n; w++){ if (!mem_is_committed(a + w)) break; fputc(MR8(a + w), o); }
        fclose(o);
        fprintf(stderr, "[career] f=%ld memdump 0x%08x %u bytes -> %s\n", f, a, w, a3);
        return 1;
    }
    if (!strcasecmp(cmd, "waitx")){
        if (*wait_start < 0) *wait_start = f;
        uint32_t v = eval(a1), want = eval(a2); long tmo = a3[0] ? atol(a3) : 0;
        if (v != want){
            if (tmo && f - *wait_start >= tmo){
                fprintf(stderr, "[career] f=%ld waitx %s==%s TIMEOUT (value 0x%x)\n", f, a1, a2, v);
                *wait_start = -1; return 1; }
            return 0; }
        fprintf(stderr, "[career] f=%ld waitx %s==%s satisfied after %ld frames\n", f, a1, a2, f - *wait_start);
        *wait_start = -1; return 1;
    }
    if (!strcasecmp(cmd, "peekx")){
        uint32_t a = eval(a1), n = a2[0] ? eval(a2) : 1;
        for (uint32_t i = 0; i < n && i < 64; i++){
            uint32_t q = a + 4u * i;
            if (!mem_is_committed(q) || !mem_is_committed(q + 3)){
                fprintf(stderr, "[career] f=%ld peekx [0x%08x] = <unmapped>\n", f, q); continue; }
            uint32_t v = MR32(q); float fv; memcpy(&fv, &v, 4);
            fprintf(stderr, "[career] f=%ld peekx %s [0x%08x] = 0x%08x (%d, %g)\n", f, i ? "" : a1, q, v, (int)v, (double)fv);
        }
        return 1;
    }
    if (!strcasecmp(cmd, "waitfe") || !strcasecmp(cmd, "tapfe")){
        /* waitfe NAME [tmo] / tapfe KEY NAME [every] [tmo]: wait (tapping KEY)
         * until package NAME is active, overlays included */
        int tap = !strcasecmp(cmd, "tapfe");
        const char *nm = tap ? a2 : a1;
        long every = tap && a3[0] ? atol(a3) : 120, tmo = 0;
        { char t4[32] = {0}, t3[32] = {0}; sscanf(rest, "%*s %*s %31s %31s", t3, t4);
          tmo = tap ? (t4[0] ? atol(t4) : 0) : (a2[0] ? atol(a2) : 0); }
        if (*wait_start < 0) *wait_start = f;
        if (career_fe_active(nm)){
            fprintf(stderr, "[career] f=%ld %s %s satisfied after %ld frames\n", f, cmd, nm, f - *wait_start);
            *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){
            fprintf(stderr, "[career] f=%ld %s %s TIMEOUT\n", f, cmd, nm); *wait_start = -1; return 1; }
        if (tap && (f - *wait_start) % every == 0){
            static const struct { const char *n; unsigned k; } keys[] = {
                {"RETURN",0x1c},{"ESCAPE",0x01},{"LEFT",0xcb},{"RIGHT",0xcd},{"UP",0xc8},{"DOWN",0xd0},{0,0} };
            for (int i = 0; keys[i].n; i++) if (!strcasecmp(keys[i].n, a1)) autotest_tap(keys[i].k, 6);
        }
        return 0;
    }
    if (!strcasecmp(cmd, "waitroam")){        /* waitroam [tmo]: free roam, settled for 2 s */
        long tmo = a1[0] ? atol(a1) : 0; const char *t = autotest_top_fng();
        if (*wait_start < 0) *wait_start = f;
        if (roaming() && t && !strcasecmp(t, "HUD_SingleRace.fng") && f - B.roam_since >= 240){
            fprintf(stderr, "[career] f=%ld waitroam satisfied after %ld frames\n", f, f - *wait_start);
            *wait_start = -1; return 1; }
        if (tmo && f - *wait_start >= tmo){
            fprintf(stderr, "[career] f=%ld waitroam TIMEOUT\n", f); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(cmd, "showgo")){
        uint32_t t = MR32(G_SHOWS), n = MR32(G_NSHOWS), q = 0;
        for (uint32_t i = 0; t && i < n && !q; i++) if (!strcasecmp(gstr(t + i * SHOW_SIZE), a1)) q = t + i * SHOW_SIZE;
        char what[64]; snprintf(what, sizeof what, "showgo %s", a1);
        if (q){ goto_marker(marker_by_hash(MR32(q + 0x2cu)), what); return 1; }
        /* ... or an "other" event's (STAGE_3_PERF_1: its trigger, the name at +2) */
        t = MR32(G_OTHERS); n = MR32(G_NOTHERS);
        for (uint32_t i = 0; t && i < n && !q; i++) if (!strcasecmp(race_name(t + i * 0x18u), a1)) q = t + i * 0x18u;
        if (!q){ fprintf(stderr, "[career] f=%ld %s: no such showcase or other event\n", f, what); return 1; }
        uint32_t h = 0xffffffffu;
        for (const char *s = gstr(MR32(G_STRPOOL) + MR16(q + 2u)); *s; s++) h = h * 33u + (uint8_t)*s;   /* bStringHash */
        goto_marker(marker_by_hash(h), what);
        return 1;
    }
    if (!strcasecmp(cmd, "cargo")){
        /* cargo K [BEHIND]: jump BEHIND m (default 12) behind the K-th car of
         * the car manager [0x890080] (+0x10 count, +0x1C car pointers; car
         * +0x60 position, +0x40C heading), e.g. to challenge an outrun tuner */
        uint32_t cm = MR32(0x890080u), k = eval(a1), n = cm ? MR32(cm + 0x10u) : 0;
        float back = a2[0] ? (float)atof(a2) : 12.f;
        if (!cm || k >= n || k >= 64){ fprintf(stderr, "[career] f=%ld cargo %u: no such car (%u)\n", f, k, n); return 1; }
        uint32_t car = MR32(cm + 0x1cu + 4u * k);
        if (!car || !mem_is_committed(car + 0x410)){ fprintf(stderr, "[career] f=%ld cargo %u: bad car\n", f, k); return 1; }
        uint16_t ang = MR16(car + 0x40cu); double a = ang / 65536.0 * 2.0 * 3.14159265358979;
        float x = MRF32(car + 0x60u) - back * (float)cos(a), y = MRF32(car + 0x64u) - back * (float)sin(a);
        MRF32(G_HOTPOS) = x; MRF32(G_HOTPOS + 4) = y; MRF32(G_HOTPOS + 8) = MRF32(car + 0x68u) + 0.5f;
        MR16(G_HOTPOS_ANGLE) = ang; MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
        fprintf(stderr, "[career] f=%ld cargo %u/%u: car 0x%08x at %.1f %.1f, racer 0x%08x; jump to %.1f %.1f\n", f, k, n,
                car, MRF32(car + 0x60u), MRF32(car + 0x64u), MR32(car + 0x1cu), x, y);
        return 1;
    }
    if (!strcasecmp(cmd, "outrun")){
        /* outrun [K] [tmo]: wait (up to tmo frames, default 3600) for a tuner,
         * get within reach of it, then challenge it */
        static long next_try;
        long tmo = a2[0] ? atol(a2) : 3600;
        if (*wait_start < 0){ *wait_start = f; next_try = f; }
        if (f >= next_try){
            int r = outrun_start(a1[0] ? eval(a1) : 0, f > *wait_start);
            if (r){ *wait_start = -1; return 1; }
            next_try = f + 120;      /* after a jump, let the world settle */
        }
        if (f - *wait_start >= tmo){
            fprintf(stderr, "[career] f=%ld outrun: TIMEOUT\n", f); *wait_start = -1; return 1; }
        return 0;
    }
    if (!strcasecmp(cmd, "watchw")){          /* debug: report who writes X (mem.c mprot) */
        void mprot_arm(uint32_t); mprot_arm(eval(a1)); return 1; }
    if (!strcasecmp(cmd, "careerlist")){ career_list(); return 1; }
    if (!strcasecmp(cmd, "fnglist")){
        fprintf(stderr, "[career] f=%ld fe packages:", f); fe_each(fe_print, a1[0] ? (void *)1 : 0); fputc('\n', stderr); return 1; }
    if (!strcasecmp(cmd, "careergo")){ career_go(a1); return 1; }
    if (!strcasecmp(cmd, "mpcredit")){ int mpcredit_test(const char *, float); mpcredit_test(a1, a2[0] ? (float)atof(a2) : 100.f); return 1; }
    if (!strcasecmp(cmd, "shopgo")){
        uint32_t sh = shop_by_name(a1);
        char what[64]; snprintf(what, sizeof what, "shopgo %s", a1);
        if (!sh) fprintf(stderr, "[career] f=%ld %s: no such shop\n", f, what);
        else goto_marker(marker_by_hash(MR32(sh + 0x3cu)), what);
        return 1;
    }
    if (!strcasecmp(cmd, "teleport")){
        float x = 0, y = 0, z = 0; unsigned ang = 0;
        if (sscanf(rest, "%f %f %f %i", &x, &y, &z, (int *)&ang) < 3){
            fprintf(stderr, "[career] teleport: need X Y Z [ANGLE]\n"); return 1; }
        MRF32(G_HOTPOS) = x; MRF32(G_HOTPOS + 4) = y; MRF32(G_HOTPOS + 8) = z;
        MR16(G_HOTPOS_ANGLE) = (uint16_t)ang;
        MR8(G_HOTPOS_VALID) = 1; MR8(G_HOTPOS_JUMP) = 1;
        fprintf(stderr, "[career] f=%ld teleport %.1f %.1f %.1f angle 0x%04x\n", f, x, y, z, ang & 0xffffu);
        return 1;
    }
    return 1;
}
