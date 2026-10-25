/* Input and app lifecycle for the browser: what keyboard_mac.m, gamepad.m
 * and the mouse and app-delegate parts of metal.m do on the Mac.
 *
 * The page (web/src/input) is the only producer of real input.  It writes
 * events into the HostMailbox (input_proto.h): keys already translated from
 * KeyboardEvent.code to the key's position (DIK scan code) and Windows
 * virtual-key code, with the characters the layout typed in code page 1252;
 * mouse positions in the shown image; focus, visibility and quit.  A
 * dedicated input thread sleeps on the event counter (the page notifies it)
 * and feeds the portable C exactly where the AppKit main thread did:
 *   - keys: kbd_set_key(KSRC_HOST) (DirectInput state and the event ring of
 *     keyboard.c) and kbd_post (WM_KEYDOWN/UP, WM_SYSKEYDOWN/UP, WM_CHAR,
 *     WM_DEADCHAR, WM_SYSCHAR), with keyboard_mac.m's rules for Alt, text
 *     typed with Option/AltGr, autorepeat and keys that lost their release;
 *   - mouse: the DIMOUSESTATE2 accumulators and the cursor that
 *     metal_fill_mouse / metal_get_cursor hand to com.c and GetCursorPos,
 *     plus WM_MOUSEMOVE, WM_xBUTTONDOWN/UP and WM_MOUSEWHEEL;
 *   - lifecycle: lifecycle_set_active (blur, hidden page) and
 *     lifecycle_request_quit (WM_CLOSE), as metal.m's app delegate.
 * The game pad is read straight from the mailbox (a seqlock the page fills
 * from navigator.getGamepads() every frame) by gamepad_read on the guest
 * thread; the autotest's synthetic pad works as in gamepad.m.
 *
 * The autotest ns* commands (kbdmac_synth_*) go the other way: requests in
 * a ring that the page turns into DOM events dispatched through its own
 * handlers, so a script takes the same path as a person at the keyboard.
 *
 * All of the keyboard state below belongs to the input thread. */
#include "cpu.h"
#include "game.h"
#include "kbd.h"
#include "gamepad.h"
#include "host_abi.h"
#include "input_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#define WM_KEYDOWN     0x0100u
#define WM_KEYUP       0x0101u
#define WM_CHAR        0x0102u
#define WM_DEADCHAR    0x0103u
#define WM_SYSKEYDOWN  0x0104u
#define WM_SYSKEYUP    0x0105u
#define WM_SYSCHAR     0x0106u
#define WM_SYSDEADCHAR 0x0107u
#define WM_MOUSEWHEEL  0x020Au

#define DIK_LMENU 0x38u
#define DIK_RMENU 0xB8u
#define DIK_CAPITAL 0x3Au

void lifecycle_set_active(int on, const char *why);              /* lifecycle.c */
void lifecycle_request_quit(const char *why);
int  lifecycle_quit_requested(void);
int  lifecycle_game_parked(void);                               /* inactive and not in a LAN session */
void lifecycle_tick(void);
int  lifecycle_drain_writes(double max_s);
void watchdog_log_download(void);                               /* watchdog.c */
int  win32_freeze_guest(int max_ms);                              /* win32_ext.c */
void metal_game_size(unsigned *w, unsigned *h);                   /* the renderer */
long metal_frames(void);
extern void (*g_report_fn)(void);

static HostMailbox g_mb __attribute__((aligned(64)));
#define MH(i) g_mb.h[i]

