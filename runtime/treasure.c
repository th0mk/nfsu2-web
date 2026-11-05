/* treasure.c -- the daily treasure hunt in the career's free roam (web/src/app/treasure.ts draws it).
 *
 * Each UTC day has its 15 diamonds, the same for every player, all over the city: spots picked by the day's number
 * from the pool (pool: the road points by the career's events and shops, the same in every game), at least PICK_APART
 * from each other, the ones within POOL_SAME of another taken as one.  Each spot has the stage of the races' start
 * nearest it (the races' table [0x8383E0] x [0x8383E4], 0x88 bytes each: +0x28 its marker's hash, +0x37 its stage): a
 * part of the city the career opens then (a spot of a later stage was behind its barrier).  A diamond whose home is of a later stage than the player's goes to a spot of the
 * open part of the city instead (place: the same for every player at that stage), so every stage has its 15; only
 * where the open part has too few spots is one locked.  The prologue counts as stage 1 (City Core).
 *
 * The page says which day it is and which of its diamonds were found already (IEV_TREASURE: a the day, b the found
 * ones, a bit each); driving within FIND_R of where one is for the player finds it.  The last one of the day pays
 * the career's bank twice what a win of the player's stage pays (mprace_prize_of(1)), once: a day the page says was
 * all found already pays nothing more (the others pay nothing: the prize is for all fifteen).  The game's autosave keeps
 * the money as any (mprace.c pays a race's prize the same way).
 *
 * Out in two status slots (runtime/web/status_web.c), while the career's free roam is on:
 *   th_state  the day (16 bits) | found << 16 (15 bits, a bit each) | on << 31 (the career's free roam: the hunt can be
 *             played now) | paid << 32 ($ / 10 this game, 16 bits) | locked << 48 (4 bits: not found, no open spot)
 *   th_aim    the nearest open diamond not found yet: its distance (m, 16 bits) | its bearing from the car's nose << 16
 *             (degrees clockwise, 9 bits) | which one << 25 (4 bits) | the day's prize << 29 ($ / 10, 12 bits) |
 *             aimed << 41 (1: there is one to aim at)
 *   th_screen where that one is on the screen (project: 0 behind the camera)
 * and a [treasure] line in the log for the pool (once), each diamond found and the prize. */
#include "cpu.h"
#include "splitscreen_mem.h"
#include "mprace.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

__attribute__((weak)) int host_status_slot(const char *name){ (void)name; return -1; }
__attribute__((weak)) void host_status_set_n(const int *slot, const double *value, int n){ (void)slot; (void)value; (void)n; }
long autotest_frame(void);
/* roam_game.c: a place on the minimap (none in a build without free roam's multiplayer) */
__attribute__((weak)) void roam_minimap_beacon(int on, float x, float y){ (void)on; (void)x; (void)y; }
/* walls.c: the career's roads the start reaches as the barriers are now */
int walls_open_roads(void);
int walls_open_node(int i, float *x, float *y, float *z);
int walls_open_at(float x, float y);
int walls_road_z(float x, float y, float near_z, float *z);

#define G_PATHS       0x0088f298u
#define G_FLOW        0x008654a4u   /* 6: in the world */
#define G_GAMEMODE    0x00850078u   /* 1 career */
#define G_RACEKIND    0x0089e7b0u   /* 0 free roam */
#define G_CAREER      0x0085ad40u
#define C_BANK        0x7134u
#define G_STAGE       0x00862838u   /* the career's stage (Career +0x7AF8): 0 the prologue, 1 to 5, 6 done */
#define G_RACES       0x008383e0u   /* the career's races, RACE_SIZE each, [G_NRACES] of them */
#define G_NRACES      0x008383e4u
#define RACE_SIZE     0x88u
#define G_PLAYER1     SPLIT_PLAYERS_INDEX
#define G_VIEW1       0x00832e50u   /* eView 1: +0x40 its camera */
#define CAM_FOV       0xc4u
#define FLOAT_UP      1.5f          /* m: the marker over the spot (it floats at a car's roof) */
#define N_DIAMONDS    15
#define ALL_FOUND     ((1u << N_DIAMONDS) - 1u)
#define POOL_MAX      512
#define POOL_SAME     60.f          /* m: two markers this close are one spot */
#define TRIG_EVENT    13            /* the track's triggers: the career's events' */
#define TRIG_DOOR     14            /* ...the shops', the garage's, the car lot's */
#define PICK_APART    400.f         /* m: the day's diamonds at least this far from each other (less where it must) */
#define FIND_R        18.f          /* m: this close finds one */
#define MINIMAP_R     500.f         /* m: the nearest one this close is on the minimap */

