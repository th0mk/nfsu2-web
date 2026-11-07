/* hudbank.c -- two parts of career free roam's HUD only when they say
 * something.  Original: both always there in career free roam.
 *
 * The Bank (top right, "Bank: 1,609,790"): for BANK_CHANGE_S after the bank
 * changed (a race's or an outrun's prize, a shop's price, a money icon), for
 * BANK_START_S once free roam starts (the world loaded, a new HUD) and for
 * BANK_BACK_S once the HUD is back after a menu (the pause menu, the map, a
 * shop's screens); else hidden.
 *
 * The box at the top left (a dark box, the D-pad and its icons: the envelope
 * of an SMS waiting, Engage at a marker or a door, an outrun's tuner): for
 * BOX_NEW_S after a message came (an SMS, the career's note), while the car
 * is at a marker, a door or an outrun's tuner and BOX_HERE_S after, for
 * BOX_START_S once free roam starts and BOX_BACK_S after a menu; else hidden.
 *
 * The HUD's parts show by bits: Player::Update's HUD step (0x600590) has
 * them made every frame (0x600090: eax the low ones, edx the high ones) and
 * hands them to the HUD (0x4CA090) when they change, which shows or hides
 * the parts whose bits changed (0x4C6690: a part is on while its masks, +0x10
 * and +0x14, meet the bits; its objects shown or hidden there, and its own
 * update keeps them so).  The Bank is the HUD's +0x80 (0x4F4F10: Cash_Bank,
 * Cash_data, Cash_header, Show_case_backing; its update 0x4A9EF0), the high
 * 0x80000, which 0x600090 sets in the career (0x850078 1) out of a race
 * (0x89E7B0 0).  The box is the HUD's +0x58 (0x4F83D0; its update 0x4DA9A0),
 * the high 2, which 0x600090 sets out of a race (0x5EA8E0).  Here, right
 * after 0x600090, a part's bit goes while it has nothing to say: the HUD
 * hides the part as for any change of the bits.
 *
 * A money icon's "Bank Reward" (0x4AC1B0, the part's +0x28, which its update
 * shows) comes with a change of the bank, so it shows.  The reward pop-up
 * (Cash_Reward, 0x4F5060, the HUD's +0x78, the high 0x80) is another part,
 * as it was.  The Bank comes and goes at once: its objects have no fade of
 * their own (their scripts: INIT, HIDE, ZoomIn, Cash_Animate).
 *
 * The box's messages are 8 slots of 0x14 bytes at the part's +0x60: +0 the
 * state (0 none, 1 posted, 2 new: its sound and pulse, 1 s; 3 its blink, 4 s;
 * 4 resting, its icon still there), +1 the kind.  0x4C9170 posts one (or
 * makes a resting one of the kind new again), 0x4DAB80 takes one away.
 * Kinds: a marker's or a door's Engage 1-8, 0xB-0x12 (0x5EAFB0 posts it as
 * the car comes onto it, 0x4DB820 takes it away), an outrun's tuner 9
 * (0x4DB890), an SMS waiting 0x13 (0x4DB8C0, the envelope, until it is
 * read), the career's note 0x14 (0x534290, never taken away).  The part's
 * update runs only while it is shown, so a message posted while it is hidden
 * waits as posted (1) until the box shows: it shows the box, and the update
 * then plays its sound and pulse as it would have.  A posted one that a
 * resting one of a higher kind keeps waiting (0x4C90B0 shows the highest:
 * Engage, outrun, SMS, note) shows the box once, when it came.  A new HUD
 * posts the SMS waiting again (0x4DB8C0 counts from 0): messages in free
 * roam's first BOX_START_S are its start's.
 *
 * Free roam starts when the Player's HUD is another one; the HUD is back
 * after a menu when the bit comes after a step without it (0x600090 gives
 * no bits while the game is paused), or the HUD step did not run for
 * GAP_S (screens that stop the world). */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <time.h>

#define BANK_CHANGE_S 20.0        /* shown after the bank changed */
#define BANK_START_S  10.0        /* ... after free roam starts */
#define BANK_BACK_S   8.0         /* ... after a menu */
#define BOX_NEW_S     20.0        /* the box shown after a message came */
#define BOX_HERE_S    2.0         /* ... while the car is at a marker, a door or an outrun's tuner, and this after */
#define BOX_START_S   10.0        /* ... after free roam starts */
#define BOX_BACK_S    8.0         /* ... after a menu */
#define GAP_S         1.0         /* the HUD step not run this long: the world stood (a menu, a loading screen) */
#define A_BANK        0x00861e74u /* the career's bank (Career 0x85AD40 +0x7134) */
#define PL_HUD        0x9a4u      /* Player: its HUD */
#define HUD_BOX       0x58u       /* the HUD: the box's part */
#define BOX_SLOTS     0x60u       /* the box's part: its messages, BOX_N of BOX_SLOT bytes (+0 state, +1 kind) */
#define BOX_N         8u
#define BOX_SLOT      0x14u
#define KIND_SMS      0x13u       /* an SMS waiting */
#define KIND_NOTE     0x14u       /* the career's note */
#define BIT_BANK      0x80000u    /* the HUD's high bits: the Bank */
#define BIT_BOX       0x2u        /* ... the box */

