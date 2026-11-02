/* uniques.c -- once Caleb is beaten, the unique visual parts (the wide body
 * kits and the other visual rewards of the stages' outruns) are in every
 * career car's shops.
 *
 * The game gives each unique visual part only to the one car driven to its
 * star (STAGE_2_VISUAL_1 .. STAGE_5_VISUAL_5, the last one the wide body kit),
 * and the stage-5 star is gone once the last URL race is won.  Its own ending
 * opens all of them after Caleb, but only in Customize mode, whose cars free
 * roam never drives: there the career car is the one everybody sees.  So
 * after Caleb the career shops sell them for every car too, the rule the game
 * already has for Customize.  The stars and the outrun rewards stay as they
 * are, and the unique performance parts (a separate system: UI_ChooseUniquePart,
 * RideInfo +0x318) stay one per stage.
 *
 * The game's checks (v1.2 NA):
 *   0x52A010 IsUnlocked(filter, slot, part, car): the shops' and the garage's
 *     test of a car part (slot = CARSLOTID, WIDE_BODY 6; part = 14-byte record,
 *     byte +5's top 3 bits its unlock level, 4 = unique).  The filter comes from
 *     0x50F0D0: in career ([0x850078] 1) 6 in the shops and 2 in the garage
 *     ([0x83898C] 1), in Customize ([0x850078] 8) 0x08..0x1F8.
 *   0x515500(filter, slot, part): the career's half.  Bit 2: the car owns the
 *     part (the car's record +0x434, set by a buy, 0x5289F0).  Bit 4: the stage
 *     has opened its level (0x515360), and level 4 never.  Also called with 4
 *     alone from 0x530004 (a shop's category).
 *   0x529D60: Customize's half; unlocks level 4 once the final event of stage 5
 *     is won: Career 0x500B80([[0x86262C] + 0x28]), [0x86262C] being stage 5's
 *     stage info (its record +0x100) and +0x28 its final event, S5_CIRCUIT_36.
 * Here 0x515500 also says yes to a level-4 part with bit 4 asked once that same
 * event is won.  Nothing else changes: such a part is bought like any other and
 * is then the car's, in the profile too. */
#include "cpu.h"
#include <stdio.h>

#define G_CAREER       0x0085ad40u
#define C_NEVENTS      0x7afcu        /* career: events with a record */
#define C_EVENTS       0x7b00u        /* career: those records, 8 bytes {hash, ..., +6 state (1 won)} */
#define C_MAXEVENTS    0x181u         /* room up to +0x8708 */
#define G_S5_INFO      0x0086262cu    /* stage 5's stage info (its stage record +0x100) */
#define SI_FINAL       0x28u          /* stage info: the final event's hash */
#define P_LEVEL        5u             /* part: top 3 bits its unlock level */
#define LEVEL_UNIQUE   4u
#define F_STAGE        4u             /* 0x515500's filter: the stage's unlocks (the shops) */

static int committed(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }

/* Career 0x500B80 with stage 5's final event: Caleb's race won */
int uniques_caleb_beaten(void){
    uint32_t si = MR32(G_S5_INFO);
    if (!committed(si, SI_FINAL + 4u)) return 0;
    uint32_t h = MR32(si + SI_FINAL), n = MR32(G_CAREER + C_NEVENTS);
    if (!h || n > C_MAXEVENTS) return 0;
    for (uint32_t i = 0; i < n; i++){
        uint32_t r = G_CAREER + C_EVENTS + 8u * i;
        if (MR32(r) == h) return MR8(r + 6u) == 1u;
    }
    return 0;
}

static int g_said;

void sub_00515500_orig(CPU *c);
void sub_00515500(CPU *c){
    uint32_t filter = MR32(c->esp + 4u), slot = MR32(c->esp + 8u), part = MR32(c->esp + 12u);
    sub_00515500_orig(c);
    if ((c->eax & 0xffu) || !(filter & F_STAGE) || !committed(part, P_LEVEL + 1u)) return;
    if ((MR8(part + P_LEVEL) >> 5) != LEVEL_UNIQUE || !uniques_caleb_beaten()) return;
    c->eax = (c->eax & ~0xffu) | 1u;
    if (!g_said){
        g_said = 1;
        fprintf(stderr, "[career] Caleb beaten: the unique visual parts are in every car's shops (first asked: slot %u)\n", slot);
    }
}
