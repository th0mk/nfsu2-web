/* hornmenu.c -- the horns' volume (horn.c) on the game's own Audio options:
 * a Horn Volume slider after Engine Volume, in the menus' Options > Audio
 * (0x4B6E60, before its Audio Mode) and in the pause menu's (0x4C3370, where
 * Engine Volume is the last), from 10 % to 100 % in steps of 5 %.  Moving it
 * sounds the player's own horn for a moment at the new volume.  The value is
 * the registry's HornVolume (percent; registry.txt, the game's options),
 * read at startup; none: 100 %.  The slider itself: optslider.c (the menus'
 * UI_Options.fng has the parts for an eighth slider, the pause menu's
 * UI_PauseOptions.fng all but the bar's, which are copies of the seventh's). */
#include "cpu.h"
#include "horn.h"
#include "optslider.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <math.h>
#include <stdio.h>
#include <string.h>

int  win32_reg_get_dword(const char *nm, uint32_t *out);
void win32_reg_put_dword(const char *nm, uint32_t v);
const char *language_name(void);

#define REG_NAME      "HornVolume"
#define FN_ADD        0x0054f410u   /* thiscall screen (entry, 1): the Options list takes a slider */
#define FN_ADD_PAUSE  0x0054f800u   /* thiscall screen (entry, 1): the pause menu's */

static int g_installed;
static uint32_t g_pct = 100;                     /* the volume, percent, as the registry has it */

/* the label in the game's language: the Latin-script texts as the language
 * files write their own sliders' ("Volume moteur", "Motorvolume", ...; in
 * Latin-1, as they are; Polish's "Glosnosc silnika" with the letters its
 * pack's fonts draw as Polish ones: l-stroke 0xEE, s-acute 0xE2, c-acute
 * 0xE9; Turkish's "Korna Sesi" after its "Motor Sesi", ASCII); English for
 * the others, whose fonts have their own encodings but draw ASCII as it is
 * (car names) */
static const char *label(void){
    static const struct { const char *lang, *text; } T[] = {
        { "french", "Volume klaxon" }, { "german", "Hupenlautst\xe4rke" }, { "italian", "Volume clacson" },
        { "spanish", "Volumen claxon" }, { "dutch", "Claxonvolume" }, { "swedish", "Tutvolym" },
        { "danish", "Lydstyrke for horn" }, { "portuguese", "Volume da Buzina" },
        { "polish", "G\xeeo\xe2no\xe2\xe9 klaksonu" }, { "turkish", "Korna Sesi" },
    };
    const char *l = language_name();
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(l, T[i].lang)) return T[i].text;
    return "Horn Volume";
}
static float value(void){ return (float)g_pct / 100.f; }
/* the volume to the horns, the registry and a honk */
static void moved(float v){
    uint32_t pct = (uint32_t)lrintf(v * 100.f);
    if (pct < 10u) pct = 10u;
    if (pct > 100u) pct = 100u;
    horn_set_volume((float)pct / 100.f);
    if (pct != g_pct){ g_pct = pct; win32_reg_put_dword(REG_NAME, pct); fprintf(stderr, "[horn] volume %u %%\n", pct); }
    horn_preview();
}
static OptSlider g_slider = { "Horn Volume", HORN_VOL_MIN, 1.f, 0.05f, value, moved, label, 0 };

/* the registry's volume to the horns (once) */
void hornmenu_install(void){
    if (g_installed) return;
    g_installed = 1;
    uint32_t pct;
    if (win32_reg_get_dword(REG_NAME, &pct)){
        g_pct = pct < 10u ? 10u : pct > 100u ? 100u : pct;
        horn_set_volume((float)g_pct / 100.f);
        fprintf(stderr, "[horn] volume %u %% (registry %s)\n", g_pct, REG_NAME);
    }
}

int hornmenu_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x004b7102): hornmenu_install(); optslider_add(c, c->esi, FN_ADD, &g_slider); return 1;          /* Options > Audio: after Engine Volume, before Audio Mode */
    case HOOK_SITE(0x004c36a3): hornmenu_install(); optslider_add(c, c->esi, FN_ADD_PAUSE, &g_slider); return 1;    /* the pause menu's: after Engine Volume */
    }
    return 0;
}
