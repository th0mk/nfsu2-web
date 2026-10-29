/* autotest.c -- automated-test infrastructure (v1.2 NA port).
 *
 * Everything here is driven from the GUEST thread at IDirect3DDevice9::
 * Present (autotest_on_present), i.e. in lock-step with the game's own
 * frames, so a script replays identically in windowed and headless runs.
 *
 *   NFSU2_INPUT_SCRIPT=<file>   timed input script (format below)
 *   NFSU2_QUIT_FRAMES=<n>       clean exit (reports) after n guest Present()s;
 *                               works headless too (QUITFRAMES counts host
 *                               frames and needs the window)
 *   NFSU2_TIMEOUT=<sec>         alias of ALARM (boot.c): exit after N s
 *   NFSU2_STATUS_EVERY=<n>      every n frames log: frame, wall time, guest
 *                               fps, the game's flow state (game.h), CPU%
 *   NFSU2_LOG=<file>            stdout+stderr -> file (see autotest_early)
 *   NFSU2_FNG_LOG=1             log every change of the top front-end package
 *                               (3: also every press of tapuntil/tappaduntil)
 *   NFSU2_CAR_TRACE=<file>      CSV of every race car per frame (UG2: debug_ug2.c)
 *   NFSU2_DUMP_FRAMES / NFSU2_DUMP_DIR / NFSU2_DUMP_SCALE: see metal.m
 *
 * Script format: one event per line, '#' comments; ${NAME} is replaced by
 * the environment variable NAME (empty when unset).
 *     <when> <command> [args]
 *   <when>:  N | fN   absolute guest frame (Present count, 1-based)
 *            Nms      absolute ms since the first Present
 *            Nwms     game-world ms (sum of the world dt the game simulated,
 *                     framerate.c) since the last "wzero" (or the start): the
 *                     same driving input at any frame rate
 *            +N | +Nms | +Nwms  relative to the moment the previous line fired
 *   Lines fire strictly in order; a line never fires before the previous
 *   one (so "waitmem" blocks everything after it).
 *   commands:
 *     down KEY | up KEY        key state (DirectInput + WM_KEYDOWN/UP)
 *     tap KEY [frames]         down, release after frames (default 6)
 *     type TEXT                WM_CHAR + DI taps for each character
 *     dump [label]             write the next rendered frame to PNG
 *     waitmem ADDR VAL [tmo]   wait until dword [ADDR]==VAL (tmo frames,
 *                              default unlimited); logs the result
 *     log TEXT                 marker line in the log
 *     waitfng NAME [tmo]       wait until the top front-end package, or the
 *                              current screen (last "Init Package"/"Switch-
 *                              FEngPackages ... to" seen by the game's debug
 *                              printf 0x50D510), is NAME (e.g. UI_QRTrackSelect.fng).
 *                              A qualifier after the package's name waits for
 *                              a state within it, where the game has them
 *                              (game.h fe_match; MW: fe_mw.c mw_fe_match):
 *                              PACKAGE:LABEL a language label shown, @HASH the
 *                              highlighted object, 'TEXT a text shown, ^TEXT
 *                              that text highlighted; NAME|NAME any of them,
 *                              bare packages too (the game's to judge as well);
 *                              -PACKAGE that package gone (MW)
 *     tapuntil KEY NAME [every] [tmo]  tap KEY every `every` frames (default
 *                              90) until the top package is NAME (or NAME's
 *                              state, as waitfng)
 *     peek ADDR [count]        log count dwords at ADDR (hex + float); ADDR
 *                              "@in+OFF" is relative to the player's input
 *                              controller (runtime/dinput_pad.c)
 *   synthetic game controller (runtime/gamepad.m; it connects on first use
 *   and goes through the same GameController -> DirectInput path as a real
 *   one; while connected it replaces the real controller):
 *     pad BUTTON down|up       button state; BUTTON = A B X Y LB RB BACK START
 *                              LS RS HOME DUP DDOWN DLEFT DRIGHT (or CROSS,
 *                              CIRCLE, SQUARE, TRIANGLE, L1, R1, VIEW, MENU...)
 *     pad connect|disconnect   plug / unplug the synthetic controller
 *     pad connect wheel        plug it in as a steering wheel (LX the wheel,
 *                              LT/RT the pedals; dinput_pad.c's driving device)
 *     tappad BUTTON [frames]   press, release after frames (default 6)
 *     axis LX|LY|RX|RY V       stick position, -1..1 (+X right, +Y down)
 *     trigger LT|RT V          trigger position, 0..1
 *     tappaduntil BUTTON NAME [every] [tmo]  like tapuntil, with a pad button
 *     poke ADDR VAL            write dword VAL to ADDR (debug only)
 *     wzero [now]              world-time origin for absolute Nwms lines: the
 *                              start of the current race (GameFlow 6) when
 *                              racing, else (or with "now") the current time
 *     photo WHAT [ARGS]        photo mode as the page drives it (photo.c
 *                              photo_script: on, off, orbit, free, turn X Y ...)
 *     headlights MODE          the other cars' headlights (headlights.c): its
 *                              NFSU2_HEADLIGHTS modes, for an A/B in one run
 *     displayhz HZ             act as if the window moved to a screen with
 *                              this refresh rate (framerate.c, NFSU2_FPS=auto
 *                              tests); 0 = the detected rate again
 *     autopilot on|off [tmo]   hand the player car to the game's AI driver;
 *                              off gives it back to the player's input; "on
 *                              tmo" waits up to tmo frames for player 1 to
 *                              drive as a human first (a LAN race makes the
 *                              local player one at its start)  (test-only, for
 *                              soaks, career and LAN race runs; career.c,
 *                              which takes every career command)
 *     status [label]           print a [status] line now (memory trend points)
 *     stall SECONDS            test-only: block the guest thread (GIL held) to
 *                              exercise the stall watchdog (watchdog.c)
 *     loop N                   repeat the lines up to the matching "endloop"
 *     endloop                  N times (loops may nest; N=0 means forever)
 *     breakfng NAME            in a loop: when NAME (as waitfng) holds now,
 *                              leave the innermost loop (the next line is
 *                              the one after its "endloop"); else nothing
 *     trace EVERY NAME=EXPR... log the expressions every EVERY frames (one
 *                              "[trace]" line; up to 24; replaces the last set).
 *                              EXPR = [TYPE:]ADDR, ADDR = number | [ADDR] |
 *                              ADDR+number | ADDR-number, where [X] reads the
 *                              dword at X.  TYPE f (float, default), i (int32),
 *                              x (hex dword), b (byte), w (word), d (double).
 *                              $P is the local human racer of the current race
 *                              (the game's, game.h racer; UG2: GRaceStatus
 *                              [0x890118], game_ug2.c), $R0..$R9 racer slot
 *                              i, 0 if none.
 *                              $O(PACKAGE:HASH) is the object of that name
 *                              hash in the active front-end package PACKAGE
 *                              (game.h fe_object; MW: fe_mw.c), 0 if none.
 *                              Example: i:[$P+0x1d4]+0x24
 *     untrace                  stop the periodic trace
 *     expect EXPR OP VALUE     check once: logs "[expect] PASS|FAIL ..."
 *                              (tests/run_suite.sh fails a run on FAIL).
 *                              EXPR as in trace (TYPE:ADDR), OP one of
 *                              > >= < <= == !=, VALUE a number
 *     mark NAME EXPR           remember the value of EXPR as @NAME; VALUE in
 *                              expect/waitexpr may be @NAME or @NAME+number,
 *                              and EXPR may be @NAME
 *     edge NAME EXPR OP VALUE  from now on, every frame: count the frames where
 *                              the condition becomes true into @NAME (up to 8)
 *     waitexpr EXPR OP VALUE [tmo]  wait until the condition holds (tmo
 *                              frames, default unlimited; logs the result)
 *     covreset                 zero the per-function call counters (COVERAGE=1
 *                              build with COVREPORT set)
 *     covdump FILE             write the counters to FILE (relative: run dir)
 *     click X Y                a left click at X, Y (fractions of the window
 *                              content from the top left; metal.m), released
 *                              8 frames later (the Host LAN Server button)
 *     srvcycle N [MS]          LAN hosting test: start and stop the lifted
 *                              server.dll N times, MS ms up (module.c)
 *     quit                     clean exit
 *     close                    what Cmd-Q / the close button do: WM_CLOSE to
 *                              the game, which then shuts itself down
 *     blur MS                  app deactivation for MS ms, then reactivation
 *                              (the game freezes meanwhile, see lifecycle.c)
 *     audioreset               handle a default output device change now
 *     audiostall MS            test-only, browser: the audio pump mixes nothing
 *                              for MS ms, so the worklet underruns
 *   real-keyboard path (runtime/keyboard_mac.m): NSEvents are made and posted
 *   into AppKit's event queue on the main thread, so they go through the
 *   same handler as keys typed on a real keyboard (Win32 messages, DIK
 *   state, Cmd shortcuts for the app), not through the injection above:
 *     nsdown KEY | nsup KEY    key down / up; KEY = position name as above
 *                              (A = the key in the US A position), LCMD,
 *                              RCMD, LOPTION, ROPTION, CAPSLOCK, FN, or
 *                              kc:0xNN (a mac key code); "cmd+", "shift+",
 *                              "ctrl+", "opt+" prefixes hold modifiers
 *     nstap KEY [frames]       down, up after frames (default 6)
 *     nstapuntil KEY NAME [every] [tmo]  like tapuntil, with nstap
 *     nsrepeat KEY [n]         n autorepeat key downs (default 5), one every
 *                              2 frames, for a key that is down
 *     nstype TEXT              type characters on the keyboard layout in use
 *                              (NFSU2_KEYLAYOUT can pick another), Shift and
 *                              Option pressed as needed; one every 4 frames
 *     nsfocus out|in           the app loses / regains focus (AppKit's own
 *                              resign/become-active handlers)
 *     nsmouse move X Y         mouse at X, Y (fractions of the window)
 *     nsmouse down|up L|R|M    mouse buttons;  nsmouse scroll DY  (lines)
 *   KEY: DIK name (RETURN ENTER ESCAPE UP DOWN LEFT RIGHT SPACE BACK TAB
 *        LSHIFT RSHIFT LCONTROL RCONTROL LMENU RMENU A..Z 0..9 F1..F12
 *        NUMPAD0..9 ...), or a number (0x1c = DIK code), or VK_xx / vk:0xNN.
 */
