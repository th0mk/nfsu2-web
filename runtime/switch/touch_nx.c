/* touch_nx.c -- the console's touch screen as the PC game's mouse (the browser page's touch: web/src/input/index.ts, and the
 * mouse the game sees: runtime/web/input_web.c, which this file stands in for on the console).
 *   one finger   the mouse: a tap is a left click on the menu's button, a drag moves the cursor
 *   two fingers  the middle button held with the cursor between them: the garage's way to turn the car
 * The game reads the mouse once a frame (GetDeviceState, GetCursorPos), never its events, so the changes queue here and the game
 * takes them a frame at a time (the moves up to the next button change, or that button change); a finger landing at the cursor's
 * own place first puts it two menu units aside for a frame, because the menus look under the cursor only in a frame it moved.
 * touch_nx_poll runs in the app's loop (60 Hz); metal_fill_mouse and metal_get_cursor are the game thread's, and replace the weak
 * empty ones of null_platform.c. */
#include <switch.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "kbd.h"

long metal_frames(void);
void metal_game_size(unsigned *w, unsigned *h);

enum { MS_MOVE, MS_BTN, MS_HOP };
typedef struct { int kind, x, y, dx, dy; unsigned btn; uint64_t us; } MouseStep;
#define MOUSE_STEPS 64
static MouseStep g_ms[MOUSE_STEPS];
static int g_ms_head, g_ms_n;
static int g_view_x = 640, g_view_y = 360, g_view_dx, g_view_dy;      /* what the game has taken */
static unsigned g_view_btn;
static long g_view_frame = -1;
static pthread_mutex_t g_ms_mx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_mouse_x = 640, g_mouse_y = 360;                   /* the pointer now (the WM_ messages) */
static _Atomic unsigned g_mouse_btn;
static _Atomic int g_ui_asp = 10000, g_ui_asp_y = 10000;               /* the HUD's area on a wider screen (widescreen.c) */
static unsigned g_taps, g_drags, g_turns;

static uint64_t mono_us(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }

static void step_take(void){                                           /* g_ms_mx held */
    const MouseStep *s = &g_ms[g_ms_head];
    if (s->kind != MS_BTN){ g_view_x = s->x; g_view_y = s->y; g_view_dx += s->dx; g_view_dy += s->dy; }
    else g_view_btn = s->btn;
    g_ms_head = (g_ms_head + 1) % MOUSE_STEPS; g_ms_n--;
}
static void mouse_step(MouseStep s){
    s.us = mono_us();
    pthread_mutex_lock(&g_ms_mx);
    MouseStep *last = g_ms_n ? &g_ms[(g_ms_head + g_ms_n - 1) % MOUSE_STEPS] : 0;
    if (s.kind == MS_MOVE && last && last->kind == MS_MOVE){ last->x = s.x; last->y = s.y; last->dx += s.dx; last->dy += s.dy; }
    else {
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
    while (g_ms_n && now - g_ms[g_ms_head].us > 1000000u) step_take();      /* older than a second: not played back one a frame */
    if (f != g_view_frame){
        g_view_frame = f;
        if (g_ms_n && g_ms[g_ms_head].kind != MS_MOVE) step_take();
        else while (g_ms_n && g_ms[g_ms_head].kind == MS_MOVE) step_take();
    }
    pthread_mutex_unlock(&g_ms_mx);
}

void metal_set_ui_aspect(float ax, float ay){
    if (!(ax > 0.05f && ax <= 1.0f)) ax = 1.0f;
    if (!(ay > 0.05f && ay <= 1.0f)) ay = 1.0f;
    atomic_store(&g_ui_asp, (int)lroundf(ax * 10000.f));
    atomic_store(&g_ui_asp_y, (int)lroundf(ay * 10000.f));
}
void metal_mouse_add(int dx, int dy){ pthread_mutex_lock(&g_ms_mx); g_view_dx += dx; g_view_dy += dy; pthread_mutex_unlock(&g_ms_mx); }
void metal_fill_mouse(unsigned char *dst, unsigned n){
    if (n < 20) return;
    mouse_take();
    pthread_mutex_lock(&g_ms_mx);
    int dx = g_view_dx, dy = g_view_dy; unsigned b = g_view_btn;
    g_view_dx = g_view_dy = 0;
    pthread_mutex_unlock(&g_ms_mx);
    int dz = 0;
    memcpy(dst + 0, &dx, 4); memcpy(dst + 4, &dy, 4); memcpy(dst + 8, &dz, 4);
    for (int i = 0; i < 8; i++) dst[12 + i] = (b >> i) & 1u ? 0x80 : 0x00;
}
void metal_get_cursor(int *x, int *y){
    mouse_take();
    pthread_mutex_lock(&g_ms_mx);
    if (x) *x = g_view_x;
    if (y) *y = g_view_y;
    pthread_mutex_unlock(&g_ms_mx);
}

static uint32_t mouse_mk(void){
    unsigned b = atomic_load(&g_mouse_btn);
    return ((b & 1u) ? 0x01u : 0u) | ((b & 2u) ? 0x02u : 0u) | ((b & 4u) ? 0x10u : 0u);
}
static uint32_t mouse_lp(void){ return ((uint32_t)(atomic_load(&g_mouse_y) & 0xFFFF) << 16) | (uint32_t)(atomic_load(&g_mouse_x) & 0xFFFF); }
static void mouse_button(int b, int down){
    if (b < 0 || b > 2) return;
    unsigned bit = 1u << b, was = atomic_load(&g_mouse_btn) & bit;
    if (down) atomic_fetch_or(&g_mouse_btn, bit); else atomic_fetch_and(&g_mouse_btn, ~bit);
    if (!!was == !!down) return;
    mouse_step((MouseStep){ .kind = MS_BTN, .btn = atomic_load(&g_mouse_btn) });
    static const uint32_t dn[3] = { 0x0201u, 0x0204u, 0x0207u };            /* WM_L/R/MBUTTONDOWN */
    kbd_post(down ? dn[b] : dn[b] + 1u, mouse_mk(), mouse_lp());
}

/* a point on the screen (1280x720) -> the game's client area, narrowed to the HUD's area */
static double g_last_nx = -1, g_last_ny = -1, g_frac_dx, g_frac_dy;
static void mouse_move(double sx, double sy, int touch){
    unsigned lw = 1280, lh = 720; metal_game_size(&lw, &lh);
    if (!lw || !lh){ lw = 1280; lh = 720; }
    double ax = atomic_load(&g_ui_asp) / 10000.0, ay = atomic_load(&g_ui_asp_y) / 10000.0;
    double nx = (sx / 1280.0 - (1.0 - ax) * 0.5) / ax, ny = (sy / 720.0 - (1.0 - ay) * 0.5) / ay;
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
    if (touch){
        double ux = lw / 640.0, uy = lh / 480.0;
        if (fabs(x - atomic_load(&g_mouse_x)) < 2.0 * ux && fabs(y - atomic_load(&g_mouse_y)) < 2.0 * uy){
            int h = (int)ceil(2.0 * ux), hx = (int)x + h <= (int)lw - 1 ? (int)x + h : (int)x - h;
            mouse_step((MouseStep){ .kind = MS_HOP, .x = hx, .y = (int)y });
        }
    }
    mouse_step((MouseStep){ .kind = MS_MOVE, .x = (int)x, .y = (int)y, .dx = dx, .dy = dy });
    atomic_store(&g_mouse_x, (int)x); atomic_store(&g_mouse_y, (int)y);
    kbd_post_mousemove(mouse_mk(), mouse_lp());
}

/* ---- the touch screen -> fingers ------------------------------------------------------------------------------- */
typedef struct { int on; u32 id; double x, y; } Finger;
/* a finger held still HOLD_US on the screen "grabs": the left button goes up and the middle one down, so the drag that follows turns
 * the car (the garage) as a held click does with a mouse wheel button; a drag that starts at once stays the left button (sliders, lists) */
#define HOLD_US 300000u
#define HOLD_PX 22.0
static uint64_t g_t0; static double g_x0, g_y0; static int g_grab, g_moved, g_grabs, g_hold_on;      /* g_hold_on: only with the file /switch/nfsu2/holdturn (two fingers turn the car without it) */
static Finger g_f[2];                         /* [0]: the first finger (the mouse), [1]: the second (the turn) */
static int g_inited, g_block;                 /* g_block: after a turn ends, no finger is the mouse until all are lifted */

static void between(double *x, double *y){ *x = (g_f[0].x + g_f[1].x) / 2; *y = (g_f[0].y + g_f[1].y) / 2; }
static void release_all(void){
    if (g_f[1].on || g_grab){ mouse_button(2, 0); g_turns++; }
    else if (g_f[0].on) mouse_button(0, 0);
    g_f[0].on = g_f[1].on = 0; g_block = 1; g_grab = 0;
}
static Finger *find(u32 id){ for (int i = 0; i < 2; i++) if (g_f[i].on && g_f[i].id == id) return &g_f[i]; return NULL; }

void touch_nx_init(void){
    hidInitializeTouchScreen();
    { struct stat st; g_hold_on = stat("/switch/nfsu2/holdturn", &st) == 0; }
    g_inited = 1;
}

void touch_nx_poll(void){
    if (!g_inited) return;
    HidTouchScreenState st; memset(&st, 0, sizeof st);
    if (!hidGetTouchScreenStates(&st, 1)) return;
    int n = st.count > 16 ? 16 : st.count;
    /* the fingers now, by id */
    int seen0 = 0, seen1 = 0;
    for (int i = 0; i < n; i++){
        u32 id = st.touches[i].finger_id; double x = st.touches[i].x, y = st.touches[i].y;
        Finger *f = find(id);
        if (f){
            f->x = x; f->y = y;
            if (f == &g_f[0]) seen0 = 1; else seen1 = 1;
            if (g_f[1].on){ double bx, by; between(&bx, &by); mouse_move(bx, by, 0); }
            else if (f == &g_f[0]){
                if (!g_grab && !g_moved && (fabs(x - g_x0) > HOLD_PX || fabs(y - g_y0) > HOLD_PX)) g_moved = 1;
                mouse_move(x, y, 0); g_drags++;
            }
            continue;
        }
        if (g_block) continue;
        if (!g_f[0].on){                                                   /* the first finger: the mouse, left button */
            g_f[0] = (Finger){ 1, id, x, y }; seen0 = 1;
            g_t0 = mono_us(); g_x0 = x; g_y0 = y; g_grab = 0; g_moved = 0;
            mouse_move(x, y, 1);
            mouse_button(0, 1);
            g_taps++;
        } else if (!g_f[1].on){                                            /* a second one: the turn (middle button) */
            g_f[1] = (Finger){ 1, id, x, y }; seen1 = 1;
            if (!g_grab) mouse_button(0, 0);
            double bx, by; between(&bx, &by); mouse_move(bx, by, 0);
            mouse_button(2, 1);
        }
    }
    /* fingers that are gone */
    if (g_f[1].on && (!seen0 || !seen1)){ release_all(); }
    else if (g_f[0].on && !g_f[1].on && !seen0){ if (g_grab){ mouse_button(2, 0); g_turns++; g_grab = 0; } else mouse_button(0, 0); g_f[0].on = 0; }
    else if (g_hold_on && g_f[0].on && !g_f[1].on && !g_grab && !g_moved && mono_us() - g_t0 >= HOLD_US){
        mouse_button(0, 0); mouse_move(g_f[0].x, g_f[0].y, 0); mouse_button(2, 1); g_grab = 1; g_grabs++;
    }
    if (!n) g_block = 0;
}

void touch_nx_report(FILE *f){ fprintf(f, "touch: %u touches, %u moves, %u turns (%u of them by holding)\n", g_taps, g_drags, g_turns, g_grabs); }
