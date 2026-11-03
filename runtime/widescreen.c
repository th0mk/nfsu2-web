/* widescreen.c - native-resolution presentation on screens wider (or
 * narrower) than the game's own 4:3 mode.
 *
 * The game keeps rendering for the mode it picked (1280x960, 640x480 HUD
 * space); com.c creates the device at the window's backing pixels and maps
 * the 4:3 HUD into the middle of the screen (res_pick / aspect_apply).  That
 * mapping alone leaves a few things wrong, which the hooks here fix at the
 * game's own code sites (the ones ThirteenAG's NFSUnderground2.WidescreenFix
 * patches, MIT; addresses verified in our SPEED2.EXE v1.2):
 *
 *   FOV       Hor+ in the projection setup (sub_005c7ec0) for the views that
 *             fill the screen.  The old renderer-side Hor+ scaled every
 *             perspective draw, including the car's cube map faces, and the
 *             game still culled with its 4:3 frustum.
 *   borders   the cutscene letterbox bars are 640-unit HUD quads and covered
 *             only the 4:3 middle of the screen.
 *   FMV       movie quad extents (fit / fill / stretch).
 *   HUD       optional edge anchoring of the race HUD (WFP's .dat table).
 *   marker    a HUD marker placed over a world position (sub_004ebec0)
 *             gets the inverse of the HUD squeeze.
 *   rain      the lens rain drops keep their 4:3 shape.
 *   fill      quads over a whole render target (sub_005c7140: the rain and
 *             overcast tint) are 640-unit HUD quads too and covered only the
 *             4:3 middle; the renderer leaves them full size.
 *
 *   shape     optional square pixels in the views that fill the screen: the
 *             game's projection makes the picture 4-14 % too wide (below).
 *
 * Every hook site is listed with HOOK_SITE() so gen.sh can hand it to the
 * lifter (fill is an override of the whole function).  Options (all read once):
 *   NFSU2_FOV=game|renderer       where Hor+ is applied (default game)
 *   NFSU2_FOV_SCALE=original|correct  the game's proportions (default) or
 *                                 square pixels (WidescreenFix's "FOV Scale:
 *                                 Mathematically Correct")
 *   NFSU2_HUD=43|wide             centred 4:3 HUD or edge-anchored HUD
 *   NFSU2_MOVIE=fit|fill|stretch  FMV framing (default fit = pillarbox)
 *   NFSU2_CUTSCENE_BORDERS=full|off  letterbox bars across the whole width
 *                                    (default) or none
 *   NFSU2_RENDER=backing|panel|scale:<f>  device size of the "native" display
 *                                 row: the window's backing pixels (default),
 *                                 the physical panel's pixel density, or the
 *                                 backing size times f                     */
#include "widescreen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FOV_GAME, FOV_RENDERER };
enum { HUD_43, HUD_WIDE };
enum { MOV_FIT, MOV_FILL, MOV_STRETCH };
enum { BRD_FULL, BRD_OFF };
enum { RENDER_BACKING, RENDER_PANEL, RENDER_SCALE };
enum { SHAPE_ORIGINAL, SHAPE_CORRECT };

static int   g_init;
static int   g_fov = FOV_GAME, g_hud = HUD_WIDE, g_movie = MOV_FIT, g_brd = BRD_FULL;
static int   g_shape = SHAPE_ORIGINAL;
static int   g_hudfix = 1;
static int   g_render = RENDER_BACKING;
static double g_render_f = 1.0;
static float g_k = 1.f;                 /* game aspect / device aspect */
static unsigned long g_hits[16];
/* FE re-render after a mapping change (see the 0x52c711 hook): until this
 * many frames are drawn, at most g_redirty_cap more objects. */
static long g_redirty_until;
static unsigned g_redirty_cap;

