/* The distance to the GPS destination under its arrow (settings.ini gps_distance, the Mods tab's GPS Distance; off at
 * first): in a straight line, not by road, in m/km or ft/mi as Speedometer Units has it, under the arrow of each
 * player on this computer whose HUD has one, from their own car (co-op's player 2's HUD has none: the GPS is player
 * 1's in the game; tests/scripts/gps_distance_coop.txt).  The text is a copy of the HUD's ONLINE_PLAYER_1 named
 * GPS_DISTANCE (roam_game_clone_hud_text), the HUD package's own: freed with it, and looked up by its name every
 * frame, so no pointer outlives a HUD (shops, races, pause).  "[gps]" log line when one is made. */
#include "cpu.h"
#include "splitscreen_mem.h"
#include "gpsdistance_format.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

void dispatch_indirect(CPU *, uint32_t);
uint32_t roam_game_clone_hud_text(CPU *, uint32_t, uint32_t, const char *);

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
    /* settings.ini gps_distance; fixed for this launch, off by default. */
    static int enabled = -1;
    if (enabled < 0){
        const char *v = getenv("NFSU2_GPS_DISTANCE");
        enabled = v && (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "true"));
    }
    if (!enabled) return;
    /* The destination is shared; distance and arrow visibility belong to
     * the Player whose HUD has just updated. Some HUDs have no GPS part. */
    uint32_t pl = c->esi;
    unsigned seat = 0;
    while (pl && seat < SPLIT_PORT_CAPACITY && MR32(SPLIT_PLAYERS_INDEX + seat * 4u) != pl) ++seat;
    if (!pl || seat == SPLIT_PORT_CAPACITY) return;
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
    int made = !obj;
    if (!obj){
        strcpy((char *)g_mem + scratch, "ONLINE_PLAYER_1");
        args[1] = one(c, 0x505450u, 0, scratch);
        uint32_t source = call(c, 0x537a10u, 0, 2, args);
        if (!source) return;
        /* Use the same ownership-checked clone path as multiplayer HUD labels.
         * GPS_SEARCH_Text belongs to the native error/search group and
         * Distance_Number to outrun; neither is an unused navigation label. */
        obj = roam_game_clone_hud_text(c, pkg, source, "GPS_DISTANCE");
        if (!obj) return;
        MR32(obj + 0x6cu) = (MR32(obj + 0x6cu) & ~0xfu) | 5u; /* centred */
        MR32(obj + 0x1cu) |= 0x100u;
        for (int i = 0; i < 4; ++i) MR32(scratch + 4u * i) = 255;
        uint32_t av[2] = {scratch, 0};
        call(c, 0x517e20u, obj, 2, av);
        MRF32(scratch) = 0.f; MRF32(scratch + 4u) = -55.f; MRF32(scratch + 8u) = 0.5f;
        call(c, 0x517c10u, obj, 2, av);
        MRF32(scratch) = MRF32(scratch + 4u) = 0.7f; MRF32(scratch + 8u) = 1.f;
        call(c, 0x517db0u, obj, 2, av);
    }
    char text[32];
    gpsdistance_format(text, sizeof text, distance, MR8(0x83aa1au) == 1);
    /* FEString's setter only runs when the displayed distance changes. Its
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
    if (made) fprintf(stderr, "[gps] distance under the arrow for player %u: %s\n", seat + 1, text);
}