#include "cpu.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include "gamepad.h"
#include "kbd.h"
#include <math.h>

/* ---- key tables ------------------------------------------------------- */
typedef struct { const char *nm; unsigned char dik, vk; char ch; } KeyDef;
static const KeyDef K[] = {
    {"ESCAPE",0x01,0x1B,0x1B},{"ESC",0x01,0x1B,0x1B},
    {"1",0x02,'1','1'},{"2",0x03,'2','2'},{"3",0x04,'3','3'},{"4",0x05,'4','4'},
    {"5",0x06,'5','5'},{"6",0x07,'6','6'},{"7",0x08,'7','7'},{"8",0x09,'8','8'},
    {"9",0x0A,'9','9'},{"0",0x0B,'0','0'},{"MINUS",0x0C,0xBD,'-'},{"EQUALS",0x0D,0xBB,'='},
    {"BACK",0x0E,0x08,0x08},{"BACKSPACE",0x0E,0x08,0x08},{"TAB",0x0F,0x09,0x09},
    {"Q",0x10,'Q','q'},{"W",0x11,'W','w'},{"E",0x12,'E','e'},{"R",0x13,'R','r'},
    {"T",0x14,'T','t'},{"Y",0x15,'Y','y'},{"U",0x16,'U','u'},{"I",0x17,'I','i'},
    {"O",0x18,'O','o'},{"P",0x19,'P','p'},{"LBRACKET",0x1A,0xDB,'['},{"RBRACKET",0x1B,0xDD,']'},
    {"RETURN",0x1C,0x0D,0x0D},{"ENTER",0x1C,0x0D,0x0D},{"LCONTROL",0x1D,0x11,0},{"CTRL",0x1D,0x11,0},
    {"A",0x1E,'A','a'},{"S",0x1F,'S','s'},{"D",0x20,'D','d'},{"F",0x21,'F','f'},
    {"G",0x22,'G','g'},{"H",0x23,'H','h'},{"J",0x24,'J','j'},{"K",0x25,'K','k'},
    {"L",0x26,'L','l'},{"SEMICOLON",0x27,0xBA,';'},{"APOSTROPHE",0x28,0xDE,'\''},
    {"GRAVE",0x29,0xC0,'`'},{"LSHIFT",0x2A,0x10,0},{"SHIFT",0x2A,0x10,0},{"BACKSLASH",0x2B,0xDC,'\\'},
    {"Z",0x2C,'Z','z'},{"X",0x2D,'X','x'},{"C",0x2E,'C','c'},{"V",0x2F,'V','v'},
    {"B",0x30,'B','b'},{"N",0x31,'N','n'},{"M",0x32,'M','m'},{"COMMA",0x33,0xBC,','},
    {"PERIOD",0x34,0xBE,'.'},{"SLASH",0x35,0xBF,'/'},{"RSHIFT",0x36,0x10,0},
    {"MULTIPLY",0x37,0x6A,'*'},{"LMENU",0x38,0x12,0},{"ALT",0x38,0x12,0},{"SPACE",0x39,0x20,' '},
    {"CAPITAL",0x3A,0x14,0},
    {"F1",0x3B,0x70,0},{"F2",0x3C,0x71,0},{"F3",0x3D,0x72,0},{"F4",0x3E,0x73,0},
    {"F5",0x3F,0x74,0},{"F6",0x40,0x75,0},{"F7",0x41,0x76,0},{"F8",0x42,0x77,0},
    {"F9",0x43,0x78,0},{"F10",0x44,0x79,0},{"F11",0x57,0x7A,0},{"F12",0x58,0x7B,0},
    {"NUMPAD7",0x47,0x67,'7'},{"NUMPAD8",0x48,0x68,'8'},{"NUMPAD9",0x49,0x69,'9'},
    {"SUBTRACT",0x4A,0x6D,'-'},{"NUMPAD4",0x4B,0x64,'4'},{"NUMPAD5",0x4C,0x65,'5'},
    {"NUMPAD6",0x4D,0x66,'6'},{"ADD",0x4E,0x6B,'+'},{"NUMPAD1",0x4F,0x61,'1'},
    {"NUMPAD2",0x50,0x62,'2'},{"NUMPAD3",0x51,0x63,'3'},{"NUMPAD0",0x52,0x60,'0'},
    {"DECIMAL",0x53,0x6E,'.'},{"NUMPADENTER",0x9C,0x0D,0x0D},{"RCONTROL",0x9D,0x11,0},
    {"DIVIDE",0xB5,0x6F,'/'},{"RMENU",0xB8,0x12,0},{"PAUSE",0xC5,0x13,0},
    {"HOME",0xC7,0x24,0},{"UP",0xC8,0x26,0},{"PRIOR",0xC9,0x21,0},{"PGUP",0xC9,0x21,0},
    {"LEFT",0xCB,0x25,0},{"RIGHT",0xCD,0x27,0},{"END",0xCF,0x23,0},{"DOWN",0xD0,0x28,0},
    {"NEXT",0xD1,0x22,0},{"PGDN",0xD1,0x22,0},{"INSERT",0xD2,0x2D,0},{"DELETE",0xD3,0x2E,0},
};
#define NK (int)(sizeof K / sizeof *K)
static const KeyDef *key_by_dik(unsigned dik){
    for (int i = 0; i < NK; i++) if (K[i].dik == dik) return &K[i];
    return 0;
}
static int parse_key(const char *s, unsigned *dik){
    if (!strncasecmp(s, "DIK_", 4)) s += 4;
    if (!strncasecmp(s, "VK_", 3) || !strncasecmp(s, "vk:", 3)){
        unsigned vk = 0; const char *p = s + 3;
        if (isdigit((unsigned char)*p)) vk = (unsigned)strtoul(p, 0, 0);
        else for (int i = 0; i < NK; i++) if (!strcasecmp(K[i].nm, p)){ vk = K[i].vk; break; }
        for (int i = 0; i < NK; i++) if (K[i].vk == vk){ *dik = K[i].dik; return 1; }
        return 0;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')){ *dik = (unsigned)strtoul(s, 0, 16) & 0xFFu; return 1; }
    for (int i = 0; i < NK; i++) if (!strcasecmp(K[i].nm, s)){ *dik = K[i].dik; return 1; }
    if (isdigit((unsigned char)s[0])){ *dik = (unsigned)strtoul(s, 0, 0) & 0xFFu; return 1; }
    return 0;
}

/* ---- injected input (DirectInput state: runtime/keyboard.c) ------------- */
/* win32_ext.c: guest window message queue */
void win32_post_message(uint32_t msg, uint32_t wp, uint32_t lp);
static void key_event(unsigned dik, int down){
    kbd_set_key(KSRC_SCRIPT, dik & 0xFFu, down);
    const KeyDef *k = key_by_dik(dik);
    if (!k) return;
    uint32_t lp = 1u | ((uint32_t)(dik & 0x7Fu) << 16) | ((dik & 0x80u) ? (1u << 24) : 0u);
    if (down){
        win32_post_message(0x0100u, k->vk, lp);              /* WM_KEYDOWN */
        if (k->ch) win32_post_message(0x0102u, (uint32_t)(unsigned char)k->ch, lp);   /* WM_CHAR */
    } else win32_post_message(0x0101u, k->vk, lp | 0xC0000000u);  /* WM_KEYUP */
}

/* ---- script ----------------------------------------------------------- */
enum { C_DOWN, C_UP, C_TAP, C_TYPE, C_DUMP, C_WAITMEM, C_LOG, C_QUIT, C_PEEK, C_POKE, C_WAITFNG, C_TAPUNTIL,
       C_CLOSE, C_BLUR, C_AUDIORESET, C_AUDIOSTALL, C_WZERO, C_DISPLAYHZ, C_TEXREP, C_HEADLIGHTS,
       C_PAD, C_TAPPAD, C_AXIS, C_TAPPADUNTIL,
       C_NSDOWN, C_NSUP, C_NSTAP, C_NSREPEAT, C_NSTYPE, C_NSFOCUS, C_NSMOUSE, C_NSTAPUNTIL,
       C_LOOP, C_ENDLOOP, C_STATUS, C_STALL, C_CAREER,
       C_COVRESET, C_COVDUMP, C_TRACE, C_UNTRACE, C_EXPECT, C_WAITEXPR, C_MARK, C_EDGE, C_ROAM,
       C_SRVCYCLE, C_CLICK, C_PHOTO, C_BREAKFNG };
/* roam.c: free-roam multiplayer research probes ("roam <sub> ...") */
int roam_cmd(CPU *c, const char *line, long *wait_start);
#define PAD_PLUG 0xFFFFu           /* C_PAD: "pad connect|disconnect" */
/* career.c: test-only career aids (call, memdump, waitx, teleport, autopilot ...) */
int career_is_cmd(const char *cmd);
int career_cmd(CPU *c, const char *line, long *wait_start);
typedef struct {
    int rel, ms; long t;           /* timing */
    int cmd; unsigned key; long n; uint32_t addr, val; float fv;
    char text[200]; int line;
    char *xtext;                   /* trace, mark, edge, expect, waitexpr: the expressions */
} Ev;
static Ev   *g_ev; static int g_nev, g_iev;
static long  g_last_fire_f = 0; static double g_last_fire_ms = 0, g_last_fire_wms = 0, g_wzero = 0;
double hfr_sim_ms(void);                 /* framerate.c: world time, ms */
static long  g_wait_start = -1;
typedef struct { unsigned dik; long at; } Rel;
static Rel   g_rel[64]; static int g_nrel;
static Rel   g_prel[32]; static int g_nprel;   /* pad button releases */
/* NSEvent synthesis (ns* commands): scheduled requests, fired in order */
typedef struct { long at; int type; char key[40]; uint32_t ch; double a, b; } NsReq;
static NsReq g_nsq[1024]; static int g_nnsq;
static void ns_sched(long at, int type, const char *key, uint32_t ch, double a, double b){
    if (g_nnsq >= 1024){ fprintf(stderr, "[autotest] ns queue full\n"); return; }
    NsReq *q = &g_nsq[g_nnsq++];
    q->at = at; q->type = type; q->ch = ch; q->a = a; q->b = b;
    snprintf(q->key, sizeof q->key, "%s", key ? key : "");
}
static void ns_fire(const NsReq *q){
    int ok = 1;
    switch (q->type){
    case KSYN_DOWN: case KSYN_UP: case KSYN_REPEAT: ok = kbdmac_synth_key(q->type, q->key); break;
    case KSYN_CHAR_DOWN: case KSYN_CHAR_UP: ok = kbdmac_synth_char(q->type, q->ch); break;
    default: kbdmac_synth_simple(q->type, q->a, q->b); break;
    }
    if (!ok){
        if (q->type == KSYN_CHAR_DOWN) fprintf(stderr, "[autotest] nstype: no key types U+%04X on this layout\n", q->ch);
        else if (q->type != KSYN_CHAR_UP) fprintf(stderr, "[autotest] ns: unknown key '%s'\n", q->key);
    }
}
/* UTF-8 -> code point; advances *p */
static uint32_t utf8_next(const char **p){
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0]; int n = 0;
    if (c >= 0xF0 && c < 0xF8){ c &= 0x07; n = 3; }
    else if (c >= 0xE0){ c &= 0x0F; n = 2; }
    else if (c >= 0xC0){ c &= 0x1F; n = 1; }
    else if (c >= 0x80){ (*p)++; return 0xFFFD; }
    s++;
    for (int i = 0; i < n; i++, s++){ if ((*s & 0xC0) != 0x80) break; c = (c << 6) | (*s & 0x3F); }
    *p = (const char *)s;
    return c;
}
/* "type": characters still to send */
static char  g_type_q[256]; static int g_type_h, g_type_t; static long g_type_next;

