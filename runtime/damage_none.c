/* damage_none.c -- the crash damage (damage.c, damagecore.cpp) where it is not built: the Mac's link.sh.
 * damagecore.cpp reads guest addresses as pointers, which only the browser build can; here the game's two
 * functions damage.c overrides run as they are, and its hook and free-roam calls do nothing. */
#include "cpu.h"
#include <stdint.h>

void sub_00637d70_orig(CPU *c);   /* CarDamage::SetDamageToZero */
void sub_005a0b00_orig(CPU *c);   /* CarCollisionBody::ApplyImpulse */
void sub_005a0b00(CPU *c){ sub_005a0b00_orig(c); }
void sub_00637d70(CPU *c){ sub_00637d70_orig(c); }
int damage_hook(CPU *c, uint32_t ip){ (void)c; (void)ip; return 0; }
void damage_render(CPU *c){ (void)c; }
void damage_frame(CPU *c){ (void)c; }
int damage_net_on(void){ return 0; }
uint32_t damage_net_rev(void){ return 0; }
int damage_net_mine(uint8_t *epoch, uint16_t *first, uint8_t *dents, int max){
    (void)epoch; (void)first; (void)dents; (void)max; return 0; }
void damage_net_car(CPU *c, int k, uint32_t car, uint32_t peer, uint8_t epoch, uint16_t first, int n, const uint8_t *dents){
    (void)c; (void)k; (void)car; (void)peer; (void)epoch; (void)first; (void)n; (void)dents; }
int damage_part_lost(uint32_t ri, int part){ (void)ri; (void)part; return 0; }