static int committed(uint32_t a, uint32_t n){
    if (!a || a + n < a) return 0;
    for (uint32_t pg = a >> 14; pg <= (a + n - 1) >> 14; pg++) if (!mem_is_committed(pg << 14)) return 0;
    return 1;
}
static uint32_t rd(uint32_t a){ return committed(a, 4) ? MR32(a) : 0; }
static float rdf(uint32_t a){ if (!committed(a, 4)) return 0.f; float f = MRF32(a); return f == f ? f : 0.f; }
static int sane(float x, float y){ return x == x && y == y && fabsf(x) < 20000.f && fabsf(y) < 20000.f && (x || y); }

typedef struct { float x, y, z; int stage; } Spot;
static struct {
    int npool; Spot pool[POOL_MAX];                /* the spots, sorted */
    uint32_t day; int known;                       /* the page's day (IEV_TREASURE), and whether it said */
    uint32_t found; int was_done;                  /* the day's found ones; all of them already when the page said */
    int picked_day_ok; uint32_t picked_day; int pick[N_DIAMONDS];   /* the day's homes: the same for every player */
    int placed_ok; uint32_t placed_day; int placed_stage, placed_roads;
    Spot at[N_DIAMONDS]; int moved[N_DIAMONDS], locked[N_DIAMONDS];   /* where each is for this player */
    unsigned paid;                                 /* the money this game paid ($) */
    int slot[3];
} T = { .slot = { -2, -2, -2 } };

/* the page: the day, and its diamonds found so far */
void treasure_input(uint32_t day, uint32_t found){
    T.day = day; T.found = found & ALL_FOUND; T.known = 1; T.was_done = T.found == ALL_FOUND; T.placed_ok = 0;
    fprintf(stderr, "[treasure] day %u, %d of %d found already\n", day, __builtin_popcount(T.found), N_DIAMONDS);
}

static int cmp_spot(const void *a, const void *b){
    const Spot *p = a, *q = b;
    return p->x < q->x ? -1 : p->x > q->x ? 1 : p->y < q->y ? -1 : p->y > q->y;
}
static uint32_t marker(uint32_t h){
    uint32_t m = rd(G_PATHS + 0x43cu), size = rd(G_PATHS + 0x438u), end = m + size;
    if (!m || !size || size > (8u << 20) || !committed(m, size)) return 0;
    for (uint32_t k = 0; m < end && k < 8192; k++){
        if (MR32(m + 0x30u) == h) return m;
        int16_t sz = (int16_t)MR16(m + 0x42u);
        if (sz <= 0) break;
        m += (uint32_t)sz;
    }
    return 0;
}
/* a spot into the pool: one within POOL_SAME is the same spot (of the lower stage of the two) */
static void add_spot(float x, float y, float z, int stage){
    for (int i = 0; i < T.npool; i++)
        if (hypotf(T.pool[i].x - x, T.pool[i].y - y) < POOL_SAME){ if (stage < T.pool[i].stage) T.pool[i].stage = stage; return; }
    if (T.npool < POOL_MAX) T.pool[T.npool++] = (Spot){ x, y, z, stage };
}
/* the pool, once the races' table and the track's triggers are there; 1 when it has the day's diamonds' worth.  The
 * spots: the road points of the career's events' and the shops', garage's and car lot's triggers (the track's
 * triggers of a type t are [0x88F298 +0x440 + 4t] .. [+0x444 + 4t], records of [+0x42] bytes: +4 +8 the middle, +0x34
 * +0x38 +0x3C where the GPS goes, whole metres: the game's own point of the road by the place; roam_game.c reads
 * them so), each of the stage of the races' start nearest it (the career opens the city a part at a time).  The
 * map's other markers are no spots: one of them was in a building */