static double mono_s(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- script key names (keyboard_mac.m's table, by DIK) -------------------
 * The ns* commands name keys as the Mac port does; "kc:0xNN" is a Mac
 * virtual key code, so the Mac table is kept by key code. */
typedef struct { const char *name; uint8_t dik; } MacKey;
static const MacKey K[128] = {
    [0x00] = {"A", 0x1E}, [0x01] = {"S", 0x1F}, [0x02] = {"D", 0x20}, [0x03] = {"F", 0x21},
    [0x04] = {"H", 0x23}, [0x05] = {"G", 0x22}, [0x06] = {"Z", 0x2C}, [0x07] = {"X", 0x2D},
    [0x08] = {"C", 0x2E}, [0x09] = {"V", 0x2F}, [0x0A] = {"OEM_102", 0x56}, [0x0B] = {"B", 0x30},
    [0x0C] = {"Q", 0x10}, [0x0D] = {"W", 0x11}, [0x0E] = {"E", 0x12}, [0x0F] = {"R", 0x13},
    [0x10] = {"Y", 0x15}, [0x11] = {"T", 0x14}, [0x12] = {"1", 0x02}, [0x13] = {"2", 0x03},
    [0x14] = {"3", 0x04}, [0x15] = {"4", 0x05}, [0x16] = {"6", 0x07}, [0x17] = {"5", 0x06},
    [0x18] = {"EQUALS", 0x0D}, [0x19] = {"9", 0x0A}, [0x1A] = {"7", 0x08}, [0x1B] = {"MINUS", 0x0C},
    [0x1C] = {"8", 0x09}, [0x1D] = {"0", 0x0B}, [0x1E] = {"RBRACKET", 0x1B}, [0x1F] = {"O", 0x18},
    [0x20] = {"U", 0x16}, [0x21] = {"LBRACKET", 0x1A}, [0x22] = {"I", 0x17}, [0x23] = {"P", 0x19},
    [0x24] = {"RETURN", 0x1C}, [0x25] = {"L", 0x26}, [0x26] = {"J", 0x24}, [0x27] = {"APOSTROPHE", 0x28},
    [0x28] = {"K", 0x25}, [0x29] = {"SEMICOLON", 0x27}, [0x2A] = {"BACKSLASH", 0x2B}, [0x2B] = {"COMMA", 0x33},
    [0x2C] = {"SLASH", 0x35}, [0x2D] = {"N", 0x31}, [0x2E] = {"M", 0x32}, [0x2F] = {"PERIOD", 0x34},
    [0x30] = {"TAB", 0x0F}, [0x31] = {"SPACE", 0x39}, [0x32] = {"GRAVE", 0x29}, [0x33] = {"BACK", 0x0E},
    [0x35] = {"ESCAPE", 0x01},
    [0x36] = {"RCMD", 0xDC}, [0x37] = {"LCMD", 0xDB},       /* Meta: the browser's, never the game's */
    [0x38] = {"LSHIFT", 0x2A}, [0x39] = {"CAPSLOCK", 0x3A}, [0x3A] = {"LOPTION", 0x38},
    [0x3B] = {"LCONTROL", 0x1D}, [0x3C] = {"RSHIFT", 0x36}, [0x3D] = {"ROPTION", 0xB8},
    [0x3E] = {"RCONTROL", 0x9D},
    [0x41] = {"DECIMAL", 0x53}, [0x43] = {"MULTIPLY", 0x37}, [0x45] = {"ADD", 0x4E},
    [0x47] = {"CLEAR", 0x45}, [0x4B] = {"DIVIDE", 0xB5}, [0x4C] = {"NUMPADENTER", 0x9C},
    [0x4E] = {"SUBTRACT", 0x4A}, [0x51] = {"NUMPADEQUALS", 0x8D}, [0x52] = {"NUMPAD0", 0x52},
    [0x53] = {"NUMPAD1", 0x4F}, [0x54] = {"NUMPAD2", 0x50}, [0x55] = {"NUMPAD3", 0x51},
    [0x56] = {"NUMPAD4", 0x4B}, [0x57] = {"NUMPAD5", 0x4C}, [0x58] = {"NUMPAD6", 0x4D},
    [0x59] = {"NUMPAD7", 0x47}, [0x5B] = {"NUMPAD8", 0x48}, [0x5C] = {"NUMPAD9", 0x49},
    [0x5D] = {"YEN", 0x7D}, [0x5E] = {"RO", 0x73}, [0x5F] = {"NUMPADCOMMA", 0xB3},
    [0x60] = {"F5", 0x3F}, [0x61] = {"F6", 0x40}, [0x62] = {"F7", 0x41}, [0x63] = {"F3", 0x3D},
    [0x64] = {"F8", 0x42}, [0x65] = {"F9", 0x43}, [0x66] = {"EISU", 0x7B}, [0x67] = {"F11", 0x57},
    [0x68] = {"KANA", 0x70}, [0x69] = {"F13", 0x64}, [0x6B] = {"F14", 0x65}, [0x6D] = {"F10", 0x44},
    [0x6E] = {"APPS", 0xDD}, [0x6F] = {"F12", 0x58}, [0x71] = {"F15", 0x66}, [0x72] = {"INSERT", 0xD2},
    [0x73] = {"HOME", 0xC7}, [0x74] = {"PRIOR", 0xC9}, [0x75] = {"DELETE", 0xD3}, [0x76] = {"F4", 0x3E},
    [0x77] = {"END", 0xCF}, [0x78] = {"F2", 0x3C}, [0x79] = {"NEXT", 0xD1}, [0x7A] = {"F1", 0x3B},
    [0x7B] = {"LEFT", 0xCB}, [0x7C] = {"RIGHT", 0xCD}, [0x7D] = {"DOWN", 0xD0}, [0x7E] = {"UP", 0xC8},
};
static const struct { const char *alias, *name; } k_alias[] = {
    {"ENTER", "RETURN"}, {"ESC", "ESCAPE"}, {"BACKSPACE", "BACK"}, {"SHIFT", "LSHIFT"},
    {"CTRL", "LCONTROL"}, {"CONTROL", "LCONTROL"}, {"ALT", "LOPTION"}, {"OPTION", "LOPTION"},
    {"LMENU", "LOPTION"}, {"RMENU", "ROPTION"}, {"LALT", "LOPTION"}, {"RALT", "ROPTION"},
    {"CMD", "LCMD"}, {"COMMAND", "LCMD"}, {"META", "LCMD"}, {"CAPITAL", "CAPSLOCK"}, {"CAPS", "CAPSLOCK"},
    {"PGUP", "PRIOR"}, {"PAGEUP", "PRIOR"}, {"PGDN", "NEXT"}, {"PAGEDOWN", "NEXT"},
    {"HELP", "INSERT"}, {"FORWARDDELETE", "DELETE"}, {"NUMLOCK", "CLEAR"}, {"SECTION", "OEM_102"},
};
/* a script key name -> DIK, 0 if unknown */
static unsigned lookup_key(const char *nm){
    if (!strncasecmp(nm, "kc:", 3)){
        unsigned v = (unsigned)strtoul(nm + 3, 0, 0);
        return v < 128 && K[v].name ? K[v].dik : 0;
    }
    if (!strncasecmp(nm, "DIK_", 4)) nm += 4;
    for (unsigned i = 0; i < sizeof k_alias / sizeof *k_alias; i++)
        if (!strcasecmp(nm, k_alias[i].alias)){ nm = k_alias[i].name; break; }
    for (int kc = 0; kc < 128; kc++) if (K[kc].name && !strcasecmp(K[kc].name, nm)) return K[kc].dik;
    return 0;
}

/* ---- keys: keyboard_mac.m's delivery rules ------------------------------- */
/* alttext: pressed with Option/AltGr and typed text, so its release is a
 * WM_KEYUP even while Alt is still down, as with AltGr on Windows */
typedef struct { uint8_t down, vk, sys, alttext; } Down;
static Down g_down[256];             /* by DIK: keys the game has seen go down */
static int  g_alt_other;             /* a key was pressed while Alt was down */
static int  g_nheld;

static uint32_t key_lparam(unsigned dik, int alt, int prev, int up){
    return 1u | ((uint32_t)(dik & 0x7Fu) << 16) | ((dik & 0x80u) ? (1u << 24) : 0u)
         | (alt ? (1u << 29) : 0u) | (prev ? (1u << 30) : 0u) | (up ? (1u << 31) : 0u);
}
static void post(uint32_t m, uint32_t wp, uint32_t lp){
    kbd_post(m, wp, lp);
    if (kbd_log()) fprintf(stderr, "[key]   -> %s 0x%02x lParam 0x%08x\n", kbd_msg_name(m), wp, lp);
}
static int is_alt(unsigned dik){ return dik == DIK_LMENU || dik == DIK_RMENU; }
static void press(unsigned dik, unsigned vk, int sys, int repeat){
    if (!repeat){
        if (!g_down[dik].down) g_nheld++;
        g_down[dik].down = 1; g_down[dik].vk = (uint8_t)vk; g_down[dik].sys = (uint8_t)sys;
        g_down[dik].alttext = 0;
        kbd_set_key(KSRC_HOST, dik, 1);
    }
    if (vk) post(sys ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, key_lparam(dik, sys, repeat, 0));
}
static void release(unsigned dik, int alt){
    Down *d = &g_down[dik];
    if (!d->down) return;
    d->down = 0; g_nheld--;
    kbd_set_key(KSRC_HOST, dik, 0);
    /* Alt alone: WM_SYSKEYUP; a key released while Alt is down too, unless
     * it was pressed as text (WM_KEYDOWN, so WM_KEYUP) */
    int sys = is_alt(dik) ? !g_alt_other : (alt && !d->alttext);
    if (d->vk) post(sys ? WM_SYSKEYUP : WM_KEYUP, d->vk, key_lparam(dik, sys, 1, 1));
}
static const char *reason_name(int r){
    switch (r){
    case IREASON_WINDOW: return "window"; case IREASON_VISIBILITY: return "page visibility";
    case IREASON_FULLSCREEN: return "fullscreen change"; case IREASON_META: return "Meta key";
    case IREASON_PAGEHIDE: return "page hide"; case IREASON_SCRIPT: return "autotest nsfocus";
    case IREASON_ELEMENT: return "game element"; case IREASON_PAGE: return "page";
    default: return "page";
    }
}
static void mouse_release_all(void);
/* every key the game holds goes up (WM_KEYUP and DirectInput), and with
 * mouse != 0 the buttons too: a focus loss takes both, as the Mac's resign
 * handlers did; a Meta press only the keys */
static void release_all(const char *why, int mouse){
    int n = 0;
    for (unsigned k = 1; k < 256u; k++) if (g_down[k].down){ release(k, 0); n++; }
    kbd_release_source(KSRC_HOST);
    g_alt_other = 0; g_nheld = 0;
    if (mouse) mouse_release_all();
    if (n || kbd_log()) fprintf(stderr, "[key] %s: %d held key(s) released\n", why, n);
}

static int is_modifier(unsigned dik){
    return dik == 0x2A || dik == 0x36 || dik == 0x1D || dik == 0x9D || is_alt(dik);
}
static void key_down(const InEv *e){
    unsigned dik = e->dik;
    if (!dik || dik == 0xDB || dik == 0xDC) return;            /* Meta: the browser's */
    int repeat = (e->flags & IKF_REPEAT) != 0;
    if (repeat && !g_down[dik].down) return;    /* pressed with Meta, or released by a focus loss */
    if (!repeat && g_down[dik].down) repeat = 1;                  /* its key up was lost */
    int alt = (e->flags & IKF_ALT) != 0;
    if (kbd_log())
        fprintf(stderr, "[key] host %s dik=0x%02x vk=0x%02x flags 0x%02x chars %d%s%s\n",
                repeat ? "repeat " : "keyDown", dik, e->vk, e->flags, e->nch,
                (e->flags & IKF_DEAD) ? " (dead key)" : "", (e->flags & IKF_SYNTH) ? " [synthetic]" : "");
    if (is_modifier(dik)){
        /* modifiers: as keyboard_mac.m's flags_changed */
        if (!repeat){
            if (is_alt(dik)) g_alt_other = 0;
            else if (g_down[DIK_LMENU].down || g_down[DIK_RMENU].down) g_alt_other = 1;
        }
        press(dik, e->vk, is_alt(dik) || alt, repeat);
        return;
    }
    int dead = (e->flags & IKF_DEAD) ? (e->nch ? e->ch[0] : '?') : 0;
    int nch = (e->flags & IKF_DEAD) ? 0 : e->nch;
    /* Alt+key that types something (a character or a dead key) is text,
     * like AltGr on Windows; otherwise it is Alt+key: WM_SYSKEYDOWN */
    int opt = alt && !(e->flags & IKF_CTRL);
    int text = dead != 0;
    for (int i = 0; i < nch; i++) if (e->ch[i] >= 0x20 && e->ch[i] != '?') text = 1;
    int sys = opt && !text;
    if (sys) nch = 0;
    if (g_down[DIK_LMENU].down || g_down[DIK_RMENU].down) g_alt_other = 1;
    press(dik, e->vk, sys, repeat);
    if (!repeat) g_down[dik].alttext = (uint8_t)(opt && text);
    if (dead) post(sys ? WM_SYSDEADCHAR : WM_DEADCHAR, (uint32_t)dead, key_lparam(dik, sys, repeat, 0));
    for (int i = 0; i < nch; i++) post(WM_CHAR, e->ch[i], key_lparam(dik, 0, repeat, 0));
    if (sys && e->b >= 0x20 && e->b != 0x7F)                     /* Alt+key: the key's plain character */
        post(WM_SYSCHAR, (uint32_t)e->b, key_lparam(dik, 1, repeat, 0));
}
static void key_up(const InEv *e){
    unsigned dik = e->dik;
    if (!dik || dik > 255u) return;
    if (kbd_log() && g_down[dik].down) fprintf(stderr, "[key] host keyUp   dik=0x%02x\n", dik);
    release(dik, (e->flags & IKF_ALT) != 0);
}

/* ---- mouse (metal.m) ------------------------------------------------------- */
static _Atomic int g_mouse_x = 640, g_mouse_y = 360;   /* the pointer now (the WM_ messages, the page) */
static _Atomic int g_mouse_dz = 0;
static _Atomic unsigned g_mouse_btn = 0;
static uint64_t mono_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }

/* What the game sees of the mouse: one change a frame.  The game reads the
 * mouse once a frame (GetDeviceState, GetCursorPos), never its events, so two
 * changes inside one frame are one to it: a click whose press and release
 * fall between two frames never happened, and a press in the frame of a move
 * picks what the menu highlighted before the move.  A tap on a phone is a
 * move, a press and a release within about 100 ms, which one slow frame takes
 * whole: the cursor moved, nothing was picked.  So the input thread queues
 * the changes and the game takes them a frame at a time: the moves up to the
 * next button change, or that button change.  Moving alone comes as soon as
 * before; a press right after a move comes a frame later.  Changes older than
 * a second (the game did not read the mouse, as in a race) are taken at once,
 * not played back one a frame.
 * The menus look for what is under the cursor only in a frame it moved, in
 * their 640x480 units (sub_005bf750 from GetCursorPos; if (dx || dy) in
 * sub_004ed140, sub_0055d410 ...), and a dialog opened by the last tap
 * starts on its default button: a tap where the cursor already was (Yes,
 * then Yes on the next question) pressed nothing.  So a finger landing
 * within two of those units of the cursor first puts it two units aside for
 * a frame of its own (MS_HOP), and the cursor comes back to the finger with
 * a move the menu sees. */
enum { MS_MOVE, MS_BTN, MS_HOP };
typedef struct { int kind, x, y, dx, dy; unsigned btn; uint64_t us; } MouseStep;
#define MOUSE_STEPS 64
static MouseStep g_ms[MOUSE_STEPS];
static int g_ms_head, g_ms_n;
static int g_view_x = 640, g_view_y = 360, g_view_dx, g_view_dy;   /* what the game has taken */
static unsigned g_view_btn;
static long g_view_frame = -1;
static pthread_mutex_t g_ms_mx = PTHREAD_MUTEX_INITIALIZER;
static void step_take(void){                  /* g_ms_mx held */
    const MouseStep *s = &g_ms[g_ms_head];
    if (s->kind != MS_BTN){ g_view_x = s->x; g_view_y = s->y; g_view_dx += s->dx; g_view_dy += s->dy; }
    else g_view_btn = s->btn;
    g_ms_head = (g_ms_head + 1) % MOUSE_STEPS; g_ms_n--;
}
static void mouse_step(MouseStep s){          /* the input thread, the autotest's click */
    s.us = mono_us();
    pthread_mutex_lock(&g_ms_mx);
    MouseStep *last = g_ms_n ? &g_ms[(g_ms_head + g_ms_n - 1) % MOUSE_STEPS] : 0;
    if (s.kind == MS_MOVE && last && last->kind == MS_MOVE){
        last->x = s.x; last->y = s.y; last->dx += s.dx; last->dy += s.dy;
    } else {
        if (g_ms_n == MOUSE_STEPS) step_take();
        g_ms[(g_ms_head + g_ms_n) % MOUSE_STEPS] = s; g_ms_n++;
    }
    pthread_mutex_unlock(&g_ms_mx);
}
/* the game reads the mouse (guest thread): the next change, once a frame */
static void mouse_take(void){
    long f = metal_frames();
    uint64_t now = mono_us();
    pthread_mutex_lock(&g_ms_mx);
    while (g_ms_n && now - g_ms[g_ms_head].us > 1000000u) step_take();
    if (f != g_view_frame){
        g_view_frame = f;
        if (g_ms_n && g_ms[g_ms_head].kind != MS_MOVE) step_take();
        else while (g_ms_n && g_ms[g_ms_head].kind == MS_MOVE) step_take();
    }
    pthread_mutex_unlock(&g_ms_mx);
}
/* the HUD is narrowed on a wider screen (x) and lowered on a narrower one
 * (y), widescreen.c; the pointer maps into that area */
static _Atomic int g_ui_asp = 10000, g_ui_asp_y = 10000;
void metal_set_ui_aspect(float ax, float ay){
    if (!(ax > 0.05f && ax <= 1.0f)) ax = 1.0f;
    if (!(ay > 0.05f && ay <= 1.0f)) ay = 1.0f;
    atomic_store(&g_ui_asp, (int)lroundf(ax * 10000.f));
    atomic_store(&g_ui_asp_y, (int)lroundf(ay * 10000.f));
}
void metal_mouse_add(int dx, int dy){
    pthread_mutex_lock(&g_ms_mx); g_view_dx += dx; g_view_dy += dy; pthread_mutex_unlock(&g_ms_mx);
}
void metal_fill_mouse(unsigned char *dst, unsigned n){
    if (n < 20) return;
    mouse_take();
    pthread_mutex_lock(&g_ms_mx);
    int dx = g_view_dx, dy = g_view_dy;
    unsigned b = g_view_btn;
    g_view_dx = g_view_dy = 0;
    pthread_mutex_unlock(&g_ms_mx);
    int dz = atomic_exchange(&g_mouse_dz, 0);
    memcpy(dst + 0, &dx, 4);
    memcpy(dst + 4, &dy, 4);
    memcpy(dst + 8, &dz, 4);
    for (int i = 0; i < 8; i++) dst[12 + i] = (b >> i) & 1u ? 0x80 : 0x00;
}
void metal_get_cursor(int *x, int *y){
    /* MOUSEAT=<x>,<y>[@<frame>,<x>,<y>]: pin the cursor in game space (test) */
    { const char *e = getenv("MOUSEAT");
      if (e && *e){ int ax = 0, ay = 0, f2 = 0, bx = 0, by = 0;
          int n2 = sscanf(e, "%d,%d@%d,%d,%d", &ax, &ay, &f2, &bx, &by);
          if (n2 == 5 && metal_frames() >= f2){ ax = bx; ay = by; }
          if (x) *x = ax; if (y) *y = ay; return; } }
    mouse_take();
    pthread_mutex_lock(&g_ms_mx);
    if (x) *x = g_view_x;
    if (y) *y = g_view_y;
    pthread_mutex_unlock(&g_ms_mx);
}
static uint32_t mouse_mk(int flags){
    unsigned b = atomic_load(&g_mouse_btn);
    return ((b & 1u) ? 0x01u : 0u) | ((b & 2u) ? 0x02u : 0u) | ((b & 4u) ? 0x10u : 0u)
         | ((b & 8u) ? 0x20u : 0u) | ((b & 16u) ? 0x40u : 0u)
         | ((flags & IKF_SHIFT) ? 0x04u : 0u) | ((flags & IKF_CTRL) ? 0x08u : 0u);
}
static uint32_t mouse_lp(void){
    return ((uint32_t)(atomic_load(&g_mouse_y) & 0xFFFF) << 16) | (uint32_t)(atomic_load(&g_mouse_x) & 0xFFFF);
}
static void mouse_button(int b, int down, int flags){
    if (b < 0 || b > 7) return;
    unsigned bit = 1u << b, was = atomic_load(&g_mouse_btn) & bit;
    if (down) atomic_fetch_or(&g_mouse_btn, bit); else atomic_fetch_and(&g_mouse_btn, ~bit);
    if (!!was == !!down) return;
    mouse_step((MouseStep){ .kind = MS_BTN, .btn = atomic_load(&g_mouse_btn) });
    static const uint32_t dn[3] = {0x0201u, 0x0204u, 0x0207u};     /* WM_L/R/MBUTTONDOWN */
    if (b < 3) kbd_post(down ? dn[b] : dn[b] + 1u, mouse_mk(flags), mouse_lp());
}
static void mouse_release_all(void){
    for (int b = 0; b < 8; b++) mouse_button(b, 0, 0);
    atomic_store(&g_mouse_btn, 0u);
    /* a focus loss: the game lets go at once, no click still to come is played */
    pthread_mutex_lock(&g_ms_mx);
    g_ms_n = 0; g_view_btn = 0; g_view_x = atomic_load(&g_mouse_x); g_view_y = atomic_load(&g_mouse_y);
    pthread_mutex_unlock(&g_ms_mx);
}
/* position in the shown image (1/65536 of its size) -> the game's client
 * area, narrowed to the HUD's area as metal.m's mouse_from_point does; the
 * relative motion DirectInput reports is the same move in game pixels */
static double g_last_nx = -1, g_last_ny = -1, g_frac_dx, g_frac_dy;
static void mouse_move(const InEv *e){
    unsigned lw = 1280, lh = 720; metal_game_size(&lw, &lh);
    if (!lw || !lh){ lw = 1280; lh = 720; }
    double ax = atomic_load(&g_ui_asp) / 10000.0, ay = atomic_load(&g_ui_asp_y) / 10000.0;
    double nx = ((double)e->a / 65536.0 - (1.0 - ax) * 0.5) / ax;
    double ny = ((double)e->b / 65536.0 - (1.0 - ay) * 0.5) / ay;
    int dx = 0, dy = 0;
    if (g_last_nx >= -0.5){
        g_frac_dx += (nx - g_last_nx) * lw; g_frac_dy += (ny - g_last_ny) * lh;
        dx = (int)g_frac_dx; dy = (int)g_frac_dy;
        g_frac_dx -= dx; g_frac_dy -= dy;
    }
    g_last_nx = nx; g_last_ny = ny;
    double x = nx * lw, y = ny * lh;
    if (x < 0.0) x = 0.0; else if (x > lw - 1.0) x = lw - 1.0;
    if (y < 0.0) y = 0.0; else if (y > lh - 1.0) y = lh - 1.0;
    if (e->flags & IKF_TOUCH){
        double ux = lw / 640.0, uy = lh / 480.0;
        if (fabs(x - atomic_load(&g_mouse_x)) < 2.0 * ux && fabs(y - atomic_load(&g_mouse_y)) < 2.0 * uy){
            int h = (int)ceil(2.0 * ux), hx = (int)x + h <= (int)lw - 1 ? (int)x + h : (int)x - h;
            mouse_step((MouseStep){ .kind = MS_HOP, .x = hx, .y = (int)y });
        }
    }
    mouse_step((MouseStep){ .kind = MS_MOVE, .x = (int)x, .y = (int)y, .dx = dx, .dy = dy });
    atomic_store(&g_mouse_x, (int)x); atomic_store(&g_mouse_y, (int)y);
    atomic_store(&MH(IM_MOUSE_X), (uint32_t)(int)x); atomic_store(&MH(IM_MOUSE_Y), (uint32_t)(int)y);
    kbd_post_mousemove(mouse_mk(e->flags), mouse_lp());
}
/* autotest "click x y" (metal.m's metal_request_click): a left click at that
 * fraction of the shown image, from the guest thread at a Present (the LAN
 * scripts click Host LAN Server and Select Car).  The button goes up about
 * 8 frames later, from the input thread, as the Mac's posted click does. */
static _Atomic uint64_t g_click_up_us;
void metal_request_click(double x, double y){
    InEv e; memset(&e, 0, sizeof e);
    e.a = (int32_t)lround(x * 65536.0); e.b = (int32_t)lround(y * 65536.0);
    mouse_move(&e);
    mouse_button(0, 1, 0);
    atomic_store(&g_click_up_us, mono_us() + 140000u);
    fprintf(stderr, "app: script click frame %ld at %.3f,%.3f -> game %d,%d\n", metal_frames(), x, y,
            atomic_load(&g_mouse_x), atomic_load(&g_mouse_y));
}
static void click_release_due(void){
    uint64_t t = atomic_load(&g_click_up_us);
    if (t && mono_us() >= t && atomic_compare_exchange_strong(&g_click_up_us, &t, 0)) mouse_button(0, 0, 0);
}
static void mouse_wheel(const InEv *e){
    int dz = e->a;
    if (!dz) return;
    atomic_fetch_add(&g_mouse_dz, dz);
    kbd_post(WM_MOUSEWHEEL, ((uint32_t)(dz & 0xFFFF) << 16) | mouse_mk(e->flags), mouse_lp());
}

/* ---- lifecycle -------------------------------------------------------------- */
static double g_quit_t0 = 0;
static int    g_quit_events = 0, g_forcing = 0;
static double quit_grace_s(void){
    const char *e = getenv("NFSU2_QUIT_GRACE");
    double g = (e && *e) ? atof(e) : 10.0;
    return g > 0 ? g : 10.0;
}
/* lifecycle.c ends the Mac app's event loop when the game ignores WM_CLOSE
 * (a second request, or the grace period); here that means ending the
 * process the way main() does after the event loop.  At most 5 s for open
 * save files plus 2 s to freeze the guest: the page offers "Stop the game"
 * only after that (web/src/app/shell.ts FORCED_CLOSE_MAX_MS, STOP_AFTER_MS). */
static void force_exit(const char *why){
    if (g_forcing) return;
    g_forcing = 1;
    fprintf(stderr, "app: forcing the exit (%s)\n", why);
    lifecycle_drain_writes(5.0);
    if (!win32_freeze_guest(2000)) fprintf(stderr, "app: a guest thread still holds the guest lock\n");
    if (g_report_fn) g_report_fn();
    fflush(stdout); fflush(stderr);
    _exit(0);
}
static void lifecycle_poll(void){
    if (!lifecycle_quit_requested()) return;
    double t = mono_s();
    if (!g_quit_t0) g_quit_t0 = t;
    atomic_store(&MH(IM_QUIT_REQ), 1u);
    lifecycle_tick();                                   /* its log line */
    if (t - g_quit_t0 > quit_grace_s() + 0.5) force_exit("the game did not exit after WM_CLOSE");
}

/* ---- the input thread --------------------------------------------------------- */
/* a multiplayer race's trip drives the game by itself (mprace.c): the
 * player's keys, mouse and pad are not passed on meanwhile (releases and
 * focus still are: nothing stays held) */
int mprace_owns_input(void);
static int g_muted;
static void handle(const InEv *e){
    int mute = mprace_owns_input();
    if (mute != g_muted){
        g_muted = mute;
        if (mute) release_all("a multiplayer race's trip drives the game", 1);
        fprintf(stderr, "[key] the player's input %s\n", mute ? "muted: the game drives itself to the multiplayer race" : "back");
    }
    if (mute && (e->type == IEV_KEY_DOWN || e->type == IEV_KEY_TAP || e->type == IEV_TEXT || e->type == IEV_MOUSE_MOVE || e->type == IEV_MOUSE_DOWN || e->type == IEV_WHEEL)) return;
    switch (e->type){
    case IEV_KEY_DOWN: key_down(e); break;
    case IEV_KEY_UP:   key_up(e); break;
    case IEV_TEXT:                                       /* a phone's keyboard: characters, no key */
        if (kbd_log()) fprintf(stderr, "[key] host text, %d char(s)\n", e->nch);
        for (int i = 0; i < e->nch && i < 3; i++) post(WM_CHAR, e->ch[i], 1u);
        break;
    case IEV_KEY_TAP: {                                  /* Caps Lock on macOS */
        InEv u = *e; u.type = IEV_KEY_UP; u.flags &= (uint8_t)~IKF_REPEAT;
        InEv d = u; d.type = IEV_KEY_DOWN;
        if (kbd_log()) fprintf(stderr, "[key] host Caps Lock toggled\n");
        key_down(&d); key_up(&u); break; }
    case IEV_MOUSE_MOVE: mouse_move(e); break;
    case IEV_MOUSE_DOWN: mouse_button(e->a, 1, e->flags); break;
    case IEV_MOUSE_UP:   mouse_button(e->a, 0, e->flags); break;
    case IEV_WHEEL:      mouse_wheel(e); break;
    case IEV_FOCUS: {
        char why[48]; snprintf(why, sizeof why, "%s %s", reason_name(e->b), e->a ? "active" : "inactive");
        if (!e->a) release_all(why, 1);
        if (e->b == IREASON_SCRIPT) fprintf(stderr, "[key] simulated %s\n", e->a ? "focus gain" : "focus loss");
        lifecycle_set_active(e->a ? 1 : 0, why);
        break; }
    case IEV_RELEASE_ALL: release_all(reason_name(e->b), e->b != IREASON_META); break;
    case IEV_QUIT:
        g_quit_events++;
        lifecycle_request_quit(e->b == IREASON_SCRIPT ? "autotest" : "page quit request");
        if (g_quit_events >= 2) force_exit("second quit request");
        break;
    case IEV_LOG: watchdog_log_download(); break;
    case IEV_SMS: if (g_game.sms_request) g_game.sms_request(e->a); break;
    case IEV_PHOTO: if (g_game.photo_input) g_game.photo_input(e->dik, e->a, e->b); break;
    default: break;
    }
}
static void *input_main(void *unused){
    (void)unused;
    uint32_t r = atomic_load(&MH(IM_EV_R));
    for (;;){
        uint32_t w = atomic_load(&MH(IM_EV_W));
        if (w - r > INPUT_EV_N){                         /* the page overran us: never, it checks */
            fprintf(stderr, "[input] event ring overrun: %u events lost\n", w - r - INPUT_EV_N);
            r = w - INPUT_EV_N;
        }
        while (r != w){
            InEv e = g_mb.ev[r & (INPUT_EV_N - 1u)];
            r++;
            atomic_store(&MH(IM_EV_R), r);
            handle(&e);
            atomic_fetch_add(&MH(IM_EVENTS), 1u);
        }
        atomic_store(&MH(IM_KEYS_HELD), (uint32_t)g_nheld);
        atomic_store(&MH(IM_GAME_INACTIVE), (uint32_t)lifecycle_game_parked());
        lifecycle_poll();
        /* the page notifies IM_EV_W after each batch; the timeout drives the
         * quit grace period */
        __builtin_wasm_memory_atomic_wait32((int32_t *)&MH(IM_EV_W), (int32_t)w,
                                            atomic_load(&g_click_up_us) ? 20000000ll : 100000000ll);
        click_release_due();
    }
    return 0;
}

/* ---- the game's text entry, for the page ------------------------------------------ */
/* A phone has no keys: while the game's text entry is up (a new profile's
 * name, the LAN name), the page shows a field of its own that brings up the
 * phone's keyboard, starts it with the game's text and sends what is typed
 * as IEV_TEXT (web/src/app/textentry.ts).  The entry is the game's to read
 * (game.h text_entry; UG2's object at 0x83A7B0, game_ug2.c).  Published at
 * every Present (autotest.c) when it changed, under IM_TEXT_SEQ. */
void input_web_frame(void){
    static uint32_t l_max, l_kind, l_len;
    static uint8_t l_txt[INPUT_TEXT_BYTES];
    uint32_t kind = 0, len = 0;
    uint8_t txt[INPUT_TEXT_BYTES] = { 0 };
    uint32_t max = g_game.text_entry ? g_game.text_entry(&kind, &len, txt, sizeof txt) : 0u;
    if (max == l_max && kind == l_kind && len == l_len && !memcmp(txt, l_txt, sizeof txt)) return;
    if (!max != !l_max) fprintf(stderr, "[input] the game's text entry %s\n", max ? "is up" : "is down");
    l_max = max; l_kind = kind; l_len = len; memcpy(l_txt, txt, sizeof txt);
    uint32_t s = atomic_load(&MH(IM_TEXT_SEQ));
    atomic_store(&MH(IM_TEXT_SEQ), s + 1u);
    atomic_store(&MH(IM_TEXT_MAX), max); atomic_store(&MH(IM_TEXT_KIND), kind); atomic_store(&MH(IM_TEXT_LEN), len);
    for (unsigned i = 0; i < INPUT_TEXT_BYTES / 4u; i++)
        atomic_store(&MH(IM_TEXT + i), (uint32_t)txt[4*i] | (uint32_t)txt[4*i+1] << 8 | (uint32_t)txt[4*i+2] << 16 | (uint32_t)txt[4*i+3] << 24);
    atomic_store(&MH(IM_TEXT_SEQ), s + 2u);
}

void input_web_init(void){
    static int done; if (done) return; done = 1;
    MH(IM_MAGIC) = INPUT_MAGIC; MH(IM_VERSION) = INPUT_VERSION;
    MH(IM_SIZE) = (uint32_t)sizeof g_mb; MH(IM_EV_N) = INPUT_EV_N;
    atomic_store(&MH(IM_MOUSE_X), 640u); atomic_store(&MH(IM_MOUSE_Y), 360u);
    nfsu2_register(HOST_KIND_INPUT, &g_mb, (uint32_t)sizeof g_mb);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256u * 1024u);
    pthread_t th;
    if (pthread_create(&th, &at, input_main, 0) != 0){
        fprintf(stderr, "[input] the input thread could not start: no keyboard, mouse or focus input\n");
        return;
    }
    pthread_detach(th);
    fprintf(stderr, "[input] HostMailbox at 0x%08x (%u bytes), input thread started\n",
            (unsigned)(uintptr_t)&g_mb, (unsigned)sizeof g_mb);
}

