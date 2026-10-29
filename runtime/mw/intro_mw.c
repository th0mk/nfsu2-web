/* intro_mw.c -- Most Wanted's boot movies: skippable (default), original, or
 * off, as runtime/intro.c does them for Underground 2.
 *
 *   NFSU2_INTRO=skippable | original | off      (settings.ini: intro = ...)
 *   NFSU2_INTRO_LOG=1   also log every message the boot-movie screens get
 *
 * speed.exe v1.3 boots through LS_EALogo (ealogo), LS_PSA (psa) and
 * MW_LS_AttractFMV (attract_movie) to the title MW_LS_Splash; LS_EA_hidef
 * (eahd_bumper), MW_LS_IntroFMV (intro_movie) and the WS_ twins of each are
 * in its tables too.  All of them are one screen class: the factory 0x591bb0
 * (the package table 0x8f4640), the constructor 0x58eca0, the vtable
 * 0x8a17d4, whose NotificationMessage is 0x57dda0 (thiscall: msg, obj, p1,
 * p2; ret 0x10).  The constructor finds its package in the table 0x8f3cd8
 * (0x10 bytes an entry: package name, movie name, a hash, a byte "skippable",
 * a byte "start at once") and keeps the skippable byte at +0x2c.  The handler
 * ends the screen on the movie player's 0xc3960eb9 (the movie ended, or it
 * could not be opened), and on PAD_ACCEPT or PAD_START only when +0x2c is set:
 * the table has 0 for the EA logo and the EA bumper, 1 for the PSA, the intro
 * and the attract movie.  So the EA logo cannot be skipped in the original.
 * Enter, pad A and a left click arrive as PAD_ACCEPT, pad Start as PAD_START
 * (the input page's runs: Enter and click end the PSA and the attract movie,
 * not the EA logo; Escape ends none).
 *
 * skippable: PAD_ACCEPT and PAD_START end every boot movie (+0x2c is set
 * before the handler looks at it, so the game's own skip runs), and so does a
 * new press of Escape, Space or pad B, seen on the screen's per-update tick
 * 0xc98356ba (a screen gets one every frame; nothing else of those presses
 * reaches it): the handler gets PAD_ACCEPT in its place.
 *
 * off: the front end's movie object starts a movie in 0x559680 (thiscall: the
 * event; ret 4; the event's +0x18 is 7 and +0x24 the movie's name).  For a
 * boot movie it posts 0xc3960eb9 at once and plays nothing, as 0x5597e1 does
 * for a file that is not there ([[0x91cadc]]->0x5b7780(0xc3960eb9, 0, this+4,
 * -1, 0)); each screen then ends as after a full play.  The attract movie the
 * title brings back when left alone is a boot movie too.
 *
 * original: none of this.  Story, tutorial and Blacklist movies are never
 * touched. */
#include "cpu.h"
#include "kbd.h"
#include "gamepad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
const char *autotest_screen(void);
void sub_005b7780(CPU *c);

#define MSG_PAD_ACCEPT 0x406415e3u   /* bStringHash("PAD_ACCEPT") */
#define MSG_PAD_START  0xb5af2461u   /* bStringHash("PAD_START") */
#define MSG_PAD_BACK   0x911ab364u   /* bStringHash("PAD_BACK") */
#define MSG_MOVIE_END  0xc3960eb9u   /* posted by the movie player */
#define MSG_TICK       0xc98356bau   /* every front-end update (as UG2's) */
#define G_FENG         0x0091cadcu   /* cFEng; its FEngine at +0 */

enum { INTRO_SKIPPABLE, INTRO_ORIGINAL, INTRO_OFF };
static int g_mode = -1, g_log, g_pad = 1;

static int intro_mode(void){
    if (g_mode < 0){
        const char *e = getenv("NFSU2_INTRO");
        g_mode = INTRO_SKIPPABLE;
        if (e && *e){
            if (!strcasecmp(e, "original")) g_mode = INTRO_ORIGINAL;
            else if (!strcasecmp(e, "off") || !strcasecmp(e, "no") || !strcasecmp(e, "false") || !strcmp(e, "0"))
                g_mode = INTRO_OFF;          /* settings.ini's false words mean off there too */
            else if (strcasecmp(e, "skippable"))
                fprintf(stderr, "[intro] NFSU2_INTRO=%s is not skippable, original or off; using skippable\n", e);
        }
        const char *l = getenv("NFSU2_INTRO_LOG"); g_log = l && *l && *l != '0';
        /* no controller for the game (NFSU2_PAD=off, dinput_pad.c): none here either */
        const char *p = getenv("NFSU2_PAD");
        g_pad = (p && !strcasecmp(p, "hotplug")) || gamepad_env_flag("NFSU2_PAD", 1);
        fprintf(stderr, "[intro] boot movies: %s\n",
                g_mode == INTRO_OFF ? "off" : g_mode == INTRO_ORIGINAL ? "original" : "skippable");
    }
    return g_mode;
}

