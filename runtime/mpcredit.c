/* mpcredit.c -- a race of the career won in a multiplayer race from free roam: the game's own win of it (mpcredit.h).
 *
 * The game (SPEED2.EXE v1.2 NA), as read for this file.  A career race's end
 * (the post-race list's last step 0x4A3E30 -> 0x54C9D0 -> 0x54C940, on the
 * Career 0x85AD40) asks the race's results whether the player won (0x5287A0)
 * and then calls RaceWon 0x541420(the race's hash), else Lost 0x528500.
 * RaceWon:
 *  - the race's event entry (+0x7AFC of them at +0x7B00, 8 bytes: +0 hash, +6
 *    1 won) gets +6 = 1.  It must exist (for a race never engaged there is
 *    none, and the game writes to address 6), as StartRace 0x541160 makes it
 *    (0x512F10: the entry, a new one if none);
 *  - when the results say to pay (results +0xD26 set and +0xD27 not; results
 *    at +0xD24) and a race is being run (+0x8730): its prize 0x532E50 into the
 *    bank (+0x7134; a sponsor race, kind 1: the signed sponsor's word +2,
 *    sponsor at stage record +0xE8 of the current stage; else the race's
 *    +0x30) and the margin 0x513FE0 into the race's stage record by 0x513430
 *    (+0xE0 the points, +0xE4 the races: the stage's average is what later
 *    sponsors ask for, 0x5005E0): the points 0x512B50 by the race's type
 *    (0x512AE0, the table 0x7F7AC8): 100, 200, 300 from a margin of 1, 2 and
 *    5 (one type 10, 20, 30), none below.  The margin is the next finisher's
 *    time over the winner's, (1 - t1 / t2) x 100 (a drift race's points,
 *    p1 / p2 - 1, x 100); 100 when nobody else finished or the race was won
 *    without racing (results +0xD24 set);
 *  - then the cover shoots tied to the race (0x5337D0 on +0x4210), its text
 *    messages (0x529920 on +0x5D20), the race into its stage record's lists
 *    (0x513390: 35 normal, 4 sponsor and 10 URL races; the stage's wins,
 *    0x5004D0, count the normal ones), the stage's end (0x532C60 on that
 *    stage record: its final event won and its cover shoots done, or with no
 *    final every race of the stage won -> 0x528320, the next stage), the
 *    career's events (0x5242C0(race, 1) on +0x887C) and the story's steps
 *    (0x533B40 on +0xC00).
 * The game's own "won without racing" (0x541680, from the debug switches at
 * 0x5417F0) runs StartRace, sets results +0xD24, RaceWon, signs a random
 * sponsor when the stage wants one, and makes the open races again
 * (0x527D30).  After a career race the post-race list (0x4F3EA0) also offers
 * the sponsors (UI_SponsorPopup, UI_Sponsorship_new) when 0x4A4610 says so
 * (the career; the current stage, past the first, wants a sponsor, 0x500610:
 * none signed and one whose wins asked for, stage info +0x1C, the stage has;
 * one of the stage info's sponsors, +1 of them at +8, asks for no more
 * reputation, its +0xE, than the stage before's average), and after some
 * races a movie (0x513A50: the race's scene, the race table's string +4;
 * Scene07 into the second stage).  Those screens work only inside that list
 * ([0x8363DC]: the popup's Accept and the signing's Yes step it on, 0x4BFF80).
 *
 * Here, in the crib on the way back from the race (after its snapshot of the
 * profile, so that Return to Explore Mode's autosave saves the change):
 *  1. the event entry (0x512F10), and the career's count of races started
 *     (+0x872C) one up, as StartRace does;
 *  2. the prize into the bank and the margin into the race's stage record,
 *     as RaceWon's paying part does, with the margin from the multiplayer
 *     race's results (mpcredit.h): the winner's, a friend's win (the player
 *     came second or later) too; below 0 the race type's least points;
 *  3. RaceWon with its paying part off (results +0xD26 cleared for the call,
 *     then put back), so every other step is the game's own;
 *  4. the open races made again (0x527D30), as the game's own helper does:
 *     the race leaves the map, the career opens the next ones;
 *  5. the sponsors the game would offer now (0x4A4610's test, the ones the
 *     sponsor screen lists: the stage info's first ones whose wins and
 *     reputation the career has): the player picks one in the crib, before
 *     Return to Explore Mode (mpcredit_frame).  The game's sponsor screens
 *     step on only inside the post-race list ([0x8363DC]: the popup's Accept
 *     and the signing's Yes call its 0x4BFF80; at the list's end nothing
 *     follows in the career), and one that is not up crashes them.  So the
 *     game's generic dialog (0x558750, as roam_game.c's Solo / Multiplayer
 *     question; the front end's GenericDialog_MED, IG_ is the world's) over
 *     the crib asks, one offer after the other: "Sponsor offer 1 of 3: <name>.
 *     Signing bonus $X, its races pay $Y." [Sign] [Next offer] (Back: the
 *     next one too; the game's popup has no way out either).  Its answer
 *     goes to the crib's message handler (0x4AF3A0, patched: only its
 *     vtable calls it).  Sign: Career 0x532CF0 (the bonus, +0xC, into the
 *     bank).  The name: the game's text for "SPONSOR_<the sponsor's name>"
 *     (0x5129F0 -> 0x4FFA80), as the sponsor screen shows it;
 *  6. the race's movie, as the post-race list's end plays it (0x4A3E30:
 *     0x513A50 the scene, 0x53D7A0 plays it): the race table's string +4
 *     (S2_URL_1 SCENE08, S2_URL_3 SUBURBAN, S3_URL_8 INDUSTEAST, S4_URL_7
 *     INDUSTWEST, S5_URL_4 SCENE12), Scene07 and UpperClass into the second
 *     stage, Scene15 after S5_CIRCUIT_36.  As after a Solo race (the post-
 *     race list's save, then the movie, then the world) it comes between
 *     Return to Explore Mode's save and the world: the crib's message once
 *     the save is over (0x251BF83E; with nothing to save 0x9A3B571D) is held
 *     in its patched handler, the movie plays (the names at 0x8382A0,
 *     IG_PlayMovie.fng added over the others by 0x555E80, as 0x4C57C0 adds
 *     PC_Loading; its end mode [0x838568] 5: in the front end it takes
 *     itself away, 0x558F00, and nothing follows), then the crib's handler
 *     gets 0x9A3B571D: the world, as it goes there with no save to make.
 *     A playing movie puts the front end's screens away: after it nothing
 *     there takes keys and the crib's own objects are freed (the held
 *     0x251BF83E, the screens' "save is over" 0x538CA0, wrote into them),
 *     so nothing of the front end's comes after it, as in the game (mode 4
 *     post-race: the world by the race coordinator; mode 6 Resume Career:
 *     the world).  Played in the crib before Return to Explore Mode the
 *     front end took no more keys (the save's prompt then waited for good).
 * While the question is up or the movie plays the trip waits
 * (mpcredit_busy): the input is the player's, the page's cover off.
 * A race already won, or not in the table, is left (0).  In free roam (the
 * trip missed the crib) the win waits for the next crib (mpcredit_frame):
 * RaceWon there could move the career to the next stage under the running
 * world. */
