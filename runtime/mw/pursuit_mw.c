/* Most Wanted's pursuit, rap sheet and memory pool aids (test only, like career_mw.c's race aids, which run these
 * commands from the frame anchor; TRACKER M4-16, M4-17, M4-18, M4-28, M4-35).  Nothing here edits the game's state
 * (L-022): the aids read it, and two instruction hooks count what the game does.
 *
 *   rapsheet         log the career's rap sheet and what a bust costs: the ten rap sheet values, the best single
 *                    pursuit of each, each one's Most Wanted rank, the cash, the impound markers, the cars that are
 *                    not impounded and each career car's record (its impound strikes)
 *   icons [T]        log the world map's icons (GManager's list): the count of each type, and each icon of type T
 *                    (12 pursuit breakers, 11 hiding spots, 8 safehouses) with its place
 *   pools [watch]    log the game's memory pools (each bMemoryPool: its size, what is allocated, the number of
 *                    allocations) and the frame arena (two buffers of 0x64000: the game's own peak of a frame's
 *                    use and of its overflow), with the peaks since `pools watch` (sampled once a frame anchor);
 *                    under `pursuit watch` they are logged again at each pursuit's end (mw_pursuit_end)
 * and `pursuit watch` (career_mw.c) appends the pursuit's counts the game keeps by name (below) and the hooks' counts.
 *
 * The game's records, from speed.exe v1.3's code:
 *   GManager [0x91E00C]: a std::map at +0x98 of values by the key of a name (0x454640, cdecl (const char *)): node +0xC
 *     the key, +0x14 the value, +0x18 its best, +0x21 the nil byte (the find 0x5EC420; GetValue 0x5EDBC0, SetValue
 *     0x5EDA20, IncValue 0x5EDB50).  The pursuit's are set as it runs: cops_in_pursuit and helis_in_pursuit by the cop
 *     manager's update (0x41912D, 0x419145), roadblocks_dodged (0x43A06A) and tire_spikes_dodged (0x43A0CA..),
 *     cops_damaged (0x40AF98), cops_destroyed (the pursuit's +0xA8, 0x43EA3A), pursuit_length (0x443CE7), and at an
 *     evade pursuits_in_a_row and pursuit_evasion_time (0x4110BD, 0x4110D8).
 *     Its icons: +0x2C0 their count, +0x2C4 an array of pointers; an icon: +0 (u16) its type (the world map's table
 *     0x8F3AC0, 20 bytes a type {type, map flag, name, two hashes}: 1-6 the race kinds, 7 the rival, 8 SAFEHOUSE,
 *     9 CAR_LOT, 10 SHOP, 11 HIDING_SPOT_%d, 12 PURSUIT_BREAKER_%d, 13 SPEED_TRAP_CAM_%d), +2 flags
 *     (bit 0 on, bit 1 shown: the map's list, 0x5E4940, takes those with both), its place at +0x10..+0x18 (the world's
 *     axes: simulation space's (-y, z, x), as 0x5E4850 turns a place around); +0x2BC the count 0x5E4850 walks.
 *   A pursuit breaker coming down is the event EBreakerStopCops (its name 0x8A68D4; its handler about 0x620E10): it
 *     posts MBreakerStopCops (the cops in a radius stop) and calls GManager 0x5E4850(place, 20 m), which clears the
 *     on bit (0x5E48CB) of the pursuit breaker's own icon (type 12) within 20 m.  GManager's list is the mode's: in a
 *     chase the 37 pursuit breakers, in the cops' search 34 hiding spots and 3 safehouses, in free roam the events,
 *     safehouses, car lots, shops and speed trap cameras (runs of 2026-10-30).
 *   The rap sheet, in the user profile [[0x91CF90]+0x10] at +0x90E0 (the ranking's object): ten values, 0x5999F0
 *     (thiscall on profile+0x9C24, (i)): 0 pursuit length, 1 cops involved, 2 cops damaged, 3 cops destroyed
 *     (immobilized), 4 spike strips dodged, 5 roadblocks dodged, 6 helicopters, 7 cost to state from profile+0x9C24
 *     + 4i; 8 infractions (the car table's 0x590430(1) + 0x590430(0)) and 9 bounty (0x590500).  The best single
 *     pursuit of each, profile+0x9C48 + 8i.  The Most Wanted rank of a value, 0x5A3C30 (thiscall on profile+0x90E0,
 *     (i, career)): the rival whose threshold it reaches (rankings collection field 0xF9A7D5F7), 16 none.  The names
 *     0x5A3C30 reads its thresholds by: pursuit_length, cops_involved, cops_damaged, cops_destroyed,
 *     tire_spikes_dodged, roadblocks_dodged, helis_involved, cost_to_state, total_infractions, bounty (and _in_pursuit).
 *   The car table (FEPlayerCarDB, profile+0x414): 200 cars of 0x14 bytes (+0 the handle, -1 none; +0x11 the career
 *     record's index, 0xFF none), 25 career records of 0x38 bytes at +0x8728 (+0 the handle, 0xFF none; +4 a byte, 0
 *     for a car that can be driven: 0x5811A0 counts the cars with it 0).  GameOver.fng comes (0x7AD5C6) when that
 *     count is 0 and no Impound Release marker is left.
 *   The career, profile+0xA8: +4 flags (0x800 game over), +8 the Blacklist bin (byte), +0xC the cash.
 *   The markers, 0x91D928: 63 of {type, param, state}; 0x576F10 (thiscall (type, param)) counts those held.  Types
 *     (the table at 0x8F5480): 0x11 out_of_jail_free, 0x12 pink_slip, 0x13 cash_bonus, 0x14 add_impound_box,
 *     0x15 release_car_from_impound.
 *   bMemoryPool: the pools' pointers at 0x90DF10 (16), each: +0 its name, +0x1C its memory, +0x20 its size, +0x24
 *     the number of allocations, +0x2C the memory it holds, +0x30 what is allocated, +0x38 what is free (+0x2C -
 *     +0x30; the free path 0x464875).  The FEng pool is pool [0x8F578C] (FEngMemoryPoolSize [0x8F5790]), the car
 *     loader's [0x904A00] (CarLoaderPoolSizes [0x8F7EF0]), made by 0x465240.
 *   The frame arena (0x4FAD80 at each frame's swap): [0x915F7C] a buffer's size (0x64000), [0x915FA0] the game's
 *     own peak of a frame's use, [0x915FA4] the last frame's, [0x915F9C] the peak overflow ([0x915F98] while
 *     [0x915F94] is set).
 *
 * The hooks (HOOK_SITE; counted always, logged while `pursuit watch` is on):
 *   0x43DD30  the roadblock spawner's entry (its callers 0x43E7D1, 0x43EC35; the purecall guard 0x43E005 is in it)
 *   0x439E60  MBreakerStopCops' handler (the cop manager): a pursuit breaker came down; the message holds its place
 *             (+0x10..+0x18), its radius (+0x1C) and a time (+0x20).  (EPursuitBreaker, 0x641670, is the
 *             Speedbreaker's slow motion, not a pursuit breaker: 0x6E9AE9 and the player's destructor make it.)
 *   0x41ECD0  the cop manager's cop car by name (thiscall (const char *name)): it hashes the name (0x5CC240) and
 *             takes a car of its pool whose IVehicle method +0x4C (the vehicle's name) hashes the same, else makes
 *             one.  Its callers pick the name: the pursuit's (0x4266E9: "copheli" for the helicopter's kind
 *             [0x92C4F4]; 0x426726: the heat's table through 0x419AE0, else "copmidsize"), the roadblock's
 *             (0x43EBCB) and others (0x41F1CD, 0x41F588, 0x42E729, 0x431495).  Counted by name for each pursuit and
 *             logged at its end with the pools (mw_pursuit_end).
 * The cops' search (the cooldown, the pursuit's state +0x218 == 2; its update about 0x4447F8..0x444A00, esi the
 * pursuit's base + 8, the pursuit object at esi + 0x40): +0xFC the meter, (+0x124 + +0x128) / +0x104, evaded at 1
 * (state 4); +0x128 the search's time (dt each frame, 0x4440E5), +0x124 a bonus that grows by dt x a rate (the
 * attribute 0x7FCEE250 of the instance at the update's [esp+0x10], else [0x890604]) only while the object at
 * +0x74 answers yes to its method +0x2C or +0x30 (0x444824..0x44488D), +0x104 the search's length (s; from the
 * heat's table, 0x443D5E). */
