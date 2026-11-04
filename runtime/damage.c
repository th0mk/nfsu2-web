/* damage.c -- crash damage on the cars' own meshes: the visual part of FRS Mods'
 * "Crashing Foreign Cars" (Damage.asi, NFSU2 v1.2), ported to the browser build.
 *
 * The work is in damagecore.cpp (C++, as the .asi); this file is its link to the
 * game: the overrides and instruction hooks the lifter knows of (gen.sh greps the
 * runtime's .c files for them), and the guest calls the core makes (the game's
 * functions and its Direct3D device, run through the dispatcher on the CPU of the
 * hook that called the core).  The .asi's own hooks, and what stands in here:
 *
 *   CarRenderInfo::Render 0x620AE0 (detour)      showoff.c's override calls damage_render first: the
 *                                                  core's frame work and the bent wheels, then the game's
 *   CarCollisionBody::ApplyImpulse 0x5A0B00      override: the game's, then the impact (vtable slot
 *     (vtable slot +0x50)                          +0x50 checked, as the .asi checks the slot)
 *   CarDamage::SetDamageToZero 0x637D70 (detour) override: the repair, then the game's
 *   Render's open flags 0x620C2B (detour)        HOOK_SITE before the instruction
 *   Render's draws of the body, exhausts and     HOOK_SITE at each call (Body's matrix; the clone of
 *     plates (calls redirected)                    that tip or plate swapped in) and at the next
 *                                                  instruction (the original put back)
 *   UpdateCameraMovers call 0x581493 (main loop) the main-loop hook (roam.c): the texture pack
 *   (none: not in the .asi)                      HOOK_SITE in the car loader (0x63AFEF): the engine's
 *                                                  texture, <CAR>_ENGINE, loaded with the car's others, so the
 *                                                  engine a lost hood shows is not black (on Windows too)
 *
 * Only what is seen: dents, folds, loose and lost parts (lying on the road, the
 * doors hitting walls and cars), scratches, broken lights, the game's own window
 * cracks with ProStreet's broken glass, and bent wheels (drawn only).  Not ported (yet): the engine and suspension
 * damage, the health bar and "totaled", the career's saved damage and repairs, the
 * neon and the sounds.  The game's own repair (SetDamageToZero: a race's start, the
 * garage) takes the damage away.
 * In free roam friends see the player's damage (RC_EXT_DAMAGE in the car record, roam_game.c): the player's dents
 * are published (damage_net_*), and a friend's car gets the dents its player's record has.
 *   Off unless NFSU2_DAMAGE=1 (the start page's Crash damage, off by default); NFSU2_DAMAGE_LOG=1 logs every
 *   impact. */
#include "cpu.h"
#include "widescreen.h"           /* HOOK_SITE */
#include <stdio.h>
#include <stdlib.h>

void sub_00637d70_orig(CPU *c);   /* CarDamage::SetDamageToZero */
void sub_005a0b00_orig(CPU *c);   /* CarCollisionBody::ApplyImpulse */
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
uint32_t gheap_alloc(int heap, uint32_t n, int zero);
void gheap_free(int heap, uint32_t p);

/* damagecore.cpp */
int  dmg_init(void);
void dmg_render_entry(uint32_t cri, uint32_t args);
void dmg_impulse(uint32_t body, uint32_t impulse, uint32_t point);
void dmg_zero(uint32_t car_damage, uint32_t caller);
void dmg_open_flags(uint32_t cri, uint32_t frame);
void dmg_body_draw(uint32_t model, uint32_t l2w);
void dmg_marker_pre(int slot, uint32_t model, uint32_t l2w);
void dmg_marker_post(void);
void dmg_main_loop(void);
uint32_t dmg_net_rev(void);
int  dmg_net_mine(uint8_t *epoch, uint16_t *first, uint8_t *dents, int max);
void dmg_net_car(int k, uint32_t car, uint32_t peer, uint8_t epoch, uint16_t first, int n, const uint8_t *dents);
void dmg_car_textures(uint32_t used, uint32_t name);

#define APPLY_IMPULSE   0x005a0b00u
#define VT_APPLY_IMPULSE 0x50u        /* its vtable slot */

static CPU *g_c;                      /* the CPU of the hook the core runs under */
static int g_on = -1;

static int on(void){
    if (g_on < 0){
        const char *e = getenv("NFSU2_DAMAGE");
        g_on = e && *e && *e != '0' && dmg_init();   /* off unless asked for */
    }
    return g_on;
}

