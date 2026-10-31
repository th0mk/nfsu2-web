/* menusize.c -- the menus' small buttons drawn bigger (a phone's fingers):
 * the help bar's and the scrollers' arrows.
 *
 * The PC help bar (UI_PC_Help_Bar.fng: Back, Continue, Quit Game and the like,
 * bottom right of every menu) is six button slots in two rows, each a 144 x 24
 * frame (an image, FEng type 1) with its label (a string, type 2, size 0.45)
 * and a highlight image over it, all top-level objects of the package placed
 * in units from the screen's centre: columns at x -72, 76 and 223, rows at y
 * 203 and 226 (the band behind them and the profile box bottom left are a
 * group of their own).  On a phone 390 px tall a frame is some 117 x 19 px and
 * its label 7 px high.  The icon scrollers' arrows (the main menu's, Quick
 * Race's: images named the same in every package) are 26 units, 21 px.
 *
 * Here the bar's objects get their position and size scaled about the frames'
 * bottom right corner, so the bar grows up and to the left and its rows and
 * columns keep their gaps; the arrows get their size scaled about their
 * centres.  FEng draws an object from its data (+0x2c: position +0x1c, size
 * +0x38; an image is position +- size / 2, a string's glyphs are scaled by its
 * size) and hit-tests the mouse against the same data (0x52d100 -> 0x52d020 ->
 * 0x51d9f0, the bounds), so a tap lands on the button as drawn.  The bar lays
 * its buttons out every frame (their positions, not their sizes), so the
 * scaling runs right before the bar takes the mouse (its message handler
 * 0x55dfa0, message 0x9803f6e2, every frame: a hover or a click, 0x52d4f0 /
 * 0x55de00) and again before each package is drawn (the package loop
 * sub_00536520, widescreen.c's hook at 0x536745: menusize_obj).  Each object's
 * own values are kept: a value that is not the one written here was set by the
 * game and is the new own value, so nothing is scaled twice; a change sets the
 * object's rebuild flag (0x400000), as FEng's own setters do.
 *
 * The scale: the page's (web/src/app/touch.ts menuSize: the Controls sheet's
 * Menu button size, auto big enough for a finger), input mailbox word
 * IM_MENU_SCALE, read through input_menu_scale(); NFSU2_MENU_SCALE=<f>
 * overrides it.  1 = the game's own size.  The bar and the arrows each have
 * their own: auto sizes the arrows alone, as the bar's Continue and Back are
 * the touch controls' OK and Back, and a bigger bar covers the menus' own
 * things near it (the paint list, a part's name).  Only where menusize_install ran
 * (the web build), as the bar's mouse needs the handler.
 * NFSU2_MENU_DUMP=<package names> prints those packages' objects once.
 * Original: no scaling. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define O_NEXT    0x04u
#define O_TYPE    0x18u
#define O_FLAGS   0x1cu
#define O_DATA    0x2cu
#define O_HASH    0x10u     /* its name's hash */
#define O_KIDS    0x64u     /* a group's first child */
#define P_OBJS    0x4cu     /* a package's first top-level object */
#define D_POS     0x1cu     /* data: position x, y (units from the screen's centre) */
#define D_SIZE    0x38u     /* data: size x, y */
#define T_IMAGE   1u
#define T_STRING  2u
#define T_GROUP   5u
#define F_DIRTY   0x400000u /* rebuild the vertices */
#define FN_HB_MSG 0x0055dfa0u   /* the help bar's message handler, thiscall (msg, ...), only from its vtable */
#define MSG_MOUSE 0x9803f6e2u
#define HB_MASK   0x64u     /* the bar's buttons now (the screen table's help bar mask: 0x4 Continue, 0x400 Back ...) */
#define MOUSE_WENT_DOWN 0x008763f2u   /* the mouse (0x8763d0): left button went down this frame; right +3 */
#define BAR_FNG   "UI_PC_Help_Bar.fng"
#define FRAME_W   100.f     /* the frames are 144 wide, the labels' size under 1 */
#define MAX_SCALE 2.5f
#define MAX_OBJS  64       /* a package's top-level objects looked at */
#define KEPT_MAX  512

__attribute__((weak)) float input_menu_scale(int bar){ (void)bar; return 1.f; }
void sub_0055dfa0(CPU *c);
void dispatch_patch(uint32_t a, void (*f)(CPU *));

static int g_installed;
static uint32_t g_bar;                          /* the help bar's package, once drawn */
static float g_last[2] = { 1.f, 1.f };          /* the scales logged last: the arrows', the bar's */