#include "cpu.h"
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

long autotest_frame(void);
uint32_t mw_aid_call(CPU *c, uint32_t fn, uint32_t self, int n, const uint32_t *args);   /* career_mw.c */
int mw_aid_pw_on(void);
unsigned long mw_fix_purecall_calls(void);                    /* fixes_mw.c: the guard site's runs */

static uint32_t rd(uint32_t a){ return a && mem_is_committed(a) && mem_is_committed(a + 3u) ? MR32(a) : 0u; }
static uint8_t rd8(uint32_t a){ return a && mem_is_committed(a) ? MR8(a) : 0u; }
static float rdf(uint32_t a){ uint32_t v = rd(a); float f; memcpy(&f, &v, 4); return f; }
static uint32_t profile(void){ return rd(rd(0x0091cf90u) + 0x10u); }

/* ---- GManager's values by name ---- */
#define G_GMANAGER   0x0091e00cu
#define GM_MAP       0x98u
#define FN_GM_KEY    0x00454640u
static int gm_value(CPU *c, uint32_t name, float *v){
    uint32_t gm = rd(G_GMANAGER);
    if (!gm) return 0;
    uint32_t key = mw_aid_call(c, FN_GM_KEY, 0, 1, &name);
    uint32_t head = rd(gm + GM_MAP + 4u), x = rd(head + 4u), lb = head;
    for (int i = 0; x && i < 64 && !rd8(x + 0x21u); i++){       /* the game's find (0x5EC420) */
        if (rd(x + 0xcu) < key) x = rd(x + 8u); else { lb = x; x = rd(x); }
    }
    if (!head || lb == head || key < rd(lb + 0xcu)) return 0;
    *v = rdf(lb + 0x14u);
    return 1;
}
static const struct { uint32_t name; const char *tag; } gm_names[] = {
    { 0x00891150u, "cops" },               /* cops_in_pursuit */
    { 0x0089113cu, "helis" },              /* helis_in_pursuit */
    { 0x00892b98u, "rb_dodged" },          /* roadblocks_dodged */
    { 0x00892b7cu, "spikes_dodged" },      /* tire_spikes_dodged */
    { 0x00890eb0u, "damaged" },            /* cops_damaged */
    { 0x00892ef0u, "destroyed" },          /* cops_destroyed */
    { 0x00891564u, "total_cops" },         /* total_cops_in_pursuit */
};
#define N_GM (int)(sizeof gm_names / sizeof gm_names[0])

