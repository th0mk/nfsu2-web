/* bf_raceai.c -- Bayview Forever's Race AI fixes: fewer AI blocks and "PIT on
 * the pass" moves on the player, no blocking at a race's start, and the
 * career's catch-up (rubber band) off as an option.
 *
 * The game (SPEED2.EXE v1.2 NA, as read for this file; the names are the mod's):
 *  - AI_FindCarToBlock 0x414130, cdecl (brain, CarObj **out) -> the index of the
 *    car to block or -1; *out the car (0 when none).  brain: the AI's racer
 *    brain (+0 its vehicle, +4 the race's entry count, +8 the entries, 0x30
 *    each, +0x308 (s8) the AI's own entry; entry +0xC half the car's length,
 *    +0x10 its distance along the route, +0x2C (s16) its world object's index
 *    in [0x89E970]).  It takes the first entry behind the AI's own that is
 *    within [0x7836A4] m of it; its gap (0x60BFF0: the other's +0x10 minus the
 *    own +0x10; with the AI's block style's flag ([0x81CB60] + style*8 +0x938,
 *    style = racer +0x902) the two half-lengths are added: bumper to bumper) is
 *    [esp+0x10]; at 0x4142DA..0x4142E9: FLD gap; FCOMP [0x7D7D3C] (0.0); JP fail
 *    unless gap < 0.  The other tests follow (gap > -[0x7836A0], speeds ...).
 *    Its three callers: 0x41CC55 (the running block still valid? -1 ends it,
 *    another car re-targets it, 0x41BC60) and AI_ChooseAction / AI_ChooseAction2
 *    (0x42D36A, 0x42D96A: a block starts on the car found).
 *  - The pass (0x42B940, thiscall action, (brain)): target = [action +0x218]
 *    (a world object), its entry by 0x60C120 (thiscall brain (obj)); gap =
 *    0x60BFF0 (its distance minus the AI's, centre to centre); at 0x42B97D
 *    FCOMP gap,[0.0]; TEST AH,41h; JNE: gap <= 0 (the two cars level) ends the
 *    pass, the AI steers back to its racing line (0x420D40) -- into the car it
 *    passed, which is still beside it ("PIT on the pass").
 *  - Catch Up: the race option [0x89E7F8] (career races: on); the AI's rubber
 *    band reads it at 0x4084B9 (AI_CatchUpRubberBand) and 0x41757B
 *    (AI_UpdateCatchUpSpeedScale), MOV EAX,[0x89E7F8]; TEST EAX,EAX; JE (off).
 *    (The catch-up between two human players, 0x5F6740, reads it elsewhere:
 *    mprace.c's LAN races keep it.)
 *  - The world's clock [0x86518C], 4000 ticks a second, reset to 4000 at each
 *    world load (0x576D01).
 *  - A human racer: WorldObj_IsHumanRacer 0x614880: [[[obj +0x1C] +0x14] +4] == 1.
 *
 * What the mod does (BayviewForever.asi 0x712D5490, the patches 0x712D55A3..):
 *  - DisableCatchUp: both reads become XOR EAX,EAX (31 C0 90 90 90).  Here: the
 *    TEST EAX,EAX after each read sees 0 (HOOK_SITE 0x4084BE, 0x417580).
 *  - BlockMinGap > 0: the 0.0 of 0x4142DE becomes -BlockMinGap: a car is blocked
 *    only when it is more than BlockMinGap m behind (and the running block's
 *    re-check at 0x41CC55 ends it once the car behind is closer).  Here: at
 *    0x4142DA a gap in [-BlockMinGap, 0) is made positive, so the same compare
 *    fails just as the mod's would; any other gap goes on untouched.
 *  - PassClearGap > 0: the CALL 0x60C120 at 0x42B95C goes through 0x712D5220,
 *    which sets the threshold of 0x42B97D (the operand now points at it) to
 *    -(half length of the target + half length of the AI + PassClearGap) when
 *    the target is a human (0 otherwise): the pass ends once the AI's rear
 *    bumper is PassClearGap m ahead of the player's nose.  A half length is the
 *    entry's +0xC when it is in (0.5, 10), else 2.25.  Here: at 0x42B97D the
 *    gap (st0) gets that sum added, the compare against 0 is the same.
 *  - The three calls of AI_FindCarToBlock go through wrappers (0x712D65D0 for
 *    0x41CC55, 0x712D6680 for the other two).  Per AI (a record by brain, 16 at
 *    most a race) and per race (a new race: the clock back by more than 1 s, or
 *    60 s of real time since the last call; its start is the clock at the first
 *    call), on a found car:
 *      choose: within BlockStartDelay s of the start: no block on anyone (*out =
 *      0, -1).  A human target: the first time it picks the player (or after 1 s
 *      with no pick of the player) starts its clock; BlockPlayer = 0: never;
 *      before BlockDelay s on that clock: not yet.  Otherwise the game's block.
 *      still valid: the player's pick time kept fresh, nothing refused.
 *    (Delays truncated to ticks: (int)(4000 * s).)  The rest is the mod's log.
 *  - Five more call sites (the pass's racing-line calls 0x42B9C9 / 0x42B9F8 and
 *    the three pass starts 0x42B858 / 0x42D3C5 / 0x42D9C5, CALL 0x42B5B0) are
 *    hooked for its log only: not needed here.
 * Here the wrapper is an override of 0x414130 told apart by its return address
 * (its only callers).  One change from the mod: a new race is told by the world
 * clock alone (back by more than 1 s, or 60 s of it with no call): the mod's 60
 * real seconds would count a long pause as a new race.
 *
 * The port's own races: the network races (Online, LAN, free roam's
 * multiplayer races, mprace.c) have no AI racers, so no blocks and no passes;
 * their human catch-up is not the AI's.  A career race of co-op's two players
 * (coop.c) has two humans: both are "the player" to these rules (BlockPlayer
 * protects both; each AI keeps one pick clock, as in the mod).  Free roam's
 * traffic and friends' cars are no race AI.
 *
 * Settings ([RaceAI]; the page writes them):
 *   NFSU2_BF_FEATURES_RACEAI         0/1, default 1
 *   NFSU2_BF_RACEAI_BLOCKMINGAP      m, 0..50, default 30   (0 = the game)
 *   NFSU2_BF_RACEAI_BLOCKSTARTDELAY  s, 0..120, default 20
 *   NFSU2_BF_RACEAI_BLOCKDELAY       s, 0..30, default 1.5
 *   NFSU2_BF_RACEAI_PASSCLEARGAP     m, 0..10, default 5
 *   NFSU2_BF_RACEAI_BLOCKPLAYER      0/1, default 1
 *   NFSU2_BF_RACEAI_DISABLECATCHUP   0/1, default 0
 */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);