struct Item { uint32_t hud; int had, shown; double until, start; };   /* the HUD its last step with the bit saw, the last
                                                                        * step had the bit, it is shown, shown until,
                                                                        * free roam's start (seconds) */
static struct St {
    uint32_t pl, bank, came, here;               /* the Player, the bank its last step saw, the box's posted or new
                                                  * slots, its slots at a marker, a door or a tuner */
    int known;                                   /* bank is one */
    double last;                                 /* the last step */
    struct Item b, x;                            /* the Bank, the box */
} g_st[2];                                       /* player 1's, co-op's player 2's */

static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static void show_for(struct Item *it, double t, double sec){ if (it->until < t + sec) it->until = t + sec; }

/* the part's bit in this step: shown for start once free roam starts, for back once the HUD is back after a menu */
static const char *bit_came(struct Item *it, uint32_t hud, double t, int gap, double start, double back){
    const char *why = 0;
    if (hud != it->hud){ it->hud = hud; it->start = t; show_for(it, t, start); why = "free roam"; }
    else if (!it->had || gap){ show_for(it, t, back); why = "back"; }
    it->had = 1;
    return why;
}

static void bank(CPU *c, struct St *s, uint32_t hud, double t, int gap){
    struct Item *it = &s->b;
    if (!(c->edx & BIT_BANK)){ it->had = 0; return; }
    uint32_t bank = MR32(A_BANK);
    const char *why = bit_came(it, hud, t, gap, BANK_START_S, BANK_BACK_S);
    if (s->known && bank != s->bank){ show_for(it, t, BANK_CHANGE_S); why = "changed"; }
    s->known = 1; s->bank = bank;
    int on = t < it->until;
    if (why || on != it->shown)
        fprintf(stderr, "[bank] %s (%s, bank %u)\n", on ? "shown" : "hidden", why ? why : "time's up", bank);
    it->shown = on;
    if (!on) c->edx &= ~BIT_BANK;
}

static void box(CPU *c, struct St *s, uint32_t hud, double t, int gap){
    struct Item *it = &s->x;
    if (!(c->edx & BIT_BOX)){ it->had = 0; return; }
    uint32_t part = hud && mem_is_committed(hud + HUD_BOX) ? MR32(hud + HUD_BOX) : 0, slots = part + BOX_SLOTS;
    if (!part || !mem_is_committed(slots) || !mem_is_committed(slots + BOX_N * BOX_SLOT - 1u)) return;
    const char *why = bit_came(it, hud, t, gap, BOX_START_S, BOX_BACK_S);
    uint32_t came = 0, here = 0;
    int pulse = 0;
    for (uint32_t k = 0; k < BOX_N; k++){
        uint8_t st = MR8(slots + k * BOX_SLOT), kind = MR8(slots + k * BOX_SLOT + 1u);
        if (!st || !kind) continue;
        if (kind != KIND_SMS && kind != KIND_NOTE) here |= 1u << k;
        else if (st == 1 || st == 2){ came |= 1u << k; pulse |= st == 2; }
    }
    int settled = t - it->start >= BOX_START_S;   /* (before: the new HUD's own SMS waiting, posted again) */
    if (settled && (came & ~s->came)){ show_for(it, t, BOX_NEW_S); why = "message"; }
    else if (settled && pulse) show_for(it, t, BOX_NEW_S);   /* (an SMS the game keeps new, 0x4C8AC0: all along) */
    if (here){ show_for(it, t, BOX_HERE_S); if ((here & ~s->here) && !why) why = "here"; }
    s->came = came; s->here = here;
    int on = t < it->until;
    if (why || on != it->shown) fprintf(stderr, "[box] %s (%s)\n", on ? "shown" : "hidden", why ? why : "time's up");
    it->shown = on;
    if (!on) c->edx &= ~BIT_BOX;
}

/* Player::Update's HUD step, its bits just made: esi the Player, edx the high bits */
static void step(CPU *c){
    uint32_t pl = c->esi, hud = MR32(pl + PL_HUD);
    struct St *s = g_st[0].pl == pl || !g_st[0].pl ? &g_st[0] : g_st[1].pl == pl || !g_st[1].pl ? &g_st[1] :
                   g_st[0].last < g_st[1].last ? &g_st[0] : &g_st[1];
    double t = now_s();
    if (s->pl != pl) *s = (struct St){ .pl = pl };
    int gap = s->last > 0 && t - s->last > GAP_S;
    s->last = t;
    bank(c, s, hud, t, gap);
    box(c, s, hud, t, gap);
}

int hudbank_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x0060059a): step(c); return 1;   /* Player::Update's HUD step: its bits (0x600090), before it compares them */
    }
    return 0;
}