static int opt_is(const char *v, const char *s){ return v && !strcasecmp(v, s); }
static void ws_init(void){
    if (g_init) return;
    g_init = 1;
    const char *e;
    if ((e = getenv("NFSU2_FOV")) && *e)
        g_fov = opt_is(e, "renderer") ? FOV_RENDERER : FOV_GAME;
    if ((e = getenv("NFSU2_HUD")) && *e)
        g_hud = (opt_is(e, "43") || opt_is(e, "4:3")) ? HUD_43 : HUD_WIDE;
    if ((e = getenv("NFSU2_MOVIE")) && *e)
        g_movie = opt_is(e, "fill") ? MOV_FILL : opt_is(e, "stretch") ? MOV_STRETCH : MOV_FIT;
    if ((e = getenv("NFSU2_CUTSCENE_BORDERS")) && *e)
        g_brd = (opt_is(e, "off") || *e == '0') ? BRD_OFF : BRD_FULL;
    if ((e = getenv("NFSU2_FOV_SCALE")) && *e)
        g_shape = (opt_is(e, "correct") || opt_is(e, "corrected")) ? SHAPE_CORRECT : SHAPE_ORIGINAL;
    /* HUDFIX=0: legacy, HUD stretched to the full width (diagnostic). */
    if ((e = getenv("HUDFIX")) && *e == '0') g_hudfix = 0;
    if ((e = getenv("NFSU2_RENDER")) && *e){
        if (opt_is(e, "panel")) g_render = RENDER_PANEL;
        else if (!strncasecmp(e, "scale:", 6) || (*e >= '0' && *e <= '9') || *e == '.'){
            double f = atof(strchr(e, ':') ? strchr(e, ':') + 1 : e);
            if (f >= 0.25 && f <= 2.0){ g_render = RENDER_SCALE; g_render_f = f; }
            else fprintf(stderr, "ws: NFSU2_RENDER=%s ignored (scale 0.25..2)\n", e);
        }
    }
    fprintf(stderr, "ws: fov=%s fov scale=%s hud=%s movie=%s cutscene borders=%s render=%s",
            g_fov == FOV_GAME ? "game" : "renderer", g_shape == SHAPE_CORRECT ? "correct" : "original",
            g_hud == HUD_WIDE ? "wide" : "4:3",
            g_movie == MOV_FIT ? "fit" : g_movie == MOV_FILL ? "fill" : "stretch",
            g_brd == BRD_FULL ? "full" : "off",
            g_render == RENDER_PANEL ? "panel" : g_render == RENDER_SCALE ? "scale" : "backing");
    if (g_render == RENDER_SCALE) fprintf(stderr, ":%.3g", g_render_f);
    fprintf(stderr, "\n");
}

/* Device size of the "native" display row relative to the window's backing
 * pixels (com.c modes_refresh).  panel: the physical panel's density, which
 * in a scaled display mode is lower than the backing store's (3024 panel
 * pixels for 3600 backing pixels on this Mac); never above the backing
 * store, the window server would only scale it down again. */
double ws_render_factor(void){
    ws_init();
    double f = 1.0;
    if (g_render == RENDER_PANEL){
        double bs = 0.0, pp = 0.0;
        void metal_screen_metrics(double *, double *);
        metal_screen_metrics(&bs, &pp);
        if (bs > 0.0 && pp > 0.0) f = pp / bs;
        if (f > 1.0) f = 1.0;
    } else if (g_render == RENDER_SCALE) f = g_render_f;
    return f;
}

double ws_max_pixels(void){
    const char *e = getenv("NFSMAXPIX");
    return (e && *e) ? atof(e) : WS_MAX_PIXELS_DEFAULT;
}

void ws_set_mapping(float k){
    ws_init();
    if (!(k > 0.05f && k < 20.f)) k = 1.f;
    if (k != g_k){
        long metal_frames(void);
        fprintf(stderr, "ws: game/device aspect %.4f\n", (double)k);
        g_redirty_until = metal_frames() + 3; g_redirty_cap = 100000;
    }
    g_k = k;
}
/* The test script's "fovscale MODE" (autotest.c), for an A/B in one run. */
int ws_set_fov_scale(const char *mode){
    ws_init();
    if (opt_is(mode, "original")) g_shape = SHAPE_ORIGINAL;
    else if (opt_is(mode, "correct") || opt_is(mode, "corrected")) g_shape = SHAPE_CORRECT;
    else return 0;
    return 1;
}
float ws_persp_x(void){ ws_init(); return g_fov == FOV_RENDERER ? fminf(g_k, 1.f) : 1.f; }
float ws_persp_y(void){ ws_init(); return g_fov == FOV_RENDERER ? fminf(1.f / g_k, 1.f) : 1.f; }
float ws_ui_x(void){ ws_init(); return g_hudfix ? fminf(g_k, 1.f) : 1.f; }
float ws_ui_y(void){ ws_init(); return g_hudfix ? fminf(1.f / g_k, 1.f) : 1.f; }

/* HUD space is 640x480 and com.c shows it in the middle of the screen; the
 * whole screen is 640/ui_x units wide.  Extra units on each side: */
