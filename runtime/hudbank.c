/* hudbank.c -- the Bank in career free roam's HUD (top right, "Bank:
 * 1,609,790") only when it says something: for BANK_CHANGE_S after the
 * bank changed (a race's or an outrun's prize, a shop's price, a money
 * icon), for BANK_START_S once free roam starts (the world loaded, a new
 * HUD) and for BANK_BACK_S once the HUD is back after a menu (the pause
 * menu, the map, a shop's screens); else hidden.  Original: always there in
 * career free roam.
 *
 * The HUD's parts show by bits: Player::Update's HUD step (0x600590) has
 * them made every frame (0x600090: eax the low ones, edx the high ones) and
 * hands them to the HUD (0x4CA090) when they change, which shows or hides
 * the parts whose bits changed (0x4C6690: a part is on while its masks, +0x10
 * and +0x14, meet the bits; its objects shown or hidden there, and its own
 * update keeps them so).  The Bank is the HUD's +0x80 (0x4F4F10: Cash_Bank,
 * Cash_data, Cash_header, Show_case_backing; its update 0x4A9EF0), the high
 * 0x80000, which 0x600090 sets in the career (0x850078 1) out of a race
 * (0x89E7B0 0).  Here, right after 0x600090, the bit goes while the Bank
 * has nothing to say: the HUD hides the part as for any change of the bits.
 * A money icon's "Bank Reward" (0x4AC1B0, the part's +0x28, which its update
 * shows) comes with a change of the bank, so it shows.  The reward pop-up
 * (Cash_Reward, 0x4F5060, the HUD's +0x78, the high 0x80) is another part,
 * as it was.  The Bank comes and goes at once: its objects have no fade of
 * their own (their scripts: INIT, HIDE, ZoomIn, Cash_Animate).
 *
 * Free roam starts when the Player's HUD is another one; the HUD is back
 * after a menu when the bit comes after a step without it (0x600090 gives
 * no bits while the game is paused), or the HUD step did not run for
 * BANK_GAP_S (screens that stop the world). */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <time.h>

#define BANK_CHANGE_S 20.0        /* shown after the bank changed */
#define BANK_START_S  10.0        /* ... after free roam starts */
#define BANK_BACK_S   8.0         /* ... after a menu */
#define BANK_GAP_S    1.0         /* the HUD step not run this long: the world stood (a menu, a loading screen) */
#define A_BANK        0x00861e74u /* the career's bank (Career 0x85AD40 +0x7134) */
#define PL_HUD        0x9a4u      /* Player: its HUD */
#define BIT_BANK      0x80000u    /* the HUD's high bits: the Bank */

static struct St {
    uint32_t pl, hud, bank;                      /* the Player, its HUD, the bank its last step with the bit saw */
    int had, known, shown;                       /* the last step had the bit, bank is one, the Bank is shown */
    double last, until;                          /* the last step, shown until (seconds) */
} g_st[2];                                       /* player 1's, co-op's player 2's */

static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static void show_for(struct St *s, double t, double sec){ if (s->until < t + sec) s->until = t + sec; }

/* Player::Update's HUD step, its bits just made: esi the Player, edx the high bits */
static void step(CPU *c){
    uint32_t pl = c->esi, hud = MR32(pl + PL_HUD);
    struct St *s = g_st[0].pl == pl || !g_st[0].pl ? &g_st[0] : g_st[1].pl == pl || !g_st[1].pl ? &g_st[1] :
                   g_st[0].last < g_st[1].last ? &g_st[0] : &g_st[1];
    double t = now_s();
    if (s->pl != pl) *s = (struct St){ .pl = pl };
    int gap = s->last > 0 && t - s->last > BANK_GAP_S;
    s->last = t;
    if (!(c->edx & BIT_BANK)){ s->had = 0; return; }
    uint32_t bank = MR32(A_BANK);
    const char *why = 0;
    if (hud != s->hud){ s->hud = hud; show_for(s, t, BANK_START_S); why = "free roam"; }
    else if (!s->had || gap){ show_for(s, t, BANK_BACK_S); why = "back"; }
    if (s->known && bank != s->bank){ show_for(s, t, BANK_CHANGE_S); why = "changed"; }
    s->had = s->known = 1; s->bank = bank;
    int on = t < s->until;
    if (why || on != s->shown)
        fprintf(stderr, "[bank] %s (%s, bank %u)\n", on ? "shown" : "hidden", why ? why : "time's up", bank);
    s->shown = on;
    if (!on) c->edx &= ~BIT_BANK;
}

int hudbank_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x0060059a): step(c); return 1;   /* Player::Update's HUD step: its bits (0x600090), before it compares them */
    }
    return 0;
}
