/* featurelist.h -- the runtime's features as features.c lists them: what each
 * installs at boot, its instruction hooks, its calls every frame; with the
 * prototypes of every function those lists name.  On wasm a call through a
 * declaration that does not match its definition links and traps when it
 * runs (web/build.sh warns of it): these are the definitions' own.
 * Not named features.h: wasi-libc's headers include <features.h>, and the
 * builds put runtime/ on the include path. */
#ifndef NFSU2_FEATURELIST_H
#define NFSU2_FEATURELIST_H
#include "cpu.h"

/* features.c */
void features_install(void);               /* at boot, after win32_init (tests/boot.c, web/main_web.c) */
int  features_hook(CPU *c, uint32_t ip);   /* an instruction hook (debug.c guest_hook): 1 when a feature's took it */
void features_frame(CPU *c);               /* every frame, from the main loop's hook (roam.c) */

/* ---- installs ------------------------------------------------------------ */
void overrides_install(void);
void language_install(void);
void sponsors_install(void);
void lobbyclick_install(void);
void menusize_install(void);
void olfilter_install(void);
void tunegraph_install(void);
void dyno_install(void);
void regional_install(void);
void garage_install(void);
void vinylpacks_install(void);
void specialties_install(void);
void stance_install(void);
void showoff_install(void);
void carshadow_install(void);
void hornmenu_install(void);
void tonemenu_install(void);
void powerunits_install(void);

/* ---- instruction hooks: 1 when the address is the hook's ------------------- */
int splitscreen_hook(CPU *c, uint32_t ip);
int coop_hook(CPU *c, uint32_t ip);
int ws_hook(CPU *c, uint32_t ip);
int roam_hook(CPU *c, uint32_t ip);
int language_hook(CPU *c, uint32_t ip);
int hornmenu_hook(CPU *c, uint32_t ip);
int tonemenu_hook(CPU *c, uint32_t ip);
int powerunits_hook(CPU *c, uint32_t ip);
int shownames_hook(CPU *c, uint32_t ip);
int hudbank_hook(CPU *c, uint32_t ip);
int regional_hook(CPU *c, uint32_t ip);
int garage_hook(CPU *c, uint32_t ip);
int vinylcat_hook(CPU *c, uint32_t ip);
int stance_hook(CPU *c, uint32_t ip);
int sndlead_hook(CPU *c, uint32_t ip);
int drift_hook(CPU *c, uint32_t ip);
int quickrace_hook(CPU *c, uint32_t ip);
int trafficcars_hook(CPU *c, uint32_t ip);
int travel_hook(CPU *c, uint32_t ip);
int damage_hook(CPU *c, uint32_t ip);

/* ---- every frame --------------------------------------------------------- */
void damage_frame(CPU *c);
void reads_frame(CPU *c);
void intro_frame(CPU *c);
void language_texts_frame(CPU *c);
void sms_frame(CPU *c);
void hiddenraces_frame(void);
void visualrep_frame(void);
void whine_frame(void);
void carspot_frame(void);
void walls_frame(void);
void travel_frame(CPU *c);
void mprace_frame(CPU *c);
void mpcredit_frame(CPU *c);
int  roam_asipeer_frame(CPU *c);           /* 1: the Windows plugin under Unicorn took roam_game_frame's place */
void roam_game_frame(CPU *c);
void racehorn_frame(CPU *c);
void racenet_frame(CPU *c);
void optslider_frame(CPU *c);
void stance_frame(CPU *c);
void drift_frame(CPU *c);
void cameras_frame(CPU *c);
void showoff_frame(CPU *c);
void plates_frame(CPU *c);
void lightkey_frame(CPU *c);
void photo_frame(CPU *c);
void savegame_frame(void);
void coop_frame(CPU *c);
void engage_frame(CPU *c);
void roam_frame(CPU *c);
#endif
