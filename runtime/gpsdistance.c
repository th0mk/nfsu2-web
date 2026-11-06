/* Distance to the GPS destination under the direction arrow. A single native
 * FE string belongs to the HUD package and is freed with it. Look it up in the
 * current package, rather than retaining pointers across shops/races. This is
 * straight-line horizontal distance, in world metres, not road-route length. */
#include "cpu.h"
#include "splitscreen_mem.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void dispatch_indirect(CPU *, uint32_t);

static uint32_t call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args){
    CPU saved = *c;
    c->esp = (c->esp - 512u) & ~15u;
    for (int i = n - 1; i >= 0; --i) PUSH32(args[i]);
    c->ecx = self;
    PUSH32(fn);
    dispatch_indirect(c, fn);
    uint32_t result = c->eax;
    *c = saved;
    return result;
}
static uint32_t one(CPU *c, uint32_t fn, uint32_t self, uint32_t arg){ return call(c, fn, self, 1, &arg); }

void gpsdistance_hud(CPU *c){
    /* GPS is shared in co-op, as is the game's direction arrow. */
    uint32_t pl = MR32(SPLIT_PLAYERS_INDEX);
    if (!pl || c->esi != pl) return;
    uint32_t hud = MR32(pl + 0x9a4u), car = MR32(pl + 4u);
    if (!hud) return;
    uint32_t pkg = MR32(hud + 0x10u), part = MR32(hud + 0x5cu);
    if (!pkg || !part) return;
    /* Scratch is above the guest-call stack, never kept by a setter. */
    uint32_t scratch = (c->esp - 256u) & ~15u;
    static uint32_t hash;
    if (!hash){
        strcpy((char *)g_mem + scratch, "GPS_DISTANCE");
        hash = one(c, 0x505450u, 0, scratch);
    }
    uint32_t args[2] = {pkg, hash};
    uint32_t obj = call(c, 0x537a10u, 0, 2, args);
    int on = car && MR8(0x8385b9u) && !MR32(0x89e7b0u) &&
        ((MR32(part + 0x10u) & MR32(part + 0x18u)) ||
         (MR32(part + 0x14u) & MR32(part + 0x1cu)));
    /* GPS::GetState (0x501100): 1/2 report no satellite/path, 4 is off.
     * Both 0 and 3 are active navigation states; 3 is a completed route
     * search, not arrival at the destination. */
    if (on){
        uint32_t state = call(c, 0x501100u, 0x838590u, 0, NULL);
        on = state == 0 || state == 3;
    }
    if (!on){ if (obj) one(c, 0x50ca00u, 0, obj); return; }
    float dx = MRF32(0x8385a0u) - MRF32(car + 0x50u);
    float dy = MRF32(0x8385a4u) - MRF32(car + 0x54u);
    float distance = hypotf(dx, dy);
    if (!isfinite(distance) || distance >= 1000000.f){
        if (obj) one(c, 0x50ca00u, 0, obj);
        return;
    }
    if (!obj){
        strcpy((char *)g_mem + scratch, "ONLINE_PLAYER_1");
        args[1] = one(c, 0x505450u, 0, scratch);
        uint32_t source = call(c, 0x537a10u, 0, 2, args);
        uint32_t pack = one(c, 0x52cef0u, 0, pkg);
        if (!source || !pack) return;
        obj = one(c, MR32(MR32(source) + 4u), source, 1); /* FEString::Clone */
        if (!obj) return;
        uint32_t len = MR32(source + 0x30u), from = MR32(source + 0x2cu), to = MR32(obj + 0x2cu);
        if (from && to && from != to && len <= 0x100u && len == MR32(obj + 0x30u))
            memcpy(g_mem + to, g_mem + from, len);
        MR32(obj + 0x58u) = 0; /* renderer makes this clone's render object */
        MR32(obj + 0x10u) = hash;
        MR32(obj + 0x6cu) = (MR32(obj + 0x6cu) & ~0xfu) | 5u; /* centred */
        MR32(obj + 0x1cu) |= 0x100u;
        uint32_t last = MR32(pack + 0x50u);
        MR32(obj + 4u) = 0; MR32(obj + 8u) = last;
        if (last) MR32(last + 4u) = obj; else MR32(pack + 0x4cu) = obj;
        MR32(pack + 0x50u) = obj; MR32(pack + 0x48u) += 1;
        for (int i = 0; i < 4; ++i) MR32(scratch + 4u * i) = 255;
        uint32_t av[2] = {scratch, 0};
        call(c, 0x517e20u, obj, 2, av);
        MRF32(scratch) = 0.f; MRF32(scratch + 4u) = -55.f; MRF32(scratch + 8u) = 0.5f;
        call(c, 0x517c10u, obj, 2, av);
        MRF32(scratch) = MRF32(scratch + 4u) = 0.7f; MRF32(scratch + 8u) = 1.f;
        call(c, 0x517db0u, obj, 2, av);
    }
    char text[32];
    snprintf(text, sizeof text, "%.0f m", (double)distance);
    /* FEString's setter only runs when the displayed integer changes. Its
     * UTF-16 buffer is owned by the object, so no stale host-side cache. */
    uint32_t str = MR32(obj + 0x64u);
    int same = str != 0;
    for (unsigned i = 0; same; ++i){
        if (MR16(str + i * 2u) != (unsigned char)text[i]) same = 0;
        if (!text[i]) break;
    }
    if (!same){
        strcpy((char *)g_mem + scratch, text);
        one(c, 0x535fe0u, obj, scratch);
        MR32(obj + 0x1cu) |= 2u;
    }
    one(c, 0x50ca50u, 0, obj);
}