/* ---- keyboard_mac.m's other entry points -------------------------------------- */
/* the key's name on the page's keyboard layout (navigator.keyboard
 * .getLayoutMap(), written into the mailbox by the page), or 0: dikeys.c */
const char *kbdmac_layout_name(unsigned dik){
    if (dik > 255u || !atomic_load(&MH(IM_NAMES_GEN)) || !g_mb.names[dik][0]) return 0;
    return g_mb.names[dik];
}
/* ---- the autotest's ns* commands without a page --------------------------
 * Normally the page turns each request into DOM events (web/src/input/
 * synth.ts) that its own handlers (index.ts) translate into mailbox events,
 * so a script takes a person's path.  A page without the input layer (the
 * headless test page hostcheck.html, which the suite runner uses) never
 * reads the request ring, and the scripts' keys would be lost.  Then the
 * guest thread does both halves itself, as keyboard_mac.m does on the Mac
 * (NSEvents into AppKit's queue, then the same delivery code): the same
 * steps as synth.ts + index.ts for a US layout with macOS conventions (Caps
 * Lock one event per toggle, Caps or Shift give capitals; Option+key is a
 * command when e.key is the key's own character or its capital, as for the
 * page's synthetic events, else text: Option+Shift+1 types '!'), written
 * into the event ring the page would have written, which the input thread
 * then handles exactly like the page's events (web/test/input-chars.test.mjs
 * compares every key and modifier combination with the page's path).
 * Who writes the event ring is settled at the first request: IM_ATTACHED 0
 * -> 2 (compare-exchange; the page attaches with 0 -> 1 and backs off if
 * it lost), so the ring keeps a single writer. */