static float hud_extra_x(void){ return 320.f * (1.f / ws_ui_x() - 1.f); }
static float hud_extra_y(void){ return 240.f * (1.f / ws_ui_y() - 1.f); }

/* ---- edge-anchored HUD ---------------------------------------------------
 * Element anchors from WidescreenFix's NFSUnderground2.WidescreenFix.dat
 * (73 entries, identical to the copy in the user's install): FE object
 * position (x, y) -> anchored to the left (-1) or right (+1) screen edge.
 * WFP shifts by -109/+119 units at 16:9, i.e. 2.33 resp. 12.33 units more
 * than the 106.67 extra units a 16:9 screen has.  Here the shift is the
 * extra width the screen really has plus that margin; the margin fades in
 * up to 16:9, so a 4:3 window gets no shift and 16:9 matches WFP exactly.
 * Wider than 16:9 the HUD stays on the edges of a centred 16:9 area: the
 * game stops drawing some backing panels (tachometer dial, leaderboard box)
 * once they lie entirely beyond its 640-unit screen, which happened from
 * about 2.2:1 on (21:9 test). */
static const struct { int16_t x, y; int8_t side; } g_anchor[] = {
    {-295,-170,-1},{-295,-186,-1},{-295,-204,-1},{-295,-211,-1},       /* EA Trax text */
    {-173,-177,-1},{-127,-177,-1},{-275,-181,-1},{-125,-177,-1},       /* EA Trax */
    { -31,-177,-1},{-131,-177,-1},{ -42,-177,-1},{-275,-182,-1},
    { -43,-177,-1},{ -92,-177,-1},{ -93,-177,-1},{  33,-177,-1},
    {-223,  70,-1},{-223, 142,-1},{-223,  71,-1},{-223, 143,-1},       /* minimap */
    {-143, 196,-1},
    {-248,-122,-1},{-195,-112,-1},                                     /* SMS bar, icon */
    {-185, -66,-1},{-196,-185,-1},{-224,-117,-1},                      /* drift info */
    { 142, 195, 1},{ 199, 177, 1},{-110, -26,-1},{-254, 171,-1},       /* tachometer */
    { 205, 181, 1},{ 168, 202, 1},{ 207, 175, 1},{ 160, 206, 1},
    { 220, 151, 1},{ 253, 117, 1},                                     /* N2O */
    {-263,  36,-1},                                                    /* satellite signal */
    { 285,  14, 1},                                                    /* "too slow" */
    { 270,  91, 1},{ 284,  -9, 1},{ 284,  -2, 1},{ 295, -89, 1},       /* N2O earned */
    { 295, -78, 1},
    {-246,-158,-1},{ 224,-157, 1},                                     /* outrun distance, time */
    { 257,-182, 1},{ 213,-224, 1},{ 196,-100, 1},{ 215,-159, 1},       /* leaderboard */
    { 224, -52, 1},{ 253,-149, 1},{ 217,-161, 1},{ 240,-161, 1},
    { 139,-163, 1},{ 223, -52, 1},{ 266,-180, 1},{ 280,-129, 1},
    { 275,-167, 1},{ 187,-190, 1},{ 179,-134, 1},{ 242,-161, 1},
    { 219,-161, 1},{ 140,-162, 1},{ 223, -52, 1},{ 224,-132, 1},
    { 191,-127, 1},{ 201,-137, 1},                                     /* test run module */
    { 227,-184, 1},{ 193,-194, 1},{ 283,-163, 1},                      /* bank */
    {-223,  34,-1},{-223, -87,-1},{-223,-172,-1},                      /* wrong way sign */
};
/* x offset for the element anchored at (x, y); 0 if it is not anchored. */
static float hud_shift(float x, float y){
    if (g_hud != HUD_WIDE) return 0.f;
    float extra = hud_extra_x();
    if (extra <= 0.f) return 0.f;
    int ix = (int)floorf(x), iy = (int)floorf(y);
    for (unsigned i = 0; i < sizeof g_anchor / sizeof g_anchor[0]; i++){
        if (g_anchor[i].x != ix || g_anchor[i].y != iy) continue;
        const float e169 = 320.f * (16.f / 9.f * 0.75f - 1.f);          /* 106.67 */
        float m = (g_anchor[i].side < 0 ? 109.f : 119.f) - e169;        /* 2.33 / 12.33 */
        float e = fminf(extra, e169);
        float d = e + m * e / e169;
        return g_anchor[i].side < 0 ? -d : d;
    }
    return 0.f;
}

