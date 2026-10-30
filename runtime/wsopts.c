/* wsopts.c - the device size of the "native" display row, for every game
 * (com.c modes_refresh; the page's settings `render` and `max_pixels`):
 *   NFSU2_RENDER=backing|panel|scale:<f>  the window's backing pixels
 *                                 (default), the physical panel's pixel
 *                                 density, or the backing size times f
 *   NFSMAXPIX=<pixels>            the upper bound of every native-size device
 * The game's own aspect mapping (Hor+, the HUD) is the game's file's:
 * widescreen.c for Underground 2, runtime/mw/widescreen_mw.c for Most Wanted. */
#include "widescreen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int    g_init;
static int    g_render = WS_RENDER_BACKING;
static double g_render_f = 1.0;

static void render_init(void){
    if (g_init) return;
    g_init = 1;
    const char *e = getenv("NFSU2_RENDER");
    if (!e || !*e) return;
    if (!strcasecmp(e, "panel")) g_render = WS_RENDER_PANEL;
    else if (!strncasecmp(e, "scale:", 6) || (*e >= '0' && *e <= '9') || *e == '.'){
        double f = atof(strchr(e, ':') ? strchr(e, ':') + 1 : e);
        if (f >= 0.25 && f <= 2.0){ g_render = WS_RENDER_SCALE; g_render_f = f; }
        else fprintf(stderr, "ws: NFSU2_RENDER=%s ignored (scale 0.25..2)\n", e);
    }
}

int ws_render_mode(double *scale){
    render_init();
    if (scale) *scale = g_render_f;
    return g_render;
}

/* Device size of the "native" display row relative to the window's backing
 * pixels (com.c modes_refresh).  panel: the physical panel's density, which
 * in a scaled display mode is lower than the backing store's (3024 panel
 * pixels for 3600 backing pixels on this Mac); never above the backing
 * store, the window server would only scale it down again. */
double ws_render_factor(void){
    render_init();
    double f = 1.0;
    if (g_render == WS_RENDER_PANEL){
        double bs = 0.0, pp = 0.0;
        void metal_screen_metrics(double *, double *);
        metal_screen_metrics(&bs, &pp);
        if (bs > 0.0 && pp > 0.0) f = pp / bs;
        if (f > 1.0) f = 1.0;
    } else if (g_render == WS_RENDER_SCALE) f = g_render_f;
    return f;
}

double ws_max_pixels(void){
    const char *e = getenv("NFSMAXPIX");
    return (e && *e) ? atof(e) : WS_MAX_PIXELS_DEFAULT;
}
