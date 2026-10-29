/* optslider.h -- sliders of our own on the game's option screens
 * (optslider.c): Horn Volume (hornmenu.c), Exposure and White Level
 * (tonemenu.c) */
#ifndef OPTSLIDER_H
#define OPTSLIDER_H
#include "cpu.h"

typedef struct {
    const char *name;                    /* the log's ("Horn Volume") */
    float min, max, step;                /* its range; a left or right moves it a step */
    float (*value)(void);                /* where it stands as its screen opens */
    void  (*moved)(float v);             /* the player moved it to v, a step of its range */
    const char *(*label)(void);          /* its text in the game's language */
    uint32_t vt;                         /* (optslider.c) its class in guest memory */
} OptSlider;

/* an entry for s on the option screen scr, made as the screen is set up;
 * fn_add: the screen's list's (0x54F410, the pause menu's 0x54F800) */
void optslider_add(CPU *c, uint32_t scr, uint32_t fn_add, OptSlider *s);
/* the main loop's, every frame: the copied parts take their originals' colours */
void optslider_frame(CPU *c);
#endif