#define G_CLOCK         0x0086518cu   /* the world's clock, 4000 a second */
#define G_CATCHUP       0x0089e7f8u   /* the race option Catch Up */
#define TICKS           4000
#define BR_OWN          0x308u        /* brain: s8 its own entry */
#define BR_ENTRIES      0x08u         /* brain: the entries, 0x30 each */
#define EN_HALFLEN      0x0cu         /* entry: half the car's length */
#define ACT_TARGET      0x218u        /* the pass action: the car passed (a world object) */
#define RET_STILL_VALID 0x0041cc5au   /* 0x414130's callers, by their return addresses */
#define RET_CHOOSE      0x0042d36fu
#define RET_CHOOSE2     0x0042d96fu

static struct {
    int on, block_player, no_catchup;
    float min_gap, start_delay, delay, pass_gap;
} S = { -1, 1, 0, 30.f, 20.f, 1.5f, 5.f };

static int env_on(const char *name, int def){
    const char *e = getenv(name);
    if (!e || !*e) return def;
    return *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
}
static float env_f(const char *name, float def, float lo, float hi){
    const char *e = getenv(name);
    float v = e && *e ? (float)atof(e) : def;
    return v < lo ? lo : v > hi ? hi : v;        /* the mod's clamps */
}
static void raceai_init(void){
    if (S.on >= 0) return;
    S.on = env_on("NFSU2_BF_FEATURES_RACEAI", 0);
    S.min_gap = env_f("NFSU2_BF_RACEAI_BLOCKMINGAP", 30.f, 0.f, 50.f);
    S.start_delay = env_f("NFSU2_BF_RACEAI_BLOCKSTARTDELAY", 20.f, 0.f, 120.f);
    S.delay = env_f("NFSU2_BF_RACEAI_BLOCKDELAY", 1.5f, 0.f, 30.f);
    S.pass_gap = env_f("NFSU2_BF_RACEAI_PASSCLEARGAP", 5.f, 0.f, 10.f);
    S.block_player = env_on("NFSU2_BF_RACEAI_BLOCKPLAYER", 1);
    S.no_catchup = env_on("NFSU2_BF_RACEAI_DISABLECATCHUP", 0);
    if (S.on)
        fprintf(stderr, "[bf-ai] Race AI fixes: BlockMinGap=%.1f m, BlockDelay=%.2f s, BlockStartDelay=%.0f s, BlockPlayer=%d, "
                "PassClearGap=%.1f m (player only), DisableCatchUp=%d\n", (double)S.min_gap, (double)S.delay, (double)S.start_delay,
                S.block_player, (double)S.pass_gap, S.no_catchup);
    else fprintf(stderr, "[bf-ai] Race AI fixes off\n");
}