enum { KK_NONE, KK_US, KK_NUM };     /* no characters / US plain+shifted / numpad */
typedef struct { uint8_t vk, kind, plain, shifted; } DikKey;
/* web/src/input/keymap.ts: the first KeyboardEvent.code of each DIK (its
 * virtual-key code) and that key's US characters */
static const DikKey DK[256] = {
    [0x01] = {0x1B, KK_NONE, 0, 0},   /* Escape */
    [0x02] = {0x31, KK_US, '1', '!'},   /* Digit1 */
    [0x03] = {0x32, KK_US, '2', '@'},   /* Digit2 */
    [0x04] = {0x33, KK_US, '3', '#'},   /* Digit3 */
    [0x05] = {0x34, KK_US, '4', '$'},   /* Digit4 */
    [0x06] = {0x35, KK_US, '5', '%'},   /* Digit5 */
    [0x07] = {0x36, KK_US, '6', '^'},   /* Digit6 */
    [0x08] = {0x37, KK_US, '7', '&'},   /* Digit7 */
    [0x09] = {0x38, KK_US, '8', '*'},   /* Digit8 */
    [0x0A] = {0x39, KK_US, '9', '('},   /* Digit9 */
    [0x0B] = {0x30, KK_US, '0', ')'},   /* Digit0 */
    [0x0C] = {0xBD, KK_US, '-', '_'},   /* Minus */
    [0x0D] = {0xBB, KK_US, '=', '+'},   /* Equal */
    [0x0E] = {0x08, KK_NONE, 0, 0},   /* Backspace */
    [0x0F] = {0x09, KK_NONE, 0, 0},   /* Tab */
    [0x10] = {0x51, KK_US, 'q', 'Q'},   /* KeyQ */
    [0x11] = {0x57, KK_US, 'w', 'W'},   /* KeyW */
    [0x12] = {0x45, KK_US, 'e', 'E'},   /* KeyE */
    [0x13] = {0x52, KK_US, 'r', 'R'},   /* KeyR */
    [0x14] = {0x54, KK_US, 't', 'T'},   /* KeyT */
    [0x15] = {0x59, KK_US, 'y', 'Y'},   /* KeyY */
    [0x16] = {0x55, KK_US, 'u', 'U'},   /* KeyU */
    [0x17] = {0x49, KK_US, 'i', 'I'},   /* KeyI */
    [0x18] = {0x4F, KK_US, 'o', 'O'},   /* KeyO */
    [0x19] = {0x50, KK_US, 'p', 'P'},   /* KeyP */
    [0x1A] = {0xDB, KK_US, '[', '{'},   /* BracketLeft */
    [0x1B] = {0xDD, KK_US, ']', '}'},   /* BracketRight */
    [0x1C] = {0x0D, KK_NONE, 0, 0},   /* Enter */
    [0x1D] = {0x11, KK_NONE, 0, 0},   /* ControlLeft */
    [0x1E] = {0x41, KK_US, 'a', 'A'},   /* KeyA */
    [0x1F] = {0x53, KK_US, 's', 'S'},   /* KeyS */
    [0x20] = {0x44, KK_US, 'd', 'D'},   /* KeyD */
    [0x21] = {0x46, KK_US, 'f', 'F'},   /* KeyF */
    [0x22] = {0x47, KK_US, 'g', 'G'},   /* KeyG */
    [0x23] = {0x48, KK_US, 'h', 'H'},   /* KeyH */
    [0x24] = {0x4A, KK_US, 'j', 'J'},   /* KeyJ */
    [0x25] = {0x4B, KK_US, 'k', 'K'},   /* KeyK */
    [0x26] = {0x4C, KK_US, 'l', 'L'},   /* KeyL */
    [0x27] = {0xBA, KK_US, ';', ':'},   /* Semicolon */
    [0x28] = {0xDE, KK_US, '\'', '"'},   /* Quote */
    [0x29] = {0xC0, KK_US, '`', '~'},   /* Backquote */
    [0x2A] = {0x10, KK_NONE, 0, 0},   /* ShiftLeft */
    [0x2B] = {0xDC, KK_US, '\\', '|'},   /* Backslash */
    [0x2C] = {0x5A, KK_US, 'z', 'Z'},   /* KeyZ */
    [0x2D] = {0x58, KK_US, 'x', 'X'},   /* KeyX */
    [0x2E] = {0x43, KK_US, 'c', 'C'},   /* KeyC */
    [0x2F] = {0x56, KK_US, 'v', 'V'},   /* KeyV */
    [0x30] = {0x42, KK_US, 'b', 'B'},   /* KeyB */
    [0x31] = {0x4E, KK_US, 'n', 'N'},   /* KeyN */
    [0x32] = {0x4D, KK_US, 'm', 'M'},   /* KeyM */
    [0x33] = {0xBC, KK_US, ',', '<'},   /* Comma */
    [0x34] = {0xBE, KK_US, '.', '>'},   /* Period */
    [0x35] = {0xBF, KK_US, '/', '?'},   /* Slash */
    [0x36] = {0x10, KK_NONE, 0, 0},   /* ShiftRight */
    [0x37] = {0x6A, KK_NUM, '*', '*'},   /* NumpadMultiply */
    [0x38] = {0x12, KK_NONE, 0, 0},   /* AltLeft */
    [0x39] = {0x20, KK_US, ' ', ' '},   /* Space */
    [0x3A] = {0x14, KK_NONE, 0, 0},   /* CapsLock */
    [0x3B] = {0x70, KK_NONE, 0, 0},   /* F1 */
    [0x3C] = {0x71, KK_NONE, 0, 0},   /* F2 */
    [0x3D] = {0x72, KK_NONE, 0, 0},   /* F3 */
    [0x3E] = {0x73, KK_NONE, 0, 0},   /* F4 */
    [0x3F] = {0x74, KK_NONE, 0, 0},   /* F5 */
    [0x40] = {0x75, KK_NONE, 0, 0},   /* F6 */
    [0x41] = {0x76, KK_NONE, 0, 0},   /* F7 */
    [0x42] = {0x77, KK_NONE, 0, 0},   /* F8 */
    [0x43] = {0x78, KK_NONE, 0, 0},   /* F9 */
    [0x44] = {0x79, KK_NONE, 0, 0},   /* F10 */
    [0x45] = {0x90, KK_NONE, 0, 0},   /* NumLock */
    [0x46] = {0x91, KK_NONE, 0, 0},   /* ScrollLock */
    [0x47] = {0x67, KK_NUM, '7', '7'},   /* Numpad7 */
    [0x48] = {0x68, KK_NUM, '8', '8'},   /* Numpad8 */
    [0x49] = {0x69, KK_NUM, '9', '9'},   /* Numpad9 */
    [0x4A] = {0x6D, KK_NUM, '-', '-'},   /* NumpadSubtract */
    [0x4B] = {0x64, KK_NUM, '4', '4'},   /* Numpad4 */
    [0x4C] = {0x65, KK_NUM, '5', '5'},   /* Numpad5 */
    [0x4D] = {0x66, KK_NUM, '6', '6'},   /* Numpad6 */
    [0x4E] = {0x6B, KK_NUM, '+', '+'},   /* NumpadAdd */
    [0x4F] = {0x61, KK_NUM, '1', '1'},   /* Numpad1 */
    [0x50] = {0x62, KK_NUM, '2', '2'},   /* Numpad2 */
    [0x51] = {0x63, KK_NUM, '3', '3'},   /* Numpad3 */
    [0x52] = {0x60, KK_NUM, '0', '0'},   /* Numpad0 */
    [0x53] = {0x6E, KK_NUM, '.', '.'},   /* NumpadDecimal */
    [0x56] = {0xE2, KK_US, 0xA7, 0xB1},   /* IntlBackslash */
    [0x57] = {0x7A, KK_NONE, 0, 0},   /* F11 */
    [0x58] = {0x7B, KK_NONE, 0, 0},   /* F12 */
    [0x64] = {0x7C, KK_NONE, 0, 0},   /* F13 */
    [0x65] = {0x7D, KK_NONE, 0, 0},   /* F14 */
    [0x66] = {0x7E, KK_NONE, 0, 0},   /* F15 */
    [0x70] = {0x15, KK_NONE, 0, 0},   /* KanaMode */
    [0x73] = {0xE2, KK_NONE, 0, 0},   /* IntlRo */
    [0x7B] = {0x1D, KK_NONE, 0, 0},   /* Lang2 */
    [0x7D] = {0xDC, KK_NONE, 0, 0},   /* IntlYen */
    [0x8D] = {0x92, KK_NUM, '=', '='},   /* NumpadEqual */
    [0x9C] = {0x0D, KK_NONE, 0, 0},   /* NumpadEnter */
    [0x9D] = {0x11, KK_NONE, 0, 0},   /* ControlRight */
    [0xB3] = {0x6C, KK_NUM, ',', ','},   /* NumpadComma */
    [0xB5] = {0x6F, KK_NUM, '/', '/'},   /* NumpadDivide */
    [0xB7] = {0x2C, KK_NONE, 0, 0},   /* PrintScreen */
    [0xB8] = {0x12, KK_NONE, 0, 0},   /* AltRight */
    [0xC5] = {0x13, KK_NONE, 0, 0},   /* Pause */
    [0xC7] = {0x24, KK_NONE, 0, 0},   /* Home */
    [0xC8] = {0x26, KK_NONE, 0, 0},   /* ArrowUp */
    [0xC9] = {0x21, KK_NONE, 0, 0},   /* PageUp */
    [0xCB] = {0x25, KK_NONE, 0, 0},   /* ArrowLeft */
    [0xCD] = {0x27, KK_NONE, 0, 0},   /* ArrowRight */
    [0xCF] = {0x23, KK_NONE, 0, 0},   /* End */
    [0xD0] = {0x28, KK_NONE, 0, 0},   /* ArrowDown */
    [0xD1] = {0x22, KK_NONE, 0, 0},   /* PageDown */
    [0xD2] = {0x2D, KK_NONE, 0, 0},   /* Insert */
    [0xD3] = {0x2E, KK_NONE, 0, 0},   /* Delete */
    [0xDB] = {0x5B, KK_NONE, 0, 0},   /* MetaLeft */
    [0xDC] = {0x5C, KK_NONE, 0, 0},   /* MetaRight */
    [0xDD] = {0x5D, KK_NONE, 0, 0},   /* ContextMenu */
};
static pthread_mutex_t g_cs_mx = PTHREAD_MUTEX_INITIALIZER;   /* the C driver's state (guest threads) */
static int  cs_owner;                /* 0 undecided, 1 page, 2 this driver */
static uint8_t cs_held[256];         /* modifiers held by synthetic events, by DIK */
static int  cs_caps;
static double cs_mx = 0.5, cs_my = 0.5, cs_wheel;
static unsigned cs_buttons;          /* DOM buttons mask the page would have */

