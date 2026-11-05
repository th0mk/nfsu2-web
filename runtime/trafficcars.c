/* trafficcars.c -- a traffic vehicle as the player's own car (a save edited to drive the fire truck, the bus, a
 * van ...): its body on the player's own screen, not only its lights and its shadow.  Friends always saw it.
 *
 * The game (SPEED2.EXE):
 *  - CarTypeInfo ([0x8A1CCC], 0x890 bytes each) +0x844 the type's use: 0 for the 31 player cars, 2 for the 15
 *    traffic vehicles (TAXI .. HATCHBACK02, types 31 .. 45).
 *  - The race's cars (GRaceParameters 0x89E7A0: [+0x70] 0x940 bytes each, [0x89E814] of them; 0x5ED900 the i-th):
 *    +1 the racer's index, +4 its kind (1 a player of this game, 2 another racer, 3 traffic: free roam's 16 at racers
 *    50 .. 65), +0x10 its RideInfo (+0 the type), +0x8F8 its CarLoader handle, +0x908 the traffic model it shares.
 *  - The world's load streams each one's ride (0x57F680 .., CarLoader::Load 0x63BC70) but a traffic type's
 *    (0x57F6A2); the traffic manager takes those instead (0x4201A2 -> 0x413440, 0x412C7C: +0x908), and draws the
 *    models it streams only on its own traffic.  The player's car of a traffic type is made all the same, with its
 *    lights and its shadow, but no model of its own is ever loaded: nothing of its body is drawn.
 *  - A friend's game loads the car as any friend's (roam_game.c: CarLoader::Load on its ride), and the loader and
 *    the car's render info take a traffic type as they should (the whole GEOMETRY.BIN at once, no vinyls).
 * Here a racer's entry (kind 1 or 2) is a car of its own at those three checks: the type's use is read as type 0's.
 * Traffic (kind 3) stays the traffic manager's, and every player car is as before.
 *
 * The camera behind such a vehicle: CarTypeInfo +0x720 the count of its camera spots, +0x724 split screen's, +0x730
 * and +0x790 the spots (0x10 bytes: s16 the camera kind, u16 its pitch, float forward, height, sideways); 0x5ECCA0
 * (the car's spot of a kind, 0x441140) is how the cameras find them.  Kinds 0 and 1 are the chase views (C's views
 * 3 and 2), 5 the drift camera (view 4); bumper and hood (2, 4) have a camera of their own.  Every traffic type has
 * a small car's spots (5.8 m behind, 1.7 m up), so the camera is inside the bus and right behind the fire truck,
 * below its roof.  The game's own SUVs have spots of their own (the Hummer 6.7 m and 2.3 m).  Here the big traffic
 * types' chase and drift spots are moved back and up by the vehicle's size against a taxi's, the larger of its
 * length behind the middle (the taxi 2.30 m) and its height (1.38 m), from each body's box (CARS\<type>\
 * GEOMETRY.BIN, <type>_BODY_A): the view as a car's, scaled; the pitch stays.  Only the cameras read the spots, and
 * the player's 31 cars keep theirs.
 *
 * Test knobs (with NFSU2_INPUT_SCRIPT only): NFSU2_TEST_TRAFFIC_CAR=<type name> makes the player's car that type
 * (stock parts) at the world's load; NFSU2_TEST_TRAFFIC_GAME=1 leaves the three checks as the game has them. */
#include "cpu.h"
#include "widescreen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define G_CARTYPES      0x008a1cccu
#define CT_SIZE         0x890u
#define CT_USE          0x844u
#define USE_TRAFFIC     2u
#define N_TYPES         46u
#define RC_KIND         0x4u          /* the race's cars (GRaceParameters 0x89E7A0 +0x70), 0x940 bytes each */
#define RC_RIDE         0x10u
#define KIND_PLAYER     1u
#define KIND_RACER      2u
#define FN_RIDE_INIT    0x00610270u   /* thiscall (type, 1, 0, 0) */
#define FN_RIDE_STOCK   0x00637040u   /* thiscall (0) */
#define FN_RIDE_SKINS   0x0061c280u   /* thiscall (n) */
#define CALL_RET        0x00581475u

#define CT_SPOTS        0x720u        /* the count; +4 split screen's; the spots at +0x10 and +0x70 */
#define SPOT_SIZE       0x10u
#define SPOTS_MAX       6u
static const struct { const char *type; float s; } BIG[] = {
    { "BUS", 2.77f }, { "FIRETRUCK", 1.93f }, { "PARCELVAN", 1.57f }, { "AMBULANCE", 1.46f },
    { "PANELVAN", 1.09f }, { "SUV", 1.09f }, { "PICKUP", 1.07f } };

static int test_on(const char *name){
    const char *s = getenv("NFSU2_INPUT_SCRIPT"), *v = getenv(name);
    return s && *s && v && *v && strcmp(v, "0");
}
static uint32_t guest_call(CPU *c, uint32_t fn, uint32_t ecx, int n, const uint32_t *arg){
    CPU save = *c;
    c->esp = (c->esp - 256u) & ~15u;
    for (int i = n - 1; i >= 0; i--) PUSH32(arg[i]);
    c->ecx = ecx;
    PUSH32(CALL_RET);
    dispatch_indirect(c, fn);
    uint32_t r = c->eax;
    *c = save;
    return r;
}
static void type_name(uint32_t type, char *out, int len){
    uint32_t t = MR32(G_CARTYPES) + type * CT_SIZE;
    int k = 0;
    if (type < N_TYPES) for (; k < len - 1 && MR8(t + (uint32_t)k); k++) out[k] = (char)MR8(t + (uint32_t)k);
    out[k] = 0;
}
static int traffic_type(uint32_t type){ return type < N_TYPES && MR32(MR32(G_CARTYPES) + type * CT_SIZE + CT_USE) == USE_TRAFFIC; }