#include "cpu.h"
#include "mpcredit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
int career_fe_active(const char *name);
void dispatch_indirect(CPU *c, uint32_t addr);

#define G_CAREER        0x0085ad40u
#define G_GAMEMODE      0x00850078u   /* 1 career */
#define G_RACES         0x008383e0u   /* the career's race table ([0x8383E4] races of 0x88 bytes) */
#define G_NRACES        0x008383e4u
#define G_STRPOOL       0x00838428u   /* the career tables' strings: a race's name at +[race +0] (u16) */
#define G_SNAPSHOT      0x00851148u   /* the crib's snapshot of the profile (0: none) */
#define G_POINTS        0x007f7ac8u   /* the margin's points by race type: 7 x 0x1C (+0 type, +4 its first step) */
#define RACE_SZ         0x88u
#define C_RESULTS       0xd24u        /* the race's results: +2 pay, +3 do not pay (RaceWon) */
#define C_BANK          0x7134u
#define C_OPEN          0x71c8u       /* the races open now (64) */
#define C_STAGEREC      0x72d8u       /* 8 stage records of 0x104 bytes */
#define C_STAGE         0x7af8u
#define C_NEVENTS       0x7afcu
#define C_EVENTS        0x7b00u
#define C_STARTED       0x872cu       /* races started (StartRace) */
#define SR_SIZE         0x104u
#define SR_POINTS       0xe0u         /* float: the margins' points */
#define SR_RUNS         0xe4u         /* ...of this many races */
#define SR_SPONSOR      0xe8u         /* the sponsor signed (0 none) */
#define SR_INFO         0x100u        /* the stage's info: +1 its sponsors, +8 them, +0x1C the wins each asks for (int16) */
#define SP_PRIZE        0x02u         /* a sponsor: its races' prize (u16) */
#define SP_REP          0x0eu         /* ...the reputation it asks for (int16) */
#define FN_ENTRY        0x00512f10u   /* thiscall Career (hash): the event entry, made if none */
#define FN_RACEWON      0x00541420u   /* thiscall Career (hash), ret 4 */
#define FN_MARGIN_REC   0x00513430u   /* thiscall stage record (float margin, race), ret 8 */
#define FN_RACE_TYPE    0x00512ae0u   /* cdecl (race): the margin's race type, 1..7 (0 none) */
#define FN_OPEN_LIST    0x00527d30u   /* thiscall Career: the open races made again */
#define FN_WANTS_SP     0x00500610u   /* thiscall stage record -> al: it wants a sponsor */
#define FN_SIGN         0x00532cf0u   /* thiscall Career (sponsor), ret 4: signed (with its bonus) when none is */
#define SP_BONUS        0x0cu         /* a sponsor: its signing bonus (int16) */
#define FN_SP_NAMEHASH  0x005129f0u   /* thiscall sponsor: the hash of "SPONSOR_<its name>" */
#define FN_TEXT         0x004ffa80u   /* cdecl (hash): the game's text for it */
#define FN_MALLOC       0x00575620u
#define FN_DIALOG2      0x00558750u   /* cdecl (from package, dialog package, label1, label2, msg1, msg2, back msg, first, fmt, ...) */
#define G_DLG           0x00838974u   /* the dialog up now (0 none) */
#define FN_CRIB_MSG     0x004af3a0u   /* UI_CareerCrib's message handler (thiscall msg, obj, a3, a4; ret 0x10) */
#define G_FEMGR_LIST    0x008637b4u   /* the FE manager's screens: n = [it]; until n == it: screen [n - 4 + 0x14], n = [n] */
#define G_MOVIES        0x008382a0u   /* the movies to play: 4 names of 0x40 */
#define G_MOVIE_AT      0x0083856cu   /* ...the one playing */
#define G_MOVIE_MODE    0x00838568u   /* what follows the movie screen (0x54AF80): 5 nothing */
#define G_FE            0x008384c4u   /* the FE manager */
#define FN_FE_ADD       0x00555e80u   /* cdecl (package, 0): the package added, over the others */
#define FN_MOVIE_PREP   0x0043b640u   /* thiscall 0x827E88: as 0x53D7A0 before its movie */
#define MSG_CRIB_GO     0x451e768eu   /* to the crib: "Return to Explore Mode?" Yes (0x4AF360: the save, if any) */
#define MSG_SAVE_WORLD  0x461a18eeu   /* to the crib from the memory card, its save over: on to the world (first) */
#define MSG_SAVE_DONE   0x251bf83eu   /* to the crib: its save is over (the memory card, 0x4A31D0): on to the world */
#define MSG_CRIB_WORLD  0x9a3b571du   /* to the crib: no save to make: the world (0x5404A0) */
#define S_PLAYMOVIE     0x00799ef0u   /* "IG_PlayMovie.fng" */
#define G_MOVIES_DONE   0x008383a8u   /* 1: the movie screen played the last of them (0x55B088) */
#define RACE_LAST       0x04cce3e1u   /* S5_CIRCUIT_36, the career's last race (0x513A50: Scene15) */
#define MSG_SIGN        0x4d504301u   /* our answers (not the game's messages: its screens ignore them) */
#define MSG_NEXT        0x4d504302u
#define HOOK_MAINLOOP   0x00581475u