static uint32_t fbits(float f){ uint32_t u; memcpy(&u, &f, 4); return u; }
static float    bitsf(uint32_t u){ float f; memcpy(&f, &u, 4); return f; }

/* Cutscene letterbox bar: four (x, y, z, w) vertices in 640x480 HUD units,
 * passed to the quad drawer sub_005c5020.  The game draws them from x 0 to
 * 640, i.e. only across the 4:3 middle of a wider screen. */
static int g_border_quad;
int ws_take_border_quad(void){ int b = g_border_quad; g_border_quad = 0; return b; }
static void border_quad(uint32_t v){
    if (!v || !mem_is_committed(v) || !mem_is_committed(v + 63u)) return;
    g_border_quad = 1;
    float ex = hud_extra_x() + 1.f, ey = hud_extra_y() + 1.f;
    for (int i = 0; i < 4; i++){
        uint32_t a = v + 16u * (uint32_t)i;
        float x = MRF32(a), y = MRF32(a + 4u);
        if (g_brd == BRD_OFF){ MRF32(a) = 0.f; continue; }      /* zero width, like WFP */
        MRF32(a) = x < 320.f ? -ex : 640.f + ex;
        /* Narrower than 4:3: the picture extends above and below the HUD
         * space, keep the bars on its outer edges. */
        if (y <= 0.f) MRF32(a + 4u) = -ey; else if (y >= 480.f) MRF32(a + 4u) = 480.f + ey;
    }
}

/* FMV quad (sub_00536950 -> sub_0051a1c0): left, top, right, bottom in HUD
 * fractions, -0.5..0.5 = the 4:3 HUD space.  Fit leaves it (pillarbox on a
 * wide screen), fill covers the screen keeping the aspect, stretch fills
 * the screen ignoring it. */
static void movie_quad(uint32_t sp){
    float sx = 1.f / ws_ui_x(), sy = 1.f / ws_ui_y();          /* screen in HUD units */
    float hx = 0.5f, hy = 0.5f;
    if (g_movie == MOV_FILL){ float s = fmaxf(sx, sy); hx = hy = 0.5f * s; }
    else if (g_movie == MOV_STRETCH){ hx = 0.5f * sx; hy = 0.5f * sy; }
    if (fabsf(MRF32(sp) + 0.5f) > 1e-4f || fabsf(MRF32(sp + 8u) - 0.5f) > 1e-4f) return;
    MRF32(sp) = -hx; MRF32(sp + 4u) = -hy; MRF32(sp + 8u) = hx; MRF32(sp + 12u) = hy;
}

/* sub_005c7140(surface, texture, colour, ?, src blend, dest blend, flag)
 * makes the surface the render target and draws one quad over all of it:
 * 0..640 x 0..480 HUD units, i.e. identity-matrix clip space, which com.c
 * narrows to the 4:3 middle like the HUD.  Its callers mean the whole
 * target: sub_005c7860's weather tint (DESTCOLOR x SRCCOLOR, a 16x16 grey
 * texture times the rain/overcast colour, into the frame's copy) and two
 * alpha passes in sub_005caad0.  While it runs, aspect_apply leaves
 * orthographic draws alone. */
static int g_fill;
void sub_005c7140_orig(CPU *c);
void sub_005c7140(CPU *c){
    g_hits[12]++;
    g_fill++;
    sub_005c7140_orig(c);
    g_fill--;
}
int ws_fill_target(void){ return g_fill > 0; }

void ws_hud_marker(float *x, float *y){
    ws_init();
    if (g_fov != FOV_GAME) return;
    float ux = ws_ui_x(), uy = ws_ui_y();
    if (ux < 1.f) *x = 320.f + (*x - 320.f) / ux;
    if (uy < 1.f) *y = 240.f + (*y - 240.f) / uy;
}