/* ---- the hooks ---- */
#define HOOK_SITE(va) (va##u)
#define SITE_ROADBLOCK_TRY HOOK_SITE(0x0043dd30) /* the roadblock spawner: a try (about every other frame in a pursuit) */
#define SITE_ROADBLOCK   HOOK_SITE(0x0043df43)   /* ... past its road checks: a roadblock is put up (its cars by heat) */
#define SITE_BREAKER     HOOK_SITE(0x00439e60)   /* MBreakerStopCops' handler */
#define SITE_COPCAR      HOOK_SITE(0x0041ecd0)   /* the cop manager's cop car by name */
static unsigned long n_roadblock_try, n_roadblock, n_breaker;
unsigned long mw_pursuit_breakers(void){ return n_breaker; }     /* career_mw.c: `pursuit wait breaker` */
/* the cop cars asked for by name since the last pursuit's end (the names are the game's own strings) */
#define N_COPNAMES 16
static struct { char name[24]; unsigned long n, all; } copcar[N_COPNAMES];
static unsigned long n_copcar_lines;
static void copcar_count(uint32_t s, uint32_t from){
    char nm[24] = "?";
    for (int k = 0; s && k < 23 && rd8(s + (uint32_t)k); k++){ nm[k] = (char)rd8(s + (uint32_t)k); nm[k + 1] = 0; }
    int i = 0;
    while (i < N_COPNAMES && copcar[i].name[0] && strcmp(copcar[i].name, nm)) i++;
    if (i < N_COPNAMES){
        if (!copcar[i].name[0]) memcpy(copcar[i].name, nm, sizeof nm);
        copcar[i].n++; copcar[i].all++;
    }
    if (mw_aid_pw_on() && n_copcar_lines++ < 400)
        fprintf(stderr, "[mwpursuit] f=%ld cop car asked: %s (from 0x%08x)\n", autotest_frame(), nm, from);
}
int mw_pursuit_hook(CPU *c, uint32_t va){
    if (va == SITE_ROADBLOCK_TRY){ n_roadblock_try++; return 1; }
    if (va == SITE_COPCAR){ copcar_count(MR32(c->esp + 4u), MR32(c->esp)); return 1; }
    if (va == SITE_ROADBLOCK){
        n_roadblock++;
        if (mw_aid_pw_on())
            fprintf(stderr, "[mwpursuit] f=%ld roadblock: put up (0x43DF43, after %lu tries), roadblock %lu\n",
                    autotest_frame(), n_roadblock_try, n_roadblock);
        return 1;
    }
    if (va == SITE_BREAKER){
        n_breaker++;
        uint32_t m = MR32(c->esp + 4u);
        if (mw_aid_pw_on())
            fprintf(stderr, "[mwpursuit] f=%ld breaker: MBreakerStopCops (0x439E60) at %.1f %.1f %.1f radius %.1f "
                    "time %.1f, breaker %lu\n", autotest_frame(), rdf(m + 0x10u), rdf(m + 0x14u), rdf(m + 0x18u),
                    rdf(m + 0x1cu), rdf(m + 0x20u), n_breaker);
        return 1;
    }
    return 0;
}

