/* intro.c -- the boot movies: skippable (default), original, or off.
 *
 *   NFSU2_INTRO=skippable | original | off      (settings.ini: intro = ...)
 *   NFSU2_INTRO_LOG=1   also log every message the boot-movie screens get
 *
 * The game boots through a list of front-end screens (tables 0x7f65e8 /
 * 0x7f6620, in the boot-flow object [0x836488] made by 0x4d9580): MC_Bootup,
 * LS_BlankMovie (blank.vp6), LS_EAlogo (ealogo.vp6), LS_THXMovie
 * (THX_logo.vp6), LS_PSAMovie (PSA.vp6), UG_LS_IntroFMV (FMVOpening.vp6),
 * UG_LS_Splash (the title).  Each movie
 * screen ends in its NotificationMessage handler (thiscall: msg, obj, p1, p2;
 * ret 0x10), which goes on to the next screen with 0x4c4f50:
 *
 *   0x4c53a0  LS_BlankMovie, LS_EAlogo, LS_THXMovie: only on 0xc3960eb9, the
 *             message the movie player posts when a movie ends (0x5660f0) or
 *             cannot be opened (0x566240).  Input is ignored: unskippable.
 *   0x4c53d0  LS_PSAMovie: on 0xc3960eb9 (sets [0x8383a8], saves the video
 *             options with 0x5beea0, sets "boot done" [0x836494]); on
 *             PAD_ACCEPT/PAD_START only when the memory-card object
 *             [0x836498] has byte +5 set, i.e. a saved profile exists
 *             (0x4c618c, from 0x4a9c40).  First startup: unskippable.
 *   0x4c5330  UG_LS_IntroFMV: on 0xc3960eb9, PAD_ACCEPT or PAD_START.
 *
 * What reaches these handlers (cFEng 0x54d4b0 only sends a pad message that
 * the package has a response for): Enter, pad A and a left click arrive as
 * PAD_ACCEPT, pad Start as PAD_START.  Escape and pad B raise the joy event
 * FEPad_Back, but no boot package responds to PAD_BACK, so nothing arrives;
 * Space is no front-end key at all.
 *
 * skippable: PAD_ACCEPT/PAD_START (and PAD_BACK, should one come) end every
 * boot movie, and so does a new press of Escape, Space (DirectInput key
 * events, keyboard.c) or pad B (gamepad_read), seen on the screen's per-frame
 * tick 0xc98356ba.  Those three never produce PAD_ACCEPT/PAD_START here, so
 * one press ends one movie.  Where the game has its own skip (the intro; the
 * PSA with a profile) the screen gets PAD_ACCEPT; elsewhere it gets
 * 0xc3960eb9 and takes the same path as when its movie plays to the end (the
 * player is stopped by the package teardown, 0x511bc0, which posts nothing).
 * off: the movie player's start (0x566020, called by the FE movie object
 * 0x566240 with "<dir>MOVIES\<name>.vp6") posts 0xc3960eb9 at once for the
 * boot movies instead of opening them, as 0x566020 itself does when
 * [0x8650a8] is set (and 0x566240 for a missing file, which WidescreenFix's
 * SkipIntro relies on).  Every screen then ends as after a full play.
 * original: none of this.  Story, tutorial and region movies (IG_PlayMovie)
 * are never touched. */
#include "cpu.h"
#include "kbd.h"
#include "gamepad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
const char *autotest_screen(void);
void sub_0055dc50(CPU *c);

#define MSG_PAD_ACCEPT 0x406415e3u   /* bStringHash("PAD_ACCEPT") */
#define MSG_PAD_START  0xb5af2461u   /* bStringHash("PAD_START") */
#define MSG_PAD_BACK   0x911ab364u   /* bStringHash("PAD_BACK") */
#define MSG_MOVIE_END  0xc3960eb9u   /* posted by the movie player */
#define MSG_TICK       0xc98356bau   /* every front-end update */

enum { INTRO_SKIPPABLE, INTRO_ORIGINAL, INTRO_OFF };
static int g_mode = -1, g_log, g_pad = 1;