static int okm(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t rd(uint32_t a){ return okm(a, 3) ? MR32(a) : 0; }
static uint32_t call(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 512u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(HOOK_MAINLOOP);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static const char *race_name(uint32_t r){
    uint32_t s = rd(G_STRPOOL) + MR16(r);
    return okm(s, 63) ? (const char *)(g_mem + s) : "?";
}
static int race_in_table(uint32_t r){
    uint32_t t = rd(G_RACES), n = rd(G_NRACES);
    return t && n <= 1024 && r >= t && r < t + n * RACE_SZ && (r - t) % RACE_SZ == 0 && okm(r, RACE_SZ);
}
static int event_won(uint32_t h){
    uint32_t n = rd(G_CAREER + C_NEVENTS);
    for (uint32_t i = 0; i < n && i < 384; i++) if (MR32(G_CAREER + C_EVENTS + 8u * i) == h) return MR8(G_CAREER + C_EVENTS + 8u * i + 6u) == 1;
    return 0;
}
static int race_open(uint32_t r){ for (uint32_t i = 0; i < 64; i++) if (rd(G_CAREER + C_OPEN + 4u * i) == r) return 1; return 0; }
static uint32_t stage_rec(uint32_t s){ return s < 8 ? G_CAREER + C_STAGEREC + s * SR_SIZE : 0; }
/* the stage's wins as 0x5004D0 counts them: its normal races' list (35) */
static int stage_wins(uint32_t sr){ int n = 0; for (uint32_t i = 0; i < 35; i++) n += rd(sr + 4u * i) != 0; return n; }
static float stage_rep(uint32_t sr){ int runs = (int)rd(sr + SR_RUNS); return runs > 0 ? MRF32(sr + SR_POINTS) / (float)runs : 0.f; }

static MpCredit g_last;
int mpcredit_last(MpCredit *o){ *o = g_last; return g_last.race != 0; }

/* the sponsors the game would offer now (0x4A4610 without its race: the career, the current stage past the first
 * wants one, and those of its sponsors whose wins the stage has and whose reputation the stage before's average
 * is, as 0x500670 counts them; the sponsor screen lists the stage info's first that many, 0x5006D0), in order */
#define OFFERS_MAX 5
static int sponsor_offers(CPU *c, uint32_t *o){
    uint32_t st = rd(G_CAREER + C_STAGE), sr = stage_rec(st), info = sr ? rd(sr + SR_INFO) : 0;
    if (rd(G_GAMEMODE) != 1u || st <= 1 || st >= 8 || !okm(info, 0x30)) return 0;
    if (!(call(c, FN_WANTS_SP, sr, 0, 0) & 0xffu)) return 0;
    float rep = stage_rep(sr - SR_SIZE); int wins = stage_wins(sr), n = (int8_t)MR8(info + 1u), k = 0;
    for (int i = 0; i < n && i < OFFERS_MAX; i++){
        uint32_t sp = rd(info + 8u + 4u * (uint32_t)i);
        if (okm(sp, 0x10) && wins >= (int16_t)MR16(info + 0x1cu + 2u * (uint32_t)i) && (float)(int16_t)MR16(sp + SP_REP) <= rep) k++;
    }
    for (int i = 0; i < k; i++) o[i] = rd(info + 8u + 4u * (uint32_t)i);
    return k;
}

/* ---- after the win, in the crib: the sponsor offer, the race's movie (mpcredit_frame) -------- */
enum { UI_NONE, UI_OFFER, UI_ASKED, UI_WAIT, UI_MOVIE, UI_PLAYING };
static struct {
    int      st;
    uint32_t offer[OFFERS_MAX]; int n, i;      /* the sponsors offered, the one asked about */
    char     scene[4][16]; int nscene;         /* the races' movies (the game plays up to 4 in a row) */
    uint32_t answer;                           /* MSG_SIGN / MSG_NEXT, from the crib's handler */
    long     at; int seen;
    uint32_t str, lab_sign, lab_next; int patched;
    int      leaving;                          /* the crib's Return to Explore Mode went (its Yes, 0x451E768E) */
    uint32_t held_this, held[3][4]; int nheld; /* the crib's messages that go on to the world, held for the movie */
} g_ui;
/* (a movie that waits for Return to Explore Mode is not: the trip goes on to it) */
int mpcredit_busy(void){ return g_ui.st != UI_NONE && g_ui.st != UI_WAIT; }
/* movies on: NOMOVIES not mode 1, as fileio.c reads it (the page sets 1 where its movies are not kept; the movie
 * files then do not open, and the movie screen would wait for good: the world comes without it, as before) */
static int movies_on(void){
    const char *e = getenv("NOMOVIES");
    return ((e && *e && *e != '0') ? atoi(e) : 0) != 1;
}
/* the race's movie, as 0x513A50 picks it (the race won; stage_now: the stage after the win) */
static void race_scene(uint32_t r, uint32_t race_stage, uint32_t stage_now){
    char nm[16] = "";
    uint32_t s = rd(G_STRPOOL) + MR16(r + 4u);
    if (okm(s, 15) && MR8(s)){ for (int i = 0; i < 15 && MR8(s + (uint32_t)i); i++) nm[i] = (char)MR8(s + (uint32_t)i); }
    else if (race_stage != stage_now && stage_now == 2u) snprintf(nm, sizeof nm, "Scene07");
    else if (MR32(r + 8u) == RACE_LAST) snprintf(nm, sizeof nm, "Scene15");
    if (!nm[0] || g_ui.nscene >= 4) return;
    snprintf(g_ui.scene[g_ui.nscene++], sizeof g_ui.scene[0], "%s", nm);
    if (!strcasecmp(nm, "Scene07") && g_ui.nscene < 4) snprintf(g_ui.scene[g_ui.nscene++], sizeof g_ui.scene[0], "UpperClass");   /* (0x53D7A0) */
}
/* a sponsor's name as the sponsor screen shows it: the game's text (Latin-1), and as UTF-8 */
static void sponsor_name(CPU *c, uint32_t sp, char *lat, int nl, char *utf, int nu){
    uint32_t h = call(c, FN_SP_NAMEHASH, sp, 0, 0), t = call(c, FN_TEXT, 0, 1, &h);
    int a = 0, b = 0;
    for (uint32_t i = 0; okm(t + i, 1) && MR8(t + i) && i < 40; i++){
        uint8_t ch = MR8(t + i);
        if (ch < 0x20 || ch == '%') ch = ' ';
        if (a + 1 < nl) lat[a++] = (char)ch;
        if (ch < 0x80){ if (b + 1 < nu) utf[b++] = (char)ch; }
        else if (b + 2 < nu){ utf[b++] = (char)(0xc0 | ch >> 6); utf[b++] = (char)(0x80 | (ch & 0x3f)); }
    }
    lat[a] = 0; utf[b] = 0;
}
static void gs(uint32_t a, const char *s, uint32_t cap){ uint32_t i = 0; for (; s[i] && i + 1 < cap; i++) MR8(a + i) = (uint8_t)s[i]; MR8(a + i) = 0; }
/* the crib screen's package name (the FE's own string), else ours */
static uint32_t crib_name(void){
    uint32_t n = rd(G_FEMGR_LIST);
    for (int k = 0; n && n != G_FEMGR_LIST && k < 256; k++, n = rd(n)){
        uint32_t sc = rd(n - 4u + 0x14u), nm = sc ? rd(sc + 4u) : 0;
        if (okm(nm, 18) && !strcasecmp((const char *)(g_mem + nm), "UI_CareerCrib.fng")) return nm;
    }
    gs(g_ui.str + 0x200u, "UI_CareerCrib.fng", 0x20);
    return g_ui.str + 0x200u;
}
/* the dialog's answer goes to the crib's handler: ours, while we ask; the rest the game's */
void sub_004af3a0(CPU *c);
void dispatch_patch(uint32_t a, void (*f)(CPU *));
uint32_t language_add_text(const char *name, const char *text);
void language_texts_frame(CPU *c);
static void crib_msg(CPU *c){
    uint32_t m = MR32(c->esp + 4u);
    if ((m == MSG_SIGN || m == MSG_NEXT) && g_ui.st == UI_ASKED){
        g_ui.answer = m; POP32(c->eip); c->esp += 0x10u;
        fprintf(stderr, "[mpcredit] f=%ld the answer: %s\n", autotest_frame(), m == MSG_SIGN ? "Sign" : "Next offer");
        return;
    }
    /* a race's movie waits: the crib's way to the world after Return to Explore Mode is held (the save over: the
     * memory card's 0x461A18EE, the world, then 0x251BF83E; no save to make: 0x9A3B571D), the movie plays, then
     * they go on as they came (ui_frame) */
    if (g_ui.st == UI_WAIT && m == MSG_CRIB_GO) g_ui.leaving = 1;
    if ((g_ui.st == UI_WAIT || (g_ui.st == UI_MOVIE && g_ui.nheld)) && g_ui.leaving && g_ui.nheld < 3
        && (m == MSG_SAVE_WORLD || m == MSG_SAVE_DONE || m == MSG_CRIB_WORLD)){
        if (!g_ui.nheld){ g_ui.held_this = c->ecx; g_ui.at = autotest_frame(); }
        for (uint32_t k = 0; k < 4; k++) g_ui.held[g_ui.nheld][k] = MR32(c->esp + 4u + 4u * k);
        g_ui.nheld++;
        g_ui.st = UI_MOVIE; POP32(c->eip); c->esp += 0x10u;
        fprintf(stderr, "[mpcredit] f=%ld the crib's way to the world (%08x) held: the race's movie first\n", autotest_frame(), m);
        return;
    }
    sub_004af3a0(c);
}
/* The question in the game's language, as roam_game.c's race card asks its own: the languages whose fonts draw their
 * letters (language_game_letters), English for the others; UTF-8 here, one row of one brace each (the log-english
 * test takes such a row for the game's text).  offer: the offer's number, how many, the sponsor, its bonus, its
 * races' prize, in that order in every language */
static const struct { const char *lang, *offer, *sign, *next; } SP_TX[] = {
    { "english", "Sponsor offer %d of %d: %s. Signing bonus %d, its races pay %u. Sign with them? No: the next offer.", "Sign", "Next offer" },
    { "french", "Offre de sponsor %d sur %d : %s. Prime de signature %d, ses courses rapportent %u. Signer avec eux ? Non : l'offre suivante.", "Signer", "Offre suivante" },
    { "german", "Sponsorangebot %d von %d: %s. Unterschriftsbonus %d, seine Rennen zahlen %u. Unterschreiben? Nein: das nächste Angebot.", "Unterschreiben", "Nächstes Angebot" },
    { "italian", "Offerta sponsor %d di %d: %s. Bonus alla firma %d, le sue gare pagano %u. Firmi con loro? No: l'offerta successiva.", "Firma", "Offerta successiva" },
    { "spanish", "Oferta de patrocinador %d de %d: %s. Prima por firmar %d, sus carreras pagan %u. ¿Firmar con ellos? No: la siguiente oferta.", "Firmar", "Siguiente oferta" },
    { "portuguese", "Oferta de patrocinador %d de %d: %s. Bônus de assinatura %d, as corridas dele pagam %u. Assinar com eles? Não: a próxima oferta.", "Assinar", "Próxima oferta" },
    { "dutch", "Sponsoraanbod %d van %d: %s. Tekenbonus %d, zijn races betalen %u. Tekenen? Nee: het volgende aanbod.", "Tekenen", "Volgend aanbod" },
    { "swedish", "Sponsorerbjudande %d av %d: %s. Signeringsbonus %d, dess race ger %u. Skriva på? Nej: nästa erbjudande.", "Skriv på", "Nästa erbjudande" },
    { "danish", "Sponsortilbud %d af %d: %s. Underskriftsbonus %d, dets løb giver %u. Skrive under? Nej: næste tilbud.", "Skriv under", "Næste tilbud" },
    { "polish", "Oferta sponsora %d z %d: %s. Premia za podpisanie %d, jego wyścigi płacą %u. Podpisać? Nie: następna oferta.", "Podpisz", "Następna oferta" },
    { "turkish", "Sponsor teklifi %d/%d: %s. İmza primi %d, yarışları %u öder. İmzalansın mı? Hayır: sonraki teklif.", "İmzala", "Sonraki teklif" },
    { "hungarian", "Szponzorajánlat %d / %d: %s. Aláírási bónusz %d, versenyenként %u. Aláírod? Nem: a következő ajánlat.", "Aláírás", "Következő ajánlat" },
};
const char *language_name(void);
void language_game_letters(const char *utf8, char *out, size_t n);
static int sp_tx(void){
    const char *l = language_name();
    for (int i = 1; l && i < (int)(sizeof SP_TX / sizeof SP_TX[0]); i++) if (!strcmp(l, SP_TX[i].lang)) return i;
    return 0;
}
static int ui_ready(CPU *c){
    if (!g_ui.patched){ dispatch_patch(FN_CRIB_MSG, crib_msg); g_ui.patched = 1; }
    /* (scratch: the dialog's package +0, its format +0x38, its text +0x40, the crib's name +0x200, the movie's +0x220) */
    if (!g_ui.str){ uint32_t sz = 0x240u; g_ui.str = call(c, FN_MALLOC, 0, 1, &sz); }
    static int lang = -1;
    if (!g_ui.lab_sign || lang != sp_tx()){
        char s[48]; lang = sp_tx();
        language_game_letters(SP_TX[lang].sign, s, sizeof s); g_ui.lab_sign = language_add_text("MPCREDIT_SIGN", s);
        language_game_letters(SP_TX[lang].next, s, sizeof s); g_ui.lab_next = language_add_text("MPCREDIT_NEXT", s);
        language_texts_frame(c);
    }
    return g_ui.str != 0;
}
/* the question about offer g_ui.i over the crib (the game's generic dialog, roam_game.c mp_ask's way: the text as a
 * "%s" argument, with no % in it) */
static int dialog_up(void){ return career_fe_active("GenericDialog_MED.fng") || career_fe_active("GenericDialog_SMALL.fng") || rd(G_DLG); }
static int ui_ask(CPU *c){
    if (!ui_ready(c) || dialog_up()) return 0;
    uint32_t sp = g_ui.offer[g_ui.i];
    char nm[48], utf[64], t[0x1c0], fmt_l[0x100], en[0x1c0];
    sponsor_name(c, sp, nm, sizeof nm, utf, sizeof utf);
    /* in the game's language and letters (the sponsor's name is the game's own already); the log's line in English */
    language_game_letters(SP_TX[sp_tx()].offer, fmt_l, sizeof fmt_l);
    snprintf(t, sizeof t, fmt_l, g_ui.i + 1, g_ui.n, nm, (int)(int16_t)MR16(sp + SP_BONUS), MR16(sp + SP_PRIZE));
    snprintf(en, sizeof en, SP_TX[0].offer, g_ui.i + 1, g_ui.n, nm, (int)(int16_t)MR16(sp + SP_BONUS), MR16(sp + SP_PRIZE));
    uint32_t dl = g_ui.str, fmt = g_ui.str + 0x38u, txt = g_ui.str + 0x40u;
    gs(dl, "GenericDialog_MED.fng", 0x38); gs(fmt, "%s", 8); gs(txt, t, 0x1c0);     /* (the front end's; IG_ is the world's) */
    uint32_t a[10] = { crib_name(), dl, g_ui.lab_sign, g_ui.lab_next, MSG_SIGN, MSG_NEXT, MSG_NEXT, 0, fmt, txt };
    call(c, FN_DIALOG2, 0, 10, a);
    uint32_t language_text(uint32_t hash);
    fprintf(stderr, "[mpcredit] f=%ld asked: \"%s\"%s%s (its buttons' texts %s in the game's table: %08x %08x)\n", autotest_frame(), en,
            sp_tx() ? " in " : "", sp_tx() ? SP_TX[sp_tx()].lang : "", language_text(g_ui.lab_sign) && language_text(g_ui.lab_next) ? "found" : "NOT found",
            g_ui.lab_sign, g_ui.lab_next);
    return 1;
}
/* the race's movie over the crib, as 0x53D7A0 plays its own (the names, mode 5 nothing after it; 0x43B640 on
 * 0x827E88 first: without it the movies stopped after about 2.5 s; the package added over the others, 0x555E80, as
 * 0x4C57C0 adds PC_Loading) */
static int ui_movie(CPU *c){
    if (!ui_ready(c) || !rd(G_FE)) return 0;
    for (uint32_t k = 0; k < 4; k++) MR8(G_MOVIES + 0x40u * k) = 0;
    for (int k = 0; k < g_ui.nscene; k++) gs(G_MOVIES + 0x40u * (uint32_t)k, g_ui.scene[k], 0x40);
    MR32(G_MOVIE_AT) = 0; MR32(G_MOVIE_MODE) = 5; MR32(G_MOVIES_DONE) = 0;
    call(c, FN_MOVIE_PREP, 0x00827e88u, 0, 0);
    uint32_t a[2] = { S_PLAYMOVIE, 0 };
    call(c, FN_FE_ADD, 0, 2, a);
    fprintf(stderr, "[mpcredit] f=%ld the race's movie: %s%s%s\n", autotest_frame(), g_ui.scene[0], g_ui.nscene > 1 ? ", then " : "",
            g_ui.nscene > 1 ? g_ui.scene[1] : "");
    return 1;
}
static void ui_frame(CPU *c){
    long f = autotest_frame();
    int crib = career_fe_active("UI_CareerCrib.fng"), movie = career_fe_active("IG_PlayMovie.fng");
    switch (g_ui.st){
    case UI_OFFER:
        if (crib && !career_fe_active("MC_Main.fng") && ui_ask(c)){ g_ui.st = UI_ASKED; g_ui.at = f; g_ui.answer = 0; }
        break;
    case UI_ASKED:
        if (g_ui.answer == MSG_SIGN){
            uint32_t sp = g_ui.offer[g_ui.i], b0 = rd(G_CAREER + C_BANK), st = rd(G_CAREER + C_STAGE);
            call(c, FN_SIGN, G_CAREER, 1, &sp);
            if (rd(stage_rec(st) + SR_SPONSOR) == sp){
                char nm[48];
                g_last.sponsor = sp; g_last.bonus = (int)(rd(G_CAREER + C_BANK) - b0);
                sponsor_name(c, sp, nm, sizeof nm, g_last.sponsor_name, sizeof g_last.sponsor_name);
            }
            fprintf(stderr, "[mpcredit] f=%ld signed with %s (offer %d of %d): bonus $%d, bank $%u -> $%u%s\n", f, g_last.sponsor_name,
                    g_ui.i + 1, g_ui.n, g_last.bonus, b0, rd(G_CAREER + C_BANK), g_last.sponsor == sp ? "" : " (the game did not sign it)");
            g_ui.st = g_ui.nscene ? UI_WAIT : UI_NONE;
        } else if (g_ui.answer == MSG_NEXT){
            g_ui.i = (g_ui.i + 1) % g_ui.n; g_ui.st = UI_OFFER;
        } else if (!dialog_up() && f - g_ui.at > 30) g_ui.st = UI_OFFER;     /* (gone another way: again) */
        g_ui.answer = 0;
        break;
    case UI_WAIT:                                  /* (crib_msg: Return to Explore Mode, its save over) */
        break;
    case UI_MOVIE:
        if (ui_movie(c)){ g_ui.st = UI_PLAYING; g_ui.at = f; g_ui.seen = 0; }
        else if (f - g_ui.at > 60){ g_ui.st = UI_PLAYING; g_ui.at = f - 100000; }   /* (could not: on to the world) */
        break;
    case UI_PLAYING:
        /* over once the movie screen came and went or said it is done ([0x8383A8]; the player may skip it), one that
         * never comes, or one still up after 10 minutes.  Then the held messages to the crib's handler, as they
         * came: the world.  (0x5404A0 called alone, with the crib's message not held, loaded the world twice: half
         * loaded, or a crash in the sound's streams; the world's loading under a playing movie cut it after 2 s) */
        if (movie) g_ui.seen = 1;
        if ((!movie && (g_ui.seen || rd(G_MOVIES_DONE) == 1u || f - g_ui.at > 300)) || f - g_ui.at > 60 * 600){
            fprintf(stderr, "[mpcredit] f=%ld the race's movie %s: the world, as the crib goes there\n", f,
                    g_ui.seen || rd(G_MOVIES_DONE) == 1u ? "is over" : "did not play");
            g_ui.st = UI_NONE; g_ui.leaving = 0; g_ui.nscene = 0;
            int n = g_ui.nheld; g_ui.nheld = 0;
            for (int i = 0; i < n; i++) call(c, FN_CRIB_MSG, g_ui.held_this, 4, g_ui.held[i]);
        }
        break;
    }
}

static int win_now(CPU *c, uint32_t r, float margin){
    uint32_t h = MR32(r + 8u), kind = MR8(r + 0x34u), rs = MR8(r + 0x37u), st0 = rd(G_CAREER + C_STAGE);
    uint32_t sr = stage_rec(rs), cur = stage_rec(st0);
    if (!sr || !cur){ fprintf(stderr, "[mpcredit] f=%ld %s: stage %u, no stage record: left\n", autotest_frame(), race_name(r), rs); return 0; }
    int open = race_open(r), wins0 = stage_wins(cur);
    uint32_t bank0 = rd(G_CAREER + C_BANK), runs0 = rd(sr + SR_RUNS);
    float pts0 = MRF32(sr + SR_POINTS);
    /* 1: the event entry, and the race started, as StartRace */
    uint32_t e = call(c, FN_ENTRY, G_CAREER, 1, &h);
    if (!okm(e, 8) || MR32(e) != h){ fprintf(stderr, "[mpcredit] f=%ld %s: no event entry (0x%08x): left\n", autotest_frame(), race_name(r), e); return 0; }
    MR32(G_CAREER + C_STARTED) += 1u;
    /* 2: the prize and the margin, as RaceWon's paying part (0x532E50, 0x513FE0 -> 0x513430) */
    uint32_t sp = kind == 1u ? rd(cur + SR_SPONSOR) : 0, prize = sp && okm(sp, 4) ? MR16(sp + SP_PRIZE) : MR32(r + 0x30u);
    if (prize > 1000000u) prize = 0;
    MR32(G_CAREER + C_BANK) = bank0 + prize;
    if (margin < 0.f){
        /* (no margin: the race type's first step, the least points a win gets) */
        uint32_t ty = call(c, FN_RACE_TYPE, 0, 1, &r);
        margin = 0.f;
        for (uint32_t k = 0; k < 7; k++) if (rd(G_POINTS + 0x1cu * k) == ty){ margin = MRF32(G_POINTS + 0x1cu * k + 4u); break; }
    }
    uint32_t a[2] = { fbits(margin), r };
    call(c, FN_MARGIN_REC, sr, 2, a);
    /* 3: RaceWon, its paying part off */
    uint8_t pay = MR8(G_CAREER + C_RESULTS + 2u);
    MR8(G_CAREER + C_RESULTS + 2u) = 0;
    call(c, FN_RACEWON, G_CAREER, 1, &h);
    MR8(G_CAREER + C_RESULTS + 2u) = pay;
    /* 4: the open races again */
    call(c, FN_OPEN_LIST, G_CAREER, 0, 0);
    uint32_t st1 = rd(G_CAREER + C_STAGE);
    memset(&g_last, 0, sizeof g_last);
    g_last.race = r; g_last.prize = (int)prize; g_last.stage_from = (int)st0; g_last.stage_to = (int)st1;
    g_last.points = (int)(MRF32(sr + SR_POINTS) - pts0 + 0.5f);
    /* 5, 6: the sponsors the post-race list would offer now, and the race's movie: in the crib (ui_frame) */
    if (g_ui.st == UI_NONE || g_ui.st == UI_WAIT){
        /* (more wins in one crib: the test aid's; the game plays up to 4 movies in a row) */
        if (g_ui.st == UI_NONE){ g_ui.nscene = 0; g_ui.leaving = 0; }
        g_ui.n = sponsor_offers(c, g_ui.offer); g_ui.i = 0;
        int k = g_ui.nscene;
        if (movies_on()) race_scene(r, rs, st1);
        g_ui.st = g_ui.n ? UI_OFFER : g_ui.nscene ? UI_WAIT : UI_NONE;
        g_last.offers = g_ui.n;
        if (g_ui.nscene > k) snprintf(g_last.movie, sizeof g_last.movie, "%s", g_ui.scene[k]);
        /* the crib's handler ours now: the trip sends its Return to Explore Mode in this same frame, right after
         * the win (a LAN trip's movie never came while it was patched only at the next frame) */
        if (g_ui.st != UI_NONE) ui_ready(c);
    }
    fprintf(stderr, "[mpcredit] f=%ld %s (kind %u, stage %u, %s): won %d, still open %d; bank $%u -> $%u (prize $%u%s); stage %u record: "
            "%.0f points of %u races -> %.0f of %u (margin %.2f); wins of stage %u %d -> %d; stage %u -> %u; %d sponsor offers%s%s\n",
            autotest_frame(), race_name(r), kind, rs, open ? "open" : "not open", event_won(h), race_open(r), bank0, rd(G_CAREER + C_BANK),
            prize, sp ? ", the sponsor's" : "", rs, (double)pts0, runs0, (double)MRF32(sr + SR_POINTS), rd(sr + SR_RUNS), (double)margin,
            st0, wins0, stage_wins(cur), st0, st1, g_last.offers, g_last.movie[0] ? "; the movie " : "", g_last.movie);
    return event_won(h);
}

/* Wins to give at the next crib, in order (free roam, or the test aid), kept beside the profile until a save of the
 * profile has them: "NFS Underground 2/<NAME>/MPWINS.DAT" (a name with a dot is no profile's), as garage.c keeps
 * GARAGE.DAT there (fileio_resolve_save: the same folder on every platform; the page moves it with its profile,
 * backs it up and syncs it with the save folder, and a restore takes a profile folder's other files with it or
 * leaves them out: profilesIn).  A tab closed before the crib keeps the win: the career's next load reads the file
 * (mpcredit_frame, by the profile in play: player 1's, its name in the FEDatabase, a profile made this session too)
 * and the next crib gives it.  A win given (a waiting one, or the trip's in the crib) stays in the file until the
 * game writes the profile with it (garage_profile_written -> mpcredit_profile_written: a whole profile on disk whose
 * career has the race won; one the career no longer has, gone back to a load or to the payload the game kept, waits
 * again): with the game's autosave off it is in no save until the player saves, and a load before that (of this
 * profile again too: each load of player 1's profile reads the file again, garage_profile_loads) gives it again (a
 * race already won is left).  File: PEND_MAGIC, 1, the count, then a race's name hash and its margin (float) each.
 * The profile deleted: the file goes with it (garage_profile_deleted). */
#define PEND_MAX 32
#define PEND_FILE       "MPWINS.DAT"
#define PEND_MAGIC      0x574d324eu   /* "N2MW" */
#define G_P1_NAME       0x0083a9e0u   /* player 1's profile (FEDatabase +0x10): its name, 32 bytes, first */
int fileio_resolve_save(const char *win, char *out, size_t outsz);
int fileio_resolve_save_w(const char *win, char *out, size_t outsz);
unsigned garage_profile_loads(void);
static struct {
    uint32_t race[PEND_MAX]; float margin[PEND_MAX]; int n; long since;      /* waiting */
    uint32_t done[PEND_MAX]; float done_margin[PEND_MAX]; int ndone;         /* given, not saved yet (their hashes) */
    char     profile[33];                                                     /* whose file this is ("" none) */
    unsigned loads;                                                           /* garage_profile_loads when it was read */
} g_pend;
/* the profile in play (player 1's name in the FEDatabase; "" none) */
static void live_profile(char out[33]){
    unsigned i = 0;
    for (; i < 32u; i++){ uint8_t ch = MR8(G_P1_NAME + i); if (ch < 0x20u || ch == 0x7fu) break; out[i] = (char)ch; }
    out[i] = 0;
}
static uint32_t race_by_hash(uint32_t h){
    uint32_t t = rd(G_RACES), n = rd(G_NRACES);
    for (uint32_t i = 0; t && i < n && i < 1024; i++) if (rd(t + i * RACE_SZ + 8u) == h) return t + i * RACE_SZ;
    return 0;
}
/* the file's host path; to write: where it goes (the resolve says 0 for a file not there yet: its folder's path, the
 * folder found case-blind, with the file's name) */
static int pend_path(const char *name, char *out, size_t n, int write){
    char win[128];
    if (!name[0]) return 0;
    snprintf(win, sizeof win, "NFS Underground 2\\%s\\%s", name, PEND_FILE);
    if (!write) return fileio_resolve_save(win, out, n);
    out[0] = 0;
    fileio_resolve_save_w(win, out, n);
    return out[0] != 0;
}
/* the file as the lists are now (none left: no file) */
static void pend_write(void){
    char path[1100];
    if (!pend_path(g_pend.profile, path, sizeof path, 1)){
        fprintf(stderr, "[mpcredit] %s: no place for " PEND_FILE " (no profile loaded?): the waiting wins are in memory only\n", g_pend.profile);
        return;
    }
    int n = g_pend.n + g_pend.ndone;
    if (!n){ if (remove(path) == 0) fprintf(stderr, "[mpcredit] %s: no wins wait, " PEND_FILE " gone\n", g_pend.profile); return; }
    FILE *o = fopen(path, "wb");
    if (!o){ fprintf(stderr, "[mpcredit] %s: cannot write %s\n", g_pend.profile, path); return; }
    uint32_t hd[3] = { PEND_MAGIC, 1u, (uint32_t)n };
    int ok = fwrite(hd, sizeof hd, 1, o) == 1;
    for (int i = 0; i < g_pend.n; i++){ uint32_t e[2] = { MR32(g_pend.race[i] + 8u), fbits(g_pend.margin[i]) }; ok = ok && fwrite(e, sizeof e, 1, o) == 1; }
    for (int i = 0; i < g_pend.ndone; i++){ uint32_t e[2] = { g_pend.done[i], fbits(g_pend.done_margin[i]) }; ok = ok && fwrite(e, sizeof e, 1, o) == 1; }
    ok = fclose(o) == 0 && ok;
    fprintf(stderr, "[mpcredit] %s: " PEND_FILE " %s: %d waiting, %d given but not saved yet\n", g_pend.profile,
            ok ? "written" : "WRITE FAILED", g_pend.n, g_pend.ndone);
}
static int pend_waits(uint32_t r){ for (int k = 0; k < g_pend.n; k++) if (g_pend.race[k] == r) return 1; return 0; }
static void pend_wait(uint32_t r, float m){ if (r && !pend_waits(r) && g_pend.n < PEND_MAX){ g_pend.race[g_pend.n] = r; g_pend.margin[g_pend.n++] = m; } }
/* the profile in play changed, or was loaded again: its file's wins wait now (given ones too: the crib gives a race
 * the career has not won, and leaves one it has).  The same profile again keeps what waited in memory as well */
static void pend_load(const char *name, unsigned loads){
    char path[1100];
    int same = g_pend.profile[0] && !strcasecmp(name, g_pend.profile);
    if (same){
        for (int i = 0; i < g_pend.ndone; i++) pend_wait(race_by_hash(g_pend.done[i]), g_pend.done_margin[i]);
        g_pend.ndone = 0;
    } else memset(&g_pend, 0, sizeof g_pend);
    snprintf(g_pend.profile, sizeof g_pend.profile, "%s", name);
    g_pend.loads = loads; g_pend.since = 0;
    if (!pend_path(name, path, sizeof path, 0)){
        fprintf(stderr, "[mpcredit] %s: the profile in play%s, no " PEND_FILE " (%d wins wait)\n", name, same ? " (loaded again)" : "", g_pend.n);
        if (same && g_pend.n) pend_write();
        return;
    }
    FILE *in = fopen(path, "rb");
    if (!in){ fprintf(stderr, "[mpcredit] %s: the career's profile, %d wins wait\n", name, g_pend.n); if (g_pend.n) pend_write(); return; }
    uint32_t hd[3] = { 0 }, e[2];
    int ok = fread(hd, sizeof hd, 1, in) == 1 && hd[0] == PEND_MAGIC && hd[1] == 1u && hd[2] <= 64u;
    for (uint32_t i = 0; ok && i < hd[2] && fread(e, sizeof e, 1, in) == 1; i++){
        float m; memcpy(&m, &e[1], 4);
        pend_wait(race_by_hash(e[0]), m);
    }
    fclose(in);
    fprintf(stderr, "[mpcredit] %s: " PEND_FILE "%s, %d wins wait for the crib%s\n", name, ok ? "" : " is not one of ours", g_pend.n,
            same ? " (the profile loaded again)" : "");
    if (same) pend_write();
}
/* a race waiting already (won twice before a crib): not again, the second win is the place's prize money (0) */
static int pend_add(uint32_t r, float margin){
    if (g_pend.n >= PEND_MAX || pend_waits(r)) return 0;
    g_pend.race[g_pend.n] = r; g_pend.margin[g_pend.n++] = margin; g_pend.since = 0;
    pend_write();
    return 1;
}
/* the first waiting win given: kept in the file until the profile is written */
static void pend_given(uint32_t r, float margin){
    if (g_pend.ndone < PEND_MAX){ g_pend.done[g_pend.ndone] = MR32(r + 8u); g_pend.done_margin[g_pend.ndone++] = margin; }
    pend_write();
}
/* garage.c: the game wrote a profile (its host path), or deleted one */
static const char *profile_of(const char *path){ const char *sl = strrchr(path, '/'); return sl ? sl + 1 : path; }
void mpcredit_profile_written(const char *path){
    if (!g_pend.profile[0] || strcasecmp(profile_of(path), g_pend.profile) || (!g_pend.ndone && !g_pend.n)) return;
    /* (wins waiting only: written beside it now, a profile made this session had no folder for them before) */
    if (!g_pend.ndone){ pend_write(); return; }
    /* the write has what the career has now: a given win it has won is saved; one it has not (it went back to a
     * load, or to the payload the game kept, without it) waits for the next crib again */
    int saved = 0, again = 0;
    for (int i = 0; i < g_pend.ndone; i++){
        if (event_won(g_pend.done[i])){ saved++; continue; }
        uint32_t r = race_by_hash(g_pend.done[i]);
        if (r){ pend_wait(r, g_pend.done_margin[i]); again++; }
    }
    g_pend.ndone = 0;
    if (again) fprintf(stderr, "[mpcredit] %s written: %d given win%s saved, %d not in the career any more: waiting again\n",
                       g_pend.profile, saved, saved == 1 ? "" : "s", again);
    pend_write();
}
void mpcredit_profile_deleted(const char *path){
    char g[1100]; const char *sl = strrchr(path, '/');
    snprintf(g, sizeof g, "%.*s/%s", sl ? (int)(sl - path) : 1, sl ? path : ".", PEND_FILE);
    if (remove(g) == 0) fprintf(stderr, "[mpcredit] %s deleted with its profile\n", g);
    if (g_pend.profile[0] && !strcasecmp(profile_of(path), g_pend.profile)){ g_pend.n = g_pend.ndone = 0; }
}

int mpcredit_win(CPU *c, uint32_t race, float margin, int in_roam){
    if (!race_in_table(race)){ fprintf(stderr, "[mpcredit] f=%ld race 0x%08x is not in the race table: left\n", autotest_frame(), race); return 0; }
    if (event_won(MR32(race + 8u))){ fprintf(stderr, "[mpcredit] f=%ld %s is won already: left\n", autotest_frame(), race_name(race)); return 0; }
    if (rd(G_GAMEMODE) != 1u){ fprintf(stderr, "[mpcredit] f=%ld %s: not in the career (mode 0x%x): left\n", autotest_frame(), race_name(race), rd(G_GAMEMODE)); return 0; }
    if (in_roam){
        if (!pend_add(race, margin)) return 0;
        fprintf(stderr, "[mpcredit] f=%ld %s: in free roam, the win waits for the next crib\n", autotest_frame(), race_name(race));
        return 1;
    }
    /* given in the crib: kept in the file too until the profile is written (the game's autosave off, the tab closed
     * before the player's own save: the next load gives it again) */
    int ok = win_now(c, race, margin);
    if (ok) pend_given(race, margin);
    return ok;
}
int mpcredit_pending(void){ return g_pend.n; }

/* per frame: the sponsor question and the movie of a win (ui_frame); the waiting wins in the crib, one at a time
 * (each with its own question and movie), once the crib took its snapshot of the profile (180 frames at most: the
 * autosave off takes none).  A question or a movie whose crib went away (the player left it some other way) is
 * dropped */
void mpcredit_frame(CPU *c){
    if (rd(G_GAMEMODE) == 1u){
        char pn[33]; live_profile(pn);
        unsigned loads = garage_profile_loads();
        if (pn[0] && (strcasecmp(pn, g_pend.profile) || loads != g_pend.loads)) pend_load(pn, loads);
    }
    if (g_ui.st != UI_NONE){
        int gone = rd(G_GAMEMODE) != 1u || ((g_ui.st == UI_OFFER || g_ui.st == UI_ASKED || (g_ui.st == UI_WAIT && !g_ui.leaving))
                                            && !career_fe_active("UI_CareerCrib.fng"));
        if (gone){
            fprintf(stderr, "[mpcredit] f=%ld the crib is gone: its %s left\n", autotest_frame(), g_ui.st == UI_WAIT ? "movie" : "sponsor question");
            g_ui.st = UI_NONE; g_ui.leaving = 0;
            return;
        }
        if (g_ui.st != UI_WAIT){ ui_frame(c); return; }
        ui_ready(c);                               /* (the crib's handler patched: its way to the world is ours) */
    }
    if (!g_pend.n) return;
    if (rd(G_GAMEMODE) != 1u || !career_fe_active("UI_CareerCrib.fng") || career_fe_active("MC_Main.fng")){ g_pend.since = 0; return; }
    long f = autotest_frame();
    if (!g_pend.since) g_pend.since = f;
    if (!rd(G_SNAPSHOT) && f - g_pend.since < 180) return;
    uint32_t r = g_pend.race[0]; float m = g_pend.margin[0];
    for (int i = 1; i < g_pend.n; i++){ g_pend.race[i - 1] = g_pend.race[i]; g_pend.margin[i - 1] = g_pend.margin[i]; }
    g_pend.n--;
    if (!race_in_table(r) || event_won(MR32(r + 8u))){ fprintf(stderr, "[mpcredit] f=%ld a waiting win: won already or gone, left\n", f); pend_write(); return; }
    fprintf(stderr, "[mpcredit] f=%ld the waiting win of %s, in the crib%s\n", f, race_name(r), rd(G_SNAPSHOT) ? "" : " (no snapshot: the autosave is off)");
    if (win_now(c, r, m)) pend_given(r, m); else pend_write();
}

/* test aid (career.c "mpcredit NAME[,NAME...] [MARGIN]"): the wins of those races at the next crib, in order;
 * "mpcredit unwin:NAME": the race not won in the career any more (its event entry's won flag, as a load of an older
 * save of the profile has it), for what a profile written then does with a win given but not saved */
int mpcredit_test(const char *names, float margin){
    uint32_t t = rd(G_RACES), n = rd(G_NRACES); int got = 0;
    if (!strncasecmp(names, "unwin:", 6)){
        uint32_t r = 0, ne = rd(G_CAREER + C_NEVENTS);
        for (uint32_t i = 0; t && i < n && i < 1024 && !r; i++) if (!strcasecmp(race_name(t + i * RACE_SZ), names + 6)) r = t + i * RACE_SZ;
        for (uint32_t i = 0; r && i < ne && i < 384; i++)
            if (MR32(G_CAREER + C_EVENTS + 8u * i) == MR32(r + 8u)){ MR8(G_CAREER + C_EVENTS + 8u * i + 6u) = 0; got = 1; }
        fprintf(stderr, "[mpcredit] f=%ld test: %s %s\n", autotest_frame(), names + 6, got ? "not won in the career any more" : "has no event entry");
        return got;
    }
    for (const char *s = names; *s; ){
        const char *e = strchr(s, ','); size_t k = e ? (size_t)(e - s) : strlen(s);
        char nm[48]; snprintf(nm, sizeof nm, "%.*s", (int)(k < 47 ? k : 47), s);
        s += k + (e ? 1 : 0);
        uint32_t r = 0;
        for (uint32_t i = 0; t && i < n && i < 1024 && !r; i++) if (!strcasecmp(race_name(t + i * RACE_SZ), nm)) r = t + i * RACE_SZ;
        if (!r){ fprintf(stderr, "[mpcredit] f=%ld test: no race %s\n", autotest_frame(), nm); continue; }
        if (!pend_add(r, margin)){ fprintf(stderr, "[mpcredit] f=%ld test: %s not queued (waiting already, or %d wait)\n", autotest_frame(), nm, g_pend.n); continue; }
        got++;
        fprintf(stderr, "[mpcredit] f=%ld test: the win of %s (margin %.2f) at the next crib; open %d, won %d\n", autotest_frame(), nm,
                (double)margin, race_open(r), event_won(MR32(r + 8u)));
    }
    return got;
}