static int ok(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }
static const char *pkg_name(uint32_t pkg){
    if (!ok(pkg, 0x50u)) return 0;
    uint32_t s = MR32(pkg + 0x0cu);
    if (!ok(s, 64u)) return 0;
    return (const char *)(g_mem + s);
}
static uint32_t field(int j){ return (j < 2 ? D_POS : D_SIZE) + 4u * (uint32_t)(j & 1); }

/* ---- the dump ---------------------------------------------------------- */
static void dump_obj(uint32_t o, int depth){
    for (int n = 0; o && n < MAX_OBJS && ok(o, 0x70u); n++, o = MR32(o + O_NEXT)){
        uint32_t d = MR32(o + O_DATA), t = MR32(o + O_TYPE);
        float v[4] = {0};
        if (ok(d, 0x44u)) for (int j = 0; j < 4; j++) v[j] = MRF32(d + field(j));
        fprintf(stderr, "[menusize] %*s%08x t=%u fl=%08x hash=%08x pos=%.1f,%.1f size=%.2f,%.2f\n",
                depth * 2, "", o, t, MR32(o + O_FLAGS), MR32(o + O_HASH), (double)v[0], (double)v[1], (double)v[2], (double)v[3]);
        if (t == T_GROUP) dump_obj(MR32(o + O_KIDS), depth + 1);
    }
}
static void dump(uint32_t pkg, const char *nm){
    static const char *want; static int init;
    if (!init){ init = 1; want = getenv("NFSU2_MENU_DUMP"); if (want && !*want) want = 0; }
    if (!want || !strstr(want, nm)) return;
    static uint32_t seen[64]; static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == pkg) return;
    if (nseen < 64) seen[nseen++] = pkg;
    fprintf(stderr, "[menusize] package %s (%08x):\n", nm, pkg);
    dump_obj(MR32(pkg + P_OBJS), 0);
}

/* ---- the scale --------------------------------------------------------- */
/* bar: the help bar's, else the arrows' */
static float scale_now(int bar){
    static int init; static float env = 0.f;
    if (!init){
        init = 1;
        const char *e = getenv("NFSU2_MENU_SCALE");
        if (e && *e) env = (float)atof(e);
        if (env) fprintf(stderr, "[menusize] NFSU2_MENU_SCALE=%.2f\n", (double)env);
    }
    float s = env ? env : input_menu_scale(bar);
    if (!(s >= 1.f)) s = 1.f;                   /* also NaN */
    return s > MAX_SCALE ? MAX_SCALE : s;
}

/* an object's own position and size, and the ones written here; seen: the frame it was last scaled in */
typedef struct { uint32_t data; long seen; float own[4], put[4]; } Kept;
static Kept g_kept[KEPT_MAX];
static int g_nkept;
static long g_now;

static Kept *kept(uint32_t d){
    Kept *k = 0;
    for (int i = 0; i < g_nkept; i++) if (g_kept[i].data == d){ k = &g_kept[i]; break; }
    if (!k){
        if (g_nkept < KEPT_MAX) k = &g_kept[g_nkept++];
        else { k = &g_kept[0]; for (int i = 1; i < KEPT_MAX; i++) if (g_kept[i].seen < k->seen) k = &g_kept[i]; }
        k->data = d;
        for (int j = 0; j < 4; j++) k->own[j] = k->put[j] = MRF32(d + field(j));
    }
    for (int j = 0; j < 4; j++){
        float cur = MRF32(d + field(j));
        if (cur != k->put[j]) k->own[j] = cur;          /* the game set it */
    }
    k->seen = g_now;
    return k;
}
/* want into the object (position x, y, size x, y), its vertices rebuilt if that changed anything */
static void put(uint32_t o, Kept *k, const float want[4]){
    int changed = 0;
    for (int j = 0; j < 4; j++){
        if (MRF32(k->data + field(j)) != want[j]){ MRF32(k->data + field(j)) = want[j]; changed = 1; }
        k->put[j] = want[j];
    }
    if (changed) MR32(o + O_FLAGS) |= F_DIRTY;
}