/* the pursuit's counts for career_mw.c's [mwpursuit] line: sig[] for its change test (8 values), the text in out */
#define PURSUIT_STATE      0x218u
#define PURSUIT_METER      0xfcu        /* the cops' search (state 2): the meter, 0..1 */
#define PURSUIT_SEARCH_LEN 0x104u       /* ... its length (s) */
#define PURSUIT_BONUS      0x124u       /* ... the bonus seconds */
#define PURSUIT_SEARCH_T   0x128u       /* ... its seconds so far */
#define PURSUIT_HIDE       0x74u        /* the object whose +0x2C / +0x30 let the bonus grow */
void mw_pursuit_extras(CPU *c, uint32_t po, uint32_t sig[8], char *out, size_t n){
    size_t o = 0;
    out[0] = 0;
    memset(sig, 0, 8 * sizeof sig[0]);
    o += (size_t)snprintf(out + o, n - o, " state %u gm", po ? rd(po + PURSUIT_STATE) : 0u);
    sig[0] = po ? rd(po + PURSUIT_STATE) : 0u;
    for (int i = 0; i < N_GM && o < n; i++){
        float v;
        int ok = gm_value(c, gm_names[i].name, &v);
        if (i < 6) sig[1 + i] = ok ? (uint32_t)(int)v : 0xffffffffu;
        o += (size_t)(ok ? snprintf(out + o, n - o, " %s %.0f", gm_names[i].tag, v)
                         : snprintf(out + o, n - o, " %s -", gm_names[i].tag));
    }
    sig[7] = (uint32_t)(n_roadblock * 1000u + n_breaker);
    if (o < n) o += (size_t)snprintf(out + o, n - o, " roadblocks %lu breakers %lu guard %lu", n_roadblock, n_breaker,
                                     mw_fix_purecall_calls());
    if (po && rd(po + PURSUIT_STATE) == 2u && o < n){  /* the cops' search: the meter by tenths, its parts */
        float m = rdf(po + PURSUIT_METER);
        sig[0] |= (uint32_t)(int)(m * 10.0f) << 8;
        sig[0] |= (rd(po + PURSUIT_HIDE) ? 1u : 0u) << 16;
        snprintf(out + o, n - o, " search %.2f (%.1f s + bonus %.1f s of %.0f s) +0x74 %08x", m,
                 rdf(po + PURSUIT_SEARCH_T), rdf(po + PURSUIT_BONUS), rdf(po + PURSUIT_SEARCH_LEN), rd(po + PURSUIT_HIDE));
    }
}

