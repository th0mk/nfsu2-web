/* tonemenu.c -- Exposure and White Level: two sliders on the game's Display
 * options, after Level Of Detail (the menus' Options > Display, 0x4B7180,
 * and the pause menu's, 0x4C36C0), for eyes the picture's brightness tires,
 * as qUINT Lightroom's sliders through ReShade on the PC game.  The page's last pass applies them to the whole picture, menus and
 * HUD included (gpu_web.c gw_tone, every frame's GwTone; web/src/gpu):
 *   Exposure     the colour times 2^exposure, -1 .. +1 in steps of 0.1 (as
 *                qUINT's: half to twice as bright); 0: as the game draws it
 *   White Level  the brightest the picture gets, 40 % .. 100 % in steps of
 *                5 %: the top fifth below it bends down to it (a soft
 *                shoulder from KNEE of it); 100 %: as the game draws it
 * A step shows at once (the page draws every frame with them); a frame dump
 * and a screenshot keep the colours as drawn.  Both are the registry's
 * (registry.txt: the device's, as the game's own video options; cloud saves
 * leave it out), read at startup: Exposure in hundredths of a stop, signed
 * (stored as the game stores FSAA's -1), WhiteLevel in percent; none: 0 and
 * 100.  The sliders themselves: optslider.c.  Web only: the Mac renderer
 * has no such pass. */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */

#ifdef __wasm__
#include "optslider.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int  win32_reg_get_dword(const char *nm, uint32_t *out);
void win32_reg_put_dword(const char *nm, uint32_t v);
const char *language_name(void);
void gw_tone(float mult, float white, float knee);  /* runtime/web/gpu_web.c */

#define REG_EXPOSURE "Exposure"
#define REG_WHITE    "WhiteLevel"
#define FN_ADD       0x0054f410u    /* thiscall screen (entry, 1): the Options list takes a slider */
#define KNEE         0.8f           /* White Level's shoulder starts at this part of it */

static int g_installed;
static int32_t  g_exp = 0;                       /* Exposure, hundredths of a stop */
static uint32_t g_white = 100;                   /* White Level, percent */

static void apply(void){ gw_tone(exp2f((float)g_exp / 100.f), (float)g_white / 100.f, KNEE); }

/* the labels in the game's language, as hornmenu.c's (Latin-1 where the
 * language's own texts have those letters: German's ß, Swedish's å, Spanish's
 * ó, the Portuguese pack's ç, ã, í); English for the others */
static const char *label_of(int white){
    static const struct { const char *lang, *exposure, *white; } T[] = {
        { "french", "Exposition", "Niveau de blanc" }, { "german", "Belichtung", "Wei\xdfpegel" },
        { "italian", "Esposizione", "Livello del bianco" }, { "spanish", "Exposici\xf3n", "Nivel de blanco" },
        { "dutch", "Belichting", "Witniveau" }, { "swedish", "Exponering", "Vitniv\xe5" },
        { "danish", "Eksponering", "Hvidniveau" }, { "portuguese", "Exposi\xe7\xe3o", "N\xedvel de Branco" },
        { "polish", "Ekspozycja", "Poziom bieli" }, { "turkish", "Pozlama", "Beyaz Seviyesi" },
    };
    const char *l = language_name();
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(l, T[i].lang)) return white ? T[i].white : T[i].exposure;
    return white ? "White Level" : "Exposure";
}
static const char *exp_label(void){ return label_of(0); }
static const char *white_label(void){ return label_of(1); }
static float exp_value(void){ return (float)g_exp / 100.f; }
static float white_value(void){ return (float)g_white / 100.f; }
static void exp_moved(float v){
    int32_t h = (int32_t)lrintf(v * 100.f);
    if (h < -100) h = -100;
    if (h > 100) h = 100;
    if (h == g_exp) return;
    g_exp = h;
    win32_reg_put_dword(REG_EXPOSURE, (uint32_t)h);
    fprintf(stderr, "[tone] exposure %+.1f\n", h / 100.0);
    apply();
}
static void white_moved(float v){
    uint32_t pct = (uint32_t)lrintf(v * 100.f);
    if (pct < 40u) pct = 40u;
    if (pct > 100u) pct = 100u;
    if (pct == g_white) return;
    g_white = pct;
    win32_reg_put_dword(REG_WHITE, pct);
    fprintf(stderr, "[tone] white level %u %%\n", pct);
    apply();
}
static OptSlider g_exposure = { "Exposure", -1.f, 1.f, 0.1f, exp_value, exp_moved, exp_label, 0 };
static OptSlider g_whitelv  = { "White Level", 0.4f, 1.f, 0.05f, white_value, white_moved, white_label, 0 };

/* the registry's Exposure and White Level to the renderer (once) */
void tonemenu_install(void){
    if (g_installed) return;
    g_installed = 1;
    uint32_t v;
    if (win32_reg_get_dword(REG_EXPOSURE, &v)){
        int32_t h = (int32_t)v;
        g_exp = h < -100 ? -100 : h > 100 ? 100 : h;
    }
    if (win32_reg_get_dword(REG_WHITE, &v)) g_white = v < 40u ? 40u : v > 100u ? 100u : v;
    if (g_exp || g_white != 100u) fprintf(stderr, "[tone] exposure %+.1f, white level %u %% (registry)\n", g_exp / 100.0, g_white);
    apply();
}

int tonemenu_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x004b725a): break;           /* Options > Display: after Level Of Detail, before Customize Display Settings */
    case HOOK_SITE(0x004c3835): break;           /* the pause menu's */
    default: return 0;
    }
    tonemenu_install();
    optslider_add(c, c->esi, FN_ADD, &g_exposure);
    optslider_add(c, c->esi, FN_ADD, &g_whitelv);
    return 1;
}
#else
int tonemenu_hook(CPU *c, uint32_t ip){ (void)c; (void)ip; return 0; }
#endif