static int pool(void){
    if (T.npool) return T.npool >= N_DIAMONDS;
    uint32_t races = rd(G_RACES), n = rd(G_NRACES);
    if (!races || !n || n > 1024 || !committed(races, n * RACE_SIZE)) return 0;
    static Spot st[POOL_MAX]; int ns = 0;                      /* the races' starts, with their stages */
    for (uint32_t r = 0; r < n && ns < POOL_MAX; r++){
        uint32_t e = races + r * RACE_SIZE, m = marker(MR32(e + 0x28u));
        if (!m) continue;
        float x = MRF32(m + 4u), y = MRF32(m + 8u);
        int stage = MR8(e + 0x37u) < 1 ? 1 : MR8(e + 0x37u);
        if (sane(x, y)) st[ns++] = (Spot){ x, y, 0.f, stage };
    }
    if (!ns) return 0;                                         /* (the map not loaded yet) */
    static const int TYPES[] = { TRIG_EVENT, TRIG_DOOR };
    int got[2] = { 0, 0 };
    for (int ti = 0; ti < 2; ti++){
        uint32_t e = rd(G_PATHS + 0x440u + 4u * (uint32_t)TYPES[ti]), end = rd(G_PATHS + 0x444u + 4u * (uint32_t)TYPES[ti]);
        for (int k = 0; e && e < end && k < 1024 && committed(e, 0x44); k++){
            int16_t sz = (int16_t)MR16(e + 0x42u);
            float x = (float)(int32_t)MR32(e + 0x34u), y = (float)(int32_t)MR32(e + 0x38u), z = (float)(int32_t)MR32(e + 0x3cu);
            if (!sane(x, y)){ x = MRF32(e + 4u); y = MRF32(e + 8u); z = 0.f; }   /* (no road point: its middle) */
            if (sane(x, y)){
                int stage = 1; float best = 1e9f;
                for (int i = 0; i < ns; i++){ float d = hypotf(st[i].x - x, st[i].y - y); if (d < best){ best = d; stage = st[i].stage; } }
                add_spot(x, y, fabsf(z) < 5000.f ? z : 0.f, stage); got[ti]++;
            }
            if (sz <= 0) break;
            e += (uint32_t)sz;
        }
    }
    qsort(T.pool, (size_t)T.npool, sizeof T.pool[0], cmp_spot);
    int by[8] = { 0 };
    for (int i = 0; i < T.npool; i++) by[T.pool[i].stage & 7]++;
    fprintf(stderr, "[treasure] the pool: %d spots on the road by %d events' and %d shops' triggers; by stage 1:%d 2:%d 3:%d 4:%d 5:%d\n",
            T.npool, got[0], got[1], by[1], by[2], by[3], by[4], by[5]);
    return T.npool >= N_DIAMONDS;
}
/* the day's diamonds: a pick of the pool by the day's number, PICK_APART apart where it can */
static void pick(void){
    if (T.picked_day_ok && T.picked_day == T.day) return;
    uint32_t s = T.day * 2654435761u ^ 0x5eedu;
    int used[POOL_MAX] = { 0 }, n = 0;
    for (float apart = PICK_APART; n < N_DIAMONDS && apart >= 0.f; apart -= 100.f)
        for (int tries = 0; n < N_DIAMONDS && tries < T.npool * 4; tries++){
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            int i = (int)(s % (uint32_t)T.npool), ok = !used[i];
            for (int k = 0; k < n && ok; k++) ok = hypotf(T.pool[i].x - T.pool[T.pick[k]].x, T.pool[i].y - T.pool[T.pick[k]].y) >= apart;
            if (ok){ used[i] = 1; T.pick[n++] = i; }
        }
    T.picked_day = T.day; T.picked_day_ok = 1;
}

/* where each of the day's diamonds is for this player: at its home when the player can drive there, else at a place of
 * the roads the player can, picked by the day and the diamond (the same for every player whose career has the same
 * walls), PICK_APART from the others where it can.  The roads: walls.c's, the traffic's roads the career's start
 * reaches past none of the barrier lines on (so a place behind a later stage's wall is never one); without them
 * (a new career's prologue, a network not as walls.c expects) the stages of the races near the homes say, and a
 * diamond with no open spot left is locked.  Again when the day, the stage or the walls change (a diamond whose
 * home opened goes back there; the ones found stay found) */
