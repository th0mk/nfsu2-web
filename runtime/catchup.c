/* catchup.c -- the career's rubberbanding (the AI's "catch up"), as the
 * player chooses it.  NFSU2_CATCHUP = original (default, the original game),
 * noboost or off: settings.ini's catchup (Mods: Rubberbanding).
 *
 * Addresses: SPEED2.EXE v1.2 NA; the European exe has these same ones.
 *
 * Every AI racer's update calls 0x4172D0 (thiscall, the AI driver): while the
 * race's Catch Up flag is on ([0x89E7F8], session +0x58; a career race leaves
 * it on, Quick Race has it as its Catch Up option) 0x4084A0 turns the gap to
 * the player into a factor M from the DifficultyInfo chunk 0x34B00 of GLOBALB
 * ([0x81CB60]: 8 curves of 11 (gap m, factor) pairs, 0x58 bytes each, one
 * for skill 0 and one for skill 9 blended by the AI's skill, picked by the
 * difficulty and the laps; in the retail data all four pairs are the same:
 * 320 m behind +0.22 (skill 0) to +0.37 (skill 9), 160 m +0.15 to +0.22,
 * 320 m ahead -0.15 to -0.10, 640 m ahead -0.25 to -0.15; plus a term for
 * the speed difference, at most 0.05).  1 is the AI's own
 * pace; ahead of the player M is below 1 (it waits), behind it above 1 (it
 * gains).  0x4172D0 keeps M in the driver (+0x770: its target speed, read by
 * 0x41C8A8 and 0x41CB76) and max(M, 1) in the car (vehicle = [driver+4],
 * +0x468: torque, governor and grip).  Measured in a career race (tests:
 * the player still at the start for 90 s): with the flag on the three AI
 * drove 1674 to 1861 m at 51 to 88 km/h, with it off 2592 to 2844 m at 94 to
 * 133 km/h.
 *
 * The opponents' "CatchUp" byte of the career race table (GCareerRace +0x4B,
 * +0x57, ...; racer +0x902) only picks one of three rows of the same chunk
 * (+0x934, 8 bytes each: 200/1, 200/1, 400/1), and the rows 0 and 1 are the
 * same.
 *
 * Here, after the game's own 0x4172D0 and only in a career race (Career
 * +0x8730):
 *   noboost  M at most 1 in the driver and 1 in the car: an opponent ahead
 *            still waits for you, one behind never gets past its own pace
 *   off      1 in both, as with the Catch Up flag off (Quick Race's Catch Up
 *            Off): no waiting and no boost.  With the flag off the game's M
 *            is 1 exactly: 0x4084A0 gives 0 for both its terms, and the scale
 *            by the race's progress after it (GLOBALB +0x504) only scales M - 1
 * Quick Race keeps its own option, and a race over the network has no AI. */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define CAREER_RACE     0x8730u
#define DRV_PACE        0x770u            /* AI driver: M, the target speed's factor */
#define DRV_VEHICLE     0x4u
#define VEH_BOOST       0x468u            /* vehicle: max(M, 1), power and grip */

enum { CU_ORIGINAL, CU_NOBOOST, CU_OFF };
static int g_mode = -1;

static int catchup_mode(void){
    if (g_mode < 0){
        const char *e = getenv("NFSU2_CATCHUP");
        g_mode = !e ? CU_ORIGINAL : !strcasecmp(e, "noboost") ? CU_NOBOOST : !strcasecmp(e, "off") ? CU_OFF : CU_ORIGINAL;
        if (g_mode != CU_ORIGINAL)
            fprintf(stderr, "[catchup] career rubberbanding: %s\n", g_mode == CU_OFF ? "off" : "no boost");
    }
    return g_mode;
}

static int okf(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u); }

/* NFSU2_CATCHUP_LOG=1 (tests): every 120 AI updates of a career race, the
 * lowest and highest factors the game made (M, the car's) and how many were
 * changed here */
static int g_log = -1;
static long g_calls, g_changed;
static float g_mlo = 9.f, g_mhi = 0.f, g_bhi = 0.f;
static void stat(uint32_t drv, float m, float b){
    if (g_log < 0){ const char *e = getenv("NFSU2_CATCHUP_LOG"); g_log = e && *e && *e != '0'; }
    if (!g_log) return;
    if (m < g_mlo) g_mlo = m;
    if (m > g_mhi) g_mhi = m;
    if (b > g_bhi) g_bhi = b;
    if (++g_calls % 120 == 0){
        fprintf(stderr, "[catchup] %ld AI updates: the game's M %.3f..%.3f, car boost up to %.3f; %ld changed (driver 0x%08x)\n",
                g_calls, (double)g_mlo, (double)g_mhi, (double)g_bhi, g_changed, drv);
        g_mlo = 9.f; g_mhi = 0.f; g_bhi = 0.f;
    }
}

/* the factors the game left, as the setting wants them (catchup_unit.c calls it too) */
void catchup_apply(uint32_t drv){
    int mode = catchup_mode();
    if (!okf(G_CAREER + CAREER_RACE) || !MR32(G_CAREER + CAREER_RACE)) return;
    if (!okf(drv + DRV_PACE) || !okf(drv + DRV_VEHICLE)) return;
    uint32_t veh = MR32(drv + DRV_VEHICLE);
    int car = okf(veh + VEH_BOOST);
    float m = MRF32(drv + DRV_PACE), b = car ? MRF32(veh + VEH_BOOST) : 1.f;
    int changed = 0;
    if (mode != CU_ORIGINAL){
        if (mode == CU_OFF ? m != 1.0f : m > 1.0f){ MRF32(drv + DRV_PACE) = 1.0f; changed = 1; }
        if (car && b != 1.0f){ MRF32(veh + VEH_BOOST) = 1.0f; changed = 1; }
    }
    g_changed += changed;
    stat(drv, m, b);
}

#ifndef CATCHUP_UNIT
void sub_004172d0_orig(CPU *c);
void sub_004172d0(CPU *c){
    uint32_t drv = c->ecx;
    sub_004172d0_orig(c);
    catchup_apply(drv);
}
#endif
