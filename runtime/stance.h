/* stance.h -- a car's stance (stance.c): its values, as the menu sets them, the saves keep them (modpacks.c's
 * MODPARTS.DAT) and the car record carries them to friends (roam_core.c RC_EXT_STANCE) */
#ifndef NFSU2_STANCE_H
#define NFSU2_STANCE_H
#include <stdint.h>

typedef struct {
    int16_t  fc, rc;                 /* camber, front and rear: tenths of a degree, the wheels' tops in */
    int16_t  fo, ro;                 /* offset, front and rear: mm, the wheels out (below 0: in) */
    uint16_t tw;                     /* the tires' width, per cent of the car's own */
} StanceV;
#define STANCE_CAMBER_MIN 0
#define STANCE_CAMBER_MAX 150        /* 15 degrees, as the Stance Mod's .ini allows */
#define STANCE_OFFSET_MIN (-40)
#define STANCE_OFFSET_MAX 80
#define STANCE_TIRE_MIN   80
#define STANCE_TIRE_MAX   130
#define STANCE_CARS       64         /* a player's cars with a stance of their own (20 Customize, 10 career, presets) */
#define STANCE_FRIENDS    64         /* roam_game.c's slots */

#define STANCE_KIND       4u         /* MODPARTS.DAT's entry kind (modpacks.c) */

int  stance_on(void);                                 /* settings.ini stance */
int  stance_ok(const StanceV *v);                     /* every value in its range */
const StanceV *stance_mine(int p);                    /* player p's car's, 0: none of its own */
int  stance_wire_mine(int16_t v[5]);                 /* roam_game.c: RC_EXT_STANCE */
void stance_wire_car(int k, uint32_t car, int has, const int16_t v[5]);
void stance_forget(void);
/* modpacks.c, MODPARTS.DAT: player p's stances as its entries (8 words: handle, car type, the cambers, the offsets,
 * the tires' width, 0, 0, STANCE_KIND) -> how many; a profile of player p (its name) loaded: none until stance_load
 * gives each of its file's (1 taken); a car gone (traded), a car moved to another record */
int  stance_entries(int p, uint32_t (*out)[8], int max);
void stance_loaded(int p, const char *name);
int  stance_load(int p, const uint32_t w[8]);
void stance_car_gone(int p, uint32_t h);
void stance_car_moved(int p, uint32_t hf, uint32_t ht);
void stance_deleted(const char *name);
int  stance_has(int p, uint32_t h);                   /* modpacks.c: the car of handle h has a stance set here */
uint32_t modparts_car_type(int p, uint32_t r);        /* modpacks.c: the car type record r is saved with */                /* modpacks.c: that profile deleted: its stances go */
void stance_split_keep(void);                         /* garage.c: split screen begins: player 1's stances aside ... */
void stance_split_back(void);                         /* ... and back at its end, as its cars come back */
#endif