static const char *msg_name(uint32_t m){
    switch (m){
    case MSG_PAD_ACCEPT: return "PAD_ACCEPT";
    case MSG_PAD_START:  return "PAD_START";
    case MSG_PAD_BACK:   return "PAD_BACK";
    case MSG_MOVIE_END:  return "movie end";
    default:             return 0;
    }
}
static const char *screen(void){ const char *s = autotest_screen(); return s ? s : "?"; }

/* NFSU2_INTRO_LOG=1: every message a boot movie screen gets */
static void trace(uint32_t m){
    if (!g_log || m == MSG_TICK) return;
    const char *n = msg_name(m);
    if (n) fprintf(stderr, "[intro] f=%ld movie screen (%s): %s\n", autotest_frame(), screen(), n);
    else   fprintf(stderr, "[intro] f=%ld movie screen (%s): 0x%08x\n", autotest_frame(), screen(), m);
}

/* ---- presses the front end does not pass on: Escape, Space, pad B ---------
 * Counted from the first message a screen object gets (a new object, or one
 * that got nothing for half a second), so a press made before it (on the
 * previous movie, or held over) never ends it. */
static struct { uint32_t self, kseq, pad; long frame; } g_base;
static uint32_t pad_buttons(void){
    if (!g_pad) return 0;
    PadState s;
    return gamepad_read(&s) ? s.buttons : 0;
}
static void rebase(uint32_t self){ g_base.self = self; g_base.kseq = kbd_ev_head(); g_base.pad = pad_buttons(); }
static const char *new_press(void){
    const char *what = 0;
    uint32_t head = kbd_ev_head();
    if (head - g_base.kseq > 256u) g_base.kseq = head - 256u;
    for (uint32_t s = g_base.kseq; s != head; s++){
        KbdEv e;
        if (kbd_ev_get(s, &e) == 1 && e.down && !what)
            what = e.dik == 0x01 ? "Escape" : e.dik == 0x39 ? "Space" : 0;
    }
    g_base.kseq = head;
    uint32_t b = pad_buttons();
    if (!what && (b & ~g_base.pad & (1u << PAD_B))) what = "pad B";
    g_base.pad = b;
    return what;
}

/* the boot movie screens' NotificationMessage */
void sub_0057dda0_orig(CPU *c);
void sub_0057dda0(CPU *c){
    uint32_t m = MR32(c->esp + 4u), self = c->ecx;
    intro_mode();
    trace(m);                            /* in every mode, so the unchanged game can be watched too */
    if (g_mode == INTRO_SKIPPABLE){
        long f = autotest_frame();
        if (self != g_base.self || f - g_base.frame > 30) rebase(self);
        g_base.frame = f;
        const char *by = 0;
        if (m == MSG_PAD_ACCEPT || m == MSG_PAD_START || m == MSG_PAD_BACK) by = msg_name(m);
        else if (m == MSG_TICK) by = new_press();
        if (by){
            if (m != MSG_PAD_START) MR32(c->esp + 4u) = MSG_PAD_ACCEPT;
            MR8(self + 0x2cu) = 1;       /* skippable: the game's own skip */
            fprintf(stderr, "[intro] f=%ld %s skipped by %s\n", f, screen(), by);
            rebase(self);
        }
    }
    sub_0057dda0_orig(c);
}

/* ---- off ------------------------------------------------------------------ */
static int boot_movie(uint32_t name){
    if (!name || !mem_is_committed(name)) return 0;
    char b[32]; int n = 0;
    while (n < (int)sizeof b - 1 && mem_is_committed(name + (uint32_t)n) && MR8(name + (uint32_t)n)){
        b[n] = (char)MR8(name + (uint32_t)n); n++; }
    b[n] = 0;
    static const char *const names[] = { "ealogo", "eahd_bumper", "psa", "intro_movie", "attract_movie" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(b, names[i])) return 1;
    return 0;
}

/* the front end's movie object: an event (+0x18 == 7: start the movie named at +0x24) */
void sub_00559680_orig(CPU *c);
void sub_00559680(CPU *c){
    uint32_t ev = MR32(c->esp + 4u), self = c->ecx;
    if (intro_mode() == INTRO_OFF && mem_is_committed(ev) && MR32(ev + 0x18u) == 7u && boot_movie(MR32(ev + 0x24u))){
        fprintf(stderr, "[intro] f=%ld %s: movie not played (off)\n", autotest_frame(), screen());
        /* as 0x5597e1 for a movie that is not there */
        PUSH32(0u); PUSH32(0xffffffffu); PUSH32(MR32(self + 4u)); PUSH32(0u); PUSH32(MSG_MOVIE_END);
        c->ecx = MR32(MR32(G_FENG));
        PUSH32(0x005597fdu); sub_005b7780(c);
        c->ecx = self;
        c->eax &= ~0xffu;                /* al = 0, as there */
        POP32(c->eip); c->esp += 4u;     /* ret 4 */
        return;
    }
    sub_00559680_orig(c);
}
