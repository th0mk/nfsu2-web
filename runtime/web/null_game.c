/* wasm: stand-ins for the game feature functions the runtime's core still
 * calls by name (the rest of the game is reached through runtime/game.h),
 * and for the game table itself.
 *
 * Every definition is weak: a build with the game's files (UG2's
 * game_ug2.c, widescreen.c, framerate.c, career.c, roam*.c, d3dx_hle.c,
 * pad_ug2.c, ...) replaces them at link time, so they change nothing
 * there.  A build without those files links with these instead and behaves
 * as if the feature were absent: no game to run, no aspect mapping, no
 * frame-rate modes, no free roam, no D3DX statistics, no headlight ramps, no
 * test career commands, no player input controller to peek, no Online lobby
 * hooks.
 * web/tools/corelink.sh links such a build. */
#include "cpu.h"
#include "game.h"
#include "widescreen.h"
#include "carsfx.h"
#include "mprace.h"
#include <stdint.h>

#define WEAK __attribute__((weak))

/* ---- no game: nothing to run (main_web.c refuses to start without an entry) */
WEAK const GameOps g_game = {
    .id = "none",
    .exe_name = "GAME.EXE",
    .vroot = "C:\\GAME",
    .module_path = "C:\\GAME\\GAME.EXE",
};

/* ---- widescreen.c: the game's aspect is the device's */
WEAK void   ws_set_mapping(float k){ (void)k; }
WEAK float  ws_persp_x(void){ return 1.f; }
WEAK float  ws_persp_y(void){ return 1.f; }
WEAK float  ws_ui_x(void){ return 1.f; }
WEAK float  ws_ui_y(void){ return 1.f; }
WEAK double ws_render_factor(void){ return 1.0; }
WEAK double ws_max_pixels(void){ return WS_MAX_PIXELS_DEFAULT; }
WEAK int    ws_fill_target(void){ return 0; }
WEAK int    ws_take_border_quad(void){ return 0; }
WEAK void   ws_report(void){ }

/* ---- framerate.c: the game's own rate, the virtual clock's 4 ms floor */
WEAK int      hfr_display_live(void){ return 1; }
WEAK int      hfr_mode_off(void){ return 1; }
WEAK int      hfr_mailbox(void){ return 0; }
WEAK void     hfr_note_vsync(double ts, double dur){ (void)ts; (void)dur; }
WEAK double   hfr_race_start_ms(void){ return -1; }
WEAK double   hfr_sim_ms(void){ return 0; }
WEAK uint64_t hfr_vclock_floor_us(void){ return 4000u; }
WEAK void     hfr_set_display_hz(int hz){ (void)hz; }
WEAK void     hfr_report(void){ }

/* ---- d3dx_hle.c: no D3DX of the exe's own, no effect statistics */
int      g_efx_stat WEAK;
int      g_dx_in_pass WEAK;
WEAK int  efx_watched(uint32_t a){ (void)a; return 0; }
WEAK void efx_dispatch(CPU *c, uint32_t addr){ (void)c; (void)addr; }
WEAK void guest_cpu_mark(void){ }
WEAK void d3dx_hle_report(void){ }

/* ---- headlights.c: other players' headlights are not drawn */
WEAK void headlights_vs_ramp(float s, float o){ (void)s; (void)o; }
WEAK int  headlights_set(const char *mode){ (void)mode; return 0; }
WEAK void headlights_report(void){ }

/* ---- overrides.c, language.c */
WEAK void overrides_report(void){ }
WEAK const char *language_registry(void){ return 0; }      /* "Language" not found: the game's default */

/* ---- career.c, roam.c, photo.c: test script commands */
WEAK int      career_is_cmd(const char *cmd){ (void)cmd; return 0; }
WEAK int      career_cmd(CPU *c, const char *line, long *wait_start){ (void)c; (void)line; (void)wait_start; return 1; }
WEAK int      roam_cmd(CPU *c, const char *line, long *wait_start){ (void)c; (void)line; (void)wait_start; return 1; }
WEAK int      photo_script(const char *s){ (void)s; return 0; }    /* "photo ...": not known */

/* ---- free roam with friends (roam_game.c, mprace.c), its sounds (carsfx.c) */
WEAK int  roam_game_racing(void){ return 0; }
WEAK void roam_game_event(int ev, int peer, const char *text){ (void)ev; (void)peer; (void)text; }
WEAK void roam_game_parked(int on){ (void)on; }
WEAK void roam_game_report(void){ }
WEAK int  mprace_owns_input(void){ return 0; }
WEAK void carsfx_mix(float *acc, unsigned frames, unsigned rate){ (void)acc; (void)frames; (void)rate; }

/* ---- pad_ug2.c: no player input controller for the scripts' "@in+OFF"
 * peeks (the game pad itself is core's dinput_pad.c) */
WEAK uint32_t dipad_input_ctx(void){ return 0; }

/* ---- garage.c: no career garage's slots to keep beside a saved or deleted
 * profile (fileio.c) */
WEAK void garage_profile_written(const char *path){ (void)path; }
WEAK void garage_profile_deleted(const char *path){ (void)path; }

/* ---- online.c, olfilter.c: the Online lobby's traffic, as it is */
WEAK void online_lobby_read(uint32_t link, const uint8_t *p, int n){ (void)link; (void)p; (void)n; }
WEAK void olfilter_lobby_mark(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }
WEAK void olfilter_lobby_sent(uint32_t link, uint8_t *p, int n){ (void)link; (void)p; (void)n; }