/* ---- for the core --------------------------------------------------------- */
uint32_t dmg_call(uint32_t fn, int n, const uint32_t *a){ return call_guest(g_c, fn, n, a); }
uint32_t dmg_thiscall(uint32_t fn, uint32_t self, int n, const uint32_t *a){
    uint32_t ecx = g_c->ecx;
    g_c->ecx = self;
    uint32_t r = call_guest(g_c, fn, n, a);
    g_c->ecx = ecx;
    return r;
}
/* a thiscall that returns a float in st0 (the topology's GetElevation): read before the CPU is put back */
void dispatch_indirect(CPU *c, uint32_t addr);
float dmg_thiscall_f(uint32_t fn, uint32_t self, int n, const uint32_t *a){
    CPU *c = g_c;
    CPU save = *c;
    c->ecx = self;
    for (int i = n - 1; i >= 0; i--){ c->esp -= 4u; MR32(c->esp) = a[i]; }
    c->esp -= 4u; MR32(c->esp) = 0xDEADBEEFu;   /* fake return address */
    dispatch_indirect(c, fn);
    const double r = c->st[c->top & 7];
    *c = save;
    return (float)r;
}
uint32_t dmg_alloc(uint32_t n){ return gheap_alloc(1, n, 1); }
void dmg_free(uint32_t p){ gheap_free(1, p); }
int dmg_committed(uint32_t a, uint32_t n){ return a && mem_is_committed(a) && mem_is_committed(a + n - 1u); }

/* ---- overrides ------------------------------------------------------------ */
/* CarRenderInfo::Render (0x620AE0), before the game's: showoff.c overrides it and calls this first */
void damage_render(CPU *c){
    if (on()){ g_c = c; dmg_render_entry(c->ecx, c->esp + 4u); }   /* its stack arguments: the bent wheels' matrices */
}
void sub_005a0b00(CPU *c){
    uint32_t body = c->ecx, impulse = MR32(c->esp + 4u), point = MR32(c->esp + 8u);
    sub_005a0b00_orig(c);
    if (!on() || !dmg_committed(body, 4) || !dmg_committed(MR32(body) + VT_APPLY_IMPULSE, 4) ||
        MR32(MR32(body) + VT_APPLY_IMPULSE) != APPLY_IMPULSE) return;
    g_c = c;
    dmg_impulse(body, impulse, point);
}
void sub_00637d70(CPU *c){
    if (on()){ g_c = c; dmg_zero(c->ecx, MR32(c->esp)); }
    sub_00637d70_orig(c);
}

/* ---- instruction hooks (debug.c guest_hook) -------------------------------- */
int damage_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x00620c2b):   /* Render, right after the four open flags: esi the CarRenderInfo */
        if (on()){ g_c = c; dmg_open_flags(c->esi, c->esp); }
        return 1;
    case HOOK_SITE(0x00624499):   /* the part loop's draw: thiscall view (model, l2w, ...) */
        if (on()){ g_c = c; dmg_body_draw(MR32(c->esp), MR32(c->esp + 4u)); }
        return 1;
    case HOOK_SITE(0x00624c52):   /* an exhaust tip's draw */
        if (on()){ g_c = c; dmg_marker_pre(16, MR32(c->esp), MR32(c->esp + 4u)); }
        return 1;
    case HOOK_SITE(0x00625a27):   /* a plate's draw */
        if (on()){ g_c = c; dmg_marker_pre(33, MR32(c->esp), MR32(c->esp + 4u)); }
        return 1;
    case HOOK_SITE(0x0063afef):   /* a car's textures, before the loader asks for them: ebx the list, ebp the car's name */
        if (on()){ g_c = c; dmg_car_textures(c->ebx, c->ebp); }
        return 1;
    case HOOK_SITE(0x00624c57):   /* back from those draws */
    case HOOK_SITE(0x00625a2c):
        if (on()){ g_c = c; dmg_marker_post(); }
        return 1;
    }
    return 0;
}

/* ---- free roam (roam_game.c through roam_host.h): friends see the damage ------- */
/* the player's car's damage as published (+1 for each change), and its dents (RC_EXT_DAMAGE's) */
uint32_t damage_net_rev(void){ return on() ? dmg_net_rev() : 0u; }
int damage_net_mine(uint8_t *epoch, uint16_t *first, uint8_t *dents, int max){
    return on() ? dmg_net_mine(epoch, first, dents, max) : 0;
}
/* slot k's friend's car (0: none shown) and the dents its player's record has: made on it */
void damage_net_car(CPU *c, int k, uint32_t car, uint32_t peer, uint8_t epoch, uint16_t first, int n, const uint8_t *dents){
    if (!on()) return;
    g_c = c;
    dmg_net_car(k, car, peer, epoch, first, n, dents);
}

/* roam.c's main-loop hook, once a frame */
void damage_frame(CPU *c){
    if (!on()) return;
    g_c = c;
    dmg_main_loop();
}