/* the end of a pursuit (career_mw.c's hooks at the evade and the player's bust): the cop cars it asked for by name and,
 * under `pools watch`, the pools with their peaks so far, so a run stopped later still has them */
static int pools_on;
void mw_aid_pools(CPU *c, int watch);
void mw_pursuit_end(CPU *c, const char *how){
    char b[512]; int o = 0;
    b[0] = 0;
    for (int i = 0; i < N_COPNAMES && copcar[i].name[0]; i++){
        if (copcar[i].n) o += snprintf(b + o, sizeof b - (size_t)o, " %s %lu", copcar[i].name, copcar[i].n);
        copcar[i].n = 0;
    }
    fprintf(stderr, "[mwpursuit] f=%ld end (%s): cop cars asked by name:%s; roadblocks %lu, breakers %lu, guard %lu\n",
            autotest_frame(), how, o ? b : " none", n_roadblock, n_breaker, mw_fix_purecall_calls());
    if (pools_on) mw_aid_pools(c, 0);
}

/* ---- the rap sheet ---- */
#define PROF_CAREER      0xa8u
#define PROF_CARDB       0x414u
#define PROF_RANKING     0x90e0u
#define PROF_RAP         0x9c24u
#define PROF_BEST        0x9c48u
#define FN_RAP_VALUE     0x005999f0u
#define FN_RAP_RANK      0x005a3c30u
#define FN_CARS_FREE     0x005811a0u
#define FN_INFRACTIONS   0x00590430u
#define G_MARKERS        0x0091d928u
#define FN_MARKERS       0x00576f10u
#define CARDB_CAREER     0x8728u
#define CAREER_SIZE      0x38u
static uint32_t call_rap(CPU *c, uint32_t prof, uint32_t i){ return mw_aid_call(c, FN_RAP_VALUE, prof + PROF_RAP, 1, &i); }
static const char *const rap_names[10] = { "pursuit_length", "cops_involved", "cops_damaged", "cops_destroyed",
    "spikes_dodged", "roadblocks_dodged", "helis", "cost_to_state", "infractions", "bounty" };
void mw_aid_rapsheet(CPU *c, const char *why){
    long f = autotest_frame();
    uint32_t prof = profile();
    if (!prof){ fprintf(stderr, "[mwrap] f=%ld rapsheet (%s): no profile\n", f, why); return; }
    char b[1024]; int o = 0;
    for (uint32_t i = 0; i < 10; i++)
        o += snprintf(b + o, sizeof b - (size_t)o, " %s %u", rap_names[i], call_rap(c, prof, i));
    fprintf(stderr, "[mwrap] f=%ld rapsheet (%s):%s\n", f, why, b);
    o = 0;
    for (uint32_t i = 0; i < 10; i++)
        o += snprintf(b + o, sizeof b - (size_t)o, " %u", rd(prof + PROF_BEST + 8u * i));
    o += snprintf(b + o, sizeof b - (size_t)o, "; ranks");
    for (uint32_t i = 0; i < 10; i++){
        uint32_t a[2] = { i, 1 }, a0[2] = { i, 0 };
        o += snprintf(b + o, sizeof b - (size_t)o, " %u/%u", mw_aid_call(c, FN_RAP_RANK, prof + PROF_RANKING, 2, a),
                      mw_aid_call(c, FN_RAP_RANK, prof + PROF_RANKING, 2, a0));
    }
    fprintf(stderr, "[mwrap] f=%ld   best pursuit:%s (career/best, 16 = none)\n", f, b);
    uint32_t db = prof + PROF_CARDB, one = 1, zero = 0;
    uint32_t mk[3] = { 0x11u, 0x14u, 0x15u }, m[3];
    for (int i = 0; i < 3; i++){ uint32_t a[2] = { mk[i], 0 }; m[i] = mw_aid_call(c, FN_MARKERS, G_MARKERS, 2, a); }
    fprintf(stderr, "[mwrap] f=%ld   cash %u, bin %u, career flags %08x, cars not impounded %u, markers: out of jail "
            "%u, extra strike %u, impound release %u; infractions %u + %u\n", f, rd(prof + PROF_CAREER + 0xcu),
            rd8(prof + PROF_CAREER + 8u), rd(prof + PROF_CAREER + 4u), mw_aid_call(c, FN_CARS_FREE, db, 0, 0), m[0],
            m[1], m[2], mw_aid_call(c, FN_INFRACTIONS, db, 1, &one), mw_aid_call(c, FN_INFRACTIONS, db, 1, &zero));
    for (uint32_t k = 0; k < 200; k++){             /* the cars with a career record */
        uint32_t car = db + 0x14u * k;
        if (rd(car) == 0xffffffffu) continue;
        uint8_t ci = rd8(car + 0x11u);
        if (ci == 0xff || ci > 0x19) continue;
        uint32_t r = db + CARDB_CAREER + CAREER_SIZE * ci;
        o = 0;
        for (uint32_t j = 0; j < CAREER_SIZE; j++) o += snprintf(b + o, sizeof b - (size_t)o, "%02x%s", rd8(r + j),
                                                                  (j & 3) == 3 ? " " : "");
        fprintf(stderr, "[mwrap] f=%ld   car %u (key %08x) career record %u: %s\n", f, k, rd(car + 8u), ci, b);
    }
}