static int synth_owner(void){
    if (cs_owner) return cs_owner;
    uint32_t a = 0;
    if (atomic_compare_exchange_strong(&MH(IM_ATTACHED), &a, 2u)){
        cs_owner = 2;
        fprintf(stderr, "[input] no page input layer: the script's ns* input is translated here "
                        "(US layout, macOS conventions) into the event ring\n");
    } else cs_owner = a == 2u ? 2 : 1;
    return cs_owner;
}
/* index.ts push(): one record, published by the store of IM_EV_W */
static void cs_push(int type, unsigned dik, unsigned vk, unsigned flags, const uint8_t *ch, int nch, int32_t a, int32_t b){
    uint32_t w = atomic_load(&MH(IM_EV_W)), r = atomic_load(&MH(IM_EV_R));
    if (w - r >= INPUT_EV_N){ atomic_fetch_add(&MH(IM_EV_DROP), 1u); return; }
    InEv *e = &g_mb.ev[w & (INPUT_EV_N - 1u)];
    memset(e, 0, sizeof *e);
    e->type = (uint8_t)type; e->dik = (uint8_t)dik; e->vk = (uint8_t)vk; e->flags = (uint8_t)flags;
    e->nch = (uint8_t)(nch > 3 ? 3 : nch);
    for (int i = 0; i < e->nch; i++) e->ch[i] = ch[i];
    e->a = a; e->b = b;
    atomic_store(&MH(IM_EV_W), w + 1u);
    __builtin_wasm_memory_atomic_notify((int32_t *)&MH(IM_EV_W), 1u);
}
static int cs_shift(void){ return cs_held[0x2A] || cs_held[0x36]; }
static int cs_ctrl(void){ return cs_held[0x1D] || cs_held[0x9D]; }
static int cs_alt(void){ return cs_held[DIK_LMENU] || cs_held[DIK_RMENU]; }
static int cs_meta(void){ return cs_held[0xDB] || cs_held[0xDC]; }
static unsigned cs_flags(void){
    return (cs_alt() ? IKF_ALT : 0u) | (cs_ctrl() ? IKF_CTRL : 0u) | (cs_shift() ? IKF_SHIFT : 0u) | IKF_SYNTH;
}
static int is_meta_dik(unsigned dik){ return dik == 0xDB || dik == 0xDC; }
/* keymap.ts keyFor(): the character e.key would hold, 0 for a named key */
static unsigned cs_keychar(unsigned dik){
    const DikKey *k = &DK[dik];
    if (k->kind == KK_NUM) return k->plain;
    if (k->kind != KK_US) return 0;
    if (k->plain >= 'a' && k->plain <= 'z') return (cs_shift() || cs_caps) ? k->plain - 0x20u : k->plain;
    return cs_shift() ? k->shifted : k->plain;
}
/* index.ts onKeyDown / onKeyUp for one synthetic KeyboardEvent */
static void cs_key(int down, unsigned dik, int repeat){
    const DikKey *k = &DK[dik];
    if (is_meta_dik(dik)){
        if (down && !repeat) cs_push(IEV_RELEASE_ALL, 0, 0, 0, 0, 0, 0, IREASON_META);
        return;
    }
    if (!k->vk) return;
    if (down && cs_meta()) return;                         /* the browser's shortcut */
    unsigned fl = cs_flags();
    if (dik == DIK_CAPITAL){ cs_push(IEV_KEY_TAP, dik, k->vk, fl, 0, 0, 0, 0); return; }   /* macOS */
    if (!down){ cs_push(IEV_KEY_UP, dik, k->vk, fl, 0, 0, 0, 0); return; }
    /* charsOf(): WM_CHAR bytes, and the plain character for WM_SYSCHAR */
    uint8_t ch[1]; int nch = 0; unsigned sys = 0;
    switch (dik){
    case 0x1C: case 0x9C: ch[0] = 0x0D; nch = 1; break;    /* Enter, NumpadEnter */
    case 0x0E: ch[0] = 0x08; nch = 1; break;               /* Backspace */
    case 0x01: ch[0] = 0x1B; nch = 1; break;               /* Escape */
    case 0x0F: ch[0] = 0x09; nch = 1; break;               /* Tab */
    default: {
        unsigned c = cs_keychar(dik);
        if (!c) break;
        unsigned plain = k->plain;                         /* baseChar(): the US plain character */
        sys = plain;
        if (cs_ctrl() && !cs_alt()){                        /* Ctrl+letter: a control character */
            unsigned u = (c >= 'a' && c <= 'z') ? c - 0x20u : c;
            if (u >= 'A' && u <= 'Z'){ ch[0] = (uint8_t)(u - 0x40u); nch = 1; }
            sys = 0;
        } else if (cs_alt() && !cs_ctrl() &&
                   (c == plain || c == ((plain >= 'a' && plain <= 'z') ? plain - 0x20u : plain))){
            /* index.ts charsOf on macOS: Option+key is a command when e.key
             * is the key's own character or its capital (synthetic events
             * carry no Option characters); Option+Shift+1 ('!') is text */
        } else { ch[0] = (uint8_t)c; nch = 1; }
    } }
    cs_push(IEV_KEY_DOWN, dik, k->vk, fl | (repeat ? IKF_REPEAT : 0u), ch, nch, 0, (int32_t)sys);
}
/* synth.ts modifier() */
static void cs_modifier(unsigned dik, int down){
    if (dik == DIK_CAPITAL){                               /* macOS: one event per toggle */
        if (!down) return;
        cs_caps = !cs_caps; cs_key(cs_caps, dik, 0);
        return;
    }
    cs_held[dik] = (uint8_t)(down != 0);
    cs_key(down, dik, 0);
}
static int is_mod_dik(unsigned dik){
    return dik == 0x2A || dik == 0x36 || dik == 0x1D || dik == 0x9D || is_alt(dik) || is_meta_dik(dik) || dik == DIK_CAPITAL;
}
static void cs_press(unsigned dik, int type){
    if (is_mod_dik(dik)){ if (type != KSYN_REPEAT) cs_modifier(dik, type != KSYN_UP); return; }
    cs_key(type != KSYN_UP, dik, type == KSYN_REPEAT);
}
/* synth.ts strokeFor() on the US layout: the key and Shift typing u */
static unsigned cs_stroke(uint32_t u, int *shift){
    static const uint8_t order[] = {   /* keymap.ts US: Object.keys order */
        0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x27,
        0x28, 0x33, 0x34, 0x35, 0x39, 0x56,
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10,
        0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    *shift = 0;
    switch (u){
    case '\r': case '\n': return 0x1C;
    case '\b': return 0x0E;
    case '\t': return 0x0F;
    case 0x1B: return 0x01;
    default: break;
    }
    for (unsigned i = 0; i < sizeof order; i++) if (DK[order[i]].plain == u) return order[i];
    for (unsigned i = 0; i < sizeof order; i++){
        unsigned pl = DK[order[i]].plain, sh = (pl >= 'a' && pl <= 'z') ? pl - 0x20u : DK[order[i]].shifted;
        if (sh == u){ *shift = 1; return order[i]; }
    }
    return 0;
}
/* the DOM pointer position the page would compute (index.ts pos()) */
static void cs_move(void){
    cs_push(IEV_MOUSE_MOVE, 0, 0, cs_flags(), 0, 0, (int32_t)lround(cs_mx * 65536.0), (int32_t)lround(cs_my * 65536.0));
}
static unsigned g_cs_dibtn;          /* index.ts `buttons`: what the page last sent */
static void cs_sync_buttons(void){
    unsigned now = cs_buttons & 31u;
    for (int i = 0; i < 5; i++){
        unsigned bit = 1u << i;
        if ((now & bit) != (g_cs_dibtn & bit))
            cs_push((now & bit) ? IEV_MOUSE_DOWN : IEV_MOUSE_UP, 0, 0, cs_flags(), 0, 0, i, 0);
    }
    g_cs_dibtn = now;
}
/* synth.ts handle() */
static void cs_handle(const SynReq *q){
    static const struct { uint8_t bit, dik; } MODS[] = {
        {ISYN_META, 0xDB}, {ISYN_SHIFT, 0x2A}, {ISYN_CTRL, 0x1D}, {ISYN_ALT, DIK_LMENU} };
    switch (q->type){
    case KSYN_DOWN: case KSYN_UP: case KSYN_REPEAT: {
        if (!DK[q->dik].vk){ fprintf(stderr, "[key] synthetic key: no DOM code for DIK 0x%x\n", q->dik); return; }
        if (q->type == KSYN_DOWN)
            for (unsigned i = 0; i < 4; i++) if (q->mods & MODS[i].bit) cs_modifier(MODS[i].dik, 1);
        cs_press(q->dik, q->type);
        if (q->type == KSYN_UP)
            for (int i = 3; i >= 0; i--) if (q->mods & MODS[i].bit) cs_modifier(MODS[i].dik, 0);
        return; }
    case KSYN_CHAR_DOWN: case KSYN_CHAR_UP: {
        int sh; unsigned dik = cs_stroke(q->ch, &sh);
        if (!dik) return;                                   /* refused on the guest thread already */
        if (q->type == KSYN_CHAR_DOWN){ if (sh) cs_modifier(0x2A, 1); cs_key(1, dik, 0); }
        else { cs_key(0, dik, 0); if (sh) cs_modifier(0x2A, 0); }
        return; }
    case KSYN_FOCUS_OUT: case KSYN_FOCUS_IN:
        cs_push(IEV_FOCUS, 0, 0, 0, 0, 0, q->type == KSYN_FOCUS_IN, IREASON_SCRIPT);
        return;
    case KSYN_MOUSE_MOVE:
        cs_mx = q->a; cs_my = q->b;
        cs_move();                                          /* onPointerMove */
        if ((cs_buttons & 31u) != g_cs_dibtn) cs_sync_buttons();
        return;
    case KSYN_MOUSE_DOWN: case KSYN_MOUSE_UP: {
        int b = (int)q->a; unsigned bit = b == 1 ? 2u : b == 2 ? 4u : 1u;
        if (q->type == KSYN_MOUSE_DOWN){ cs_buttons |= bit; cs_move(); cs_sync_buttons(); }   /* onPointerDown */
        else { cs_buttons &= ~bit; cs_sync_buttons(); }                                      /* onPointerUp */
        return; }
    case KSYN_SCROLL: {                                     /* onWheel: deltaY = -lines, line mode */
        cs_wheel += (double)q->a * 120.0;
        int dz = (int)trunc(cs_wheel);
        cs_wheel -= dz;
        if (dz) cs_push(IEV_WHEEL, 0, 0, cs_flags(), 0, 0, dz, 0);
        return; }
    default: return;
    }
}

static int syn_push(const SynReq *q){
    pthread_mutex_lock(&g_cs_mx);
    if (synth_owner() == 2){ cs_handle(q); pthread_mutex_unlock(&g_cs_mx); return 1; }
    pthread_mutex_unlock(&g_cs_mx);
    uint32_t w = atomic_load(&MH(IM_SYN_W)), r = atomic_load(&MH(IM_SYN_R));
    if (w - r >= INPUT_SYN_N){ atomic_fetch_add(&MH(IM_SYN_DROP), 1u);
        fprintf(stderr, "[input] synthetic input ring full: request dropped\n"); return 0; }
    g_mb.syn[w & (INPUT_SYN_N - 1u)] = *q;
    atomic_store(&MH(IM_SYN_W), w + 1u);
    return 1;
}
/* "cmd+shift+Q": modifier prefixes and a key name; 0 if the key is unknown
 * (validated here, on the guest thread, so the script logs it) */
int kbdmac_synth_key(int type, const char *key){
    char tmp[40]; snprintf(tmp, sizeof tmp, "%s", key);
    SynReq q; memset(&q, 0, sizeof q); q.type = (uint8_t)type;
    char *p = tmp, *plus;
    while ((plus = strchr(p, '+')) && plus[1]){
        *plus = 0;
        if (!strcasecmp(p, "cmd") || !strcasecmp(p, "meta")) q.mods |= ISYN_META;
        else if (!strcasecmp(p, "shift")) q.mods |= ISYN_SHIFT;
        else if (!strcasecmp(p, "ctrl")) q.mods |= ISYN_CTRL;
        else if (!strcasecmp(p, "opt") || !strcasecmp(p, "alt")) q.mods |= ISYN_ALT;
        p = plus + 1;
    }
    unsigned dik = lookup_key(p);
    if (!dik) return 0;
    q.dik = (uint8_t)dik;
    syn_push(&q);
    return 1;
}
int kbdmac_synth_char(int type, uint32_t u){
    SynReq q; memset(&q, 0, sizeof q); q.type = (uint8_t)type; q.ch = u;
    pthread_mutex_lock(&g_cs_mx);
    int mine = synth_owner() == 2;
    pthread_mutex_unlock(&g_cs_mx);
    int sh;
    if (mine && !cs_stroke(u, &sh)) return 0;              /* the autotest logs it */
    syn_push(&q);
    return 1;           /* the page logs a character no key types */
}
void kbdmac_synth_simple(int type, double a, double b){
    SynReq q; memset(&q, 0, sizeof q); q.type = (uint8_t)type; q.a = (float)a; q.b = (float)b;
    syn_push(&q);
}

/* ---- game pad (gamepad.m) ------------------------------------------------------- */
static pthread_mutex_t g_pad_mx = PTHREAD_MUTEX_INITIALIZER;
static PadState g_syn;               /* the input script's controller */
static PadState g_real_last;         /* the last consistent read of the page's pad */

static float envf(const char *nm, float dflt, float lo, float hi){
    const char *e = getenv(nm);
    if (!e || !*e) return dflt;
    float v = (float)atof(e);
    return v < lo ? lo : (v > hi ? hi : v);
}
int gamepad_env_flag(const char *nm, int dflt){
    const char *e = getenv(nm);
    if (!e || !*e) return dflt;
    if ((*e >= '0' && *e <= '9') || *e == '-' || *e == '+') return atoi(e) != 0;
    return strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
}
void gamepad_init(void){
    static _Atomic int once = 0;
    if (atomic_exchange(&once, 1)) return;
    fprintf(stderr, "[pad] Gamepad API: the page polls navigator.getGamepads() every frame (standard mapping)\n");
}
static const char *family_name(int f){
    return f == PADF_XBOX ? "Xbox" : f == PADF_PLAYSTATION ? "PlayStation" : f == PADF_NINTENDO ? "Nintendo" : "generic";
}
/* the page's pad: a seqlock, retried while the page is writing */
static void read_real(PadState *s){
    for (int tries = 0; tries < 64; tries++){
        uint32_t s1 = atomic_load_explicit(&MH(IM_PAD_SEQ), memory_order_acquire);
        if (s1 & 1u) continue;
        PadState t; memset(&t, 0, sizeof t);
        t.connected = (int)atomic_load_explicit(&MH(IM_PAD_CONNECTED), memory_order_relaxed);
        t.family = (int)atomic_load_explicit(&MH(IM_PAD_FAMILY), memory_order_relaxed);
        t.buttons = atomic_load_explicit(&MH(IM_PAD_BUTTONS), memory_order_relaxed);
        t.wheel = atomic_load_explicit(&MH(IM_PAD_WHEEL), memory_order_relaxed) == 1u;
        for (int i = 0; i < PAD_NAXES; i++){
            uint32_t v = atomic_load_explicit(&MH(IM_PAD_AXIS + i), memory_order_relaxed);
            memcpy(&t.axis[i], &v, 4);
            if (!isfinite(t.axis[i])) t.axis[i] = 0.f;
        }
        memcpy(t.name, g_mb.pad_name, sizeof t.name);
        t.name[sizeof t.name - 1] = 0;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&MH(IM_PAD_SEQ), memory_order_relaxed) == s1){ g_real_last = t; break; }
    }
    *s = g_real_last;
    static uint32_t gen_seen = 0;
    uint32_t gen = atomic_load(&MH(IM_PAD_GEN));
    if (gen != gen_seen){
        gen_seen = gen;
        fprintf(stderr, "[pad] player 1 controller: %s (%s%s)\n", s->connected ? s->name : "none",
                s->connected ? family_name(s->family) : "-", s->connected && s->wheel ? ", a steering wheel" : "");
    }
}
static void stick_dz(float *x, float *y, float dz){
    float r = sqrtf(*x * *x + *y * *y);
    if (r <= dz || r <= 0.f){ *x = *y = 0.f; return; }
    float k = (r >= 1.f ? 1.f : (r - dz) / (1.f - dz)) / r;
    *x *= k; *y *= k;
    if (*x > 1.f) *x = 1.f; if (*x < -1.f) *x = -1.f;
    if (*y > 1.f) *y = 1.f; if (*y < -1.f) *y = -1.f;
}
static float trig_dz(float v, float dz){
    if (v <= dz) return 0.f;
    return v >= 1.f ? 1.f : (v - dz) / (1.f - dz);
}
int gamepad_read(PadState *s){
    static float dz = -1.f, tdz = -1.f;
    if (dz < 0.f){ dz = envf("NFSU2_PAD_DEADZONE", 0.08f, 0.f, 0.9f);
                   tdz = envf("NFSU2_PAD_TRIGGER_DEADZONE", 0.02f, 0.f, 0.9f); }
    pthread_mutex_lock(&g_pad_mx);
    int syn = g_syn.connected;
    if (syn) *s = g_syn;                         /* the script's pad wins while connected */
    pthread_mutex_unlock(&g_pad_mx);
    if (!syn) read_real(s);
    if (!syn && mprace_owns_input()){ int con = s->connected, wh = s->wheel; memset(s, 0, sizeof *s); s->connected = con; s->wheel = wh; }   /* (handle above) */
    /* a wheel's position is the steering, with no dead zone around the middle (its pedals keep the triggers') */
    if (!s->wheel) stick_dz(&s->axis[PAD_AXIS_LX], &s->axis[PAD_AXIS_LY], dz);
    stick_dz(&s->axis[PAD_AXIS_RX], &s->axis[PAD_AXIS_RY], dz);
    s->axis[PAD_AXIS_LT] = trig_dz(s->axis[PAD_AXIS_LT], tdz);
    s->axis[PAD_AXIS_RT] = trig_dz(s->axis[PAD_AXIS_RT], tdz);
    return s->connected;
}
/* rumble: the page drives the controller's motors from these words
 * (web/src/input/gamepad.ts); the autotest's synthetic pad "has" motors so
 * scripts can exercise force feedback */