static float apart_from(int k, float x, float y){
    float m = 1e30f;
    for (int j = 0; j < N_DIAMONDS; j++) if (j != k && !T.locked[j] && (j < k || !T.moved[j])){
        float d = hypotf(T.at[j].x - x, T.at[j].y - y); if (d < m) m = d;
    }
    return m;
}
static void place(int stage, int roads){
    if (T.placed_ok && T.placed_day == T.day && T.placed_stage == stage && T.placed_roads == roads) return;
    for (int k = 0; k < N_DIAMONDS; k++){
        const Spot *h = &T.pool[T.pick[k]];
        int open = roads ? walls_open_at(h->x, h->y) : h->stage <= stage;
        T.at[k] = *h; T.moved[k] = !open; T.locked[k] = 0;
    }
    int nopen = 0;
    if (!roads) for (int i = 0; i < T.npool; i++) nopen += T.pool[i].stage <= stage;
    for (int k = 0; k < N_DIAMONDS; k++){
        if (!T.moved[k]) continue;
        uint32_t s = (T.day * 2654435761u) ^ ((uint32_t)stage * 40503u) ^ ((uint32_t)(k + 1) * 0x9e3779b9u);
        int got = 0, n = roads ? roads : nopen;
        for (float apart = PICK_APART; !got && n && apart >= 0.f; apart -= 100.f)
            for (int tries = 0; !got && tries < 64; tries++){
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                Spot c = { 0 };
                if (roads){
                    if (!walls_open_node((int)(s % (uint32_t)roads), &c.x, &c.y, &c.z)) continue;
                } else {
                    int i = (int)(s % (uint32_t)T.npool);
                    if (T.pool[i].stage > stage) continue;
                    c = T.pool[i];
                }
                if (apart_from(k, c.x, c.y) >= (apart > 0.f ? apart : 1.f)){ T.at[k] = c; got = 1; }
            }
        if (!got) T.locked[k] = 1;
    }
    /* a home with no height of its own (an event's or a shop's road point: 0) has its road's: the marker over it
     * stays where it is, not at the car's height */
    for (int k = 0; k < N_DIAMONDS; k++){
        float z;
        if (roads && !T.locked[k] && T.at[k].z == 0.f && walls_road_z(T.at[k].x, T.at[k].y, 0.f, &z)) T.at[k].z = z ? z : 0.01f;
    }
    int moved = 0, locked = 0;
    for (int k = 0; k < N_DIAMONDS; k++){ moved += T.moved[k] && !T.locked[k]; locked += T.locked[k]; }
    fprintf(stderr, "[treasure] day %u at stage %d (%s): %d diamonds at their homes, %d moved onto the roads you can drive, %d locked\n",
            T.day, stage, roads ? "the career's walls" : "the races' stages", N_DIAMONDS - moved - locked, moved, locked);
    T.placed_day = T.day; T.placed_stage = stage; T.placed_roads = roads; T.placed_ok = 1;
}

static void publish(int on, unsigned locked, double aim, double screen){
    if (T.slot[0] == -2){ T.slot[0] = host_status_slot("th_state"); T.slot[1] = host_status_slot("th_aim"); T.slot[2] = host_status_slot("th_screen"); }
    if (T.slot[0] < 0 || T.slot[1] < 0 || T.slot[2] < 0) return;
    double v[3] = { (double)((uint64_t)(T.day & 0xffffu) | (uint64_t)T.found << 16 | (uint64_t)(on ? 1 : 0) << 31 |
                             (uint64_t)((T.paid / 10u) & 0xffffu) << 32 | (uint64_t)(locked & 0xfu) << 48), aim, screen };
    host_status_set_n(T.slot, v, 3);
}
/* where p (world) is on the screen, through view 1's camera ([0x832E50] +0x40: +0x40 its eye, +0x50 its forward, +0xC4
 * its field of view, 65536 a turn, across a 4:3 picture): x and y in half the picture's height from its middle (y up:
 * the widescreen's Hor+ keeps the height, so these fit any screen's width), packed as th_screen: (x + 8) * 2048 (15
 * bits) | (y + 8) * 2048 << 15 | 1 << 30 (in front of the camera); 0 behind it or with no camera */