/* ---- the world map's icons ---- */
#define GM_ICONS_N  0x2c0u
#define GM_ICONS    0x2c4u
void mw_aid_icons(CPU *c, int type){
    (void)c;
    uint32_t gm = rd(G_GMANAGER), n = rd(gm + GM_ICONS_N), a = rd(gm + GM_ICONS);
    long f = autotest_frame();
    int count[32] = { 0 }, shown = 0;
    for (uint32_t i = 0; a && i < n && i < 4096u; i++){
        uint32_t ic = rd(a + 4u * i);
        unsigned t = ic ? (unsigned)(rd(ic) & 0xffffu) : 0u;
        if (t < 32) count[t]++;
        if ((int)t == type && shown < 64){
            shown++;
            fprintf(stderr, "[mwicon] f=%ld icon %u type %u flags %02x at %.1f %.1f %.1f (+0x8 %.1f %.1f, +0x1c %.1f)\n", f,
                    i, t, rd8(ic + 2u), rdf(ic + 0x10u), rdf(ic + 0x14u), rdf(ic + 0x18u), rdf(ic + 8u), rdf(ic + 0xcu),
                    rdf(ic + 0x1cu));
        }
    }
    char b[512]; int o = 0;
    for (int t = 0; t < 32; t++) if (count[t]) o += snprintf(b + o, sizeof b - (size_t)o, " %d:%d", t, count[t]);
    fprintf(stderr, "[mwicon] f=%ld icons %u, +0x2bc %u (type:count%s)\n", f, n, rd(gm + 0x2bcu), b);
}
/* the place of icon k of type T, as the icons line gives it; 0 = none */
int mw_aid_icon_place(int type, int k, float pos[3]){
    uint32_t gm = rd(G_GMANAGER), n = rd(gm + GM_ICONS_N), a = rd(gm + GM_ICONS);
    for (uint32_t i = 0; a && i < n && i < 4096u; i++){
        uint32_t ic = rd(a + 4u * i);
        if (!ic || (int)(rd(ic) & 0xffffu) != type || k-- > 0) continue;
        for (int j = 0; j < 3; j++) pos[j] = rdf(ic + 0x10u + 4u * (uint32_t)j);
        return 1;
    }
    return 0;
}

/* the icon of type T that is on (flags bit 0) nearest a place in simulation space: its place there (sim (-y, z, x) of
 * the world's axes, as career_mw.c's icon go turns it), its index and distance (X, Z); -1 = none */