/* the big traffic types' chase and drift spots moved back and up, once (a spot still the small car's: -5.8 m) */
static void big_cameras(void){
    uint32_t types = MR32(G_CARTYPES);
    static uint32_t done;
    if (!types || done == types) return;
    done = types;
    for (uint32_t t = 0; t < N_TYPES; t++){
        if (!traffic_type(t)) continue;
        char nm[24]; type_name(t, nm, sizeof nm);
        float s = 1.f;
        for (unsigned k = 0; k < sizeof BIG / sizeof BIG[0]; k++) if (!strcmp(nm, BIG[k].type)) s = BIG[k].s;
        uint32_t ti = types + t * CT_SIZE;
        if (s == 1.f || MR32(ti + CT_SPOTS) == 0 || MRF32(ti + CT_SPOTS + 0x10u + 4u) != -5.8f) continue;
        for (uint32_t l = 0; l < 2u; l++){
            uint32_t n = MR32(ti + CT_SPOTS + 4u * l), e = ti + CT_SPOTS + (l ? 0x70u : 0x10u);
            for (uint32_t k = 0; k < n && k < SPOTS_MAX; k++, e += SPOT_SIZE){
                int16_t kind = (int16_t)MR16(e);
                if (kind != 0 && kind != 1 && kind != 5) continue;
                MRF32(e + 4u) *= s; MRF32(e + 8u) *= s;
            }
        }
        fprintf(stderr, "[traffic] the %s's chase cameras %.2f times as far back and up (%.1f m, %.1f m)\n", nm, (double)s,
                (double)-MRF32(ti + CT_SPOTS + 0x10u + 4u), (double)MRF32(ti + CT_SPOTS + 0x10u + 8u));
    }
}

/* (test) the player's ride made the named type, stock parts, its own skin slot */
static void test_ride(CPU *c, uint32_t e){
    const char *want = getenv("NFSU2_TEST_TRAFFIC_CAR");
    if (!test_on("NFSU2_TEST_TRAFFIC_CAR")) return;
    { char nm[24]; type_name(MR32(e + RC_RIDE), nm, sizeof nm);
      fprintf(stderr, "[traffic] (test) race car 0x%08x: racer %u, kind %u, %s\n", e, MR8(e + 1u), MR32(e + RC_KIND), nm); }
    if (MR32(e + RC_KIND) != KIND_PLAYER) return;
    for (uint32_t t = 0; t < N_TYPES; t++){
        char nm[24]; type_name(t, nm, sizeof nm);
        if (strcasecmp(nm, want)) continue;
        uint32_t ride = e + RC_RIDE, a4[4] = { t, 1u, 0u, 0u }, z = 0, slot = (uint32_t)MR8(e + 1u) + 1u;
        if (MR32(ride) == t) return;
        guest_call(c, FN_RIDE_INIT, ride, 4, a4);
        guest_call(c, FN_RIDE_STOCK, ride, 1, &z);
        guest_call(c, FN_RIDE_SKINS, ride, 1, &slot);
        fprintf(stderr, "[traffic] (test) the player's car made a %s (type %u)\n", nm, t);
        return;
    }
    fprintf(stderr, "[traffic] (test) no car type %s\n", want);
}

/* the race's car e at a check of its type's use: a racer's of a traffic type is read as type 0's (a player car) */
static uint32_t as_player(uint32_t e, uint32_t off, const char *where){
    uint32_t type = MR32(e + RC_RIDE), kind = MR32(e + RC_KIND);
    if ((kind != KIND_PLAYER && kind != KIND_RACER) || !traffic_type(type)) return off;
    static uint32_t said[3]; static const char *at[3] = { "load", "traffic models", "traffic manager" };
    int k = !strcmp(where, at[0]) ? 0 : !strcmp(where, at[1]) ? 1 : 2;
    char nm[24]; type_name(type, nm, sizeof nm);
    if (test_on("NFSU2_TEST_TRAFFIC_GAME")){
        if (said[k] != e){ said[k] = e; fprintf(stderr, "[traffic] racer %u's %s: the game's way at the %s\n", MR8(e + 1u), nm, where); }
        return off;
    }
    if (said[k] != e){ said[k] = e; fprintf(stderr, "[traffic] racer %u's %s: a car of its own at the %s\n", MR8(e + 1u), nm, where); }
    return 0;
}

int trafficcars_hook(CPU *c, uint32_t ip){
    switch (ip){
    case HOOK_SITE(0x0057f6a2):              /* the world's load, cmp [ecx+eax+0x844], 2: esi the race's car, ecx its type x 0x890 */
        big_cameras();
        test_ride(c, c->esi);
        c->ecx = as_player(c->esi, MR32(c->esi + RC_RIDE) * CT_SIZE, "load");
        return 1;
    case HOOK_SITE(0x004201a2):              /* cmp [eax+ecx+0x844], 2: edi the race's car, eax its type x 0x890 */
        test_ride(c, c->edi);
        c->eax = as_player(c->edi, MR32(c->edi + RC_RIDE) * CT_SIZE, "traffic models");
        return 1;
    case HOOK_SITE(0x00412c7c):              /* cmp [eax+edx+0x844], 2: ebx the race's car, eax its type x 0x890 */
        test_ride(c, c->ebx);
        c->eax = as_player(c->ebx, MR32(c->ebx + RC_RIDE) * CT_SIZE, "traffic manager");
        return 1;
    }
    return 0;
}