int ws_hook(CPU *c, uint32_t ip){
    ws_init();
    switch (ip){
    /* sub_005c7ec0, projection setup per eView ([ebp+4] = view id):
     *   P00 = 1/tan(FOV*hscale/2) stored at 0x5c7fee,
     *   P11 = 1/tan(FOV*aspect/2) stored at 0x5c8025,
     * then the view*projection product and the culling frustum planes are
     * built from them.  Views 1/2 (player) and 4/5 (their road reflection,
     * projected in screen space) fill the screen and get Hor+ (or Vert+ on
     * a narrower screen).  The rear-view mirror (3), shadow views (9, 10)
     * and the cube map faces (16-21) keep the game's projection.
     *
     * Shape: in those views hscale = [0x7a27dc] = 1.1 and aspect = the view's
     * H/W (960/1280) x [0x7a27e0] / [0x7a27d8] = 1.1 / 0.8696, so the angle
     * is scaled, not its tangent, and pixels are not square: a wheel shows
     * 1.14 times as wide as tall at the garage's ~32 degrees, 1.09-1.11 at
     * the race cameras' 65-75.  NFSU2_FOV_SCALE=correct keeps the game's
     * height (P11, after any Vert+) and makes P00 = P11 x the device's H/W
     * (= the view's H/W x k): the picture shows 4-14 % more at the sides.  At
     * 0x5c8025 P00 waits at [esp+0x28] (stored after 0x5c7fee, three pushes
     * ago) and goes into the matrix after; the product and the culling
     * planes are made of both, so they agree. */
    case HOOK_SITE(0x005c7fee):
    case HOOK_SITE(0x005c8025): {
        g_hits[ip == 0x005c7feeu ? 0 : 1]++;
        if (g_fov != FOV_GAME) return 1;
        uint32_t view = mem_is_committed(c->ebp + 4u) ? MR32(c->ebp + 4u) : 0u;
        if (!(view == 1u || view == 2u || view == 4u || view == 5u)) return 1;
        double before = ST(0);
        uint32_t vp = mem_is_committed(c->ebp + 0x5bu) ? MR32(c->ebp + 0x58u) : 0u;
        uint32_t vw = vp && mem_is_committed(vp + 0x1bu) ? MR32(vp + 0x14u) : 0u;
        uint32_t vh = vp && mem_is_committed(vp + 0x1bu) ? MR32(vp + 0x18u) : 0u;
        if (ip == 0x005c7feeu){ if (g_k < 1.f) ST(0) *= (double)g_k; }
        else if (g_k > 1.f) ST(0) /= (double)g_k;
        { static unsigned shown[2]; int s = ip == 0x005c7feeu ? 0 : 1;
          if (shown[s] < 4 && ST(0) != before){ shown[s]++;
              fprintf(stderr, "ws: view %u %s %.4f -> %.4f (view size %ux%u)\n", view,
                      s ? "P11" : "P00", before, ST(0), vw, vh); } }
        if (ip == 0x005c8025u && g_shape == SHAPE_CORRECT && vw && vh
            && mem_is_committed(c->esp + 0x2bu)){
            float p00 = MRF32(c->esp + 0x28u);
            float sq = (float)(ST(0) * (double)vh / (double)vw * (double)g_k);
            if (p00 > 0.01f && p00 < 100.f && sq > 0.01f && sq < 100.f){
                MRF32(c->esp + 0x28u) = sq; g_hits[13]++;
                static unsigned said;
                if (said < 4 && fabsf(sq - p00) > 1e-4f){ said++;
                    fprintf(stderr, "ws: view %u square pixels: P00 %.4f -> %.4f (P11 %.4f, k %.4f)\n",
                            view, (double)p00, (double)sq, ST(0), (double)g_k); }
            }
        }
        return 1; }
    /* sub_004ebec0 puts a HUD marker over a world position: the view's
     * screen position (sub_005bc4a0, from the view*projection product, so
     * already Hor+) divided by the view size, times 640 resp. 480 HUD
     * units.  The HUD is shown narrowed to the 4:3 middle, so the marker
     * needs the inverse of that (it sat k times too close to the centre). */
    case HOOK_SITE(0x004ec35c):
    case HOOK_SITE(0x004ec36e): {
        int y = ip == 0x004ec36eu;
        g_hits[y ? 9 : 8]++;
        if (g_fov != FOV_GAME) return 1;
        float u = y ? ws_ui_y() : ws_ui_x(), c0 = y ? 240.f : 320.f;
        if (u < 1.f) ST(0) = c0 + (ST(0) - c0) / u;
        return 1; }
    /* Rain drops on the lens (sub_005c7b..): quads in clip units, centre
     * [esp+10] plus/minus the extent in ST0 (= [esp+0c]).  The same extent
     * in x and y gave 4:3-shaped drops at 4:3 and wider ones on a wider
     * screen; scaling x by the HUD factor keeps the original shape (WFP W2
     * makes them round and smaller instead). */
    case HOOK_SITE(0x005c7ce9): {
        g_hits[10]++;
        ST(0) *= (double)ws_ui_x();
        return 1; }
    /* Letterbox bars: top (0x5cbebd) and bottom (0x5cbf31) quad, the vertex
     * pointer is the first argument already on the stack. */
    case HOOK_SITE(0x005cbebd):
    case HOOK_SITE(0x005cbf31):
        g_hits[2]++;
        border_quad(MR32(c->esp));
        return 1;
    /* FMV quad: the four extents are the last four pushes. */
    case HOOK_SITE(0x00536a32):
        g_hits[3]++;
        movie_quad(c->esp);
        return 1;
    /* Edge-anchored HUD, the four places WidescreenFix hooks:
     * FE object position just stored to [esp+60]/[esp+64]. */
    case HOOK_SITE(0x0051b198): {
        g_hits[4]++;
        float x = MRF32(c->esp + 0x60u), y = MRF32(c->esp + 0x64u);
        float d = hud_shift(x, y);
        if (d != 0.f) MRF32(c->esp + 0x60u) = x + d;
        return 1; }
    /* Radar blips: ecx = [ebx+1c] is about to be stored as the blip x. */
    case HOOK_SITE(0x0051d655): {
        g_hits[5]++;
        float x = bitsf(c->ecx), y = MRF32(c->ebx + 0x20u);
        float d = hud_shift(x, y);
        if (d != 0.f) c->ecx = fbits(x + d);
        return 1; }
    /* ecx = [esi+1c], stored as the element x a few instructions later. */
    case HOOK_SITE(0x004c6759): {
        g_hits[6]++;
        float x = bitsf(c->ecx), y = MRF32(c->esi + 0x20u);
        float d = hud_shift(x, y);
        if (d != 0.f) c->ecx = fbits(x + d);
        return 1; }
    /* "Stop sign": [esi] = ST0 + [edx] was just stored; move it by the
     * shift of the anchor at [edx]. */
    case HOOK_SITE(0x0050516f): {
        g_hits[7]++;
        float d = hud_shift(MRF32(c->edx), MRF32(c->edx + 4u));
        if (d != 0.f) MRF32(c->esi) += d;
        return 1; }
    /* FEng's render traversal (sub_0052c5d0), edi = the child about to be
     * drawn.  FEng rebuilds an object's vertices only when its flags (+1c)
     * have 0x400000 set, which its own setters do when they move it.  The
     * HUD anchoring above is applied while the vertices are built, so
     * after a resize or a fullscreen switch in a race the static parts
     * (tachometer dial, minimap ring, leaderboard labels) kept the old
     * shift while needles and numbers followed.  For a few frames after
     * the mapping changes every object drawn gets that flag. */
    case HOOK_SITE(0x0052c711): {
        long metal_frames(void);
        if (g_redirty_cap && metal_frames() < g_redirty_until){
            g_redirty_cap--; g_hits[11]++;
            if (mem_is_committed(c->edi + 0x1fu)) MR32(c->edi + 0x1cu) |= 0x400000u;
        }
        return 1; }
    /* The same for a package's top-level objects: the package loop
     * (sub_00536520) draws them itself, ebx = the object about to be drawn
     * (a group goes to the traversal above).  The race leaderboard's header
     * panel is one: turned from portrait to landscape in a race, it stayed
     * where the portrait screen had it. */
    case HOOK_SITE(0x00536745): {
        long metal_frames(void);
        void menusize_obj(CPU *);
        menusize_obj(c);
        if (g_redirty_cap && metal_frames() < g_redirty_until){
            g_redirty_cap--; g_hits[11]++;
            if (mem_is_committed(c->ebx + 0x1fu)) MR32(c->ebx + 0x1cu) |= 0x400000u;
        }
        return 1; }
    default:
        return 0;
    }
}

void ws_report(void){
    if (!g_init) return;
    fprintf(stderr, "ws: hooks fov=%lu/%lu borders=%lu movie=%lu hud=%lu/%lu/%lu/%lu marker=%lu/%lu rain=%lu"
            " fe-rerender=%lu fill=%lu square=%lu (k=%.4f)\n",
            g_hits[0], g_hits[1], g_hits[2], g_hits[3],
            g_hits[4], g_hits[5], g_hits[6], g_hits[7], g_hits[8], g_hits[9], g_hits[10],
            g_hits[11], g_hits[12], g_hits[13], (double)g_k);
}