int gamepad_rumble_ok(void){
    if (g_syn.connected) return 1;
    return atomic_load_explicit(&MH(IM_PAD_RUMBLE), memory_order_relaxed) == 1u;
}
void gamepad_rumble(float strong, float weak){
    uint32_t s = (uint32_t)(strong <= 0.f ? 0.f : strong >= 1.f ? 65535.f : strong * 65535.f + 0.5f);
    uint32_t w = (uint32_t)(weak <= 0.f ? 0.f : weak >= 1.f ? 65535.f : weak * 65535.f + 0.5f);
    atomic_store_explicit(&MH(IM_RUMBLE_STRONG), s, memory_order_relaxed);
    atomic_store_explicit(&MH(IM_RUMBLE_WEAK), w, memory_order_relaxed);
    atomic_fetch_add_explicit(&MH(IM_RUMBLE_SEQ), 1u, memory_order_release);
}
/* the phone's tilt the 3D view turns against (web/src/app/touch.ts: the
 * level horizon), radians, + = the phone turned clockwise; read once a
 * frame, so the main view and its road reflection (overrides.c
 * sub_005c7ec0) turn alike */
float input_view_roll(void){
    static long frame = -1; static float roll = 0.f;
    long f = metal_frames();
    if (f == frame) return roll;
    frame = f;
    uint32_t v = atomic_load_explicit(&MH(IM_VIEW_ROLL), memory_order_relaxed);
    memcpy(&roll, &v, 4);
    if (!isfinite(roll)) roll = 0.f;
    if (roll > 1.5f) roll = 1.5f; else if (roll < -1.5f) roll = -1.5f;
    return roll;
}
/* the menus' help bar buttons' size (web/src/app/touch.ts: the Controls
 * sheet's Menu button size; runtime/menusize.c), 1 = the game's own */