static int intro_mode(void){
    if (g_mode < 0){
        const char *e = getenv("NFSU2_INTRO");
        g_mode = INTRO_SKIPPABLE;
        if (e && *e){
            if (!strcasecmp(e, "original")) g_mode = INTRO_ORIGINAL;
            else if (!strcasecmp(e, "off") || !strcasecmp(e, "no") || !strcasecmp(e, "false") || !strcmp(e, "0"))
                g_mode = INTRO_OFF;          /* settings.ini's false words mean off there too (bundle.m enum_value) */
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
static int is_pad(uint32_t m){ return m == MSG_PAD_ACCEPT || m == MSG_PAD_START || m == MSG_PAD_BACK; }
static const char *screen(void){ const char *s = autotest_screen(); return s ? s : "?"; }

/* NFSU2_INTRO_LOG=1: every message but the per-update tick */
static void trace(const char *who, uint32_t m){
    if (!g_log || m == MSG_TICK) return;
    const char *n = msg_name(m);
    if (n) fprintf(stderr, "[intro] f=%ld %s (%s): %s\n", autotest_frame(), who, screen(), n);
    else   fprintf(stderr, "[intro] f=%ld %s (%s): 0x%08x\n", autotest_frame(), who, screen(), m);
}

/* ---- presses the front end does not pass on: Escape, Space, pad B ---------
 * Counted from the first message a screen object gets (a new object, or one
 * that got nothing for half a second: a live screen gets a tick every frame),
 * so a press made before it (on the previous movie, or held over) never
 * ends it. */
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

/* The skip for one message: 0 = none, else the message the screen gets
 * instead.  own = the screen's own skip message (0: it has none). */
static uint32_t skip(CPU *c, const char *who, uint32_t own){
    uint32_t m = MR32(c->esp + 4u);
    intro_mode();
    trace(who, m);                       /* in every mode, so the unchanged game can be watched too */
    if (g_mode == INTRO_ORIGINAL) return 0;
    if (g_mode != INTRO_SKIPPABLE) return 0;
    long f = autotest_frame();
    if (c->ecx != g_base.self || f - g_base.frame > 30) rebase(c->ecx);
    g_base.frame = f;
    const char *by = 0;
    if (is_pad(m)) by = msg_name(m);
    else if (m == MSG_TICK) by = new_press();
    if (!by) return 0;
    uint32_t to = own ? own : MSG_MOVIE_END;
    if (is_pad(m) && own && m != MSG_PAD_BACK) to = m;        /* the game's own skip, as is */
    fprintf(stderr, "[intro] f=%ld %s skipped by %s (%s)\n", autotest_frame(), screen(), by, msg_name(to));
    rebase(c->ecx);
    return to;
}

/* LS_BlankMovie, LS_EAlogo, LS_THXMovie */
void sub_004c53a0_orig(CPU *c);
void sub_004c53a0(CPU *c){
    uint32_t to = skip(c, "movie screen", 0);
    if (to) MR32(c->esp + 4u) = to;
    sub_004c53a0_orig(c);
}

/* LS_PSAMovie: its own skip needs a saved profile (byte +5 of the
 * memory-card object [0x836498], as 0x4c53ee tests) */
void sub_004c53d0_orig(CPU *c);
void sub_004c53d0(CPU *c){
    uint32_t mc = MR32(0x836498u);
    uint32_t to = skip(c, "PSA screen", mc && MR8(mc + 5u) ? MSG_PAD_ACCEPT : 0);
    if (to) MR32(c->esp + 4u) = to;
    sub_004c53d0_orig(c);
}

/* UG_LS_IntroFMV (also the title screen's attract replay after 30 s) */
void sub_004c5330_orig(CPU *c);
void sub_004c5330(CPU *c){
    uint32_t to = skip(c, "intro screen", MSG_PAD_ACCEPT);
    if (to) MR32(c->esp + 4u) = to;
    sub_004c5330_orig(c);
}

/* ---- off ------------------------------------------------------------------ */
/* the boot movies by file name; blank.vp6 only on its own boot screen */
static int boot_movie(uint32_t path){
    if (!path || !mem_is_committed(path)) return 0;
    char p[128]; int n = 0;
    while (n < (int)sizeof p - 1 && mem_is_committed(path + (uint32_t)n) && MR8(path + (uint32_t)n)){
        p[n] = (char)MR8(path + (uint32_t)n); n++; }
    p[n] = 0;
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    size_t l = strlen(b);
    if (l > 4 && !strcasecmp(b + l - 4, ".vp6")) l -= 4;
    static const char *const names[] = { "ealogo", "THX_logo", "THX_logo_fr", "PSA", "FMVOpening" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strlen(names[i]) == l && !strncasecmp(b, names[i], l)) return 1;
    return l == 5 && !strncasecmp(b, "blank", 5) && !strcasecmp(screen(), "LS_BlankMovie.fng");
}

/* 0x566020(path, 1, flags), cdecl: start the movie player */
void sub_00566020_orig(CPU *c);
void sub_00566020(CPU *c){
    if (intro_mode() == INTRO_OFF && boot_movie(MR32(c->esp + 4u))){
        fprintf(stderr, "[intro] f=%ld %s: movie not played (off)\n", autotest_frame(), screen());
        /* as 0x56602a: FEManager (0x8637b0) ->QueueMessage(0xc3960eb9, 0)
         * (thiscall, ret 8); ecx comes back as it was (push/pop ecx) */
        uint32_t ecx = c->ecx;
        PUSH32(0u); PUSH32(MSG_MOVIE_END); c->ecx = 0x8637b0u;
        PUSH32(0x56603bu); sub_0055dc50(c);
        c->ecx = ecx;
        POP32(c->eip);
        return;
    }
    sub_00566020_orig(c);
}

/* ---- the title's second line ----------------------------------------------
 * The title screen (UG_LS_Splash.fng) has EA's copyright in a band at the
 * bottom: string 0xC70CCFC7, a top-level object centred at (0, 194) in HUD
 * units (640 x 480 about the centre), size 0.9, in the band (group 0x356DE316
 * at y 195.5: two images 40 high).  Under it goes the site's address, in a
 * copy of that string (optslider.c's fe_copy) 12 units lower at 3/4 of its
 * size, inside the band, made each time the package is loaded (the title
 * comes back after its attract movie: a new package, a new copy).  The copy's
 * animation (the copyright's, cloned) puts it back where the copyright is, so
 * every frame it gets its place and size again, and the colour of "Press
 * Enter Key" (string 0xC4DF3FF2, the game's green) with the copyright's
 * alpha: it fades in and out with the copyright.  An object's drawing data (+0x2C):
 * colour (b, g, r, a ints) +0, position +0x1C, size +0x38; a change sets its
 * rebuild flag (+0x1C 0x400000), as FEng's setters do (menusize.c). */
const char *autotest_top_fng(void);
void dispatch_indirect(CPU *c, uint32_t addr);
uint32_t fe_copy(CPU *c, uint32_t pk, uint32_t o, uint32_t hash);

#define TITLE_FNG     "UG_LS_Splash.fng"
#define TITLE_LINE    "nfsu2.xyz"
#define LINE_DY       12.f          /* HUD units under the copyright's centre */
#define LINE_SIZE     0.75f         /* of the copyright's size */
#define H_COPYRIGHT   0xc70ccfc7u
#define H_PRESS       0xc4df3ff2u   /* "Press Enter Key" (its group slides in) */
#define FN_NEW        0x00575620u   /* cdecl (size): operator new */
#define FN_FE_HASH    0x00505450u   /* cdecl (name) -> an FE object name's hash */
#define FN_FE_PACKAGE 0x0052cef0u   /* cdecl (package name) -> FEPackage*, 0: not loaded */
#define FN_PKG_FIND   0x00517f80u   /* thiscall package (hash) -> its object, 0: none */
#define FN_FE_SETSTR  0x00535fe0u   /* thiscall string object (char*): its text */

static struct { uint32_t scr, hash; int off; } g_tl;

/* a guest function with ecx and stack arguments, the CPU as it was after */
static uint32_t gcall(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(fn);                                  /* the callee's ret pops it */
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n); }
static uint32_t gstr(uint32_t a, const char *s){
    uint32_t i = 0;
    for (; s[i] && i < 63u; i++) MR8(a + i) = (uint8_t)s[i];
    MR8(a + i) = 0;
    return a;
}

void intro_frame(CPU *c){
    if (g_tl.off) return;
    const char *top = autotest_top_fng();
    if (!top || strcasecmp(top, TITLE_FNG)) return;
    if (!g_tl.scr){
        uint32_t n = 64u;
        if (!(g_tl.scr = gcall(c, FN_NEW, 0, 1, &n))){ g_tl.off = 1; return; }
        uint32_t a = gstr(g_tl.scr, "SITE_ADDRESS_LINE");
        g_tl.hash = gcall(c, FN_FE_HASH, 0, 1, &a);
    }
    uint32_t a = gstr(g_tl.scr, TITLE_FNG), pk = gcall(c, FN_FE_PACKAGE, 0, 1, &a), h = H_COPYRIGHT;
    uint32_t o = ok(pk, 0x54) ? gcall(c, FN_PKG_FIND, pk, 1, &h) : 0;
    if (!ok(o, 0x70) || MR32(o + 0x18u) != 2u || !ok(MR32(o + 0x2cu), 0x40)) return;   /* 2: a string */
    uint32_t line = gcall(c, FN_PKG_FIND, pk, 1, &g_tl.hash);
    if (!line){
        if (!(line = fe_copy(c, pk, o, g_tl.hash)) || !ok(MR32(line + 0x2cu), 0x40)){
            fprintf(stderr, "[intro] the title's copyright could not be copied: no line under it\n");
            g_tl.off = 1;
            return;
        }
        a = gstr(g_tl.scr, TITLE_LINE);
        gcall(c, FN_FE_SETSTR, line, 1, &a);
        MR32(line + 0x1cu) |= 2u;                /* a text of its own, as FEPrintf leaves it (0x537B30) */
        uint32_t d0 = MR32(o + 0x2cu);
        char tx[64]; uint32_t w = MR32(o + 0x64u), k = 0;
        for (; ok(w, 2u * k + 1u) && k + 1 < sizeof tx && MR16(w + 2u * k); k++) tx[k] = (char)MR16(w + 2u * k);
        tx[k] = 0;
        fprintf(stderr, "[intro] title: \"%s\" under \"%s\" (y %.1f, size %.2f)\n", TITLE_LINE, tx,
                (double)(MRF32(d0 + 0x20u) + LINE_DY), (double)(MRF32(d0 + 0x38u) * LINE_SIZE));
    }
    /* every frame, as the copy's animation (the copyright's, cloned) puts it back where the copyright is */
    uint32_t d0 = MR32(o + 0x2cu), d1 = MR32(line + 0x2cu), hp = H_PRESS;
    float want[4] = { MRF32(d0 + 0x1cu), MRF32(d0 + 0x20u) + LINE_DY, MRF32(d0 + 0x38u) * LINE_SIZE, MRF32(d0 + 0x3cu) * LINE_SIZE };
    static const uint32_t AT[4] = { 0x1cu, 0x20u, 0x38u, 0x3cu };
    for (int i = 0; i < 4; i++)
        if (MRF32(d1 + AT[i]) != want[i]){ MRF32(d1 + AT[i]) = want[i]; MR32(line + 0x1cu) |= 0x400000u; }
    uint32_t p = gcall(c, FN_PKG_FIND, pk, 1, &hp), dp = ok(p, 0x70) ? MR32(p + 0x2cu) : 0;
    int32_t col[4];
    for (uint32_t i = 0; i < 4; i++) col[i] = (int32_t)MR32((i < 3 && ok(dp, 16) ? dp : d0) + 4u * i);
    for (uint32_t i = 0; i < 4; i++)
        if ((int32_t)MR32(d1 + 4u * i) != col[i]){ MR32(d1 + 4u * i) = (uint32_t)col[i]; MR32(line + 0x1cu) |= 0x400000u; }
}
