/* visualrep.c -- the career's visual rating set by the stage, not by the car's
 * parts, as thelink2012's Visual Reputation Remover (MIT,
 * https://github.com/thelink2012/novisualrep, licenses/novisualrep.txt) has
 * it.  visual_rep_remover = on (NFSU2_VISUAL_REP_REMOVER=1); off by default,
 * the original game.
 *
 * CalculateVisualRep (0x5234C0, thiscall (this, car), ret 4, st0: 0 to 10
 * stars) adds up the points of the car's visual parts, 500 a star; it first
 * returns the float at 0x7FA290 instead when that is not -1.0 (0x7842A0),
 * the exe's value.  It is called for the career's player car (0x528410) and
 * by the customization menus' Visual Rating box (0x55B1A0, 0x53D870).  The
 * career uses it:
 *  - when free roam loads (0x5412D0 -> 0x5331C0): the stage's DVD cover
 *    shoots (showcases of kind 2 and 4, [0x83840C] x [0x838408], 0x40 bytes)
 *    whose need (+0x3C) the rating reaches are offered (event entry +4 = 2,
 *    a text message the first time), the others taken back (+4 = 0);
 *  - when a cover shoot pays (0x498E70 -> 0x541930 -> 0x541210): its cash
 *    and the rating times the stage's word (stage record +0x100 -> +4).
 * The mod jumps from 0x5234C0 to its own function: 0, 1, 3, 5.5, 8 stars
 * at stages 0 to 4, 10 from stage 5 (Career +0x7AF8, 0x862838); the DVD
 * covers' needs are 1 (stage 2), 2 and 3, 4 to 6, 7 to 10.  Here the same
 * without code changes: 0x7FA290 gets the stage's value every frame (roam.c's
 * main-loop hook).  The mod also takes out the push of the box's package
 * (UI_Menu_Asset_Reputation.fng, 0x557EDE, its only push: 0x53D8C0 and
 * 0x560689 pop it); here roam.c's hook at 0x557ED0 makes the "is it up
 * already?" answer before it (0x52CF60, al) yes, so 0x557EC0 skips the push
 * and does the rest, as with the mod.  Nothing of it is in a profile; what
 * the career did with the rating is (covers offered, shot, paid): with the
 * setting off again the next free-roam load offers the covers by the car's
 * own rating.  NFSU2_CAREER_BOT_VISUAL (career.c's test bot, the same float)
 * wins over the stage's value. */
#include "cpu.h"
#include "setting.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define G_VISUAL_TWEAK  0x007fa290u   /* CalculateVisualRep returns this when not -1.0 */
#define G_STAGE         0x00862838u   /* Career 0x85AD40 +0x7AF8: 0 prologue, 1..5, 6 career complete */

static int g_on = -1;                 /* the setting */
static int g_rate = 0;                /* the setting, and no NFSU2_CAREER_BOT_VISUAL */
static float g_said = -1.f;           /* the last value logged */

static void visualrep_init(void){
    if (g_on >= 0) return;
    g_on = setting_bool("NFSU2_VISUAL_REP_REMOVER", 0);
    if (!g_on) return;
    const char *b = getenv("NFSU2_CAREER_BOT_VISUAL");
    g_rate = !(b && atof(b) > 0);
    fprintf(stderr, "[career] visual rating by stage (Visual Reputation Remover)%s\n",
            g_rate ? "" : ": NFSU2_CAREER_BOT_VISUAL set, its rating stays");
}

/* the mod's CalculateVisualRep */
static float stage_rating(int32_t s){
    switch (s){
    case 0: return 0.f;
    case 1: return 1.f;
    case 2: return 3.f;
    case 3: return 5.5f;
    case 4: return 8.f;
    default: return 10.f;
    }
}

/* every frame (roam.c's main-loop hook) */
void visualrep_frame(void){
    visualrep_init();
    if (!g_rate) return;
    float want = stage_rating((int32_t)MR32(G_STAGE));
    if (MRF32(G_VISUAL_TWEAK) != want) MRF32(G_VISUAL_TWEAK) = want;
    if (want != g_said){
        fprintf(stderr, "[career] visual rating %.1f stars (stage %d)\n", (double)want, (int32_t)MR32(G_STAGE));
        g_said = want;
    }
}

/* roam.c's hook at 0x557ED0 (test al, al), in the push of the Visual Rating
 * box: al = 1, "up already", jumps past the push to 0x557EF0; eax is not
 * read after the test */
void visualrep_meter(CPU *c){
    visualrep_init();
    if (g_on) c->eax |= 1u;
}