float input_menu_scale(void){
    uint32_t v = atomic_load_explicit(&MH(IM_MENU_SCALE), memory_order_relaxed);
    float s; memcpy(&s, &v, 4);
    return isfinite(s) && s >= 1.f ? s : 1.f;
}
/* on: 0 unplugged, 1 a pad, 2 a steering wheel */
void gamepad_synth_connect(int on){
    pthread_mutex_lock(&g_pad_mx);
    if ((on != 0) != g_syn.connected || (on == 2) != g_syn.wheel){
        memset(&g_syn, 0, sizeof g_syn);
        g_syn.connected = on ? 1 : 0;
        g_syn.synthetic = 1;
        g_syn.wheel = on == 2;
        g_syn.family = PADF_XBOX;
        snprintf(g_syn.name, sizeof g_syn.name, on == 2 ? "Virtual Wheel (autotest)" : "Virtual Controller (autotest)");
    }
    pthread_mutex_unlock(&g_pad_mx);
}
void gamepad_synth_button(int b, int down){
    if (b < 0 || b >= PAD_NBUTTONS) return;
    if (!g_syn.connected) gamepad_synth_connect(1);
    pthread_mutex_lock(&g_pad_mx);
    if (down) g_syn.buttons |= 1u << b; else g_syn.buttons &= ~(1u << b);
    pthread_mutex_unlock(&g_pad_mx);
}
void gamepad_synth_axis(int a, float v){
    if (a < 0 || a >= PAD_NAXES) return;
    if (!g_syn.connected) gamepad_synth_connect(1);
    float lo = a >= PAD_AXIS_LT ? 0.f : -1.f;
    pthread_mutex_lock(&g_pad_mx);
    g_syn.axis[a] = v < lo ? lo : (v > 1.f ? 1.f : v);
    pthread_mutex_unlock(&g_pad_mx);
}
int gamepad_button_by_name(const char *nm){
    static const struct { const char *nm; int b; } T[] = {
        {"A",PAD_A},{"CROSS",PAD_A},{"B",PAD_B},{"CIRCLE",PAD_B},{"X",PAD_X},{"SQUARE",PAD_X},
        {"Y",PAD_Y},{"TRIANGLE",PAD_Y},{"LB",PAD_LB},{"L1",PAD_LB},{"RB",PAD_RB},{"R1",PAD_RB},
        {"BACK",PAD_BACK},{"VIEW",PAD_BACK},{"SELECT",PAD_BACK},{"SHARE",PAD_BACK},{"CREATE",PAD_BACK},
        {"START",PAD_START},{"MENU",PAD_START},{"OPTIONS",PAD_START},
        {"LS",PAD_LS},{"L3",PAD_LS},{"RS",PAD_RS},{"R3",PAD_RS},{"HOME",PAD_HOME},{"GUIDE",PAD_HOME},
        {"DUP",PAD_DUP},{"DDOWN",PAD_DDOWN},{"DLEFT",PAD_DLEFT},{"DRIGHT",PAD_DRIGHT},
    };
    for (unsigned i = 0; i < sizeof T / sizeof *T; i++) if (!strcasecmp(nm, T[i].nm)) return T[i].b;
    return -1;
}
int gamepad_axis_by_name(const char *nm){
    static const char *const T[PAD_NAXES] = {"LX","LY","RX","RY","LT","RT"};
    for (int i = 0; i < PAD_NAXES; i++) if (!strcasecmp(nm, T[i])) return i;
    if (!strcasecmp(nm, "L2")) return PAD_AXIS_LT;
    if (!strcasecmp(nm, "R2")) return PAD_AXIS_RT;
    return -1;
}
