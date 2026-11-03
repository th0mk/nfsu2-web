/* Native-resolution presentation: per-view Hor+ FOV, cutscene borders, FMV
 * framing and the edge-anchored HUD.  See widescreen.c. */
#ifndef WIDESCREEN_H
#define WIDESCREEN_H
#include <stdint.h>
#include "cpu.h"

/* Mark a guest instruction where ws_hook gets control first.  gen.sh greps
 * the runtime sources for HOOK_SITE(0x00xxxxxx) and passes the list to the lifter
 * (HOOKS=), which emits guest_hook(c, va) before that instruction. */
#define HOOK_SITE(va) (va##u)

/* Game aspect / device aspect (com.c res_pick).  1 = the device has the
 * game's own aspect; < 1 = wider screen (Hor+), > 1 = narrower (Vert+). */
void  ws_set_mapping(float k);
/* NFSU2_FOV_SCALE from a test script ("fovscale original|correct",
 * autotest.c); 0 if the mode is not one of those. */
int   ws_set_fov_scale(const char *mode);
/* Clip-space factors the renderer applies itself (com.c aspect_apply):
 * perspective draws only when NFSU2_FOV=renderer, the 4:3 HUD always. */
float ws_persp_x(void);
float ws_persp_y(void);
float ws_ui_x(void);
float ws_ui_y(void);
/* NFSU2_RENDER: size of the "native" display row relative to the window's
 * backing pixels (1 = 1:1). */
double ws_render_factor(void);
/* NFSMAXPIX: upper bound for every native-size device (pixels; 0 or a value
 * up to 100,000 = no bound).  The one default for com.c (display mode rows)
 * and metal.m (fullscreen, placeholder window, non-row modes): 9 MP keeps a
 * 3600x2196 backing store (7.9 MP) and its fullscreen size uncapped and
 * still bounds 5K/6K displays.  Thread-safe (reads the environment). */
#define WS_MAX_PIXELS_DEFAULT 9.0e6
double ws_max_pixels(void);
/* A HUD marker over a world position, in 640x480 HUD units from the view's
 * pixels (x / width * 640, y / height * 480): moved out to where the
 * position is shown on a screen wider than the 4:3 HUD (the game's own
 * marker, sub_004ebec0, gets this at its hook sites; roam_game.c's names
 * over friends' cars call it). */
void  ws_hud_marker(float *x, float *y);
/* 1 while the game draws a quad over a whole render target (sub_005c7140,
 * e.g. the rain tint): com.c's aspect_apply does not narrow it to 4:3. */
int   ws_fill_target(void);
/* 1 once after a cutscene bar's quad was placed (its draw follows): com.c
 * does not keep that draw to the HUD's 4:3 band. */
int   ws_take_border_quad(void);
/* 1 if the hook at ip was a widescreen site (guest_hook returns then). */
int   ws_hook(CPU *c, uint32_t ip);
void  ws_report(void);

#endif