static int okm(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
/* WorldObj_IsHumanRacer (0x614880) */
static int is_human(uint32_t obj){
    if (!okm(obj + 0x1cu, 4)) return 0;
    uint32_t x = MR32(obj + 0x1cu);
    if (!okm(x + 0x14u, 4)) return 0;
    uint32_t y = MR32(x + 0x14u);
    return okm(y + 4u, 4) && MR32(y + 4u) == 1u;
}
static float half_len(uint32_t entry){
    float v = okm(entry + EN_HALFLEN, 4) ? MRF32(entry + EN_HALFLEN) : 0.f;
    return v > 0.5f && v < 10.f ? v : 2.25f;
}

/* ---- per race, per AI ---------------------------------------------------- */
#define NREC 16
typedef struct { uint32_t brain; int32_t first, last; uint8_t held, blocked, told_no, told_start; } Rec;
static Rec g_rec[NREC];
static int g_nrec, g_race_on;
static int32_t g_race_start, g_last_now;
static unsigned g_races;
static Rec *rec_of(uint32_t brain, int32_t now){
    if (!g_race_on || now + TICKS < g_last_now || now - g_last_now > 60 * TICKS){
        g_race_on = 1; g_race_start = now; g_nrec = 0; g_races++;
        fprintf(stderr, "[bf-ai] f=%ld race %u: Catch Up option %u%s\n", autotest_frame(), g_races, MR32(G_CATCHUP),
                S.no_catchup ? " (ignored: DisableCatchUp=1)" : "");
    }
    g_last_now = now;
    for (int i = 0; i < g_nrec; i++) if (g_rec[i].brain == brain) return &g_rec[i];
    Rec *r = &g_rec[g_nrec < NREC ? g_nrec++ : NREC - 1];
    memset(r, 0, sizeof *r); r->brain = brain;
    return r;
}

/* cdecl (brain, CarObj **out) -> index or -1: AI_FindCarToBlock */
void sub_00414130_orig(CPU *c);
void sub_00414130(CPU *c){
    raceai_init();
    uint32_t ret = MR32(c->esp), brain = MR32(c->esp + 4u), out = MR32(c->esp + 8u);
    sub_00414130_orig(c);
    if (!S.on || (int32_t)c->eax == -1 || !okm(out, 4)) return;
    uint32_t target = MR32(out);
    if (!target) return;
    int32_t now = (int32_t)MR32(G_CLOCK);
    Rec *r = rec_of(brain, now);
    int human = is_human(target);
    if (ret == RET_STILL_VALID){                 /* the running block: the player's pick kept fresh */
        if (human && r->first) r->last = now;
        return;
    }
    if (ret != RET_CHOOSE && ret != RET_CHOOSE2) return;
    float t = (float)now / TICKS;
    if (now - g_race_start < (int32_t)((float)TICKS * S.start_delay)){
        if (!r->told_start){ r->told_start = 1;
            fprintf(stderr, "[bf-ai] f=%ld START: AI 0x%08x wanted to block %s at %.1f s (%.1f s into the race), not allowed yet\n",
                    autotest_frame(), brain, human ? "the player" : "an AI", (double)t, (double)(now - g_race_start) / TICKS); }
        goto refuse;
    }
    if (!human) return;
    if (!r->first || now - r->last > TICKS){ r->first = now; r->held = r->blocked = r->told_no = 0; }
    r->last = now;
    if (!S.block_player){
        if (!r->told_no){ r->told_no = 1;
            fprintf(stderr, "[bf-ai] f=%ld NO BLOCK: AI 0x%08x wanted to block the player at %.1f s (BlockPlayer=0)\n", autotest_frame(), brain, (double)t); }
        goto refuse;
    }
    if ((int32_t)((float)TICKS * S.delay) > now - r->first){
        if (!r->held){ r->held = 1;
            fprintf(stderr, "[bf-ai] f=%ld HOLD: AI 0x%08x wants to block the player at %.1f s, waiting %.2f s\n", autotest_frame(), brain,
                    (double)t, (double)S.delay); }
        goto refuse;
    }
    if (!r->blocked){ r->blocked = 1;
        fprintf(stderr, "[bf-ai] f=%ld BLOCK: AI 0x%08x blocks the player at %.1f s (target for %.2f s)\n", autotest_frame(), brain,
                (double)t, (double)(now - r->first) / TICKS); }
    return;
refuse:
    MR32(out) = 0;
    c->eax = 0xffffffffu;
}

int bf_raceai_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x004084be):                  /* AI_CatchUpRubberBand: test eax, eax (eax = Catch Up) */
    case HOOK_SITE(0x00417580):                  /* AI_UpdateCatchUpSpeedScale: the same */
        raceai_init();
        if (S.on && S.no_catchup) c->eax = 0;
        return 1;
    case HOOK_SITE(0x004142da): {                /* AI_FindCarToBlock: fld gap; fcomp 0.0 (a block only on a car behind) */
        raceai_init();
        if (!S.on || S.min_gap <= 0.f) return 1;
        float g = MRF32(c->esp + 0x10u);
        if (g < 0.f && g >= -S.min_gap) MRF32(c->esp + 0x10u) = 1.f;   /* not BlockMinGap behind: the compare fails, -1 */
        return 1;
    }
    case HOOK_SITE(0x0042b97d): {                /* the pass: fcomp gap (st0), 0.0; <= 0 ends the pass */
        raceai_init();
        if (!S.on || S.pass_gap <= 0.f) return 1;
        uint32_t act = c->esi, brain = c->edi, entry = c->ebp;          /* entry: the target's (not 0 here) */
        uint32_t target = okm(act + ACT_TARGET, 4) ? MR32(act + ACT_TARGET) : 0u;
        if (!target || !is_human(target) || !okm(brain + BR_OWN, 1)) return 1;
        uint32_t own = MR8(brain + BR_OWN);
        if (own > 15u) return 1;
        uint32_t mine = brain + BR_ENTRIES + own * 0x30u;
        float k = half_len(entry) + half_len(mine) + S.pass_gap;
        c->st[c->top & 7] += (double)k;           /* gap + k <= 0  <=>  gap <= -k: the mod's threshold */
        return 1;
    }
    }
    return 0;
}