int mw_aid_icon_near(int type, const float at[3], float out[3], float *dist){
    uint32_t gm = rd(G_GMANAGER), n = rd(gm + GM_ICONS_N), a = rd(gm + GM_ICONS);
    int best = -1;
    float bd = 0;
    for (uint32_t i = 0; a && i < n && i < 4096u; i++){
        uint32_t ic = rd(a + 4u * i);
        if (!ic || (int)(rd(ic) & 0xffffu) != type || !(rd8(ic + 2u) & 1u)) continue;
        float s[3] = { -rdf(ic + 0x14u), rdf(ic + 0x18u), rdf(ic + 0x10u) };
        float dx = s[0] - at[0], dz = s[2] - at[2], d = dx * dx + dz * dz;
        if (best < 0 || d < bd){ best = (int)i; bd = d; memcpy(out, s, sizeof s); }
    }
    if (dist) *dist = best < 0 ? 0.0f : sqrtf(bd);
    return best;
}

/* ---- the memory pools ---- */
#define POOL_PTRS   0x0090df10u
#define N_POOLS     16
static struct { uint32_t used, allocs, size; int seen; } peak[N_POOLS];
static uint32_t arena_peak_seen;
int mw_aid_where(CPU *c, float pos[3], float *kmh);           /* career_mw.c */
void mw_pursuit_frame(CPU *c){
    static unsigned long breakers_seen;
    if (n_breaker != breakers_seen && mw_aid_pw_on()){   /* where the car was when a pursuit breaker came down */
        float p[3] = { 0 }, kmh = 0;
        breakers_seen = n_breaker;
        if (mw_aid_where(c, p, &kmh))
            fprintf(stderr, "[mwpursuit] f=%ld breaker %lu: the car at %.1f %.1f %.1f, %.0f km/h\n", autotest_frame(),
                    n_breaker, p[0], p[1], p[2], kmh);
    }
    if (!pools_on) return;
    for (int i = 0; i < N_POOLS; i++){
        uint32_t p = rd(POOL_PTRS + 4u * (uint32_t)i);
        if (!p) continue;
        uint32_t u = rd(p + 0x30u), n = rd(p + 0x24u);
        if (u > peak[i].used) peak[i].used = u;
        if (n > peak[i].allocs) peak[i].allocs = n;
        peak[i].size = rd(p + 0x2cu); peak[i].seen = 1;
    }
    uint32_t a = rd(0x00915fa4u);
    if (a > arena_peak_seen) arena_peak_seen = a;
}
void mw_aid_pools(CPU *c, int watch){
    (void)c;
    long f = autotest_frame();
    if (watch && !pools_on){ pools_on = 1; memset(peak, 0, sizeof peak); arena_peak_seen = 0; }
    for (int i = 0; i < N_POOLS; i++){
        uint32_t p = rd(POOL_PTRS + 4u * (uint32_t)i);
        if (!p) continue;
        char nm[40] = "?"; uint32_t s = rd(p);
        for (int k = 0; s && k < 39 && rd8(s + (uint32_t)k); k++){ nm[k] = (char)rd8(s + (uint32_t)k); nm[k + 1] = 0; }
        fprintf(stderr, "[mwpool] f=%ld pool %d \"%s\": holds %u KB, allocated %u KB in %u, free %u KB; peak %u KB in %u\n",
                f, i, nm, rd(p + 0x2cu) >> 10, rd(p + 0x30u) >> 10, rd(p + 0x24u), rd(p + 0x38u) >> 10,
                peak[i].used >> 10, peak[i].allocs);
    }
    fprintf(stderr, "[mwpool] f=%ld frame arena: %u KB a buffer, the last frame %u KB, the game's peak %u KB (%u KB seen "
            "since watch), overflow peak %u KB; FEng pool %d (size %u KB), car loader pool %d (CarLoaderPoolSizes %u)\n", f,
            rd(0x00915f7cu) >> 10, rd(0x00915fa4u) >> 10, rd(0x00915fa0u) >> 10, arena_peak_seen >> 10,
            rd(0x00915f9cu) >> 10, (int)rd(0x008f578cu), rd(0x008f5790u) >> 10, (int)rd(0x00904a00u), rd(0x008f7ef0u));
}