static double project(float px, float py, float pz){
    uint32_t cam = rd(G_VIEW1 + 0x40u);
    if (!committed(cam, CAM_FOV + 2u)) return 0;
    float ex = MRF32(cam + 0x40u), ey = MRF32(cam + 0x44u), ez = MRF32(cam + 0x48u);
    float fx = MRF32(cam + 0x50u), fy = MRF32(cam + 0x54u), fz = MRF32(cam + 0x58u);
    float fl = sqrtf(fx * fx + fy * fy + fz * fz), hfov = (float)MR16(cam + CAM_FOV) * 6.2831853f / 65536.f;
    if (!(fl > 0.1f) || !(hfov > 0.1f && hfov < 3.f)) return 0;
    fx /= fl; fy /= fl; fz /= fl;
    float rx = fy, ry = -fx, rl = sqrtf(rx * rx + ry * ry);              /* right: forward x up (z) */
    if (!(rl > 1e-3f)) return 0;
    rx /= rl; ry /= rl;
    float ux = -fz * ry, uy = fz * rx, uz = fx * ry - fy * rx;          /* up: right x forward */
    float dx = px - ex, dy = py - ey, dz = pz - ez;
    float depth = dx * fx + dy * fy + dz * fz;
    if (depth < 1.f) return 0;
    float tanv = tanf(hfov / 2.f) * 0.75f;                             /* (the 4:3 picture's half height) */
    float sx = (dx * rx + dy * ry) / depth / tanv, sy = (dx * ux + dy * uy + dz * uz) / depth / tanv;
    if (fabsf(sx) > 7.9f || fabsf(sy) > 7.9f) return 0;
    return (double)((uint64_t)lrintf((sx + 8.f) * 2048.f) | (uint64_t)lrintf((sy + 8.f) * 2048.f) << 15 | 1ull << 30);
}
static void pay(unsigned amount, const char *what){
    uint32_t b = rd(G_CAREER + C_BANK);
    if (!amount || !committed(G_CAREER + C_BANK, 4)) return;
    MR32(G_CAREER + C_BANK) = b + amount; T.paid += amount;
    fprintf(stderr, "[treasure] f=%ld %s: $%u into the bank ($%u -> $%u)\n", autotest_frame(), what, amount, b, b + amount);
}

void treasure_frame(CPU *c){
    (void)c;
    uint32_t pl = rd(G_PLAYER1), car = pl ? rd(pl + 4u) : 0;
    int on = T.known && rd(G_FLOW) == 6u && rd(G_GAMEMODE) == 1u && rd(G_RACEKIND) == 0u && car && committed(car + 0x60u, 0x48)
             && pool();
    if (!on){ publish(0, 0, 0, 0); roam_minimap_beacon(0, 0.f, 0.f); return; }
    pick();
    int stage = rd(G_STAGE) < 1u ? 1 : (int)rd(G_STAGE), win = mprace_prize_of(1);
    /* (the walls looked at again now and then: a new stage's come with a new world, or with the races' screens) */
    static int roads, roads_at = -1000000; static uint32_t roads_day; static int roads_stage;
    long f = autotest_frame();
    if (f - roads_at >= 120 || f < roads_at || roads_day != T.day || roads_stage != stage){
        roads = walls_open_roads(); roads_at = (int)f; roads_day = T.day; roads_stage = stage;
    }
    place(stage, roads);
    unsigned prize = win > 0 ? 2u * (unsigned)win / 10u * 10u : 0u;    /* (all fifteen: twice a win, whole tens of dollars) */
    float x = rdf(car + 0x60u), y = rdf(car + 0x64u), fx = rdf(car + 0x90u), fy = rdf(car + 0x94u);
    int best = -1; float bd = 1e9f; unsigned locked = 0;
    for (int k = 0; k < N_DIAMONDS; k++){
        if (T.found & (1u << k)) continue;
        if (T.locked[k]){ locked++; continue; }
        const Spot *s = &T.at[k];
        float d = hypotf(s->x - x, s->y - y);
        if (d < FIND_R){
            T.found |= 1u << k;
            fprintf(stderr, "[treasure] f=%ld diamond %d found (%d of %d)%s\n", autotest_frame(), k + 1, __builtin_popcount(T.found),
                    N_DIAMONDS, T.moved[k] ? " (moved from its home)" : "");
            if (T.found == ALL_FOUND && !T.was_done) pay(prize, "all of the day's diamonds");
            continue;
        }
        if (d < bd){ bd = d; best = k; }
    }
    double aim = (double)((uint64_t)((prize / 10u) & 0xfffu) << 29), screen = 0;
    if (best >= 0 && bd < MINIMAP_R) roam_minimap_beacon(1, T.at[best].x, T.at[best].y);
    else roam_minimap_beacon(0, 0.f, 0.f);
    if (best >= 0){
        const Spot *b = &T.at[best];
        screen = project(b->x, b->y, (b->z ? b->z : rdf(car + 0x68u)) + FLOAT_UP);
        /* the bearing: from the car's nose (its forward axis, +0x90) to the diamond, clockwise seen from above */
        float dx = b->x - x, dy = b->y - y;
        double a = atan2((double)dy, (double)dx) - atan2((double)fy, (double)fx);
        int deg = (int)lround(-a * 180.0 / 3.14159265358979) % 360;
        if (deg < 0) deg += 360;
        aim += (double)((uint64_t)(bd > 65535.f ? 65535u : (unsigned)bd) | (uint64_t)deg << 16 | (uint64_t)best << 25 | 1ull << 41);
    }
    publish(1, locked, aim, screen);
}