/* the bar's top-level images and strings, scaled by s about the frames' bottom right corner */
static void apply_bar(uint32_t pkg, float s){
    uint32_t obj[MAX_OBJS]; Kept *k[MAX_OBJS]; int n = 0;
    float px = 0.f, py = 0.f; int pivot = 0;
    for (uint32_t o = MR32(pkg + P_OBJS); o && n < MAX_OBJS && ok(o, 0x30u); o = MR32(o + O_NEXT)){
        uint32_t t = MR32(o + O_TYPE), d = MR32(o + O_DATA);
        if ((t != T_IMAGE && t != T_STRING) || !ok(d, 0x44u) || !(k[n] = kept(d))) continue;
        if (t == T_IMAGE && k[n]->own[2] > FRAME_W){
            float r = k[n]->own[0] + k[n]->own[2] * 0.5f, b = k[n]->own[1] + k[n]->own[3] * 0.5f;
            if (!pivot || r > px) px = r;
            if (!pivot || b > py) py = b;
            pivot = 1;
        }
        obj[n++] = o;
    }
    for (int i = 0; pivot && i < n; i++){
        const float *own = k[i]->own;
        float want[4] = { px + (own[0] - px) * s, py + (own[1] - py) * s, own[2] * s, own[3] * s };
        put(obj[i], k[i], want);
    }
}

/* a scroller's arrows (their FEng names' hashes, the same in every package that has them), scaled by s about
 * their own centres: the main menu's and Quick Race's, 26 units */
static const uint32_t ARROWS[] = { 0x444969fdu, 0x444969feu };
static void apply_arrows(uint32_t pkg, float s){
    int n = 0;
    for (uint32_t o = MR32(pkg + P_OBJS); o && n < MAX_OBJS && ok(o, 0x30u); o = MR32(o + O_NEXT), n++){
        uint32_t h = MR32(o + O_HASH), d = MR32(o + O_DATA);
        if (MR32(o + O_TYPE) != T_IMAGE || (h != ARROWS[0] && h != ARROWS[1]) || !ok(d, 0x44u)) continue;
        Kept *k = kept(d);
        float want[4] = { k->own[0], k->own[1], k->own[2] * s, k->own[3] * s };
        put(o, k, want);
    }
}

static void apply(uint32_t pkg, int bar){
    float s = scale_now(bar);
    if (s == 1.f && !g_nkept) return;              /* the game's own size, and nothing written */
    long metal_frames(void);
    g_now = metal_frames();
    if (bar) apply_bar(pkg, s); else apply_arrows(pkg, s);
    if (s != g_last[bar]){
        fprintf(stderr, "[menusize] %s at %.0f %%\n", bar ? "help bar" : "scroller arrows", (double)(s * 100.f));
        g_last[bar] = s;
    }
}

/* the package loop is about to queue a top-level object: ebx = it, edi = its package */
void menusize_obj(CPU *c){
    if (!g_installed) return;
    uint32_t pkg = c->edi;
    if (!ok(pkg, 0x50u) || c->ebx != MR32(pkg + P_OBJS)) return;   /* once a package: at its first object */
    const char *nm = pkg_name(pkg);
    if (!nm) return;
    dump(pkg, nm);
    int bar = !strcasecmp(nm, BAR_FNG);
    if (bar) g_bar = pkg;
    apply(pkg, bar);
}

/* The bar takes the mouse as a screen opens: a tap that closed a dialog or the screen before (its No, a list's
 * item) was also a press for the next screen's bar, in the frame it got its buttons, and a bigger bar is more
 * often under that tap (No on "create a new profile?" lies on the main menu's Continue at 175 %: Quick Race
 * opened).  So a press within a frame of the bar's buttons changing (its mask, +0x64) is a hover. */
static void bar_msg(CPU *c){
    if (MR32(c->esp + 4u) != MSG_MOUSE){ sub_0055dfa0(c); return; }
    if (g_bar){
        const char *nm = pkg_name(g_bar);              /* still the bar's: it is never freed while shown */
        if (nm && !strcasecmp(nm, BAR_FNG)) apply(g_bar, 1); else g_bar = 0;
    }
    static uint32_t mask; static long changed;
    long metal_frames(void), f = metal_frames();
    uint32_t m = ok(c->ecx, 0x70u) ? MR32(c->ecx + HB_MASK) : 0u;
    if (m != mask){ mask = m; changed = f; }
    uint8_t l = MR8(MOUSE_WENT_DOWN), r = MR8(MOUSE_WENT_DOWN + 3u);
    if (f - changed > 1 || g_last[1] == 1.f || !(l | r)){ sub_0055dfa0(c); return; }
    MR8(MOUSE_WENT_DOWN) = 0; MR8(MOUSE_WENT_DOWN + 3u) = 0;
    sub_0055dfa0(c);
    MR8(MOUSE_WENT_DOWN) = l; MR8(MOUSE_WENT_DOWN + 3u) = r;
    fprintf(stderr, "[menusize] a press as the help bar's buttons changed, not a click on them\n");
}

void menusize_install(void){ dispatch_patch(FN_HB_MSG, bar_msg); g_installed = 1; }
