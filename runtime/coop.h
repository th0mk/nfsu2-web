/* Co-op free roam: player 2 in player 1's career free roam (coop.c). */
#ifndef NFSU2_COOP_H
#define NFSU2_COOP_H
#include <stdint.h>
#define COOP_DEV_KB 6            /* player 2's device: the keyboard's WASD half (else a local pad seat 0..5) */
int coop_in(void);               /* player 2 is in this world (flow 6) */
int coop_wanted(void);           /* co-op is on (player 2 in, or back after the next world load) */
int coop_driving(void);          /* player 2 in and player 1's HUD in front (no menu or dialog over it) */
int coop_racing_together(void);  /* a career race player 2 races in (two local humans) */
int coop_device(void);           /* player 2's device while co-op is on, else -1 */
uint32_t coop_player(void);      /* player 2's Player (0: none) */
uint32_t coop_p2_car(void);
void coop_join(int dev);
void coop_leave(void);
uint32_t coop_fe_top(uint32_t fe_manager);   /* the top front-end package as player 1 sees it (player 2's HUD skipped) */
#endif