static long   g_frame = 0;               /* guest Present count */
static int    g_fng_log = 0;
static double g_t0 = 0;                  /* ms at first Present */
static long   g_quit_frames = 0, g_status_every = 0;
static int    g_inited = 0;

static double now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ${NAME} -> getenv("NAME").  The result is as long as a script line may
 * be (load_script's 2048): drift's trace lines are longer than 256. */
static void expand_env(char *line, size_t cap){
    char out[2048]; size_t o = 0;
    for (const char *p = line; *p && o + 1 < sizeof out; ){
        const char *e;
        if (p[0] == '$' && p[1] == '{' && (e = strchr(p + 2, '}'))){
            char nm[64]; size_t n = (size_t)(e - p - 2);
            if (n >= sizeof nm) n = sizeof nm - 1;
            memcpy(nm, p + 2, n); nm[n] = 0;
            const char *v = getenv(nm);
            for (; v && *v && o + 1 < sizeof out; v++) out[o++] = *v;
            p = e + 1;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    snprintf(line, cap, "%s", out);
}

static void load_script(const char *path){
    FILE *f = fopen(path, "r");
    if (!f){ fprintf(stderr, "[autotest] cannot open input script %s\n", path); return; }
    char line[2048]; int ln = 0, cap = 0;
    while (fgets(line, sizeof line, f)){
        ln++;
        expand_env(line, sizeof line);
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;
        Ev e; memset(&e, 0, sizeof e); e.line = ln;
        char when[32] = {0}, cmd[32] = {0}; int used = 0;
        if (sscanf(p, "%31s %31s %n", when, cmd, &used) < 2){
            fprintf(stderr, "[autotest] script line %d: parse error\n", ln); continue; }
        char *rest = p + used; rest[strcspn(rest, "\r\n")] = 0;
        const char *w = when;
        if (*w == '+'){ e.rel = 1; w++; }
        if (*w == 'f' || *w == 'F') w++;
        e.t = strtol(w, (char**)&w, 10);
        if (!strncasecmp(w, "ms", 2)) e.ms = 1;
        else if (!strncasecmp(w, "wms", 3)) e.ms = 2;
        char a1[64] = {0}, a2[64] = {0}, a3[64] = {0};
        sscanf(rest, "%63s %63s %63s", a1, a2, a3);
        if      (!strcasecmp(cmd, "down")) e.cmd = C_DOWN;
        else if (!strcasecmp(cmd, "up"))   e.cmd = C_UP;
        else if (!strcasecmp(cmd, "tap"))  { e.cmd = C_TAP; e.n = a2[0] ? atol(a2) : 6; }
        else if (!strcasecmp(cmd, "type")) { e.cmd = C_TYPE; snprintf(e.text, sizeof e.text, "%s", rest); }
        else if (!strcasecmp(cmd, "dump")) { e.cmd = C_DUMP; snprintf(e.text, sizeof e.text, "%s", a1); }
        else if (!strcasecmp(cmd, "log"))  { e.cmd = C_LOG;  snprintf(e.text, sizeof e.text, "%s", rest); }
        else if (!strcasecmp(cmd, "quit")) e.cmd = C_QUIT;
        else if (!strcasecmp(cmd, "close")) e.cmd = C_CLOSE;
        else if (!strcasecmp(cmd, "blur")) { e.cmd = C_BLUR; e.n = a1[0] ? atol(a1) : 3000; }
        else if (!strcasecmp(cmd, "audioreset")) e.cmd = C_AUDIORESET;
        else if (!strcasecmp(cmd, "audiostall")) { e.cmd = C_AUDIOSTALL; e.n = a1[0] ? atol(a1) : 200; }
        else if (!strcasecmp(cmd, "wzero")){ e.cmd = C_WZERO; e.n = !strcasecmp(a1, "now"); }
        else if (!strcasecmp(cmd, "displayhz")) { e.cmd = C_DISPLAYHZ; e.n = atol(a1); }
        else if (!strcasecmp(cmd, "trace")) { e.cmd = C_TRACE; e.n = atol(a1);
            const char *sp = rest; while (*sp && !isspace((unsigned char)*sp)) sp++;
            while (isspace((unsigned char)*sp)) sp++;
            e.xtext = strdup(sp); }
        else if (!strcasecmp(cmd, "untrace")) e.cmd = C_UNTRACE;
        else if (!strcasecmp(cmd, "mark")){ e.cmd = C_MARK; snprintf(e.text, sizeof e.text, "%s", a1);
            e.xtext = strdup(a2); }
        else if (!strcasecmp(cmd, "edge")){ e.cmd = C_EDGE; snprintf(e.text, sizeof e.text, "%s", a1);
            const char *sp = rest; while (*sp && !isspace((unsigned char)*sp)) sp++;
            while (isspace((unsigned char)*sp)) sp++;
            e.xtext = strdup(sp); }
        else if (!strcasecmp(cmd, "expect") || !strcasecmp(cmd, "waitexpr")){
            e.cmd = !strcasecmp(cmd, "expect") ? C_EXPECT : C_WAITEXPR;
            e.xtext = strdup(rest);
            if (e.cmd == C_WAITEXPR){ char a4[32] = {0}; sscanf(rest, "%*s %*s %*s %31s", a4); e.n = a4[0] ? atol(a4) : 0; } }
        else if (!strcasecmp(cmd, "covreset")) e.cmd = C_COVRESET;
        else if (!strcasecmp(cmd, "covdump")) { e.cmd = C_COVDUMP; snprintf(e.text, sizeof e.text, "%s", a1); }
        else if (!strcasecmp(cmd, "roam")){ e.cmd = C_ROAM; snprintf(e.text, sizeof e.text, "%s", rest); }
        else if (!strcasecmp(cmd, "click")) { e.cmd = C_CLICK; e.fv = (float)atof(a1); e.val = (uint32_t)(atof(a2) * 1e6); }
        else if (!strcasecmp(cmd, "srvcycle")) { e.cmd = C_SRVCYCLE; e.n = a1[0] ? atol(a1) : 1; e.val = a2[0] ? (uint32_t)atol(a2) : 0u; }
        else if (!strcasecmp(cmd, "texrep")) { e.cmd = C_TEXREP; e.n = !strcasecmp(a1, "off") ? 0 : !strcasecmp(a1, "on") ? 1 : -1; }
        else if (!strcasecmp(cmd, "headlights")) { e.cmd = C_HEADLIGHTS; snprintf(e.text, sizeof e.text, "%s", a1); }
        else if (!strcasecmp(cmd, "photo")) { e.cmd = C_PHOTO; snprintf(e.text, sizeof e.text, "%s", rest); }
        else if (!strcasecmp(cmd, "waitfng")){ e.cmd = C_WAITFNG;
            snprintf(e.text, sizeof e.text, "%s", a1); e.n = a2[0] ? atol(a2) : 0; }
        else if (!strcasecmp(cmd, "tapuntil")){ e.cmd = C_TAPUNTIL;
            if (!parse_key(a1, &e.key)){ fprintf(stderr, "[autotest] script line %d: unknown key '%s'\n", ln, a1); continue; }
            snprintf(e.text, sizeof e.text, "%s", a2);
            char a4[32] = {0}; sscanf(rest, "%*s %*s %*s %31s", a4);
            e.val = a3[0] ? (uint32_t)atol(a3) : 90u; e.n = a4[0] ? atol(a4) : 0; }
        else if (!strcasecmp(cmd, "peek")){ e.cmd = C_PEEK;
            if (!strncasecmp(a1, "@in", 3)){ e.val = 1; e.addr = (uint32_t)strtoul(a1 + 3, 0, 0); }
            else e.addr = (uint32_t)strtoul(a1, 0, 0);
            e.n = a2[0] ? atol(a2) : 1; if (e.n < 1 || e.n > 64) e.n = 1; }
        else if (!strcasecmp(cmd, "poke")){ e.cmd = C_POKE; e.addr = (uint32_t)strtoul(a1, 0, 0);
            e.val = (uint32_t)strtoul(a2, 0, 0); }
        else if (!strcasecmp(cmd, "loop")){ e.cmd = C_LOOP; e.n = atol(a1); }
        else if (!strcasecmp(cmd, "endloop")) e.cmd = C_ENDLOOP;
        else if (!strcasecmp(cmd, "breakfng")){ e.cmd = C_BREAKFNG; snprintf(e.text, sizeof e.text, "%s", a1); }
        else if (!strcasecmp(cmd, "status")){ e.cmd = C_STATUS; snprintf(e.text, sizeof e.text, "%s", a1); }
        else if (!strcasecmp(cmd, "stall")){ e.cmd = C_STALL; e.n = atol(a1); }
        else if (!strcasecmp(cmd, "waitmem")){ e.cmd = C_WAITMEM;
            e.addr = (uint32_t)strtoul(a1, 0, 0); e.val = (uint32_t)strtoul(a2, 0, 0);
            e.n = a3[0] ? atol(a3) : 0; }
        else if (!strcasecmp(cmd, "pad")){ e.cmd = C_PAD;
            if (!strcasecmp(a1, "connect") || !strcasecmp(a1, "disconnect")){
                e.key = PAD_PLUG; e.n = !strcasecmp(a1, "connect") ? (!strcasecmp(a2, "wheel") ? 2 : 1) : 0; }
            else { int b = gamepad_button_by_name(a1);
                if (b < 0){ fprintf(stderr, "[autotest] script line %d: unknown pad button '%s'\n", ln, a1); continue; }
                e.key = (unsigned)b; e.n = strcasecmp(a2, "up") != 0; } }
        else if (!strcasecmp(cmd, "tappad") || !strcasecmp(cmd, "tappaduntil")){
            int b = gamepad_button_by_name(a1);
            if (b < 0){ fprintf(stderr, "[autotest] script line %d: unknown pad button '%s'\n", ln, a1); continue; }
            e.key = (unsigned)b;
            if (cmd[6]){ e.cmd = C_TAPPADUNTIL; snprintf(e.text, sizeof e.text, "%s", a2);
                char a4[32] = {0}; sscanf(rest, "%*s %*s %*s %31s", a4);
                e.val = a3[0] ? (uint32_t)atol(a3) : 90u; e.n = a4[0] ? atol(a4) : 0; }
            else { e.cmd = C_TAPPAD; e.n = a2[0] ? atol(a2) : 6; } }
        else if (!strcasecmp(cmd, "axis") || !strcasecmp(cmd, "trigger")){ e.cmd = C_AXIS;
            int ax = gamepad_axis_by_name(a1);
            if (ax < 0 || (cmd[0] == 't' || cmd[0] == 'T') != (ax >= PAD_AXIS_LT)){
                fprintf(stderr, "[autotest] script line %d: unknown %s '%s'\n", ln, cmd, a1); continue; }
            e.key = (unsigned)ax; e.fv = (float)atof(a2); }
        else if (!strcasecmp(cmd, "nsdown") || !strcasecmp(cmd, "nsup") || !strcasecmp(cmd, "nstap") ||
                 !strcasecmp(cmd, "nsrepeat")){
            e.cmd = !strcasecmp(cmd, "nsdown") ? C_NSDOWN : !strcasecmp(cmd, "nsup") ? C_NSUP
                  : !strcasecmp(cmd, "nstap") ? C_NSTAP : C_NSREPEAT;
            if (!a1[0]){ fprintf(stderr, "[autotest] script line %d: %s needs a key\n", ln, cmd); continue; }
            snprintf(e.text, sizeof e.text, "%s", a1);
            e.n = a2[0] ? atol(a2) : (e.cmd == C_NSTAP ? 6 : 5); }
        else if (!strcasecmp(cmd, "nstapuntil")){ e.cmd = C_NSTAPUNTIL;
            snprintf(e.text, sizeof e.text, "%s", a1); snprintf(e.text + 48, sizeof e.text - 48, "%s", a2);
            char a4[32] = {0}; sscanf(rest, "%*s %*s %*s %31s", a4);
            e.val = a3[0] ? (uint32_t)atol(a3) : 90u; e.n = a4[0] ? atol(a4) : 0; }
        else if (!strcasecmp(cmd, "nstype")){ e.cmd = C_NSTYPE; snprintf(e.text, sizeof e.text, "%s", rest);
            for (size_t l = strlen(e.text); l && (e.text[l - 1] == ' ' || e.text[l - 1] == '\t'); ) e.text[--l] = 0; }
        else if (!strcasecmp(cmd, "nsfocus")){ e.cmd = C_NSFOCUS; e.n = !strcasecmp(a1, "in"); }
        else if (!strcasecmp(cmd, "nsmouse")){ e.cmd = C_NSMOUSE;
            if (!strcasecmp(a1, "move")){ e.n = KSYN_MOUSE_MOVE; e.fv = (float)atof(a2); e.val = 0;
                                          e.addr = 0; snprintf(e.text, sizeof e.text, "%s", a3); }
            else if (!strcasecmp(a1, "down") || !strcasecmp(a1, "up")){
                e.n = !strcasecmp(a1, "down") ? KSYN_MOUSE_DOWN : KSYN_MOUSE_UP;
                e.fv = (float)(a2[0] == 'R' || a2[0] == 'r' ? 1 : a2[0] == 'M' || a2[0] == 'm' ? 2 : 0); }
            else if (!strcasecmp(a1, "scroll")){ e.n = KSYN_SCROLL; e.fv = (float)atof(a2); }
            else { fprintf(stderr, "[autotest] script line %d: nsmouse move|down|up|scroll\n", ln); continue; } }
        else if (career_is_cmd(cmd)){ e.cmd = C_CAREER; snprintf(e.text, sizeof e.text, "%s %s", cmd, rest); }
        else { fprintf(stderr, "[autotest] script line %d: unknown command '%s'\n", ln, cmd); continue; }
        if (e.cmd == C_DOWN || e.cmd == C_UP || e.cmd == C_TAP){
            if (!parse_key(a1, &e.key)){
                fprintf(stderr, "[autotest] script line %d: unknown key '%s'\n", ln, a1); continue; } }
        if (g_nev == cap){ cap = cap ? cap * 2 : 64; g_ev = realloc(g_ev, (size_t)cap * sizeof *g_ev); }
        g_ev[g_nev++] = e;
    }
    fclose(f);
    fprintf(stderr, "[autotest] input script %s: %d events\n", path, g_nev);
}


/* ---- front-end package tracking ----------------------------------------
 * The top package is the game's to find (game.h top_screen); the screens the
 * game initialises come from its own debug printf (autotest_note_screen). */
const char *autotest_top_fng(void);
static char g_screen[64];          /* last "Init Package"/"SwitchFEngPackages ... to" name */
static long g_screen_frame = -1;
/* Every screen initialised in the frame of the last one: returning to the
 * main menu prints UI_Main, GarageMain and Chyron_FE in one frame, so the
 * last name alone cannot tell the main menu from an Options page (the top
 * package stays GarageMain on both). */
static char g_batch[8][64]; static int g_nbatch = 0;
void autotest_note_screen(const char *nm){
    if (!nm) return;
    if (g_screen_frame != g_frame) g_nbatch = 0;
    if (g_nbatch < 8) snprintf(g_batch[g_nbatch++], 64, "%s", nm);
    /* a screen is often noted twice in a row (its Init Package and the switch to it): logged once (the
     * browser app logs screens for crash reports, settings.ts APP_DEFAULTS) */
    int again = !strncmp(g_screen, nm, sizeof g_screen - 1);
    snprintf(g_screen, sizeof g_screen, "%s", nm);
    g_screen_frame = g_frame;
    if (g_fng_log && !again) fprintf(stderr, "[fng] f=%ld screen %s\n", g_frame, g_screen);
}
/* the current front-end screen, or 0 (also for runtime/dinput_pad.c and
 * runtime/discord.c) */
const char *autotest_screen(void){ return g_screen[0] ? g_screen : 0; }
/* the i-th screen initialised in the frame of the current one (in order,
 * the current one last), or 0 past the end (runtime/discord.c) */
const char *autotest_screen_batch(int i){ return i >= 0 && i < g_nbatch ? g_batch[i] : 0; }
/* NAME matches the top package (GetTopPackage), the current screen, or a
 * screen initialised together with it.  A NAME with a qualifier after the
 * package's (MC_Main.fng:LABEL, MainMenu.fng@HASH ...) names a state within
 * that package, the game's to judge (game.h fe_match), as are alternatives
 * (A.fng|B.fng:LABEL), bare packages among them, and -NAME, the package gone; a game without fe_match
 * (UG2) compares every NAME whole, as before */
static int fe_is(const char *want){
    if (g_game.fe_match && (want[0] == '-' || want[strcspn(want, ":@'^|")])) return g_game.fe_match(want);
    const char *t = autotest_top_fng();
    if ((t && !strcasecmp(t, want)) || (g_screen[0] && !strcasecmp(g_screen, want))) return 1;
    for (int i = 0; i < g_nbatch; i++) if (!strcasecmp(g_batch[i], want)) return 1;
    return 0;
}
/* NAME may list alternatives, A|B: any of them (a wait for one of two outcomes) */
static int fe_is(const char *want){
    if (!strchr(want, '|')) return fe_is1(want);
    char one[200];
    for (const char *p = want;; p++){
        size_t n = strcspn(p, "|");
        if (n && n < sizeof one){ memcpy(one, p, n); one[n] = 0; if (fe_is1(one)) return 1; }
        p += n;
        if (!*p) return 0;
    }
}
const char *autotest_top_fng(void){ return g_game.top_screen ? g_game.top_screen() : 0; }

/* ---- periodic memory trace ("trace" command) ------------------------- */
#define TR_MAX 24
static struct { char name[24]; char type; char expr[80]; } g_tr[TR_MAX];
static int g_ntr = 0; static long g_tr_every = 0;
/* $P and $Rn: the game's racers (game.h racer) */
static uint32_t tr_racer(int want_human, int slot){ return g_game.racer ? g_game.racer(want_human, slot) : 0u; }
static uint32_t tr_eval(const char **pp, int *ok){
    const char *p = *pp; uint32_t v = 0;
    while (*p == ' ') p++;
    if (*p == '$'){
        if (p[1] == 'P'){ v = tr_racer(1, 0); p += 2; }
        else if (p[1] == 'R' && isdigit((unsigned char)p[2])){ v = tr_racer(0, p[2] - '0'); p += 3; }
        else if (p[1] == 'O' && p[2] == '('){          /* $O(PACKAGE:HASH): a front-end object (game.h fe_object) */
            char pkg[64]; int n = 0;
            for (p += 3; *p && *p != ':' && *p != ')' && n < 63; p++) pkg[n++] = *p;
            pkg[n] = 0;
            uint32_t h = *p == ':' ? (uint32_t)strtoul(p + 1, (char **)&p, 0) : 0u;
            if (*p == ')') p++;
            v = g_game.fe_object && h ? g_game.fe_object(pkg, h) : 0u;
        }
        else p++;
        if (!v) *ok = 0;
    } else if (*p == '['){
        p++; uint32_t a = tr_eval(&p, ok);
        if (*p == ']') p++;
        if (*ok && mem_is_committed(a) && mem_is_committed(a + 3)) v = MR32(a); else *ok = 0;
    } else v = (uint32_t)strtoul(p, (char **)&p, 0);
    while (*p == '+' || *p == '-'){
        int neg = *p == '-'; p++;
        uint32_t k = (uint32_t)strtoul(p, (char **)&p, 0);
        v = neg ? v - k : v + k;
    }
    *pp = p; return v;
}
static void trace_set(const char *list, long every){
    g_ntr = 0; g_tr_every = every > 0 ? every : 1;
    const char *p = list;
    while (*p && g_ntr < TR_MAX){
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *eq = strchr(p, '='), *end = p; while (*end && !isspace((unsigned char)*end)) end++;
        if (!eq || eq > end){ p = end; continue; }
        int nl = (int)(eq - p); if (nl > 23) nl = 23;
        memcpy(g_tr[g_ntr].name, p, (size_t)nl); g_tr[g_ntr].name[nl] = 0;
        const char *x = eq + 1; char ty = 'f';
        if (x[0] && x[1] == ':' && strchr("fixbwd", x[0])){ ty = x[0]; x += 2; }
        int xl = (int)(end - x); if (xl > 79) xl = 79;
        memcpy(g_tr[g_ntr].expr, x, (size_t)xl); g_tr[g_ntr].expr[xl] = 0;
        g_tr[g_ntr].type = ty; g_ntr++; p = end;
    }
}
static void trace_emit(long frame, double tms){
    char line[2048]; int n = snprintf(line, sizeof line, "[trace] f=%ld t=%.0f w=%.3f", frame, tms, hfr_sim_ms());
    for (int i = 0; i < g_ntr && n < (int)sizeof line - 64; i++){
        const char *x = g_tr[i].expr; int ok = 1; uint32_t a = tr_eval(&x, &ok);
        int sz = g_tr[i].type == 'b' ? 1 : g_tr[i].type == 'w' ? 2 : g_tr[i].type == 'd' ? 8 : 4;
        if (!ok || !mem_is_committed(a) || !mem_is_committed(a + (uint32_t)sz - 1)){
            n += snprintf(line + n, sizeof line - (size_t)n, " %s=?", g_tr[i].name); continue; }
        switch (g_tr[i].type){
        case 'i': n += snprintf(line + n, sizeof line - (size_t)n, " %s=%d", g_tr[i].name, (int32_t)MR32(a)); break;
        case 'x': n += snprintf(line + n, sizeof line - (size_t)n, " %s=0x%08x", g_tr[i].name, MR32(a)); break;
        case 'b': n += snprintf(line + n, sizeof line - (size_t)n, " %s=%u", g_tr[i].name, MR8(a)); break;
        case 'w': n += snprintf(line + n, sizeof line - (size_t)n, " %s=%u", g_tr[i].name, MR16(a)); break;
        case 'd': n += snprintf(line + n, sizeof line - (size_t)n, " %s=%.9g", g_tr[i].name, MRF64(a)); break;
        default:  n += snprintf(line + n, sizeof line - (size_t)n, " %s=%.7g", g_tr[i].name, (double)MRF32(a)); break;
        }
    }
    fprintf(stderr, "%s\n", line);
}

/* "mark" values */
#define NMARK 16
static struct { char name[24]; double v; } g_mark[NMARK]; static int g_nmark;
static double *mark_ref(const char *name, int create){
    for (int i = 0; i < g_nmark; i++) if (!strcmp(g_mark[i].name, name)) return &g_mark[i].v;
    if (!create || g_nmark == NMARK) return 0;
    snprintf(g_mark[g_nmark].name, 24, "%s", name); g_mark[g_nmark].v = 0;
    return &g_mark[g_nmark++].v;
}
static double tr_value(const char *ex, int *okp){
    if (ex[0] == '@'){ double *m = mark_ref(ex + 1, 0); *okp = m != 0; return m ? *m : NAN; }
    const char *x = ex; char ty = 'f';
    if (x[0] && x[1] == ':' && strchr("fixbwd", x[0])){ ty = x[0]; x += 2; }
    int ok = 1; uint32_t a = tr_eval(&x, &ok);
    int sz = ty == 'b' ? 1 : ty == 'w' ? 2 : ty == 'd' ? 8 : 4;
    if (!ok || !mem_is_committed(a) || !mem_is_committed(a + (uint32_t)sz - 1)){ *okp = 0; return NAN; }
    *okp = 1;
    return ty == 'i' ? (double)(int32_t)MR32(a) : ty == 'x' ? (double)MR32(a) : ty == 'b' ? MR8(a)
         : ty == 'w' ? MR16(a) : ty == 'd' ? MRF64(a) : (double)MRF32(a);
}
/* "EXPR OP VALUE": 1 true, 0 false, -1 unreadable; *got = the value read */
static int cond_eval(const char *spec, double *got, char *desc, size_t dn){
    char ex[96] = {0}, op[4] = {0}, vs[48] = {0}; double want = 0;
    if (sscanf(spec, "%95s %3s %47s", ex, op, vs) < 3){ snprintf(desc, dn, "parse error: %s", spec); return -1; }
    if (vs[0] == '@'){
        char *plus = strpbrk(vs + 1, "+-"); double add = plus ? atof(plus) : 0.0;
        if (plus) *plus = 0;
        int f = 0;
        for (int i = 0; i < g_nmark; i++) if (!strcmp(g_mark[i].name, vs + 1)){ want = g_mark[i].v + add; f = 1; }
        if (!f){ snprintf(desc, dn, "unknown mark %s", vs); return -1; }
    } else want = atof(vs);
    if (ex[0] == '@'){
        int ok; double v = tr_value(ex, &ok);
        if (!ok){ snprintf(desc, dn, "unknown mark %s", ex); *got = NAN; return -1; }
        *got = v;
        int r = !strcmp(op, ">") ? v > want : !strcmp(op, ">=") ? v >= want : !strcmp(op, "<") ? v < want
              : !strcmp(op, "<=") ? v <= want : !strcmp(op, "==") ? v == want : !strcmp(op, "!=") ? v != want : -1;
        snprintf(desc, dn, "%s %s %g (value %.9g)", ex, op, want, v);
        return r;
    }
    const char *x = ex; char ty = 'f';
    if (x[0] && x[1] == ':' && strchr("fixbwd", x[0])){ ty = x[0]; x += 2; }
    int ok = 1; uint32_t a = tr_eval(&x, &ok);
    int sz = ty == 'b' ? 1 : ty == 'w' ? 2 : ty == 'd' ? 8 : 4;
    if (!ok || !mem_is_committed(a) || !mem_is_committed(a + (uint32_t)sz - 1)){
        snprintf(desc, dn, "%s unreadable", ex); *got = NAN; return -1; }
    double v = ty == 'i' ? (double)(int32_t)MR32(a) : ty == 'x' ? (double)MR32(a) : ty == 'b' ? MR8(a)
             : ty == 'w' ? MR16(a) : ty == 'd' ? MRF64(a) : (double)MRF32(a);
    *got = v;
    int r = !strcmp(op, ">") ? v > want : !strcmp(op, ">=") ? v >= want : !strcmp(op, "<") ? v < want
          : !strcmp(op, "<=") ? v <= want : !strcmp(op, "==") ? v == want : !strcmp(op, "!=") ? v != want : -1;
    snprintf(desc, dn, "%s %s %g (value %.9g)", ex, op, want, v);
    return r;
}

/* "edge" counters, evaluated every frame */
#define NEDGE 8
static struct { char name[24]; char *spec; int last; } g_edge[NEDGE]; static int g_nedge;
static void edges_tick(void){
    for (int i = 0; i < g_nedge; i++){
        double v; char d[200];
        int r = cond_eval(g_edge[i].spec, &v, d, sizeof d) == 1;
        if (r && !g_edge[i].last){ double *m = mark_ref(g_edge[i].name, 1); if (m) *m += 1.0; }
        g_edge[i].last = r;
    }
}

void metal_request_dump(long guest_frame, const char *label);   /* metal.m */
void metal_request_quit(void);                                   /* metal.m */
int  metal_window_ready(void);
extern void (*g_report_fn)(void);

static void do_quit(const char *why){
    fprintf(stderr, "\n[autotest] quit at guest frame %ld (%s)\n", g_frame, why);
    if (getenv("WINDOW") && metal_window_ready()){ metal_request_quit(); return; }
    { extern void discord_shutdown(void); discord_shutdown(); }   /* _exit skips atexit: clear the presence first */
    if (g_report_fn) g_report_fn();
    fflush(stdout); fflush(stderr);
    _exit(0);
}

static void status_line(double t, double tms, const char *label);
/* loop/endloop: start line and passes left (-1 = forever) */
static struct { int start; long left, pass; } g_lstk[8]; static int g_nl;
/* returns 1 if the event fired (or is finished), 0 if it must wait */
static int fire(CPU *c, Ev *e, double tms){
    switch (e->cmd){
    case C_DOWN: key_event(e->key, 1); break;
    case C_UP:   key_event(e->key, 0); break;
    case C_TAP:
        key_event(e->key, 1);
        if (g_nrel < 64){ g_rel[g_nrel].dik = e->key; g_rel[g_nrel].at = g_frame + (e->n > 0 ? e->n : 1); g_nrel++; }
        break;
    case C_TYPE: {
        for (const char *s = e->text; *s && ((g_type_t + 1) & 255) != g_type_h; s++){
            g_type_q[g_type_t] = *s; g_type_t = (g_type_t + 1) & 255; }
        g_type_next = g_frame;
        break; }
    case C_DUMP: metal_request_dump(g_frame, e->text[0] ? e->text : "dump"); break;
    case C_LOG:  fprintf(stderr, "[autotest] f=%ld t=%.0fms w=%.1fms LOG %s\n", g_frame, tms, hfr_sim_ms(), e->text); break;
    case C_CLICK: { void metal_request_click(double, double);   /* fractions of the window content */
        metal_request_click(e->fv, e->val / 1e6); break; }
    case C_SRVCYCLE: { int module_test_cycle(CPU *, int, int);   /* module.c: LAN server start/stop */
        module_test_cycle(c, (int)e->n, (int)e->val); break; }
    case C_QUIT: do_quit("script"); break;
    case C_CLOSE: { void lifecycle_request_quit(const char *); lifecycle_request_quit("autotest close"); break; }
    case C_BLUR:  { void lifecycle_simulate_blur(long);
        fprintf(stderr, "[autotest] f=%ld blur %ld ms\n", g_frame, e->n); lifecycle_simulate_blur(e->n); break; }
    case C_AUDIORESET: { void audio_device_changed(const char *);
        audio_device_changed("autotest audioreset"); break; }
    case C_AUDIOSTALL:
#ifdef __wasm__
        { void audio_test_stall(int); fprintf(stderr, "[autotest] f=%ld audiostall %ld ms (test-only)\n", g_frame, e->n);
          audio_test_stall((int)e->n); }
#else
        fprintf(stderr, "[autotest] f=%ld audiostall: the browser's audio pump only\n", g_frame);
#endif
        break;
    case C_WZERO: {
        double hfr_race_start_ms(void);
        double rs = hfr_race_start_ms();
        g_wzero = (!e->n && rs >= 0) ? rs : hfr_sim_ms();
        fprintf(stderr, "[autotest] f=%ld t=%.0fms world-time origin at w=%.3fms (now w=%.3fms)\n",
                g_frame, tms, g_wzero, hfr_sim_ms());
        break; }
    case C_PHOTO: {        /* "photo ...": photo mode as the page drives it (photo.c photo_script) */
        int photo_script(const char *s);
        fprintf(stderr, "[autotest] f=%ld t=%.0fms photo %s%s\n", g_frame, tms, e->text, photo_script(e->text) ? "" : " (not known)");
        break; }
    case C_HEADLIGHTS: {   /* "headlights MODE": the other cars' headlights (headlights.c), A/B in one run */
        int headlights_set(const char *mode);
        fprintf(stderr, "[autotest] f=%ld t=%.0fms headlights %s%s\n", g_frame, tms, e->text,
                headlights_set(e->text) ? "" : " (not known here)");
        break; }
    case C_TEXREP: {       /* "texrep on|off|toggle": show / hide a texture pack (A/B) */
        void texrep_set_shown(int on);
        texrep_set_shown((int)e->n);
        fprintf(stderr, "[autotest] f=%ld t=%.0fms texrep %ld\n", g_frame, tms, e->n);
        break; }
    case C_DISPLAYHZ: {
        void hfr_set_display_hz(int hz);
        hfr_set_display_hz((int)e->n);
        fprintf(stderr, "[autotest] f=%ld t=%.0fms display refresh set to %ld Hz (0 = detected)\n",
                g_frame, tms, e->n);
        break; }
    case C_TRACE: trace_set(e->xtext ? e->xtext : "", e->n);
        fprintf(stderr, "[autotest] f=%ld trace every %ld: %d expressions\n", g_frame, g_tr_every, g_ntr);
        break;
    case C_UNTRACE: g_ntr = 0; break;
    case C_MARK: { int ok; double v = tr_value(e->xtext ? e->xtext : "", &ok);
        int i; for (i = 0; i < g_nmark && strcmp(g_mark[i].name, e->text); i++) ;
        if (i == g_nmark && g_nmark < NMARK) snprintf(g_mark[g_nmark++].name, 24, "%s", e->text);
        if (i < NMARK) g_mark[i].v = v;
        fprintf(stderr, "[autotest] f=%ld mark %s = %.9g%s\n", g_frame, e->text, v, ok ? "" : " (unreadable)");
        break; }
    case C_EDGE:
        if (g_nedge < NEDGE){
            snprintf(g_edge[g_nedge].name, 24, "%s", e->text); g_edge[g_nedge].spec = e->xtext ? e->xtext : (char *)"";
            g_edge[g_nedge].last = 0; mark_ref(e->text, 1); g_nedge++; }
        fprintf(stderr, "[autotest] f=%ld edge @%s: %s\n", g_frame, e->text, e->xtext ? e->xtext : "");
        break;
    case C_EXPECT: { double v; char d[200]; int r = cond_eval(e->xtext ? e->xtext : "", &v, d, sizeof d);
        fprintf(stderr, "[expect] %s f=%ld w=%.0f line %d: %s\n", r == 1 ? "PASS" : "FAIL", g_frame, hfr_sim_ms(), e->line, d);
        break; }
    case C_WAITEXPR: { double v; char d[200];
        if (g_wait_start < 0) g_wait_start = g_frame;
        int r = cond_eval(e->xtext ? e->xtext : "", &v, d, sizeof d);
        if (r != 1){
            if (e->n && g_frame - g_wait_start >= e->n){
                fprintf(stderr, "[autotest] f=%ld waitexpr TIMEOUT after %ld frames: %s\n", g_frame, g_frame - g_wait_start, d);
                g_wait_start = -1; return 1; }
            return 0; }
        fprintf(stderr, "[autotest] f=%ld waitexpr satisfied after %ld frames: %s\n", g_frame, g_frame - g_wait_start, d);
        g_wait_start = -1; break; }
    case C_COVRESET: { void cov_reset(void); cov_reset();
        fprintf(stderr, "[autotest] f=%ld covreset\n", g_frame); break; }
    case C_COVDUMP: { void cov_dump(const char *); cov_dump(e->text[0] ? e->text : "cov.txt");
        fprintf(stderr, "[autotest] f=%ld covdump %s\n", g_frame, e->text); break; }
    case C_WAITFNG: {
        if (g_wait_start < 0) g_wait_start = g_frame;
        const char *t = autotest_top_fng();
        if (!fe_is(e->text)){
            if (e->n && g_frame - g_wait_start >= e->n){
                fprintf(stderr, "[autotest] f=%ld waitfng %s TIMEOUT after %ld frames (top=%s screen=%s)\n",
                        g_frame, e->text, g_frame - g_wait_start, t ? t : "-", g_screen);
                g_wait_start = -1; return 1; }
            return 0; }
        fprintf(stderr, "[autotest] f=%ld waitfng %s satisfied after %ld frames\n",
                g_frame, e->text, g_frame - g_wait_start);
        g_wait_start = -1;
        break; }
    case C_TAPUNTIL: {
        static long last_tap = -1000000;
        if (g_wait_start < 0){ g_wait_start = g_frame; last_tap = -1000000; }
        const char *t = autotest_top_fng();
        if (fe_is(e->text)){
            fprintf(stderr, "[autotest] f=%ld tapuntil %s satisfied after %ld frames\n",
                    g_frame, e->text, g_frame - g_wait_start);
            g_wait_start = -1; break; }
        if (e->n && g_frame - g_wait_start >= e->n){
            fprintf(stderr, "[autotest] f=%ld tapuntil %s TIMEOUT (top=%s)\n", g_frame, e->text, t ? t : "-");
            g_wait_start = -1; return 1; }
        if (g_frame - last_tap >= (long)e->val){
            last_tap = g_frame; key_event(e->key, 1);
            if (g_fng_log >= 3) fprintf(stderr, "[autotest] f=%ld tapuntil tap %s\n", g_frame, e->text);
            if (g_nrel < 64){ g_rel[g_nrel].dik = e->key; g_rel[g_nrel].at = g_frame + 6; g_nrel++; } }
        return 0; }
    case C_PAD:
        if (e->key == PAD_PLUG) gamepad_synth_connect((int)e->n);
        else gamepad_synth_button((int)e->key, (int)e->n);
        break;
    case C_TAPPAD:
        gamepad_synth_button((int)e->key, 1);
        if (g_nprel < 32){ g_prel[g_nprel].dik = e->key; g_prel[g_nprel].at = g_frame + (e->n > 0 ? e->n : 1); g_nprel++; }
        break;
    case C_AXIS: gamepad_synth_axis((int)e->key, e->fv); break;
    case C_NSDOWN: ns_sched(g_frame, KSYN_DOWN, e->text, 0, 0, 0); break;
    case C_NSUP:   ns_sched(g_frame, KSYN_UP, e->text, 0, 0, 0); break;
    case C_NSTAP:
        ns_sched(g_frame, KSYN_DOWN, e->text, 0, 0, 0);
        ns_sched(g_frame + (e->n > 0 ? e->n : 1), KSYN_UP, e->text, 0, 0, 0);
        break;
    case C_NSREPEAT:
        for (long i = 0; i < e->n && i < 200; i++) ns_sched(g_frame + 2 * i, KSYN_REPEAT, e->text, 0, 0, 0);
        break;
    case C_NSTAPUNTIL: {
        static long last_nstap = -1000000;
        const char *want = e->text + 48;
        if (g_wait_start < 0){ g_wait_start = g_frame; last_nstap = -1000000; }
        if (fe_is(want)){
            fprintf(stderr, "[autotest] f=%ld nstapuntil %s satisfied after %ld frames\n",
                    g_frame, want, g_frame - g_wait_start);
            g_wait_start = -1; break; }
        if (e->n && g_frame - g_wait_start >= e->n){
            const char *t = autotest_top_fng();
            fprintf(stderr, "[autotest] f=%ld nstapuntil %s TIMEOUT (top=%s)\n", g_frame, want, t ? t : "-");
            g_wait_start = -1; return 1; }
        if (g_frame - last_nstap >= (long)e->val){
            last_nstap = g_frame;
            ns_sched(g_frame, KSYN_DOWN, e->text, 0, 0, 0);
            ns_sched(g_frame + 6, KSYN_UP, e->text, 0, 0, 0); }
        return 0; }
    case C_NSTYPE: {
        long at = g_frame;
        for (const char *p = e->text; *p; at += 4){
            uint32_t u = utf8_next(&p);
            ns_sched(at, KSYN_CHAR_DOWN, 0, u, 0, 0);
            ns_sched(at + 2, KSYN_CHAR_UP, 0, u, 0, 0);
        }
        break; }
    case C_NSFOCUS: ns_sched(g_frame, e->n ? KSYN_FOCUS_IN : KSYN_FOCUS_OUT, 0, 0, 0, 0);
        fprintf(stderr, "[autotest] f=%ld nsfocus %s\n", g_frame, e->n ? "in" : "out"); break;
    case C_NSMOUSE: ns_sched(g_frame, (int)e->n, 0, 0, e->fv, e->text[0] ? atof(e->text) : 0); break;
    case C_ROAM:
        if (!roam_cmd(c, e->text, &g_wait_start)) return 0;
        break;
    case C_TAPPADUNTIL: {
        static long last_ptap = -1000000;
        if (g_wait_start < 0){ g_wait_start = g_frame; last_ptap = -1000000; }
        const char *t = autotest_top_fng();
        if (fe_is(e->text)){
            fprintf(stderr, "[autotest] f=%ld tappaduntil %s satisfied after %ld frames\n",
                    g_frame, e->text, g_frame - g_wait_start);
            g_wait_start = -1; break; }
        if (e->n && g_frame - g_wait_start >= e->n){
            fprintf(stderr, "[autotest] f=%ld tappaduntil %s TIMEOUT (top=%s)\n", g_frame, e->text, t ? t : "-");
            g_wait_start = -1; return 1; }
        if (g_frame - last_ptap >= (long)e->val){
            last_ptap = g_frame; gamepad_synth_button((int)e->key, 1);
            if (g_fng_log >= 3) fprintf(stderr, "[autotest] f=%ld tappaduntil tap %s\n", g_frame, e->text);
            if (g_nprel < 32){ g_prel[g_nprel].dik = e->key; g_prel[g_nprel].at = g_frame + 6; g_nprel++; } }
        return 0; }
    case C_LOOP:
        if (g_nl < 8){ g_lstk[g_nl].start = g_iev; g_lstk[g_nl].left = e->n > 0 ? e->n : -1; g_lstk[g_nl].pass = 1; g_nl++; }
        fprintf(stderr, "[autotest] f=%ld loop (line %d) pass 1\n", g_frame, e->line);
        break;
    case C_ENDLOOP:
        if (g_nl){
            typeof(&g_lstk[0]) L = &g_lstk[g_nl - 1];
            if (L->left < 0 || --L->left > 0){
                g_iev = L->start;                    /* the caller's g_iev++ enters the body again */
                fprintf(stderr, "[autotest] f=%ld loop (line %d) pass %ld\n", g_frame, g_ev[L->start].line, ++L->pass);
            } else g_nl--;
        }
        break;
    case C_BREAKFNG:                         /* leave the innermost loop when NAME holds now */
        if (g_nl && fe_is(e->text)){
            int depth = 0, i;
            for (i = g_iev + 1; i < g_nev; i++){
                if (g_ev[i].cmd == C_LOOP) depth++;
                else if (g_ev[i].cmd == C_ENDLOOP && depth-- == 0) break;
            }
            fprintf(stderr, "[autotest] f=%ld breakfng %s: out of the loop (line %d) in pass %ld\n",
                    g_frame, e->text, g_ev[g_lstk[g_nl - 1].start].line, g_lstk[g_nl - 1].pass);
            g_nl--;
            g_iev = i < g_nev ? i : g_nev - 1;   /* the caller's g_iev++ goes on after its endloop */
        } else fprintf(stderr, "[autotest] f=%ld breakfng %s: no%s\n", g_frame, e->text, g_nl ? "" : " (not in a loop)");
        break;
    case C_STATUS: {                         /* inside a loop the label gets the pass number */
        char lb[112]; snprintf(lb, sizeof lb, "%s", e->text[0] ? e->text : "mark");
        if (g_nl) snprintf(lb + strlen(lb), sizeof lb - strlen(lb), "_%ld", g_lstk[g_nl - 1].pass);
        status_line(now_ms(), tms, lb);
        break; }
    case C_STALL:
        fprintf(stderr, "[autotest] f=%ld stall %ld s (test-only)\n", g_frame, e->n);
        sleep((unsigned)(e->n > 0 ? e->n : 1));
        break;
    case C_CAREER:
        if (!career_cmd(c, e->text, &g_wait_start)) return 0;
        break;
    case C_PEEK:
        for (long i = 0; i < e->n; i++){
            uint32_t dipad_input_ctx(void);
            uint32_t base = e->val ? dipad_input_ctx() : 0u;
            if (e->val && !base){ fprintf(stderr, "[autotest] f=%ld peek @in: no input controller yet\n", g_frame); break; }
            uint32_t a = base + e->addr + 4u * (uint32_t)i;
            if (!mem_is_committed(a + 3)){ fprintf(stderr, "[autotest] f=%ld peek [0x%08x] = <unmapped>\n", g_frame, a); continue; }
            uint32_t v = MR32(a); float fv; memcpy(&fv, &v, 4);
            fprintf(stderr, "[autotest] f=%ld peek [0x%08x] = 0x%08x (%u, %g)\n", g_frame, a, v, v, (double)fv);
        }
        break;
    case C_POKE:
        if (mem_is_committed(e->addr + 3)){ MR32(e->addr) = e->val;
            fprintf(stderr, "[autotest] f=%ld poke [0x%08x] <- 0x%08x\n", g_frame, e->addr, e->val); }
        break;
    case C_WAITMEM: {
        if (g_wait_start < 0) g_wait_start = g_frame;
        uint32_t v = mem_is_committed(e->addr + 3) ? MR32(e->addr) : 0xDEADBEEFu;
        if (v != e->val){
            if (e->n && g_frame - g_wait_start >= e->n){
                fprintf(stderr, "[autotest] f=%ld waitmem [0x%08x]==%u TIMEOUT after %ld frames (value %u)\n",
                        g_frame, e->addr, e->val, g_frame - g_wait_start, v);
                g_wait_start = -1; return 1; }
            return 0; }
        fprintf(stderr, "[autotest] f=%ld waitmem [0x%08x]==%u satisfied after %ld frames\n",
                g_frame, e->addr, e->val, g_frame - g_wait_start);
        g_wait_start = -1;
        break; }
    }
    (void)c;
    if (e->cmd != C_LOG && e->cmd != C_WAITMEM && e->cmd != C_PEEK && e->cmd != C_WAITFNG && e->cmd != C_TAPUNTIL &&
        e->cmd != C_TAPPADUNTIL && e->cmd != C_NSTAPUNTIL && e->cmd != C_LOOP && e->cmd != C_ENDLOOP && e->cmd != C_BREAKFNG &&
        e->cmd != C_STATUS && e->cmd != C_CAREER && e->cmd != C_WAITEXPR && e->cmd != C_ROAM && getenv("NFSU2_INPUT_LOG"))
        fprintf(stderr, "[autotest] f=%ld line %d fired\n", g_frame, e->line);
    return 1;
}

static double g_st_t = 0; static long g_st_f = 0; static double g_st_cpu = 0, g_st_gcpu = 0;
#ifdef __wasm__
/* The browser has no CPU clocks: busy time of the guest threads instead (the
 * guest lock held minus host waits, win32_ext.c).  cpu= is then the guest
 * threads' share of one core (the page's renderer and audio not included). */
double win32_thread_busy_ms(void); double win32_busy_all_ms(void);
static double thread_cpu_ms(void){ return win32_thread_busy_ms(); }
static double proc_cpu_s(void){ return win32_busy_all_ms() / 1e3; }
#else
static double thread_cpu_ms(void){
    struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
static double proc_cpu_s(void){
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
#endif

static void status_line(double t, double tms, const char *label){
    double cpu = proc_cpu_s(), gcpu = thread_cpu_ms();
    double dt = (t - g_st_t) / 1e3; long nf = g_frame - g_st_f;
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    /* cpu = whole process (% of one core); gcpu = guest main thread CPU
     * per frame (the bottleneck; includes the game's own limiter spin) */
    /* mem: host footprint, guest heap live/peak, VA reserved, COM objects
     * and Lock buffers, Metal allocated size and live textures, kernel
     * objects, file handles, guest threads (heap.c memstat_line). */
    int memstat_line(char *buf, size_t n);
    char ms[512]; int k = memstat_line(ms, sizeof ms);
    if (getenv("NFSU2_COMLIVE") && k > 0 && k < (int)sizeof ms){       /* test-only */
        int com_live_line(char *, size_t); k += com_live_line(ms + k, sizeof ms - (size_t)k); }
    if (k > 0 && k < (int)sizeof ms){                                   /* a texture pack in use */
        int texrep_status(char *, size_t); texrep_status(ms + k, sizeof ms - (size_t)k); }
#ifdef __wasm__
    /* per-frame guest busy time (p50/p95/p99/max) and the guest lock's queue
     * since the last line (runtime/web/gcpu.c, win32_ext.c) */
    if (k > 0) k = (int)strlen(ms);
    if (k > 0 && k < (int)sizeof ms){ int gcpu_status(char *, size_t); k += gcpu_status(ms + k, sizeof ms - (size_t)k); }
    if (k > 0 && k < (int)sizeof ms){ int win32_gil_status(char *, size_t); k += win32_gil_status(ms + k, sizeof ms - (size_t)k); }
    /* the frame pacer's spin on the clock per sleep since the last line (win32_ext.c sleep_precise_ns) */
    if (k > 0 && k < (int)sizeof ms){ int pacer_spin_status(char *, size_t); pacer_spin_status(ms + k, sizeof ms - (size_t)k); }
#endif
    fprintf(stderr, "[status]%s%s f=%ld t=%.1fs fps=%.1f cpu=%.0f%% gcpu=%.2fms/f flow=%u rss=%ldMB%s\n",
            label ? " " : "", label ? label : "", g_frame, tms / 1e3, dt > 0 ? nf / dt : 0.0,
            dt > 0 ? 100.0 * (cpu - g_st_cpu) / dt : 0.0,
            nf > 0 ? (gcpu - g_st_gcpu) / nf : 0.0,
            game_flow(),
            
#ifdef __wasm__
            (long)((uintptr_t)sbrk(0) >> 20), ms);   /* no ru_maxrss: the wasm memory's size (it only grows) */
#else
            (long)(ru.ru_maxrss / (1024 * 1024)), ms);
#endif
    g_st_t = t; g_st_f = g_frame; g_st_cpu = cpu; g_st_gcpu = gcpu;
}

void autotest_on_present(CPU *c){
    if (!g_inited) return;
#ifdef __wasm__
    { void gcpu_frame(void); gcpu_frame(); }        /* per-frame guest busy time (runtime/web/gcpu.c) */
    { void status_web_frame(void); status_web_frame(); } /* whether the player drives (runtime/web/status_web.c) */
    { void input_web_frame(void); input_web_frame(); }   /* the game's text entry, for a phone's keyboard (runtime/web/input_web.c) */
#endif
    g_frame++;
    if (g_game.on_test_frame) g_game.on_test_frame();   /* UG2: NFSU2_CAR_TRACE, the front end's dump (debug_ug2.c) */
    double t = now_ms();
    if (g_frame == 1){ g_t0 = t; g_st_t = t; g_st_cpu = proc_cpu_s(); g_st_gcpu = thread_cpu_ms(); }
    double tms = t - g_t0;

    if (g_fng_log){
        static char last[64];
        const char *t = autotest_top_fng();
        if (t && strcmp(t, last)){ snprintf(last, sizeof last, "%s", t);
            fprintf(stderr, "[fng] f=%ld top package %s (flow=%u)\n", g_frame, t, game_flow()); }
    }
    /* scheduled releases */
    for (int i = 0; i < g_nrel; ){
        if (g_frame >= g_rel[i].at){ key_event(g_rel[i].dik, 0); g_rel[i] = g_rel[--g_nrel]; }
        else i++;
    }
    for (int i = 0; i < g_nprel; ){
        if (g_frame >= g_prel[i].at){ gamepad_synth_button((int)g_prel[i].dik, 0); g_prel[i] = g_prel[--g_nprel]; }
        else i++;
    }
    /* ns* requests due this frame, in the order they were made */
    { int w = 0;
      for (int i = 0; i < g_nnsq; i++){
          if (g_nsq[i].at <= g_frame) ns_fire(&g_nsq[i]);
          else g_nsq[w++] = g_nsq[i];
      }
      g_nnsq = w; }
    /* typing: one character every 4 frames (down at k, up at k+2) */
    if (g_type_h != g_type_t && g_frame >= g_type_next){
        char ch = g_type_q[g_type_h]; g_type_h = (g_type_h + 1) & 255;
        unsigned dik = 0; char up[2] = { (char)toupper((unsigned char)ch), 0 };
        if (ch == ' ') dik = 0x39; else if (ch == '\n') dik = 0x1C; else parse_key(up, &dik);
        if (dik){ kbd_set_key(KSRC_SCRIPT, dik, 1);
                  if (g_nrel < 64){ g_rel[g_nrel].dik = dik; g_rel[g_nrel].at = g_frame + 2; g_nrel++; } }
        const KeyDef *k = dik ? key_by_dik(dik) : 0;
        uint32_t lp = 1u | ((uint32_t)(dik & 0x7Fu) << 16);
        if (k) win32_post_message(0x0100u, k->vk, lp);
        win32_post_message(0x0102u, (uint32_t)(unsigned char)ch, lp);
        g_type_next = g_frame + 4;
    }
    /* script */
    while (g_iev < g_nev){
        Ev *e = &g_ev[g_iev];
        int due; double wms = hfr_sim_ms();
        if (e->ms == 2) due = wms + 1e-6 >= (e->rel ? g_last_fire_wms : g_wzero) + e->t;
        else if (e->rel) due = e->ms ? (tms >= g_last_fire_ms + e->t) : (g_frame >= g_last_fire_f + e->t);
        else        due = e->ms ? (tms >= e->t) : (g_frame >= e->t);
        if (!due) break;
        if (!fire(c, e, tms)) break;
        g_last_fire_f = g_frame; g_last_fire_ms = tms; g_last_fire_wms = wms; g_iev++;
        if (g_iev == g_nev) fprintf(stderr, "[autotest] f=%ld input script finished\n", g_frame);
    }
    if (g_nedge) edges_tick();
    if (g_ntr && g_frame % g_tr_every == 0) trace_emit(g_frame, tms);
    if (g_status_every && g_frame % g_status_every == 0) status_line(t, tms, 0);
    if (g_quit_frames && g_frame >= g_quit_frames) do_quit("NFSU2_QUIT_FRAMES");
}
long autotest_frame(void){ return g_frame; }
/* career.c (test-only career bot) and roam.c (test-only): key taps through
 * the same path as "tap" (the current screen: autotest_screen() above) */
void autotest_tap(unsigned dik, long frames){
    key_event(dik, 1);
    if (g_nrel < 64){ g_rel[g_nrel].dik = dik; g_rel[g_nrel].at = g_frame + (frames > 0 ? frames : 1); g_nrel++; }
}
void autotest_quit(const char *why){ do_quit(why); }

/* Called first thing in main(): log redirection. */
void autotest_early(void){
    const char *lf = getenv("NFSU2_LOG");
    if (lf && *lf){
        int fd = open(lf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0){ dup2(fd, 1); dup2(fd, 2); close(fd); }
        setvbuf(stdout, 0, _IOLBF, 0); setvbuf(stderr, 0, _IONBF, 0);
    }
    const char *to = getenv("NFSU2_TIMEOUT");
    if (to && *to) setenv("ALARM", to, 1);
}
void autotest_init(void){
    const char *s = getenv("NFSU2_INPUT_SCRIPT");
    if (s && *s) load_script(s);
    const char *q = getenv("NFSU2_QUIT_FRAMES"); if (q) g_quit_frames = atol(q);
    const char *st = getenv("NFSU2_STATUS_EVERY"); if (st) g_status_every = atol(st);
    { const char *fl = getenv("NFSU2_FNG_LOG"); g_fng_log = fl ? (atoi(fl) ? atoi(fl) : 1) : 0; }
    g_inited = 1;
}

/* First-occurrence logging (win32.c / dispatch.c). */
void autotest_note_unimpl(const char *dll, const char *name, uint32_t ret){
    static char seen[512][48]; static int ns = 0;
    for (int i = 0; i < ns; i++) if (!strcmp(seen[i], name)) return;
    if (ns < 512) snprintf(seen[ns++], 48, "%s", name);
    fprintf(stderr, "[hle] UNIMPLEMENTED %s!%s (first call, from 0x%08x, frame %ld)\n", dll, name, ret, g_frame);
}
void autotest_note_trap(uint32_t addr, const char *mn){
    static uint32_t seen[1024]; static int ns = 0;
    for (int i = 0; i < ns; i++) if (seen[i] == addr) return;
    if (ns < 1024) seen[ns++] = addr;
    fprintf(stderr, "[trap] unsupported instruction '%s' at 0x%08x (first hit, frame %ld)\n", mn ? mn : "?", addr, g_frame);
}
